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

- [x] **Hot-plug / drive failure handling** — pulling a member
      drive today will likely oops (NULL ptr on stale `rcraid_dev`).
      Add `.remove` cleanly tearing down a member, mark volume
      degraded (no-op for RAID0 = volume fails), drain pending I/O
      with `BLK_STS_IOERR`.
      *Resolved:* `rc_bottom_remove()` tears down **volume membership
      first**, then the controller, then the adapter — the volume must go
      before `io_queues[]` is freed, or `/dev/rcraid0` keeps dispatching
      into freed memory (the surprise-remove use-after-free). Per-member
      state machine (`FAILED` / `NEEDS_RESYNC`) plus a lock-free live mask;
      the timeout path and drain treat a NULL slot (hot-removed member) as
      permanently dead and synthesise completions instead of waiting on it.
      RAID0 fails on a lost member, mirrored levels degrade and keep serving.
      *Not yet exercised:* a real surprise removal (AER or physical unplug)
      on hardware — the rig covers death via `fail_member` and the ISR path.

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

- [x] **RAID1 (mirror)** — read from either member (load-balance);
      write to both.
      *Resolved (2026-07-10, PRs #41/#44/#45):* round-robin read
      balancing, write/discard fan-out to every mirror completing on
      last ACK, RAID-level derivation from the committed config
      generation's FirstCount × SecondCount (firmware writes DeviceType
      0x1BF6 for both levels — `RC_LDT_RAID1` is never seen on disk),
      and contiguous-fragment merging in the fan-out sg builders (mkfs
      metadata writeback is otherwise unexpressible as PRPs).
      **Hardware-validated end to end**: Kubuntu 24.04 installed onto
      and booting from a 2× T700 BIOS RAID1; KDiskMark 20.5 GB/s read /
      11.1 GB/s write on the mirror as rootfs. Full findings in
      STATUS.md's 2026-07-10 log entry.
      **Degraded mode landed** (issue #51, PR 1): per-member state
      machine + lock-free live mask; member death (CSTS fatal, timeout,
      hot-unplug) keeps the volume serving from the survivor; partial
      mirror-write failure completes OK and degrades the erring member;
      a recovered controller parks `needs-resync` (never silently
      rejoins).  Still to come from #51: degraded boot assembly (opt-in
      module param), member re-add plumbing, and the resync engine.

- [x] **RAID10** — combination of RAID0 + RAID1. Stripe across
      mirrored pairs. Reuses RAID0 stripe math + RAID1 mirror dispatch.
      *Landed 2026-10-02:* `rc_volume_dispatch_raid10()` plus the nested
      column mapping, per-pair degraded failover, pair-aware rebuild, and
      boot-time degraded assembly.  **Not yet hardware-validated on
      `1022:B000`** — [`VALIDATION_RAID10.md`](VALIDATION_RAID10.md) is the
      gate before the README/STATUS rows may say "stable".

