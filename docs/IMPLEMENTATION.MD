# Implementation Checklist — Path to Prime Time

Tracks what's left between the current driver and a daily-driver-quality
release. Grouped by priority: blockers first, then features, then polish.
Beyond daily-driver, the [Enterprise gaps](#-enterprise-gaps--beyond-daily-driver)
section tracks what an enterprise-grade release adds on top.

Check items off as PRs land. Each item should resolve to a PR (or a
documented "won't fix" with reasoning).

For status of work already done, see [`STATUS.md`](STATUS.md).

---

## ⛔ Blockers — driver isn't usable as a daily driver without these

- [x] **Partition table scan** — disk registers with `GENHD_FL_NO_PART`
      so `/dev/rcraid0p3` etc. never appear; users have to `kpartx`
      to mount partitions. Flip to standard partscan and make sure
      the GPT secondary header at end-of-disk is reachable through
      the stripe map.
      *Touch points:* `rc_volume_create_disk` (gendisk flags),
      `rc_volume_queue_rq` (last-stripe / GPT-tail addressing).
      *Resolved:* dropped `GENHD_FL_NO_PART`, bumped `minors` to 256.
      The stripe map needed no change — `rc_volume_map_lba` is pure
      modular arithmetic with no end-of-disk casework, and partscan
      round-trips a 2-partition GPT (validated on hardware).

- [x] **Fast discard / TRIM** — `mkfs.xfs` discard hangs (~70 min for
      3.6 TB), `fstrim` hangs. Current path likely issues one DSM
      per stripe instead of forwarding the bio's whole discard range
      to each member as one DSM. Add a multi-member discard dispatch
      analogous to `rc_volume_dispatch_multi_stripe` for writes.
      *Touch points:* `rc_volume_queue_rq` (REQ_OP_DISCARD branch),
      new `rc_volume_dispatch_multi_stripe_discard`.
      *Resolved:* multi-stripe discard fans one DSM per AFFECTED MEMBER
      (each member's owned stripes form one contiguous phys extent), and
      `max_hw_discard_sectors` bumped 256 KiB → 1 GiB. On 2× T700 / 3.6 TiB:
      `mkfs.xfs` discard pass 70 min → 3.0 s; `fstrim` hung → 2.4 s.

- [x] **DKMS packaging + udev autobind** — every boot the T700s
      come up bound to `nvme`; user must manually unbind + insmod.
      Need: `dkms.conf`, post-install udev rule that unbinds
      AMD-RAID members from nvme and rebinds to rcbottom, systemd
      drop-in so the array is up before fstab is consulted.
      *Touch points:* new `dkms.conf`, new `udev/50-rcraid.rules`,
      new `systemd/rcraid-bind.service`.
      *Resolved:* `dkms.conf` + `install-dkms.sh` register the module
      with DKMS so it rebuilds on kernel updates. udev rule + a
      `/usr/sbin/rcraid-bind` helper move matching members to rcbottom
      via `driver_override` + unbind + re-probe.  A systemd service
      wasn't needed — fstab device units wait for `/dev/rcraid0pN`
      naturally.  Installer auto-detects array `subsystem_vendor`
      (the vendor with ≥ 2 matching devices).

- [x] **Boot-from-RAID (initramfs)** — to install Linux *on* the array
      and have it serve as the rootfs, rcraid.ko + the udev rebind
      glue must live inside the initramfs so they're present before
      pivot_root.  The Windows analogue is `StartType = 0`
      (SERVICE_BOOT_START) + `LoadOrderGroup = "SCSI miniport"` in
      `rcbottom.inf`, which makes winload pull the driver before the
      disk subsystem comes up.
      *Touch points:* new `packaging/dracut/95rcraid/module-setup.sh`,
      new `packaging/initramfs-tools/hooks/rcraid`, install-dkms.sh
      generator detection + regen, uninstall-dkms.sh cleanup.
      *Resolved:* dracut module + initramfs-tools hook bundle the
      kernel module, `/usr/sbin/rcraid-bind`, `50-rcraid.rules`, and
      `rcraid.conf` into the initramfs.  Installer detects which
      generator the distro uses (dracut vs initramfs-tools) and
      regenerates after the rest of the install is staged.

- [x] **Live-CD install workflow** — for users coming from a blank
      array, there needs to be a one-script path that exposes
      `/dev/rcraid0` to the live distro's OS installer, then configures
      the freshly-installed target to boot off the array.  No ISO
      injection, no hosting required — works from any stock Debian /
      Ubuntu / Fedora live CD.
      *Touch points:* new `install-livecd.sh`.
      *Resolved:* two-phase script — phase 1 builds + loads rcraid in
      the live session so the installer (Ubiquity / Calamares /
      Anaconda / debian-installer) sees `/dev/rcraid0` as an ordinary
      disk; phase 2 (after user finishes the OS install) chroots into
      the new rootfs and runs the equivalent of `install-dkms.sh`
      against the target's kernel.  Mirrors DesktopECHO's
      `raidxpert2-install` UX with our clean-room stack.

- [ ] **Module signing for Secure Boot** — `insmod` fails with
      "Key was rejected by service" when SB is enabled. Ship signing
      scripts (or document MOK enrollment); for distro builds, sign
      with the kernel's ephemeral key.
      *Touch points:* new `scripts/sign-module.sh`, INSTALL.md.
      *Narrowed 2026-07:* on shim distros (Ubuntu/Fedora) DKMS already
      signs the module with the shim MOK key automatically (verified:
      `dkms build` runs kmodsign with `/var/lib/shim-signed/mok/MOK.priv`),
      so a user who has enrolled a MOK gets Secure Boot for free. What's
      left is documentation (MOK enrollment walkthrough in INSTALL.md,
      including the `mokutil --import` reboot dance) and the manual-build
      path (`build.sh` output is unsigned).

- [ ] **Writes enabled by default (with safety latch)** —
      `enable_writes=1` opt-in is a packaging nightmare. Default to
      enabled, but require a one-time `RC_VOLUME_WRITES_ACK=YES`
      sysfs write (or kernel cmdline flag) on the first boot per
      volume, so distros can't accidentally ship a footgun. Goal is
      "no extra modprobe args needed for normal use" once user has
      ack'd once.
      *Touch points:* `enable_writes` static + new sysfs gate +
      blk-mq write rejection until ack'd.

---

## 🔧 Missing features — limit real-world use beyond benchmarks

### Data path

- [x] **Suspend / resume (S3 / S4)** — `.suspend` / `.resume` PM
      callbacks at the PCI driver level. Quiesce blk-mq, save SQ/CQ
      DMA addresses, disable controller, restore on resume.
      *Resolved:* `SIMPLE_DEV_PM_OPS` on `rc_bottom_driver` covers both
      S3 and S4 with one pair of hooks.  `pm_suspend_adapter` masks
      MSI vectors and clears `CC.EN`; `pm_resume_adapter` delegates
      to `rc_nvme_reset_controller`, which already re-programs the
      admin queue and re-issues `Create CQ/SQ` against the existing
      DMA buffers.  Validated on hardware: three `rtcwake -m mem -s 15`
      cycles, 256 MiB SHA-256 matches across each, writes still work
      after.

- [ ] **Hot-plug / drive failure handling** — pulling a member
      drive today will likely oops (NULL ptr on stale `rcraid_dev`).
      Add `.remove` cleanly tearing down a member, mark volume
      degraded (no-op for RAID0 = volume fails), drain pending I/O
      with `BLK_STS_IOERR`.

- [x] **FUA / Flush / barrier semantics** — Flush command exists;
      FUA bit on write SQEs is not set even when bio has REQ_FUA.
      Databases / filesystems expecting durability see ack-before-
      durable. Wire `cmd_flags & REQ_FUA` → SQE DW12 bit 30.
      *Resolved:* `REQ_OP_FLUSH` fans out NVMe FLUSH to every member;
      `REQ_FUA` sets CDW12 bit 30 on the write SQE; queue limits
      advertise `BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA`. Verified on
      hardware (`write_cache: write back`, `fua: 1`, IRQs on both
      members for `dd conv=fsync`). See STATUS.md "FLUSH + FUA support".

- [ ] **SMART pass-through / drive health** — `nvme` userspace tool
      can't see the drives. Add a passthrough ioctl (NVME_IOCTL_ADMIN_CMD)
      that proxies to the chosen member, or expose per-member sysfs
      attrs for SMART log (LID=0x02), temperature, wear.

- [ ] **Set Features at init: arbitration + interrupt coalescing**
      — minor perf, but Windows does them. FID 0x01 (Arbitration),
      FID 0x08 (Interrupt Coalescing). Estimated 1-5 % gain.

### RAID feature parity

- [ ] **RAID1 (mirror)** — read from either member (load-balance);
      write to both. Per-member error → mark degraded, keep going.
      Metadata parser already recognises `RC_LDT_RAID1`.
      *Progress:* the I/O path shipped — round-robin read balancing,
      write/discard fan-out to every mirror completing on last ACK,
      RAID-level seeding + consistency checks at registration.
      Validated on the QEMU rig (mirror images byte-identical after the
      test workload); hardware validation pending a real RAID1 array.
      Still open (and why this box stays unchecked): **degraded mode** —
      today a member error fails the volume like RAID0 instead of
      serving from the survivor.  That lands with hot-plug / failure
      handling below.

- [ ] **RAID10** — combination of RAID0 + RAID1. Stripe across
      mirrored pairs. Reuses RAID0 stripe math + RAID1 mirror dispatch.

- [ ] **RAID5 / RAID6** — parity calculation in the write path
      (rmw + p / p+q). Significant work: scrub, rebuild, async XOR.
      Probably won't land soon; flag as future.

- [ ] **Multi-volume support** — BIOS can create multiple arrays
      on the same members. We hardcode one `/dev/rcraid0`. Refactor
      `rc_volume_registry` to be a list, create one gendisk per LD.

- [ ] **Array creation from Linux** — today the only way to make
      an array is BIOS / RAIDXpert. Add an ioctl + userspace tool
      (`rcraid-create`) that writes RAIDCore metadata + LD record.
      Would let us live without rebooting for every array change.

- [ ] **Replace / rebuild a failed drive** — moot for RAID0; needed
      for RAID1/5/10. Background rebuild kthread, progress in
      debugfs.

---

## ✨ Polish — for upstream submission / distribution

- [ ] **User-facing INSTALL.md** — current INSTALL is fine for devs
      but assumes you can read kernel logs. Need a "if you can't
      boot after enabling Secure Boot, here's what to do" guide.

- [ ] **Distro packaging** — `.deb` (DKMS-source), `.rpm` (akmods),
      AUR pkgbuild. CI artifacts so users don't have to build.

- [ ] **Kernel test suite integration** — run `blktests` against
      `/dev/rcraid0`. `xfstests` on an XFS fs on top. Currently
      no automated regression coverage beyond `bench.sh`.

- [ ] **License / upstream review** — code is GPLv2 by file
      headers, but the AMD-RAID metadata layout is reverse-
      engineered from a closed-source driver. Before any LKML
      submission, get a written read on whether this is reusable.

- [ ] **Kbuild / Kconfig** — only out-of-tree today. For upstream
      we'd need `drivers/block/rcraid/Kconfig` + `Makefile` slotting,
      and adjustments for the in-tree style.

- [x] **Doc consolidation** — `docs/` had 9 active files with heavy
      overlap. *Resolved:* consolidated to four — `STATUS.md` (state +
      impl log + error-handling design), `IMPLEMENTATION.MD` (this
      checklist), `REVERSE_ENGINEERING.md` (all RE findings: Ghidra
      analysis, rcraid geometry, multi-queue, open questions), and
      `README.md` (index). `ERROR_HANDLING.md` folded into STATUS;
      `GHIDRA_FINDINGS_2026` / `RCRAID_GEOMETRY_RE` /
      `WINDOWS_MULTIQUEUE_FINDINGS` / `OPEN_QUESTIONS` folded into
      REVERSE_ENGINEERING; `WORKSTATION_SESSION` moved to `archive/`.

---

## 🏢 Enterprise gaps — beyond daily-driver

What separates a daily-driver release from something a company could
deploy and support. Several checklist items above are enterprise table
stakes too — **RAID1/10**, **hot-plug / drive-failure handling**,
**rebuild**, **SMART pass-through**, **multi-volume**, and **kernel test
suite integration** — the redundancy trio (RAID1 + failure handling +
rebuild) being the definition of the category. The items below are what
*isn't* tracked anywhere else.

### Data integrity

- [ ] **Scrub / patrol reads** — background pass that reads every
      stripe and surfaces latent sector errors before a rebuild trips
      over them. RAID0 version is detection-only (report via uevent +
      log); becomes repair once RAID1 exists. Progress/last-run in
      sysfs or debugfs.

- [ ] **Retry policy for transient errors** — DNR=0 completions are
      retryable per spec but we end the request with `BLK_STS_IOERR`
      today. Re-dispatch after controller reset; bounded retries with
      backoff. (Already flagged in STATUS.md "Limits"; tracked here
      because enterprise workloads hit transient SCs that desktops
      never see.)

- [ ] **End-to-end protection statement (T10 PI/DIF)** — likely
      *won't* be supported (consumer NVMe members, 512e LBA format);
      the enterprise-relevant deliverable is a documented position, not
      code.

### Management & monitoring plane

- [ ] **Array state observability** — status queryable by a tool, not
      dmesg: volume state (online/dead/resetting), per-member health,
      reset/latch state. Start with sysfs attrs; grow into a
      `rcraidctl` CLI. Windows parity here is RAIDXpert2.

- [ ] **Event notification** — emit uevents on state transitions
      (member dead, auto-reset attempted/failed, volume offline) so
      monitoring stacks (node_exporter, collectd, udev rules) can
      alert without scraping logs. Structured, ratelimit-safe log
      lines with stable prefixes for the same reason.

### Validation & support

- [ ] **Tested-configuration support matrix** — everything is
      currently validated on one config: TRX50 + 2× Crucial T700 +
      one Ubuntu kernel line. Document what's tested vs. best-effort
      (kernels, distros, drive models, member counts ≥ 3), and grow
      the matrix deliberately. The AHCI variants are already flagged
      best-effort; the NVMe path has the same single-config exposure,
      just undocumented.

- [ ] **Power-fail / crash-safety validation** — the design is right
      (members' volatile write cache + honest FLUSH/FUA), but nothing
      documents torture evidence: power cut mid-write → journal
      recovery → data verification, repeated. Scripted harness +
      results write-up. Related: formalize the "no write-back driver
      cache without power-loss protection" position (currently a
      side-note in the Performance section) into a short data-durability
      statement.

### Operations & lifecycle

- [ ] **Operator runbook** — STATUS.md's error-handling design is
      engineering docs; operators need: what to do when the volume
      goes offline, interpreting the auto-reset latch, safe driver
      upgrade procedure (install-dkms.sh re-run + reboot), rollback
      when a DKMS rebuild fails on a new kernel, drive replacement
      (once rebuild exists).

- [ ] **Security & release lifecycle policy** — how vulnerabilities
      in the Linux driver get reported and fixed, which releases get
      fixes, changelog / release-notes discipline (the VERSION file
      is the start). The repo tracks the Windows-side CVE-2024-21962;
      the Linux driver needs its own policy.

- [ ] **Legal read on RE'd metadata (deployment, not just upstream)** —
      the existing "License / upstream review" polish item covers LKML
      submission; enterprise adoption additionally needs a written
      opinion on whether companies can legally *deploy* a driver built
      on a reverse-engineered metadata layout. Bigger than one
      checkbox; deserves its own doc once counsel weighs in.

---

## 🚀 Performance — diminishing returns beyond ~75 % of NAND spec

Current measured (`fio --direct=1` sustained 15 s, 2× Crucial T700,
256 KiB stripe, re-measured 2026-07 after the VWC-enable fix):

| Pattern | We get | Spec ceiling | Notes |
|---|---:|---:|---|
| SEQ1M Q8T1 read | 19.7 GB/s | ~24.8 GB/s | 79 % — close to wall |
| SEQ1M Q8T1 write | 19.7 GB/s | ~23.6 GB/s | 83 % — symmetric with read |
| RND4K Q32T16 read | 6.2 GB/s (1.5 M IOPS) | ~7 GB/s | excellent |
| RND4K Q1T1 read | 96 MB/s | ~110 MB/s | latency-bound |

(Spec ceilings are 2× the per-drive T700 2 TB figures: 12.4 GB/s read /
11.8 GB/s write each. An earlier revision of this table showed 8.9 GB/s
write — that was measured before the driver enabled each member's
volatile write cache at init; it is not a current limit. The README
quotes 19.7 read / 18.7 write from KDiskMark 3.3.0 on the same array;
the ~1 GB/s write-side gap vs this table is KDiskMark harness overhead,
not the driver — raw `fio` measures the two directions identical.)

Windows CDM's headline 22 GB/s sequential write is a benchmark artifact
(CDM uses tiny test sizes that fit in the drive's SLC cache). For real
sustained workloads we're at parity with Windows — we just measure
honestly.

- [ ] **Single-request multi-stripe fan-out (READ/WRITE)** — deliberately
      disabled: `queue_limits.chunk_sectors` is set to the stripe size, so
      blk-mq splits every READ/WRITE at stripe boundaries and each request
      maps to exactly one member. Parallelism across members comes from many
      requests in flight concurrently, not from one request fanning out.
      The fan-out path (`rc_volume_dispatch_multi_stripe`) is wired up and
      kept ready, but unreachable for READ/WRITE by design — enabling it
      means raising `chunk_sectors`, and history says to be careful: the
      old always-fan-out dispatch corrupted data on fragmented buffered
      writeback. Not worth enabling today; 256 KiB single-member commands
      already deliver the symmetric ~19.7 GB/s above.
      *Touch points:* `rc_volume_create_disk` (`queue_limits.chunk_sectors`),
      `rc_volume_queue_rq`, `rc_volume_dispatch_multi_stripe`.

- [ ] **SGL for big transfers** — for transfers > MDTS we currently
      split into multiple PRP-list commands. SGL would let one
      command carry an arbitrarily-large scatter-gather list. Marginal
      gain at current bottleneck.

- [ ] **NUMA-aware queue placement** — single-socket today; if
      TRX50 expands to multi-socket the per-queue `dma_pool` should
      live on the local node.

- [ ] **Write-back driver cache (controversial)** — would replicate
      Windows CDM's inflated numbers by ack'ing writes once they land
      in driver-owned RAM, then asynchronously pushing to NVMe.
      Major data-loss risk on power failure. Probably won't ship
      without UPS detection + journaling.

---

## 🤔 Open reverse-engineering questions

See the "Open questions" section of
[`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md). Most relevant
remaining items:

- [ ] `rcraid.sys` completion ISR (`FUN_14000d2b8`) — never decompiled.
      Confirm CQ entry layout vs. NVMe-standard.
- [ ] AHCI path bring-up (devices 0x7905 / 7916 / 7917 / 43BD) —
      MODULE_DEVICE_TABLE already claims them; the AHCI submit /
      complete path needs to be implemented fresh from `rcbottom.sys`.

---

## Suggested next-PR order

The original daily-driver sequence (partition scan, fast discard,
DKMS/udev, suspend/resume, boot-from-RAID) has shipped. Current order,
aimed at the enterprise bar:

1. **RAID1 support** — first redundancy level; unlocks everything in
   the enterprise category. Needs a second test array (can't reformat
   the dev box's rootfs RAID0), or a loopback/qemu harness first.
2. **Hot-plug / drive-failure handling** — degraded-mode semantics;
   pairs with RAID1 (for RAID0 it just means failing the volume
   cleanly instead of oopsing).
3. **Rebuild** — background resync kthread + progress reporting;
   completes the redundancy trio.
4. **Array state observability + uevents** — sysfs status attrs and
   state-transition events; cheap once the degraded-state machinery
   from (2) exists.
5. **SMART pass-through** — per-member health visibility.
6. **Kernel test suite integration** — blktests/xfstests in CI so the
   redundancy work above stays regression-backed.
7. **Secure Boot documentation** (MOK enrollment; the DKMS/shim path
   already signs) and **writes-enabled-by-default latch**.
8. **Multi-volume support**, then the remaining enterprise doc items
   (support matrix, runbook, durability statement) alongside.

Everything else can come after.
