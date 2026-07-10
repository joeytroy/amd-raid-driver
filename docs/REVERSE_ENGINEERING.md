# Reverse-Engineering Reference

The authoritative record of what the AMD-RAID Windows driver actually does,
reverse-engineered from the shipped binaries, and how the Linux port mirrors
it. This is the single source of truth for the RE work; where an older doc
disagrees with this file, this file wins.

**Binaries analysed** (formerly under `drivers/windows/trx50/` — removed
from the tree; restore with `git checkout 69738ee -- drivers/` or
download from AMD):

| Binary | Version | Role |
|---|---|---|
| `rcbottom.sys` | 9.3.2-00255, 9.3.3-00291 | Per-PCI-device PnP driver: PCI/NVMe register programming, queue init, completion handling |
| `rcraid.sys` | 9.3.2-00255, 9.3.3-00291 | RAID virtualization layer: on-disk metadata, member config, RAID-level dispatch |
| `rccfg.sys` | both | User-space IOCTL surface for `rcadm`. Reference only — not ported |

Tooling: **Ghidra 12.1** headless (scripts in `scripts/ghidra/`) plus PyGhidra
for targeted decompiles. Raw extracts live under `docs/ghidra_output/`
(`rcbottom_9.3.2/`, `rcbottom_9.3.3/`, `rcraid_9.3.2/`). The Linux driver's
version string comes from the single-line `VERSION` file at the repo root
(injected by the Makefile) and names the AMD Windows release whose behavior
the port matches — currently `9.3.3.00291`. The geometry ground truth was
established on 9.3.2-00255 and re-verified on 9.3.3-00291 (see "Version
delta" below).

---

## Version delta: 9.3.2 vs 9.3.3

`rcraid.sys` is **NOT** byte-identical between the two releases (an early repo
note wrongly assumed it was, before the 9.3.3 binary was in the tree; corrected
2026-07-07):

| Binary | 9.3.2-00255 | 9.3.3-00291 | Δ |
|---|---|---|---|
| `rcraid.sys` | SHA256 `f0a6fc8b…`, 560,576 B | SHA256 `3f241608…`, 563,528 B | +2,952 B |
| `rcbottom.sys` | 106,384 B | 108,352 B | +1,968 B (localized bounds checks) |
| `rccfg.sys` | — | −600 B | CVE-2024-21962 fix (config IOCTL input validation) |

The CVE-2024-21962 fix lives entirely in `rccfg.sys`, which the Linux port does
not use. A string/symbol diff of the two `rcraid.sys` builds shows the only
genuinely new functional symbol is `RC_HW_NVME_EjectDrive` (a hot-eject /
surprise-removal handler); every other differing string is just the version
bump `RC-932`→`RC-933` in embedded source paths. **The geometry/data-path code
is functionally identical between versions** — verified by re-running the
pipeline on 9.3.3 (see "rcraid geometry" below). So 9.3.3 is a maintenance /
security release, not a feature release, for the NVMe RAID0 path this port
targets.

### Tracking a new AMD release

When AMD ships a new Windows RAID driver (e.g. a future 9.3.4):

1. Extract the new `rcraid.sys` / `rcbottom.sys` / `rccfg.sys` into
   `drivers/windows/trx50/<version>/`.
2. Re-run the Ghidra pipeline on them ("Reproducing the analysis" below) and
   string/symbol-diff against the previous release, focusing on the geometry
   parser and the NVMe queue-init path.
3. Update this section with the findings; port any data-path change the diff
   surfaces.
4. Bump the single-line `VERSION` file at the repo root to the new release
   (e.g. `9.3.4.00xxx`) — the Makefile injects it into the module banner and
   `modinfo`; no code change is needed for the version itself.

---

## The AHCI vs NVMe code-path split

The single most load-bearing finding: **device `0xB000` (the TRX50 NVMe RAID
Bottom) takes a completely different code path than the AHCI SATA variants.**
Early analysis treated the controller as AHCI and programmed completion
registers at `BAR0 + 0x100 + qid*0x10` — those offsets are AHCI port registers
and are meaningless for `0xB000`, which is why writes there never persisted.

`FUN_140007d40` (once mislabelled "firmware capability parsing") is the
**callback-table installer** that switches paths on the PCI hardware ID:

```
                                     +-- ahci.c path (iVar7 == 1)
 PnP HW-ID = VEN_1022 & DEV_7905  ---+
 PnP HW-ID = VEN_1022 & DEV_7916  ---+
 PnP HW-ID = VEN_1022 & DEV_7917  ---+
 PnP HW-ID = VEN_1022 & DEV_43BD  ---+

                                     +-- nvme.c path (iVar7 == 2)
 ANY OTHER ID + (param_3 != 0)    ---+   <-- includes DEV_B000

 ANY OTHER ID + (param_3 == 0)    ---+-- stub path (iVar7 == 99, no-op vtable)
```

The internal AMD source files are named in embedded error strings:
`…\rcbottom\rcbottom\ahci.c` (at `FUN_1400021d4`, AHCI completion-register
install) and `…\rcbottom\rcbottom\nvme.c` (at `FUN_14000dd44`, NVMe queue
init). The third parameter to `FUN_140007d40` is `1` iff the PCI class-code
string contains `"CC_010802"` (NVMe class), read via StorPort service `+0x3f0`
opcode `2` in `FUN_140005ff4`. This matches the INF exactly:

| Device | INF class | param_3 | Path |
|---|---|---|---|
| 0x7905 / 0x7916 / 0x7917 / 0x43BD | CC_0104 | 0 | AHCI (1) |
| 0xB000 | CC_0108 | 1 | **NVMe (2)** |

So `0xB000` is **always** NVMe.

### The installed vtable (`devExt+0x16100..0x16168`)

A 13-slot function-pointer table; `nvme.c` and `ahci.c` install different sets,
and the same outer code dispatches through them:

| Slot | AHCI (iVar7=1) | NVMe (iVar7=2) | Purpose |
|---|---|---|---|
| `+0x16100` | FUN_140004090 | FUN_14000fafc | Primary dispatch |
| `+0x16108` | FUN_140001438 | FUN_14000c0bc | Cmd submit |
| `+0x16110` | **FUN_1400021d4** | **FUN_14000dd44** | Queue/CR init |
| `+0x16118` | FUN_140003f7c | FUN_14000303c | Cleanup |
| `+0x16120` | FUN_140003048 | FUN_14000e800 | Disable/quiesce |
| `+0x16128` | FUN_1400027a8 | LAB_14000918c | Mode toggle |
| `+0x16130` | FUN_1400028f8 | FUN_14000e494 | Enable/resume |
| `+0x16138` | FUN_140001ba4 | FUN_14000c814 | State getter |
| `+0x16140` | FUN_140001bbc | FUN_14000c82c | Activity check |
| `+0x16148` | FUN_140003838 | FUN_14000d06c | Secondary disp |
| `+0x16158` | FUN_14000303c | FUN_14000e59c | Aux cleanup |
| `+0x16160` | FUN_140003598 | FUN_14001023c | Queue cleanup |
| `+0x16168` | LAB_140002808 | FUN_1400100c0 | Status update |

The stub path installs `FUN_1400102d8` (safe no-op) everywhere.

### HwInitialize body (`FUN_140005ff4`)

Called from `FUN_140008638` after WDF class binding. It: registers two
power/PnP event handlers via service `+0x248`; reads the PnP hardware-ID and
compatible-ID strings via `+0x3f0` opcodes 1/2 (the NVMe-class detection);
reads the driver-version string via `+0x3f0` opcode 10; inserts the per-adapter
context into the global array at `DAT_140216780` (max 32); **calls
`FUN_140007d40` (the AHCI/NVMe split)**; programs interrupt steering via `+0x1a8`
(mask `0xfff`); and calls `+0x2f0` with a 0x50-byte block (likely MSI/X alloc).

### StorPort service-slot inventory

The only StorPort vtable entries that matter for porting (hit counts from
`rcbottom.sys` 9.3.2):

| Slot | Hits | Used for |
|---|---|---|
| `+0xa8` / `+0xb0` / `+0xb8` | 7 each | DMA alloc / DMA→VA / DMA→PA |
| `+0xe8` | 2 | Set adapter parameters (interrupt model) |
| `+0x100` | 5 | DriverObject lookup |
| `+0x108` | 11 | Get adapter handle |
| `+0x188` | **11** | **Doorbell write (ring doorbell n)** |
| `+0x248` | many | Register event handler |
| `+0x3f0` | many | Read PnP / class strings |
| `+0x418` | 4 | WDF class bind (NOT a firmware accessor — see Debunked) |
| `+0x650` | 41 | Get devExt from adapter ctx |
| `+0x680` | 13 | StorPort logging / event |
| `+0x9d8` | **5** | **MMIO register-set programming (per-struct)** |
| `+0xdb0` | many | Debug print |

`+0x9d8` is the only mechanism in `rcbottom.sys` for programming queue
base/size/enable registers; each call passes a pointer to a small struct
describing "write these MMIO registers with these values." Its implementation
is inside storport.sys (out of scope); on Linux we replace it with direct
`writel()` to the NVMe BAR0 registers.

### Doorbell ordering

Two distinct sequences exist:
- **Init** (`FUN_140008638`, `0x140008969…`): order **1, 4, 2, 3**, signature
  `service_188(stor_ctx, adapter, idx, 1)` (note the extra `1`).
- **Re-arm** (`FUN_14000924c`): order **1, 2, 3, 4**, signature without the
  extra arg; runs after init.

Neither sequence is implemented in the Linux port, and none is needed: the
1-4-2-3 ordering belongs to the AHCI/StorPort `+0x188` template model, and the
NVMe path (`0xB000`) rings standard per-queue SQ/CQ doorbells at
`BAR0 + 0x1000 + (2*qid + dir) * (4 << CAP.DSTRD)` instead
(`rc_nvme_ring_sq_doorbell` in `rc_nvme.c`).  An earlier revision of this
section claimed a Linux `rc_activate_doorbells()` implemented the 1-4-2-3
order — no such function ever existed; the only related helper,
`rc_bottom_init_bar_templates()`, is deliberately a no-op.  Revisit only if
the AHCI path is ever built.

---

## NVMe queue init and multi-queue architecture

`FUN_14000dd44` is the NVMe queue-init callback (vtable `+0x16110`) for
`0xB000`. Structural details:

- I/O queue count = `min(devExt[+0xb0] − 1, 4)` — MSI vector count minus one,
  **hard-capped at 4** regardless of CPU count. Single-queue fallback if only 1
  vector (`devExt+0xb0 == 1`), hibernate path, or forced via `devExt+0xb4`.
- Each I/O queue handle is `0x78` bytes at `devExt + 0x15d98 + i*0x78`; the
  admin queue is index 0 (`devExt+0x15940` points at it).
- Per queue, allocate `0x30000` (192 KiB) DMA, 4 KiB-aligned. Layout confirmed
  from `FUN_14000f454`:

  | Offset | Size | Purpose |
  |---|---|---|
  | 0x00000 | 16 KiB | SQ entries (256 × 64 B) |
  | 0x08000 | 16 KiB | CQ entries (1024 × 16 B) |
  | 0x10000 | 128 KiB | PRP list region |
  | 0x18000 | 32 KiB | slack |

- Per-queue depth (`FUN_14000f454`): `SQ = min(MQES, 0x100) = 256`,
  `CQ = min(MQES, 0x400) = 1024`, where `MQES = CAP[15:0]` read from BAR0. On
  the dev box T700s `CAP=0x080000203c01ffff` → `MQES=0xffff`, so both clamp.
- Windows also keeps a static per-controller-per-queue command-tracking buffer
  of `0xf000` bytes (256 cmds × 120 B) at
  `&DAT_14021e8c0 + (queue + (ctrl_idx−1)*4)*0xf000`.

The `+0x9d8` calls at `FUN_140008a48` program the NVMe SQ/CQ base/size
registers (admin queue first, then the rest inside `FUN_14000f454`).

### Windows target → Linux (IMPLEMENTED)

Windows runs **4 I/O queues × SQ depth 256 = 1024 outstanding commands** per
controller. The Linux port **now matches this** (it was 1 queue × 64 in the
first bring-up):

| | Original Linux | Windows target | Linux today |
|---|---|---|---|
| I/O queues / controller | 1 | 4 | `RC_NVME_IO_QUEUE_TARGET = 4` |
| SQ depth / queue | 64 | 256 | `RC_NVME_IO_QUEUE_DEPTH = 256` |
| Total outstanding | 64 | 1024 | 1024 (4 × 256) |
| blk-mq `nr_hw_queues` | 1 | 4 | 4 (`queue_depth=256` → 1024) |

Implemented as: MSI→MSI-X migration (`pci_alloc_irq_vectors`, `N+1` vectors);
Set Features Number of Queues (admin opc 0x09, FID 0x07) at init; N SQ/CQ pairs
each with its own doorbells, DMA buffers, and MSI-X vector/ISR; blk-mq per-CPU
hctx with `.map_queues = blk_mq_pci_map_queues`; queue depth 256. The queue
count caps at `min(num_online_cpus, MSI-X vectors − 1, 4)` — on the dev box
(32+ CPUs, 128 MSI-X vectors) that resolves to 4, matching Windows.

Deliberate differences from Windows: no static cmd-tracking buffer (blk-mq
tagset does that — 256 tags × N queues × `sizeof(pdu)`, each request carries a
per-hctx PRP-list buffer); `scatterlist`-native DMA; **CQ depth 256, not
Windows' 1024** (the CQ is sized equal to the SQ — with at most 256 commands
outstanding per queue a 256-entry CQ can never overflow, so the 4× headroom
Windows allocates buys nothing; this follows Linux `nvme-core` convention);
and the software design follows Linux `nvme-core` patterns since the
on-the-wire NVMe protocol is spec-defined — the Windows RE only fixed the
*queue count/depth* target.

