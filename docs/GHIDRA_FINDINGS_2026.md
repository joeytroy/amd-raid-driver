# Ghidra Findings — Fresh Analysis (May 2026)

This document captures new findings from a fresh headless-Ghidra pass on
`rcbottom.sys` (version 9.3.2-00255). It **supersedes and corrects** several
load-bearing claims in `TECHNICAL_REFERENCE.md`, `GHIDRA_ANALYSIS_NEEDED.md`,
and `IMPLEMENTATION_STATUS.md`.

Output of the analysis is in `docs/ghidra_output/rcbottom_9.3.2/`.

## TL;DR — Why the driver doesn't work

The Linux driver is treating the controller as if it were the AHCI variant
(devices `0x7905 / 0x7916 / 0x7917 / 0x43BD`). For **device `0xB000`** —
which is the one in the test system — the Windows driver takes a **completely
different code path** (`nvme.c`) with a different callback table, different
register programming, and different completion handling. None of the
"AHCI completion-register-at-0x100" work applies here. That's why writes to
offset `0x100+` don't persist for `0xb000`: those offsets aren't its
completion registers at all.

## CVE-2024-21962 status

- `rcraid.sys` is **byte-identical** between 9.3.2-00255 and 9.3.3-00291
  (SHA256 match). All prior `rcraid` analysis is valid for the patched build.
- `rccfg.sys` shrank by 600 bytes — the CVE fix lives here (config IOCTL
  surface, "improper input validation"). Not relevant to the Linux port.
- `rcbottom.sys` grew by 1968 bytes — small, likely localized bounds checks.

## Major finding 1 — `nvme.c` and `ahci.c` are separate code paths

`FUN_140007d40` (the function previously called "firmware capability parsing")
is actually the **callback-table installer** that switches between two
hardware code paths based on the PCI hardware ID:

```
                                       +-- ahci.c path (iVar7 == 1)
   PnP HW-ID = VEN_1022 & DEV_7905  ---+
   PnP HW-ID = VEN_1022 & DEV_7916  ---+
   PnP HW-ID = VEN_1022 & DEV_7917  ---+
   PnP HW-ID = VEN_1022 & DEV_43BD  ---+

                                       +-- nvme.c path (iVar7 == 2)
   ANY OTHER ID + (param_3 != 0)    ---+   <-- includes DEV_B000

   ANY OTHER ID + (param_3 == 0)    ---+-- stub path (iVar7 == 99)
```

The internal AMD source paths are **embedded in error strings** in the
binary:

- `Y:\RC-932\RC_932_00255\fulcrum\rc\platforms\rcbottom\rcbottom\ahci.c` —
  seen at FUN_1400021d4 (AHCI completion-register install)
- `Y:\RC-932\RC_932_00255\fulcrum\rc\platforms\rcbottom\rcbottom\nvme.c` —
  seen at FUN_14000dd44 (NVMe queue init)

These two files install **different sets of function pointers** at
`devExt+0x16100`...`devExt+0x16168`. The same outer code (FUN_140008a48
queue-init, FUN_14000924c doorbell-activation) then dispatches through
these pointers, so the same call site does AHCI or NVMe work depending
on which set was installed.

### Path-selection input

The third parameter to `FUN_140007d40` (called `uVar17` at the call site)
is `1` if and only if the PCI class-code string contains `"CC_010802"`
(NVMe-class PCI device). This is read via StorPort service `+0x3f0` opcode
`2` in `FUN_140005ff4`.

This matches the INF perfectly:

| Device  | INF class code | param_3 | iVar7 path |
|---------|---------------|---------|------------|
| 0x7905  | CC_0104       | 0       | AHCI (1)   |
| 0x7916  | CC_0104       | 0       | AHCI (1)   |
| 0x7917  | CC_0104       | 0       | AHCI (1)   |
| 0x43BD  | CC_0104       | 0       | AHCI (1)   |
| 0xB000  | CC_0108       | 1       | NVMe (2)   |

So `0xB000` is **always** NVMe.

### What the AHCI/NVMe split installs

`devExt+0x16100..0x16168` is a 13-slot vtable. Excerpt:

| Slot         | AHCI value (iVar7=1) | NVMe value (iVar7=2) | Purpose       |
|--------------|----------------------|----------------------|---------------|
| `+0x16100`   | FUN_140004090        | FUN_14000fafc        | Primary dispatch |
| `+0x16108`   | FUN_140001438        | FUN_14000c0bc        | Cmd submit     |
| `+0x16110`   | **FUN_1400021d4**    | **FUN_14000dd44**    | Queue/CR init  |
| `+0x16118`   | FUN_140003f7c        | FUN_14000303c        | Cleanup        |
| `+0x16120`   | FUN_140003048        | FUN_14000e800        | Disable/quiesce |
| `+0x16128`   | FUN_1400027a8        | LAB_14000918c        | Mode toggle   |
| `+0x16130`   | FUN_1400028f8        | FUN_14000e494        | Enable/resume |
| `+0x16138`   | FUN_140001ba4        | FUN_14000c814        | State getter  |
| `+0x16140`   | FUN_140001bbc        | FUN_14000c82c        | Activity check |
| `+0x16148`   | FUN_140003838        | FUN_14000d06c        | Secondary disp |
| `+0x16158`   | FUN_14000303c        | FUN_14000e59c        | Aux cleanup    |
| `+0x16160`   | FUN_140003598        | FUN_14001023c        | Queue cleanup  |
| `+0x16168`   | LAB_140002808        | FUN_1400100c0        | Status update  |

