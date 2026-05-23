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
- Writes gated behind `enable_writes=1` module parameter (off by
  default for safety; load with the param to allow).
- Bench throughput on a 2-member Crucial T700 RAID0 dev box:
  ~1.1 GB/s @ `bs=64K`, scaling up to ~4.7 GB/s at `bs=4M`.

See `docs/STATUS.md` for the full state and the next-steps list.

## What's NOT here yet

- RAID levels other than RAID0 (RAID1 / 5 / 10 are roadmap).
- SATA RAID (`1022:43F6` and the AHCI variants `7905 / 7916 / 7917 /
  43BD`) — code paths exist but are stubs.
- Suspend / resume hooks.
- `REQ_OP_FLUSH` / FUA propagation to NVMe FLUSH (0x00).
- Interrupt-driven completion (currently polled).
- `rcadm`-equivalent userspace tooling.

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