> The per-hctx dimension on the PRP-list pool matters: `req->tag` is unique
> only within one hctx, so two concurrent commands on different queues can draw
> the same tag and must not share a PRP buffer. See `rc_nvme.c`
> (`rc_volume_prp_va[hctx][member][tag]`).

---

## rcraid.sys geometry and metadata parsing

Ground truth for the on-disk RAID-metadata layout, verified against
`rcraid.sys` 9.3.2-00255. **Bottom line: the Linux driver's RAID0 geometry
parsing is correct** — chunk-size mapping, struct offsets, and RAID-level
magics all match the binary; the one uncertain field (member count) was settled
on hardware as `0x68`.

### Parse flow

| Function | Addr | Role |
|---|---|---|
| `RC_ReadMetaData` | `0x14001fd60` | reads the config packet off each member |
| `RC_BuildConfigMetadataFromRing` | `0x140019fb8` | walks the config ring: PhysicalDevice (`0x25bc`) then LogicalDevice (`0x25bd`) records |
| `FUN_140018444` | `0x140018444` | **copies on-disk LD packet → in-memory LD** (offset source of truth) |
| `FUN_14001c674` | `0x14001c674` | dispatches on DeviceType → installs per-RAID-level I/O handlers |
| `FUN_1400121d0` | `0x1400121d0` | **chunk_index → stripe-size mapping** |

