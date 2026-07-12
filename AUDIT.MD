# Codebase Audit — rcraid (AMD RAID driver)

**Date:** 2026-07-11
**Scope:** All driver C sources (`rc_*.c`, `rc_linux.h`, `rc_pci_ids.h`), build system (`Makefile`, `dkms.conf`), all shell scripts (build/install/uninstall/test/bench/boot-safety), packaging, CI workflows, and user-facing docs. Ghidra output under `docs/ghidra_output/` was treated as reference material, not audited code.
**Method:** Three independent senior-level review passes (core NVMe/RAID engine; driver scaffolding + build; scripts/packaging/docs), findings verified against surrounding code before inclusion.

---

## Executive summary

The driver's happy path is in genuinely good shape: the metadata parser validates bounds/checksums/commit linkage carefully, dangerous paths fail closed (write veto on untrusted geometry), the completion-claim protocol and sync-CID handshake show real races found and fixed empirically, and CI for the C code (multi-distro builds, warning gates, QEMU rigs) is strong for a hobby driver.

The risk is concentrated in three areas:

1. **Device-lifecycle is unfinished.** `rc_nvme_cleanup_controller()` is dead code — nothing calls it from PCI remove or probe error paths. Every unbind/hot-remove leaks IRQs and DMA memory, leaves the controller enabled, and (worse) frees an adapter that the assembled volume still dereferences: a use-after-free on the I/O path.
2. **Shared submission queues have no depth accounting.** blk-mq tags, cache fills, prefetches, and sync commands all share per-queue SQs sized exactly at the tag count; the SQ can be overrun, which corrupts in-flight commands silently.
3. **The boot-safety tooling doesn't deliver what the docs promise.** `test_driver.sh` can hand the running OS disk to the driver via a "Samsung" brand heuristic; `verify-boot-safety.sh` reports false results on dracut distros; the apt kernel-hold protection is never installed anywhere.

RAID1 is correct in the fast path but has **no degraded mode**: one dead member fails the whole volume, and a half-failed mirror write leaves the copies silently divergent with no tracking or resync. It currently provides redundancy of data at rest, not availability.

**Recommended priority order:** C-1/C-2/C-3 (lifecycle), C-4 (SQ overrun), S-1 (test_driver.sh unbind), then the High items.

---

## Critical

### C-1. PCI remove never tears down the NVMe controller — `rc_nvme_cleanup_controller()` has zero callers outside rc_nvme.c
`rc_bottom.c:470-488`. `rc_bottom_remove()` only calls the stub `rc_hw_cleanup()` then `kfree(adapter)`. Per-I/O-queue IRQs registered at `rc_nvme.c:1315` are still installed when `pci_free_irq_vectors()` runs (kernel WARN, vectors freed with live actions); the controller is left with CC.EN=1; admin/IO queue DMA memory leaks on every unbind. Guaranteed splat on every module unload with an active adapter.

### C-2. Hot-unbinding one member frees the adapter while the volume still references it (use-after-free)
`rc_bottom.c:470-488` + `rc_nvme.c:4914`. `rc_volume_teardown()` is only called from `rc_exit()` (`rc_main.c:92`); `rc_bottom_remove()` never checks membership in `rc_volume_members[]` before `kfree(adapter)`. Surprise-remove or sysfs unbind of one NVMe member while `/dev/rcraid0` is mounted → next `rc_volume_queue_rq` dereferences the freed adapter and submits into freed `io_queues[]` → oops or silent corruption. Full `rmmod` is safe only by init/exit ordering luck; single-device removal (including AER-triggered) is not.

### C-3. NVMe init failure during probe is treated as non-fatal
`rc_bottom.c:430-434`, `rc_firmware.c:78-84`. If `rc_nvme_init_controller()` fails midway (admin queue up, some IRQs requested), probe still returns 0, exports sysfs/debugfs on a half-dead adapter, and no error path releases the partially-allocated NVMe resources.

