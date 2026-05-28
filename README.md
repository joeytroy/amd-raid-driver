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
  of in-flight requests when a controller dies, automatic controller
  reset on first timeout per death episode, and manual reset via
  `echo 1 > /sys/bus/pci/devices/<bdf>/rcraid/reset` as the fallback
  when auto-reset itself fails.  See `docs/ERROR_HANDLING.md`.
- Scatterlist-native DMA: hardware reads/writes the bio's user pages
  directly via `dma_map_sg` + PRP enumeration — no bounce buffer, no
  memcpy on either side.  Drops ~33 MiB of pinned per-tag buffers vs
  the prior path.
- Bench throughput on a 2-member Crucial T700 RAID0 dev box:
  ~2.1 GB/s @ `bs=64K`, ~6.7 GB/s @ `bs=1M`, ~11.9 GB/s aggregate
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

The big rocks (see `IMPLEMENTATION.MD` for the full checklist):

- **RAID levels other than RAID0** — RAID1 / 10 are roadmap; RAID5 is not.
- **No DKMS / udev autobind** — every boot, the drives come up under
  `nvme`; you re-run the unbind + insmod sequence by hand.
- **No Secure Boot signing** — module is unsigned; SB must be off.
- **No suspend / resume** hooks.
- **SATA RAID stubs** — AHCI variants (`7905 / 7916 / 7917 / 43BD`)
  are claimed but not implemented.
- **No `rcadm`-equivalent** — array must be pre-created in BIOS.
- **Retry of transient NVMe errors** — DNR=0 commands bubble up as
  I/O errors instead of being re-dispatched after reset.

See `IMPLEMENTATION.MD` for the prioritised checklist,
`docs/OPEN_QUESTIONS.md` for remaining reverse-engineering work, and
`docs/STATUS.md` for the implementation history.

## Quick start

Working assumption: BIOS is in RAID mode and you've already created
the array in RAIDXpert. If not, see `INSTALL.md` for the one-time BIOS
setup.

### Prerequisites

- **Secure Boot disabled** (or the module signed — see `INSTALL.md`).
  Without this, `insmod` returns `Key was rejected by service`.
- Kernel headers for the running kernel:
  `sudo apt install build-essential linux-headers-$(uname -r)`.

### 1. Build

```sh
sudo ./build.sh
# produces rcraid.ko
```

### 2. Find your AMD-RAID members

```sh
lspci -d 1022:b000 -k
```

You'll see one or more `[AMD] RAID Bottom Device` entries bound to
`nvme`. Note the BDFs (e.g. `81:00.0`, `82:00.0`).

**Critical**: if your OS lives on an NVMe behind the same AMD chipset,
it will also appear as `1022:b000` (the chipset shadows every NVMe). Use
the `Subsystem:` line to identify it (Samsung vs. Crucial vs. WD etc.)
and **do not unbind your OS drive**.

### 3. Unbind RAID members from `nvme`, then load `rcraid`

```sh
# For each member you want under rcraid (skip the OS drive):
echo 0000:81:00.0 | sudo tee /sys/bus/pci/drivers/nvme/unbind
echo 0000:82:00.0 | sudo tee /sys/bus/pci/drivers/nvme/unbind

# Read-only mode (safest first run):
sudo insmod ./rcraid.ko

# Or read-write:
sudo insmod ./rcraid.ko enable_writes=1
```

If your OS drive's subsystem vendor differs from the array members,
you can also use `safe_subsys_vendor=0x<hex>` to make the driver refuse
to bind anything whose subsystem vendor doesn't match the array — see
`INSTALL.md`.

### 4. Verify the array came up

```sh
sudo dmesg | grep -E 'rcraid|rc_volume' | tail -20
# Look for: rc_volume_create_disk: /dev/rcraid0 up, ... (read-write)
# And:      rc_init: Found N adapters

lsblk /dev/rcraid0
ls -l /dev/rcraid0
```

### 5. Mount it

**Whole-disk filesystem** (recommended on a fresh array):

```sh
sudo mkfs.xfs -f -d su=256k,sw=<num_members> /dev/rcraid0
sudo mkdir -p /mnt/rcraid0
sudo mount -o noatime /dev/rcraid0 /mnt/rcraid0
```

**If the array has a GPT** (Windows install, etc.) the kernel
auto-scans on `add_disk` and partitions appear as `/dev/rcraid0p1`,
`/dev/rcraid0p2`, … — mount directly:

```sh
lsblk /dev/rcraid0
sudo mount /dev/rcraid0pN /mnt/somewhere
```

### 6. Unload

```sh
sudo umount /mnt/rcraid0           # or kpartx -d /dev/rcraid0 first
sudo rmmod rcraid
```

After `rmmod`, the kernel won't auto-rebind the drives to `nvme` —
they'll sit unbound until you `echo 0000:XX:00.0 > /sys/bus/pci/drivers/nvme/bind`
or reboot.

### Caveats today (read this once)

- **Every reboot**: drives come up bound to `nvme`. You have to repeat
  step 3. A persistent udev/systemd setup is on the roadmap
  (see `IMPLEMENTATION.MD`).
- **Secure Boot**: must be disabled, or the module signed and the cert
  enrolled in MOK. Tracked in `IMPLEMENTATION.MD`.

Full setup, troubleshooting, and the Live-USB fallback path are in
`INSTALL.md`. The road from here to "no manual steps" is tracked in
`IMPLEMENTATION.MD`.

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