Records are tag-prefixed `u32`s (`0x25bc` = `RC_DST_PHYSICAL_DEVICE`, `0x25bd`
= `RC_DST_LOGICAL_DEVICE`), walked until the tag no longer matches.

### On-disk LogicalDevice packet layout (verified)

From `FUN_140018444` (`param_2` = on-disk packet, `param_1` = in-memory LD):

| Field | On-disk offset | Linux `#define` | Status |
|---|---|---|---|
| record magic `0x25BD` | `+0x00` | `RC_DST_LOGICAL_DEVICE` | ✅ |
| DeviceType | `+0x0C` | `RC_LD_DEVICETYPE_OFFSET 0x0C` | ✅ |
| element-array offset | `+0x04` | `RC_LD_ELEMENTOFFSET_OFFSET 0x04` | ✅ |
| Capacity (u64) | `+0x50` | `RC_LD_CAPACITY_OFFSET 0x50` | ✅ |
| DEVICES (member count) | `+0x68` | `RC_LD_DEVICES_OFFSET 0x68` | ✅ **confirmed on hw (=2)** |
| (FirstCount) | `+0x6C` | `RC_LD_FIRSTCOUNT_OFFSET 0x6C` | ✅ |
| (SecondCount) | `+0x70` | `RC_LD_SECONDCOUNT_OFFSET 0x70` | ℹ️ =1 on 2-member RAID0 — NOT the member count |
| PacketSize | `+0x90` | `RC_LD_PACKETSIZE_OFFSET 0x90` | ✅ |
| ChunkSize (u32 sectors; 0 for RAID0) | `+0xAC` | `RC_LD_CHUNKSIZE_OFFSET 0xAC` | ✅ |
| chunk_index (u32) | `+0x110` | `RC_LD_CHUNKINDEX_OFFSET 0x110` | ✅ |

