# rcraid — AMD-RAID Linux driver

A clean-room Linux kernel module for AMD-RAID NVMe and SATA RAID
arrays. Initially targets the TRX50 chipset's NVMe RAID Bottom
controller (PCI `1022:B000`); SATA and additional NVMe variants are
on the roadmap.

## What works today

- PCI `1022:B000` (NVMe RAID Bottom) end to end: controller bring-up,
  admin + I/O queues, NVMe Identify, RAID0 volume assembly from
  on-disk metadata, blk-mq read/write at `/dev/rcraid0`.
- Member count, per-member position, and volume capacity all parsed
  from the on-disk `RC_LogicalDevice` record — no hardcoding.
- Interrupt-driven async completion: MSI ISR walks the CQ and routes
  each CQE to `blk_mq_complete_request`; queue depth 32; no polling
  on the dispatch path.
- `REQ_OP_FLUSH` (NVMe FLUSH 0x00, fanned out to every member) and
  `REQ_FUA` (CDW12.FUA passthrough) — filesystems can fsync safely.
- `REQ_OP_DISCARD` via NVMe DSM Deallocate (0x09), split at the
  stripe boundary so each request lands on one member.
- Writes gated behind `enable_writes=1` module parameter (off by
  default for safety; load with the param to allow).
- `safe_subsys_vendor` module parameter to keep `rcraid` off the OS
  drive — the AMD chipset shadows every NVMe as `1022:b000`, so this
  filter is what prevents the driver from claiming the boot SSD.
- Error handling + timeouts: blk-mq `.timeout` at 30 s, per-adapter
  `dead` flag, ISR CSTS canary, best-effort NVMe Abort, tagset drain
  of in-flight requests when a controller dies.  See
  `docs/ERROR_HANDLING.md`.
- Bench throughput on a 2-member Crucial T700 RAID0 dev box:
  ~1.3 GB/s @ `bs=4K`, ~4.7 GB/s @ `bs=4M`, ~11.9 GB/s aggregate
  across 8 concurrent readers.

See `docs/STATUS.md` for the full state and the next-steps list.

## Hardware support

The driver claims the same five PCI IDs as AMD's Windows
`rcbottom.sys`, covering both the NVMe and the older SATA RAID
controllers AMD has shipped:

| PCI ID | Variant | Status here |
|--------|---------|-------------|
| `1022:B000` | NVMe RAID Bottom (TRX50, WRX90, X870E/X870, B850/B840, X670E/X670, etc.) | **Fully implemented, RAID0 R/W validated.** |
| `1022:43BD` | Promontory SATA RAID | Claimed; AHCI path is stub. |
| `1022:7905` | Older SATA RAID | Claimed; AHCI path is stub. |
| `1022:7916` | Older SATA RAID | Claimed; AHCI path is stub. |
| `1022:7917` | X570S-era SATA RAID | Claimed; AHCI path is stub. |

These IDs cover every CPU/chipset in AMD's published support matrix
for `rcraid` — Ryzen 7000+ desktop, Ryzen AI 300 / AI Max 300,
Threadripper 7000 / 9000 (TRX50 / WRX90), and the X870/B850/X670/B650
consumer chipsets.  The platform variety is in firmware, not in
distinct PCI devices.

RAID-level coverage matches AMD: RAID 0, 1, 10 are target features
for any of the above; RAID 5 is only supported by AMD on the 3rd Gen
Threadripper (TRX40 / Castle Peak) and is not currently on our
roadmap.

## What's NOT here yet

- RAID levels other than RAID0 (RAID1 / 10 are roadmap; RAID5 is not).
- SATA RAID (AHCI variants `7905 / 7916 / 7917 / 43BD`) — code paths
  exist but are stubs.
- Controller reset & recovery.  Today a stuck command takes the
  volume offline until module reload — intentional, since without
  reset we can't safely recycle CIDs.  See
  `docs/ERROR_HANDLING.md` for the reasoning.
- Retry of transient NVMe errors.  Depends on controller reset above.
- Per-CPU I/O queues.  Single hw queue caps small-I/O IOPS today.
- `scatterlist`-native DMA.  We currently bounce through per-tag-per-
  member DMA buffers (~33 MiB pinned at QD=32).
- Suspend / resume hooks.
- `rcadm`-equivalent userspace tooling (create / inspect / delete
  arrays).  Today the array must already exist.

See `docs/OPEN_QUESTIONS.md` for what still needs reverse-engineering
work, and `docs/STATUS.md` for the implementation roadmap.

## Build and load

```sh
make
sudo ./test_driver.sh        # loads the module, binds /dev/rcraid0 (RO)
sudo ./bench.sh              # post-load smoke test
```

For read-write use, load with the write flag:

```sh
sudo modprobe -r rcraid 2>/dev/null
sudo insmod rcraid.ko enable_writes=1
```

Full setup and troubleshooting in `INSTALL.md`.

## License

This driver is licensed under **GPL-2.0-only**. See `LICENSE` for the
full text and `RE_METHODOLOGY.md` for the clean-room reverse-engineering
process and legal record.

The PCI ID table, on-disk metadata layouts, and protocol-level
behaviour are derived from analysis of AMD's publicly distributed
Windows drivers (`rcbottom.sys`, `rcraid.sys`, `rccfg.sys`) under
DMCA §1201(f) interoperability protections. No AMD source code is
incorporated into this driver.

## Repository layout

| Path | What's there |
|------|--------------|
| `rc_*.c`, `rc_*.h` | The Linux driver. SPDX `GPL-2.0-only`. |
| `Makefile`, `build.sh`, `test_driver.sh`, `bench.sh`, `unload.sh` | Build + test helpers. |
| `docs/` | Status, open questions, Ghidra findings, decompiled extracts. |
| `drivers/windows/trx50/` | AMD Windows binaries (.sys / .inf / .cat) used as primary RE input. |
| `drivers/reference/` | Third-party reference material kept under its own licenses; not compiled into the driver. |
| `scripts/ghidra/` | Headless-Ghidra scripts used to extract the structure information that informs the driver. |

## Maintainership

Maintained by Joey Troy (`@joeytroy`). Long-term goal is mainline
Linux kernel inclusion. Contributions welcome via pull request;
please add yourself to the contributor list and sign off your commits
(`git commit -s`).

## A note for AMD

This project is an interoperability driver for hardware AMD has sold,
so Linux owners of that hardware can access their data. It does not
redistribute AMD's binaries beyond what is necessary for the
development team's reference, and we do not link against AMD's
proprietary core. We would be happy to coordinate with AMD on
documentation, hardware references, or any concerns — please open
an issue.
