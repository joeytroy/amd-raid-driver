# AMD RAID Driver — Build and Test

> **Status (2026-07-10)**: the driver is a working daily driver for
> **RAID0 and RAID1** — both hardware-validated up to installing and
> booting Linux from the array.  This document covers the
> **development setup** (build loop, manual testing, troubleshooting);
> for the user-facing install flows (live-USB install onto the array,
> DKMS install) see the **README quick start**.  See `docs/STATUS.md`
> for the full state, `docs/REVERSE_ENGINEERING.md` for the
> reverse-engineering background, and `docs/RE_METHODOLOGY.md`
> for the license and legal record.

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
can unbind only the detected array members while leaving the OS drive
alone.

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

Three things to know:

- It detects the array by **subsystem vendor** (the same logic the
  installers use — the vendor with ≥ 2 devices behind `1022:b000`) and
  unbinds **only** devices matching that vendor. The same vendor is
  passed to `insmod` as `safe_subsys_vendor=`. Treat that as a coarse
  filter only: when every drive comes from one OEM the value matches the
  OS disk as well and discriminates nothing (issue #8).
- Independently of the vendor filter, it resolves which PCI device(s)
  back the running root filesystem and **refuses to unbind them under
  any circumstances** — whatever brand your OS drive is.
- The packaged udev rule does not rely on the vendor at all: it fires on
  the namespace and `rcraid-bind` only proceeds when the RAIDCore
  metadata at LBA `0x5000` is present, which a non-member disk does not
  have.
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

If the machine **boots from the array** (DKMS install), verify boot
safety after every kernel update and **before rebooting**:

```bash
sudo scripts/verify-boot-safety.sh
```

It checks every installed kernel for headers, a successful DKMS build,
the module on disk, and — decisively — the module inside that kernel's
initramfs. It supports both initramfs-tools (Debian/Ubuntu,
`lsinitramfs`) and dracut (Fedora/RHEL/Arch, `lsinitrd`) layouts, prints
the exact fix command for any kernel that fails, and exits non-zero if
any installed kernel would not boot. It also warns when a MOK enrolment is
still queued: the next boot will stop at the MOK prompt, and until it is
answered Secure Boot does not trust the signed module. Both the DKMS and the
live-USB installer also drop `/etc/apt/apt.conf.d/52-rcraid-kernel-hold`, so
unattended-upgrades cannot install a kernel whose rebuild nobody watched. Note the DKMS installer also drops
`/etc/apt/apt.conf.d/52-rcraid-kernel-hold` on apt systems so
unattended-upgrades never installs a kernel behind your back.

---

## Troubleshooting

### Secure Boot: `insmod` fails with `Key was rejected by service`

Secure Boot is enabled and the module is unsigned, so the lockdown kernel
refuses it (`dmesg` shows `Loading of unsigned module is rejected`).
This usually appears right after a BIOS reset (CMOS clear, firmware
update, "load optimized defaults") — all of which silently re-enable
Secure Boot.

**Fix: sign the module with a Machine Owner Key the firmware trusts**, or
turn Secure Boot off.

Signing is split across a reboot on purpose, and it is the reboot the install
needs anyway. At install time the script puts a key pair on disk, points DKMS
at it (`/etc/dkms/framework.conf.d/rcraid-signing.conf`, so kernel-update
rebuilds are signed too) and queues a MOK enrolment. The first boot after the
install completes it: shim's MOK manager runs **before** the kernel, so by the
time the initramfs loads `rcraid.ko` the key is already trusted. There is no
"enrol, reboot, install, reboot" round trip.

What that looks like:

- `install-dkms.sh` prints a one-time password and names the key it used.
- At the next boot, shim shows "MOK management": **Enroll MOK → Continue →
  Yes**, type the password, and let it reboot.
- If a key is already enrolled — the usual case on Ubuntu/Debian, where
  `update-secureboot-policy` has created the shim key — it is reused, with no
  prompt and no password.

Skipping the prompt leaves the key untrusted, so the kernel rejects
`rcraid.ko` and the array is missing at that boot. If `/` lives on the array,
that boot cannot complete: answer the prompt (it comes back until it is
answered, and `sudo mokutil --import <cert>` re-queues it), or switch Secure
Boot off to get in.

The prompt does not have to be answered with Secure Boot on, and the safe order
is to do it with Secure Boot still **off**: shim launches MokManager whenever a
request is pending, whatever the firmware is enforcing, and with enforcement
off a rejected module costs nothing — the array still comes up. Enrol there,
then switch Secure Boot on. The installers disable MokManager's ~10 s default
timeout (`mokutil --timeout -1`) so a slow boot cannot silently drop the
request, which is what MokManager otherwise does.

All of this assumes the boot chain goes through shim, the default on Ubuntu,
Debian and Fedora. A kernel started directly by firmware through its own EFI
stub never runs MOK manager, and there Secure Boot has to stay off.

`install-dkms.sh` verifies the signature of what DKMS installed, and refuses to
finish when Secure Boot is enforcing but signing could not be arranged (missing
`mokutil`/`openssl`, a DKMS too old for `framework.conf.d`, a firmware that
rejects the request) — that failure belongs at install time, not at the next
boot. The knobs:

| Variable | Effect |
|---|---|
| `RCRAID_MOK_KEY` / `RCRAID_MOK_CERT` | sign with this pair instead of the discovered one (DER certificate) |
| `RCRAID_MOK_PASSWORD` | use this one-time enrolment password instead of a generated one |
| `RCRAID_MOK_ENROLL=0` / `1` | skip / force the key enrolment without asking: in `install-livecd.sh` for the target system, in `install-dkms.sh` for the machine it runs on — the latter is how a system installed with Secure Boot off arranges signing afterwards (`README.md`, phase 3) |
| `RCRAID_ALLOW_SECURE_BOOT=1` | install anyway, unsigned — you sign by hand |

### Live USB and Secure Boot

The live session cannot run with Secure Boot on: the live kernel refuses an
unsigned out-of-tree module, and the key that would sign it can only be
enrolled by rebooting the live session — which throws away the session and the
key with it. `install-livecd.sh` stops and says so, instead of failing at
`insmod` after the disks have already been handed to the distro installer.

The sequence that works:

1. Switch Secure Boot off in firmware setup.
2. Run `sudo ./install-livecd.sh`. It asks whether the installed system should
   get its own signing key (default yes) — say yes if you intend to switch
   Secure Boot back on later.
3. Let the first boot finish: shim asks for the MOK password you typed at the
   installer's prompt, exactly as in the DKMS flow above.
4. Only then re-enable Secure Boot.

If you never plan to enable Secure Boot, answer no (or set
`RCRAID_MOK_ENROLL=0`) and nothing is created, signed, or prompted.

### Signing by hand

A plain `make` build (outside DKMS) is not signed for you, so that path
still needs one step per build:

```bash
MODULE="./rcraid.ko"
# Debian/Ubuntu header layout:
sudo "/usr/src/linux-headers-$(uname -r)/scripts/sign-file" \
    sha256 MOK.priv MOK.der "$MODULE"
# Fedora/RHEL layout:
sudo "/usr/src/kernels/$(uname -r)/scripts/sign-file" \
    sha256 MOK.priv MOK.der "$MODULE"

modinfo -F signer "$MODULE"    # must name your key

# and enrol the certificate the way the installers do:
sudo mokutil --import MOK.der  # one-time password; answer it at the next boot
```

To create a pair yourself — only needed when you want a key of your own, since
`install-dkms.sh` creates one when Secure Boot is on and nothing is enrolled,
or when run as `sudo RCRAID_MOK_ENROLL=1 ./install-dkms.sh`:

```bash
openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
    -subj "/CN=rcraid module signing/" \
    -addext "extendedKeyUsage=codeSigning" \
    -keyout MOK.priv -outform DER -out MOK.der
```

Keep `MOK.priv` somewhere permanent: an enrolled certificate whose private key
is gone cannot sign anything, and the enrolment cannot be reused. Put the pair
in `/var/lib/shim-signed/mok/` to have the installers find it automatically.
`uninstall-dkms.sh` removes rcraid's DKMS drop-in but leaves the key pair alone
— it is enrolled in firmware, and other modules may be signed with it too.

If the OS boots from the array, the initramfs carries its own copy of
the module — regenerate it so the copy loaded at boot is the signed one
(`sudo update-initramfs -u`, or `sudo dracut -f`).

Because the DKMS tree is separate from the build directory, the module
the kernel actually loads for an installed driver is
`/lib/modules/$(uname -r)/updates/dkms/rcraid.ko` — signing `./rcraid.ko`
only affects a hand-loaded module.

### Driver builds but `lspci` shows the device with no driver bound

Check that BIOS is in **RAID mode**, not AHCI. In AHCI mode the
controller doesn't expose itself as `1022:B000` at all.

### Build fails on an older distro kernel (below 6.15)

The driver uses block-layer APIs that only exist on modern kernels — it
targets Linux 7.x and needs at least 6.15 (the 2-argument
`blk_rq_map_sg()` landed then). On Ubuntu 24.04 that means the HWE stack,
not the 6.8 GA kernel. If an otherwise-modern kernel still fails:

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

Only `1022:b000` (NVMe variant) is claimed by the driver via the table in
`rc_bottom.c`.  The SATA-RAID IDs (`43bd`, `7905`, `7916`, `7917`) are
deliberately *not* claimed — their AHCI path is unimplemented, so those
controllers stay bound to `ahci` and keep working at whatever level `ahci`
supports.  If your device ID is something else, it's not currently
supported.

### Driver loads, NVMe init runs, but `ASQ/ACQ` read-back mismatch

That means we wrote the right registers but they didn't stick — likely
the controller needs a vendor-specific config write before MMIO comes
up. See the low-priority open questions (BAR discovery, vendor PCI
config writes) in `docs/REVERSE_ENGINEERING.md`. Capture the full dmesg
and paste it into a new development session.

### `rmmod rcraid` says "Module is in use" after a live-CD install

Phase 2 of `install-livecd.sh` mounts the target root and then bind-mounts
the live session's `/dev`, `/proc`, `/sys` and `/run` into it for the
chroot. Mount events propagate, and the live session's root is a shared
peer, so those mounts also appear in every systemd service namespace. The
installer unmounts its own view of them but not the copies: the target
filesystem stays open, the block device never closes, and the module keeps
refcount 1. `rmmod rcraid` then fails with "Module rcraid is in use", and
every later attempt to mount the target reports `already mounted or mount
point busy`.

Phase 2 must therefore run in a **private mount namespace**. The installer
re-execs itself once under

```bash
unshare --mount --propagation private -- /bin/bash "$0" "$@"
```

so every mount it makes stays local to that process tree and is discarded
when it exits. Setting propagation flags around each mount is not enough:
applying `--make-rslave` after the bind is too late, and making the target
private *before* binding still leaks the target mount itself, because that
one propagated at creation. Measured against `systemd-udevd`'s namespace on
Ubuntu 26.10: 3 leaked mounts with the previous code, 1 with the flag
applied first, 0 inside the private namespace.

A missing `unshare(1)` only downgrades this to a warning, since the install
itself is unaffected — but the session then cannot unload `rcraid` until it
reboots. If you are running an older copy and hit this, the copies can be
found and unmounted per namespace:

```bash
for pid in $(lsns -t mnt -o PID -n); do
    nsenter -t "$pid" -m -- bash -c \
        'findmnt -A -n -r -o TARGET | grep "^/mnt/rcraid-target" | sort -r |
         while read -r t; do umount -l "$t"; done'
done
```

Note that `findmnt -A` from your own shell shows nothing; the mounts live in
namespaces you are not in. Cleaning them up restores the ability to mount and
write the target (which is how a driver can still be repaired in place), but
the filesystem instance that already leaked can keep the module pinned until
the next reboot — so the leak is worth preventing rather than cleaning up.

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

The chroot step runs inside a private mount namespace
(`unshare --mount --propagation private`). That is mandatory, not tidiness:
mounts made without it propagate into systemd's service namespaces and keep
the array's filesystem open after the install, leaving `rcraid` impossible to
unload. See `rmmod rcraid` under Troubleshooting for the details and the
measured leak counts.

The array itself comes from the **UEFI RAIDXpert2 utility**, and that is what
the script requires: it reads the RAIDCore metadata at LBA `0x5000` and, when
it finds it, uses exactly those disks as members and writes nothing — the
level and geometry are the firmware's, RAID 5/6 included. When no disk carries
that metadata it stops and says so. There is no disk/level menu and no
install-time array creation: the only writers in this tree
(`scripts/qemu-test/mkmeta.py`, `rxp2adm`) stamp metadata over blank disks for
the QEMU rig, which is not an install path.

Before handing `rcraid0` over, it checks `/sys/block/rcraid0/ro` and says
whether the volume is writable for the level it read. A read-only volume
is a hard stop: RAID 5/6 have no parity write path yet (issue #6), and an
untrusted geometry forces read-only too — the reason is printed from
dmesg. Recreate the array as RAID 0 or RAID 1 in the UEFI utility and
re-run.