`FUN_140018444` defaults chunk_index to 1 when the on-disk field is 0. The
per-member `RC_LogicalElement` `UserDataOffset` at LE `+0x20` is each member's
physical data offset (`ld_userdata_offset`; = 1069056 sectors on the
workstation array).

### chunk_index → stripe size (verified — the main "guess")

`FUN_1400121d0` maps chunk_index (in-mem `piVar2[0x32]` ← on-disk `+0x110`) to
a stripe size in 512-byte sectors:

```c
if (chunk_index == 3)      size = 0x200;   // 512 sectors = 256 KiB
else { size = 0x80;                        // 128 sectors =  64 KiB (default)
       if (chunk_index == 2) size = 0x100; } // 256 sectors = 128 KiB
```

Identical to the Linux `rc_volume_chunk_sectors_for` (`case 3 → 512`,
`case 2 → 256`, `case 1/default → 128`). Windows handles only `2` and `3`
explicitly; every other value (including `1` and `0`→`1`) falls to 64 KiB.
**There is no index ≥ 4 handling** — see the latent gap below.

### DeviceType → RAID level (verified)

`FUN_14001c674` switches on DeviceType (in-mem `+4` ← on-disk `+0x0C`):

| DeviceType | RAID level | Linux `#define` |
|---|---|---|
| `0x1BF5` | (RAID-related; not in Linux enum) | — |
| `0x1BF6` | RAID0 | `RC_LDT_RAID0` ✅ |
| `0x1BF7` | RAID1 | `RC_LDT_RAID1` ✅ |
| `0x1BF9` | (per-member "raw disk" variant) | — |
| `0x1BFA` | RAID5 | `RC_LDT_RAID5` ✅ |
| `0x1BFB` | RAID10 | `RC_LDT_RAID10` |

