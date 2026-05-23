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
  this dev box (2048 sectors = 1 MiB).  Real volume-level fields
  (member count, stripe = ChunkSize, member position) live in an
  `RC_LogicalDevice` record reachable through the config packet at
  LBA 0x5001 → see `docs/OPEN_QUESTIONS.md` for the layout-drift
  blocker.

### Implemented before this pass, may need revisiting

- Block / SCSI scaffolding (`rc_blk.c`, `rc_raid.c`) — built for the
  AHCI model. Now superseded by the NVMe path; the AHCI-only register
  writes in `rc_queue_init` still run but no-op against B000 because
  the offsets aren't writable on this device.
- Vendor mailbox builder (`rc_queue.c`, `rc_ahci_build_mailbox`) — AHCI
  command path; not needed for DEV_B000.

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

## Next implementation steps (in order)

1. **Member-count auto-detect** — by reading LBA 0x5001 (volume
   metadata) which appears to encode the member list.
2. **Member-position resolution** — either decode further or send a
   vendor-specific admin command to query controller-side state.
3. **Interrupt-driven completion** for both admin and I/O queues.
4. **Per-CPU I/O queues** to improve concurrency.
5. **Write support** behind a flag, carefully gated until ordering is
   proven on a populated array.