The "stub" path (iVar7=99) installs `FUN_1400102d8` everywhere — that's a
"do nothing safely" no-op handler.

## Major finding 2 — `DAT_140012258` is NOT a firmware blob

Prior docs treat `DAT_140012258` as a firmware capability descriptor blob
parsed by an "accessor function pointer at `devExt+0x1C2D0`" using opcodes
`0x05/0x10/0x11/0x34`, and hypothesized that `FUN_14002ce29` is the
accessor.

Both claims are wrong:

- `DAT_140012258` is a **WDF/KMDF class-registration blob** containing
  GUIDs (`{109c af dc-5f89-481f-...}` etc.) and the literal strings
  `"ZPODDWORKITEM"`, `"NOTIFYWORKITEM"`, `"KmdfLibrary"`, `"DriverEntry
  failed 0x%x for driver %wZ"`, etc. It is the WDF version-bind table the
  KMDF stub library uses at driver load.
- StorPort service `+0x418` is the **WDF class-bind entry point**
  (`FxStubBindClasses`-style). It does NOT parse hardware capabilities; it
  resolves the WDF library version into a context (`devExt+0x1c2a0`) and a
  per-version function pointer (`devExt+0x1c2d0`).
- The "opcodes" (`0x05`, `0x10`, `0x11`, `0x34`, `0x82`, `0xc6`, `0x108`,
  `0x10a`) are **WDF API indices**, not firmware capability opcodes. They
  vary by device-family because different families bind different WDF API
  versions.
- `FUN_14002ce29` is outside `rcbottom.sys`'s address space (binary ends
  near `0x14001a700`). It does not exist in this binary.

**Implication for Linux port:** there is no descriptor accessor to
reimplement. The "firmware capability words" that prior docs say live in
`DAT_140012258` don't exist there. Whatever the driver needs to know about
the device, it gets from PCI config space (vendor/device/class) and from
the NVMe controller registers in BAR0 — both of which Linux exposes
directly.

## Major finding 3 — Doorbell order 1,4,2,3 is correct (for init)

`FUN_140008638` (HwInitialize) rings doorbells in order **1, 4, 2, 3** at
addresses `0x140008969 / 0x140008989 / 0x1400089a6 / 0x1400089c6`. The
current Linux driver matches this. The later "re-arm" path in
`FUN_14000924c` uses `1, 2, 3, 4` instead, but that runs after init and
takes a different argument shape. The 1-4-2-3 init order in the Linux
`rc_activate_doorbells()` is correct.

## Major finding 4 — Service slot inventory

Confirmed indirect-call buckets in `rcbottom.sys` (counts are hits in this
binary). These are the only StorPort vtable entries that matter for
porting:

| Service slot | Hits | Used for                                       |
|--------------|------|------------------------------------------------|
| `+0xa8`      | 7    | DMA buffer alloc (with `+0xb0`/`+0xb8`)        |
| `+0xb0`      | 7    | DMA → virtual address                          |
| `+0xb8`      | 7    | DMA → physical address                         |
| `+0xe8`      | 2    | Set adapter parameters (interrupt model)       |
| `+0x100`     | 5    | DriverObject lookup                            |
| `+0x108`     | 11   | Get adapter handle                             |
| `+0x188`     | **11**| **Doorbell write (ring doorbell n)**          |
| `+0x248`     | (1ff4)| Register event handler                        |
| `+0x3f0`     | (1ff4)| Read PnP / class strings                       |
| `+0x418`     | 4    | WDF class bind (NOT firmware accessor)         |
| `+0x650`     | 41   | Get devExt from adapter ctx                    |
| `+0x680`     | 13   | StorPort logging / event                       |
| `+0x6d8`     | 1    | (single use in FUN_140008bc0)                  |
| `+0x9d8`     | **5**| **MMIO register-set programming (per-struct)**|
| `+0x9f8`     | 1    | Memory copy/init                               |
| `+0xdb0`     | many | Debug print                                    |

`+0x9d8` is called with a pointer to a small struct that describes a set
of MMIO writes. It is the only mechanism in `rcbottom.sys` for programming
the queue base/size/enable registers. Implementations of `+0x9d8` live in
storport.sys (out of scope for us); on Linux we replace it with direct
`writel()` to the same set of registers.

## Major finding 5 — Doorbell order 1,4,2,3 init vs. 1,2,3,4 re-arm

Two separate doorbell sequences exist:

- **Init** (`FUN_140008638` line 0x140008969..): order **1, 4, 2, 3**, with
  signature `service_188(stor_ctx, adapter, idx, 1)` — note the extra `1`.
- **Re-arm** (`FUN_14000924c`): order **1, 2, 3, 4**, signature
  `service_188(stor_ctx, adapter, idx)` (no extra arg).