AMD also writes one single-device "raw disk" LD per physical member
(`devtype=0x1BF9, devices=1, capacity=NSZE`); the parser skips those by
checking whether our `DeviceId` appears in the element array.

### Member count resolved: `0x68`, not `0x70`

`FUN_140018444` copies three adjacent counts (`0x68`→`[0x2c]`, `0x6C`→`[0x2d]`,
`0x70`→`[0x2e]`). A static read of the striping calc suggested `0x70` might be
authoritative, but **on-hardware logging disproved it**: on the healthy
2-member RAID0, `devices(0x68)=2` and `second_count(0x70)=1`. So `0x68` is the
member count (Linux is correct); the driver still logs `0x70` for forensics on
exotic layouts but deliberately keys off `0x68` — keying off `0x70` would have
mis-counted the array as 1 member and broken assembly. A clean example of
empirical validation catching a plausible-but-wrong decompiler inference.

### Latent gap: chunk_index ≥ 4

Neither Windows (9.3.2 **or** 9.3.3) nor the Linux driver handles
`chunk_index ≥ 4`; both silently fall back to 64 KiB. If a future
RAIDXpert2/firmware ever encodes a 512 KiB or 1 MiB stripe as index 4/5, both
drivers would mis-map and corrupt data on write. The gap is inherent to AMD's
encoding, not a version quirk. The Linux driver's fail-closed guard (mark
unrecognized chunk_index untrusted) covers it — the geometry write-veto alone
would not, since the LD parses fine and only the stripe size is wrong.

