# Workstation session — start-here checklist

Context notes for working on the **Kubuntu 24.04 (HWE)** RAID box, where the
rootfs lives **on the rcraid array**. Because the running `rcraid` module is
what boots the machine, driver changes here are load-bearing: a bad rebuild can
leave the box unbootable. Work safest-first.

> This file exists because the planning context did not live in the repo. It is
> a working checklist, not formal documentation — prune it once the items are
> done.

## 0. Safety net (do this before touching the driver)

- [ ] Keep the **rcraid live USB** on hand — the recovery path if a rebuild
      breaks boot.
- [ ] Back up the current, known-good initramfs:
      ```sh
      sudo cp /boot/initrd.img-"$(uname -r)"{,.good}
      ```
- [ ] Note the current kernel: `uname -r` (must be ≥ 6.15 — the driver uses the
      2-arg `blk_rq_map_sg()`; on Ubuntu 24.04 that means the HWE stack).

## 1. Read-only check first — do NOT install yet

Confirm what is actually running before deciding whether a reinstall is even
warranted:

- [ ] In-use driver: `modinfo rcraid | grep -E 'version|vermagic|filename'`
      and `dmesg | grep -i rcraid | head`
- [ ] Array health: `cat /proc/mdstat` and the driver's own status node
- [ ] Confirm root is on the array: `findmnt /` and `lsblk`

If the installed driver is already current, skip the reinstall and go to §3.

## 2. Reinstall-to-latest — only if warranted

A `sudo ./install-dkms.sh` rebuild is worth it **only if the box was installed
before** these landed:

- suspend/resume support (**PR #7**)
- initramfs bundling for boot-from-RAID (**PR #8**)

If installed after those (or a fresh install from current `main`), the driver is
already current — **skip this step**, reinstalling only adds risk.

If you do rebuild:

- [ ] `git pull` to current `main` first.
- [ ] `sudo ./install-dkms.sh`
- [ ] **Verify the new initramfs contains rcraid before rebooting:**
      ```sh
      lsinitramfs /boot/initrd.img-"$(uname -r)" | grep rcraid
      ```
- [ ] Only then reboot. If it fails to come up, boot the live USB and restore
      `initrd.img-<ver>.good`.

> Note: the multi-stripe write fan-out (§5) **is already in the code** as of
> PR #10, so the driver on current `main` already has it — a reinstall does not
> add it (it was merged before this checklist was written).

## 3. Validate the live-CD installer fixes (PRs #24, #26)

These were reviewed + CI-green but have **never run on real hardware**. Validate
from a live-USB install run:

- [ ] **Phase-2 array eject/unmount** — the original bug: the installer should
      release the array cleanly on its own, without you manually unmounting and
      hitting enter.
- [ ] **Shared-ESP safety** — an existing foreign `BOOTX64.EFI` (e.g. Windows)
      is backed up, not clobbered; our own loader from a prior distro is
      replaced without piling up backups.
- [ ] **Self-registered UEFI boot entry** — `efibootmgr` shows the rcraid entry
      with the correct loader path (derived, not a hardcoded Kubuntu path).

## 4. Write-path test — blocked on a scratch-target decision

`test_write_path.sh` is **destructive** and the array now hosts the rootfs, so it
cannot run against the live array. Decide a scratch target first:

- a spare partition / spare NVMe, **or**
- a loopback file on the array (tests the block path, not the raw members).

Then run the test against that target only.

## 5. Multi-stripe write fan-out — DONE (PR #10)

**Already implemented and merged** — this checklist's claim that it was "not yet
in the code" was stale (PR #10, `241b787`, landed hours before this doc). A
stripe-spanning READ/WRITE is fanned out into one NVMe command per member by
`rc_volume_dispatch_multi_stripe()` in `rc_nvme.c`; `REQ_OP_WRITE` is handled,
not disabled. Writes are gated behind the `enable_writes` module parameter
(default `0` → disk comes up read-only); load with `enable_writes=1` for
read-write. The workstation box runs with `enable_writes=1`.

Follow-ups (status as of 2026-07-07):

- [x] **Measure it — done, with a correction.** BEWARE compressible test data:
      this array's storage stack compresses writes, so fio with its default
      (low-entropy) buffers reports ~15–18 GiB/s writes — **not real**. With
      *incompressible* data (`--buffer_compress_percentage=0`, as KDiskMark
      uses) the honest numbers are:
      - Sequential **read** ~18–19 GB/s (Q8); ~10 GB/s (Q1).
      - Sequential **write** ~8.6 GiB/s (**9 GB/s**) — matches KDiskMark's
        8559 MB/s.
      So writes run ~45% of reads. That is normal NVMe write<read media
      asymmetry, **not** a driver defect. The fan-out still works: ~9 GB/s is
      ~2× a single drive, i.e. both RAID0 members write in parallel. PR #10's
      win is *command efficiency* (2 NVMe cmds/spanning-IO instead of 4), not
      making NAND writes as fast as reads. **Always bench this array with
      incompressible data.**
- [x] **`enable_writes` default — decided: KEEP read-only default.** Flipping
      it removes a safety interlock and buys nothing (installs already set
      `enable_writes=1` via `/etc/modprobe.d/rcraid.conf`). The write-safety
      improvement was folded into the geometry veto below instead.
- [x] **Stripe/member auto-detection hardening — VALIDATED on hardware.**
      Branch `harden-geometry-write-veto`: when any member is assembled via the
      untrusted legacy BDF fallback (LD parse failed), the volume is forced
      **read-only even with `enable_writes=1`**, so a guessed member order can't
      corrupt the array. Purely additive (can only force read-only). Installed
      via `install-dkms.sh` and rebooted 2026-07-07: new module (srcversion
      `41C1D5B…`) booted, both members took the trusted LD path, dmesg logged
      `writes ENABLED / read-write` with no `UNTRUSTED` warning, and an
      O_DIRECT write+read test passed. The veto is compiled in and runs each
      boot but correctly stays silent on this box's trusted geometry.
      Still uncommitted on the branch — commit + PR pending.
