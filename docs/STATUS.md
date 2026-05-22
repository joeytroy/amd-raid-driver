# Implementation Status

Current focus: **PCI 1022:B000 only** (TRX50 NVMe RAID Bottom). The AHCI
variants (`7905 / 7916 / 7917 / 43BD`) are not on the target hardware and
are best-effort/untested.

## Where we are

### Done

- **PCI-ID-based code path selection** (`rc_firmware.c`). DEV_B000 →
  NVMe, AHCI variants → AHCI, anything else → stub. Mirrors what
  `FUN_140007d40` in `rcbottom.sys` does.
- **NVMe controller boot** (`rc_nvme.c`, `rc_nvme_init_controller`).
  Reads `CAP`, disables, allocates 4 KiB-aligned admin SQ/CQ via
  `dma_alloc_coherent`, programs `AQA/ASQ/ACQ/CC`, waits for
  `CSTS.RDY`. Read-back of `ASQ/ACQ` verifies the register layout
  before continuing.
- **AHCI register programming gated** on `ctrl_mode == AHCI`
  (`rc_hw.c`, `rc_bottom.c`). DEV_B000 no longer triggers the
  AHCI-only `rc_hw_program_queues()` writes to BAR offsets `0x20–0x38`
  or the `rc_activate_doorbells()` `1,4,2,3` sequence.
- **Doorbell init order 1,4,2,3** kept as-is for the AHCI path
  (confirmed correct by `FUN_140008638`).

### Implemented before this pass, may need revisiting

- Block / SCSI scaffolding (`rc_blk.c`, `rc_raid.c`) — built for the
  AHCI model. Probably reusable for NVMe, but not exercised yet.
- Vendor mailbox builder (`rc_queue.c`, `rc_ahci_build_mailbox`) — AHCI
  command path; not needed for DEV_B000.
- Work-item queue (`rc_queue.c`) — generic deferred work; reusable.

### Not started for the NVMe path

- **Admin command submission** (Identify Controller, Identify Namespace,
  Set Features). Driver gets the controller to `CSTS.RDY = 1` but
  doesn't submit any admin commands yet.
- **I/O queue creation** (Create I/O CQ / Create I/O SQ). Same.
- **Doorbell writes for NVMe** (`BAR0 + 0x1000 + 2 * qid * (4 << CAP.DSTRD)`).
  Helpers not written yet.
- **Completion ISR for NVMe.** Phase-bit walk of the CQ; not started.
- **RAID volume discovery.** Vendor-specific path inside the AMD
  controller; needs further reverse engineering of `rcraid.sys`.
- **Block device registration.** Currently keyed on the AHCI path
  completing — won't fire for DEV_B000 yet.

## How to validate the current state

On a Linux box (kernel headers required):

```sh
make clean && make
sudo insmod rcraid.ko debug_level=0
dmesg | grep -E 'rc_(parse|nvme|hw)_'
```

Expected first-time output for DEV_B000:

```
rc_parse_firmware_capabilities: PCI 1022:b000 class=0x010800 → mode=2 (NVMe)
rc_nvme_init_controller: CAP=0x... VS=0x... MQES=... TO=...
rc_nvme_init_controller: AQA=0x... (rb 0x...) ASQ=0x... (rb 0x...) ACQ=0x... (rb 0x...)
rc_nvme_init_controller: ready — admin SQ depth=...
rc_hw_init: skipping AHCI register programming (ctrl_mode=2)
```

The decisive check is the **AQA / ASQ / ACQ read-back**:

- If the read-back values match what was written → NVMe register layout
  is correct. The "writes to `0x100+` don't persist" issue documented
  in `GHIDRA_FINDINGS_2026.md` was just a wrong-register-set problem.
  Move on to admin commands.
- If they don't match → the controller may be in a mode that needs a
  vendor-specific unlock first (BAR may need to be re-read, or there
  may be a config-space write needed). Reopen Ghidra and trace
  `FUN_140008f34` (BAR discovery) before the WDF class-bind.

## Next implementation steps (in order)

1. NVMe doorbell helpers (`rc_nvme_write_sq_tail`, `rc_nvme_write_cq_head`).
2. Synchronous admin-command submission with completion polling
   (`rc_nvme_submit_admin_sync`).
3. `Identify Controller` (CNS=1) → dump model / serial / firmware /
   max queue count. Confirms a real command round-trip.
4. `Identify Namespace` (CNS=2 list of NSIDs, then CNS=0 per NSID) →
   discover backing namespaces.
5. I/O queue creation (one CQ + one SQ to start, depth = `min(64, MQES)`).
6. Hook up block-device registration only after queue creation
   succeeds.