### Cross-version check (9.3.3-00291)

Same pipeline on 9.3.3: the binary differs (SHA256 `3f241608…` vs `f0a6fc8b…`,
+2,952 B) but the **geometry parsing is unchanged** — identical chunk_index
ladder (no index ≥ 4), same `[0x32]`←`+0x110` field, same ChunkSize `+0xAC`,
same RAID-level magics (`0x1BF6/7/A/B`). All geometry conclusions hold for
9.3.3 because the code is unchanged, not because the binary is.

---

## Debunked theories (do not re-investigate)

Each wasted significant effort in the 2024 analysis round. They are wrong; the
2024 docs under `docs/archive/legacy_2024/` still describe them and are
retained only as historical record.

- **"Descriptor accessor at `devExt+0x1C2D0`"** — it's the WDF/KMDF class-bind
  function pointer installed by service `+0x418`, not a firmware-capability
  accessor.
- **`DAT_140012258` "firmware capability blob"** — it's the WDF version-bind
  table (GUIDs + KMDF strings like `"KmdfLibrary"`, `"ZPODDWORKITEM"`).
  Confirmed by the hex dump in
  `docs/ghidra_output/rcbottom_9.3.2/firmware_descriptor_blob.txt`. The
  "opcodes" `0x05/0x10/0x11/0x34/…` are WDF API indices, varying by
  device-family because different families bind different WDF versions.
- **`FUN_14002ce29`** — outside `rcbottom.sys` (binary ends near
  `0x14001a700`); the function does not exist in this binary.
- **"40-entry controller descriptor table" (`DAT_1400140B0`)** — never existed.
- **Completion register writes at `BAR0 + 0x100 + qid*0x10`** — those are AHCI
  port registers, wrong for `0xB000` by construction.

**Implication:** there is no descriptor accessor to reimplement. Everything the
driver needs comes from PCI config space (vendor/device/class) and the NVMe
controller registers in BAR0, both of which Linux exposes directly.

---

## Open questions

Still needing Ghidra investigation. Run via `scripts/ghidra/HuntBlockers.java`
(command line in `docs/README.md`).

### High priority — the NVMe path to issue commands

1. **`FUN_14000f608` (rcbottom.sys)** — per-queue MMIO arming, called from
   `FUN_14000dd44` once per I/O queue. We have the prep code (DMA alloc, handle
   layout) in `ghidra_output/rcbottom_9.3.2/key_funcs.c` but not this. Want:
   doorbell stride/base confirmation (`BAR0 + 0x1000` vs vendor-specific);
   Create-IO-CQ/SQ command shapes if AMD uses non-standard fields; whether it
   pre-creates the CQ via admin or programs MMIO directly.
