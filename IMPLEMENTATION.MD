# Implementation Checklist — Path to Prime Time

Tracks what's left between the current driver and a daily-driver-quality
release. Grouped by priority: blockers first, then features, then polish.

Check items off as PRs land. Each item should resolve to a PR (or a
documented "won't fix" with reasoning).

For status of work already done, see [`docs/STATUS.md`](docs/STATUS.md).

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

- [ ] **Module signing for Secure Boot** — `insmod` fails with
      "Key was rejected by service" when SB is enabled. Ship signing
      scripts (or document MOK enrollment); for distro builds, sign
      with the kernel's ephemeral key.
      *Touch points:* new `scripts/sign-module.sh`, INSTALL.md.

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

- [ ] **Suspend / resume (S3 / S4)** — `.suspend` / `.resume` PM
      callbacks at the PCI driver level. Quiesce blk-mq, save SQ/CQ
      DMA addresses, disable controller, restore on resume.
      Untested today; will probably hang or corrupt.

- [ ] **Hot-plug / drive failure handling** — pulling a member
      drive today will likely oops (NULL ptr on stale `rcraid_dev`).
      Add `.remove` cleanly tearing down a member, mark volume
      degraded (no-op for RAID0 = volume fails), drain pending I/O
      with `BLK_STS_IOERR`.

- [ ] **FUA / Flush / barrier semantics** — Flush command exists;
      FUA bit on write SQEs is not set even when bio has REQ_FUA.
      Databases / filesystems expecting durability see ack-before-
      durable. Wire `cmd_flags & REQ_FUA` → SQE DW12 bit 30.

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

- [ ] **Doc consolidation** — `docs/` has STATUS.md, OPEN_QUESTIONS.md,
      GHIDRA_FINDINGS_2026.md, ERROR_HANDLING.md plus archives.
      Consolidate the active ones into one "Developer Guide" and
      move the rest to `docs/archive/`.

---

## 🚀 Performance — diminishing returns beyond ~75 % of NAND spec

Current measured (`fio --direct=1` sustained, 2× Crucial T700, 256 KiB stripe):

| Pattern | We get | Spec ceiling | Notes |
|---|---:|---:|---|
| SEQ1M Q8T1 read | 20.2 GB/s | ~24 GB/s | 84 % — close to wall |
| SEQ1M Q8T1 write | 8.9 GB/s | ~12 GB/s | 74 % — NAND-limited |
| RND4K Q32T16 read | 6.2 GB/s (1.5 M IOPS) | ~7 GB/s | excellent |
| RND4K Q1T1 read | 96 MB/s | ~110 MB/s | latency-bound |

Windows CDM's headline 22 GB/s sequential write is a benchmark artifact
(CDM uses tiny test sizes that fit in the drive's SLC cache). We're not
actually slower than Windows for real sustained workloads — we just
measure honestly.

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

See [`docs/OPEN_QUESTIONS.md`](docs/OPEN_QUESTIONS.md). Most relevant
remaining items:

- [ ] `rcraid.sys` completion ISR (`FUN_14000d2b8`) — never decompiled.
      Confirm CQ entry layout vs. NVMe-standard.
- [ ] AHCI path bring-up (devices 0x7905 / 7916 / 7917 / 43BD) —
      MODULE_DEVICE_TABLE already claims them; the AHCI submit /
      complete path needs to be implemented fresh from `rcbottom.sys`.

---

## Suggested next-PR order

1. Partition table scan (fastest unblock for daily use)
2. Fast discard (fix mkfs hang)
3. DKMS + udev autobind (so reboot doesn't break the array)
4. Suspend / resume
5. RAID1 support (first redundancy level)
6. Module signing path
7. Multi-volume support
8. Hot-plug handling

Everything else can come after.