### C-4. No SQ-full check — submission queues can be overrun, corrupting live commands
`rc_nvme.c:1471-1487` (submit) + tagset sizing at `rc_nvme.c:4684-4713`. `sq_tail` advances unconditionally. The tag pool (`RC_VOLUME_QUEUE_DEPTH = 256` per hctx) equals the SQ depth, but (a) NVMe queues are full at depth−1, (b) cache fills, prefetches, and sync commands share the same SQs *on top of* the 256 tags, and (c) `rc_nvme_create_one_io_queue` clamps depth to `CAP.MQES` (`rc_nvme.c:1232-1234`) while the tagset stays at 256. Under full tag load plus a cache fill — or on any controller with MQES < 256 — the submitter wraps over unconsumed SQEs → the controller executes corrupted/duplicate commands → silent data corruption. Reachable with stock settings by a legal workload.

### S-1. `test_driver.sh` protects the OS disk with a "Samsung" brand heuristic, then loads the driver unfiltered
`test_driver.sh:373-399, 422`. TEST 2.5 unbinds every `1022:b000` device from `nvme` unless the subsystem string contains "samsung", then TEST 3 runs `insmod rcraid.ko` with no `safe_subsys_vendor` argument (the filter defaults to disabled, `rc_bottom.c:33`, and insmod bypasses modprobe.d). On any box whose OS NVMe isn't Samsung, the step `INSTALL.md:90` tells every user to run unbinds the root disk and hands it to the experimental driver — instant loss of the running root device. This bypasses the entire subsystem-vendor safety mechanism the installers carefully build.

### S-2. `verify-boot-safety.sh` reports the opposite of reality on dracut distros
`scripts/verify-boot-safety.sh:71-78`. Hardcodes the Debian initrd path (`/boot/initrd.img-$K`) and `lsinitramfs`; on Fedora/RHEL/Arch the command-not-found is swallowed by `2>/dev/null | grep -q` and every kernel falsely FAILs "rcraid MISSING — this kernel will NOT boot", with a wrong fix hint (`update-initramfs`). The tool that exists to prevent bricked boots is unusable on half the supported matrix — and no user-facing doc mentions it exists at all.

---

## High

### H-1. Timeout/drain reclaims DMA mappings while the controller may still own them
`rc_nvme.c:3865-3892, 3950-4004`. The `.timeout` path only sets `dead=true` (controller disable happens later, asynchronously in reset work), then drain does `dma_unmap_sg` + `blk_mq_end_request` while the command is still live in hardware. A late DMA hits unmapped IOVAs (IOMMU fault) or, without an IOMMU, recycled pages → silent corruption of unrelated memory. Mainline nvme disables the controller before reclaiming requests.

### H-2. Cache-fill waiter can complete the wrong (reallocated) request
`rc_nvme.c:3459-3462, 4395-4441`. If a waiter request is ended by timeout or drain, nothing clears `e->waiter`; when the fill's CQE finally arrives, `rc_cache_isr` completes whatever request now owns that tag — possibly one not yet submitted. Double/mis-completion. (Opt-in: `cache_entries>0`.)

### H-3. Prefetch invalidation on write is per-hctx only — stale-data reads
`rc_nvme.c:3344-3346, 3618-3620`. A write on hctx A invalidates only that hctx's prefetch slots; a read on hctx B covering the same LBAs is served from the stale buffer. Classic write-then-read consistency violation. (Opt-in: `prefetch=1`. The stripe cache invalidates globally — the asymmetry confirms the gap.)

### H-4. Admin path has no stale-CQE defense (CID always 0)
`rc_nvme.c:890-935`. After an admin timeout, the late CQE is consumed by the *next* admin command as its own completion (same CID, matching phase), attributing the wrong status and desyncing `admin_cq_head`. The I/O sync path got exactly this fix (rolling `sync_seq`, lines 205-223); the admin path did not.

### H-5. `ctrl_mode` set to NVMe before NVMe state is initialized, with a shared IRQ already live
`rc_firmware.c:53` + `rc_bottom.c:422`. On legacy shared INTx, an interrupt from another device on the line between those two points routes into `rc_nvme_irq()` against zeroed state → NULL deref in hard-IRQ context. Set `ctrl_mode` after init succeeds, or request the IRQ last. Related: `rc_hw_init` (`rc_hw.c:26`) memsets `adapter->hw` *after* interrupts are live and never `spin_lock_init()`s `pending_lock`/`irq_lock`/`sync_lock` — lockdep splat via debugfs `pending_requests`.

### H-6. `uninstall-dkms.sh` has no root-on-array guard
`uninstall-dkms.sh:14-50`. The only protection is the incidental EBUSY from `rmmod`. Run from a rescue boot (module not loaded) against a system whose root is on `/dev/rcraid0pN`: it removes the DKMS module and rebuilds the initramfs without rcraid → next boot cannot mount root. Needs a `findmnt -no SOURCE /` check or loud confirmation.