2. **`rcraid.sys` NVMe completion ISR (`FUN_14000d2b8`)** — never decompiled.
   Want: exact CQ entry layout (16-byte NVMe-spec vs AMD extension); phase-bit
   walk vs head-pointer DMA write-back; how error/Identify completions route
   back to the submitter.
3. **Per-namespace discovery in `rcraid.sys`** — where it issues Identify and
   assembles RAID metadata. Want: any vendor-specific RAID-enumeration opcode
   (or confirmation it's all standard Identify + Read); the metadata
   signature/version constants.

### Medium priority

4. **Service-slot `+0x9d8` struct shape** — called 5× with a "program these
   MMIO registers" struct (see
   `ghidra_output/rcbottom_9.3.2/indirect_call_offsets.txt`). Not strictly
   needed (we replaced it with direct `writel` in NVMe mode), but tracing
   `FUN_140008a48`'s argument shape would let us write a general helper and drop
   the duplicated code in `rc_hw.c`.

### Low priority — only if the controller refuses to come ready

5. **`FUN_140008f34` BAR discovery** — walks `MmMapIoSpace`-ed BARs into the
   device extension; should match Linux `pci_iomap`. Re-read if the
   `rc_nvme_init_controller` read-back fails.
6. **Vendor PCI config writes before MMIO bring-up** — `FUN_140005ff4` calls
   `+0xa8/+0xb0/+0xb8/+0xe8/+0x108/+0x188` first. Mostly DMA setup, but `+0xe8`
   (a 0x1c-byte struct `0x1c,4,2,2,2,2,1`) may set adapter parameters we don't
   replicate. Worth a look if NVMe init produces a controller fatal status.

### Resolved

- **Volume-metadata parsing** — fully understood (see "rcraid geometry" above,
  cross-checked against DWARF in the AMD Linux SDK's `rcblob.x86_64`). The
  516-byte `RC_LogicalDevice` (`__packed__`, first u32 = `0x25BD`) in the config
  ring at `ConfigRingOffset` (LBA 0x5800 on the dev box) yields member count
  (`+0x68`), element-array offset (`+0x04`), capacity (`+0x50`), DeviceType
  (`+0x0C`), and per-member `DeviceID` matching for position.
- **9.3.2 vs 9.3.3 binary diff** — done (see "Version delta"): binaries differ
  but the data-path/geometry code is functionally identical. Re-run the string
  diff if a 9.3.4 ships.

---

## Reproducing the analysis

Ghidra 12 needs JDK 17+. The project is stored outside the repo.

```sh
export JAVA_HOME="C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot"

# Re-import (binaries are no longer in the tree — first restore them
# with `git checkout 69738ee -- drivers/` or download from AMD)
"C:\Program Files\ghidra_12.1_PUBLIC\support\analyzeHeadless.bat" \
  C:/Users/dev/ghidra_projects amd-raid \
  -import drivers/windows/trx50/9.3.2-00255/rcbottom/rcbottom.sys -overwrite

# Re-run the hunt (writes into docs/ghidra_output/rcbottom_9.3.2/)
"C:\Program Files\ghidra_12.1_PUBLIC\support\analyzeHeadless.bat" \
  C:/Users/dev/ghidra_projects amd-raid \
  -process rcbottom.sys -noanalysis \
  -scriptPath scripts/ghidra -postScript HuntBlockers.java \
  docs/ghidra_output/rcbottom_9.3.2
```

For targeted decompiles (PyGhidra venv, `GHIDRA_INSTALL_DIR` set):

```python
import pyghidra; pyghidra.start()
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
with pyghidra.open_program(SYS, analyze=True) as api:
    prog = api.getCurrentProgram()
    dec = DecompInterface(); dec.openProgram(prog)
    addr = prog.getAddressFactory().getDefaultAddressSpace().getAddress(0x1400121d0)
    f = prog.getFunctionManager().getFunctionContaining(addr)
    print(dec.decompileFunction(f, 60, ConsoleTaskMonitor()).getDecompiledFunction().getC())
```
