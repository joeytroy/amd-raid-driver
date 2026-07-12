# Implementation Status

Current focus: **PCI 1022:B000 only** (TRX50 NVMe RAID Bottom). The AHCI
variants (`7905 / 7916 / 7917 / 43BD`) are not on the target hardware and
are best-effort/untested.

## Current state (2026-07)

The driver is a working daily-driver for **NVMe RAID0 and RAID1**: it boots
Linux from the array as rootfs (validated on hardware for both levels),
reads and writes at drive-limited speeds, and survives kernel updates via
DKMS. What's live today:

- **NVMe RAID0 + RAID1**, geometry auto-detected from on-disk RAIDCore
  metadata (member count, order, stripe size, RAID level — nothing
  hardcoded). The parser follows the **config-commit block** to the active
  config generation (the ring is a journal; deleted arrays' records
  persist in older generations) and derives the RAID level from
  **FirstCount × SecondCount** (firmware writes DeviceType 0x1BF6 for both
  levels). See the 2026-07-10 log entry below and
  [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md).
- **RAID1 mirror semantics**: round-robin read balancing (~2× single-drive
  read throughput), write/discard fan-out to every mirror with last-ACK
  completion (an fsync can't pass while a mirror holds stale data).
  Hardware-validated 2026-07-10: Kubuntu 24.04 installed onto and booting
  from a 2× T700 RAID1; KDiskMark SEQ1M Q8T1 20.5 GB/s read /
  11.1 GB/s write on the mirror as rootfs.
- **Reads and writes** (`enable_writes=1`; installers set it). Writes stage
  through scatterlist-native DMA — no bounce buffers. The fan-out paths
  merge physically-contiguous bio fragments before building PRPs
  (sub-page buffer-head writeback — mkfs metadata — is unexpressible as
  PRPs otherwise; see the 2026-07-10 log entry).
- **4 I/O queues × depth 256** per controller (blk-mq `nr_hw_queues=4`,
  1024 outstanding), MSI-X, fully interrupt-driven async completion. Each
  hctx has its own PRP-list pool (per-tag-per-member-per-hctx).
- **FLUSH, FUA, DISCARD/TRIM** wired up; volatile write cache enabled on each
  member at init.
- **Error handling**: 30 s timeout, per-adapter dead-flag, best-effort NVMe
  Abort, tagset drain, and automatic controller reset (manual sysfs reset as
  fallback). See "Error handling and reset (design)" below.
- **Boot-from-RAID** via dracut / initramfs-tools hooks; DKMS + udev autobind;
  live-CD install workflow; suspend/resume (S3/S4). Operational guards:
  `scripts/verify-boot-safety.sh` (checks every installed kernel's initramfs
  contains rcraid, on both initramfs-tools and dracut systems) and an apt
  kernel-hold drop-in installed by both installers so unattended-upgrades
  never pulls a kernel behind the DKMS rebuild's back.

Not yet: **RAID1 degraded mode and resync** (a member failure still fails
the volume — the mirror's data survives on both drives but the volume goes
down; and nothing tracks or repairs divergence between the copies after a
half-failed mirror write, so RAID1 currently provides redundancy of data
at rest, not availability; this is the top roadmap item), RAID10/5,
hot-plug/rebuild, SMART pass-through,
multi-volume, Secure Boot signing out of the box, array creation from Linux.
The prioritized roadmap is in [`IMPLEMENTATION.MD`](IMPLEMENTATION.MD); the
RE ground truth is in [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md).

The sections below are the dated implementation log — how each piece was built,
kept for forensics. Where an early "Not started" / "Next steps" note has since
been implemented, it's marked.

## 2026-07-10 — RAID1 hardware validation (PRs #43, #44, #45)

Reformatting the dev box for a real BIOS RAID1 array turned hardware
validation into a bug hunt that ended with the box booting from the mirror.
Every failure below shipped past the QEMU rig as it then existed; each fix
landed with the rig upgraded to regression-test that exact class.

- **Secure Boot preflight** (#43): the live installer died at insmod with
  "Key was rejected by service" — a BIOS reset had silently re-enabled
  Secure Boot. Both installers now read the SecureBoot efivar up front
  (failing closed on unreadable/truncated variables) and abort with
  instructions before building anything.
- **The config ring is a journal; the commit block names the truth** (#44):
  the driver assembled the box's *deleted* RAID0 (3.69 TB, writable!)
  instead of the new RAID1 — deleted arrays' LD records persist in older
  config generations, DeviceIDs are per-disk, so first-match-in-ring finds
  the corpse, and the stale record is internally consistent so the
  geometry-trust gate never fires. The commit block at ConfigCommitOffset
  names the active generation (extent + timestamp linkage, both now
  validated). Same PR: firmware writes DeviceType 0x1BF6 for RAID0 *and*
  RAID1 — the real level encoding is FirstCount × SecondCount (stripe
  width × mirror count).
- **Three latent concurrency bugs** (#44, exposed by the new CI rig):
  boot-time sync metadata reads raced the per-queue ISR on the same CQ
  during module reload (fixed with a locked handshake + dedicated
  *sequenced* sync CIDs — a fixed CID 0 aliases blk-mq tag 0, and a stale
  CQE from a timed-out sync command must not satisfy the next one); the
  ISR's direct-end fast path could double-complete a request against the
  dead-member drain (fixed with an atomic per-request completion claim);
  raw single-disk LDs (0x1BF9) could assemble as a bogus 1-member volume
  (their element array *is* the disk's own DeviceID — devices < 2 records
  are now skipped).
- **Fan-out scatterlists must merge contiguous fragments** (#45): the OS
  install died at mkfs — every mirror write of mke2fs metadata rejected by
  both members with NVMe SC 0x13 "PRP Offset Invalid". The hand-rolled
  bvec walks in the mirror/multi-stripe paths built one sg entry per bvec;
  sub-page buffer-head writeback then puts non-first segments at in-page
  offsets, which PRP lists cannot express. `blk_rq_map_sg` (the
  single-member path) merges — the fan-out builders now do too
  (`rc_volume_sg_append`), and `rc_volume_build_prp` rejects
  unexpressible layouts with one clean I/O error instead of submitting.
- **CI now boots the driver in QEMU on every PR** (`qemu-rig` jobs,
  raid0 + raid1): synthetic metadata booby-trapped with a stale decoy
  generation and a raw single-disk LD, plus a guest battery of
  write/readback, 3× module-reload cycles (with legacy-fallback
  detection), discard+rewrite, and a real bundled `mke2fs` run.

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

### DISCARD (NVMe DSM Deallocate)

`REQ_OP_DISCARD` is now handled: blk-mq splits each discard request
at `chunk_sectors` (one stripe = one member), and `queue_rq` issues
exactly one NVMe DSM command (opcode 0x09) with the AD attribute to
that member's I/O queue.  The DSM range list (16 bytes for one
range) is staged in the per-tag PRP buffer that already exists.

Queue limits expose `max_hw_discard_sectors = stripe_sectors`,
`discard_granularity = 512`, `max_discard_segments = 1`.  Verified
on the dev box with `blkdiscard`: returned 0, IRQs counted on both
members, post-discard reads return zeros (the Crucial T700 happens
to zero on deallocate even though we conservatively don't advertise
that guarantee).  No regressions.

### FLUSH + FUA support

`REQ_OP_FLUSH` is now handled: fans out one NVMe FLUSH (opcode 0x00)
to every member's I/O queue, atomically tracks completions in a new
per-request `atomic_t members_pending`, and only the last ISR to land
calls `blk_mq_complete_request`.  `REQ_FUA` writes pass the FUA bit
through into the SQE's CDW12.

Queue limits now advertise `BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA`, so
filesystems will route fsync/sync as REQ_OP_FLUSH and use FUA for
sync writes.  Verified on the dev box: `/sys/block/rcraid0/queue/`
shows `write_cache: write back` and `fua: 1`; a `dd ... conv=fsync`
generates IRQs on both members.

Cumulative I/O status (`pdu->sc_sct`) is logged via
`printk_ratelimited` if any member returns a non-zero NVMe SC, and
the request ends with `BLK_STS_IOERR`.

### True async completion (Stage 2)

`rc_volume_queue_rq` now submits + returns `BLK_STS_OK` immediately;
the dispatch path is non-blocking (no more `BLK_MQ_F_BLOCKING`).
The MSI handler `rc_nvme_irq` walks the I/O CQ, looks each CQE's
CID up via `blk_mq_tag_to_rq`, and completes the request with
`blk_mq_complete_request`.  A new `.complete` callback runs in
softirq and finishes the request: bvec memcpy for READ, then
`blk_mq_end_request`.  Per-adapter `spin_lock_irqsave` guards the
SQ tail (submitter) + CQ head (ISR).

Queue depth raised from 1 → 32.  Per-tag-per-member DMA buffer pool
(`rc_volume_dma_va[member][tag]`) backs the bounce-buffer staging
for each in-flight request — memory cost ~33 MiB on the dev box.

NVMe CID = blk-mq tag, so the ISR's CID→request lookup is O(1) via
`blk_mq_tag_to_rq`.  A boot-time `rc_nvme_io_cmd_sync` helper keeps
the metadata-read path synchronous (uses the same MSI wake path
since `rc_volume_disk` is NULL during boot and `rc_nvme_irq`
returns early without touching the CQ).

Concurrent-reader bench on the dev box (8 × `dd bs=1M count=128`,
spread across the volume): **11.9 GB/s aggregate**, ~2× what
Stage 1 could sustain.  Single-threaded `bench.sh` also up
30–70 % across block sizes (1.3 GB/s @ 4K, 1.9 GB/s @ 8K).  No
"unknown CID" warnings, no RCU complaints, no IRQ disable.

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

### Error handling and timeouts

`rc_volume_mq_ops.timeout` now fires at the blk-mq default 30 s.  Per-adapter
`nvme.dead` flag tracks controller health: set from the ISR's CSTS canary
(catches `CFS` and `CSTS=~0` / hot-unplug), from `.timeout` when CSTS goes
bad mid-investigation, and from `.timeout` after a stuck command (since we
have no controller-reset path, we can't safely recycle the CID and must
take the adapter offline).  Dispatch fast-fails through dead members,
`blk_mq_tagset_busy_iter` drains every in-flight request that targeted a
dead adapter, and best-effort NVMe Abort (admin opcode 0x08) is issued
before declaring a still-CSTS-alive controller dead.  `rc_volume_complete`
now decodes SC / SCT / DNR / More from the stored status; a sentinel
`RC_VOLUME_SC_DEAD = 0x7fff` is used by drain so the logs distinguish
"controller dead" from real NVMe failures.  `rc_nvme_admin_cmd` is now
serialised by a per-adapter `admin_mutex` so timeout-issued Aborts don't
collide with teardown.

Behaviour change worth flagging: one stuck command takes the volume
offline until either the operator runs the sysfs reset or the module
is reloaded.  Full design notes in the "Error handling and reset
(design)" section at the end of this document.

### Scatterlist-native DMA

Hardware now reads/writes the bio's user pages directly via
`dma_map_sg` + PRP enumeration.  Per-tag `rc_volume_dma_va[][]`
bounce buffers are gone (~33 MiB freed at QD=32); the WRITE
bvec→DMA-buffer memcpy in `queue_rq` and the READ DMA→bvec memcpy
in `.complete` are both gone with them.

Queue limits added `virt_boundary_mask = PAGE_SIZE - 1` so blk-mq
splits bios so every segment after the first is page-aligned —
which is what NVMe PRP semantics require (PRP1 may have an in-page
offset, PRP2+ must be page-aligned).  `rc_volume_build_prp` walks
the dma_map_sg output one page at a time, handling IOMMU-coalesced
multi-page sg entries correctly.

`pdu->sg[RC_VOLUME_DATA_PAGES]` is inline in the per-request command
data (cmd_size += ~5 KiB per tag = ~165 KiB total at QD=32 vs the
33 MiB it replaced).  `rc_volume_unmap_request_sg` is idempotent and
called from `.complete` for the normal path; the timeout direct-end
safety-nets call it too so an unmapped sg never leaks through the
no-`.complete` paths.

Bench impact on the dev box: ~2× at 64 K reads (2.1 GB/s vs 1.1 GB/s
before — the eliminated READ memcpy in softirq was the bottleneck),
~6.7 GB/s at 1 M (was ~4.4), flat at 4 M (already bandwidth-bound).
4 K direct slightly worse (249 MB/s vs 1.3 reported in earlier
benches) — single-page transfers see `dma_map_sg` overhead without
amortising it, but most workloads aren't 4 K direct anyway.

### Controller reset (manual)

`rc_nvme_reset_controller(adapter)` is the recovery path out of the
`dead` state.  Operator triggers it via the per-adapter sysfs `reset`
attribute under `/sys/bus/pci/devices/<bdf>/rcraid/reset`.  Sequence:
quiesce + drain → `disable_irq` → mask + `CC.EN=0` → wait `CSTS.RDY=0`
→ zero the SQ/CQ buffers in place → reprogram AQA/ASQ/ACQ → `CC.EN=1`
→ wait `CSTS.RDY=1` → unmask + `enable_irq` → re-issue Create I/O CQ
+ Create I/O SQ → clear `dead` → unquiesce.  Whole thing held under
`admin_mutex`; DMA buffers (admin queues, I/O queues, per-tag I/O
buffers) are reused.  Any failure beyond the disable leaves the
adapter dead and returns `-EIO`.

### Automatic reset on timeout

`.timeout` schedules `rc_nvme_reset_controller` automatically for any
adapter it flags dead, via a per-adapter `work_struct`.  Policy is one
attempt per death episode: a successful reset clears both `dead` and
the `auto_reset_disabled` latch (so the next death is also
auto-treatable); a failed reset latches `auto_reset_disabled` and
further `.timeout` invocations only drain — operator must run the
sysfs `reset` to recover, which clears the latch on success.  Avoids
thrashing genuinely-fried silicon while self-healing the common cases
without operator intervention.  `cancel_work_sync` runs first in
`rc_nvme_cleanup_controller` so an in-flight auto-reset can't race
admin/MMIO teardown at module unload.

### Since done (this section was the mid-2025 backlog)

The four items below were open when this log was first written; all have
since shipped and are described in the sections above:

- **Interrupt-driven completion** — done (Stage 1 + Stage 2 async above).
- **Multiple I/O queues (per-CPU)** — done: 4 hctx, `nr_hw_queues=4`,
  `blk_mq_pci_map_queues`, per-hctx PRP pool.
- **Write support** — done behind `enable_writes=1`, scatterlist DMA.
- **Member position / member count** — done: both are read from the on-disk
  `RC_LogicalDevice` config-ring record (`Devices` at `+0x68`, element-array
  membership for position), replacing the hardcoded `2` and the PCI-BDF
  ordering heuristic. `reverse_member_order` remains as an escape hatch.

## How to validate the current state

```sh
sudo ./build.sh
sudo ./test_driver.sh
```

Expected dmesg highlights for DEV_B000 (current build — writes on via the
installers, 4 I/O queues, depth 256):

```
rc_nvme_init_controller: CAP=0x080000203c01ffff MQES=65536 ...
rc_nvme_identify_controller: ... MN='CT2000T700SSD3' ... MDTS=7 ... VWC=1
rc_nvme_enable_write_cache: 0000:81:00.0 volatile write cache ENABLED (WCE=1)
rc_nvme_identify_namespace: nsid=1 NSZE=3907029168 ... LBA=512 B ...
rc_nvme_create_io_queues: 4 I/O queues up — SQ depth=256 CQ depth=1024
rc_volume_register_member: 0000:81:00.0 registered at pos 0 (1/2)
rc_volume_register_member: 0000:82:00.0 registered at pos 1 (2/2)
rc_volume_create_disk: blk-mq nr_hw_queues=4 (queue_depth=256 → 1024 total outstanding)
rc_volume_create_disk: /dev/rcraid0 up, 7811850240 sectors (3814380 MiB, read-write)
```

Then `lsblk /dev/rcraid0` shows a 3.6 TiB disk. Sustained `fio --direct=1`
SEQ1M Q8T1 measures **~19.7 GB/s read and ~19.7 GB/s write** on the 2× T700
array (see the perf table in [`IMPLEMENTATION.MD`](IMPLEMENTATION.MD)).

Writes are enabled by the installers; a bare manual `insmod` is read-only
unless loaded with `enable_writes=1`:

```sh
sudo modprobe -r rcraid && sudo insmod rcraid.ko enable_writes=1
```

## Next implementation steps (in order)

The original bring-up backlog (interrupt-driven completion, per-CPU queues) is
done — see "Since done" above. What remains, in rough priority:

1. **Non-RAID0 support (RAID1 first, then RAID10)** — the LD parser already
   recognises other `RC_LDT_*` device types; the stripe mapping and dispatch
   need RAID-level-specific paths. See [`IMPLEMENTATION.MD`](IMPLEMENTATION.MD).
2. **Multi-volume support** — one gendisk per LD instead of a single
   `/dev/rcraid0`.
3. **Hot-plug / drive-failure handling** — clean member teardown + drain.
4. **SATA RAID (AHCI) path** — the `1022:7905 / 43BD / 7916 / 7917`
   MODULE_DEVICE_TABLE claims are in place; the AHCI bring-up must be
   implemented fresh from `rcbottom.sys` (not the old deleted scaffolding).

---

## Error handling and reset (design)

How the I/O path detects, contains, and reports failures. This all lives in
`rc_nvme.c`; this section explains the *why* so a reader can navigate the code
without reverse-engineering the design from individual functions. (The
implementation-log entries above — "Error handling and timeouts", "Controller
reset (manual)", "Automatic reset on timeout" — are the summary; this is the
full design.)

What's implemented: a 30 s blk-mq timeout callback; a per-adapter `dead` flag
with three set paths; best-effort NVMe Abort on stuck commands; tagset-wide
drain of in-flight requests when an adapter dies; decoded SC/SCT/DNR/More in
failure logs; and manual + automatic controller reset. Still open (see "Limits"
at the end): retry of transient (DNR=0) errors, and recovery for the sync
init-time helpers.

### Adapter health states

Every adapter is either **alive** or **dead**. The flag lives in
`adapter->ctx.nvme.dead` (`rc_linux.h:struct rc_nvme_state`). It starts zero
(kzalloc), and — absent a reset — transitions to `true` once and is read with
`READ_ONCE` / written with `WRITE_ONCE`. (A successful controller reset clears
it back to false; see below.)

Three transitions to dead:

1. **`rc_nvme_irq` health canary.** Every ISR entry reads `CSTS` first. If
   `CSTS == 0xffffffff` (hot-unplug / PCI vanished) or `CSTS.CFS` (controller
   fatal status) is set, the CQ contents are no longer trustworthy. The flag is
   set, both wait queues are woken so sync-init helpers return promptly, and the
   ISR returns without touching the CQ. Hardirq-safe — no sleeping, no work
   scheduling; the next `.timeout` handles cleanup.
2. **`rc_nvme_check_dead` from `.timeout`.** Process-context probe that re-reads
   CSTS with the same predicates, used by `rc_volume_timeout` to decide whether
   a stuck request is just slow or actually unreachable.
3. **`rc_volume_timeout` after a "stuck" command.** Even if CSTS still reads
   sane, an unresponsive command means we cannot safely recycle its CID (see
   "CID-recycle hazard"); the handler flags the adapter dead as part of its
   response, which then triggers auto-reset.

### I/O path participants

Five functions cooperate, each with one job:

| Function | Context | Job |
|---|---|---|
| `rc_volume_queue_rq` | submitter | Fast-fail if `rc_volume_any_member_dead()` before touching the SQ. |
| `rc_nvme_irq` | hardirq | CSTS canary first, then walk the CQ and `blk_mq_complete_request` per CID. |
| `rc_volume_complete` | softirq | Decode `pdu->sc_sct`; log SCT/SC/DNR/More for real failures, "controller dead" for the sentinel; end with `BLK_STS_IOERR` or (READ memcpy first) `BLK_STS_OK`. |
| `rc_volume_timeout` | workqueue | The recovery decision tree (below). |
| `rc_volume_drain_dead` | workqueue | Walk `blk_mq_tagset_busy_iter`, complete every in-flight request that touched a dead adapter with the sentinel SC. |

### Timeout flow

Triggered when a request's CQE hasn't landed within 30 s. Returns
`BLK_EH_DONE` in every path — blk-mq will not retry; the driver has taken
responsibility for ending the request.

```
.timeout(req)
 │
 ├── blk_mq_request_completed(req)?      ─yes→ BLK_EH_DONE  (ISR raced and won)
 │
 ├── rc_nvme_check_dead(involved members)?
 │   yes ────────────────────────────────→ drain_dead() → end(IOERR) → BLK_EH_DONE
 │   no
 │
 ├── issue rc_nvme_abort() to involved members  (best effort)
 ├── blk_mq_request_completed(req)?      ─yes→ BLK_EH_DONE  (abort completed it)
 │
 └── WRITE_ONCE(involved.dead = true)
     → drain_dead() → end(TIMEOUT) → schedule auto-reset → BLK_EH_DONE
```

"Involved members": R/W/DISCARD → just `pdu->member_idx` (stripe-mapped at
submit); FLUSH → all members (fan-out, can't complete unless every member
ACKs). The `blk_mq_request_completed` re-check after Abort matters: the Abort
may have caused the controller to post the original CQE (SC = "Aborted by
Request"), so the ISR may have run during the abort and we mustn't end the same
tag twice.

### CID-recycle hazard

This is the core reason `.timeout` is so aggressive about flagging adapters
dead. When `blk_mq_end_request(req, BLK_STS_TIMEOUT)` returns, blk-mq frees the
request and the tag (the integer that became `CID` in the SQE) is eligible for
reuse. If the controller later posts a CQE for that CID — late but real — the
ISR's `blk_mq_tag_to_rq(tags, cid)` looks up whatever request now holds the
tag, possibly an unrelated I/O. For a READ the ISR would memcpy the per-tag DMA
buffer (data for the *original* LBA) into the new request's bvecs — **silent
data corruption**. Without a reset path the only safe option is to stop
dispatching to that controller and drain in-flight requests. A real reset
recovers the CID space cleanly (CC.EN=0 → re-enable resets the CID counters),
letting the volume keep serving — which is why auto-reset exists.

### Drain protocol

`rc_volume_drain_dead` is the cleanup pass, called from `.timeout` only (the
ISR is hardirq and can't run it):

1. Build `dead_mask` — one bit per dead member.
2. If zero, no-op.
3. `blk_mq_tagset_busy_iter` runs `rc_volume_drain_iter` for every in-flight
   request.
4. Each request: "would it need a dead member to complete?" (FLUSH: any dead
   member; R/W/DISCARD: `member_idx ∈ dead_mask`).
5. If yes and not already completed, set `pdu->sc_sct = RC_VOLUME_SC_DEAD` and
   `blk_mq_complete_request`.

`blk_mq_complete_request` is atomic against the ISR's competing call — only one
wins the state-machine transition, so a naturally-arriving CQE during iteration
completes safely and drain becomes a no-op for that request.

### Sentinel SC

`RC_VOLUME_SC_DEAD` (`0x7fff`) distinguishes "we killed this because the
controller is dead" from a real NVMe error. `pdu->sc_sct` is `CQE.status >> 1`:
bit 14 = DNR, bit 13 = More, bits 10:8 = SCT, bits 7:0 = SC. `0x7fff` sets
every bit (DNR=1, More=1, SCT=7, SC=0xff) — a combination a spec-compliant
controller never posts for a real command, so it's reliably distinct.
`rc_volume_complete` checks for it first and prints "controller dead".

### admin_mutex

`rc_nvme_admin_cmd` is serialized by a per-adapter `nvme.admin_mutex`, held
across SQE write, doorbell ring, CQE wait, and CQ-head advance. Before async,
the admin queue was only used single-threaded at init; now `.timeout` can issue
an Abort from a workqueue while teardown or another timeout touches the same
queue. CID stays hardcoded to `0` — only one admin command is in flight at a
time, guaranteed by the mutex. The split between `__rc_nvme_admin_cmd_locked`
and `rc_nvme_admin_cmd` lets reset hold the mutex across its Create I/O CQ/SQ
commands without deadlocking.

### Controller reset (manual + automatic)

`rc_nvme_reset_controller(adapter)` is the recovery path out of `dead`.
Operators trigger it via the per-adapter sysfs attribute:

```
echo 1 | sudo tee /sys/bus/pci/devices/0000:81:00.0/rcraid/reset
```

Returns 0 on success or `-errno` (`-EIO` if the controller refused to come
ready, `-ENOTSUPP` for non-NVMe adapters, `-EINVAL` for input other than `1`).
Sequence under `admin_mutex`:

```
1. WRITE_ONCE(dead = true)      (idempotent)
2. blk_mq_quiesce_queue         no new .queue_rq
3. rc_volume_drain_dead         fail in-flight; their CIDs are about to be wiped
4. disable_irq                  let in-flight ISR finish before register writes
5. INTMS=~0, CC.EN=0, wait CSTS.RDY=0
6. memset SQ/CQ buffers; reset tail/head/phase
7. Reprogram AQA/ASQ/ACQ, CC.EN=1, wait CSTS.RDY=1
8. INTMC=~0, enable_irq
9. Create I/O CQ + Create I/O SQ on the existing DMA buffers
10. WRITE_ONCE(dead = false)
11. blk_mq_unquiesce_queue      new I/O resumes
```

Any failure between 5 and 9 returns `-EIO` with `dead` still set; module reload
becomes the only recovery. The reset deliberately does **not** re-issue
Identify (CAP/MDTS/namespace shouldn't change on the same silicon),
re-allocate DMA buffers (handles persist across CC.EN=0; the controller just
needs them re-bound via Create I/O CQ/SQ), or touch the gendisk/tagset.

`.timeout` schedules this automatically for each adapter it flags dead, via one
`work_struct` per adapter plus an `auto_reset_disabled` latch. Policy is **one
attempt per death episode**: `schedule_work`'s "already queued" check coalesces
multiple `.timeout` invocations; a successful reset clears both `dead` and the
latch (so a fresh death hours later gets its own attempt); a failed reset
latches `auto_reset_disabled` and further timeouts only drain — the operator's
manual sysfs reset clears the latch on success. This avoids the thrash mode
(fried silicon → 30 s timeout → 50 ms reset → timeout → …) while self-healing a
single hung command or transient fault without intervention.
`cancel_work_sync(&nvme->auto_reset_work)` runs first in
`rc_nvme_cleanup_controller` so an in-flight auto-reset can't race teardown.

### Limits (still open)

- **Retry on transient SCs.** Distinguish DNR=0 (retryable) from DNR=1 and
  re-dispatch rather than ending the request. Belongs on top of reset (a
  recovered controller is the right place to retry). *This is the caveat the
  README lists under "Not yet supported."*
- **Abort path for sync init helpers.** `rc_nvme_admin_cmd` and
  `rc_nvme_io_cmd_sync` time out at 2 s and surface `-ETIMEDOUT`; no caller
  needs to soldier on past that yet, so no recovery code exists for them.

### Behaviour for users

- **Before**: a stuck command wedged its tag forever with a ratelimited
  warning; no recovery short of reboot.
- **After**: stuck commands time out at 30 s and trigger an automatic reset.
  The offending request still fails with `BLK_STS_TIMEOUT`, but the controller
  is back ~50 ms later and subsequent I/O succeeds. If the auto-reset itself
  fails, the controller stays dead and `echo 1 > .../rcraid/reset` is the manual
  escape hatch.

If the volume goes offline unexpectedly, check `dmesg` for:

```
rcraid: rc_nvme_irq: 0000:..:.. controller dead (CSTS=0x........) — failing in-flight I/O
rcraid: rc_volume_timeout: tag=N op=M: member dead — draining in-flight
rcraid: rc_volume_timeout: tag=N op=M: issuing NVMe Abort
```

The first is the ISR catching CSTS.CFS or a hot-unplug; the second is
`.timeout` finding a dead controller; the third is the "still looks alive but
this command is hung" path.
