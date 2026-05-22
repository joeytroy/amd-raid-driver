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

### Implemented before this pass, may need revisiting

- Block / SCSI scaffolding (`rc_blk.c`, `rc_raid.c`) — built for the
  AHCI model. Now superseded by the NVMe path; the AHCI-only register
  writes in `rc_queue_init` still run but no-op against B000 because
  the offsets aren't writable on this device.
- Vendor mailbox builder (`rc_queue.c`, `rc_ahci_build_mailbox`) — AHCI
  command path; not needed for DEV_B000.

### Not started

- **PRP list for transfers > 64 KiB**. Current data buffer per member
  is 64 KiB. Bumping it up to one full stripe (1 MiB) would let the
  kernel send max-size requests.
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

## Next implementation steps (in order)

1. **PRP list to one stripe** — allow 1 MiB requests for better
   sequential perf.
2. **Member-count auto-detect** — by reading LBA 0x5001 (volume
   metadata) which appears to encode the member list.
3. **Member-position resolution** — either decode further or send a
   vendor-specific admin command to query controller-side state.
4. **Interrupt-driven completion** for both admin and I/O queues.
5. **Per-CPU I/O queues** to improve concurrency.
6. **Write support** behind a flag, carefully gated until ordering is
   proven on a populated array.
