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

> Note: the write-perf fix (multi-stripe write fan-out, §5) is **not yet in the
> code**, so a reinstall will not change throughput.

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

## 5. Multi-stripe write fan-out rewrite

The one real remaining **code** change. A write spanning a stripe boundary is
currently disabled (why writes trail reads). The fix fans a spanning write out
into per-member I/Os. Needs the hardware to test against — do it after §4 has a
scratch target so it can be exercised safely.