### H-7. The apt kernel-hold protection is dead code — never installed
`packaging/apt.conf.d/52-rcraid-kernel-hold`. Neither installer copies it to `/etc/apt/apt.conf.d/` and no doc references it. The exact failure the file's own comment describes (unattended kernel upgrade → DKMS build breaks → initramfs lacks rcraid → unbootable, unattended) is unprotected on every installed system.

### H-8. `install-dkms.sh` reports success when initramfs regeneration fails
`install-dkms.sh:217-231`. `update-initramfs`/`dracut` failure (full `/boot` is common) only WARNs; the script exits 0 saying "Install complete… Next: reboot to test." For a boot-critical component this must be a hard failure.

---

## Medium

- **M-1. No RAID1 degraded mode** (`rc_nvme.c:2706-2714, 3098-3229, 3262-3269`). Any dead member fails the whole volume (`rc_volume_any_member_dead` fast-fails everything), so RAID1 offers zero availability benefit. A half-failed mirror write leaves silently divergent copies with no dirty-region tracking or resync, and the round-robin balancer (`rc_nvme.c:2113-2119`) will alternate between divergent copies on retry. Docs should state this until fixed.
- **M-2. Abort always targets SQ 1** (`rc_nvme.c:3986-3989`). Requests on hctx > 0 can never be aborted, and an unrelated live command on queue 1 sharing the tag value can be wrongly aborted.
- **M-3. Cache fill always reads 1 MiB** (`rc_nvme.c:4351-4389`; guard at 3455 checks one stripe only). Near end-of-volume the physical read runs past the namespace → LBA Out of Range → a perfectly valid READ is ended `BLK_STS_IOERR`.
- **M-4. No CC.SHN shutdown notification on suspend/cleanup** (`rc_nvme.c:5279-5340, 5548-5570`). Driver enables WCE yet performs unsafe shutdowns per NVMe spec; unsafe-shutdown SMART counters climb every suspend/reboot.
- **M-5. `rc_volume_teardown` frees cache/prefetch DMA buffers with fills possibly in flight** (`rc_nvme.c:4914-4950`). Fills aren't blk-mq requests so `del_gendisk` doesn't wait for them; controllers are disabled only later → DMA into freed memory on unload.
- **M-6. Reset-path memset of SQ/CQ without `q->lock`** (`rc_nvme.c:5064-5069, 5111-5124`). Narrow race with a queue_rq that passed the dead-check pre-quiesce; low probability, corrupting consequence.
- **M-7. Adapter slot table leaks slots forever** (`rc_bottom.c:354-376`). `num_adapters` never decrements; 11 bind/unbind cycles exhaust `RC_MAX_ADAPTERS`, then attach fails silently while probe returns success with `instance` = 0, colliding with a real adapter's debugfs dir.
- **M-8. Three of five claimed PCI IDs always fail probe** (`rc_bottom.c:152-181` vs `rc_pci_ids.h`). 0x7905/0x7916/0x7917 hit "Unknown device ID" → the driver claims the device away from ahci, then binds nothing.
- **M-9. `debugfs_create_dir()` checked with `!ptr` instead of `IS_ERR()`** (`rc_debugfs.c:291, 316`); `regs_show` (`rc_debugfs.c:253-267`) does unconditional MMIO reads with no length/PM/reset awareness — reading `registers` on a D3 device can fault.
- **M-10. `dkms.conf` problems.** `REMAKE_INITRD` is removed in dkms ≥ 3.0; `PACKAGE_VERSION="0.1.0"` is frozen while the driver version is 9.3.3.x, so different source versions install into the same dkms tree. Also, future DKMS autoinstalls never regenerate the initramfs on dracut systems (kernel-update ordering not guaranteed) → new kernel may boot without rcraid.
- **M-11. `install-livecd.sh` builds for only the newest kernel in the target** (`install-livecd.sh:372`); if the target stages two kernels (GA + HWE), first boot into the other has no rcraid. Also Phase 2 re-runs die on an unguarded `dkms add` (`:435`).
- **M-12. No shellcheck/bats gate in CI** for the ten boot-critical shell scripts; C code is well-gated but the code that can actually brick boots merges unlinted. checkpatch.pl is fetched unpinned from `torvalds/linux master` with `|| true` (`ci.yml:245-249`).
- **M-13. `build.sh` needlessly requires root, lacks `set -e`, and runs `make clean` before validating KERNELDIR** (`build.sh:10-17, 48`).
- **M-14. `rmmod` paths rely on EBUSY as the only safety** (`unload.sh:30`, `test_driver.sh:417-419`; the latter does `rmmod || true` then `insmod`, proceeding into confused state on a busy device).

