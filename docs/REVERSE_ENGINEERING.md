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
version string comes from the single-line `docs/VERSION` file (the installers
stage it as `VERSION` for the Makefile to inject into the banner and `modinfo`)
and names the AMD Windows release whose behavior the port matches — currently
`9.3.3.00291`. The DKMS *package* version is a separate knob: `PACKAGE_VERSION`
in `dkms.conf` (`1.0.0-custom`), read by both installers so the tree they stage
under `/usr/src/rcraid-<version>` always matches the version they hand to
`dkms`. The geometry ground truth was established on 9.3.2-00255 and
re-verified on 9.3.3-00291 (see "Version delta" below).

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
4. Bump the single-line `docs/VERSION` file to the new release (e.g.
   `9.3.4.00xxx`) — the Makefile injects it into the module banner and
   `modinfo`, and the installers carry it into the DKMS tree; no code change is
   needed for the version itself. `PACKAGE_VERSION` in `dkms.conf` is separate:
   change it only when the *package* version should move, since it names the
   `/usr/src/rcraid-<version>` directory the installers stage into.

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
| FirstCount (stripe width) | `+0x6C` | `RC_LD_FIRSTCOUNT_OFFSET 0x6C` | ✅ **=2 on RAID0, =1 on RAID1 (hw)** |
| SecondCount (mirror count) | `+0x70` | `RC_LD_SECONDCOUNT_OFFSET 0x70` | ✅ **=1 on RAID0, =2 on RAID1 (hw)** |
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

### DeviceType → RAID level: the on-disk truth is FirstCount × SecondCount

`FUN_14001c674` switches on DeviceType (in-mem `+4` ← on-disk `+0x0C`):

| DeviceType | RAID level | Linux `#define` |
|---|---|---|
| `0x1BF5` | RAID5 (confirmed 2026-11-08 on a BIOS-created 3-disk array) | `RC_LDT_RAID5` ✅ |
| `0x1BF6` | RAID0 | `RC_LDT_RAID0` ✅ |
| `0x1BF7` | RAID1 | `RC_LDT_RAID1` (never yet seen on disk) |
| `0x1BF9` | (per-member "raw disk" variant) | `RC_LDT_SINGLE` |
| `0x1BFA` | RAID6 (parity count = `(devtype == 0x1BFA) + 1`) | `RC_LDT_RAID6` |
| `0x1BFB` | RAID10 | `RC_LDT_RAID10` |