- [ ] **RAID5 / RAID6** — **read-only by default; full-row RAID 5 writes
      opt-in, 2026-11-13.** Parity math and the dedicated-parity layout map are
      done (`src/patch_parity_math.c`, the one place to change once a vendor
      array confirms the placement). RAID 6 Q is the vendor's EVENODD
      construction (prime 17, `src/raid6_evenodd.h`), still with no caller: no
      dual-parity write path exists. The level is identified from the LD
      `DeviceType` (`0x1BF5` / `0x1BFA`, confirmed on a BIOS-created 3-disk
      RAID 5), a parity volume assembles at `(N-1)` / `(N-2)` × per-member user
      size, and reads go through the shared map.
      **Write path (opt-in):** with `enable_writes=1 parity_writes=1` a request
      that covers one complete row is written as one NVMe WRITE per data member
      plus the dedicated parity chunk, with P generated from the request's own
      bytes into a bounded pool of driver buffers. A partial row is
      read-modify-written: only the touched data chunks and the old parity are
      read into per-member DMA buffers, the request's bytes are overlaid and P
      becomes P_old ^ D_old ^ D_new before the touched chunks plus parity are
      rewritten. All RAID 5 writes are
      serialized by one semaphore so a partial and a full write cannot update
      the same row's parity from different old bytes. Reads fan out per data
      member when the queue is not chunk-split.      The QEMU rig and, on 2026-11-13, the array box verify full-row, in-row,
      boundary and cross-row writes; the hardware run also XOR-checked the
      parity chunk on media. Discards are refused. **Degraded reads and
      parity rebuild landed 2026-11-13:** with one member failed the volume
      reports `degraded`, reconstructs a read chunk from P plus the surviving
      chunks through the synchronous RMW buffers, and refuses writes;
      `AMD_IOCTL_START_RESYNC` rebuilds a `FAILED` member from the other N-1
      members in 128 KiB chunks, including the final partial-page tail.
      Hardware-validated on a 4-member 256 MiB fixture (64 MiB written, member
      failed, 64 MiB degraded readback, 85 MiB rebuild, optimal readback and
      new writes). Without `parity_writes=1` the volume stays read-only and the
      installer's hard stop is unchanged. Still open: the write hole, parity
      initialisation, automatic rebuild, a firmware-created
      parity array, and vendor-written corroboration (issues #6 and #3).

- [ ] **Multi-volume support** — BIOS can create multiple arrays
      on the same members. We hardcode one `/dev/rcraid0`. Refactor
      `rc_volume_registry` to be a list, create one gendisk per LD.

- [x] **Replace / rebuild a failed drive** — moot for RAID0; needed
      for RAID1/5/10. Background rebuild kthread, progress in
      debugfs.
      *Resolved:* `rc_volume_resync_thread` copies the survivor over the
      `NEEDS_RESYNC` target behind a logical write-exclusion window, one
      member at a time; `rc_volume_resync_pick_survivor()` picks the source,
      and RAID10 rebuilds from the column's pair partner. Progress is
      `/sys/block/rcraid0/rcraid/resync_progress` (`memberN cur/tot pct%`).
      **Scope: RAID1, RAID10 and RAID5** — RAID 5 rebuild XORs the other N-1
      members into the target; RAID 6 remains open.

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

- [x] **Register the control bridge (`/dev/amd_ctl0`)** —
      `rc_init()` calls `rc_amd_init_ioctl_bridge()`, which registers a
      `misc` device at `/dev/amd_ctl0`; `rc_exit()` removes it before volume
      teardown. `register_chrdev()` was replaced because it allocated a major
      but created no node and reallocated the major on every load.
      Read-only `GET_STATUS` / `GET_HARDWARE_INVENTORY` are unprivileged;
      `FAIL_MEMBER`, `START_RESYNC`, `COMMIT_NEW_ARRAY`, and `DESTROY_ARRAY`
      require `CAP_SYS_ADMIN`. The bridge routes to the assembled volume
      (`rc_volume_*`), not the idle async engine. Array create/destroy still
      return `-EOPNOTSUPP` because the metadata writer does not exist.
      *Touch points:* `rc_init()`/`rc_exit()` in `rc_main.c`,
      `src/patch_ioctl_bridge.c`, and `src/amd_raid_uapi.h`.

- [ ] **`rcraidctl` / live control CLI** — the transport now exists, but no
      CLI consumes it. Next step is a small tool over
      `AMD_IOCTL_GET_STATUS`, `AMD_IOCTL_GET_HARDWARE_INVENTORY`, and
      `AMD_IOCTL_FAIL_MEMBER`, with `START_RESYNC` limited to RAID 1/10.

- [x] **Array state observability — sysfs half** — status queryable by a
      tool, not dmesg: volume state (online/dead/resetting), per-member
      health, reset/latch state.
      *Resolved:* `/sys/block/rcraid0/rcraid/{state,members,resync_progress,fail_member}`
      plus the adapter-level `adapter_info` / `queue_stats` /
      `doorbell_state` / `pending_requests` / `reset` attrs. Machine-readable
      without log scraping, but only by polling.

- [ ] **`rcraidctl` CLI** — a query/control front end over the sysfs attrs
      above (and the ioctl bridge, once registered). Windows parity here is
      RAIDXpert2. The attrs already carry everything such a tool needs; what
      is missing is the tool, and a transport for the control half.

- [ ] **Event notification** — emit uevents on state transitions
      (member dead, auto-reset attempted/failed, volume offline) so
      monitoring stacks (node_exporter, collectd, udev rules) can
      alert without scraping logs. Structured, ratelimit-safe log
      lines with stable prefixes for the same reason.
      *Status 2026-11-08:* there are **zero** `kobject_uevent()` calls in the
      tree — degraded, member death, reset latch and resync completion are
      all dmesg-only, so the sysfs attrs are poll-only.

### Validation & support

- [ ] **Tested-configuration support matrix** — everything is
      currently validated on one config: TRX50 + 2× Crucial T700 +
      one Ubuntu kernel line. Document what's tested vs. best-effort
      (kernels, distros, drive models, member counts ≥ 3), and grow
      the matrix deliberately. The AHCI variants are already flagged
      best-effort; the NVMe path has the same single-config exposure,
      just undocumented.

      *Status 2026-11-14:* the build half is covered — CI compiles this
      tree warning-clean inside Fedora, Arch, Ubuntu 24.04 HWE and
      Ubuntu 26.04 containers on every push and PR, and the QEMU rig
      boots the module through five array scenarios (`raid0`, `raid1`,
      `raid1-degraded`, `raid1-degraded-boot`, `raid5-parity-write`) plus
      the guest battery. What that does
      **not** establish: runtime on anything but the array box's kernel
      (the container jobs are compile-only), drive models other than the
      Sabrent/Crucial sets, or member counts above 4 on real hardware.
      Those stay open, so the bullet stays unticked.

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

## 🛰️ Long-horizon phases (0–8)

**Source of truth.** The table below is the only copy: `README.md` links
here instead of duplicating it, so a phase cannot read "planned" in one file
and "in progress" in the other. Near-term priority is the ordered list at the
bottom of this file; the phases are the long view, and several of them (OCE,
ORLM, COW, P2PDMA, io_uring, ZNS/FDP) are not on the daily-driver path at all.

| Phase | Feature | Status (2026-11-14) | Enables |
|---|---|---|---|
| 0 | RAID 0 / 1 / 10 I/O + rebuild | ✅ RAID0 and RAID1 shipped and hardware-validated; RAID10 feature-complete, array-box run pending ([VALIDATION_RAID10.md](VALIDATION_RAID10.md)) | Production baseline |
| 1 | RAID 5/6 I/O + rebuild | 🟡 Assembly + reads wired; RAID 5 writes opt-in and hardware-validated 2026-11-13, RAID 6 forced read-only; create/delete metadata is a lab-only writer (RAID 5 behind `parity_create=1`, no longer rig-exercised); first firmware 3-disk RAID 5 metadata parsed, assembled and read back 2026-11-08 (`0x1BF5`, N=3, `(N-1)` × user size); placement settled from the vendor core, corroboration pending (issue #6); Q generator now the vendor's EVENODD, uncalled (issue #10); RAID 5 degraded reads + manual parity rebuild hardware-validated 2026-11-13 | Inspect and read existing parity arrays |
| 2 | Rig metadata fixtures for levels no firmware produced | ⏳ Planned — `scripts/qemu-test/mkmeta.py` writes RAID 0/1 for the QEMU rig only; this is test-fixture generation, not array lifecycle: real arrays are created in the UEFI RAIDXpert2 utility and no install path waits for it | QEMU scenarios at RAID10 and synthetic parity levels |
| 3 | OCE (Online Capacity Expansion) | 🗺️ Roadmap — this driver | Add drives online, restripe, grow FS |
| 4 | ORLM (Online RAID Level Migration) | 🗺️ Roadmap — this driver | RAID 0↔5↔6↔10 online, zero host CPU |
| 5 | COW snapshots + instant clone | 🗺️ Future | Fast clones, no space cost |
| 6 | P2PDMA (peer-to-peer DMA) | 🗺️ Future | Drive-to-drive DMA, no host copy |
| 7 | io_uring support | 🗺️ Future | Userspace async I/O, zero-copy |
| 8 | Hardware offload (NVMe ZNS/FDP) | 🗺️ Future | Zero host CPU, device-managed placement |

Phases 3–8 have no code, no design notes and no exit criteria in this tree.
They are recorded so the ambition is explicit, not because they are
scheduled. Phase 2 is the only one of the set that changes what the tree can
test — the rig can only boot arrays it encoded itself — which is why it opens
the next-PR order as fixture work. It is not on the install path: there the
UEFI RAIDXpert2 utility creates the array.

## 🤔 Open reverse-engineering questions

See the "Open questions" section of
[`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md). Most relevant
remaining items:

- [ ] `rcraid.sys` completion ISR (`FUN_14000d2b8`) — never decompiled.
      Confirm CQ entry layout vs. NVMe-standard.
- [ ] AHCI path bring-up (devices 0x7905 / 7916 / 7917 / 43BD) — not in
      MODULE_DEVICE_TABLE on purpose (claiming them would take the device
      from `ahci` and drive nothing); re-add them with the AHCI submit /
      complete path, implemented fresh from `rcbottom.sys`.

---

## Suggested next-PR order

The original daily-driver sequence (partition scan, fast discard, DKMS/udev,
suspend/resume, boot-from-RAID) shipped long ago. So did the first two items
of the previous version of this list — **hot-plug / degraded failover** and
**rebuild**, plus the sysfs half of **observability** — and **RAID1** is
hardware-validated (2026-07-10, the dev box boots from the mirror) and
**RAID10** is feature-complete pending its array-box run. The order below is
what is actually left; several items are now verification and documentation
rather than code.

1. **Rig metadata fixtures for RAID10/5/6** — `scripts/qemu-test/mkmeta.py`
   writes RAID 0/1 only, so the rig cannot boot a synthetic array at any other
   level. Implement the records (RAID10 first: `0x1BF6` + 2×2 is already proven
   by the X399 fixtures). This is fixture generation for the QEMU rig, not
   array lifecycle: no install path depends on it, because `install-livecd.sh`
   requires the array the UEFI RAIDXpert2 utility created (issue #16).
2. **Corroborate the RAID5/6 on-disk placement** (issue #6) — the read-only
   parity path assembles and reads: a firmware-created 3-disk RAID 5 was
   parsed, assembled and read on 2026-11-08 (`0x1BF5`, N=3, `(N-1)` × user
   size). That array no longer exists, so there is currently no
   firmware-written parity array to re-read — and it was zeroed at creation
   anyway, so even while it existed it carried no vendor-written data to
   check the placement against. The dedicated-parity placement in `src/patch_parity_math.c` is
   taken from the vendor core rather than inferred, and the RAID6 Q generator
   is now the vendor's EVENODD (issue #10) — still with no caller, because the
   full-row path that landed is RAID 5 P-only and RAID 6 has no writes at all.
   A staged fixture can only show that the
   driver reads the map it implements: re-stamped under the dedicated layout
   on 2026-11-08 it round-trips byte-identically, which is self-consistency,
   not corroboration. Only data written through RAIDXpert2/Windows can prove
   the firmware writes that map, so until such data is read back, trusting a
   vendor-written parity array rests on AMD's code rather than on observation.
3. **RAID10 array-box run** — [`VALIDATION_RAID10.md`](VALIDATION_RAID10.md)'s
   results table is empty and it is the only thing between a finished
   feature and promoting it in README/STATUS. Note the scope limit recorded
   in step 1 there: the adjacent-pair convention is corroborated for 2×2
   only, and a 6-member 3×2 would assemble on the same unverified
   assumption.
4. **SMART pass-through** — per-member health visibility (SMART log
   LID=0x02, temperature, wear). Small, isolated, no data-path risk, and
   currently the largest user-visible health gap.
5. **Control bridge → uevents → `rcraidctl`** — the ioctl transport is now
   registered and live; the remaining work is a CLI and uevents. uevents are
   the cheap half of monitoring once state transitions are already tracked.
6. **Multi-volume support** (the BIOS can carry multiple arrays; the
   committed-generation parser already sees every LD record — the volume
   layer is what's single-instance), the **writes-enabled-by-default latch**,
   the **signing script** (docs already cover MOK enrollment), then the
   remaining enterprise doc items (support matrix, runbook, durability
   statement) alongside.
7. **Kernel test suite integration** — blktests/xfstests in CI so the
   failure-handling and rebuild work above stays regression-backed. The QEMU
   rig covers assembly and the degraded paths, but nothing exercises the
   block layer the way blktests does.

Everything else can come after.
