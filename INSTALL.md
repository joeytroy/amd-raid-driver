# AMD RAID Driver — Build and Test

> **Status (2026-07-10)**: the driver is a working daily driver for
> **RAID0 and RAID1** — both hardware-validated up to installing and
> booting Linux from the array.  This document covers the
> **development setup** (build loop, manual testing, troubleshooting);
> for the user-facing install flows (live-USB install onto the array,
> DKMS install) see the **README quick start**.  See `docs/STATUS.md`
> for the full state, `docs/REVERSE_ENGINEERING.md` for the
> reverse-engineering background, and `RE_METHODOLOGY.md` at the
> repository root for the license and legal record.

The fastest, safest setup is described first. The Live USB approach is
kept at the bottom as a fallback for when no separate Linux drive is
available.

---

## Recommended setup: Kubuntu on a non-RAID drive

This is the path with the lowest friction and zero risk of corrupting
the RAID data. You boot a permanent Linux install from a drive that
isn't behind the AMD RAID controller, leave the Crucial RAID0 (or
whatever you have on the controller) untouched, and iterate on
`rcraid.ko` from there.

### What you need

- **Kubuntu 24.04 LTS** installed on a non-RAID drive (e.g. a Samsung
  NVMe in a slot that bypasses the AMD RAID controller).
- BIOS still configured with **SATA / NVMe mode = RAID**. This is what
  makes the controller expose itself as `1022:B000`. If you switched
  BIOS to AHCI, the underlying drives appear as raw NVMe / SATA and
  this driver is not relevant.
- The Crucial NVMe drives (or whatever you have in the RAID array)
  configured into a RAID volume from the BIOS RAID utility. Don't
  reconfigure the volume during driver development — losing the
  metadata would scrub the array.

### Boot setup (one-time)

When BIOS is in RAID mode but the kernel has both the AHCI driver and
this driver compiled in, you want the AHCI driver disabled so it
doesn't grab the AMD controller before `rcraid.ko` does. Add to the
kernel command line via GRUB:

```bash
sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 modprobe.blacklist=ahci,sata_ahci"/' /etc/default/grub
sudo update-grub
sudo reboot
```

After reboot, verify:

```bash
lspci -nn | grep -iE 'raid|non-volatile'
# Should list something like:
#   46:00.0 Non-Volatile memory controller [0108]: Advanced Micro Devices [AMD] Device [1022:b000]
# Plus your Samsung OS drive as a separate NVMe.
```

Do **not** blacklist `nvme` — the kernel still uses it for the Samsung
OS drive, and `test_driver.sh` relies on `nvme` being available so it
can unbind only the AMD controller while leaving Samsung alone.

### One-time package install

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    linux-headers-$(uname -r) \
    git \
    flex bison libssl-dev libelf-dev dwarves
```

### Get the source

```bash
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
```

If you already have the repo: `git pull`.

### The iteration loop

```bash
sudo ./build.sh         # builds rcraid.ko (uses Makefile under the hood)
sudo ./test_driver.sh   # loads the driver, runs the full check battery,
                        # writes driver_diagnostics_<ts>.txt, prompts to unload
```

Read the new **TEST 4.6 (NVMe init check)** first — it tells you in
plain English whether:

- The driver routed to the NVMe path (mode=2) vs accidentally falling
  back to AHCI / stub.
- `CAP` was readable.
- `AQA / ASQ / ACQ` got programmed and the read-back matched.
- The controller actually reached `CSTS.RDY = 1`.

Between iterations:

```bash
sudo ./unload.sh        # only if you skipped the script's unload prompt
```

### Useful live monitoring

In a second terminal during the test:

```bash
dmesg -w | grep -iE 'rc_|rcraid|rcbottom'
```

Or, once the driver is loaded:

```bash
watch -n1 'cat /sys/bus/pci/drivers/rcbottom/*/rcraid/queue_stats'
```

### What you should see

- **`/dev/rcraid0`** (plus `/dev/rcraid0pN` for any partitions) once the
  volume assembles — check `lsblk /dev/rcraid0` and
  `dmesg | grep rc_volume` for the assembly log (committed config
  generation, RAID level, capacity).
- **`lspci -k` showing `Kernel driver in use: rcbottom`** for every
  array member.
- Writes only when loaded with `enable_writes=1` (the installers set
  it; a bare `insmod` stays read-only).

---

## Safety notes

### Don't develop kernel code inside a VM

VMware / VirtualBox / QEMU guests don't see the host's PCI devices
unless you do passthrough, and you can't pass through hardware the host
is itself using. So:

- ✅ Edit code in a VM — fine, share the folder back to the host.
- ✅ Compile in a VM — fine if the kernel version matches the host's.
- ❌ `insmod rcraid.ko` in a VM — module loads but binds to nothing.
- ❌ `test_driver.sh` in a VM — every hardware check fails.

The build-and-test loop has to run on the bare-metal Kubuntu install.

### `test_driver.sh` is safe by design

Two things to know:

- It unbinds **only** PCI devices matching `1022:b000` (the AMD RAID
  controller). It can never touch your Samsung OS drive, even if the
  Samsung-protection branch is irrelevant to your model.
- It does no writes against the RAID volume: its checks are read-only,
  and it loads the driver without `enable_writes=1`, so the volume
  itself is read-only for the whole run.

### Kernel oopses still hang you

We're writing to MMIO on real hardware. If the driver does something
the controller dislikes, the machine may freeze and need a hard reboot.
Commit your code before each test run; don't leave unsaved work in
editors.

---

## After a kernel update

`linux-headers-*` for your new kernel must be installed, then rebuild:

```bash
sudo apt install -y linux-headers-$(uname -r)
cd ~/amd-raid-driver
sudo ./build.sh
```

The Makefile picks up the running kernel's headers automatically. No
chroot or initramfs gymnastics are needed while the driver is still in
development (you're loading it manually with `insmod` each time, not
booting from it).

---

## Troubleshooting

### Secure Boot: `insmod` fails with `Key was rejected by service`

Secure Boot is enabled. `rcraid.ko` is unsigned, so the lockdown kernel
refuses to load it (`dmesg` shows `Loading of unsigned module is
rejected`). Both installers (`install-livecd.sh` and `install-dkms.sh`)
check for this up front and abort with instructions before building
anything. Watch out for BIOS resets (CMOS clear, firmware update,
"load optimized defaults"): they silently re-enable Secure Boot.

**Fix: disable Secure Boot in BIOS setup — and keep it off.** DKMS
rebuilds the module unsigned on every kernel update, so re-enabling
Secure Boot later stops the array from coming up at boot; if the OS
lives on the array, the machine won't boot at all.

If you must run with Secure Boot on, sign the module with your own
Machine Owner Key and bypass the installer check with
`RCRAID_ALLOW_SECURE_BOOT=1`:

```bash
# One-time: create and enrol a signing key.
openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
    -subj "/CN=rcraid module signing/" \
    -keyout MOK.priv -outform DER -out MOK.der
