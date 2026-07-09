# rcraid — AMD-RAID driver for Linux
[![Donate via PayPal](https://img.shields.io/badge/Donate-PayPal-blue.svg)](https://www.paypal.com/paypalme/joeytroynm)

A clean-room Linux kernel module that lets you read and write your
**AMD-RAID NVMe arrays** natively — no Windows, no proprietary blob.

If your motherboard is in RAID mode and you built an array in RAIDXpert,
`rcraid` assembles it into an ordinary block device at `/dev/rcraid0`
that you can partition, format, mount, and even **boot Linux from**.

> ⚠️ **Read the [safety notes](#safety-first) before loading the driver.**
> The AMD chipset makes *every* NVMe look like the same PCI device, so
> the driver has guardrails to keep it off your OS disk — but you must
> point it at the right drives.

---

## Is my hardware supported?

**Yes, if you have an AMD-RAID NVMe array** on any recent AMD platform —
Ryzen 7000+, Ryzen AI 300 / AI Max, Threadripper 7000 / 9000 (TRX50 /
WRX90), or the X870 / B850 / X670 / B650 chipsets. They all present the
NVMe RAID controller as PCI `1022:B000`, which is fully implemented here.

| PCI ID | Controller | Status |
|--------|-----------|--------|
| `1022:B000` | NVMe RAID Bottom (TRX50, WRX90, X870/X670, B850/B650, …) | ✅ **RAID0 read/write, boot-from-array validated** |
| `1022:43BD` `7905` `7916` `7917` | SATA RAID (Promontory / older) | ⚙️ Claimed but stubbed — not yet implemented |

**RAID level:** RAID0 works today. RAID1 / RAID10 are on the roadmap.
RAID5 is not planned (AMD only supports it on 3rd-gen Threadripper).

---

## What works today

- **RAID0 volumes, read *and* write** at `/dev/rcraid0` — member count,
  member order, stripe size, and capacity are all read from the on-disk
  RAIDXpert metadata. Nothing is hardcoded.
- **Boot Linux from the array.** Validated end to end: Kubuntu 24.04
  (HWE kernel 6.17) installed onto and booting from a 2× Crucial T700
  RAID0 array, with DKMS rebuilds and an initramfs hook that brings the
  array up before `pivot_root`.
- **Filesystem-safe.** FLUSH, FUA, and DISCARD/TRIM are all wired up, so
  `fsync`, journaling, and `fstrim` behave correctly.
- **Fast — and symmetric.** ~19.7 GB/s read / ~18.7 GB/s write on that
  2-drive array (KDiskMark 3.3.0, SEQ1M Q8T1). Interrupt-driven async completion
  with a scatterlist-native DMA path — the hardware reads and writes your
  pages directly, no bounce buffers or memcpy.
- **Resilient.** 30 s command timeouts, controller health tracking,
  best-effort NVMe Abort, and automatic controller reset on the first
  timeout — with a manual sysfs reset as a fallback.

Writes are **off by the module's default** (`enable_writes=1` to opt in) —
the `install-livecd.sh` and DKMS installers turn writes on for you; the
read-only default only applies to a bare manual `insmod`.

<p align="center">
  <img src="image/kdiskmark.png" width="480" alt="KDiskMark on /dev/rcraid0: 19,730 MB/s read, 18,715 MB/s write (SEQ1M Q8T1)"><br>
  <sub><em>KDiskMark 3.3.0 on <code>/dev/rcraid0</code> — RAID0 across two Crucial T700 NVMe SSDs (PCIe 5.0) on a TRX50 motherboard.</em></sub>
</p>

---

## Safety first

Two things protect your data and your OS install. Understand both:

1. **The driver is read-only unless writes are enabled with
   `enable_writes=1`.** The install scripts enable writes for you; a bare
   manual `insmod` stays read-only. On the manual path, do your first run
   read-only and confirm the array assembles correctly before enabling
   writes.

2. **Your OS drive can masquerade as an array member.** The AMD chipset
   shadows every NVMe as PCI `1022:b000`, including your boot SSD. The
   driver refuses to touch a disk whose `subsystem_vendor` doesn't match
   the array (the `safe_subsys_vendor` parameter), and the install
   scripts set this for you — but if you bind drives by hand, **never
   unbind your OS drive.** Identify members by their `Subsystem:` line
   (Crucial vs. Samsung vs. WD, etc.).

Also note: the module is **unsigned**, so **Secure Boot must be off**
(or you sign the module yourself and enrol the key — see `INSTALL.md`).

---

## Quick start

**Prerequisites**
- BIOS in RAID mode, array already created in RAIDXpert (see `INSTALL.md`
  for the one-time BIOS setup).
- Secure Boot disabled.
- Kernel **≥ 6.15** (on Ubuntu 24.04 that means the HWE stack, 6.17+ —
  the 6.8 GA kernel is too old).
- `sudo apt install build-essential linux-headers-$(uname -r)`

Pick the path that matches what you're doing:

### 🅐 Install Linux *onto* the array (from a live USB)

This is the recommended path for a fresh array. `install-livecd.sh` runs
in two phases around the normal OS installer — you don't reboot until the
very end. Full walkthrough:

**1. Boot the live USB and enter the live desktop.**
Flash a stock Ubuntu / Kubuntu / Debian / Fedora ISO, boot it, and choose
**Try** (the live session) — *not* Install yet. Connect to the network.

**2. Build and load the driver.**

```sh
sudo apt install -y git          # or: sudo dnf install -y git
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo ./install-livecd.sh
```

The script installs build tools, compiles the module against the live
kernel, auto-detects your array members, binds them, and loads `rcraid`.
When it prints that **`/dev/rcraid0` is up**, it pauses and waits for you.

**3. Run the OS installer — leave this terminal open.**
Launch the distro's installer from the live desktop (*Install Kubuntu*,
etc.) and:

- ✅ **Tick "Download updates while installing."** Keep the machine online
  for the whole run — in our testing this is what keeps the installer from
  restarting the session and dropping the loaded driver (and the array)
  partway through the install.
- Point the install at **`/dev/rcraid0`**. The simplest option is
  **"Erase disk and install"** with `/dev/rcraid0` selected as the target
  — the installer auto-partitions it and phase 2 picks up the result. (Use
  Manual/Custom instead only if you want a specific layout.) Just make sure
  the disk you erase is `/dev/rcraid0`, **not** your OS drive or the live
  USB.
- Let it finish, but when it offers to reboot, **do *not* reboot.** Close
  the installer and return to the terminal.

**4. Press Enter to finish.**
Back in the terminal, press Enter. Phase 2 finds your new rootfs on
`/dev/rcraid0pN`, chroots in, installs DKMS + the initramfs hook against
the *target* kernel, and writes a UEFI boot entry (plus an
`\EFI\BOOT\BOOTX64.EFI` fallback, since installer-created entries often
don't stick on RAIDXpert firmware).

**5. Reboot.** `sudo reboot` — the system now boots from the array on its
own. If the first boot misbehaves, hit `e` in GRUB and add
`rd.driver.blacklist=nvme` to prove the rcraid path, then revert.

### 🅑 Mount an array from an existing Linux install (DKMS, persistent)

Survives kernel updates and comes up automatically at every boot:

```sh
sudo apt install dkms
sudo ./install-dkms.sh
```

It detects your array members (vs. your OS drive), installs the module
via DKMS, pins the members to the driver with a udev rule, enables writes
by default, and adds an initramfs hook. Reboot and `/dev/rcraid0` (plus
any `/dev/rcraid0pN` partitions) is just there. `sudo ./uninstall-dkms.sh`
reverses all of it.

### 🅒 Build and load by hand (dev iteration / one-off)

<details>
<summary>Manual unbind / insmod steps</summary>

```sh
sudo ./build.sh                        # produces rcraid.ko

lspci -d 1022:b000 -k                   # find your members' BDFs
                                        # ⚠️ do NOT include your OS drive

# Unbind each array member from nvme:
echo 0000:81:00.0 | sudo tee /sys/bus/pci/drivers/nvme/unbind
echo 0000:82:00.0 | sudo tee /sys/bus/pci/drivers/nvme/unbind

sudo insmod ./rcraid.ko                 # read-only (safest first run)
sudo insmod ./rcraid.ko enable_writes=1 # read-write

# Confirm it came up:
sudo dmesg | grep -E 'rcraid|rc_volume' | tail -20
lsblk /dev/rcraid0
```

Then format/mount as any disk, e.g. a fresh whole-disk XFS:

```sh
sudo mkfs.xfs -f -d su=256k,sw=<num_members> /dev/rcraid0
sudo mount -o noatime /dev/rcraid0 /mnt/rcraid0
```

Unload with `sudo umount … && sudo rmmod rcraid`. Note the manual path
must be repeated every reboot (path 🅑 automates it), and after `rmmod`
the drives stay unbound until you rebind them to `nvme` or reboot.

</details>

Full setup, troubleshooting, and the Secure-Boot / signing details are in
**[`INSTALL.md`](INSTALL.md)**.

---

## Not yet supported

- **RAID1 / RAID10** — roadmap. **RAID5** — not planned.
- **SATA RAID** — the `43BD / 7905 / 7916 / 7917` controllers are claimed
  but the AHCI path is a stub.
- **No array management** — create/modify the array in BIOS/RAIDXpert.
- **No Secure Boot signing** out of the box.
- **No retry of transient NVMe errors** — a failed command surfaces as an
  I/O error (`BLK_STS_IOERR`) even when the drive reports it as retryable
  (DNR=0); it isn't re-dispatched. Controller-level failures still trigger
  the automatic reset described above.

The prioritized checklist lives in
[`docs/IMPLEMENTATION.MD`](docs/IMPLEMENTATION.MD).

---

## For developers

This is a clean-room driver reverse-engineered from AMD's publicly
distributed Windows binaries under DMCA §1201(f) interoperability
protections — **no AMD source code is used**. Contributions welcome:
open a PR, add yourself to the contributor list, and sign off your
commits (`git commit -s`). The long-term goal is mainline kernel
inclusion.

| Doc | What's in it |
|-----|--------------|
| [`docs/STATUS.md`](docs/STATUS.md) | Implementation history — how each piece was built |
| [`docs/IMPLEMENTATION.MD`](docs/IMPLEMENTATION.MD) | Prioritized checklist toward a daily-driver release |
| [`docs/ERROR_HANDLING.md`](docs/ERROR_HANDLING.md) | Timeout / reset / dead-controller design |
| [`docs/OPEN_QUESTIONS.md`](docs/OPEN_QUESTIONS.md) | Remaining reverse-engineering unknowns |
| [`RE_METHODOLOGY.md`](RE_METHODOLOGY.md) | Clean-room process and legal record |

**Repository layout**

| Path | What's there |
|------|--------------|
| `rc_*.c`, `rc_*.h` | The driver (SPDX `GPL-2.0-only`) |
| `Makefile`, `build.sh`, `test_driver.sh`, `bench.sh`, `unload.sh` | Build + test helpers |
| `docs/` | Status, open questions, Ghidra findings, decompiled extracts |
| `drivers/windows/trx50/` | AMD Windows binaries used as RE input |
| `drivers/reference/` | Third-party reference material (own licenses; not compiled in) |
| `scripts/ghidra/` | Headless-Ghidra extraction scripts |

---

## License

**GPL-2.0-only** — see [`LICENSE`](LICENSE). Maintained by Joey Troy
(`@joeytroy`).

The PCI IDs, on-disk metadata layouts, and protocol behaviour are derived
from analysis of AMD's publicly distributed Windows drivers
(`rcbottom.sys`, `rcraid.sys`, `rccfg.sys`) under DMCA §1201(f). No AMD
source is incorporated.

### A note for AMD

This is an interoperability driver for hardware you sold, so Linux owners
can access their own data. It doesn't redistribute your binaries beyond
what the dev team needs as reference, and it doesn't link against your
proprietary core. We'd be glad to coordinate on documentation or hardware
references — please open an issue.