---

## Low

- **L-1.** `debug_level` module parameter is dead: `rc_printk()` ignores its level and prints everything at `KERN_INFO` (`rc_main.c:24`, `rc_linux.h:95`); RC_ERROR never reaches `KERN_ERR`.
- **L-2.** sysfs: IRQ mode reports "Legacy" for MSI-X (`rc_sysfs.c:46`); `sprintf` instead of `sysfs_emit` throughout; deprecated `-ENOTSUPP` leaked to userspace (`rc_sysfs.c:180`); `pending_requests_show` iterates without `pending_lock` (`rc_sysfs.c:209`).
- **L-3.** `rc_config_write` swallows arbitrary writes and returns success (`rc_config.c:46`), misleading userspace tools.
- **L-4.** `pdu->hctx_idx = 0xff` sentinel later used as an unbounded array index (`rc_nvme.c:3253-3256` vs `:3443`) — unreachable today only because `nr_hw` is clamped.
- **L-5.** `pdu->sc_sct` written racily by concurrent member ISRs on fan-out ops (`rc_nvme.c:407`, comment admits it) — cosmetic log tearing.
- **L-6.** Reset fails permanently if the controller grants fewer queues than before (`rc_nvme.c:5174-5188`) where degrading would be recoverable; on-disk `ChunkSize` used verbatim with no sanity bound (`rc_nvme.c:2273`).
- **L-7.** Makefile fallback `find /usr/src … | xargs sh -c` path is fragile (`Makefile:54-56, 69-71`); `bench.sh` writes a predictable root-owned `/tmp` path (`bench.sh:61`) and its hex `case` pattern relies on `\6` accidentally escaping to `6` (`bench.sh:47`); `install-livecd.sh:293-295` sources `os-release` from the target disk as root (use awk parsing).
- **L-8.** Repo hygiene: stale `.o`/`.mod`/`.cmd` files sit in the working tree (ignored, not tracked — `make clean` removes them), but `.rcraid_rev` is **not** in `.gitignore` and a stale one would silently override `git describe` (Makefile version resolution).

## Docs vs. code

- README/STATUS claim boot-from-RAID is "validated on hardware" with dracut support, while the repo's own boot verifier can't pass on dracut (S-2) and the kernel-hold protection is uninstalled (H-7). The operational-safety story documented is stronger than what ships.
- RAID1 is advertised as shipped; per M-1 it lacks degraded operation and resync — worth an explicit limitation note.
- INSTALL.md's Secure Boot section is accurate and `scripts/lib/secure-boot.sh` is genuinely well done (fail-closed, pipefail-aware).

---

## Recommended remediation order

1. **Lifecycle (C-1..C-3):** call `rc_nvme_cleanup_controller()` from `rc_bottom_remove()` and probe error paths; refuse (or safely handle) unbind of a live volume member; make NVMe init failure fatal in probe. Fixes all crash-grade scaffolding issues.
2. **SQ accounting (C-4):** track in-flight SQEs per queue (tags + internal commands) and size the tagset below `min(MQES, depth−1)` minus internal-command headroom.
3. **Boot safety (S-1, S-2, H-6..H-8):** replace test_driver.sh's brand heuristic with the subsystem-vendor filter + explicit module param; make verify-boot-safety.sh dracut-aware and document it; add root-on-array guard to uninstall; actually install the apt kernel hold; hard-fail on initramfs regeneration errors.
4. **Timeout/teardown DMA safety (H-1, H-2, M-5):** disable the controller before reclaiming requests; clear `e->waiter` on any waiter completion; drain fills before freeing their buffers.
5. **Consider deleting or quarantining the opt-in cache/prefetch layers** — they contribute most of the residual race surface (H-2, H-3, M-3, M-5) for modest benefit.
6. **CI:** add shellcheck for all scripts; pin checkpatch.pl.