The Linux driver only implements the init order. Re-arm may need adding
later, but it's not required to get the device responsive initially.

## Major finding 6 — `FUN_140005ff4` is the real HwInitialize body

This is what's called from `FUN_140008638` after WDF class binding. Key
work it does:

1. Registers two event handlers (`FUN_140006c34`, `FUN_140006da0`) via
   service `+0x248` (probably power/PnP state transitions).
2. Reads device PnP hardware ID string via service `+0x3f0` opcode `1`
   (e.g. `L"PCI\\VEN_1022&DEV_B000..."`).
3. Reads device compatible-ID string via service `+0x3f0` opcode `2`
   (e.g. contains `L"PCI\\CC_010801"` etc. — used to detect NVMe class).
4. Reads driver-version string via service `+0x3f0` opcode `10` from
   `DAT_1400142a0`. Parses out major/minor/build digits.
5. Looks up the per-adapter context via service `+0x650` and inserts it
   into the global adapter array at `DAT_140216780` (max 32 entries).
6. **Calls `FUN_140007d40(pnp_hw_id, devExt, isNvmeClass)`** — the
   callback installer. This is where the AHCI-vs-NVMe split happens.
7. Programs interrupt steering via service `+0x1a8` (passes `0xfff` mask).
8. Calls service `+0x2f0` with a 0x50-byte parameter block (probably MSI/X
   alloc).

## Major finding 7 — NVMe init (`FUN_14000dd44`)

For device 0xB000 this is the queue-init callback at `devExt+0x16110`.

Notable structural details:

- Number of I/O queues = `min(devExt[+0xb0] - 1, 4)` (PCI MSI count - 1,
  capped at 4). If only 1 vector or hibernate path, fall back to 1 queue.
- Each I/O queue handle is `0x78` bytes at `devExt + 0x15d98 + i * 0x78`.
- Per queue, allocate `0x30000` (192 KiB) DMA, 4 KiB-aligned. Layout:
  - `+0x14`: SQ DMA physical address (4 KiB-aligned)
  - `+0x18`: SQ next page
  - `+0x10`: CQ DMA virtual address
  - `+0x0c`: queue base virtual address
  - `+0xa` (u16): completion ring depth, capped at `0x400`
  - `+0xa` (u16) part 2: SQ depth, capped at `0x100`
- Per-queue MMIO programming is done in `FUN_14000f454` (called in a loop
  for each I/O queue + admin queue at index 0).
- The admin queue is the "first" queue handle at `devExt + 0x15d98`
  (`devExt + 0x15940` holds a pointer to it).

The `+0x9d8` calls at `FUN_140008a48` are about programming the **NVMe SQ
and CQ base/size registers** (admin queue, then the others are programmed
inside `FUN_14000f454`). The Linux driver currently writes to AHCI
register offsets here — that's the bug.

## Major finding 8 — `+0x9d8` is called with a struct, not naked args

All five `+0x9d8` call sites pass a pointer to a small struct (e.g.
`lVar2 + 0x16288`, `lVar2 + 0x1c2e8`, `&DAT_140216880`, `puVar17 + 0xdd`,
`puVar17 + 0xde`). The struct describes "program these MMIO registers
with these values". For the Linux port, we can synthesize the equivalent
writes from what we know about NVMe controller layout (BAR0 offsets
`0x14`, `0x18`, `0x1c`, `0x20`, `0x24`, `0x28`, `0x30`, `0x38`, `0x3c`,
and per-queue doorbells starting at `0x1000`).

## Concrete corrections to apply

- `rc_program_completion_registers()` in `rc_queue.c` writes to
  `0x100 + queue_idx*0x10`. **For device `0xB000` this is wrong.** Those
  offsets are not its completion registers. Correct programming is via
  NVMe AQA / ASQ / ACQ / CC registers in BAR0, plus per-queue doorbells
  at `BAR0 + 0x1000 + 2 * queue_idx * (4 << CAP.DSTRD)`.
- `rc_parse_firmware_capabilities()` in `rc_firmware.c` is a no-op stub
  that defaults to "safe mode (AHCI)". It needs to read the PCI device
  ID and select the AHCI or NVMe init path, exactly like
  `FUN_140007d40` does on Windows.
- All the planning around "descriptor accessor at `devExt+0x1C2D0`" and
  "implement FUN_14002ce29" can be **deleted** from the docs — it was
  based on a misunderstanding.
- `MISSING_COMPONENTS.md` items 1 (descriptor accessor) and 4
  (controller descriptor table) can be removed.

## What we still need

- `FUN_14000f454` decompilation — actual NVMe MMIO writes per queue
  (running, not yet read).
- `rcraid.sys` analysis — command submission and completion ISR. Most
  prior `rcraid` docs likely valid since the binary is unchanged, but
  worth a fresh pass on the completion path (`FUN_14000d2b8`).
- Confirmation of NVMe register offsets against the standard NVMe spec
  vs. what AMD's controller actually uses — most likely fully
  spec-compliant since `nvme.c` doesn't bother with device-specific
  branching.