**Hardware correction (2026-07-10, TRX50 metadata dumps during RAID1
validation): firmware writes DeviceType `0x1BF6` for BIOS-created RAID1
volumes too.** The record for the live 2-disk RAID1 reads
`devtype=0x1BF6, FirstCount=1, SecondCount=2, Capacity=<one member's user
size>`; the old RAID0's record reads `devtype=0x1BF6, FirstCount=2,
SecondCount=1, Capacity=<2× user size>`. So the on-disk RAID level is
encoded as **FirstCount × SecondCount = stripe width × mirror count**
(`FirstCount * SecondCount == Devices` on every observed record, including
the `0x1BF9` raw-disk LDs at 1×1), and `0x1BF6` is best read as "generic
striped/mirrored volume". Whatever the `0x1BF7` dispatch arm in
`FUN_14001c674` consumes, it is not what this firmware writes to disk —
the Windows in-memory DeviceType is presumably recomputed from the counts.
The Linux driver derives the RAID0/RAID10/RAID1 level from the counts
(`rc_ld_level_from`) and refuses combinations it has no mapping for. Parity
levels are the one exception — the counts cannot express them at all, so
`0x1BF5` / `0x1BFA` select RAID5 / RAID6 directly and a count-encoded
`0x1BF6` record is never read as a parity array (issue #3). This encoding was
independently corroborated on X399 SATA RAID10 (`FirstCount=2,
SecondCount=2, devices=4`) — see
[External corroboration](#external-corroboration-x399-zenith-extreme-sata-raid10)
below.

**Hardware correction (2026-11-08): the two parity DeviceTypes were
swapped in the table above.** A RAIDXpert2 BIOS-created 3-disk RAID 5
(`devices=3, FirstCount=1, SecondCount=3, Capacity=2 × per-member user
size`) writes `DeviceType=0x1BF5`. The vendor dispatch confirms the
direction: `FUN_140055760` and `FUN_140052404` compute
`parity_count = (devtype == 0x1BFA) + 1`, so `0x1BFA` is the two-parity
(RAID6) encoding and `0x1BF5` is single-parity (RAID5). The on-disk parity
*placement* is settled from the vendor core (issue #6, item 7 below).

AMD also writes one single-device "raw disk" LD per physical member
(`devtype=0x1BF9, devices=1, capacity=NSZE`); the parser skips those by
checking whether our `DeviceId` appears in the element array.

### Member count resolved: `0x68`, not `0x70`

`FUN_140018444` copies three adjacent counts (`0x68`→`[0x2c]`, `0x6C`→`[0x2d]`,
`0x70`→`[0x2e]`). A static read of the striping calc suggested `0x70` might be
authoritative, but **on-hardware logging disproved it**: on the healthy
2-member RAID0, `devices(0x68)=2` and `second_count(0x70)=1`. So `0x68` is the
member count (Linux is correct). The 2026-07-10 RAID1 dumps then identified
what `0x70` actually is — the **mirror count** (see the FirstCount ×
SecondCount section above): keying off it on RAID0 would have mis-counted the
array as 1 member, and the "forensic" logging of it is what let the RAID1
level encoding be recognized. A clean example of empirical validation
catching a plausible-but-wrong decompiler inference — twice.

### Config ring is a journal; the commit block names the active generation

**Verified on hardware (2026-07-10), found the hard way.** The config ring
(LBA `0x5800`, `ConfigRingSize`=2048 sectors) is not a table — it is a
journal of complete config **generations**, appended on every config change
(array create/delete, and periodically: the dumps show ~19 generations for
a handful of user actions). Each generation is:

```
+0x000  header sector: u64 timestamp, then intra-generation table offsets
+0x200  packed records: PhysicalDevice (0x25BC) × N, LogicalDevice (0x25BD) × M
```

Deleted arrays' LD records **persist in older generations** with no
tombstone — the whole generation is simply superseded. The **config-commit
block** (one sector at `RC_MetaData.ConfigCommitOffset`, `0x5001` on every
observed disk) is the two-phase-commit pointer that names the live
generation:

| Offset | Field |
|---|---|
| `+0x00` | u64 commit-write timestamp |
| `+0x08` | u32 active generation start LBA |
| `+0x0C` | u32 active generation length, bytes |
| `+0x10` | u64 active generation's timestamp (== gen header `+0x00`) |
| `+0x30` | u32 generation sequence number |

Consequence: **scanning the ring from the start and taking the first LD
whose element array contains the disk's DeviceID assembles a DEAD array.**
DeviceIDs are per-physical-disk and survive array delete/create, so a
deleted RAID0's record matches the same disks as the live RAID1 created
after it. That exact bug shipped and presented a deleted 2-disk RAID0 (4 TB,
striped, writable) in place of the live RAID1 (2 TB, mirrored) during
hardware validation — caught only because the size looked wrong before the
OS install. The parser must read the commit block, validate the extent and
the timestamp linkage, and only trust records inside the committed
generation. One quirk: the live generation carries the volume LD **twice**
(identical geometry, two trailing fields differing — possibly
current-vs-target tables for migration); first match within the committed
extent is safe precisely because they agree.

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

### External corroboration: X399 (Zenith Extreme) SATA RAID10

Reported in issue #57 (George "PlaidPiper" Crossley; original report July
2026, cross-dump 2026-10-07), cross-linked
from [this repo's issue #1](https://github.com/kubuntugamer/amd-raid-driver/issues/1).
A firmware-created **SATA** RAID10 over 4× WD100EMAZ on X399 (Zenith
Extreme) parses to the same packet this driver expects; the four raw
metadata dumps are published as
[`PlaidPiper/x399-raidcore-fixtures`](https://github.com/PlaidPiper/x399-raidcore-fixtures)
(commit block at LBA `0x5001`, config ring at `0x5800`, 2048 sectors) and
were verified against this port's parser (bottom of this section):

| Field | Observed |
|---|---|
| `DeviceType` (`+0x0C`) | `0x1BF6` (generic striped/mirrored) |
| `devices` (`0x68`) | 4 |
| `FirstCount × SecondCount` | `2 × 2` |
| `chunk_index` (`+0x110`) | `1` → 64 KiB |
| per-member `UserDataOffset` (LE `+0x20`) | 1069056 sectors |

All four members carried byte-identical committed records, and the config
ring's **stale generations were correctly skipped** — the same
journal/commit-block semantics documented above. He assembled a
**read-only** view with `dm-stripe` over the two columns (one member per
mirror pair, chunk-interleaved) and mounted it `ntfs3` with the MFT
parsing cleanly (~395 MB/s sustained).

What this establishes, on firmware years older and on a different bus
than the TRX50 NVMe dumps: the `FirstCount = stripe columns`,
`SecondCount = mirror depth` encoding; the committed-generation journal
behaviour; the `chunk_index` ladder; and the column-chunk interleave
model (`logical chunk N` → column `N mod cols`, physical chunk
`N div cols`, both copies).

**Cross-dump settles the pair convention (2026-10-07).** PlaidPiper
subsequently dd'd all four members and compared the raw data at the same
`UserDataOffset` (joeytroy#57 comment, 2026-10-07): element 0 ≡ element 1
and element 2 ≡ element 3 byte-for-byte, while the two pairs differ from
each other — sampled at both `UserDataOffset + 0` and
`UserDataOffset + 1 GiB`. That is exactly the **adjacent** pairing
`[A0, A1] | [B0, B1]` this driver assumes (`BIT(2*c) | BIT(2*c+1)`), at
both elements of both pairs. The last unverified RAID10 geometry
assumption in this port is now confirmed on real hardware — see
[RAID10 and parity geometry validation](VALIDATION_RAID10.md).

Caveats: the cross-dump covers a 2×2 set only (no 4-way `0x41BF7` or
8-way `0x81BF8` topology has been cross-dumped), and "element index"
means the order of records *inside the committed LD* — nothing yet proves
that order equals physical port order (for a 2-way set a permutation of
elements is equivalent, so the pairing conclusion holds either way). The
comparison itself cannot be re-run from the published fixtures: those
dumps cover LBA `0x5000`–`0x6000` only and stop 1,044,480 sectors short of
`UserDataOffset`, so the data-identity half rests on the reported `dd`
output rather than on a file in the tree. The metadata half above is
reproducible from the fixtures.

**Fixtures independently verified against this port's parser (2026-10-07).**
The four 2 MiB raw metadata dumps (LBA `0x5000`) are published as
[`PlaidPiper/x399-raidcore-fixtures`](https://github.com/PlaidPiper/x399-raidcore-fixtures)
(referenced from [this repo's issue #1](https://github.com/kubuntugamer/amd-raid-driver/issues/1)).
All four were run through a userspace port of this driver's exact
validation chain (`src/rc_nvme.c`): magic `RAIDCore`, version
`0x00030000`, XOR-lane-shuffle checksum over `[0x08..0x1FF]` valid,
commit block at `0x5001` valid, generation-timestamp linkage
(`0x44e3270d8b7f9c46`, byte-identical across members) matching the
generation header, LD tag `0x25BD` found — **all four parse clean, no
warnings.** Further corroborated: the committed LD record is byte-identical
across members; RAID10 derived as 2×2 with `devices = 4`, `devtype
0x1BF6`; capacity `39,063,609,344 = 2 × UserDataSize 19,531,804,672`;
`chunk_index = 1` → 64 KiB; element `DeviceID`s match each member's own
`device_id`; `my_pos` 0–3 consistent; per-member header fields (`mbr_checksum`,
`checksum`, `device_id`, generation LBA/sequence) differ exactly as
designed while the committed records match. The committed generation
contains **no** single-disk `0x1BF9` logical devices — they appear only in
stale ring generations, and the commit-block-following logic correctly
ignores them: the dead-generation pitfall documented above is present in
real X399 data and handled.

What it does **not** establish: the `1022:B000` NVMe path (bring-up,
completion, degraded failover, rebuild — separate code, never run here),
since these dumps are from a SATA/Promontory array.

### External corroboration: the vendor blob runs on modern Linux (DesktopECHO)

[`DesktopECHO/rcraid-nvme`](https://github.com/DesktopECHO/rcraid-nvme) (v2.0,
2026-03) takes the *opposite* approach to this port: it repackages AMD's own
`rcraid` 9.3.3.302 (Lenovo distribution, February 2026) and binary-patches the
proprietary `rcblob` so it loads on kernels 6.12–7.0+, targeting Ryzen APUs
(MinisForum UM890 Pro, Ryzen 9 8945HS). It is a different driver — it never
executes this clean-room code — but its technical write-up documents the vendor
architecture from the inside, and that independently corroborates several
load-bearing conclusions here:

| This port's finding | Vendor blob, as described by DesktopECHO |
|---|---|
| `0xB000` is the NVMe RAID trap; NVMe class `CC_010802` | wrapper adds `AMD_NVME_DID = 0xb000` to `rcraid_id_tbl[]` |
| Trapped device resolves to the real drive via EFI | wrapper implements `RC_Unmap_VidDid()`; Promontory EFI exposes `NvmeTrapDeviceVar`, APUs do not |
| No descriptor accessor; queue init is direct BAR0 + PCI config | blob embeds a complete NVMe controller (168 `NVM_` symbols) that owns admin/I/O queues, doorbells and MSI-X and does **not** wrap the kernel nvme driver |
| Thin OS seam around a proprietary core | 56 `RC_HW_OS_` callbacks are described as "the only seam between the proprietary RAID engine and the Linux kernel" |
| Simple mirror/stripe levels need no generic state machine | Bypass path (`RC_StartBypassIo`) handles RAID 0/1/JBOD; the 1,180-state generic path handles the complex levels |
| RAID 10 is a first-class level, and levels are gated | engine supports 0/1/5/6/10/50/60/Volume/RAIDABLE; gated by CPUID license level (`RC_HW_OS_GetLicenseLevel`) and `RC_CoreFeatureSet` |

Two further takeaways. First, the architecture is unchanged in the **newest**
engine build, 9.3.3.302 (Feb 2026) versus the 9.3.3-00291 analysed here — the
same maintenance-only story as the 9.3.2→9.3.3 delta above. Second, the vendor
rebuild path uses a **QSync bitmap** for sub-stripe targeted resynchronization
where this port currently rebuilds a whole member.

What it does **not** establish: it runs the vendor engine, not this port, so it
cannot surface port bugs; and its primary validation
target is an APU without a Promontory chipset, though it documents the
Promontory `1022:B000` path. (The RAID 10 adjacent-vs-strided pairing it also
left unsettled has since been closed by the X399 cross-dump above.) Its repo also ships
`raidxpert2-9.3.3_00302-1.x86_64.rpm`, whose engine is newer than the binary
analysed here — a possible target for the Ghidra pipeline once a Linux-ELF
(rather than PE) workflow is in place.

---

## rcadm `/dev/rc_api` config-packet ABI

Settled by disassembling the shipped `rcadm` 9.3.3-00302 (symtab + DWARF
intact). This section covers the userspace side of the ABI only; it does not
claim anything about the vendor kernel driver's own validation.

### Transport

| Item | Value |
|---|---|
| Device | `/dev/rc_api`, opened `O_RDWR` + `O_NONBLOCK` |
| Framing | `ioctl(fd, SG_IO, sg_io_hdr)` (`SG_IO` = `0x2285`), timeout `0xEA60` = 60 s |
| Identify | `INQUIRY` EVPD page `0xD7` → `"AMD-RAID Controller"` (`SG_InquiryD7`) |
| Data | `READ(10)` / `WRITE(10)` via `sg_read`/`sg_write`, 512-byte blocks, `sg_err_category3()` on completion (`SG_ReadWrite`, `lxdev_sglib.cpp:750`) |
| Descriptor | `READ(10)` LBA 0, 2 blocks (1 KiB) |
| Config packet | 64 KiB (`PacketSize` `0x4000` populated of a 64 KiB read) |

`SG_ReadWrite(int fd, unsigned dir, uchar *buf, int len, long long lba)` rounds
`len` up to 512-byte blocks and calls `sg_write` when `dir == 0`, `sg_read`
otherwise. `SG_Scsi_IO` fills `sg_io_hdr` with interface id `'S'`, the CDB, the
data pointer/length, 60 s timeout, and returns 0 on sense-free completion.

### Header: `RC_ConfigPacketHeader` (packed, 512 B)

From DWARF; every offset is confirmed against the captured packets.

| Offset | Size | Field | Captured value |
|---|---|---|---|
| `0x00` | 8 | `Checksum` | `f66466def9884e30` |
| `0x08` | 4 | `PreviousConfigPacketOffset` | |
| `0x0C` | 8 | `HeaderKey` | |
| `0x14` | 4 | `Version` | `1` |
| `0x18` | 4 | `PacketSize` | `0x4000` |
| `0x1C` | 8 | `ModifierSerialNumber` | |
| `0x24` | 4 | `PhysicalDeviceOffset` | `0x200` |
| `0x28` | 4 | `PhysicalDeviceSize` | `0x500` |
| `0x2C` | 4 | `LogicalDeviceOffset` | `0x700` |
| `0x30` | 4 | `LogicalDeviceSize` | `0x310` |
| `0x34` | 8 | `GenerationNumber` | `10` |
| `0x3C` | 4 | `WaitForAcceptConfigChange` | |
| `0x40` | 4 | `ControllerDeviceOffset` | `0xA10` |
| `0x44` | 4 | `ControllerDeviceSize` | `0x330` |
| `0x48` | 4 | `SEPDeviceOffset` | `0xD40` |
| `0x4C` | 4 | `SEPDeviceSize` | `0` |
| `0x50` | 432 | `Reserved[108]` | |

### Checksum: not validated by `rcadm`

`RC_ApiGenCRC32` (`0x4F730`) has exactly five call sites, all in firmware
update: `RC_ApiUpdateFirmwareRegion` (`0x554A0`, ×2), `RC_ApiUpdateFirmwareFrame`
(`0x559E0`, ×2), `RC_ApiUpdateHDDFirmware` (`0x5ACB0`, ×1). Nothing on the config
read path calls it or compares its result. The config walkers read **only**
header `+0x34` (`GenerationNumber`) and then start at packet `+0x200`; the
`Checksum` word at `+0x00` is never loaded (`RC_Core::RC_InitDeviceCount`
`RC_Core.cpp:884`, `RC_Core::RC_BuildConfig` `RC_Core.cpp:741`).

So the unresolved `0xf66466def9884e30` value is **not** a gate for making
`rcadm` read a synthesized packet. A packet builder need not reproduce it.
(The vendor kernel driver's own check, if any, is irrelevant once this driver
replaces it.)

### Walker semantics (what the parser actually honours)

`RC_ApiParseCoreConfig` (`0x4D0D0`, `rcparseconf.cpp:144`) only turns the
textual `RC_ApiGetCoreConfigString` output into `RC_Config` (`NextToken`,
`sscanf`, `scmp`). The binary packet is walked by `RC_InitDeviceCount` /
`RC_BuildConfig`:

- **Start:** fixed `packet + 0x200`. The header's
  `PhysicalDeviceOffset`/`LogicalDeviceOffset`/… are *not* consulted by the
  parser (the vendor driver still emits the conventional values shown above).
- **Bound:** the size argument. The live query path
  (`RC_Core::RC_GetConfig`, `0x5D1F0`) passes `0x10000`, not `PacketSize`.
- **Termination:** a zero `u32` at a record boundary, an unknown tag, or
  running past the bound.
- **Stride by tag:**

| Tag | Meaning | Advance |
|---|---|---|
| `0x25BC` | PhysicalDevice | `0x100` when flag 0, `0x80` when flag 1 |
| `0x25BD` | LogicalDevice | variable: `u32` length at record `+0x90` (`0x310` here) |
| `0x25BE` | ControllerDevice | `0x88` |
| `0x25BF` | SEPDevice | `0x40` |

The `0x80` variant is **not** what the live packet uses: `RC_GetConfig` calls
`RC_InitDeviceCount(packet, 0x10000, 0)` (flag 0 → `0x100`), and the captured
packet has tags at `0x200/0x300/0x400/0x500/0x600` then `0x25BD` at `0x700`.
Flag 1 is used only by `RC_GetPrevConfig` (previous generation) and
`RC_ApiGetCoreConfigStringFromConfigPacket` (in-memory import).

### Observed section contents (4-member RAID0 capture)

- `0x200`: five `0x25BC` records at `0x100` stride. The first (`0x200`) is a
  controller/version record (`DeviceType 0x1201`, zero capacity, version
  string); the other four are the Sabrent members.
- `0x700`: one `0x25BD` record, `PacketSize` `0x310`, `DeviceType 0x1BF6` at
  `+0x0C` — the same `RC_LogicalDevice` layout as the config-ring record
  documented above, including member count `+0x68`, `FirstCount/SecondCount
  +0x6C/+0x70`, `PacketSize +0x90`, `ChunkSize +0xAC`, `chunk_index +0x110`,
  and the element array at `+0x04`-relative offsets with a 64-byte element
  stride.
- `0xA10`: `0x25BE` controller records at `0x88` stride (six before the walk
  hits the zero tag at `0xD40`).
- The version string `9.3.3-00302` is **not** in the controller section: it
  sits at absolute `0x288`, i.e. in the first `0x25BC` record at `0x200`,
  offset `+0x88`.
- `0xD40`: no SEP records (`SEPDeviceSize 0`).

### Transport windows (settled 2026-11-09 from the CDB captures)

The device is a small mailbox, not a flat LBA space.  Every observed
non-INQUIRY access is `READ(10)`/`WRITE(10)` in 512-byte blocks:

| LBA | Access | Data | Meaning |
|---|---|---|---|
| `0x000` | `READ(10)`, 2 blocks (1 KiB) | `u32` next window, then `packet[4..1024]` | window handshake before each query |
| `0x0C0` | `WRITE(10)` 16 blocks, then `READ(10)` 128 blocks | 8 KiB request → 64 KiB reply | config window |
| `0x0D0` | same | byte-identical to the `0x0C0` reply | second window |
| `0x0E0` | same | byte-identical to the `0x0C0` reply | third window |

All three replies are identical (`pkt4/8/12.bin` compare equal), so the
LBA selects a request *channel*, not a distinct payload.  The 1 KiB read
at LBA 0 is a handshake, not an opaque blob: its first `u32` is the
window `rcadm` must use next (`0xC0`, then `0xD0`, then `0xE0`) and the
bytes behind it are the packet from its own offset 4.  A zero window
makes `rcadm` stop immediately with "no controllers, arrays, or disks
supplied".  The 8 KiB
request all three received is the same structure with the window LBA
echoed (`+0x00` command `1`, `+0x04` window LBA, `+0x18` `2`, `+0x1C`
`1`, `+0x20` `0x1000`; rest zero).  `rcadm --show` therefore does:
INQUIRY, descriptor read, then per window: write request → read 64 KiB.

### Size query: `RC_CMD_GET_CREATE_INFO` (settled 2026-11-13)

`rcadm -C -ms`, and the size a bare `-C` defaults to, do **not** come from
the config packet or from the PD free-space words.  `RC_ApiGetCreateInfo`
(`bcapi.cpp:1362`) builds an `RC_WpGetCreateInfo` and calls
`RC_Core::RC_ConfigRead(BufPtr, 8192, RC_CMD_GET_CREATE_INFO, 0)`
(`0x5C610`), which writes an 8 KiB window request and reads 8 KiB back from
the same window. The reply is parsed as an `RC_WpGetCreateInfo` **at offset
0**, not as a config packet.

Request (8 KiB, command word `+0x00` = 1):

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 4 | command 1 (window prepare) |
| `+0x04` | 4 | channel |
| `+0x08` | 8 | `GenerationNumber` |
| `+0x18` | 8 | major/minor (`2`/`1`) |
| `+0x20` | 4 | read command `0x100D` |
| `+0x24` | 172 | `RC_WpGetCreateInfo` (`RaidType +0x00`, `DeviceCount +0x04`, `FirstCount +0x28`, `SecondCount +0x2C`) |
| `+0xC0` | 16 × n | `DeviceList`: `DeviceID` u64, then handle `0x10000 \| slot` |

Reply: `Flags` `+0x14`, `SizeInMegabytes` `+0x18`, `FirstCount` `+0x28`,
`SecondCount` `+0x2C`.  Serving the config packet instead made
`SizeInMegabytes` read our `PacketSize` (`0x4000` = 16384 MiB), which the
CLI renders as `Maximum Possible Size: 17180 MB` (`×2^20/10^6`, rounded);
the neighbouring fields decoded just as literally as `PreferDeviceID`
`0x20000000000` (header `PhysicalDeviceOffset`), `FirstCount` 1280
(`PhysicalDeviceSize`) and `SecondCount` 1792 (`LogicalDeviceOffset`).

`src/rc_api.c` answers `0x100D` with that struct and computes
`SizeInMegabytes` from the selected members' free space (RAID 0 sums, 1 and
10 take the smallest member times the mirror count, 5/6 subtract parity).
Observed on four 4 TB members: 15,259,703 MiB for RAID 0, 7,629,851 MiB for
RAID 0 on two disks, 3,814,925 MiB for RAID 1; 0 MiB while the disks belong
to an assembled volume.

### Feature licensing: `CheckFeature` (settled 2026-11-13)

RAIDXpert2 sells part of its feature set as licensed. `rcadm` enforces that
itself, from the config packet, **before** it writes any mutation frame:

| Item | Value |
|---|---|
| Predicate | `CheckFeature(RC_Config*, u16)` at `0x41BC0`, worker `0x35230` |
| Short circuit | config's first word zero ⇒ predicate returns 0, "not licensed" |
| Key material | `RC_LICENSE_KEY_SIZE_IN_BYTES` 16 |
| Key location | `RC_CFE_LICENSE_KEY_ROM_START` `0x22000`, `RC_WILDCAT_LICENSE_KEY_ROM_START` `0x1c000` |
| Status codes | `RC_STS_NO_SW_LICENSE`, `RC_IOC_NOLICENSE` `0x00004000`, `RC_MSGID_ARRAY_LICENSE_VIOLATION` |

Gated features named in the binary: CACHING, DISTRIBUTED SPARE, SPANNING,
RAID1n, RAID10, RAID10n, RAID5, RAID6, RAID50, RAID60, CREATE COPY, SPLIT,
TRANSFORM and management operations. The controller list prints a `License`
column (`?BC-UNLICENSED` when absent), and the vendor core gates the same
levels on `RC_HW_OS_GetLicenseLevel`/`RC_CoreFeatureSet` (core table above).

What the gate actually reads matters for this driver, and was settled on
hardware (4 x 4 TB, MS-7E59, 2026-11-13):

- **It reads our packet, not the controller ROM.** `src/rc_api.c` emits
  `ControllerFeature` `0x01FF` and leaves `ControllerInfo` (`+0x14`, 16 B — the
  licence key) and `MaxArraySize` (`+0x58`) zero, so every gated feature
  resolves to unlicensed even on a board whose UEFI offers it. A shim that
  wants to report the real state has to surface the controller's licence
  level and key there; it must not fabricate one.
- **Severity varies by verb.** `-r5`/`-r6` are hard stops — no `CREATE` frame
  reaches the driver (no new `CREATE` line in dmesg, unlike RAID 1/10, which
  do reach it and return `-EOPNOTSUPP`). `-ca` (caching) prints the same
  message and still sends its frame.
- **The delete path is unaffected**: licensing is evaluated on create and
  transform, so a vendor-written array of any level can still be deleted
  through the CLI.

#### Platform caveat: X870E + Ryzen 9000

On AM5, RAID 5 is NVMe-only and CPU-gated, which is why a board spec sheet can
list RAID 5 while the same board's SATA ports cannot do it:

| Platform | NVMe RAID | SATA RAID |
|---|---|---|
| X870E + Ryzen 9000 | 0, 1, 5, 10 | 0, 1, 10 |
| X870E + Ryzen 8000/7000 | 0, 1, 10 | 0, 1, 10 |

MSI's MAG X870E TOMAHAWK MAX WIFI (`MS-7E59`) specification states RAID 5 is
"only supported by Ryzen 9000 series processors"; Gigabyte's X870E manuals say
the same. AMD's X870 driver download page still says RAID 5 is "supported on
3rd Gen AMD Ryzen Threadripper Processors", contradicting its own release
notes, which list X870E among supported chipsets — the UEFI RAIDXpert2 screen
is the practical ground truth, and only a Ryzen 9000 part unlocks it.
`rcadm` 9.3.3 on this platform exposes `-v`, `-ra`, `-r0`, `-r1`, `-r10` and
`-r5`, and no `-r6` verb, although the RAID6 licensing string is present.

The platform side is confirmed on this board: a **3-disk RAID 5 array was
created from the UEFI RAIDXpert2 utility and served normally** before this
work. Its metadata was later overwritten when the OS was installed onto a
RAID 0 array, so no vendor-written RAID 5 generation remains on these disks
for the parser to be tested against; parity write support is tracked in
upstream issue #68. Until such an array is recreated in the UEFI utility, a
`not licensed` answer from `rcadm` says nothing about the platform.

None of that unlocks a 5/6 create in this driver: `rc_volume_create_array()`
accepts only DeviceType `0x1BF6` with `SecondCount == 1` (RAID 0), and RAID 5/6
remain assemble-and-read-only. A vendor-created RAID 5 array from the UEFI
utility is the supported way to exercise that path.

### INQUIRY: bare string, no EVPD page header

EVPD page `0xD7` (`12 01 D7 00 14 00`) is answered with the 20 bytes
`"AMD-RAID Controller\0"` — no page-code/length prefix, unlike a
standard EVPD response.

The captured successful reply also carries `sg_io_hdr.status = 0x1`
(`masked_status = 0`).  That is the vendor driver's own convention, not
Linux sg's: `rcadm` treats `status == 0` as a failed command and stops
right after INQUIRY without reading the descriptor, so a shim must set
`status = 1` on success.

### PhysicalDevice record types (captured)

| `DeviceType` | Record | Notes |
|---|---|---|
| `0x1201` | controller/version record at `0x200` | capacity 0; version string in the **model** slot (`+0x88`) |
| `0x1200` | NVMe member | `Capacity +0x1C` equals namespace NSZE; `Devices +0x18` is 0 |
| state `0x3000` | online | seen on the controller record and on every member |

Firmware `+0x80`, model `+0x88`, serial `+0xB0` are the Identify
Controller FR/MN/SN strings, space-padded.

### ControllerDevice records (`0x25BE`, `0x88` stride)

Six records in the capture, matching rcadm's controller list.  Layout
from the DWARF type `RC_ControllerDevice` (`rc_config.h`, 136 bytes —
the `0x88` stride), confirmed against all six captured records:

| Offset | Size | Field | Capture |
|---|---|---|---|
| `+0x00` | 4 | `StructureType` | `0x25BE` |
| `+0x04` | 16 | `ControllerID` (`RC_DeviceIdentifier_LE`) | u64 id at `+0x04`, handle at `+0x0C` |
| `+0x14` | 16 | `ControllerInfo` | zero |
| `+0x24` | 16 | `ControllerName` | `"NVMe ATAPIPort "` / `"AMD-RAID …"` |
| `+0x34` | 2 | `ControllerFeature` | `0x01FF` |
| `+0x36` | 2 | `CablePresent` | 0 |
| `+0x38` | 32 | `BiosBuildNumber` | zero |
| `+0x58` | 8 | `MaxArraySize` | `0x001FFFFFFFFFFFFF` |
| `+0x60` | 8 | `SerialNumber` | 0 |
| `+0x68` | 4 | `TotalPorts` | `1` NVMe, `6` AMD-RAID |
| `+0x6C` | 2 | `VendorID` | `0x1022` |
| `+0x6E` | 2 | `DeviceID` | `0xB000` |
| `+0x70` | 4 | `ControllerNumber` | record ordinal |
| `+0x74` | 2 | `SubVendorID` | `0x1987` |
| `+0x76` | 2 | `SubDeviceID` | `0x5012` |
| `+0x78` | 8 | `ID` | same as `ControllerID` id |
| `+0x80` | 2 | `OrigVendorID` | `0x1022` |
| `+0x82` | 2 | `OrigDeviceID` | `0xB000` |
| `+0x84` | 4 | `PCILocation` | `0x00160000`, `0x00150000`, … |

`ControllerID`'s u64 is the key rcadm matches on:
`RC_Core::RC_GetControllerDeviceIndex` walks the controller array
comparing it to a device's `DeviceID` and treats 0 as "not found".  In
the capture it is `0x00001022B0000000 | ordinal` for the NVMe members
and `0x0000102243BD0003` for the two AMD-RAID records.  `ControllerName`
packs two NUL-terminated strings into 16 bytes; rcadm prints the first.
An earlier partial reading put those strings at `+0x34`/`+0x39` — that
was `ControllerFeature` plus the tail of `ControllerName`, corrected
here from the type definition.

`RC_C_ControllerDevice` (`RC_C_ControllerDevice.h`, 152 bytes) mirrors
the record behind a vptr; its constructor copies `ControllerID`,
`ControllerInfo[16]`, `ControllerName[16]`, `BiosBuildNumber[32]`,
`MaxArraySize`, `TotalPorts`, the PCI ids, `ID`, `PCILocation`,
`SerialNumber` and `ControllerNumber`.

`PCILocation` is `bus << 16 | device << 8 | function`: the six capture
values (`0x030000`, `0x060000`, `0x0A0000`, `0x110000`, `0x150000`,
`0x160000`) match this box's `lspci` — RAID bottom devices at `03:00.0`,
`11:00.0`, `15:00.0`, `16:00.0` and the `43bd` controllers at `06:00.0`,
`0a:00.0`.  The shim emits one record per member from the member's live
`pci_dev`, and rcadm renders the section as *Number / Type / License Key
/ Port Count / PCI ids / Bus-Slot-Function / SAS Address / BIOS
Version*.

### SEPDevice records (`0x25BF`, `0x40` stride)

Firmware sends `SEPSize = 0` in the capture, so there is no emitted
record to correlate with a screen column.  The DWARF type
`RC_SEPDevice` (`rc_config.h`, 64 bytes) gives the layout the parser
expects:

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 4 | `StructureType` (`0x25BF`) |
| `+0x04` | 16 | `ID` (`RC_DeviceIdentifier_LE`) |
| `+0x14` | 8 | `VendorID` |
| `+0x1C` | 16 | `ProductID` |
| `+0x2C` | 4 | `Revision` |
| `+0x30` | 8 | `SerialNumber` |
| `+0x38` | 8 | `Reserved` |

`RC_C_SEPDevice` (168 bytes) surfaces the same data as `Name[80]`,
`VendorID[12]`, `ProductID[20]`, `Revision[8]`, `SerialNumber[12]`.
Since the firmware itself sends none, the shim keeps `SEPSize = 0`
rather than inventing enclosure data.

### Implementation status (`src/rc_api.c`, 2026-11-09)

`src/rc_api.c` serves the ABI above: INQUIRY page `0xD7`, the 1 KiB
window handshake (rotating `0xC0`/`0xD0`/`0xE0`, packet behind it), and
a 64 KiB
packet built from the live member registry plus the verbatim
`RC_LogicalDevice` record parsed off disk.  Emitted sections: header,
`PD` (controller/version record + one per member), `LD` (byte copy, so
the Id and element table are the firmware's).  `CTRL` carries one `0x25BE` record per member (real BDF from the member's `pci_dev`); `SEP` stays empty because firmware itself sends `SEPSize = 0`.
Mailbox command 1 is accepted on write: the window prepare, and the
`GET_CREATE_INFO` read command (`0x100D`), answered with an
`RC_WpGetCreateInfo`.  Mutation frames (`CREATE` 2, `DELETE` 3,
`TRANSFORM` 8) are decoded by `src/rc_cfgwrite.h`, field for field.  DELETE
and CREATE execute against the selected members (`rc_volume_delete_array()`,
`rc_volume_create_array()`); the CREATE disk list comes from the preceding
`GET_CREATE_INFO` because the 172-byte payload names only one device. TRANSFORM is refused with
`CHECK CONDITION`/ILLEGAL REQUEST because it has no writer, and a success
reply for a change that is not on disk would leave rcadm believing in a
volume the next reboot exposes.  The log names the level, geometry, member
count, size and first member, so a failing verb is diagnosable without
re-running it under a capture shim.  CREATE persists and assembles correctly
but still leaves rcadm printing its own generic error; the mutation
completion reply is the open gap (issue #16).

*Hardware matrix 2026-11-13 (4× 4 TB NVMe, X870E):* RAID 0 over 4 disks
(31,251,871,744 sectors, 16 TB), RAID 0 over 2 of the 4 (15,625,934,848
sectors, 8 TB), RAID 1 over 2 (7,812,966,400 sectors, 4 TB) and RAID 10 over
4 (15,625,934,848 sectors, 8 TB) all assemble at the requested size, report
`optimal`, serve reads and delete cleanly; no member reports an untrusted
geometry. RAID 0/1/10 share the `0x1BF6` volume DeviceType with `(N,1)`,
`(1,2)` and `(N,2)`.

### PhysicalDevice record fields read by `RC_C_PhysicalDevice`

From the constructor (`0x5E550`, `RC_C_PhysicalDevice.cpp:63`), on the flag-0
(256-byte) record:

| Record offset | Size | Used as |
|---|---|---|
| `+0x04` | 8 | `DeviceID` (LE) |
| `+0x0C` / `+0x0E` | 2 / 2 | handle path/target |
| `+0x10` | 4 | `DeviceID` continuation |
| `+0x18` | 4 | `Devices` |
| `+0x1C` | 8 | `Capacity` (sectors) |
| `+0x24` | 4 | `DeviceType` |
| `+0x2C` | 4 | `DeviceState` |
| `+0x30` | 4 | `SpareInfo` |
| `+0x54` | 4 | `Features` |
| `+0x58` | 4 | `PortType` |
| `+0x80` | 8 | firmware revision (`RKT30Q.3`) |

The u32 at `+0x0C` doubles as the disk number: `0xFFFF` on the
controller record and `0x10000 | slot` on members (records 1-4 carry
`0x10000`-`0x10003`).  rcadm's *Disk* column follows it — emitting 0
there made all four members print as "Disk 0" even though their
DeviceIDs, models and serials were distinct.
| `+0x88` | 32 | model name (`Sabrent Rocket Q`) |
| `+0xA8` | 8 | serial-number prefix (8 spaces + first 8 hex digits) |
| `+0xB0` | 24 | serial number |
| `+0xC8` / `+0xD0` | 8 / 8 | largest/total free space (flag 0 only) |

This confirms the PD records are the full 256-byte form, not the 128-byte
previous-generation variant.

---

## Vendor kernel-side config-write chain (9.3.3-00302)

The packet above is only the userspace half of the ABI. The blob's kernel
side is fully symbolized, so the mutation path can be specified from DWARF
and disassembly instead of inferred from captures. Facts below are layouts
and observed control flow only; no vendor code is copied, and none of this
was executed against the vendor driver.

### Chain (`nm` addresses)

| Symbol | Address |
|---|---|
| `RC_ConfigWrite(RC_CoreIoc*)` | `0x67ED0` |
| `RC_BuildConfigWrite(RC_CoreIoc*)` | `0x67D50` |
| `RC_ConfigWriteNoLock(RC_CoreIoc*)` | `0x681E0` |
| `RC_ConfigWriteLower(RC_CoreIoc*)` | `0x5C1C0` |
| `RC_ConfigWriteDiskMetaData(RC_CoreIoc*)` | `0x5D810` |
| `RC_WriteMemCommitConfigPacket(RC_ConfigCommit*)` | `0x3D560` |
| `RC_CheckConfigCommit(RC_ConfigCommit*)` | `0x936E0` |
| `RC_GetConfigWriteLock(RC_CoreIoc*, u32)` | `0x806E0` |
| `RC_ReleaseConfigWriteLock(RC_CoreIoc*, u32)` | `0x807D0` |

`RC_ConfigWrite` allocates a `0x4000`-byte packet buffer (IOC `+0x428`) and
copies the caller's 1 KiB request (IOC `+0x60`) into it, then parks an async
state machine on IOC `+0x1D0` (states `0x2`, `0x3`, `0xF9`, `0x103`; the
previous state is saved at `+0x2B8` and the value at `+0x28` moved to `+0x1D8`).
`RC_BuildConfigWrite` makes its own `0x400`-byte copy of the same request,
stores it at `+0x428` and switches to state `0x2` (queued). The commit is a
separate record: `RC_WriteMemCommitConfigPacket` writes it,
`RC_CheckConfigCommit` is the matching check entry point, and the config
update path validates the commit checksum (below). All writers serialize on
`ConfigLockCode`/`ConfigLockLine` (IOC `+0x2EC`/`+0x2F0`).

### Request: `RC_CfgWrite` (1024 B)

The kernel-side request format; a replacement write path has to supply these
fields even though the userspace half only ever sends a packet.

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | `Command` (`RC_ConfigWriteCommand`) |
| `0x04` | 4 | `ChannelID` |
| `0x08` | 8 | `GenerationNumber` |
| `0x10` | 4 | `SyncCommand` |
| `0x14` | 4 | `ConfigWriteSizeInBytes` |
| `0x18` | 4 | `MajorVersion` |
| `0x1C` | 4 | `MinorVersion` |
| `0x20` | 992 | command payload union / `ConfigWriteSizeReservedByte[992]` |

Command values that map to `rcadm` verbs: `CREATE` 2, `DELETE` 3, `MANAGE` 7,
`TRANSFORM` 8, `SPLIT` 14, `ACCEPT_CONFIG_CHANGES` 15, `RESCAN` 16,
`OFFLINE` 17, `MANAGE_PHYSICAL_DEVICE` 29, `CONTROL_TASK` 34, `COPY` 35,
`COMMIT_PREVIOUS_CONFIG` 36, `WRITE_METADATA` 44.

Payload sizes (`pahole`): create 172 (`RC_WpGetCreateInfo`), transform 140,
manage 76, copy 56, split 52, swap 32, write-metadata 32, task-control 28,
delete 24, offline 20, commit-previous 4. `RC_CoreCreateLogicalDevice` is the
344-byte in-kernel form of create: `ChannelID` 0, `RaidType` 4, `DeviceCount`
8, `SpareDeviceCount` 12, `CacheTagSize` 16, `CacheFlags` 20, `Flags` 24,
`SizeInMegabytes` 32, `PreferDeviceID` 40, `FirstCount` 48, `SecondCount` 52,
`DistributedSpare` 56, `PreferReadElement` 60, `TaskPriority` 64,
`DevicePrivate[32]` 68, `DeviceID` 104, `SpareDeviceList[8]` 128,
`DeviceList[1]` 320.

### Captured mutation frames (rcadm 9.3.3-00302)

Observed by running `rcadm` under an `LD_PRELOAD` shim that logs
`ioctl(fd, SG_IO)` and completes a `WRITE(10)` carrying a mutation
`Command` without issuing it; reads pass through unchanged, so no array was
mutated. The only array on the box is the live RAID 0 system array: the
delete/transform runs used it with the write dropped, and create needed a
synthetic config view because the maximum size is computed by the vendor
driver from its own config, not by userspace. The shim reported the member
disks free and forced the single `GET_CREATE_INFO` reply to success; the
resulting `CREATE` frame was captured and dropped like the others.

Every offset below matched the DWARF layout exactly.

Request header as sent:

| Field | Observed |
|---|---|
| `Command` (`0x00`) | 2, 3 or 8; payload union at `0x20` |
| `ChannelID` (`0x04`) | the SG_IO LBA of the frame (`0xdb0`, `0xfd0`, ...), **not** a controller channel; the response is read back from the same LBA |
| `GenerationNumber` (`0x08`) | 13, the live config header generation |
| `SyncCommand` (`0x10`) | 0 for create/transform, **1 for delete** |
| `ConfigWriteSizeInBytes` (`0x14`) | **0 for every mutation** |
| `MajorVersion`/`MinorVersion` | 2/1 |

A mutation frame is 1536 bytes on the wire (1 KiB request plus 512); the
8 KiB `WRITE_PART_OF_READ` frames are the read handshake, not requests.
Since `ConfigWriteSizeInBytes` is always zero, a receiver has to size the
payload from `Command` (172/24/140 B, ...), never from the frame length.

`CREATE` payload (`RC_WpGetCreateInfo`, 172 B at `0x20`): `RaidType` 0,
`DeviceCount` 4, `SpareDeviceCount` 8, `CacheTagSize` 0xC, `CacheFlags`
0x10, `Flags` 0x14, `SizeInMegabytes` 0x18, `PreferDeviceID` 0x20,
`FirstCount` 0x28, `SecondCount` 0x2C, `DistributedSpare` 0x30,
`PreferReadElement` 0x34, `TaskPriority` 0x38, `DevicePrivate[32]` 0x3C,
`SpareDeviceList[4]` 0x5C, `DeviceList[1]` 0x9C. Defaults: `CacheTagSize`
3 (256 KiB) and `CacheFlags` `0xC0000000` (read+write-back).

The request does **not** distinguish RAID 0/1/10 by `RaidType`; the
geometry is in `FirstCount`/`SecondCount`:

| rcadm verb | `RaidType` | `DeviceCount` | `FirstCount` | `SecondCount` |
|---|---|---|---|---|
| RAID 0 | `0x1BF6` | N | N | 1 |
| RAID 1 | `0x1BF6` | 2 | 1 | 2 |
| RAID 10 | `0x1BF6` | 4 | 2 | 2 |
| RAID 5 | `0x1BF5` | N | 1 | N |
| volume (JBOD) | `0x1BF7` | 1 | 1 | 1 |

Size units differ between the CLI and the payload: `-s` is decimal MB
(10^6 B) while `SizeInMegabytes` is MiB (2^20 B). `-s 1024` produced 977
(1024e6 / 2^20, rounded); `-T ... -ss` on the ~16 TB array produced
15,259,608. `PreferDeviceID` is the **logical-device Id**:
`RC_ApiCreateLogicalDevice` writes its u64 argument straight into the
payload at `+0x20` and, after the config write returns, calls
`RC_FindLogicalHandle(core, that_id)` to pick the new array back up. A
writer that stamps an Id of its own makes that lookup fail and rcadm exits 1
with "error creating array" even though the array assembles — observed on
2026-11-13 and fixed in `rc_volume_create_array()`. The earlier captures
showed stack garbage in the field only because the forced capture path never
set it.

`DELETE` payload (`RC_CW_DeleteLogicalDevice`, 24 B at `0x20`): `All` 0,
`DeviceCount` 1, `DeviceList[0]` the 16-byte logical-device identifier
whose first 8 bytes are the array `Id` (`4e1f33065b470000` =
`0x0000475b06331f4e`), followed by `01000000 00000000`.

`TRANSFORM` payload (`RC_WpGetTransformInfo`, 140 B at `0x20`): `RaidType`
0, `LogicalDevice` 4 (16 B), `DeviceCount` 0x14, `SpareDeviceCount` 0x18,
`Flags` 0x1C, `SizeInMegabytes` 0x20, `FirstCount` 0x28, `SecondCount`
0x2C, `DistributedSpare` 0x30, `PreferReadElement` 0x34, `TaskPriority`
0x38, `SpareDeviceList[4]` 0x3C, `DeviceList[1]` 0x7C. Observed for
`-T -r5 -a 1 -d 0 1 2 -ss`: `RaidType` `0x1BF5`, `DeviceCount` 3,
`FirstCount` 1, `SecondCount` 3, `SizeInMegabytes` 15,259,608, and
`DeviceList` the three member device IDs.

### Commit block: `RC_ConfigCommit` (512 B)

| Offset | Size | Field |
|---|---|---|
| `0x00` | 8 | `Checksum` |
| `0x08` | 4 | `ConfigPacketOffset` |
| `0x0C` | 4 | `PacketSize` |
| `0x10` | 8 | `CommittedChecksum` |
| `0x18` | 8 | `GenerationNumber` |
| `0x20` | 8 | `LastModifierSerialNumber` |
| `0x28` | 8 | `LastOnlineSerialNumber` |
| `0x30` | 8 | `PacketNumber` |
| `0x38` | 4 | `CommittedBy` |
| `0x3C` | 452 | `Reserved[113]` |

`Checksum` at `+0x00` covers `[0x08, 0x200)`, confirmed against all four X399
fixtures (2026-11-13; see the checksum section below) — the encoders in
`src/rc_meta_build.h` and `mkmeta.py` compute it. The driver treats `+0x10`
as the committed generation's timestamp and requires it to equal the
generation header's `+0x00` word; that equality is the two-phase-commit
linkage it validates (`RC_GEN_HEADER_TS_OFFSET`), independent of the
`CommittedChecksum` field name above.

### On-disk member record: `RC_PhysicalDeviceOnDisk` (128 B, packed)

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | `StructureType` |
| `0x04` | 16 | `DeviceID` |
| `0x14` | 4 | `PreviousHandle` |
| `0x18` | 4 | `Devices` |
| `0x1C` | 8 | `Capacity` |
| `0x24` | 4 | `DeviceType` |
| `0x28` | 4 | `BusType` |
| `0x2C` | 4 | `DeviceState` |
| `0x30` | 4 | `SpareInfo` |
| `0x34` | 16 | `StartMetaDataSpace` |
| `0x44` | 16 | `EndMetaDataSpace` |
| `0x54` | 4 | `Features` |
| `0x58` | 4 | `PortType` |
| `0x5C` | 36 | `Reserved[9]` |

### Unlike `rcadm`, the kernel validates the commit

`rcadm` ignores the packet checksum (see above). The blob does not: it
contains `RC_UpdateCorePhysicalDevice: *** INVALID *** Commit config
checksum = 0x%llx`, `CommittedChecksum = 0x%llx`, `*** INVALID *** Config
packet offset = 0x%x`, `*** INVALID *** Config packet size = 0x%x`, plus the
status codes `RC_STS_INVALID_GENERATION_NUMBER` and
`RC_STS_ACCEPT_CONFIG_CHANGE_PENDING`.

Consequences for Phase 2, and for anything that claims to replace `rcadm`:

- A config write is a generation-guarded transaction. The request carries
  `GenerationNumber`, the commit record carries its own, and a
  `WaitForAcceptConfigChange` state exists between write and accept.
- Persisting means writing the 16 KiB packet and a 512-byte commit block with
  correct `Checksum`/`CommittedChecksum`; a shim may not report success for a
  mutation it did not persist.
- Member stamps use the 128-byte packed `RC_PhysicalDeviceOnDisk` record, not
  the 256-byte userspace PD variant the read path emits.

#### Checksum algorithm (`RC_GenerateChecksum`)

`RC_CheckMetaData` (0x93200) and the config-commit check both funnel into
`RC_GenerateChecksum(void *, unsigned int)` (0x931b0) and its callee
`RCS_GenerateChecksum(void *, const u64 &, u64 *)` (0xb2b0). It is not a CRC:
it is an order-dependent XOR of the 64-bit words, with a data-dependent swap
of two 16-bit halves inside each word before the XOR.

```
acc = 0
for i in 0 .. len/8 - 1:
    q = word[i]                       /* little-endian u64, memory order */
    a = acc & 3
    b = q & 3
    if a == b:                        /* same half-word selected twice */
        a = (i + 1) & 3
        b = i & 3
    swap the 16-bit half-words a and b of q
    acc ^= q                          /* also stored out on every step */
