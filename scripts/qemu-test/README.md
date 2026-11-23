# QEMU test rig

Boots rcraid in a VM against **virtual NVMe drives carrying synthetic
RAIDCore metadata** — an array that can be created, corrupted, and
destroyed freely, unlike the dev box's RAID0 (which is the rootfs).
This is the development rig for RAID1 / failure-handling / rebuild work,
and it is the CI `qemu-rig` gate: it runs on every push to `main` and
every PR as five scenarios — `raid0`, `raid1`, `raid1-degraded`,
`raid1-degraded-boot` and `raid5-parity-write` — each followed by the guest
battery.

```sh
# One-time: the Ubuntu kernel image is root-readable only
sudo install -m 644 /boot/vmlinuz-$(uname -r) ~/vmlinuz

./scripts/qemu-test/run-qemu-raid-test.sh --kernel ~/vmlinuz

# Same, but with the images stamped by the driver's own encoder
# (rc_meta_build.h) instead of mkmeta.py:
./scripts/qemu-test/run-qemu-raid-test.sh --kernel ~/vmlinuz --writer encoder

# Exercise the opt-in RAID 5 write path: a complete row, an in-row and
# boundary read-modify-write, a write straddling two rows, and two
# concurrent buffered writers (the contention shape behind issue #17):
./scripts/qemu-test/run-qemu-raid-test.sh --kernel ~/vmlinuz \
    --level raid5 --members 4 --parity-writes
```

Exit 0 iff the guest prints `RCRAID-TEST-PASS`. On failure the workdir
(console log, images, initramfs) is kept and its path printed.

## How it works

| Piece | Job |
|---|---|
| `mkmeta.py` | Writes what the driver's parser validates onto blank images: the RC_MetaData block at LBA 0x5000 (magic, version, ported XOR-lane-shuffle checksum) and an RC_LogicalDevice record + element array at the config ring (LBA 0x5800). |
| `src/stamp_meta_userspace.c` | The same layout stamped by the driver's own encoder (`rc_meta_build.h`) for RAID 0, 1 and 10 sets: `make stamp-tool` builds `rcstamp`, selected with `--writer encoder`. This is the writer path `rcadm` CREATE will call, so the rig proves the C encoder — not just the Python stand-in — produces an array the driver assembles. |
| `guest-init.sh` | The VM's `/init` (busybox): insmods rcraid, binds every NVMe-class PCI function to rcbottom via sysfs `new_id`, verifies `/dev/rcraid0` capacity, round-trips data at start/middle/end with dropped caches, then re-checks after a module reload cycle. |
| `run-qemu-raid-test.sh` | Host driver: builds the module against the running kernel, makes the images, packs the initramfs, boots the **host's own kernel** in QEMU (module always matches), greps the verdict. |

Design notes:

- **No guest OS.** The initramfs is just static busybox + `rcraid.ko` +
  `/init`. Nothing to download, boots in ~1 s under KVM.
- **No competing driver.** The guest kernel's `nvme` driver is a module
  we simply don't ship, so the virtual controllers stay unbound until
  `new_id` hands them to rcbottom.
- **One explicit opt-in, no hidden hooks.** QEMU's NVMe (`1b36:0010`)
  reaches the normal probe path: `rc_classify_device` routes any PCI
  class `0x0108` function to the NVMe code, and BAR selection accepts
  non-B000 NVMe-class functions **only** under `allow_foreign_nvme=1`
  (the guest init passes it). Default-off on purpose — the NVMe
  bring-up resets the controller it touches, so an accidental `new_id`
  bind of an unrelated drive must fail at BAR mapping instead.
- **Same geometry as hardware.** QEMU reports MDTS=7 / MPSMIN=0 /
  VWC=1 — identical to the Crucial T700s — so PRP-list sizing and the
  write-cache path are exercised realistically.

`mkmeta.py` writes only what the Linux driver consumes — images it
produces are for driver testing, not for RAIDXpert or Windows.

**`/dev/rc_api` is replayed, not assumed.**  Every run (except the two
degraded scenarios) also executes `rcapireadtest`, which issues the same
SG_IO commands `rcadm` issues and asserts the replies field by field: the
`0xD7` id string, the 1 KiB descriptor and its `0xC0`→`0xD0`→`0xE0` window
walk, the full 64 KiB config packet, the `GET_CREATE_INFO` query, and that
mutation frames come back as CHECK CONDITION with the right sense key/ASC
instead of success.  AMD's binary is not bundled; the ABI is (`src/rc_api.c`).

**The metadata write path verifies itself.**  This rig always loads the
module with `meta_write_probe=1 meta_write_probe_junk=1`, so at assembly
the driver rewrites each member's own RC_MetaData block through its NVMe
queues, flushes, and reads it back, refusing to write unless the block it
is about to write already matches media.  Junk mode first puts a
different block at LBA 0x5000 to prove the medium really changed — a
dropped WRITE would otherwise leave the read-back equal by luck — then
restores the real block.  Any mismatch, refusal or error fails the guest
battery, so a regression in the write path cannot pass CI silently.

## Knobs

```
--level raid0|raid1     metadata RAID level (raid1 assembles once the
                        driver grows RAID1 dispatch — that's the point)
--members N             member count (default 2)
--size-mib M            per-member image size (default 1024)
--workdir DIR           keep artifacts somewhere specific
```

## Scratch-disk workspace

`--workdir` puts everything a run produces — member images, initramfs,
console log — under one directory.  On the dev box those live on a
dedicated disk (`/dev/sdc1`, ext4 label `rclive`, mounted at
`/mnt/rclive`) so multi-gigabyte arrays and repeated runs do not fill
the root filesystem, and a failure's artifacts survive a reboot:

```sh
sudo mount /dev/sdc1 /mnt/rclive
mkdir -p /mnt/rclive/kernel
sudo cp /boot/vmlinuz-$(uname -r) /mnt/rclive/kernel/vmlinuz
chmod 644 /mnt/rclive/kernel/vmlinuz

./scripts/qemu-test/run-qemu-raid-test.sh \
    --kver "$(uname -r)" \
    --kernel /mnt/rclive/kernel/vmlinuz \
    --level raid1 --workdir /mnt/rclive/rig/raid1
```

The disk is a **data disk, never a boot target**: it carries no bootloader
entry and no ESP, and a run never reboots the host — `kernel/vmlinuz` is
only a file handed to QEMU's `-kernel`; the guest unpacks its own root into
RAM and is gone again in seconds.
Keep it that way.  This box's root filesystem is on NVMe behind the same PCI
function the driver claims, so loading the module anywhere except a QEMU
guest can take the development tree down with it.

Give each run its own subdirectory (`raid0-4x1g`, `raid1-degraded`, ...)
and delete it once the artifacts are no longer interesting; the rig
`rm -f`s the member images it is about to reuse, so a stale name only
costs space.  Exercised this way: 2 x 256 MiB RAID1 and 4 x 1 GiB RAID0
(a 4044 MiB volume), both `==> PASS`.