sudo mokutil --import MOK.der   # set a one-time password; on the next
                                # reboot the MOK manager prompts you to
                                # enrol the key with that password

# After every build (including every DKMS rebuild), sign the module the
# kernel actually loads — DKMS installs it under updates/dkms, not the
# build directory you happen to be standing in:
MODULE="/lib/modules/$(uname -r)/updates/dkms/rcraid.ko"

# Debian/Ubuntu header layout:
sudo "/usr/src/linux-headers-$(uname -r)/scripts/sign-file" \
    sha256 MOK.priv MOK.der "$MODULE"
# Fedora/RHEL layout:
sudo "/usr/src/kernels/$(uname -r)/scripts/sign-file" \
    sha256 MOK.priv MOK.der "$MODULE"

# If the OS boots from the array, the initramfs carries its own copy of
# the module — regenerate it so the copy loaded at boot is the signed one:
sudo update-initramfs -u    # Debian/Ubuntu
sudo dracut -f              # Fedora/RHEL
```

Automatic signing of DKMS rebuilds (the dkms `sign_tool` hook) is not
wired up yet, so each kernel update needs a manual re-sign — which is
why disabling Secure Boot remains the supported path.

### Driver builds but `lspci` shows the device with no driver bound

Check that BIOS is in **RAID mode**, not AHCI. In AHCI mode the
controller doesn't expose itself as `1022:B000` at all.

### Build fails on Debian-family kernels newer than 6.8

```bash
sudo make clean
sudo make simple    # less strict warning set, kept for compatibility
```

Paste the build log if it still fails — there may be a kernel API
change to deal with.

### `insmod` fails with `No such device`

The driver loaded but the PCI table doesn't match. Confirm:

```bash
lspci -d 1022: -nn
```

Both `1022:43bd` (AHCI variant) and `1022:b000` (NVMe variant) should
be claimed by the driver via the table in `rc_bottom.c`. If your
device ID is something else, it's not currently supported.

### Driver loads, NVMe init runs, but `ASQ/ACQ` read-back mismatch

That means we wrote the right registers but they didn't stick — likely
the controller needs a vendor-specific config write before MMIO comes
up. See the low-priority open questions (BAR discovery, vendor PCI
config writes) in `docs/REVERSE_ENGINEERING.md`. Capture the full dmesg
and paste it into a new development session.

### "I just want to compare against the working Windows driver"

The Windows binaries were removed from the tree (vendor-distributed
material) — restore them from git history with
`git checkout 69738ee -- drivers/`: `drivers/windows/trx50/9.3.3-00291/`
(CVE patched) and `9.3.2-00255/` (the version all Ghidra docs
reference). Use Ghidra headless via the script in
`scripts/ghidra/HuntBlockers.java`. See `docs/README.md` for the
command line.

---

## Alternative: Live USB (fallback)

Use this path only if you don't have a usable Linux install on a
non-RAID drive — for example, on a fresh system where Windows still
owns the RAID and you have nowhere else to boot Linux from.

### Create the USB

1. Download **Kubuntu 24.04 LTS** ISO.
2. Burn to USB with Rufus (Windows) or `dd` / Ventoy (Linux).

### Modify GRUB on the USB

Edit `USB:/boot/grub/grub.cfg` and `USB:/boot/grub/loopback.cfg`. For
each `linux /casper/vmlinuz …` line, append at the **end**:

```
modprobe.blacklist=ahci,sata_ahci
```

This stops the kernel AHCI driver from grabbing the controller while
the Live USB is booting. **Do not blacklist `nvme`** — this driver
relies on the kernel `nvme` driver being loaded (it unbinds the AMD
device from it, then takes over).

### Boot and build

After booting the Live USB:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git \
    flex bison libssl-dev libelf-dev dwarves

git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo cp /sys/kernel/btf/vmlinux /usr/lib/modules/$(uname -r)/build/ 2>/dev/null || true

sudo ./build.sh
sudo ./test_driver.sh
```

Changes you make to the source on the Live USB session don't persist
across reboots unless you set up persistence on the USB. Push to git or
copy to external storage before powering down.

To actually **install Linux onto the array** from a live session, don't
do it by hand — run `sudo ./install-livecd.sh` and follow the README's
quick-start path 🅐. It builds and loads the driver, hands
`/dev/rcraid0` to the distro installer, then chroots into the fresh
install to set up DKMS, the initramfs hook, and a UEFI boot entry.
Validated end to end on RAID0 and RAID1.