```

`len` is consumed as `len / 8` words (a trailing remainder is ignored); the
word count is 16-bit, so a single call covers at most 512 KiB, which the 0x1F8
(`RC_MetaData`) and packet-sized (`RC_ConfigCommit`) inputs both fit. Which
byte range each caller passes over is caller-specific; for the two blocks seen
on disk it is now established (below).

Corroborated on vendor-written bytes (2026-11-13): `RC_MetaData` (sector
`0x5000`) and `RC_ConfigCommit` (sector `0x5001`) both keep `Checksum` at
`+0x00` over `[0x08, 0x200)`, and this algorithm reproduces each stored value
on all four members of the X399 fixture set
([`PlaidPiper/x399-raidcore-fixtures`](https://github.com/PlaidPiper/x399-raidcore-fixtures),
CC0). The commit checksum is therefore no longer an open question. Re-run the
check with `scripts/qemu-test/verify-checksums.py member*.meta.bin`.

Second corroboration, on NVMe (2026-11-13): all four members of this box's live
4-member RAID 0 reproduce both stored values the same way, and the committed
generation was then walked byte by byte. That settles the two items the roadmap
listed as unestablished, and corrects one of them:

- **The generation has no checksum.** Its `+0x00` is the generation timestamp
  (`ce92a7827c5432da` on this array), equal to the commit block's `+0x10`, and
  the records start at `+0x200`. The `Checksum` word in the rcadm-facing
  packet header belongs to that runtime view only (see "Checksum: not
  validated by `rcadm`"); a writer persisting a generation does not compute
  it.
- **The 128-byte member record is inside the generation**, not a separate
  stamp: tag `0x25BC`, 128-byte stride, written before the LD records. The
  observed order is `6 × 0x25BC` at `+0x200`, then `0x25BD` records (array
  first, size from `+0x90`), then `6 × 0x25BE` at `0x88` stride.

### On-disk generation layout (observed 2026-11-13)

`0x25BC` records, 128-byte stride, all `DeviceState 0x3000`, all
`PreviousHandle 0`, all `StartMetaDataSpace`/`EndMetaDataSpace` zero:

| Slot | `DeviceType` | `DeviceID` | `Capacity` | `Features` |
|---|---|---|---|---|
| 0 | `0x1201` controller/version | `0x0000102285900000` | 0 | 0 |
| 1-3, 5 | `0x1200` NVMe member | each member's `RC_MetaData` DeviceId | 7813988352 (whole disk) | `0x22af001c` |
| 4 | `0x1203` SATA disk | `0x17bb137f72a02f21` | 1953525168 | `0x21ed0018` |

Then the LD records: the array first (`0x1BF6`, `PacketSize 0x310`, 4 elements
at `alloc_off` 1048576 / `user_off` 1069056), and one `0x1BF9` single-disk
record for the 1 TB disk that is not a member. Note there is **no** `0x1BF9`
record for the NVMe members of this array, contrary to the earlier reading
that firmware publishes one raw-disk LD per physical member; here the members
are consumed by the array and only the non-member disk gets a raw record.
Then `6 × 0x25BE` controller records: four `1022:b000` NVMe ports and two
`1022:43bd` AHCI controllers (6 ports each), `PCILocation` `0x170000`,
`0x150000`, `0x110000`, `0x0a0000`, `0x060000`, `0x030000`.

The walk ends with zero padding to a 512-byte boundary (`0xE00` total here).
Reproduce it with `scripts/qemu-test/inspect-generation.py member*.meta.bin`.
The encoders in this tree do not yet emit the PD or CTRL sections.

Still unestablished: which range the 16 KiB config packet's own checksum
covers, and the 128-byte `RC_PhysicalDeviceOnDisk` member stamp — neither
record appears in these dumps.

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

### RAID 5/6 layout — dedicated parity, EVENODD Q (settled 2026-11-08)

7. **Vendor parity placement: fixed slot, no rotation.** Derived from the
   DWARF-bearing `rcblob.x86_64` (SDK 9.3.0-00283), so every listing below
   carries AMD's own file:line rather than a guessed function name.

   - `RC_RAID5DriveMap` (`rc_cache.cpp:5500-5506`) returns
     `(lba >> chunk_pow) % (Devices - 1)` — the **data** member. Data chunks
     therefore map to members `0 .. N-2` and never to `N-1`.
   - `RC_RAID6DriveMap` (`rc_cache.cpp:5510-5515`) is the same shape with
     `% (Devices - 2)`, leaving `N-2` and `N-1` for P and Q.
   - The map result is the drive index for the Ioc: call sites write it to
     `IocList.FailedDrive` (`ioc+0x438`) in the generic read paths
     (`rc_generic_io.cpp:4724-4726`, `:5063-5066`, `:5770-5772`). Since it can
     only be `0 .. N-2`, member `N-1` never receives data.
   - `RC_FlushDirtyDataRAID5DoneGetParityLock` (`rc_raid5_io.cpp:6793-6799`)
     sets `IocList.ChildrenTotalIo = Devices - 1` — one child per data drive —
     derives the parity count as `(DeviceType == 0x1BFA) + 1`, and for RAID 6
     separately locks the Ioc at index `Devices - 1`. No stripe or row term
     appears anywhere in it.
   - `RC_GetStripeStartSector` (`rc_driver_map.cpp:2095-2115`) strides rows by
     `(Devices - parity_count) × chunk`, i.e. dense rows: `N-1` data chunks
     plus one parity chunk, all at the same row offset on their member.
   - `CacheTagSectors` (`rc_core_config.h:1007-1023`) decodes chunk index
     `3/2/other` as `0x200/0x100/0x80` sectors = 256/128/64 KiB.

   So the vendor layout is **dedicated parity on member `N-1`**, rows packed
   at `row * chunk`. It is not the left-symmetric rotating map this port used
   to implement (`parity_pos = (N-1) - (row % N)`), which disagreed with the
   vendor on two rows out of three for `N=3`; that map was replaced on
   2026-11-08, so `patch_parity_math.c` and the vendor now agree. The
   element table is not consulted for placement after all — the map is pure
   arithmetic.

   The one assumption left in that chain is that the returned data index is
   used against the member Ioc array without a per-row permutation. Nothing in
   the read, parity-lock or parity-memory paths applies a row term, and   the decompile behind issue #6 saw the parity Ioc selected as
   `pIocList[SecondCount - k]`, so a hidden rotation is unlikely — but that is
   an inference from absence, which is why it stays listed here. Reading a
   vendor-written array back is the only thing that would remove it.

   Cross-checked against `rcraid.sys` 9.3.3-00291: the Windows build carries
   the same RAID 5 family (`RC_FlushDirtyDataRAID5NonStripeWrite*` asserts,
   `RC_CreateRaidArray: Error, raid5 set greater than %d elements`), so the
   design is shared, but the map arithmetic is only readable in the DWARF
   blob.

   Consequence: `patch_parity_math.c` now implements this layout (2026-11-08),
   replacing the row-rotating map, and parity volumes remain **read-only** —
   for want of a write path, not because the layout is in doubt.

   **Where that leaves the claim.** The placement is settled against the
   vendor implementation: it is read out of AMD's code, which is the code that
   writes these arrays, and both shipped builds agree. Two things are
   unobserved rather than unsettled. First, no array written by the vendor has
   been read back through this driver — the dev array was zeroed, and the
   staged fixture is written by this driver itself (re-stamped under the
   dedicated layout on 2026-11-08, and it round-trips byte-identically), so
   neither can discriminate between candidate layouts (issue #6). Second, member order comes from the LD element array, so a
   permutation there would misroute reads without contradicting any of the
   arithmetic above. Neither is evidence against the layout; both are reasons
   the claim should say "settled against the vendor code" and not
   "hardware-proven".

   Earlier reasoning worth keeping: a parity level is selected by `DeviceType`
   alone — `FirstCount × SecondCount = Devices` holds on every real record and
   cannot express `(N-1) × 1` or `(N-2) × 2`, so the counts never identify a
   parity array (issue #3).
8. **RAID 6 Q is EVENODD, not GF(2⁸) Reed-Solomon — settled 2026-11-08.** The
   whole dual-parity path lives in one translation unit, and AMD named it
   `rc_raid6_evenodd.cpp`. There is no Galois arithmetic in it: no multiply, no
   log/exp table, no carry reduction. Every RAID 6 routine is XOR plus an
   integer modulus by 17.

   - `RC_RAID6_ComputeSingleQ` (`rc_raid6_evenodd.cpp:157`, `.text:0x8f70`) is
     the core. For Q word `col` it XORs one 32-byte word from each data
     member, taking word `RC_RAID6_AbsoluteModulus(col - row, 17)` of member
     `row` and **skipping that member entirely when the result is 16**
     (`cmp $0xf / ja`). The `mov $0x11,%esi` before the call is the prime.
   - `RC_RAID6_AbsoluteModulus` (`:124`) is `idiv %esi` with a sign fixup — a
     real integer modulus, not a field operation.
   - `RC_RAID6_ComputeQ` (`:182`) zeroes 512 bytes of Q, builds the `S`
     correction term with `RC_RAID6_ComputeS` (`:144`), then runs
     `ComputeSingleQ` for 16 words, each seeded with `S`. Because the encoder
     skips one diagonal per column, that diagonal has to be carried
     separately, and `S` is it.
   - `RC_RAID6_ComputeS` XORs the **last** 32-byte word of members
     `1 .. n-1`, stepping back one word per member (`0x3c, 0x38, 0x34 …` in
     `r9d`). Starting at the last word and walking one word per member means
     it is written for **at most 16 data members** — a 17th would step to a
     negative offset.
   - `RC_RAID6_EvenOddEncode` (`:198`) is `RC_RAID6_Compute_XOR` (`:98`, plain
     P) followed by `RC_RAID6_ComputeQ`. `RC_RAID6_GenerateNewQForColZero`
     (`:619`) XORs 512 bytes of two buffers into Q.
   - `RC_RAID6_ComputeQSyndromes` (`:480`) repeats the same construction over
     all 17 diagonal columns for recovery: modulus 17, XOR only,
     `cmp $0x11` on the outer index.
   - The rest of the family says the same thing: `EvenOddDecode` (`:547`),
     `EvenOddDataDataFailed`, `EvenOddDataParityFailed`,
     `PartialWrite_EvenOddEncode` (`:714`), `ComputeQPrime` (`:666`),
     `ComputeOldS`, `GenerateNewS`.

   This is live code, not a leftover translation unit:
   `RC_STH_MidInitialize` (`rc_sth_init.cpp:815`) installs
   `RC_STH_MemRAID6Code` into the soft-RAID handler table, and that function
   (`rc_sth_memory.cpp:532`, `.text:0x3e80`) dispatches to `EvenOddDecode`,
   `PartialWrite_EvenOddEncode` or `EvenOddEncode` depending on the Ioc's
   degraded / partial-write flags. It also wraps a running index at
   `cmp $0x11` — 17 again. `RC_MemRAID6EncodeList` / `DecodeList` on top of it
   only package an external call, so there is no second, table-driven
   implementation behind it.

   Independent check: scanning the whole binary for a GF(2⁸) exp or log table
   under each of the three usual polynomials (`0x11D`, `0x11B`, `0x187`) finds
   none. A table-driven Reed-Solomon Q would have to ship one.

   Geometry this implies: the unit is **512 bytes per member**, split into
   sixteen 32-byte words, with seventeen diagonal positions of which one is
   skipped and carried in `S`. That is the word-oriented EVENODD variant known
   as RDP (`p = 17`, `p-1 = 16` rows) under AMD's own `evenodd` naming.
   **None of it changes the read map** — placement is still
   `dedicated parity on member N-1`, and a healthy array never reads parity.
   It matters for reconstruction, for a parity verify pass, and for any write
   path.

   Consequence for this port: `rc_amd_generate_raid6_parity()` was GF(2⁸)
   Reed-Solomon and therefore **not** the vendor's Q. That has been replaced
   by this construction (issue #10): the arithmetic lives in
   `src/raid6_evenodd.h`, which the userspace harness compiles as well, and
   the GF(2⁸) tables are gone from `src/`. It still has no callers — RAID 6 is
   read-only and healthy reads never touch parity — so no Q is written yet;
   the first consumer is a P/Q write path or a Q-based rebuild, and agreement
   with firmware-written Q remains unobserved.

   One reading the trace leaves open, settled here: member `d` contributes
   word `16 - d` to `S`, not `15 - d`. The alternative is singular —
   two-failure recovery fails for nearly every member pair — whereas `16 - d`
   is full rank for all 120 pairs across 2..16 data members, which
   `src/test_parity_userspace.c` asserts directly.

   Cross-check against `rcraid.sys` 9.3.3-00291 (`V:\RC-933\RC_933_00291`):
   the Windows image is symbol-stripped, but the code is there and it is the
   same algorithm. MSVC compiles the `AbsoluteModulus(…, 17)` call into a
   reciprocal multiply — `mov $0x78787879` / `imul` / `sar $3` /
   `imul $0x11` / `sub` — followed by the identical `if (x < 0) x += 17`
   fixup, then `cmp $0x10 / jae <skip>` (diagonal 16 dropped), a `shl $0x5`
   word stride and four qword XORs. Nine such sites sit in one code block
   around `0x140071d20`, with an outer loop that terminates on `cmp $0x10`
   after 17 iterations and an `S` buffer at struct offset `+0x190`. The
   matching also explains why the naive search failed: MSVC never emits
   `idiv` for a constant divisor, so there is no modulus-17 `div` to find.
   No GF(2⁸) exp or log table exists in that binary either (`0x11D` and
   `0x187` both absent). Both shipped builds carry EVENODD Q.

   Caveat on that cross-check: the identification is by instruction
   signature, not by symbol, since the image is stripped — but the signature
   is the `S`/mod-17/32-byte layout, which nothing else in the driver uses.

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
