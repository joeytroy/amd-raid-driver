# QEMU test rig

Boots rcraid in a VM against **virtual NVMe drives carrying synthetic
RAIDCore metadata** — an array that can be created, corrupted, and
destroyed freely, unlike the dev box's RAID0 (which is the rootfs).
This is the development rig for RAID1 / failure-handling / rebuild work
and the intended foundation for CI.

```sh
# One-time: the Ubuntu kernel image is root-readable only
sudo install -m 644 /boot/vmlinuz-$(uname -r) ~/vmlinuz

./scripts/qemu-test/run-qemu-raid-test.sh --kernel ~/vmlinuz
```

Exit 0 iff the guest prints `RCRAID-TEST-PASS`. On failure the workdir
(console log, images, initramfs) is kept and its path printed.

## How it works

| Piece | Job |
|---|---|
| `mkmeta.py` | Writes what the driver's parser validates onto blank images: the RC_MetaData block at LBA 0x5000 (magic, version, ported XOR-lane-shuffle checksum) and an RC_LogicalDevice record + element array at the config ring (LBA 0x5800). |
| `guest-init.sh` | The VM's `/init` (busybox): insmods rcraid, binds every NVMe-class PCI function to rcbottom via sysfs `new_id`, verifies `/dev/rcraid0` capacity, round-trips data at start/middle/end with dropped caches, then re-checks after a module reload cycle. |
| `run-qemu-raid-test.sh` | Host driver: builds the module against the running kernel, makes the images, packs the initramfs, boots the **host's own kernel** in QEMU (module always matches), greps the verdict. |

Design notes:

- **No guest OS.** The initramfs is just static busybox + `rcraid.ko` +
  `/init`. Nothing to download, boots in ~1 s under KVM.
- **No competing driver.** The guest kernel's `nvme` driver is a module
  we simply don't ship, so the virtual controllers stay unbound until
  `new_id` hands them to rcbottom.
- **No driver test hooks.** QEMU's NVMe (`1b36:0010`) reaches the normal
  probe path: `rc_classify_device` routes any PCI class `0x0108`
  function to the NVMe code, and BAR selection accepts NVMe-class
  functions (controller registers are in BAR0 per spec).
- **Same geometry as hardware.** QEMU reports MDTS=7 / MPSMIN=0 /
  VWC=1 — identical to the Crucial T700s — so PRP-list sizing and the
  write-cache path are exercised realistically.

`mkmeta.py` writes only what the Linux driver consumes — images it
produces are for driver testing, not for RAIDXpert or Windows.

## Knobs

```
--level raid0|raid1     metadata RAID level (raid1 assembles once the
                        driver grows RAID1 dispatch — that's the point)
--members N             member count (default 2)
--size-mib M            per-member image size (default 256)
--workdir DIR           keep artifacts somewhere specific
```
