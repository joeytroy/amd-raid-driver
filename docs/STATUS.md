# Implementation Status

Current focus: **PCI 1022:B000 only** (TRX50 NVMe RAID Bottom). The AHCI
variants (`7905 / 7916 / 7917 / 43BD`) are not on the target hardware and
are best-effort/untested.

## Where we are

### Done — NVMe controller and admin path

- **PCI-ID-based code path selection** (`rc_firmware.c`). DEV_B000 →
  NVMe, AHCI variants → AHCI, anything else → stub.
- **NVMe controller boot** (`rc_nvme.c`, `rc_nvme_init_controller`).
  Reads `CAP`, disables, allocates 4 KiB-aligned admin SQ/CQ, programs
  `AQA/ASQ/ACQ/CC`, waits for `CSTS.RDY`. `MQES` is decoded as `u32` so
  the controller's `0xffff` max-queue value doesn't overflow.
- **AHCI register programming gated** on `ctrl_mode == AHCI`.
- **Doorbell helpers** for both admin and I/O queues, at
  `BAR0 + 0x1000 + (2 * qid | 2 * qid + 1) * (4 << DSTRD)`.
- **Admin command framework** (`rc_nvme_admin_cmd`). Builds an SQE,
  rings doorbell, polls CQ, advances head, returns SC/SCT.
- **Identify Controller** (CNS=`0x01`). Logs `VID/SSVID/SN/MN/FR/NN`.
  Confirms the AMD RAID Bottom is transparent at the NVMe admin layer
  on this hardware — the Crucial drives answer directly.
- **Identify Namespace** (CNS=`0x00`, NSID=1). Logs `NSZE/NCAP/NUSE`
  and the active LBA format; stashes the LBA size into adapter state.
- **I/O queue pair (qid=1)** via Create I/O CQ + Create I/O SQ admin
  commands. Polled completion, depth=64.
- **First NVMe READ** through the I/O queue — end-to-end SQE → doorbell
  → poll → CQE → status check → CQ-head advance → CQ doorbell.

### Done — RAID0 assembly

- **RAIDCore metadata** at LBA 0x5000 of each member.
  - Magic `RAIDCore` = `0x65726F4344494152` LE at offset 0x08.
  - Version `0x00030000` at offset 0x2C.
  - Stripe size at offset 0x28 (= 2048 sectors = 1 MiB on this array).
  - Per-member UUID at offset 0x10; per-member fields at 0x30/0x34/0x38.
  - Validating checksum at offset 0x00, computed by a ported version of
    `FUN_1400014ec` from `rcraid.sys`.
- **Stripe-mapping math** (`rc_volume_map_lba`):
  `member = (logical_lba / stripe) mod nmembers;
   phys   = ((logical_lba / stripe) / nmembers) * stripe + (logical_lba % stripe)`.
- **Volume registry** keyed off PCI BDF order, populated as each
  adapter's metadata validates. Fires `rc_volume_create_disk` at the
  expected member count (hardcoded `2` for now — see Open Questions).
- **/dev/rcraid0 gendisk** (blk-mq, qid=1, polled, read-only). Routes
  reads through `rc_volume_read_member` which issues one NVMe READ per
  request using PRP1/PRP2/PRP list as needed. Capacity = NSZE × member
  count = 4 TB on the dev box.
- **`reverse_member_order` module parameter** — escape hatch for the
  PCI BDF ordering guess.

### Fixed in this pass (2026-05-23, post-lockup investigation)

- **Sleep-in-RCU in `rc_volume_queue_rq`**. The blk-mq dispatch path
  runs inside an SRCU read-side critical section. `rc_nvme_io_cmd`
  polls completion with `usleep_range(10, 50)`, which is illegal
  there and triggered "Voluntary context switch within RCU read-side
  critical section" warnings under load. Fixed by setting
  `BLK_MQ_F_BLOCKING` on `rc_volume_tagset` so dispatch is allowed
  to sleep. This was almost certainly the cause of the post-bench
  lockup that motivated the reboot.
- **Wrong-pdev DMA in `rc_volume_demo_reads`**. The diagnostic
  allocated a single DMA buffer on member 0's pdev and reused the
  resulting IOVA for member 1 reads. Each member sits in its own
  IOMMU group (16 and 17 on this dev box) with a distinct domain,
  so member 1's controller saw an unmapped IOVA and AMD-Vi logged
  `IO_PAGE_FAULT` on every member-1 demo probe. The reads
  "succeeded" silently because the buffer was pre-zeroed and the
  array contains all zeros. Fixed by allocating one buffer per
  member in `rc_volume_demo_reads` and routing by `member_idx`.
  The real I/O path (`rc_volume_read_member` through
  `rc_volume_create_disk`'s per-member buffer pool) was already
  correct.
- **PRP list extended to 512 KiB per NVMe READ.** Per-member data
  buffer raised from 64 KiB → 512 KiB (16 → 128 pages). The original
  target was 1 MiB (one full stripe), but Identify Controller reports
  MDTS=7 at CAP.MPSMIN=0, which caps a single command at 2^7 × 4 KiB
  = 512 KiB. A 1 MiB request now becomes two back-to-back NVMe READs
  instead of sixteen. The PRP list buffer is still PAGE_SIZE; it
  needs 127 entries for a full 512 KiB transfer, well under the 512
  entries that fit. MDTS + CAP.MPSMIN are now decoded and logged
  alongside the existing Identify-Controller line.
- **Per-member metadata struct corrected from AMD Linux SDK.**
  `rcblob.x86_64` in the AMD-shipped Linux SDK ships with full DWARF
  debug info, exposing `struct RC_MetaData` (the LBA 0x5000 block).
  Renamed our `rc_raidcore_md` fields to match — `fld_18`→
  `config_commit_lba`, `fld_20`→`config_ring_lba`, `fld_30`→
  `features`+`spare_info`, `fld_38`→`mbr_checksum`, `member_uuid`→
  `device_id`. The +0x28 field we used as `stripe_sectors` is
  actually `ConfigRingSize`; it coincides with the real stripe on
  this dev box (2048 sectors = 1 MiB) because that's the firmware
  default for RAID0.
- **Member-count + member-position auto-detect.** We now walk the
  config ring (starting at `ConfigRingOffset` = LBA 0x5800) at probe
  time, looking for the `RC_LogicalDevice` record (tag
  `RC_DST_LOGICAL_DEVICE = 0x25BD`) whose element array contains
  this member's `DeviceId`. From that record we read `Devices`
  (member count), `Capacity`, `DeviceType` (`RC_LDT_RAID0 = 0x1BF6`),
  and the element index — that index is the member's position in the
  stripe layout. Replaces the hardcoded `RC_VOLUME_EXPECTED_MEMBERS=2`
  and the PCI-BDF ordering heuristic. AMD also publishes per-member
  "raw disk" LDs (1-device with `devtype=0x1BF9`) — the parser skips
  those by checking DeviceID membership. Capacity now comes from the
  LD (~3,814,380 MiB), correctly accounting for per-member metadata
  reserves vs the prior `NSZE * member_count` over-estimate.
- **Stripe size source confirmed.** `RC_LogicalDevice.ChunkSize`
  (offset 0xAC) is genuinely 0 for RAID0 in this firmware — verified
  by disassembling `RC_BuildConfigMetadataFromMemory` in rcblob. The
  writer never sets it for RAID0; the firmware uses a hardcoded 2048
  sector (1 MiB) default. Driver now uses ChunkSize when non-zero
  and falls back to the per-DeviceType default otherwise.
- **Write support behind `enable_writes` module parameter.** The
  `rc_volume_io_member` helper takes an opcode (READ/WRITE), and
  `rc_volume_queue_rq` handles both `REQ_OP_READ` and `REQ_OP_WRITE`:
  for writes, it stages bvec data into the per-member DMA buffer
  before submitting NVMe WRITE (opcode 0x01). The volume is read-only
  by default; loading with `enable_writes=1` drops the
  `set_disk_ro(1)` and routes writes through. Member positions come
  from the on-disk `RC_LogicalElement_LE` array — corruption-safe by
  construction once the LD parser succeeds. Verified end-to-end on
  the dev box with paired writes at sectors ~10^9 to both members
  followed by read-back — patterns match exactly, adjacent sectors
  untouched, no AMD-Vi events or kernel warnings.

### Interrupt-driven CQE wakeup (Stage 1)

`rc_nvme_io_cmd` and `rc_nvme_admin_cmd` now sleep on per-queue
`wait_queue_head_t`s (`io_cq_wait`, `admin_cq_wait`) instead of
busy-polling with `usleep_range`.  Create I/O CQ programs IEN=1 + IV=0
so the controller raises an interrupt on our MSI vector when a CQE is
posted.  `rc_nvme_init_controller` clears INTMS so the vector is
unmasked.  `rc_hw_interrupt_handler` dispatches to `rc_nvme_irq` on
NVMe-mode binds; the ISR just wakes both wait queues and the
submitter re-evaluates the phase-bit predicate inside
`wait_event_timeout`.  A 1 ms fallback poll-tick guards against
mis-armed MSI.

IRQ counts in `/proc/interrupts` climb 1:1 with completions —
~480 per a `dd bs=1M count=64` test.  Bench throughput improved
~14 % at large blocks (5.4 GB/s @ `bs=4M`, up from 4.7).  Stage 2
(full async via `blk_mq_complete_request`, queue_depth > 1, drop
`BLK_MQ_F_BLOCKING`) is next.

### AHCI scaffolding removed

The pre-NVMe AHCI scaffolding (`rc_blk.c`, `rc_metadata.c`,
`rc_raid.c`, `rc_queue.c`, the bulk of `rc_hw.c`, and the associated
struct definitions in `rc_linux.h`) has been deleted.  `rc_hw.c` is
now a small stub file that satisfies the `request_irq` /
init/cleanup call signatures for AHCI-mode binds until a real SATA
RAID path is built.  When that work happens it will be implemented
fresh from the Windows `rcbottom.sys` AHCI code path; no part of the
old scaffolding will be reused.

Source tree before: 9,406 lines across 15 source files.
Source tree after:  3,927 lines across 10.

### Not started

- **Interrupt-driven completion**. We poll, which costs a kworker
  thread per outstanding request.
- **Multiple I/O queues** (per-CPU). Currently one queue per controller,
  single hardware queue for the gendisk.
- **Write support**. Risky until member ordering and stripe-mapping is
  proven against a non-empty array. Adding NVMe WRITE (opcode `0x01`)
  is mechanically simple; gating it behind a module parameter is the
  right path.
- **Member position field**. Decompiling `RC_UpdateMetaData` confirmed
  the per-member 512 B block does *not* carry a position/index. The
  Windows driver must reconstruct ordering from controller-level state
  or BIOS config. We currently fall back to PCI BDF ordering with the
  `reverse_member_order` override.
- **Member count field**. Same story. Hardcoded `2` until decoded.

## How to validate the current state

```sh
sudo ./build.sh
sudo ./test_driver.sh
```

Expected dmesg highlights for DEV_B000:

```
rc_nvme_init_controller: CAP=0x080000203c01ffff MQES=65536 ...
rc_nvme_init_controller: ready — admin SQ depth=32 CQ depth=32, doorbell stride=0
rc_nvme_identify_controller: VID=0xc0a9 SSVID=0xc0a9 SN='...' MN='CT2000T700SSD3' FR='PACR5103' NN=1
rc_nvme_identify_namespace: nsid=1 NSZE=3907029168 ... LBA=512 B ... => 1907729 MiB
rc_nvme_create_io_queues: qid=1 up — SQ depth=64 CQ depth=64
rc_nvme_read_validate_metadata: RAIDCore v0x00030000 stripe=2048 sectors (1024 KiB) ...
rc_volume_register_member: 0000:81:00.0 registered at pos 0 (1/2)
rc_volume_register_member: 0000:82:00.0 registered at pos 1 (2/2)
rc_volume_create_disk: /dev/rcraid0 up, 7814058336 sectors (3815458 MiB, read-only)
```

Then `lsblk /dev/rcraid0` shows a 3.6 TiB read-only disk, and
`dd if=/dev/rcraid0 bs=64K count=N` reads through the assembled volume.

`bench.sh` after a successful load now reports ~760 MB/s @ `bs=4K`
rising to ~1.1 GB/s @ `bs=64K`, and confirms the RAIDCore magic at
logical sector 40960 (member 0, phys 0x5000).  Larger blocks scale
much further now that one NVMe READ carries up to 512 KiB:
~3.6 GB/s @ `bs=512K`, ~4.4 GB/s @ `bs=1M`, ~4.7 GB/s @ `bs=4M`.

For writes, load the module with `enable_writes=1`:

```sh
sudo modprobe -r rcraid && sudo insmod rcraid.ko enable_writes=1
```

`/dev/rcraid0` then accepts writes (lsblk's `RO` column reads 0) and
routes them through NVMe WRITE (opcode 0x01) at the per-member
position derived from the on-disk LD record.

## Next implementation steps (in order)

1. **Interrupt-driven completion** for both admin and I/O queues.
   Currently polls with `usleep_range`; dispatch holds a kworker per
   outstanding I/O. MSI vectors are already registered.
2. **Per-CPU I/O queues** to improve concurrency.
3. **Multiple-volume / non-RAID0 support** — the LD parser already
   handles other `RC_LDT_*` device types in principle; the stripe
   mapping and elsewhere need RAID-level-specific paths.
4. **SATA RAID path** — claim 1022:7905 / 0x43BD / 0x7916 / 0x7917 in
   MODULE_DEVICE_TABLE is already in place; the AHCI bring-up needs
   to be implemented (rewrite from rcbottom.sys, not from the old
   deleted scaffolding).
