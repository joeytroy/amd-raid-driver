// SPDX-License-Identifier: GPL-2.0-only
#include "patch_prototypes.h"
/*
 * AMD-RAID Linux driver — NVMe controller + RAID0/RAID1 volume I/O path
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * The live code path for PCI device 1022:B000 (NVMe RAID Bottom):
 * controller bring-up per NVMe 1.4, admin queue, Identify, I/O queue
 * creation, the volume-metadata reader and parser, and the blk-mq
 * READ/WRITE dispatch into per-member NVMe READ/WRITE commands.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See docs/RE_METHODOLOGY.md.
 */

/****************************************************************************
 * NVMe controller boot sequence (kept inline as documentation)
 *
 * For DEV_B000 (and any other CC_0108 device) the Windows AMD driver takes
 * its nvme.c code path, not the ahci.c one. See docs/REVERSE_ENGINEERING.md.
 * This file implements the standard NVMe 1.4 controller boot sequence:
 *
 *   1. Read CAP, snapshot MQES / DSTRD / TO.
 *   2. Disable controller (CC.EN = 0); wait for CSTS.RDY = 0.
 *   3. Allocate admin SQ + CQ (DMA-coherent, 4 KiB aligned, zeroed).
 *   4. Program AQA / ASQ / ACQ.
 *   5. Set CC (4 KiB pages, NVM cmd set, IOSQES=6, IOCQES=4, EN=1).
 *   6. Poll CSTS until RDY = 1 (or CFS = 1, or CAP.TO * 500 ms elapses).
#include "patch_prototypes.h"
 *
 * No commands are submitted from here. Once the controller is ready the
 * higher layers can issue Identify / Set Features / I/O queue creation.
 ****************************************************************************/

#include "rc_linux.h"
#include "amd_raid_uapi.h"
#include "rc_meta_build.h"
#include <linux/unaligned.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/mempool.h>
#include <linux/vmalloc.h>
#include <linux/seq_file.h>
#include <linux/semaphore.h>

/* Trivial global volume registry — assumes a single RAID0 volume.  Members
 * are inserted as their metadata validates.  Ordered by PCI BDF so the
 * stripe mapping is deterministic; this is a guess at member position
 * until the metadata field that encodes the explicit position is decoded
 * via further RE.  Tracked under rc_volume_lock. */
#define RC_VOLUME_MAX_MEMBERS	8
static struct rc_adapter *rc_volume_members[RC_VOLUME_MAX_MEMBERS];
/* Per-member physical sector offset where user data begins.  Populated from
 * RC_LogicalElement_LE.UserDataOffset (+0x20) when the LD parser matches.
 * Added to every (member, phys) tuple returned by rc_volume_map_lba so the
 * volume's logical LBA 0 actually lands on the first user-data sector of
 * the member, not on member-LBA 0 (which holds AMD-RAID metadata). */
static u64 rc_volume_member_phys_offset[RC_VOLUME_MAX_MEMBERS];
static int rc_volume_member_count;
static u32 rc_volume_stripe_sectors;
/* RAID level of the assembled volume — the NORMALIZED level (RC_LDT_RAID0 /
 * RC_LDT_RAID1) derived by rc_ld_level_from() from the on-disk DeviceType +
 * FirstCount x SecondCount, NOT the raw DeviceType (real firmware writes
 * 0x1BF6 for both levels).  Seeded by the first registered member
 * (RC_LDT_RAID0 for the legacy no-LD fallback); later members must match or
 * are rejected, same contract as stripe_sectors/expected_members.
 * Everything level-specific (map_lba, mirror fan-out in queue_rq) keys off
 * this. */
static u32 rc_volume_raid_level;
/* RAID10 geometry: number of stripe columns (= FirstCount), i.e. the number
 * of 2-way mirror pairs the logical space is striped across.  Member slots
 * 2*c and 2*c+1 are the mirror pair for column c; logical stripe N lives in
 * column (N % cols) at physical stripe (N / cols) on BOTH members of the
 * pair.  Set from the on-disk LogicalDevice record when the first member of
 * a RAID10 volume registers; 0 for every other level. */
static u32 rc_volume_raid10_cols;
/* RAID1 read balancer: monotonically increasing cursor; each read maps to
 * member (cursor % nmembers).  Plain round-robin — both mirrors see half
 * the reads, which on identical members approximates the 2x read speedup
 * without tracking per-member queue depth. */
static atomic_t rc_volume_rr_next;
/* Geometry-trust reason.  NULL while every member was placed from the
 * deterministic on-disk Logical Device (ld_my_position + LD-derived stripe).
 * Set to a human-readable cause by whichever site revokes trust.  The two
 * triggers — a legacy BDF fallback (LD parse failed) and an unrecognized
 * RAID0 chunk_index (LD parsed fine) — have DIFFERENT root causes and
 * remedies, so the reason is recorded here instead of hard-coded at the
 * message site.  A non-NULL reason vetoes writes at create_disk time
 * regardless of the enable_writes opt-in (a wrong member order or stripe
 * size silently corrupts the array on write). */
static const char *rc_volume_geometry_untrust_reason;
static DEFINE_MUTEX(rc_volume_lock);

/* Per-member availability state (RAID1 degraded mode, issue #51).
 *
 * LIVE          — serving reads and receiving writes.
 * FAILED        — dead or removed; excluded from all dispatch.
 * NEEDS_RESYNC  — controller recovered (auto/manual reset) after missing
 *                 writes, or re-plugged.  Its data is stale; it must NOT
 *                 rejoin dispatch until a resync copies the survivor over
 *                 it.  The rebuild scheduler picks these up one at a time.
 * RESYNCING     — reserved for the resync engine (receives writes, serves
 *                 no reads).
 *
 * rc_volume_member_state[] is the authoritative record, written under
 * rc_volume_lock (or via WRITE_ONCE from the mark-failed fast path — the
 * mutex protects compound transitions, not the single store).
 * rc_volume_live_mask is the lock-free hot-path mirror: bit i set iff
 * state[i] == LIVE.  Dispatch paths read ONLY the mask; degraded RAID1
 * keeps serving from whatever bits remain.
 *
 * All of this is in-memory only.  The driver never writes RAIDCore
 * metadata (firmware/BIOS own it), so degraded state does not survive a
 * module reload — a reload behind a stale mirror re-assembles it as if
 * in sync.  Documented limitation until on-disk state is decoded. */
enum rc_member_state {
	RC_MEMBER_LIVE,
	RC_MEMBER_FAILED,
	RC_MEMBER_NEEDS_RESYNC,
	RC_MEMBER_RESYNCING,
};
static enum rc_member_state rc_volume_member_state[RC_VOLUME_MAX_MEMBERS];
static atomic_t rc_volume_live_mask;
/* Write-fan-out mask: live members PLUS members currently resyncing.  A
 * resyncing member receives every application write (so blocks behind
 * the resync cursor stay current) but serves no reads (its data is not
 * yet trustworthy).  live_mask ⊆ write_mask always. */
static atomic_t rc_volume_write_mask;

/* ------------------------------------------------------------------ *
 * Resync engine state (issue #51 part 3).
 *
 * One resync at a time (a 2-way mirror has at most one stale member).
 * The kthread copies the survivor over the target in
 * RC_RESYNC_XFER_SECTORS chunks.  Concurrent application writes fan out
 * to the target too (write_mask), so the only ordering hazard is:
 *   resync READ (old data) → app WRITE lands on target → resync WRITE
 *   overwrites it with the old data.
 * Closed by a moving exclusion window + write-generation drain:
 *   - writes overlapping [window, window+XFER) return DEV_RESOURCE
 *     (blk-mq retries; the window moves on within milliseconds);
 *   - before copying a chunk the thread publishes the window, flips the
 *     write generation, and waits for all writes counted under the OLD
 *     generation to complete — those may have checked the window before
 *     it was published.  Writers increment their generation counter
 *     BEFORE checking the window, so every write is either (a) counted
 *     in the old generation and drained here, or (b) checked the window
 *     after publication and bounced.  See rc_volume_dispatch_mirror.
 * ------------------------------------------------------------------ */
#define RC_RESYNC_XFER_SECTORS	256u		/* 128 KiB per copy op */
#define RC_RESYNC_XFER_BYTES	(RC_RESYNC_XFER_SECTORS * 512u)
#define RC_RESYNC_MAX_RETRIES	10		/* per chunk, transient errors */
#define RC_RESYNC_WINDOW_NONE	(~0ULL)

static struct task_struct *rc_volume_resync_thread;	/* under rc_volume_lock */
static bool rc_volume_resync_done;	/* thread finished, awaiting reap */
static bool rc_volume_tearing_down;	/* under rc_volume_lock: no new resyncs */
static int  rc_volume_resync_slot = -1;	/* target slot, -1 = idle */
static atomic64_t rc_volume_resync_cursor;	/* progress, per-member sectors */
static u64  rc_volume_resync_total;		/* per-member sectors */
static atomic64_t rc_volume_resync_window = ATOMIC64_INIT(RC_RESYNC_WINDOW_NONE);
static atomic_t rc_volume_wgen;
static atomic_t rc_volume_wgen_count[2];

/* Serialized rebuild scheduler (defined near rc_volume_member_readmitted).
 * Declared here so the resync thread's exit path can re-arm it. */
static void rc_volume_rebuild_fn(struct work_struct *w);
static DECLARE_WORK(rc_volume_rebuild_work, rc_volume_rebuild_fn);

/* Throttle between copy chunks (0 = full speed).  Runtime-tunable. */
static unsigned int rc_volume_resync_delay_ms;
module_param_named(resync_delay_ms, rc_volume_resync_delay_ms, uint, 0644);
MODULE_PARM_DESC(resync_delay_ms,
		 "Delay in ms between 128 KiB resync copy chunks (0 = full speed).");

static const char *rc_member_state_name(enum rc_member_state st)
{
	switch (st) {
	case RC_MEMBER_LIVE:         return "live";
	case RC_MEMBER_FAILED:       return "failed";
	case RC_MEMBER_NEEDS_RESYNC: return "needs-resync";
	case RC_MEMBER_RESYNCING:    return "resyncing";
	}
	return "?";
}

/* Deferred degrade work: after a member is marked failed from IRQ/hot-path
 * context, in-flight fan-out requests waiting on it must be drained
 * (rc_volume_drain_dead sleeps — H-1 controller disable), and auto-reset
 * should be considered.  Runs in process context. */
static void rc_volume_degrade_fn(struct work_struct *w);
static DECLARE_WORK(rc_volume_degrade_work, rc_volume_degrade_fn);

/* Degraded assembly (issue #51 part 2, opt-in).  With this set, a RAID1
 * volume whose member set is still incomplete RC_VOLUME_ASSEMBLE_DELAY_MS
 * after the last member registered assembles anyway — the missing slot
 * is marked failed/absent and the volume comes up degraded.  Off by
 * default: auto-assembling half a mirror risks silent split-brain if the
 * "missing" member reappears later (in-memory state can't tell which
 * half is newer), so booting a broken mirror is an explicit choice. */
static int rc_volume_allow_degraded;
module_param_named(allow_degraded, rc_volume_allow_degraded, int, 0444);
MODULE_PARM_DESC(allow_degraded,
		 "If non-zero, assemble a RAID1 volume degraded when a member is missing at load time (default 0). Risk: if the missing member later reappears with diverged data, nothing detects it.");

#define RC_VOLUME_ASSEMBLE_DELAY_MS	10000

static void rc_volume_assemble_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(rc_volume_assemble_work, rc_volume_assemble_fn);

/* nr_hw_queues the tagset was created with — the constraint a re-added
 * member must satisfy (each hctx routes to io_queues[hctx] on every
 * member).  Set once in rc_volume_create_disk. */
static u32 rc_volume_nr_hw;

/* Hook: `slot` just re-entered the registry parked in NEEDS_RESYNC (hot
 * re-plug or recovered controller).  The resync engine starts the
 * rebuild from here.  Caller holds rc_volume_lock. */
static void rc_volume_member_readmitted(int slot);
static int  rc_volume_alloc_member_dma(int slot, u32 nr_hw);
static void rc_volume_free_member_dma(int slot, struct device *dev);
static void rc_volume_sysfs_register(void);
static void rc_volume_sysfs_unregister(void);

/* Slow-path state transition.  Caller holds rc_volume_lock. */
static void rc_volume_member_set_state(int idx, enum rc_member_state st)
{
	enum rc_member_state old = READ_ONCE(rc_volume_member_state[idx]);

	/* No early-out on old == st: the zero-initialized state array reads
	 * as LIVE before any member registers, so the first LIVE transition
	 * must still sync the mask bit.  READ_ONCE/WRITE_ONCE because
	 * rc_volume_member_mark_failed() stores this element lock-free from
	 * ISR context concurrently with lock-holding accessors. */
	WRITE_ONCE(rc_volume_member_state[idx], st);
	if (st == RC_MEMBER_LIVE)
		atomic_or(BIT(idx), &rc_volume_live_mask);
	else
		atomic_andnot(BIT(idx), &rc_volume_live_mask);
	if (st == RC_MEMBER_LIVE || st == RC_MEMBER_RESYNCING)
		atomic_or(BIT(idx), &rc_volume_write_mask);
	else
		atomic_andnot(BIT(idx), &rc_volume_write_mask);
	/* mark_failed() may have installed FAILED between the state store
	 * above and the mask ORs — its own mask-clears would then predate
	 * our ORs (no-ops), leaving zombie mask bits on a FAILED member
	 * that every subsequent write would fan out to.  Re-check and honor
	 * the failure; if mark_failed's cmpxchg instead lands after this
	 * read, its success path clears the masks itself, so every
	 * interleaving ends with FAILED + clear bits. */
	if (st == RC_MEMBER_LIVE || st == RC_MEMBER_RESYNCING) {
		smp_mb__after_atomic();
		if (READ_ONCE(rc_volume_member_state[idx]) ==
		    RC_MEMBER_FAILED) {
			atomic_andnot(BIT(idx), &rc_volume_live_mask);
			atomic_andnot(BIT(idx), &rc_volume_write_mask);
			return;
		}
	}
	if (old == st)
		return;
	rc_printk(RC_NOTE,
		  "rc_volume: member %d (%s) state %s -> %s (live_mask=0x%x)\n",
		  idx,
		  rc_volume_members[idx] ?
			pci_name(rc_volume_members[idx]->pdev) : "absent",
		  rc_member_state_name(old), rc_member_state_name(st),
		  atomic_read(&rc_volume_live_mask));
}

/* True while the volume can still serve I/O at its configured level:
 *   RAID1 : at least one mirror member is live.
 *   RAID10: every column still has at least one live copy — one failure per
 *           pair is tolerated, losing a whole pair is fatal.
 *   RAID0 : every member is live (each stripe crosses all of them).
 *   RAID5 : one failed member is tolerated — reads reconstruct that
 *           member's chunks from parity plus the surviving chunks.
 *           Losing a second member is fatal.
 *   RAID6 : every member is live (no dual-parity reconstruction yet).
 * Reads and writes both require this; a RAID10 column with no live member
 * would leave that half of the logical space unmapped. */
static bool rc_volume_can_serve(void)
{
	unsigned int live = (unsigned int)atomic_read(&rc_volume_live_mask);

	if (rc_volume_raid_level == RC_LDT_RAID1)
		return live != 0;
	if (rc_volume_raid_level == RC_LDT_RAID10) {
		u32 c, cols = rc_volume_raid10_cols;

		if (!cols)
			return false;
		for (c = 0; c < cols; c++) {
			unsigned int pair = BIT(2 * c) | BIT(2 * c + 1);

			if (!(live & pair))
				return false;
		}
		return true;
	}
	if (rc_volume_raid_level == RC_LDT_RAID5)
		return hweight32(live) + 1 >= (unsigned int)rc_volume_member_count;
	return live == (unsigned int)(BIT(rc_volume_member_count) - 1);
}

/* rc_volume_can_serve() against a hypothetical live mask with @slot
 * removed — used by rc_volume_remove_member() to decide whether a departing
 * member leaves the volume able to keep serving degraded. */
static bool rc_volume_can_serve_without(int slot)
{
	unsigned int live = (unsigned int)atomic_read(&rc_volume_live_mask);

	if (slot >= 0 && slot < RC_VOLUME_MAX_MEMBERS)
		live &= ~BIT(slot);
	if (rc_volume_raid_level == RC_LDT_RAID1)
		return live != 0;
	if (rc_volume_raid_level == RC_LDT_RAID10) {
		u32 c, cols = rc_volume_raid10_cols;

		if (!cols)
			return false;
		for (c = 0; c < cols; c++) {
			unsigned int pair = BIT(2 * c) | BIT(2 * c + 1);

			if (!(live & pair))
				return false;
		}
		return true;
	}
	if (rc_volume_raid_level == RC_LDT_RAID5)
		return hweight32(live) >= (unsigned int)rc_volume_member_count - 1;
	return live == (unsigned int)(BIT(rc_volume_member_count) - 1);
}

/* Fail a member from any context (ISR, timeout, queue_rq).  Clearing the
 * live bit is the load-bearing effect — it is what stops new dispatches —
 * and happens immediately; the state-array store is a single WRITE_ONCE
 * (the mutex only guards compound transitions).  Schedules the degrade
 * work to drain in-flight I/O waiting on this member. */
static void rc_volume_member_mark_failed(int idx)
{
	int prev;

	atomic_andnot(BIT(idx), &rc_volume_write_mask);
	prev = atomic_fetch_andnot(BIT(idx), &rc_volume_live_mask);

	if (!(prev & BIT(idx))) {
		/* Not live per the mask — but the failure must still land if
		 * the STATE says RESYNCING (drop out of the write fan-out)
		 * or LIVE (the resync completion, or set_state's own
		 * store-state-then-sync-mask gap, promoted the member
		 * concurrently: the mask bit lagging the state must not let
		 * a genuine failure be swallowed).  cmpxchg loop so whichever
		 * transition raced us is failed exactly once; the mask
		 * andnots below are re-applied because the completion path
		 * publishes its mask bits around its own cmpxchg. */
		enum rc_member_state old =
			READ_ONCE(rc_volume_member_state[idx]);

		while (old == RC_MEMBER_RESYNCING || old == RC_MEMBER_LIVE) {
			enum rc_member_state got =
				cmpxchg(&rc_volume_member_state[idx], old,
					RC_MEMBER_FAILED);
			if (got == old) {
				atomic_andnot(BIT(idx), &rc_volume_write_mask);
				atomic_andnot(BIT(idx), &rc_volume_live_mask);
				rc_printk(RC_ERROR,
					  "rc_volume: member %d FAILED while %s (live_mask=0x%x)\n",
					  idx, rc_member_state_name(old),
					  atomic_read(&rc_volume_live_mask));
				schedule_work(&rc_volume_degrade_work);
				break;
			}
			old = got;
		}
		return;
	}
	WRITE_ONCE(rc_volume_member_state[idx], RC_MEMBER_FAILED);
	rc_printk(RC_ERROR,
		  "rc_volume: member %d (%s) FAILED — %s (live_mask=0x%x)\n",
		  idx,
		  rc_volume_members[idx] ?
			pci_name(rc_volume_members[idx]->pdev) : "absent",
		  rc_volume_can_serve() ?
			"continuing degraded on surviving member(s)" :
			"volume can no longer serve I/O",
		  atomic_read(&rc_volume_live_mask));
	schedule_work(&rc_volume_degrade_work);
}

/* Conservatively hard-code expected member count = 2 (the user's RAID0).
 * A real driver would read this from the metadata; we haven't decoded
 * which field encodes it yet. */
#define RC_VOLUME_EXPECTED_MEMBERS	2

/* Member-ordering override: 0 = PCI BDF ascending (default), 1 = reverse.
 * Used only when the on-disk LD parser fails — normally member positions
 * come from RC_LogicalElement_LE indexing and this is ignored. */
static int rc_volume_reverse_order;
module_param_named(reverse_member_order, rc_volume_reverse_order, int, 0644);
MODULE_PARM_DESC(reverse_member_order,
		 "Fallback only (no LD parsed): if non-zero, sort members by descending PCI BDF");

/* Writes are read-only by default.  Setting this to 1 at load time exposes
 * /dev/rcraid0 as read-write and routes REQ_OP_WRITE through NVMe WRITE.
 * Off by default because a misconfigured stripe map or member ordering would
 * corrupt the array; verify reads behave correctly first. */
static int rc_volume_enable_writes;
module_param_named(enable_writes, rc_volume_enable_writes, int, 0444);
MODULE_PARM_DESC(enable_writes,
		 "If non-zero, expose /dev/rcraid0 as read-write (default 0). Verify the array contents look right via reads BEFORE enabling.");

/* RAID 5 writes.  Off by default so the installer path keeps delivering a
 * read-only volume until this path has had a hardware run.  A complete row
 * generates P directly from the request; a partial row is read-modify-
 * written.  Both are serialized by rc_volume_parity_lock. */
static int rc_volume_parity_writes;
module_param_named(parity_writes, rc_volume_parity_writes, int, 0444);
MODULE_PARM_DESC(parity_writes,
		 "If non-zero, allow RAID 5 writes: complete rows generate parity directly, partial rows read-modify-write the row (requires enable_writes=1).");

/*
 * RAID 5 CREATE writes a complete, firmware-shaped LD record, but it does
 * NOT zero or rebuild the members' user-data regions: the new volume would
 * expose whatever the disks held before, and parity would not describe it.
 * Keep that behind an explicit opt-in so a plain CREATE cannot turn stale
 * member bytes into a seemingly valid array.
 */
static int rc_volume_parity_create;
module_param_named(parity_create, rc_volume_parity_create, int, 0444);
MODULE_PARM_DESC(parity_create,
		 "If non-zero, allow CREATE to stamp a RAID 5 array over zeroed members. The writer emits metadata only: the data/parity region is not initialised, so only use freshly erased disks and zero the volume before writing. RAID 6 stays refused.");

/* Metadata write probe.  Off by default; the QEMU rig turns it on.  When
 * set, the driver rewrites a member's RC_MetaData block at LBA 0x5000
 * through its own NVMe queues, flushes, and reads it back — and refuses
 * to do so unless the block it is about to write is byte-identical to the
 * one already on media, so the probe cannot change what the array means.
 * This exercises the WRITE/FLUSH path and the metadata LBA without
 * touching the encoder question (that is rc_nvme_verify_meta_encoding). */
static int rc_meta_write_probe;
module_param_named(meta_write_probe, rc_meta_write_probe, int, 0444);
MODULE_PARM_DESC(meta_write_probe,
		 "If non-zero, verify the metadata write path at assembly by rewriting the member's own RC_MetaData block and reading it back (default 0).");

/* Destructive half of the probe, for disposable test arrays only: write a
 * different (junk DeviceId) block first to prove the media really changed,
 * then restore the real block and verify the restore.  Interrupting the
 * driver between those two writes leaves the member with a junk metadata
 * block, so never enable this on an array you intend to keep. */
static int rc_meta_write_probe_junk;
module_param_named(meta_write_probe_junk, rc_meta_write_probe_junk, int, 0444);
MODULE_PARM_DESC(meta_write_probe_junk,
		 "With meta_write_probe, also prove the write reaches media by writing a junk block and restoring it. Disposable arrays only (default 0).");

/* Stripe-size override for diagnostic probing.  Set at insmod time to force
 * the volume to use this many sectors per stripe, bypassing the derivation
 * from RC_LogicalDevice.ChunkSize / firmware default.  Zero = use derived
 * value (default).  Useful when the on-disk ChunkSize field doesn't encode
 * the real stripe and we need to try common values (32/64/128/256/512/1024
 * sectors = 16K/32K/64K/128K/256K/512K) until the partition table parses. */
static u32 rc_volume_stripe_override;
module_param_named(stripe_sectors_override, rc_volume_stripe_override, uint, 0444);
MODULE_PARM_DESC(stripe_sectors_override,
		 "Diagnostic: force stripe size in 512B sectors (0 = auto). Try 128 for 64KiB, 256 for 128KiB.");

/* gendisk state — at most one volume right now. */
static struct gendisk *rc_volume_disk;
static struct blk_mq_tag_set rc_volume_tagset;

/* Per-tag-per-member PRP-list buffer (allocated once at volume create).
 * Used for two distinct purposes — never simultaneously, since they
 * correspond to different NVMe opcodes:
 *
 *   - READ/WRITE > 2 pages: holds the PRP list entries the controller
 *     reads to walk through page 2..N of the transfer.
 *   - DSM Deallocate (DISCARD): holds the range list (one 16-byte entry).
 *
 * Per-tag-per-member because (a) the same tag may route to different
 * members on different requests, and (b) each member sits in its own
 * IOMMU domain so the DMA handle is only valid on the owning pdev.
 *
 * Data buffers themselves are no longer per-tag.  Hardware DMAs directly
 * to/from the bio's user pages via blk_rq_map_sg + dma_map_sg, and
 * rc_volume_build_prp enumerates the resulting scatterlist into PRPs.
 *
 * Total memory cost: PAGE_SIZE × QD × member_count = 2 MiB at QD=256
 * on a 2-member volume.  (Was ~33 MiB before the scatterlist-native
 * refactor, which dominated by the per-tag 512 KiB bounce buffers.)
 */
#define RC_VOLUME_DATA_PAGES	256
#define RC_VOLUME_DATA_BYTES	(RC_VOLUME_DATA_PAGES * PAGE_SIZE)
/* Each NVMe member command is bounded by MDTS=512 KiB on the T700.
 * For multi-member dispatch on a 2-member RAID0, a 1 MiB request maps
 * 512 KiB to each member, which is exactly 128 pages per member. */
#define RC_VOLUME_MS_PAGES_PER_MEMBER	128

/* RAID 5 full-row writes compute P into a driver-owned buffer before the
 * parity member's WRITE command is submitted.  A fixed pool bounds that
 * memory instead of allocating chunk-sized memory on the I/O path.  Only
 * the serialized write path takes a buffer, so an empty pool while that
 * lock is held is a leak, and the request fails with a logged error. */
#define RC_VOLUME_PARITY_BUFS		8

/* Bit i set while buffer i is owned by an in-flight request.  The bit
 * operations are atomic, so acquire/release need no lock. */
static void *rc_volume_parity_va[RC_VOLUME_PARITY_BUFS];
static unsigned long rc_volume_parity_inuse;
static u32 rc_volume_parity_buf_bytes;

/* RAID 5 partial-row read-modify-write.  Every RAID 5 write is serialized
 * by this semaphore: RMW reads the old row, patches and rewrites it, so a
 * second write to the same row that read the old bytes too would be lost.
 * One lock for the whole volume is coarse but trivially correct; the
 * async full-row path takes it with down(), which the BLK_MQ_F_BLOCKING
 * tagset allows.
 *
 * One chunk buffer per member (plus a PRP list) is enough because only one
 * RMW runs at a time.  Each buffer lives in its member's IOMMU domain.
 * The semaphore is released from the completion path for async full-row
 * writes; up() is legal there. */
struct rc_volume_rmw_buf {
	struct device *dev;
	void          *va;
	dma_addr_t     pa;
	__le64        *prp;
	dma_addr_t     prp_pa;
};
static struct rc_volume_rmw_buf rc_volume_rmw[RC_VOLUME_MAX_MEMBERS];
static u32 rc_volume_rmw_bytes;
static struct semaphore rc_volume_parity_lock;
/* SQ headroom reserved for internal (non-blk-mq) commands: boot/reset-time
 * sync commands (1 per queue).  Internal submitters additionally reserve their
 * SQE slots explicitly via rc_nvme_sq_reserve() and back off when the SQ
 * is full, so this headroom is a throughput knob (keeps steady-state tag
 * load from starving internal commands into constant back-off), not the
 * correctness mechanism. */
#define RC_VOLUME_SQ_INTERNAL_HEADROOM	16u
/* blk-mq tag pool size per hctx.  The per-queue NVMe SQ depth is
 * RC_NVME_IO_QUEUE_DEPTH (256, matching Windows), but an NVMe SQ is full
 * at depth-1 and internal commands share the same SQs on top of the tags,
 * so the tag pool must sit below depth-1 minus internal headroom — sizing
 * them EQUAL let a full tag load plus one internal command wrap the SQ tail
 * over live SQEs (silent corruption).  Further clamped at volume-create
 * time against the actual granted queue depths (CAP.MQES can be < 256).
 * Total in-flight requests = nr_hw_queues × this. */
#define RC_VOLUME_QUEUE_DEPTH	\
	(RC_NVME_IO_QUEUE_DEPTH - RC_VOLUME_SQ_INTERNAL_HEADROOM)
/* Max hardware queues (hctx) the volume exposes.  nr_hw_queues is the min
 * of every member's granted NVMe I/O queues, capped at
 * RC_NVME_IO_QUEUE_TARGET=4 in practice, but sized to 8 here as a ceiling.
 * Used to dimension every per-hctx array below. */
#define RC_VOLUME_MAX_HCTX		8u

/* Per-tag PRP-list / DSM-range scratch buffers.
 *
 * DIMENSIONED BY [hctx][member][tag] — the hctx index is essential, NOT
 * optional.  blk-mq tags (req->tag) are unique only WITHIN a hardware
 * queue: on an nr_hw_queues>1 volume the same tag value is live
 * simultaneously on every hctx (total in-flight = nr_hw_queues ×
 * RC_VOLUME_QUEUE_DEPTH, per the comment above).  Two concurrent requests
 * on different hctx that draw the same tag route to different NVMe queues
 * (io_queues[hctx->queue_num]) but would share one [member][tag] buffer if
 * the hctx dimension were dropped — each stomping the other's PRP list.
 * The victim command then DMAs to the other request's pages (silent
 * corruption) or to stale/unmapped IOVAs (AMD-Vi IO_PAGE_FAULT → EIO).
 * That only bites transfers > 2 pages (≤2 pages use PRP1/PRP2 directly and
 * never touch this pool) under multi-queue concurrency — which is why
 * synchronous dd passes and deep-queue fio O_DIRECT fails. */
static __le64     *rc_volume_prp_va[RC_VOLUME_MAX_HCTX][RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static dma_addr_t  rc_volume_prp_pa[RC_VOLUME_MAX_HCTX][RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];

/* CID range for boot/reset-time synchronous commands (rc_nvme_io_cmd_sync):
 * 0x400 | (per-queue sequence & 0xFF), i.e. 0x400..0x4FF.  Deliberately
 * OUTSIDE the blk-mq tag CID space (tags run 0..RC_VOLUME_QUEUE_DEPTH-1,
 * i.e. 255) because sync commands share io_queues[0] with live volume traffic
 * during module reload / controller auto-reset.  (A literal CID 0 aliased
 * blk-mq tag 0: with sync_pending armed the ISR would swallow a real tag-0
 * request's CQE as the sync completion.)
 *
 * The low byte is a rolling sequence so a STALE sync CQE — left on the ring
 * by a previous sync command that timed out and disarmed without consuming
 * it — can never be misattributed to the CURRENT command: both consumers
 * match the full expected CID (q->sync_cid), and a sync-range CQE with the
 * wrong sequence is consumed and dropped. */
#define RC_VOLUME_SYNC_CID_BASE		0x400u
#define RC_VOLUME_SYNC_CID_MASK		0xFF00u	/* (cid & MASK) == BASE → sync range */
/* blk-mq per-request command data.
 *
 * For READ/WRITE the request targets ONE member, members_pending is set
 * to 1, and the ISR completes immediately on its single CQE.
 *
 * For FLUSH the driver fans out one NVMe FLUSH command to every member;
 * members_pending starts at rc_volume_member_count and each member's ISR
 * does atomic_dec_and_test — only the last one to land calls
 * blk_mq_complete_request.
 *
 * sc_sct is the cumulative status: 0 if all members succeeded; non-zero
 * if any member returned an error.  (For multi-member FLUSH the writes
 * are racy under concurrency but error-vs-not is preserved correctly.)
 *
 * sg + nents hold the dma_map_sg result for READ/WRITE so .complete (or
 * the timeout safety-net) can dma_unmap_sg.  nents=0 means no mapping is
 * outstanding (FLUSH, DISCARD, early-error path). */
struct rc_volume_pdu {
	int                member_idx;
	u8                 op;             /* req_op() value */
	u16                sc_sct;         /* NVMe SC/SCT, 0 = success */
	u8                 hctx_idx;       /* hctx the request was queued on;
					    * .timeout uses it to derive the
					    * NVMe SQID for Abort */
	/* Bit i set = this request was submitted to member i.  Set by every
	 * dispatch path (single-member: BIT(idx); fan-outs: snapshot of the
	 * members actually submitted to).  Timeout/drain involvement checks
	 * and the completion accounting below key off this — NOT off the
	 * global member list, which can change (degrade) while the request
	 * is in flight. */
	unsigned int       member_mask;
	/* Per-member completion accounting for degraded RAID1.
	 *
	 * acked: bit i set = member i's completion has been CONSUMED for
	 * this request — either its CQE arrived (ISR) or the drain path
	 * synthesized a dead-member completion.  Both paths claim the bit
	 * with atomic_fetch_or and only decrement members_pending when the
	 * bit was previously clear, making the decrement exactly-once per
	 * (request, member) no matter how ISR and drain interleave.
	 *
	 * err_members: bit i set = member i completed this request with an
	 * error (real NVMe status or the dead sentinel).  Lets the finish
	 * path distinguish "one mirror failed, the other has the data"
	 * (degraded success) from "everyone failed" (I/O error). */
	atomic_t           acked;
	atomic_t           err_members;
	atomic_t           members_pending;
	/* Resync write-generation accounting: set when this request was
	 * counted in rc_volume_wgen_count[wgen] (RAID1 write/discard fan-out
	 * only).  Decremented exactly once at completion via
	 * rc_volume_unmap_request_sg. */
	bool               wgen_counted;
	u8                 wgen;
	/* Bounded .timeout re-arm count: how many times the EH handler has
	 * returned BLK_EH_RESET_TIMER for this request while waiting on a
	 * member that looks alive.  Once exhausted, the still-silent members
	 * are declared dead — a controller can drop a command without ever
	 * asserting CSTS.CFS, and waiting forever would hang the request
	 * (and its tag, and its submitter) with no recovery path. */
	u8                 eh_retries;
	/* Single-completion claim.  Multiple actors can race to finish one
	 * request — the ISR (batched direct-end fast path), the dead-member
	 * drain, the .timeout handler, and queue_rq's own inline error/hit
	 * paths — and blk-mq does NOT arbitrate between them (the ISR fast
	 * path deliberately bypasses blk_mq_complete_request's state machine).
	 * Every completion-initiating site must win
	 * rc_volume_claim_completion() first; losers back off, the winner's
	 * path performs the cleanup + end.  Observed unclaimed: mass-timeout
	 * drain racing the ISR direct-end → request ref double-put WARN at
	 * block/blk.h (CI qemu-rig). */
	atomic_t           completed;
	int                nents;          /* dma_map_sg output count, 0 = not mapped */
	/* Bounce fallback for layouts NVMe PRPs cannot encode.  bounce is
	 * the single-member whole-request buffer; ms_bounce is the
	 * multi-stripe buffer sliced in member-LBA order.  Both are owned by
	 * this request and freed by rc_volume_unmap_request_sg. */
	void              *bounce;
	void              *ms_bounce;
	u32                ms_bounce_off[RC_VOLUME_MAX_MEMBERS];
	u32                ms_bounce_len[RC_VOLUME_MAX_MEMBERS];
	u8                 ms_bounce_slots;
	/* Set immediately before blk_mq_start_request: a bounce buffer may
	 * only be copied back to the bio after a real completion, never on
	 * a pre-submit error path (it would leak uninitialised memory into
	 * a failed READ's pages). */
	bool               io_started;
	/* Multi-stripe (multi-member) dispatch.  When ms_active is true, the
	 * request was split across multiple members in queue_rq; ms_nents[m]
	 * and ms_sg[m] hold each member's portion.  members_pending is set to
	 * the number of members with non-zero ms_nents.  The pdu->sg/nents
	 * fields above are unused in this path. */
	bool               ms_active;
	int                ms_nents[RC_VOLUME_MAX_MEMBERS];
	/* RAID 5 full-row write: index into rc_volume_parity_va[] owned by
	 * this request, or -1.  Released by rc_volume_unmap_request_sg. */
	int                parity_buf;
	/* Set when this request owns rc_volume_parity_lock (every RAID 5
	 * write is serialized so two writes cannot update one row's parity
	 * from the same old bytes).  Released by the same unmap path. */
	bool               parity_locked;
	struct scatterlist sg[RC_VOLUME_DATA_PAGES];
	struct scatterlist ms_sg[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_MS_PAGES_PER_MEMBER];
};

/* Claim the exclusive right to complete this request.  Returns true for
 * exactly one caller per request lifetime; every site that initiates
 * completion (ISR, drain, timeout, queue_rq inline paths) must call this
 * first and back off on false — the winner's path owns cleanup + end. */
static inline bool rc_volume_claim_completion(struct rc_volume_pdu *pdu)
{
	return atomic_cmpxchg(&pdu->completed, 0, 1) == 0;
}

/* Decide the blk status for a request whose last member completion just
 * landed.  Shared by the ISR's inline-end fast path and the softirq
 * .complete handler so the degraded-RAID1 policy lives in one place:
 *
 *   - RAID1 fan-out (WRITE/DISCARD/FLUSH) where only a SUBSET of the
 *     members errored: the data is durable on the member(s) that
 *     succeeded — mark the erroring members failed (degrading the
 *     volume) and complete the request OK.  Failing it would punish the
 *     upper layers for a redundancy the array just spent a mirror to
 *     provide.
 *   - RAID1 READ error: the picked member failed the read; mark it
 *     failed so subsequent reads route to the survivor.  This request
 *     still errors (retry-on-survivor is a follow-up).
 *   - Everything else keeps today's semantics: any error fails the
 *     request. */
static blk_status_t rc_volume_finish_status(struct request *req)
{
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	unsigned long errs = (unsigned long)(unsigned int)atomic_read(&pdu->err_members);
	unsigned int mask = pdu->member_mask;
	int i;

	if (!READ_ONCE(pdu->sc_sct))
		return BLK_STS_OK;

	if ((rc_volume_raid_level != RC_LDT_RAID1 &&
	     rc_volume_raid_level != RC_LDT_RAID10) || !mask || !errs)
		return BLK_STS_IOERR;

	if (pdu->op == REQ_OP_READ) {
		for_each_set_bit(i, &errs, RC_VOLUME_MAX_MEMBERS)
			rc_volume_member_mark_failed(i);
		return BLK_STS_IOERR;
	}

	/* A non-READ request is lost only when some redundancy group lost
	 * every copy it submitted to.  RAID1 has one group (all members);
	 * RAID10 has one group per mirror pair, so a request spanning several
	 * pairs survives as long as each pair kept a copy.  This also covers
	 * FLUSH, whose mask spans every live member of every pair. */
	if (rc_volume_raid_level == RC_LDT_RAID10) {
		u32 c, cols = rc_volume_raid10_cols;

		for (c = 0; c < cols; c++) {
			unsigned int subm = mask & (BIT(2 * c) | BIT(2 * c + 1));

			if (subm && (errs & subm) == subm) {
				for_each_set_bit(i, &errs, RC_VOLUME_MAX_MEMBERS)
					rc_volume_member_mark_failed(i);
				return BLK_STS_IOERR;
			}
		}
	} else if ((errs & mask) == mask) {
		/* Every submitted member errored — the request fails, and the
		 * erroring members must ALSO be marked failed (same as the
		 * READ branch above): a lone survivor that starts failing
		 * every write would otherwise stay "live" forever, with the
		 * volume re-dispatching to it instead of being declared
		 * down. */
		for_each_set_bit(i, &errs, RC_VOLUME_MAX_MEMBERS)
			rc_volume_member_mark_failed(i);
		return BLK_STS_IOERR;
	}

	for_each_set_bit(i, &errs, RC_VOLUME_MAX_MEMBERS)
		rc_volume_member_mark_failed(i);
	printk_ratelimited(KERN_WARNING
		"rcraid: %s: op=%u lba=%llu completed degraded (err_members=0x%lx of 0x%x) — data durable on surviving copy/mirror\n",
		rc_volume_disk ? rc_volume_disk->disk_name : "?",
		pdu->op, (unsigned long long)blk_rq_pos(req), errs, mask);
	return BLK_STS_OK;
}

/* Sentinel value stored into pdu->sc_sct by the drain path so .complete
 * can log "controller dead" instead of decoding it as a real NVMe status.
 * SC=0xff (vendor-specific range), SCT=0x7 (vendor specific), DNR=1, More=1
 * — distinguishable from any value a spec-compliant controller will post. */
#define RC_VOLUME_SC_DEAD	0x7fff

#define RC_NVME_CC_DEFAULT	\
	(RC_NVME_CC_IOSQES_64 | RC_NVME_CC_IOCQES_16 | \
	 RC_NVME_CC_AMS_RR | RC_NVME_CC_MPS_4K | RC_NVME_CC_CSS_NVM)

static inline u64 rc_nvme_readq(void __iomem *base, u32 off)
{
	u32 lo = readl(base + off);
	u32 hi = readl(base + off + 4);
	return ((u64)hi << 32) | lo;
}

static inline void rc_nvme_writeq(u64 v, void __iomem *base, u32 off)
{
	writel((u32)v, base + off);
	writel((u32)(v >> 32), base + off + 4);
}

/* Doorbells live at BAR0 + 0x1000, striped by (4 << DSTRD). For each queue
 * pair qid, the SQ tail doorbell is at offset 2*qid and the CQ head doorbell
 * at 2*qid+1, both scaled by the stride. */
static inline void rc_nvme_ring_sq_doorbell(struct rc_adapter *adapter,
					    u16 qid, u32 value)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u32 stride = 4u << nvme->dstrd;
	writel(value, adapter->ctx.mmio_base + RC_NVME_REG_DBL_BASE +
		      (u32)(2 * qid) * stride);
}

static inline void rc_nvme_ring_cq_doorbell(struct rc_adapter *adapter,
					    u16 qid, u32 value)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u32 stride = 4u << nvme->dstrd;
	writel(value, adapter->ctx.mmio_base + RC_NVME_REG_DBL_BASE +
		      (u32)(2 * qid + 1) * stride);
}

/* Shared CSTS canary used by both ISR types.  Returns true if the
 * controller has gone fatal (CFS) or the device has been hot-unplugged
 * (CSTS reads as ~0).  Marks the adapter dead exactly once and wakes
 * the admin waiter so any in-flight init/abort sleeper returns promptly
 * via its wait_event_timeout. */
static bool rc_nvme_irq_csts_check(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u32 csts;

	if (!adapter->ctx.mmio_base)
		return false;
	csts = readl(adapter->ctx.mmio_base + RC_NVME_REG_CSTS);
	if (csts != 0xffffffff && !(csts & RC_NVME_CSTS_CFS))
		return false;

	if (!READ_ONCE(nvme->dead)) {
		rc_printk(RC_ERROR,
			  "rc_nvme: %s controller dead (CSTS=0x%08x) — failing in-flight I/O\n",
			  pci_name(adapter->pdev), csts);
		WRITE_ONCE(nvme->dead, true);
	}
	wake_up(&nvme->admin_cq_wait);
	return true;
}

/* Admin ISR — registered with request_irq for MSI-X vector 0.
 * Admin queue completions wake the synchronous admin_cmd path; we don't
 * walk the admin CQ here (the submitter consumes its own CQE). */
irqreturn_t rc_nvme_admin_irq(int irq, void *dev_id)
{
	struct rc_adapter *adapter = dev_id;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;

	if (rc_nvme_irq_csts_check(adapter))
		return IRQ_HANDLED;
	wake_up(&nvme->admin_cq_wait);
	return IRQ_HANDLED;
}

/* Forward declaration used inside the per-queue ISR. */
static void rc_volume_unmap_request_sg(struct rc_volume_pdu *pdu);

/* Per-I/O-queue ISR — registered for MSI-X vector qid.  Walks this
 * queue's CQ only.  Boot-time sync helpers sleep on q->cq_wait; once
 * rc_volume_disk is up the ISR completes blk-mq requests via
 * blk_mq_complete_request and the softirq .complete callback finishes
 * them (dma_unmap + blk_mq_end_request). */
irqreturn_t rc_nvme_io_queue_irq(int irq, void *dev_id)
{
	struct rc_nvme_io_queue *q = dev_id;
	struct rc_adapter *adapter = q->adapter;
	struct blk_mq_tags *tags;
	bool advanced = false;
	/* Defer-list: collect requests ready to finish inside the q->lock
	 * loop, then end them after releasing the lock so blk_mq_end_request
	 * can take its own internal locks safely.  Capped to a small batch;
	 * if more land in one IRQ window, the leftover stays for the next. */
	struct request *done_reqs[16];
	unsigned int done_n = 0;

	if (rc_nvme_irq_csts_check(adapter)) {
		wake_up(&q->cq_wait);
		return IRQ_HANDLED;
	}
	wake_up(&q->cq_wait);

	if (!rc_volume_disk)
		return IRQ_HANDLED;

	tags = rc_volume_tagset.tags[q->hctx_idx >= 0 ? q->hctx_idx : 0];

	spin_lock(&q->lock);
	for (;;) {
		struct rc_nvme_cqe *cqe =
			(struct rc_nvme_cqe *)q->cq + q->cq_head;
		u16 status = le16_to_cpu(READ_ONCE(cqe->status));
		u16 cid;
		struct request *req;
		struct rc_volume_pdu *pdu;

		if ((status & 1) != q->cq_phase)
			break;

		cid = le16_to_cpu(cqe->cid);
		if ((cid & RC_VOLUME_SYNC_CID_MASK) == RC_VOLUME_SYNC_CID_BASE) {
			/* Boot/reset-time sync command (rc_nvme_io_cmd_sync):
			 * record the status and let the sync waiter return.
			 * This ISR is live and consuming this CQ whenever
			 * rc_volume_disk exists (module reload, controller
			 * auto-reset), so without this claim it would eat the
			 * sync CQE as "unknown CID" and the waiter would time
			 * out — dropping the member to the legacy fallback
			 * path mid-reload.  A sequence mismatch (or no waiter)
			 * means a STALE CQE from a timed-out earlier sync
			 * command: consume and drop it — attributing it to the
			 * current command would hand the metadata parser a
			 * status for a read that never completed. */
			if (q->sync_pending && cid == q->sync_cid) {
				q->sync_sc = (status >> 1) & 0x7fff;
				q->sync_pending = false;
			} else {
				rc_printk(RC_WARN,
					  "rc_nvme_io_queue_irq: %s qid=%u dropping stale sync CQE CID=0x%x (expected 0x%x pending=%d)\n",
					  pci_name(adapter->pdev), q->qid, cid,
					  q->sync_cid, q->sync_pending);
			}
		} else {
			int slot = READ_ONCE(adapter->ctx.nvme.volume_slot);

			/* A blk-mq-CID CQE on an adapter with NO registry slot
			 * cannot belong to a live request: either this adapter
			 * was never a member (its queues never carry volume
			 * tags), or it was HOT-REMOVED — and the removal path
			 * drains (synthesizing this member's completion) before
			 * clearing volume_slot, so this is a late CQE whose
			 * request side is already settled.  Touching the pdu
			 * here would bypass the per-member acked gate and
			 * double-decrement a possibly TAG-REUSED request.
			 * Consume and drop. */
			if (slot < 0) {
				printk_ratelimited(KERN_WARNING
					"rcraid: %s qid=%u dropping CQE CID=%u — adapter is not a volume member (late completion after removal?)\n",
					pci_name(adapter->pdev), q->qid, cid);
				goto cqe_consumed;
			}

			req = blk_mq_tag_to_rq(tags, cid);
			if (req) {
				u16 sc = (status >> 1) & 0x7fff;
				bool consume;

				pdu = blk_mq_rq_to_pdu(req);
				if (sc) {
					/* First-error-wins, single aligned
					 * store: concurrent member ISRs on a
					 * fan-out op race here, but each
					 * store is atomic so the field holds
					 * ONE of the members' statuses (never
					 * a torn mix), and error-vs-success
					 * is always preserved.  err_members
					 * additionally records WHICH members
					 * errored, so the finish path can
					 * treat a partial mirror failure as
					 * degraded success. */
					if (!READ_ONCE(pdu->sc_sct))
						WRITE_ONCE(pdu->sc_sct, sc);
					atomic_or(BIT(slot),
						  &pdu->err_members);
					rc_printk(RC_ERROR,
						  "rc_nvme_io_queue_irq: %s qid=%u CID=%u op=%u pos=%llu len=%u failed SC/SCT=0x%04x\n",
						  pci_name(adapter->pdev), q->qid,
						  cid, pdu->op,
						  (u64)blk_rq_pos(req),
						  blk_rq_sectors(req), sc);
				}
				/* Exactly-once completion consumption per
				 * (request, member): if the drain path
				 * already synthesized this member's
				 * completion (bit set), this is a late CQE —
				 * do NOT decrement again. */
				{
					int prev = atomic_fetch_or(BIT(slot),
								   &pdu->acked);
					consume = !(prev & BIT(slot));
				}
				if (consume &&
				    atomic_dec_and_test(&pdu->members_pending) &&
				    rc_volume_claim_completion(pdu)) {
					/* Try to defer to local batch; fall back
					 * to the softirq path if the batch is full.
					 * (A lost claim means the drain/timeout
					 * path is already finishing this request
					 * — touching it again double-completes.) */
					if (done_n < ARRAY_SIZE(done_reqs))
						done_reqs[done_n++] = req;
					else
						blk_mq_complete_request(req);
				}
			} else {
				rc_printk(RC_WARN,
					  "rc_nvme_io_queue_irq: %s qid=%u CQE for unknown CID=%u (status=0x%04x)\n",
					  pci_name(adapter->pdev), q->qid, cid, status);
			}
		}

cqe_consumed:
		/* One CQE consumed = one SQE slot free (C-4 accounting). */
		if (q->sq_inflight)
			q->sq_inflight--;

		q->cq_head = (q->cq_head + 1) % q->cq_depth;
		if (q->cq_head == 0)
			q->cq_phase ^= 1;
		advanced = true;
	}
	if (advanced)
		rc_nvme_ring_cq_doorbell(adapter, q->qid, q->cq_head);
	spin_unlock(&q->lock);

	/* The entry-time wake fires before CQ processing; wake again now so
	 * a sync waiter whose CQE we just claimed doesn't sleep out its
	 * current poll tick. */
	if (advanced)
		wake_up(&q->cq_wait);

	/* Finish requests inline (no softirq round-trip).  Each call does
	 * dma_unmap_sg + blk_mq_end_request.  Saves the .complete softirq
	 * dispatch latency, which on Q1T1 dominates 5-15 us of clat. */
	{
		unsigned int i;
		for (i = 0; i < done_n; i++) {
			struct request *req = done_reqs[i];
			struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
			rc_volume_unmap_request_sg(pdu);
			blk_mq_end_request(req, rc_volume_finish_status(req));
		}
	}
	return IRQ_HANDLED;
}

/* Vector-0 handler body: rc_hw_interrupt_handler calls this when
 * ctrl_mode == NVMe.  Always services the admin CQ; additionally drains
 * any I/O queue whose CQ is bound to the admin vector (IV=0) because the
 * platform granted no dedicated I/O vectors (MSI/INTx single-vector
 * fallback).  Queues with their own MSI-X vector get
 * rc_nvme_io_queue_irq registered directly and are skipped here. */
irqreturn_t rc_nvme_irq(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	irqreturn_t ret;
	u16 i;

	ret = rc_nvme_admin_irq(0, adapter);

	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];

		if (q && q->shares_admin_vector &&
		    rc_nvme_io_queue_irq(0, q) == IRQ_HANDLED)
			ret = IRQ_HANDLED;
	}
	return ret;
}

/* Cheap dead-controller probe usable from process context (timeout callback).
 * Returns true if the adapter has been flagged dead, or transitions it to
 * dead now because CSTS reports a fatal status or the device has vanished.
 * Caller must hold a reference to the adapter (always true for a member in
 * rc_volume_members[]). */
static bool rc_nvme_check_dead(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	void __iomem *base = adapter->ctx.mmio_base;
	u32 csts;

	if (READ_ONCE(nvme->dead))
		return true;
	if (!base)
		return false;

	csts = readl(base + RC_NVME_REG_CSTS);
	if (csts == 0xffffffff || (csts & RC_NVME_CSTS_CFS)) {
		rc_printk(RC_ERROR,
			  "rc_nvme_check_dead: %s controller dead (CSTS=0x%08x)\n",
			  pci_name(adapter->pdev), csts);
		WRITE_ONCE(nvme->dead, true);
		return true;
	}
	return false;
}

static int rc_nvme_wait_csts(struct rc_adapter *adapter, u32 mask, u32 want)
{
	void __iomem *base = adapter->ctx.mmio_base;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	/* CAP.TO is in 500 ms units; default to 30 s if firmware reports 0. */
	unsigned long ms_left = nvme->timeout_500ms ?
		(unsigned long)nvme->timeout_500ms * 500UL : 30000UL;
	u32 csts;

	do {
		csts = readl(base + RC_NVME_REG_CSTS);
		if (csts == 0xffffffff) {
			rc_printk(RC_ERROR,
				  "rc_nvme_wait_csts: device gone (CSTS=0x%08x)\n",
				  csts);
			return -EIO;
		}
		if (csts & RC_NVME_CSTS_CFS) {
			rc_printk(RC_ERROR,
				  "rc_nvme_wait_csts: controller fatal status (CSTS=0x%08x)\n",
				  csts);
			return -EIO;
		}
		if ((csts & mask) == want)
			return 0;
		msleep(10);
		if (ms_left <= 10) {
			rc_printk(RC_ERROR,
				  "rc_nvme_wait_csts: timeout waiting for CSTS & 0x%x == 0x%x (CSTS=0x%08x)\n",
				  mask, want, csts);
			return -ETIMEDOUT;
		}
		ms_left -= 10;
	} while (1);
}

static int rc_nvme_disable_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	u32 cc;

	cc = readl(base + RC_NVME_REG_CC);
	if (cc & RC_NVME_CC_EN) {
		writel(cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);
		rc_printk(RC_INFO,
			  "rc_nvme_disable_controller: CC.EN cleared (was 0x%08x)\n",
			  cc);
	}
	return rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, 0);
}

/* NVMe-spec-conformant shutdown notification (CC.SHN = 01b "normal",
 * wait for CSTS.SHST = 10b "complete").  The driver enables the members'
 * volatile write caches, so ripping CC.EN low without this counts as an
 * UNSAFE shutdown: the drive may drop cached writes and its
 * unsafe-shutdown SMART counter climbs on every suspend/reboot.  Must be
 * called BEFORE clearing CC.EN.  Best-effort: on timeout we log and let
 * the caller proceed with the disable anyway. */
static void rc_nvme_shutdown_notify(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	u32 cc;

	if (!base)
		return;
	cc = readl(base + RC_NVME_REG_CC);
	if (cc == 0xffffffff || !(cc & RC_NVME_CC_EN))
		return;	/* gone or disabled — nothing to notify */

	cc = (cc & ~RC_NVME_CC_SHN_MASK) | RC_NVME_CC_SHN_NORMAL;
	writel(cc, base + RC_NVME_REG_CC);

	if (rc_nvme_wait_csts(adapter, RC_NVME_CSTS_SHST_MASK,
			      RC_NVME_CSTS_SHST_COMPLETE))
		rc_printk(RC_WARN,
			  "rc_nvme_shutdown_notify: %s shutdown processing did not complete — continuing with disable\n",
			  pci_name(adapter->pdev));
}

static int rc_nvme_alloc_admin_queues(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	size_t sq_bytes = (size_t)nvme->admin_sq_depth * RC_NVME_SQ_ENTRY_SIZE;
	size_t cq_bytes = (size_t)nvme->admin_cq_depth * RC_NVME_CQ_ENTRY_SIZE;

	/* dma_alloc_coherent returns page-aligned memory, satisfying NVMe's
	 * 4 KiB alignment requirement for queue bases. */
	nvme->admin_sq = dma_alloc_coherent(dev, sq_bytes, &nvme->admin_sq_dma,
					    GFP_KERNEL);
	if (!nvme->admin_sq)
		return -ENOMEM;

	nvme->admin_cq = dma_alloc_coherent(dev, cq_bytes, &nvme->admin_cq_dma,
					    GFP_KERNEL);
	if (!nvme->admin_cq) {
		dma_free_coherent(dev, sq_bytes, nvme->admin_sq,
				  nvme->admin_sq_dma);
		nvme->admin_sq = NULL;
		return -ENOMEM;
	}

	memset(nvme->admin_sq, 0, sq_bytes);
	memset(nvme->admin_cq, 0, cq_bytes);
	nvme->admin_sq_tail = 0;
	nvme->admin_cq_head = 0;
	nvme->admin_cq_phase = 1;
	return 0;
}

static void rc_nvme_free_admin_queues(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;

	if (nvme->admin_sq) {
		dma_free_coherent(dev,
				  (size_t)nvme->admin_sq_depth * RC_NVME_SQ_ENTRY_SIZE,
				  nvme->admin_sq, nvme->admin_sq_dma);
		nvme->admin_sq = NULL;
	}
	if (nvme->admin_cq) {
		dma_free_coherent(dev,
				  (size_t)nvme->admin_cq_depth * RC_NVME_CQ_ENTRY_SIZE,
				  nvme->admin_cq, nvme->admin_cq_dma);
		nvme->admin_cq = NULL;
	}
}

/* Copy an NVMe ASCII string field (space-padded, not NUL-terminated) into a
 * caller buffer of size len+1 and strip trailing spaces. */
static void rc_nvme_ascii_field(const u8 *src, size_t len, char *dst)
{
	memcpy(dst, src, len);
	while (len > 0 && dst[len - 1] == ' ')
		len--;
	dst[len] = '\0';
}

/* Wait for the next admin CQE slot's phase bit to flip to the expected
 * value, driven by MSI wakes on admin_cq_wait.  Falls back to a 10 ms
 * polling tick so we don't hang permanently if interrupts are mis-delivered
 * (paranoia — once MSI has been validated in the field this can drop).
 * No CQ-head advance is done here; the caller consumes the entry. */
static int rc_nvme_wait_admin_completion(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_cqe *cqe =
		(struct rc_nvme_cqe *)nvme->admin_cq + nvme->admin_cq_head;
	u8 expected_phase = nvme->admin_cq_phase;
	unsigned long total = msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);
	unsigned long deadline = jiffies + total;

	while (time_before(jiffies, deadline)) {
		long left = deadline - jiffies;
		/* Safety-net poll tick — kept tiny so a mis-armed MSI doesn't
		 * tank throughput.  Real wakeups land via rc_nvme_irq. */
		long tick = min_t(long, left, max_t(long, 1, msecs_to_jiffies(1)));

		if (wait_event_timeout(nvme->admin_cq_wait,
				       (le16_to_cpu(READ_ONCE(cqe->status)) & 1) ==
					       expected_phase,
				       tick) > 0)
			return 0;
	}
	return -ETIMEDOUT;
}

/* Inner admin-command path; caller must hold nvme->admin_mutex.  Used
 * directly by rc_nvme_reset_controller, which already holds the mutex
 * for the entire reset sequence and would deadlock if the public wrapper
 * tried to re-take it.  Same behaviour as rc_nvme_admin_cmd otherwise.
 *
 * If out_result is non-NULL, the CQE's DW0 (command-specific result) is
 * stored there before the CQ slot is released.  Used by Set Features et
 * al. to read back their response data; pass NULL when the result isn't
 * meaningful. */
static int __rc_nvme_admin_cmd_locked(struct rc_adapter *adapter,
				      struct rc_nvme_sqe *cmd,
				      u32 *out_result)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe *slot;
	struct rc_nvme_cqe *cqe;
	u32 tail;
	u16 status, sc_sct, expected_cid, ccid;
	int ret;

	/* Rolling CID — the admin-path mirror of the I/O sync path's
	 * sync_seq stale-CQE defense.  With a constant CID 0, a late CQE
	 * from a previously TIMED-OUT admin command was consumed by the
	 * next admin command as its own completion (same CID, matching
	 * phase): wrong status attributed, admin_cq_head desynced.  Now
	 * each command carries a fresh CID and mismatching CQEs are
	 * consumed and dropped. */
	nvme->admin_cid_seq++;
	expected_cid = nvme->admin_cid_seq;

	slot = (struct rc_nvme_sqe *)nvme->admin_sq + nvme->admin_sq_tail;
	cmd->cid = cpu_to_le16(expected_cid);
	memcpy(slot, cmd, sizeof(*cmd));

	tail = (nvme->admin_sq_tail + 1) % nvme->admin_sq_depth;
	nvme->admin_sq_tail = tail;
	wmb();
	rc_nvme_ring_sq_doorbell(adapter, 0, tail);

	for (;;) {
		ret = rc_nvme_wait_admin_completion(adapter);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_nvme_admin_cmd: timeout waiting for CQE (opc=0x%02x CID=0x%x)\n",
				  cmd->opc, expected_cid);
			return ret;
		}

		cqe = (struct rc_nvme_cqe *)nvme->admin_cq + nvme->admin_cq_head;
		status = le16_to_cpu(cqe->status);
		sc_sct = (status >> 1) & 0x7fff;
		ccid   = le16_to_cpu(cqe->cid);
		if (out_result)
			*out_result = le32_to_cpu(cqe->result);

		/* Consume the CQE unconditionally — a stale entry left on
		 * the ring must not be re-examined forever. */
		nvme->admin_cq_head = (nvme->admin_cq_head + 1) % nvme->admin_cq_depth;
		if (nvme->admin_cq_head == 0)
			nvme->admin_cq_phase ^= 1;
		rc_nvme_ring_cq_doorbell(adapter, 0, nvme->admin_cq_head);

		if (ccid == expected_cid)
			break;

		rc_printk(RC_WARN,
			  "rc_nvme_admin_cmd: dropping stale admin CQE CID=0x%x (expected 0x%x, status=0x%04x)\n",
			  ccid, expected_cid, status);
	}

	if (sc_sct) {
		rc_printk(RC_ERROR,
			  "rc_nvme_admin_cmd: opc=0x%02x SC/SCT=0x%04x (status=0x%04x)\n",
			  cmd->opc, sc_sct, status);
		return -EIO;
	}
	return 0;
}

/* Copy a caller-built SQE into the admin SQ tail slot, ring the doorbell,
 * poll for completion, advance the CQ head, and return 0 on success or
 * -errno (timeout, controller error). The caller fills opc/nsid/prp1/cdwN;
 * we manage CID = 0 since only one admin command is in flight at a time —
 * the admin_mutex serialises against teardown and against timeout-issued
 * Aborts that run from a different process context.
 *
 * out_result: see __rc_nvme_admin_cmd_locked.
 */
static int rc_nvme_admin_cmd(struct rc_adapter *adapter,
			     struct rc_nvme_sqe *cmd, u32 *out_result)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	int ret;

	mutex_lock(&nvme->admin_mutex);
	ret = __rc_nvme_admin_cmd_locked(adapter, cmd, out_result);
	mutex_unlock(&nvme->admin_mutex);
	return ret;
}

/* Issue an NVMe Abort admin command for (sqid, cid) on this adapter.
 * Best-effort — the spec allows the controller to return "Abort Command
 * Limit Exceeded" or to ignore the request entirely.  The caller does not
 * depend on the original command actually being aborted; it just wants to
 * give the controller a chance to free the CID before the request is ended
 * with BLK_STS_TIMEOUT.  Returns 0 on admin-command success (regardless of
 * whether the abort actually happened), negative errno on admin failure. */
static int rc_nvme_abort(struct rc_adapter *adapter, u16 sqid, u16 cid)
{
	struct rc_nvme_sqe cmd = {};

	cmd.opc   = RC_NVME_ADMIN_OP_ABORT;
	cmd.cdw10 = cpu_to_le32(((u32)cid << 16) | sqid);
	return rc_nvme_admin_cmd(adapter, &cmd, NULL);
}

/* Submit a single Identify Controller (CNS=01h) admin command, poll for
 * completion, and log the controller's VID/SSVID/SN/MN/FR/NN. Non-fatal:
 * callers should treat failure as "we got CSTS.RDY but can't talk to it."
 */
static int rc_nvme_identify_controller(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_sqe cmd = {};
	void *id_buf;
	dma_addr_t id_dma;
	int ret;

	id_buf = dma_alloc_coherent(dev, RC_NVME_IDENTIFY_BYTES, &id_dma,
				    GFP_KERNEL);
	if (!id_buf)
		return -ENOMEM;

	cmd.opc   = RC_NVME_ADMIN_OP_IDENTIFY;
	cmd.prp1  = cpu_to_le64(id_dma);
	cmd.cdw10 = cpu_to_le32(RC_NVME_IDENTIFY_CNS_CTRL);

	ret = rc_nvme_admin_cmd(adapter, &cmd, NULL);
	if (ret)
		goto out;

	{
		struct rc_nvme_state *nvme = &adapter->ctx.nvme;
		const u8 *b = id_buf;
		u16 vid   = le16_to_cpup((const __le16 *)(b + RC_NVME_ID_CTRL_VID));
		u16 ssvid = le16_to_cpup((const __le16 *)(b + RC_NVME_ID_CTRL_SSVID));
		u32 nn    = le32_to_cpup((const __le32 *)(b + RC_NVME_ID_CTRL_NN));
		u8  mdts  = b[RC_NVME_ID_CTRL_MDTS];
		u8  mpsmin = (u8)((nvme->cap >> 48) & 0xf);
		u8  vwc   = b[RC_NVME_ID_CTRL_VWC];
		char sn[21], mn[41], fr[9];

		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_SN, 20, sn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_MN, 40, mn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_FR, 8,  fr);

		nvme->mdts = mdts;
		/* Keep the identity for the /dev/rc_api config packet. */
		nvme->id_vid = vid;
		nvme->id_ssvid = ssvid;
		memcpy(nvme->id_model, b + RC_NVME_ID_CTRL_MN,
		       sizeof(nvme->id_model));
		memcpy(nvme->id_serial, b + RC_NVME_ID_CTRL_SN,
		       sizeof(nvme->id_serial));
		memcpy(nvme->id_firmware, b + RC_NVME_ID_CTRL_FR,
		       sizeof(nvme->id_firmware));
		nvme->max_transfer_bytes = mdts ?
			(1u << mdts) * (1u << (12 + mpsmin)) : 0;
		nvme->vwc_present = (vwc & 1) ? 1 : 0;

		rc_printk(RC_NOTE,
			  "rc_nvme_identify_controller: VID=0x%04x SSVID=0x%04x SN='%s' MN='%s' FR='%s' NN=%u MDTS=%u MPSMIN=%u max_xfer=%u B VWC=%u\n",
			  vid, ssvid, sn, mn, fr, nn,
			  mdts, mpsmin, nvme->max_transfer_bytes, nvme->vwc_present);
	}

out:
	dma_free_coherent(dev, RC_NVME_IDENTIFY_BYTES, id_buf, id_dma);
	return ret;
}

/* Submit Identify Namespace (CNS=00h) for the given NSID, parse NSZE/NCAP/
 * NUSE and the active LBA format, and log the result. Also stashes the LBA
 * size into adapter->ctx.nvme so the I/O path can size its buffers. */
static int rc_nvme_identify_namespace(struct rc_adapter *adapter, u32 nsid)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd = {};
	void *id_buf;
	dma_addr_t id_dma;
	int ret;

	id_buf = dma_alloc_coherent(dev, RC_NVME_IDENTIFY_BYTES, &id_dma,
				    GFP_KERNEL);
	if (!id_buf)
		return -ENOMEM;

	cmd.opc   = RC_NVME_ADMIN_OP_IDENTIFY;
	cmd.nsid  = cpu_to_le32(nsid);
	cmd.prp1  = cpu_to_le64(id_dma);
	cmd.cdw10 = cpu_to_le32(RC_NVME_IDENTIFY_CNS_NS);

	ret = rc_nvme_admin_cmd(adapter, &cmd, NULL);
	if (ret)
		goto out;

	{
		const u8 *b = id_buf;
		u64 nsze = le64_to_cpup((const __le64 *)(b + RC_NVME_ID_NS_NSZE));
		u64 ncap = le64_to_cpup((const __le64 *)(b + RC_NVME_ID_NS_NCAP));
		u64 nuse = le64_to_cpup((const __le64 *)(b + RC_NVME_ID_NS_NUSE));
		u8  flbas = b[RC_NVME_ID_NS_FLBAS] & 0xf;
		u32 lbaf  = le32_to_cpup((const __le32 *)
					 (b + RC_NVME_ID_NS_LBAF + 4 * flbas));
		u8  lbads = (lbaf >> 16) & 0xff;
		u32 lba_bytes = 1u << lbads;

		nvme->ns1_lba_bytes = lba_bytes;
		nvme->ns1_nsze = nsze;

		rc_printk(RC_NOTE,
			  "rc_nvme_identify_namespace: nsid=%u NSZE=%llu NCAP=%llu NUSE=%llu LBA=%u B (LBADS=%u, FLBAS=%u) => %llu MiB\n",
			  nsid,
			  (unsigned long long)nsze,
			  (unsigned long long)ncap,
			  (unsigned long long)nuse,
			  lba_bytes, lbads, flbas,
			  (unsigned long long)((nsze * lba_bytes) >> 20));
	}

out:
	dma_free_coherent(dev, RC_NVME_IDENTIFY_BYTES, id_buf, id_dma);
	return ret;
}

/* Inner Set Features Number of Queues; caller must hold admin_mutex.
 * Used by the reset path which already holds the mutex for the whole
 * bring-up sequence and would deadlock if the public wrapper tried to
 * re-take it. */
static int __rc_nvme_set_num_queues_locked(struct rc_adapter *adapter,
					   u16 requested)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd = {};
	u32 result = 0;
	u16 nsqa_plus1, ncqa_plus1, granted;
	int ret;

	if (requested == 0)
		return -EINVAL;

	cmd.opc   = RC_NVME_ADMIN_OP_SET_FEATURES;
	cmd.cdw10 = cpu_to_le32(RC_NVME_FID_NUMBER_OF_QUEUES);
	cmd.cdw11 = cpu_to_le32(((u32)(requested - 1) << 16) |
				(u32)(requested - 1));

	ret = __rc_nvme_admin_cmd_locked(adapter, &cmd, &result);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_set_num_queues: %s Set Features failed (%d)\n",
			  pci_name(adapter->pdev), ret);
		return ret;
	}

	nsqa_plus1 = (u16)(result & 0xffff) + 1;
	ncqa_plus1 = (u16)((result >> 16) & 0xffff) + 1;
	granted = min3(nsqa_plus1, ncqa_plus1, requested);

	nvme->nr_io_queues = granted;

	rc_printk(RC_NOTE,
		  "rc_nvme_set_num_queues: %s requested=%u granted_sq=%u granted_cq=%u using=%u\n",
		  pci_name(adapter->pdev), requested,
		  nsqa_plus1, ncqa_plus1, granted);
	return 0;
}

/* Request `requested` I/O queue pairs from the controller via Set
 * Features Number of Queues (FID 0x07).  The controller may grant
 * fewer; the granted count is parsed from CQE.result DW0:
 *
 *   bits  15:0  = NSQA (Number of SQs Allocated, 0's based)
 *   bits 31:16  = NCQA (Number of CQs Allocated, 0's based)
 *
 * We take min(NSQA+1, NCQA+1, requested) as the usable count and store
 * it in nvme->nr_io_queues for the queue-creation path to read.
 *
 * Important: per NVMe spec, the controller default is 0 I/O queues
 * unless this admin command is issued.  Without it, Create I/O CQ
 * would fail with "Invalid Queue Identifier".  Single-queue setups
 * survived without it on the dev box because the controller appears
 * to silently allow qid=1 — that's lucky, not portable. */
static int rc_nvme_set_num_queues(struct rc_adapter *adapter, u16 requested)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	int ret;

	mutex_lock(&nvme->admin_mutex);
	ret = __rc_nvme_set_num_queues_locked(adapter, requested);
	mutex_unlock(&nvme->admin_mutex);
	return ret;
}

/* Explicitly enable the controller's volatile write cache via Set Features
 * Volatile Write Cache (FID 0x06, CDW11.WCE=1).  Caller must hold admin_mutex.
 *
 * Why do this rather than inherit whatever the drive powered up with: NVMe
 * leaves the power-on WCE state implementation-defined, and a Controller Level
 * Reset can revert it.  The volume advertises BLK_FEAT_WRITE_CACHE to the block
 * layer, so we want the underlying members to actually be in write-back mode —
 * otherwise sequential writes fall off a cliff (write-through commits every LBA
 * to NAND before completing).  Setting it here makes the state deterministic on
 * every bring-up and after every auto-reset.
 *
 * NON-FATAL by contract: a controller with no write cache (Identify VWC bit
 * clear) or one that rejects the feature just keeps whatever state it had.  The
 * caller must NOT treat a failure as fatal — this driver backs the root fs, and
 * refusing to bring the adapter up over a cache-mode nicety would make the box
 * unbootable. */
static int __rc_nvme_enable_write_cache_locked(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd = {};
	int ret;

	if (!nvme->vwc_present) {
		rc_printk(RC_NOTE,
			  "rc_nvme_enable_write_cache: %s no volatile write cache present (VWC=0) — leaving cache mode untouched\n",
			  pci_name(adapter->pdev));
		return 0;
	}

	cmd.opc   = RC_NVME_ADMIN_OP_SET_FEATURES;
	cmd.cdw10 = cpu_to_le32(RC_NVME_FID_VOLATILE_WRITE_CACHE);
	cmd.cdw11 = cpu_to_le32(1);	/* WCE = 1 (enable write-back) */

	ret = __rc_nvme_admin_cmd_locked(adapter, &cmd, NULL);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_nvme_enable_write_cache: %s Set Features WCE=1 failed (%d) — continuing with inherited cache state\n",
			  pci_name(adapter->pdev), ret);
		return ret;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_enable_write_cache: %s volatile write cache ENABLED (WCE=1)\n",
		  pci_name(adapter->pdev));
	return 0;
}

/* admin_mutex-taking wrapper for the init (non-reset) bring-up path. */
static int rc_nvme_enable_write_cache(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	int ret;

	mutex_lock(&nvme->admin_mutex);
	ret = __rc_nvme_enable_write_cache_locked(adapter);
	mutex_unlock(&nvme->admin_mutex);
	return ret;
}

/* Allocate one I/O queue pair: DMA-coherent SQ + CQ, wire it up via
 * Create I/O CQ + Create I/O SQ admin commands, register its MSI-X
 * vector with rc_nvme_io_queue_irq.  qid = 1-based.  vec_index is the
 * MSI-X vector slot (1-based — vector 0 is admin), EXCEPT on the
 * single-vector MSI/INTx fallback where vec_index 0 is passed: the CQ
 * is then bound to the admin vector and no IRQ of our own is
 * registered — rc_nvme_irq drains this queue from the vector-0 handler
 * (which rc_bottom already owns) instead. */
static int rc_nvme_create_one_io_queue(struct rc_adapter *adapter,
				       u16 qid, u16 vec_index)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_io_queue *q;
	struct rc_nvme_sqe cmd;
	size_t sq_bytes, cq_bytes;
	u16 depth;
	int ret;

	depth = RC_NVME_IO_QUEUE_DEPTH;
	if (depth > nvme->mqes)
		depth = nvme->mqes;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->adapter    = adapter;
	q->qid        = qid;
	q->irq_vector = -1;
	/* hctx i ↔ io_queues[i] across every member.  Setting it here so
	 * the per-queue ISR's blk_mq_tag_to_rq lookup uses the right
	 * tags[hctx_idx] array once nr_hw_queues > 1. */
	q->hctx_idx   = qid - 1;
	spin_lock_init(&q->lock);
	init_waitqueue_head(&q->cq_wait);

	sq_bytes = (size_t)depth * RC_NVME_SQ_ENTRY_SIZE;
	cq_bytes = (size_t)depth * RC_NVME_CQ_ENTRY_SIZE;

	q->sq = dma_alloc_coherent(dev, sq_bytes, &q->sq_dma, GFP_KERNEL);
	if (!q->sq) { ret = -ENOMEM; goto err_free_ctx; }
	q->cq = dma_alloc_coherent(dev, cq_bytes, &q->cq_dma, GFP_KERNEL);
	if (!q->cq) { ret = -ENOMEM; goto err_free_sq; }

	memset(q->sq, 0, sq_bytes);
	memset(q->cq, 0, cq_bytes);
	q->sq_depth  = depth;
	q->cq_depth  = depth;
	q->sq_tail   = 0;
	q->cq_head   = 0;
	q->cq_phase  = 1;

	/* Create I/O Completion Queue (NVMe 1.4 §5.3, Figure 110).
	 * CDW10[31:16]=QSIZE-1 (0's based), [15:0]=QID
	 * CDW11[31:16]=IV, [1]=IEN, [0]=PC.  Each I/O queue's CQ uses its
	 * own MSI-X vector so completions can be processed per-CPU. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_CQ;
	cmd.prp1  = cpu_to_le64(q->cq_dma);
	cmd.cdw10 = cpu_to_le32(((u32)(depth - 1) << 16) | qid);
	cmd.cdw11 = cpu_to_le32(((u32)vec_index << 16) | 0x3u);
	ret = rc_nvme_admin_cmd(adapter, &cmd, NULL);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_create_one_io_queue: %s qid=%u Create I/O CQ failed (%d)\n",
			  pci_name(adapter->pdev), qid, ret);
		goto err_free_cq;
	}

	/* Create I/O Submission Queue (NVMe 1.4 §5.4).
	 * CDW10[31:16]=QSIZE-1, [15:0]=QID
	 * CDW11[31:16]=CQID, [2:1]=QPRIO (0=Medium), [0]=PC. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_SQ;
	cmd.prp1  = cpu_to_le64(q->sq_dma);
	cmd.cdw10 = cpu_to_le32(((u32)(depth - 1) << 16) | qid);
	cmd.cdw11 = cpu_to_le32(((u32)qid << 16) | 0x1u);
	ret = rc_nvme_admin_cmd(adapter, &cmd, NULL);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_create_one_io_queue: %s qid=%u Create I/O SQ failed (%d)\n",
			  pci_name(adapter->pdev), qid, ret);
		goto err_delete_cq;
	}

	/* Wire the per-queue ISR.  The pci_irq_vector call resolves the
	 * MSI-X slot to a Linux IRQ number; the kernel will route this
	 * vector's interrupts to rc_nvme_io_queue_irq with `q` as dev_id.
	 * vec_index 0 = single-vector fallback: the admin vector's handler
	 * (rc_hw_interrupt_handler → rc_nvme_irq) already covers vector 0
	 * and drains shared queues, so no request_irq of our own. */
	if (vec_index == 0) {
		q->shares_admin_vector = true;
	} else {
		q->irq_vector = pci_irq_vector(adapter->pdev, vec_index);
		if (q->irq_vector < 0) {
			ret = q->irq_vector;
			rc_printk(RC_ERROR,
				  "rc_nvme_create_one_io_queue: %s qid=%u pci_irq_vector(%u) failed (%d)\n",
				  pci_name(adapter->pdev), qid, vec_index, ret);
			goto err_delete_sq;
		}
		ret = request_irq(q->irq_vector, rc_nvme_io_queue_irq, 0,
				  "rcraid-ioq", q);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_nvme_create_one_io_queue: %s qid=%u request_irq(%d) failed (%d)\n",
				  pci_name(adapter->pdev), qid, q->irq_vector, ret);
			q->irq_vector = -1;
			goto err_delete_sq;
		}
	}

	nvme->io_queues[qid - 1] = q;
	rc_printk(RC_NOTE,
		  "rc_nvme_create_one_io_queue: %s qid=%u up — SQ/CQ depth=%u IV=%u IRQ=%d%s\n",
		  pci_name(adapter->pdev), qid, depth, vec_index, q->irq_vector,
		  q->shares_admin_vector ? " (shared with admin)" : "");
	return 0;

err_delete_sq:
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_SQ;
	cmd.cdw10 = cpu_to_le32(qid);
	(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
err_delete_cq:
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_CQ;
	cmd.cdw10 = cpu_to_le32(qid);
	(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
err_free_cq:
	dma_free_coherent(dev, cq_bytes, q->cq, q->cq_dma);
err_free_sq:
	dma_free_coherent(dev, sq_bytes, q->sq, q->sq_dma);
err_free_ctx:
	kfree(q);
	return ret;
}

/* Tear down one I/O queue: free_irq, Delete I/O SQ, Delete I/O CQ,
 * release DMA + context.  Safe against partial init (any unset field
 * just skips its cleanup). */
static void rc_nvme_destroy_one_io_queue(struct rc_adapter *adapter,
					 struct rc_nvme_io_queue *q)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd;
	size_t sq_bytes, cq_bytes;
	bool ctrl_alive;

	if (!q)
		return;

	/* Don't issue Delete SQ / Delete CQ at a dead or surprise-removed
	 * controller: each admin command would burn the full admin timeout
	 * (2 s × 2 cmds × up to 4 queues on a hot-removed device).  The
	 * DMA and IRQ cleanup below is host-side and always runs. */
	ctrl_alive = !READ_ONCE(nvme->dead) && adapter->ctx.mmio_base &&
		     readl(adapter->ctx.mmio_base + RC_NVME_REG_CSTS) !=
			     0xffffffff;

	if (q->irq_vector >= 0) {
		free_irq(q->irq_vector, q);
		q->irq_vector = -1;
	}

	if (q->sq) {
		if (ctrl_alive) {
			memset(&cmd, 0, sizeof(cmd));
			cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_SQ;
			cmd.cdw10 = cpu_to_le32(q->qid);
			(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
		}
		sq_bytes = (size_t)q->sq_depth * RC_NVME_SQ_ENTRY_SIZE;
		dma_free_coherent(dev, sq_bytes, q->sq, q->sq_dma);
		q->sq = NULL;
	}
	if (q->cq) {
		if (ctrl_alive) {
			memset(&cmd, 0, sizeof(cmd));
			cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_CQ;
			cmd.cdw10 = cpu_to_le32(q->qid);
			(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
		}
		cq_bytes = (size_t)q->cq_depth * RC_NVME_CQ_ENTRY_SIZE;
		dma_free_coherent(dev, cq_bytes, q->cq, q->cq_dma);
		q->cq = NULL;
	}
	kfree(q);
}

/* How many I/O queues this adapter can actually drive: the controller's
 * grant is capped by dedicated I/O IRQ vectors (nr_irq_vectors - 1; each
 * queue's CQ gets its own vector), by online CPUs (more queues than CPUs
 * buys nothing — blk-mq maps hctxs per-CPU), and by the static Windows-
 * matching target of 4.  Mirrors the documented Windows behavior of
 * min(MSI vectors - 1, 4) (docs/REVERSE_ENGINEERING.md).  Returns 0 to
 * mean "no dedicated vectors — use 1 queue sharing the admin vector". */
static u16 rc_nvme_usable_io_queues(struct rc_adapter *adapter, u16 wanted)
{
	int io_vecs = adapter->nr_irq_vectors - 1;
	u32 cpus = num_online_cpus();

	if (io_vecs < 1)
		return 0;
	if (wanted > (u16)io_vecs)
		wanted = (u16)io_vecs;
	if (cpus >= 1 && wanted > (u16)cpus)
		wanted = (u16)cpus;
	if (wanted < 1)
		wanted = 1;
	if (wanted > RC_NVME_IO_QUEUE_TARGET)
		wanted = RC_NVME_IO_QUEUE_TARGET;
	return wanted;
}

/* Allocate nvme->nr_io_queues queue pairs.  qid = i+1, vec_index = i+1
 * (vector 0 is reserved for admin), or vec_index = 0 for every queue on
 * the single-vector fallback.  On partial failure, rolls back everything
 * created so far. */
static int rc_nvme_create_io_queues(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u16 usable;
	u16 i;
	int ret;

	if (nvme->nr_io_queues == 0)
		nvme->nr_io_queues = 1;
	if (nvme->nr_io_queues > RC_NVME_IO_QUEUE_TARGET)
		nvme->nr_io_queues = RC_NVME_IO_QUEUE_TARGET;

	/* Never bind a CQ to an IRQ vector that was never allocated — that
	 * used to fail queue creation and roll back to ZERO I/O queues on
	 * vector-constrained hosts instead of degrading gracefully. */
	usable = rc_nvme_usable_io_queues(adapter, nvme->nr_io_queues);
	if (usable == 0) {
		rc_printk(RC_WARN,
			  "rc_nvme_create_io_queues: %s no dedicated I/O IRQ vectors (%d total) — 1 I/O queue sharing the admin vector\n",
			  pci_name(adapter->pdev), adapter->nr_irq_vectors);
		nvme->nr_io_queues = 1;
		return rc_nvme_create_one_io_queue(adapter, 1, 0);
	}
	if (usable < nvme->nr_io_queues) {
		rc_printk(RC_NOTE,
			  "rc_nvme_create_io_queues: %s clamping %u granted I/O queues to %u (irq_vectors=%d, cpus=%u)\n",
			  pci_name(adapter->pdev), nvme->nr_io_queues, usable,
			  adapter->nr_irq_vectors, num_online_cpus());
		nvme->nr_io_queues = usable;
	}

	for (i = 0; i < nvme->nr_io_queues; i++) {
		ret = rc_nvme_create_one_io_queue(adapter, i + 1, i + 1);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_nvme_create_io_queues: %s queue %u creation failed (%d) — rolling back\n",
				  pci_name(adapter->pdev), i + 1, ret);
			while (i-- > 0) {
				rc_nvme_destroy_one_io_queue(adapter,
							     nvme->io_queues[i]);
				nvme->io_queues[i] = NULL;
			}
			return ret;
		}
	}
	return 0;
}

/* Reserve `n` SQE slots on this queue, failing when the SQ can't hold
 * them (an NVMe SQ is full at depth-1: tail may never catch head).  Every
 * submitter — blk-mq dispatch and sync commands —
 * must win its slots here BEFORE calling rc_nvme_io_submit; without the
 * accounting a full tag load plus an internal command wrapped sq_tail
 * over an unconsumed SQE and the controller executed corrupted/duplicate
 * commands.  Callers that reserve but then bail before submitting must
 * give the slots back via rc_nvme_sq_unreserve. */
static bool rc_nvme_sq_reserve(struct rc_nvme_io_queue *q, u32 n)
{
	unsigned long flags;
	bool ok = false;

	spin_lock_irqsave(&q->lock, flags);
	if ((u32)q->sq_depth > n &&
	    q->sq_inflight <= (u32)q->sq_depth - 1 - n) {
		q->sq_inflight += n;
		ok = true;
	}
	spin_unlock_irqrestore(&q->lock, flags);
	return ok;
}

static void rc_nvme_sq_unreserve(struct rc_nvme_io_queue *q, u32 n)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	q->sq_inflight = (q->sq_inflight >= n) ? q->sq_inflight - n : 0;
	spin_unlock_irqrestore(&q->lock, flags);
}

/* Submit one I/O command to a specific NVMe I/O queue asynchronously.
 * Caller (blk-mq queue_rq) returns BLK_STS_OK immediately; completion
 * lands in rc_nvme_io_queue_irq → blk_mq_complete_request → .complete.
 * The caller MUST hold an SQE reservation (rc_nvme_sq_reserve) for this
 * command; the slot is released when its CQE is consumed. */
static void rc_nvme_io_submit(struct rc_nvme_io_queue *q,
			      struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_sqe *slot;
	unsigned long flags;
	u32 tail;

	spin_lock_irqsave(&q->lock, flags);
	slot = (struct rc_nvme_sqe *)q->sq + q->sq_tail;
	memcpy(slot, cmd, sizeof(*cmd));

	tail = (q->sq_tail + 1) % q->sq_depth;
	q->sq_tail = tail;
	wmb();
	rc_nvme_ring_sq_doorbell(q->adapter, q->qid, tail);
	spin_unlock_irqrestore(&q->lock, flags);
}

/* Synchronous I/O command (always CID RC_VOLUME_SYNC_CID, a value outside
 * the blk-mq tag space) used for metadata reads before
 * the blk-mq disk is up — and during module reload / controller auto-reset,
 * when the disk from the surviving assembly EXISTS and this queue's ISR is
 * therefore live and consuming CQEs.  That concurrency is the whole design
 * problem here: two consumers of one CQ.  The sync_pending handshake makes
 * the CQE single-consumer — whichever side sees it first (the ISR's claim
 * branch, or this poll loop under q->lock) records the status and clears
 * sync_pending; head/phase/doorbell advance exactly once, under the lock.
 * The old lock-free version snapshotted the CQ slot and raced the ISR:
 * the ISR ate the CQE ("unknown CID=0"), this helper timed out, and the
 * member dropped to the legacy fallback mid-reload (seen on CI). */
static int rc_nvme_io_cmd_sync(struct rc_adapter *adapter,
			       struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_io_queue *q;
	unsigned long deadline;
	unsigned long flags;
	u16 sc_sct = 0;
	bool done = false;

	if (!nvme->io_queues[0])
		return -ENODEV;
	q = nvme->io_queues[0];

	/* Arm the handshake BEFORE the doorbell rings — the ISR can run the
	 * moment the controller sees the SQE.  The rolling sequence in the
	 * CID's low byte keeps a stale CQE from a previously timed-out sync
	 * command from satisfying this one.
	 *
	 * The SQE slot is reserved in the same critical section (C-4): this
	 * queue is shared with live blk-mq traffic during module reload /
	 * auto-reset, and submitting into a full SQ would wrap the tail over
	 * a live command. */
	spin_lock_irqsave(&q->lock, flags);
	if ((u32)q->sq_depth <= 1 ||
	    q->sq_inflight > (u32)q->sq_depth - 2) {
		spin_unlock_irqrestore(&q->lock, flags);
		rc_printk(RC_WARN,
			  "rc_nvme_io_cmd_sync: %s qid=%u SQ full (%u in flight) — try again\n",
			  pci_name(adapter->pdev), q->qid, q->sq_inflight);
		return -EBUSY;
	}
	q->sq_inflight++;
	q->sync_seq++;
	q->sync_cid = (u16)(RC_VOLUME_SYNC_CID_BASE | (q->sync_seq & 0xFFu));
	q->sync_pending = true;
	q->sync_sc = 0;
	cmd->cid = cpu_to_le16(q->sync_cid);
	spin_unlock_irqrestore(&q->lock, flags);

	rc_nvme_io_submit(q, cmd);

	deadline = jiffies + msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);
	while (!done && time_before(jiffies, deadline)) {
		long left = deadline - jiffies;
		long tick = min_t(long, left, max_t(long, 1, msecs_to_jiffies(1)));

		wait_event_timeout(q->cq_wait, !READ_ONCE(q->sync_pending),
				   tick);

		spin_lock_irqsave(&q->lock, flags);
		if (!q->sync_pending) {
			/* ISR claimed our CQE and recorded the status. */
			sc_sct = q->sync_sc;
			done = true;
		} else {
			/* Poll the CQ head ourselves — early boot runs before
			 * IRQ delivery is functional.  Consume only CQEs in
			 * the sync CID range: ours (sequence match) completes
			 * the wait; a stale one from a previously timed-out
			 * sync command is consumed and DROPPED (attributing
			 * it to this command would report a status for a read
			 * that never completed).  Anything else on this queue
			 * belongs to the ISR and is left in place. */
			struct rc_nvme_cqe *cqe =
				(struct rc_nvme_cqe *)q->cq + q->cq_head;
			u16 status = le16_to_cpu(READ_ONCE(cqe->status));
			u16 ccid = le16_to_cpu(cqe->cid);

			if ((status & 1) == q->cq_phase &&
			    (ccid & RC_VOLUME_SYNC_CID_MASK) ==
					RC_VOLUME_SYNC_CID_BASE) {
				if (ccid == q->sync_cid) {
					sc_sct = (status >> 1) & 0x7fff;
					q->sync_pending = false;
					done = true;
				} else {
					rc_printk(RC_WARN,
						  "rc_nvme_io_cmd_sync: dropping stale sync CQE CID=0x%x (expected 0x%x)\n",
						  ccid, q->sync_cid);
				}
				/* CQE consumed here → SQE slot free (C-4). */
				if (q->sq_inflight)
					q->sq_inflight--;
				q->cq_head = (q->cq_head + 1) % q->cq_depth;
				if (q->cq_head == 0)
					q->cq_phase ^= 1;
				rc_nvme_ring_cq_doorbell(adapter, q->qid,
							 q->cq_head);
			}
		}
		spin_unlock_irqrestore(&q->lock, flags);
	}

	if (!done) {
		/* Disarm so a late CQE doesn't satisfy the NEXT sync command
		 * with this one's stale status. */
		spin_lock_irqsave(&q->lock, flags);
		q->sync_pending = false;
		spin_unlock_irqrestore(&q->lock, flags);
		rc_printk(RC_ERROR,
			  "rc_nvme_io_cmd_sync: timeout waiting for CQE (opc=0x%02x)\n",
			  cmd->opc);
		return -ETIMEDOUT;
	}

	if (sc_sct) {
		rc_printk(RC_ERROR,
			  "rc_nvme_io_cmd_sync: opc=0x%02x SC/SCT=0x%04x\n",
			  cmd->opc, sc_sct);
		return -EIO;
	}
	return 0;
}

/* Forward decls — defined below. */
static int rc_nvme_read_lba(struct rc_adapter *adapter, u64 slba,
			    u16 nlb_zbased, dma_addr_t buf_dma);
static int rc_nvme_write_lba(struct rc_adapter *adapter, u64 slba,
			     u16 nlb_zbased, dma_addr_t buf_dma);
static int rc_volume_create_disk(void);
static void rc_volume_parse_logical_device(struct rc_adapter *adapter);
static void rc_nvme_auto_reset_fn(struct work_struct *w);
void rc_volume_teardown(void);

/* AMD RAIDCore metadata checksum (ported from rcraid.sys FUN_1400014ec).
 *
 * Treats the input as a sequence of 64-bit little-endian words.  For each
 * word it picks two 16-bit "lanes":
 *
 *   - lane_a = current accumulator & 3
 *   - lane_b = current word        & 3
 *
 * If both choose the same lane, fall back to (iter & 3, (iter+1) & 3).
 * Swap the two 16-bit lanes inside the word, then XOR the modified word
 * into the accumulator.  Final accumulator is the checksum.
 */
static u64 rc_raidcore_checksum(const void *data, size_t bytes)
{
	const u8 *bp = data;
	size_t words = bytes >> 3;
	u64 acc = 0;
	size_t i;

	for (i = 0; i < words; i++) {
		u64 w = get_unaligned_le64(bp + i * 8);
		unsigned int lane_a = (unsigned int)(acc & 3);
		unsigned int lane_b = (unsigned int)(w & 3);
		u16 lanes[4];
		u16 tmp;
		int k;

		if (lane_a == lane_b) {
			lane_a = (unsigned int)(i & 3);
			lane_b = (unsigned int)((i + 1) & 3);
		}

		for (k = 0; k < 4; k++)
			lanes[k] = (u16)(w >> (k * 16));

		tmp = lanes[lane_a];
		lanes[lane_a] = lanes[lane_b];
		lanes[lane_b] = tmp;

		w = 0;
		for (k = 0; k < 4; k++)
			w |= (u64)lanes[k] << (k * 16);

		acc ^= w;
	}
	return acc;
}

/* Write (nlb_zbased + 1) LBAs from a DMA-coherent buffer.  Same PRP rules
 * and 2-page ceiling as rc_nvme_read_lba: PRP2 when the transfer spills
 * past the first page.  Synchronous — the caller owns the barrier. */
static int rc_nvme_write_lba(struct rc_adapter *adapter, u64 slba,
			     u16 nlb_zbased, dma_addr_t buf_dma)
{
	struct rc_nvme_sqe cmd;
	u32 bytes = ((u32)nlb_zbased + 1) * 512u;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_NVM_OP_WRITE;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(buf_dma);
	if (bytes > PAGE_SIZE) {
		if (WARN_ON_ONCE(bytes > 2 * PAGE_SIZE))
			return -EINVAL;
		cmd.prp2 = cpu_to_le64(buf_dma + PAGE_SIZE);
	}
	cmd.cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd.cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd.cdw12 = cpu_to_le32(nlb_zbased);
	return rc_nvme_io_cmd_sync(adapter, &cmd);
}

/* NVMe FLUSH (opcode 0x00): push the writes above to media before the
 * read-back, so a verified probe means verified on the medium and not
 * just in the controller's write cache. */
static int rc_nvme_flush(struct rc_adapter *adapter)
{
	struct rc_nvme_sqe cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc  = RC_NVME_NVM_OP_FLUSH;
	cmd.nsid = cpu_to_le32(1);
	return rc_nvme_io_cmd_sync(adapter, &cmd);
}

/* Write one sector, flush, read it back and compare.  Every step is
 * logged on failure; `what` names the phase in the message. */
static int rc_nvme_write_verify_lba(struct rc_adapter *adapter, u64 slba,
				    const u8 *src, const char *what)
{
	struct device *dev = &adapter->pdev->dev;
	void *buf;
	dma_addr_t buf_dma;
	int ret;

	buf = dma_alloc_coherent(dev, PAGE_SIZE, &buf_dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, src, RC_MB_SECTOR);
	ret = rc_nvme_write_lba(adapter, slba, 0, buf_dma);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_meta_write_probe: %s %s write LBA 0x%llx failed (%d)\n",
			  pci_name(adapter->pdev), what,
			  (unsigned long long)slba, ret);
		goto out;
	}

	ret = rc_nvme_flush(adapter);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_meta_write_probe: %s %s flush failed (%d)\n",
			  pci_name(adapter->pdev), what, ret);
		goto out;
	}

	memset(buf, 0, RC_MB_SECTOR);
	ret = rc_nvme_read_lba(adapter, slba, 0, buf_dma);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_meta_write_probe: %s %s read-back LBA 0x%llx failed (%d)\n",
			  pci_name(adapter->pdev), what,
			  (unsigned long long)slba, ret);
		goto out;
	}

	if (memcmp(buf, src, RC_MB_SECTOR)) {
		rc_printk(RC_ERROR,
			  "rc_meta_write_probe: %s %s read-back mismatch at LBA 0x%llx — metadata write path is broken\n",
			  pci_name(adapter->pdev), what,
			  (unsigned long long)slba);
		ret = -EIO;
		goto out;
	}

out:
	dma_free_coherent(dev, PAGE_SIZE, buf, buf_dma);
	return ret;
}

/* Metadata write probe: rewrite this member's own RC_MetaData block at
 * LBA 0x5000 through the driver's NVMe queues, flush, and read it back.
 * The rewrite is gated on an exact match with the block on media, so the
 * array's meaning cannot change; the optional junk phase proves the media
 * really received the write (a dropped command would leave the read-back
 * equal by luck) and then restores the real block.  Never fails assembly:
 * a broken write path is logged loudly and left for the caller to notice. */
static void rc_nvme_meta_write_probe(struct rc_adapter *adapter,
				     const u8 *on_disk)
{
	struct device *dev = &adapter->pdev->dev;
	u8 *page, *built, *junk;
	dma_addr_t page_dma;
	bool junk_done = false;
	int ret;

	/* Two sectors in one page rather than two stack blocks: keeps the
	 * frame small enough for -Wframe-larger-than and gives the buffers
	 * the alignment the NVMe PRP path expects. */
	page = dma_alloc_coherent(dev, PAGE_SIZE, &page_dma, GFP_KERNEL);
	if (!page)
		return;
	built = page;
	junk = page + RC_MB_SECTOR;

	rc_mb_metadata_block_ex(built, adapter->ctx.nvme.md_device_id,
				adapter->ctx.nvme.md_mbr_checksum);

	if (memcmp(built, on_disk, RC_MB_SECTOR)) {
		rc_printk(RC_ERROR,
			  "rc_meta_write_probe: %s refusing to write: encoder output does not match the block on media\n",
			  pci_name(adapter->pdev));
		goto out;
	}

	ret = rc_nvme_write_verify_lba(adapter, RC_MB_METADATA_LBA, built,
				       "rewrite");
	if (ret)
		goto out;

	if (rc_meta_write_probe_junk) {
		rc_mb_metadata_block(junk,
				     ~adapter->ctx.nvme.md_device_id);
		ret = rc_nvme_write_verify_lba(adapter, RC_MB_METADATA_LBA,
					       junk, "junk");
		if (!ret)
			junk_done = true;
		/* Restore regardless: after a failed junk phase we cannot
		 * tell whether the junk block reached media, and a member
		 * left carrying one will not assemble. */
		ret = rc_nvme_write_verify_lba(adapter, RC_MB_METADATA_LBA,
					       built, "restore");
		if (ret)
			goto out;
	}

	rc_printk(RC_NOTE,
		  "rc_meta_write_probe: %s probe complete (LBA 0x%x rewrite verified%s)\n",
		  pci_name(adapter->pdev), RC_MB_METADATA_LBA,
		  junk_done ? ", media proven changed, original restored" : "");

out:
	dma_free_coherent(dev, PAGE_SIZE, page, page_dma);
}


/* Encoder self-check: rebuild the RC_MetaData block this member carries
 * from its parsed DeviceId and compare against the bytes just read from
 * media.  Read-only — nothing is written.  A match means the encoder in
 * rc_meta_build.h reproduces the firmware block field for field (it also
 * covers the checksum, since the encoder computes its own and the parser
 * has already validated the media's).  A mismatch names the first
 * differing byte so the encoder can be corrected; it must match before
 * any code is allowed to stamp a member with it. */
static void rc_nvme_verify_meta_encoding(struct rc_adapter *adapter,
					 const u8 *on_disk)
{
	u8 built[RC_MB_SECTOR];
	u32 i, first = 0;

	/* MBRChecksum is firmware-owned and lives outside this block; carry
	 * the value the media has so the re-encode is byte-exact. */
	rc_mb_metadata_block_ex(built, adapter->ctx.nvme.md_device_id,
				adapter->ctx.nvme.md_mbr_checksum);

	/* Search the payload (everything but the checksum word) so a bad
	 * field is named directly; the checksum is reported separately,
	 * since any payload change also changes it. */
	for (i = sizeof(u64); i < RC_MB_SECTOR; i++) {
		if (built[i] != on_disk[i]) {
			first = i;
			break;
		}
	}

	if (!first) {
		if (!memcmp(built, on_disk, sizeof(u64))) {
			rc_printk(RC_NOTE,
				  "rc_nvme_verify_meta_encoding: %s encoder reproduces the on-media RC_MetaData block exactly\n",
				  pci_name(adapter->pdev));
			return;
		}
		rc_printk(RC_NOTE,
			  "rc_nvme_verify_meta_encoding: %s payload matches on media but the checksum differs (encoder 0x%016llx, media 0x%016llx) — metadata writer stays disabled for this member\n",
			  pci_name(adapter->pdev),
			  (unsigned long long)rc_mb_get64(built),
			  (unsigned long long)rc_mb_get64(on_disk));
		return;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_verify_meta_encoding: %s re-encoded RC_MetaData differs at +0x%x (encoder 0x%02x, media 0x%02x, checksum %s) — metadata writer stays disabled for this member\n",
		  pci_name(adapter->pdev), first, built[first], on_disk[first],
		  memcmp(built, on_disk, sizeof(u64)) ? "also differs" : "matches");
}

/* Read LBA 0x5000 (the RAIDCore metadata block), verify magic / version /
 * checksum, and stash the shared and per-member fields in adapter state.
 * Returns 0 on success, negative errno on failure. */
static int rc_nvme_read_validate_metadata(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_raidcore_md *md;
	void *buf;
	dma_addr_t buf_dma;
	u64 stored_csum, calc_csum, magic;
	u32 version;
	int ret;

	buf = dma_alloc_coherent(dev, PAGE_SIZE, &buf_dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = rc_nvme_read_lba(adapter, RC_RAIDCORE_LBA, 0, buf_dma);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_read_validate_metadata: read LBA 0x%llx failed (%d)\n",
			  (unsigned long long)RC_RAIDCORE_LBA, ret);
		goto out;
	}

	md = buf;
	magic = le64_to_cpu(md->magic);
	if (magic != RC_RAIDCORE_MAGIC) {
		rc_printk(RC_ERROR,
			  "rc_nvme_read_validate_metadata: bad magic 0x%016llx (expected 0x%016llx)\n",
			  (unsigned long long)magic,
			  (unsigned long long)RC_RAIDCORE_MAGIC);
		ret = -ENOENT;
		goto out;
	}

	version = le32_to_cpu(md->version);
	if (version != RC_RAIDCORE_VERSION) {
		rc_printk(RC_ERROR,
			  "rc_nvme_read_validate_metadata: unsupported version 0x%08x (expected 0x%08x)\n",
			  version, RC_RAIDCORE_VERSION);
		ret = -ENOTSUPP;
		goto out;
	}

	stored_csum = le64_to_cpu(md->checksum);
	calc_csum = rc_raidcore_checksum((const u8 *)md + 8,
					 RC_RAIDCORE_PAYLOAD_BYTES);
	if (stored_csum != calc_csum) {
		rc_printk(RC_ERROR,
			  "rc_nvme_read_validate_metadata: checksum mismatch (stored 0x%016llx, computed 0x%016llx)\n",
			  (unsigned long long)stored_csum,
			  (unsigned long long)calc_csum);
		ret = -EBADMSG;
		goto out;
	}

	nvme->md_device_id         = le64_to_cpu(md->device_id);
	nvme->md_config_commit_lba = le64_to_cpu(md->config_commit_lba);
	nvme->md_config_ring_lba   = le64_to_cpu(md->config_ring_lba);
	nvme->md_stripe_sectors    = le32_to_cpu(md->stripe_sectors);
	nvme->md_version           = version;
	nvme->md_features          = le32_to_cpu(md->features);
	nvme->md_spare_info        = le32_to_cpu(md->spare_info);
	nvme->md_mbr_checksum      = le64_to_cpu(md->mbr_checksum);
	nvme->md_valid             = true;

	rc_printk(RC_NOTE,
		  "rc_nvme_read_validate_metadata: RAIDCore v0x%08x ConfigRingSize=%u (used as stripe sectors = %u KiB) device_id=0x%016llx ConfigCommit@LBA=0x%llx ConfigRing@LBA=0x%llx features=0x%08x mbr_csum=0x%016llx\n",
		  version, nvme->md_stripe_sectors,
		  (nvme->md_stripe_sectors * nvme->ns1_lba_bytes) >> 10,
		  (unsigned long long)nvme->md_device_id,
		  (unsigned long long)nvme->md_config_commit_lba,
		  (unsigned long long)nvme->md_config_ring_lba,
		  nvme->md_features,
		  (unsigned long long)nvme->md_mbr_checksum);

	rc_nvme_verify_meta_encoding(adapter, buf);
	if (rc_meta_write_probe)
		rc_nvme_meta_write_probe(adapter, buf);

	/* Walk the config ring to find the active RC_LogicalDevice record;
	 * gives us the real member count, per-member position, and
	 * (where the LD writes it) the chunk size.  Logged inside. */
	rc_volume_parse_logical_device(adapter);
	ret = 0;

out:
	dma_free_coherent(dev, PAGE_SIZE, buf, buf_dma);
	return ret;
}

/* Read (nlb_zbased + 1) LBAs starting at slba into the given DMA-coherent
 * buffer.  The buffer must be PAGE_SIZE-aligned (dma_alloc_coherent gives us
 * that) and large enough for the transfer.  PRP2 is set when the transfer
 * spills past the first page.  Transfers longer than 2 * PAGE_SIZE require a
 * PRP list (not yet implemented); caller must keep within that range. */
static int rc_nvme_read_lba(struct rc_adapter *adapter, u64 slba, u16 nlb_zbased,
			    dma_addr_t buf_dma)
{
	struct rc_nvme_sqe cmd;
	u32 bytes = ((u32)nlb_zbased + 1) * 512u;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_NVM_OP_READ;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(buf_dma);
	if (bytes > PAGE_SIZE) {
		if (WARN_ON_ONCE(bytes > 2 * PAGE_SIZE))
			return -EINVAL;
		cmd.prp2 = cpu_to_le64(buf_dma + PAGE_SIZE);
	}
	cmd.cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd.cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd.cdw12 = cpu_to_le32(nlb_zbased);
	return rc_nvme_io_cmd_sync(adapter, &cmd);
}

/* Compare two adapters by PCI BDF for deterministic member ordering. */
static int rc_volume_bdf_cmp(struct rc_adapter *a, struct rc_adapter *b)
{
	unsigned int abdf = (a->pdev->bus->number << 8) | a->pdev->devfn;
	unsigned int bbdf = (b->pdev->bus->number << 8) | b->pdev->devfn;
	int cmp = (int)abdf - (int)bbdf;
	return rc_volume_reverse_order ? -cmp : cmp;
}

/* Chunking for the active-generation scan.  rc_nvme_read_lba caps a transfer
 * at 2 pages (8 KiB = 16 sectors).  Consecutive chunks overlap by
 * RC_LD_SCAN_OVERLAP_SECTORS so a record that starts near the end of one
 * chunk is re-read whole at the head of the next: new record tags are only
 * accepted in the first (CHUNK - OVERLAP) sectors of a non-final chunk.  The
 * overlap (1 KiB) exceeds the largest possible LD record
 * (0x130 fixed bytes + RC_VOLUME_MAX_MEMBERS * RC_LE_BYTES). */
#define RC_LD_SCAN_CHUNK_SECTORS	16u
#define RC_LD_SCAN_OVERLAP_SECTORS	2u

/* Human-readable normalized level (RC_LDT_*) for log lines. */
static const char *rc_level_name(u32 level)
{
	switch (level) {
	case RC_LDT_RAID0:	return "RAID0";
	case RC_LDT_RAID1:	return "RAID1";
	case RC_LDT_RAID5:	return "RAID5";
	case RC_LDT_RAID6:	return "RAID6";
	case RC_LDT_RAID10:	return "RAID10";
	default:		return "UNSUPPORTED";
	}
}

/* Derive the effective RAID level from the on-disk DeviceType +
 * FirstCount (stripe width) x SecondCount (mirror count).  Real firmware
 * writes DeviceType 0x1BF6 for BOTH RAID0 and RAID1 and encodes the layout
 * in the counts (verified on TRX50 hardware dumps, 2026-07-10 — see the
 * RC_LDT_* note in rc_linux.h).  Returns a normalized RC_LDT_RAID0 /
 * RC_LDT_RAID1 / RC_LDT_RAID10 (and, from an explicit parity DeviceType,
 * RC_LDT_RAID5 / RC_LDT_RAID6), or 0 for any layout this driver has no
 * mapping for (>2-way mirrors, count/device mismatches) — the caller must
 * refuse to assemble those rather than guess.  Parity levels are
 * read-only: assembly and the single-member read path know them, the
 * write path does not. */
static u32 rc_ld_level_from(u32 devtype, u32 first, u32 second, u32 devices)
{
	/* 1. Dedicated Baseline Mirror Gating */
	if (devtype == RC_LDT_RAID1) {
		return (devices == 2 && (!first || !second || (first == 1 && second == 2))) ? RC_LDT_RAID1 : 0;
	}

	/* 2. Stripe & Nested Mirror Matrices (RAID 0 / RAID 10) */
	if (devtype == RC_LDT_RAID0) {
		if (!first || !second || first * second != devices)
			return 0;
		if (first == 1 && second == 2)
			return RC_LDT_RAID1;	/* 2-way mirror: firmware's RAID1
						 * encoding (0x1BF6 + 1x2, see
						 * rc_linux.h) */
		if (second == 1)
			return RC_LDT_RAID0;	/* Pure RAID 0 */
		if (first >= 2 && second == 2)
			return RC_LDT_RAID10;	/* Nested RAID 10 */
	}

	/* 3. Parity levels (RAID5 / RAID6).  Identified ONLY by the explicit
	 *    DeviceType: a parity layout cannot be expressed in
	 *    FirstCount x SecondCount ((N-1) x 1 and (N-2) x 2 never equal
	 *    N), so a count-encoded 0x1BF6 record is never a parity array and
	 *    an unlabelled four-member (2,2) record stays RAID10 — the
	 *    encoding confirmed on hardware.  These arms deliberately ignore
	 *    the counts, so an explicit RAID6 DeviceType can never fall
	 *    through to the RAID10 classification above (issue #3).  If
	 *    0x1BFA turns out to be a NESTED parity level (RAID50/60), this
	 *    arm is the one to change: nested parity is neither this map nor
	 *    a plain RAID6 map.  (0x1BF5/0x1BFA are confirmed by the vendor
	 *    dispatch: parity count = (devtype == 0x1BFA) + 1, and a
	 *    BIOS-created 3-disk RAID 5 writes 0x1BF5 — the two constants
	 *    were swapped here before 2026-11-08.)
	 *
	 *    (An earlier iteration carried 0x1BF7/0x1BF8 branches here; both
	 *    were miswired — 0x1BF7 IS RAID1 and 0x1BF8 has no define — and
	 *    returning either would have assembled a parity array with RAID0
	 *    stripe math, corrupting every stripe.) */
	if (devtype == RC_LDT_RAID5)
		return devices >= 3 ? RC_LDT_RAID5 : 0;
	if (devtype == RC_LDT_RAID6)
		return devices >= 4 ? RC_LDT_RAID6 : 0;

	return 0; /* Safety reject on unmapped structural variations */
}

/* Read the config-commit block and return the active config generation's
 * extent within the ring.  The ring is a journal — records of DELETED
 * arrays persist in older generations, and both can list this disk's
 * DeviceID, so only records inside the committed generation may be trusted
 * (assembling the first DeviceID match presented a deleted RAID0 in place
 * of the live RAID1 on real hardware).  Layout in rc_linux.h.
 *
 * Validation: the extent must lie fully inside the config ring, and the
 * generation header's leading timestamp must equal the commit block's
 * recorded generation timestamp (two-phase-commit linkage) — checked by
 * the caller when it reads the first chunk.  Returns 0 and fills the out
 * params on success. */
static int rc_volume_read_commit(struct rc_adapter *adapter, u8 *buf,
				 dma_addr_t buf_dma, u64 *out_gen_lba,
				 u32 *out_gen_bytes, u64 *out_gen_ts)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u64 ring_lba  = nvme->md_config_ring_lba;
	u64 ring_end  = ring_lba + nvme->md_stripe_sectors; /* ConfigRingSize */
	u64 gen_lba, gen_ts;
	u32 gen_bytes, gen_seq;
	int ret;

	ret = rc_nvme_read_lba(adapter, nvme->md_config_commit_lba, 0,
			       buf_dma);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_volume_read_commit: %s read commit LBA 0x%llx failed (%d)\n",
			  pci_name(adapter->pdev),
			  (unsigned long long)nvme->md_config_commit_lba, ret);
		return ret;
	}

	gen_lba   = get_unaligned_le32(buf + RC_COMMIT_GEN_LBA_OFFSET);
	gen_bytes = get_unaligned_le32(buf + RC_COMMIT_GEN_LEN_OFFSET);
	gen_ts    = get_unaligned_le64(buf + RC_COMMIT_GEN_TS_OFFSET);
	gen_seq   = get_unaligned_le32(buf + RC_COMMIT_GEN_SEQ_OFFSET);

	if (gen_lba < ring_lba || gen_lba >= ring_end ||
	    !gen_bytes || gen_bytes % 512u ||
	    gen_lba + gen_bytes / 512u > ring_end ||
	    !gen_ts) {
		rc_printk(RC_WARN,
			  "rc_volume_read_commit: %s commit block invalid (gen@0x%llx len=%u ts=0x%016llx, ring 0x%llx..0x%llx) — cannot locate the active config generation\n",
			  pci_name(adapter->pdev),
			  (unsigned long long)gen_lba, gen_bytes,
			  (unsigned long long)gen_ts,
			  (unsigned long long)ring_lba,
			  (unsigned long long)ring_end);
		return -EBADMSG;
	}

	rc_printk(RC_NOTE,
		  "rc_volume_read_commit: %s active config generation @LBA 0x%llx len=%u seq=%u ts=0x%016llx\n",
		  pci_name(adapter->pdev), (unsigned long long)gen_lba,
		  gen_bytes, gen_seq, (unsigned long long)gen_ts);

	*out_gen_lba   = gen_lba;
	*out_gen_bytes = gen_bytes;
	*out_gen_ts    = gen_ts;
	return 0;
}

/* Locate this member's RC_LogicalDevice record (tag 0x25BD) inside the
 * ACTIVE config generation — the extent named by the commit block, NOT the
 * whole ring (older generations hold records of deleted arrays that also
 * list this disk's DeviceID).  Parse Devices, DeviceType,
 * FirstCount/SecondCount (from which the effective RAID level is derived),
 * Capacity, ChunkSize, and walk the LogicalElement array to find this
 * adapter's position (where md_device_id matches an element's DeviceID).
 * Results land in adapter->ctx.nvme.ld_*.  Failure leaves ld_valid=false
 * — the caller falls back to the legacy BDF-ordered registration path,
 * which is marked geometry-untrusted (read-only). */
static void rc_volume_parse_logical_device(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u8 *buf;
	dma_addr_t buf_dma;
	const u32 chunk_bytes = RC_LD_SCAN_CHUNK_SECTORS * 512u;
	const u32 chunk_step  = (RC_LD_SCAN_CHUNK_SECTORS -
				 RC_LD_SCAN_OVERLAP_SECTORS) * 512u;
	u64 gen_lba, gen_ts;
	u32 gen_bytes;
	u32 pos;

	nvme->ld_valid = false;
	nvme->ld_my_position = -1;
	nvme->ld_record_seen = false;
	nvme->ld_raw_len = 0;

	if (!nvme->md_config_ring_lba || !nvme->md_config_commit_lba) {
		rc_printk(RC_WARN,
			  "rc_volume_parse_logical_device: %s no config ring/commit LBA in metadata\n",
			  pci_name(adapter->pdev));
		return;
	}

	buf = dma_alloc_coherent(dev, chunk_bytes, &buf_dma, GFP_KERNEL);
	if (!buf)
		return;

	if (rc_volume_read_commit(adapter, buf, buf_dma,
				  &gen_lba, &gen_bytes, &gen_ts))
		goto done;

	/* Walk the generation extent in overlapping chunks.  `pos` is the
	 * byte offset of the current chunk within the generation. */
	for (pos = 0; pos < gen_bytes; pos += chunk_step) {
		u32 avail = min(chunk_bytes, gen_bytes - pos);
		bool final = (pos + chunk_bytes >= gen_bytes);
		/* Records whose tag starts in the overlap tail are picked up
		 * by the next chunk instead (they may spill past this one). */
		u32 scan_limit = final ? avail : chunk_step;
		/* The generation's first 0x200 bytes are its header sector
		 * (timestamp + table offsets), not records — skip it rather
		 * than tag-scan bytes the on-disk format says aren't records. */
		u32 i = (pos == 0) ? min(0x200u, scan_limit) : 0;
		int ret;

		ret = rc_nvme_read_lba(adapter, gen_lba + pos / 512u,
				       (u16)(avail / 512u - 1), buf_dma);
		if (ret) {
			rc_printk(RC_WARN,
				  "rc_volume_parse_logical_device: %s read LBA 0x%llx failed (%d)\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)(gen_lba + pos / 512u),
				  ret);
			break;
		}

		/* First chunk carries the generation header: verify the
		 * two-phase-commit linkage before trusting any record. */
		if (pos == 0 &&
		    get_unaligned_le64(buf + RC_GEN_HEADER_TS_OFFSET) != gen_ts) {
			rc_printk(RC_WARN,
				  "rc_volume_parse_logical_device: %s generation header ts 0x%016llx != committed ts 0x%016llx — commit/journal mismatch, refusing to parse\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)get_unaligned_le64(buf + RC_GEN_HEADER_TS_OFFSET),
				  (unsigned long long)gen_ts);
			break;
		}

		for (; i + 4 <= scan_limit; i += 4) {
			u8 *ld;
			u32 devtype, devices, elem_off, chunk, chunk_index;
			u32 first_count, second_count, level;
			u64 capacity;
			int my_pos = -1;
			u32 j;

			if (get_unaligned_le32(buf + i) !=
			    RC_DST_LOGICAL_DEVICE)
				continue;

			/* Every fixed-offset field below must lie within the
			 * bytes THIS transfer actually read — chunk_index at
			 * +0x110 is the farthest.  A tag this close to the end
			 * of a non-final chunk is re-scanned at the head of
			 * the next (overlap); this close to the end of the
			 * FINAL chunk the record is truncated by the committed
			 * generation length and invalid anyway.  Without this
			 * check the reads land in stale bytes from a previous
			 * transfer — or, for avail == chunk_bytes, past the
			 * end of the DMA buffer allocation entirely. */
			if ((u64)i + RC_LD_CHUNKINDEX_OFFSET + 4 > avail)
				continue;

			ld = buf + i;
			devtype  = get_unaligned_le32(ld + RC_LD_DEVICETYPE_OFFSET);
			devices  = get_unaligned_le32(ld + RC_LD_DEVICES_OFFSET);
			elem_off = get_unaligned_le32(ld + RC_LD_ELEMENTOFFSET_OFFSET);
			chunk    = get_unaligned_le32(ld + RC_LD_CHUNKSIZE_OFFSET);
			chunk_index = get_unaligned_le32(ld + RC_LD_CHUNKINDEX_OFFSET);
			capacity = get_unaligned_le64(ld + RC_LD_CAPACITY_OFFSET);
			/* FirstCount x SecondCount = stripe width x mirror
			 * count — the REAL RAID-level encoding (DeviceType is
			 * 0x1BF6 for both RAID0 and RAID1 on real firmware;
			 * see rc_ld_level_from). */
			first_count =
				get_unaligned_le32(ld + RC_LD_FIRSTCOUNT_OFFSET);
			second_count =
				get_unaligned_le32(ld + RC_LD_SECONDCOUNT_OFFSET);

			if (devices < 1 || devices > RC_VOLUME_MAX_MEMBERS)
				continue;
			if ((u64)i + elem_off +
			    (u64)devices * RC_LE_BYTES > avail)
				continue;

			/* The committed generation names at least one LD.  If
			 * none of them contains this disk's DeviceID, this disk
			 * is not a member of the active array (a spare, or stale
			 * metadata) — see the fallback guard in
			 * register_member. */
			nvme->ld_record_seen = true;

			/* AMD also publishes one single-device "raw disk"
			 * LD per physical member.  Only the LD whose
			 * element array contains our DeviceID is the volume
			 * we belong to. */
			u64 alloc_off = 0, alloc_sz = 0;
			u64 user_off = 0, user_sz = 0;
			for (j = 0; j < devices; j++) {
				u8 *elem = ld + elem_off + j * RC_LE_BYTES;
				u64 eid =
					get_unaligned_le64(elem + RC_LE_DEVICEID_OFFSET);
				if (eid == nvme->md_device_id) {
					my_pos = (int)j;
					alloc_off = get_unaligned_le64(elem + RC_LE_ALLOC_OFFSET_OFFSET);
					alloc_sz  = get_unaligned_le64(elem + RC_LE_ALLOC_SIZE_OFFSET);
					user_off  = get_unaligned_le64(elem + RC_LE_USERDATA_OFFSET_OFFSET);
					user_sz   = get_unaligned_le64(elem + RC_LE_USERDATA_SIZE_OFFSET);
					break;
				}
			}

			level = rc_ld_level_from(devtype, first_count,
						 second_count, devices);

			/* RC_NOTE, not RC_INFO: this is the one-time record of
			 * which LD record assembled the volume and at what RAID
			 * level — it must land in dmesg at the default
			 * debug_level (the QEMU rig also asserts on it). */
			rc_printk(RC_NOTE,
				  "rc_volume_parse_logical_device: %s LD@gen+%u.%u devtype=0x%04x first=%u second=%u (level=%s) devices=%u chunk=%u chunk_idx=%u capacity=%llu my_pos=%d alloc_off=%llu alloc_sz=%llu user_off=%llu user_sz=%llu%s\n",
				  pci_name(adapter->pdev),
				  (pos + i) / 512, (pos + i) % 512,
				  devtype, first_count, second_count,
				  rc_level_name(level),
				  devices, chunk, chunk_index,
				  (unsigned long long)capacity, my_pos,
				  (unsigned long long)alloc_off,
				  (unsigned long long)alloc_sz,
				  (unsigned long long)user_off,
				  (unsigned long long)user_sz,
				  my_pos < 0 ? " (skip — not our member)" :
				  devices < 2 ? " (skip — raw single-disk LD)" :
					        " (match)");

			if (my_pos < 0)
				continue;

			/* A devices < 2 record with our DeviceID is this
			 * disk's own raw "single disk" LD (0x1BF9), which
			 * firmware publishes for every non-array disk — NOT
			 * the array we belong to.  Matching it would assemble
			 * a bogus 1-member volume over the raw disk. */
			if (devices < 2)
				continue;

			/* Keep the record verbatim for /dev/rc_api: rcadm
			 * correlates members through the element table, so
			 * re-emitting firmware's own bytes is more faithful
			 * than rebuilding the record from these fields. */
			{
				u32 rec_len = elem_off + devices * RC_LE_BYTES;
				u32 pkt_len =
					get_unaligned_le32(ld + RC_LD_PACKETSIZE_OFFSET);

				/*
				 * rcadm strides the LD by its own PacketSize (+0x90),
				 * not by the end of the element table.  On real
				 * firmware the two differ by a 16-byte tail; using
				 * elem_off + devices*64 places the controller
				 * section 16 bytes early, so rcadm reads zeros where
				 * the first 0x25BE record should start and reports
				 * "no controllers found".  Keep the full record.
				 */
				if (pkt_len >= rec_len &&
				    (u64)i + pkt_len <= avail)
					rec_len = pkt_len;
				if (rec_len > avail - i)
					rec_len = avail - i;
				if (rec_len > RC_API_LD_MAX) {
					rc_printk(RC_WARN,
						  "rc_volume_parse_logical_device: %s LD record is %u bytes, clamping to %u; /dev/rc_api will under-report members\n",
						  pci_name(adapter->pdev), rec_len,
						  RC_API_LD_MAX);
					rec_len = RC_API_LD_MAX;
				}
				memcpy(nvme->ld_raw, ld, rec_len);
				nvme->ld_raw_len = rec_len;
			}
			nvme->ld_device_type      = devtype;
			nvme->ld_first_count      = first_count;
			nvme->ld_second_count     = second_count;
			nvme->ld_level            = level;
			nvme->ld_devices          = devices;
			nvme->ld_chunk_sectors    = chunk;
			nvme->ld_chunk_index      = chunk_index;
			nvme->ld_capacity_sectors = capacity;
			nvme->ld_my_position      = my_pos;
			nvme->ld_alloc_offset     = alloc_off;
			nvme->ld_alloc_size       = alloc_sz;
			nvme->ld_userdata_offset  = user_off;
			nvme->ld_userdata_size    = user_sz;
			nvme->ld_valid            = true;
			goto done;
		}
	}
	rc_printk(RC_WARN,
		  "rc_volume_parse_logical_device: %s no matching LogicalDevice record in the active config generation (%u bytes @LBA 0x%llx)\n",
		  pci_name(adapter->pdev), gen_bytes,
		  (unsigned long long)gen_lba);

done:
	dma_free_coherent(dev, chunk_bytes, buf, buf_dma);
}

/* Physical LBA of logical @lba on RAID10 member @m.  Both copies of a
 * column hold the same physical extent, so the result does not depend on
 * which copy @m is; only the member's own user-data base offset differs.
 * Caller must have guaranteed @m belongs to the pair for @lba's column. */
static u64 rc_volume_phys_for_member(int m, u64 logical_lba)
{
	u32 stripe = rc_volume_stripe_sectors;
	u32 cols = rc_volume_raid10_cols;
	u64 stripe_num, phys_stripe;
	u32 off;

	stripe_num  = div_u64(logical_lba, stripe);
	off         = (u32)(logical_lba - stripe_num * stripe);
	phys_stripe = div_u64(stripe_num, cols);
	return phys_stripe * stripe + off + rc_volume_member_phys_offset[m];
}

/* Map a logical LBA to (member_index, physical LBA on that member).
 *
 * RAID0: stripe math — the stripe number picks the member, and stripes
 * owned by one member are packed contiguously on its drive.
 *
 * RAID1: identity — every member holds the full LBA space, so the mapping
 * only has to PICK a member.  Only READ paths may use this for RAID1:
 * writes and discards must touch every mirror and go through
 * rc_volume_dispatch_mirror instead.  The pick is
 * round-robin, which halves each mirror's read load.
 *
 * RAID5/6: distributed parity — the stripe picks one data member (the
 * parity position rotates), delegated to the parity maps in
 * patch_parity_math.c.  Read-only today (see register_member).
 *
 * RAID10: stripe over rc_volume_raid10_cols columns, each column a 2-way
 * mirror pair.  The stripe picks the column and the physical stripe is
 * packed within the pair; this mapping then PICKS one live copy of that
 * pair (round robin), the same read-balancing contract as RAID1.  Writes
 * and discards fan out to both copies via rc_volume_dispatch_raid10 and
 * never use this function.
 *
 * Caller must hold rc_volume_lock (or be in a single-threaded init path). */
static void rc_volume_map_lba(u64 logical_lba, int *out_member, u64 *out_phys)
{
	u32 stripe = rc_volume_stripe_sectors;
	int nmembers = rc_volume_member_count;
	u64 stripe_num;
	u32 stripe_off;
	u32 member_idx;
	u64 phys_stripe;

	if (rc_volume_raid_level == RC_LDT_RAID1) {
		/* Round-robin over the LIVE members only.  The mask read is
		 * deliberately lock-free (hot path); a member failing between
		 * this pick and submission is handled by the drain path like
		 * any other in-flight death.  mask==0 cannot reach here —
		 * queue_rq fast-fails via rc_volume_fatal first — but fall
		 * back to member 0 defensively rather than div-by-zero. */
		unsigned long mask =
			(unsigned long)(unsigned int)atomic_read(&rc_volume_live_mask);
		unsigned int nlive = hweight_long(mask);

		if (unlikely(!nlive)) {
			member_idx = 0;
		} else {
			u32 n = (u32)atomic_inc_return(&rc_volume_rr_next) %
				nlive;
			unsigned int b;

			member_idx = 0;
			for_each_set_bit(b, &mask, RC_VOLUME_MAX_MEMBERS) {
				if (!n--) {
					member_idx = b;
					break;
				}
			}
		}
		*out_member = (int)member_idx;
		*out_phys   = logical_lba +
			      rc_volume_member_phys_offset[member_idx];
		return;
	}

	if (rc_volume_raid_level == RC_LDT_RAID10) {
		u32 cols = rc_volume_raid10_cols;
		u64 snum;
		u32 col;
		int c0, c1, chosen;
		unsigned int live =
			(unsigned int)atomic_read(&rc_volume_live_mask);
		bool l0, l1;

		if (unlikely(!cols || !stripe)) {
			*out_member = 0;
			*out_phys   = logical_lba;
			return;
		}
		snum = div_u64(logical_lba, stripe);
		col  = (u32)(snum % cols);
		c0   = (int)(2u * col);
		c1   = c0 + 1;
		l0   = (live & BIT(c0)) != 0;
		l1   = (live & BIT(c1)) != 0;
		/* Prefer the round-robin copy when both are live; fall back to
		 * whichever survives so a dead copy is never read.  Both dead
		 * is caught by rc_volume_fatal() before we get here. */
		if (l0 && l1)
			chosen = (atomic_inc_return(&rc_volume_rr_next) & 1)
				 ? c1 : c0;
		else if (l0)
			chosen = c0;
		else if (l1)
			chosen = c1;
		else
			chosen = c0;
		*out_member = chosen;
		*out_phys   = rc_volume_phys_for_member(chosen, logical_lba);
		return;
	}

	/* RAID5/RAID6: distributed-parity stripe map.  The layout lives in
	 * patch_parity_math.c (rc_amd_map_distributed_raid5/6) so the driver
	 * and the userspace bijection harness share ONE implementation, and
	 * that file is the single place to change once the real vendor
	 * placement is confirmed (issue #6).  These levels never see a
	 * request that spans a stripe boundary: create_disk caps the queue
	 * at the stripe size for them, so one data member holds every sector
	 * of the request.  The volume is read-only (see register_member), so
	 * the parity member returned here is only ever informational. */
	if (rc_volume_raid_level == RC_LDT_RAID5 ||
	    rc_volume_raid_level == RC_LDT_RAID6) {
		int tgt = -1, p = -1, q = -1;
		u64 phys;

		if (unlikely(!stripe || nmembers < 3)) {
			*out_member = 0;
			*out_phys   = logical_lba;
			return;
		}
		if (rc_volume_raid_level == RC_LDT_RAID5)
			phys = rc_amd_map_distributed_raid5(logical_lba, stripe,
							    &tgt, &p, nmembers);
		else
			phys = rc_amd_map_distributed_raid6(logical_lba, stripe,
							    &tgt, &p, &q,
							    nmembers);
		if (unlikely(tgt < 0 || tgt >= nmembers)) {
			*out_member = 0;
			*out_phys   = logical_lba;
			return;
		}
		*out_member = tgt;
		*out_phys   = phys + rc_volume_member_phys_offset[tgt];
		return;
	}

	stripe_num  = div_u64(logical_lba, stripe);
	stripe_off  = (u32)(logical_lba - stripe_num * stripe);
	member_idx  = (u32)(stripe_num % (u32)nmembers);
	phys_stripe = div_u64(stripe_num, (u32)nmembers);

	*out_member = (int)member_idx;
	*out_phys   = phys_stripe * stripe + stripe_off
		    + rc_volume_member_phys_offset[member_idx];
}

/* Read one physical LBA from an already-mapped member.  The caller maps the
 * logical LBA ONCE and passes BOTH the member index and its physical LBA, so
 * the per-member DMA buffer (allocated on that member's IOMMU domain) can
 * never be handed to a different member.  Mapping twice would re-roll the
 * RAID1/RAID10 round-robin pick and mismatch the buffer. */
static int rc_volume_read_lba(int member_idx, u64 phys_lba, dma_addr_t buf_dma)
{
	struct rc_adapter *adapter;

	if (member_idx < 0 ||
	    member_idx >= rc_volume_member_count ||
	    member_idx >= RC_VOLUME_MAX_MEMBERS ||
	    !rc_volume_members[member_idx])
		return -ENODEV;

	adapter = rc_volume_members[member_idx];
	return rc_nvme_read_lba(adapter, phys_lba, 0, buf_dma);
}

/* Demonstrate volume assembly: walk a few logical LBAs that straddle a
 * stripe boundary and dump the first 16 bytes of each.  Shows the mapping
 * is wiring up physical reads to the expected member.
 *
 * Per-member DMA buffers: each member sits in its own IOMMU domain, so the
 * IOVA returned by dma_alloc_coherent against member A's pdev is invalid
 * when fed to member B's controller (the read either silently corrupts or
 * the IOMMU faults).  Allocate one buffer per member, then route by index. */
static void rc_volume_demo_reads(void)
{
	void       *bufs[RC_VOLUME_MAX_MEMBERS]     = { NULL };
	dma_addr_t  buf_dmas[RC_VOLUME_MAX_MEMBERS] = { 0 };
	int i;
	u32 stripe = rc_volume_stripe_sectors;
	u64 probes[] = {
		0,			/* stripe 0 → member 0 LBA 0 */
		stripe - 1,		/* end of first stripe → member 0 */
		stripe,			/* stripe 1 → member 1 LBA 0 */
		stripe + 1,		/* member 1 LBA 1 */
		(u64)stripe * 2,	/* stripe 2 → member 0 LBA stripe */
		(u64)stripe * 3,	/* stripe 3 → member 1 LBA stripe */
		RC_RAIDCORE_LBA,	/* RAIDCore meta LBA on the volume — note: on phys
					 * member it's still at LBA 0x5000 of that member */
	};

	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		bufs[i] = dma_alloc_coherent(dev, PAGE_SIZE, &buf_dmas[i],
					     GFP_KERNEL);
		if (!bufs[i])
			goto out;
	}

	rc_printk(RC_NOTE,
		  "rc_volume_demo_reads: volume up — %d members, stripe=%u sectors\n",
		  rc_volume_member_count, stripe);

	for (i = 0; i < (int)ARRAY_SIZE(probes); i++) {
		int member_idx;
		u64 phys_lba;
		int ret;

		rc_volume_map_lba(probes[i], &member_idx, &phys_lba);
		if (member_idx < 0 || member_idx >= rc_volume_member_count ||
		    member_idx >= RC_VOLUME_MAX_MEMBERS ||
		    !rc_volume_members[member_idx]) {
			rc_printk(RC_WARN,
				  "rc_volume_demo_reads: logical %llu has no usable member — skipped\n",
				  (unsigned long long)probes[i]);
			continue;
		}
		memset(bufs[member_idx], 0, 16);
		ret = rc_volume_read_lba(member_idx, phys_lba,
					 buf_dmas[member_idx]);
		if (ret) {
			rc_printk(RC_WARN,
				  "rc_volume_demo_reads: logical %llu read failed (%d)\n",
				  (unsigned long long)probes[i], ret);
			continue;
		}
		rc_printk(RC_NOTE,
			  "rc_volume_demo_reads: logical=%llu -> member %d phys=%llu, first 16 bytes:\n",
			  (unsigned long long)probes[i], member_idx,
			  (unsigned long long)phys_lba);
		print_hex_dump(KERN_INFO, "rcraid: ", DUMP_PREFIX_OFFSET,
			       16, 1, bufs[member_idx], 16, true);
	}

out:
	for (i = 0; i < rc_volume_member_count; i++) {
		if (bufs[i]) {
			struct device *dev = &rc_volume_members[i]->pdev->dev;
			dma_free_coherent(dev, PAGE_SIZE, bufs[i], buf_dmas[i]);
		}
	}
}

/* Volume-level expected member count.  Set from the on-disk LD's Devices
 * field the first time a member registers; subsequent members must agree.
 * Zero means "no LD seen yet". */
static u32 rc_volume_expected_members;

/* Stripe size for the assembled volume in sectors.
 *
 * The on-disk RC_LogicalDevice has TWO size fields:
 *
 *   - field @ 0xAC (RC_LD_CHUNKSIZE_OFFSET): raw sector count.  Reads
 *     back as 0 on AMD-RAID-created RAID0 volumes; used verbatim when
 *     non-0.
 *
 *   - field @ 0x110 (RC_LD_CHUNKINDEX_OFFSET): a small index that
 *     encodes the actual stripe size for RAID0.  VERIFIED against
 *     rcraid.sys 9.3.2 (see docs/REVERSE_ENGINEERING.md): FUN_1400121d0
 *     dispatches on this value (read from in-memory LD[0xC8], populated
 *     from on-disk LD[0x110] by the field copier FUN_140018444) and
 *     picks a sector count:
 *
 *         index   stripe
 *         -----   ------
 *         3       512 sectors (256 KiB)   <- our test array
 *         2       256 sectors (128 KiB)
 *         else    128 sectors (64 KiB)
 *
 *     Two subtleties confirmed by the RE, not guessed:
 *       - FUN_140018444 forces chunk_index = 1 when the on-disk field
 *         is 0, so 0 and 1 both mean 64 KiB.
 *       - Windows handles ONLY 2 and 3 explicitly; every other value
 *         (including >= 4) falls to 64 KiB.  So the "64 << index"
 *         extrapolation is NOT what Windows does — an index >= 4 is an
 *         encoding neither driver understands.  We keep the 64 KiB
 *         best-effort here so the array is still READABLE, but the
 *         caller marks such geometry untrusted so the write-veto forces
 *         read-only (a wrong stripe size would corrupt on write). */
static u32 rc_volume_chunk_sectors_for(u32 devtype, u32 ld_chunk,
				       u32 ld_chunk_index)
{
	/* RAID1 first, BEFORE the ld_chunk early return: mirrors don't
	 * stripe, so ChunkSize is not a data-layout parameter for them, and
	 * every size assumption in the mirror fan-out (per-member sg arrays,
	 * one NVMe command per member, u16 NLB) is derived from this fixed
	 * granule — an on-disk ChunkSize, whatever real RAID1 firmware
	 * metadata encodes there, must not override it.  (Confirmed on real
	 * TRX50 RAID1 metadata: the record carries chunk_index=3 even though
	 * mirrors don't stripe — it must be ignored here.)  512 sectors =
	 * 256 KiB: comfortably under MDTS (512 KiB) and the per-member sg
	 * array (RC_VOLUME_MS_PAGES_PER_MEMBER pages).
	 *
	 * @devtype is the NORMALIZED level from rc_ld_level_from(), not the
	 * raw on-disk DeviceType. */
	if (devtype == RC_LDT_RAID1)
		return 512u;

	if (ld_chunk)
		return ld_chunk;
	switch (devtype) {
	case RC_LDT_RAID0:
	case RC_LDT_RAID10:
	case RC_LDT_RAID5:
	case RC_LDT_RAID6:
		/* All of these stripe the logical space, so the on-disk chunk
		 * encoding is a data-layout parameter for each.  RAID10 reuses
		 * RAID0's chunk_index -> stripe table and the parity levels reuse
		 * the same ladder (the parity blob's chunk sizes are the RAID0
		 * ones), so they must all agree on the resolved size. */
		switch (ld_chunk_index) {
		case 3:  return 512u;
		case 2:  return 256u;
		case 1:
		default: return 128u;
		}
	default:
		return 0u;
	}
}

/* Called once per adapter after its metadata block validates.  When the LD
 * parser succeeded for this member we use the on-disk position from the
 * LogicalElement array directly.  Otherwise we fall back to a BDF-sorted
 * ordering against a hardcoded count of 2 — that path is for the
 * pre-LD-parsing legacy behavior and shouldn't normally fire. */
static void rc_volume_register_member(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	bool have_ld = nvme->ld_valid && nvme->ld_my_position >= 0;
	u32 expected;
	u32 chunk_sectors;
	int pos;
	int i;

	mutex_lock(&rc_volume_lock);
	if (rc_volume_member_count >= RC_VOLUME_MAX_MEMBERS) {
		rc_printk(RC_WARN,
			  "rc_volume_register_member: too many members, ignoring\n");
		goto out;
	}

	if (have_ld) {
		/* Refuse layouts with no mapping path (>2-way mirrors,
		 * FirstCount x SecondCount != Devices, and any level the
		 * classifier did not normalize).  RAID5/6 IS accepted here —
		 * it has a read mapping — but is forced read-only below.
		 * Guessing a mapping for anything else turns every read into
		 * garbage and every write into corruption, so the member is
		 * not registered at all and the volume never assembles. */
		if (nvme->ld_level != RC_LDT_RAID0 &&
		    nvme->ld_level != RC_LDT_RAID1 &&
		    nvme->ld_level != RC_LDT_RAID10 &&
		    nvme->ld_level != RC_LDT_RAID5 &&
		    nvme->ld_level != RC_LDT_RAID6) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s unsupported RAID layout (DeviceType=0x%x FirstCount=%u SecondCount=%u Devices=%u) — ignoring member\n",
				  pci_name(adapter->pdev), nvme->ld_device_type,
				  nvme->ld_first_count, nvme->ld_second_count,
				  nvme->ld_devices);
			goto out;
		}
		/* Cross-check: total capacity must equal the per-member
		 * user-data size times the DATA column count (mirrors don't add
		 * capacity; a parity level loses one — RAID5 — or two — RAID6 —
		 * members' worth to parity).  For RAID0/RAID10 the on-disk
		 * FirstCount is that count; for parity it is derived from the
		 * member count, because FirstCount's meaning in a parity record
		 * is not confirmed (issue #3).  A mismatch means we misread the
		 * geometry somewhere — keep the volume readable but veto
		 * writes.  Runs here rather than in the parser so the write to
		 * the lock-protected untrust reason happens under
		 * rc_volume_lock like every other writer. */
		{
			u32 data_cols =
				nvme->ld_level == RC_LDT_RAID5 ?
					nvme->ld_devices - 1u :
				nvme->ld_level == RC_LDT_RAID6 ?
					nvme->ld_devices - 2u :
					nvme->ld_first_count;

			if (nvme->ld_capacity_sectors !=
			    nvme->ld_userdata_size * data_cols) {
				rc_printk(RC_WARN,
					  "rc_volume_register_member: %s capacity %llu != user_sz %llu x %s data columns %u — geometry UNTRUSTED, writes will be vetoed\n",
					  pci_name(adapter->pdev),
					  (unsigned long long)nvme->ld_capacity_sectors,
					  (unsigned long long)nvme->ld_userdata_size,
					  rc_level_name(nvme->ld_level), data_cols);
				rc_volume_geometry_untrust_reason =
					"the LD record's total capacity doesn't equal per-member user size x data columns — the on-disk geometry doesn't add up, so some field is being misread";
			}
		}
		expected = nvme->ld_devices;
		chunk_sectors = rc_volume_chunk_sectors_for(nvme->ld_level,
							    nvme->ld_chunk_sectors,
							    nvme->ld_chunk_index);
		if (!chunk_sectors) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s unknown DeviceType=0x%x with ChunkSize=0 — ignoring\n",
				  pci_name(adapter->pdev), nvme->ld_device_type);
			goto out;
		}
		/* Fail closed on an unrecognized RAID0 stripe encoding.  When
		 * ChunkSize is 0 the stripe comes from chunk_index, and the
		 * reference parser (rcraid.sys 9.3.2 FUN_1400121d0) only maps
		 * 2 and 3; anything >= 4 falls to a 64 KiB guess that would be
		 * WRONG for a real 512 KiB / 1 MiB stripe and corrupt the array
		 * on write.  Keep 64 KiB so reads still work, but distrust the
		 * geometry so create_disk forces read-only. */
		if ((nvme->ld_level == RC_LDT_RAID0 ||
		     nvme->ld_level == RC_LDT_RAID10) &&
		    nvme->ld_chunk_sectors == 0 && nvme->ld_chunk_index > 3) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s RAID%d chunk_index=%u not understood (only 0..3 map to a known stripe) — geometry UNTRUSTED, writes will be vetoed\n",
				  pci_name(adapter->pdev),
				  nvme->ld_level == RC_LDT_RAID10 ? 10 : 0,
				  nvme->ld_chunk_index);
			rc_volume_geometry_untrust_reason =
				"the on-disk RAID0/RAID10 chunk_index is not one this driver maps to a stripe size (see the earlier chunk_index warning) — an on-disk encoding this driver doesn't recognize, not a parse failure";
		}
		/* Sanity-bound a verbatim on-disk ChunkSize.  Every dispatch
		 * assumption (per-member sg arrays, one NVMe cmd per member,
		 * the 1 MiB max request) is derived from sane power-of-two
		 * stripes; an implausible value means we're misreading the
		 * field, so keep the array readable but veto writes. */
		if ((nvme->ld_level == RC_LDT_RAID0 ||
		     nvme->ld_level == RC_LDT_RAID10) &&
		    nvme->ld_chunk_sectors &&
		    (chunk_sectors < 16u ||
		     chunk_sectors > (RC_VOLUME_DATA_BYTES / 512u) ||
		     (chunk_sectors & (chunk_sectors - 1)))) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s implausible on-disk ChunkSize=%u sectors (want a power of two in [16, %lu]) — geometry UNTRUSTED, writes will be vetoed\n",
				  pci_name(adapter->pdev), chunk_sectors,
				  RC_VOLUME_DATA_BYTES / 512u);
			rc_volume_geometry_untrust_reason =
				"the on-disk ChunkSize is not a plausible stripe size (power of two, 8 KiB..1 MiB) — the field is probably being misread, so the write path cannot trust the stripe math";
		}
		/* RAID 5/6 write policy.  RAID 6 has no write path at all (the
		 * dual P/Q write is not wired), so it stays read-only.  RAID 5
		 * writes are opt-in (parity_writes=1) and cover both shapes: a
		 * complete row carries every data chunk, so P is a plain XOR of
		 * the request; a partial row is read-modify-written — read the
		 * row, overlay the request, regenerate P, rewrite the touched
		 * chunks.  create_disk makes the final read-only call: it also
		 * requires enable_writes=1, trusted geometry and a row that fits
		 * one request. */
		if (nvme->ld_level == RC_LDT_RAID6) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s RAID 6 recognised — assembly and READS only (dual-parity writes are not implemented) — volume will be READ-ONLY\n",
				  pci_name(adapter->pdev));
			rc_volume_geometry_untrust_reason =
				"RAID 6 is assembled READ-ONLY: the dual-parity (P/Q) write path is not implemented";
		} else if (nvme->ld_level == RC_LDT_RAID5) {
			rc_printk(RC_NOTE,
				  "rc_volume_register_member: %s RAID 5 recognised — reads always; writes opt-in with enable_writes=1 and parity_writes=1 (full rows generate P, partial rows read-modify-write the row under the parity lock)\n",
				  pci_name(adapter->pdev));
		}
	} else {
		/* A committed config generation with an LD record that does
		 * not name this disk means this disk is not a member of the
		 * active array — a spare, or a drive whose old metadata
		 * survived a re-create.  Refuse it BEFORE the first-member
		 * seeding below: otherwise a stray disk that probes first
		 * would seed expected=2 / RAID0 and the real members would
		 * then be rejected as a mismatch.  The legacy fallback is for
		 * metadata old enough that no LD record was written at all,
		 * where there is nothing better to go on. */
		if (nvme->ld_record_seen) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s has valid RAIDCore metadata but no LD record names this device — not a member of the active array, refusing legacy fallback\n",
				  pci_name(adapter->pdev));
			goto out;
		}

		expected = RC_VOLUME_EXPECTED_MEMBERS;
		chunk_sectors = nvme->md_stripe_sectors;
		rc_printk(RC_WARN,
			  "rc_volume_register_member: %s no LD parsed (ld_valid=%d pos=%d) — falling back to legacy BDF ordering and stripe=%u\n",
			  pci_name(adapter->pdev), nvme->ld_valid,
			  nvme->ld_my_position, chunk_sectors);
	}

	if (rc_volume_stripe_override) {
		rc_printk(RC_WARN,
			  "rc_volume_register_member: %s OVERRIDE stripe %u -> %u sectors (insmod stripe_sectors_override)\n",
			  pci_name(adapter->pdev), chunk_sectors,
			  rc_volume_stripe_override);
		chunk_sectors = rc_volume_stripe_override;
	}

	/* First member: seed expected count + stripe + RAID level (the
	 * NORMALIZED level from rc_ld_level_from — the raw on-disk DeviceType
	 * is 0x1BF6 for both RAID0 and RAID1).  Later members must match. */
	if (rc_volume_member_count == 0) {
		rc_volume_expected_members = expected;
		rc_volume_stripe_sectors   = chunk_sectors;
		rc_volume_raid_level       = have_ld ? nvme->ld_level
						     : RC_LDT_RAID0;
		/* RAID10 is striped over 2-way mirror pairs, so the number of
		 * columns is half the member count (= FirstCount). */
		rc_volume_raid10_cols      =
			rc_volume_raid_level == RC_LDT_RAID10 ? expected / 2u
							      : 0u;
	} else {
		if ((have_ld ? nvme->ld_level : RC_LDT_RAID0) !=
		    rc_volume_raid_level) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s RAID-level mismatch 0x%x vs 0x%x — ignoring\n",
				  pci_name(adapter->pdev),
				  have_ld ? nvme->ld_level : RC_LDT_RAID0,
				  rc_volume_raid_level);
			goto out;
		}
		if (expected != rc_volume_expected_members) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s expected-member mismatch %u vs %u — ignoring\n",
				  pci_name(adapter->pdev),
				  expected, rc_volume_expected_members);
			goto out;
		}
		if (chunk_sectors != rc_volume_stripe_sectors) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s stripe-size mismatch %u vs %u — ignoring\n",
				  pci_name(adapter->pdev),
				  chunk_sectors, rc_volume_stripe_sectors);
			goto out;
		}
	}

	/* Registration against a LIVE volume is a member RE-ADD.  The slot
	 * can be empty here only because that member failed or was
	 * hot-removed while the volume kept serving degraded — its data
	 * missed every write since, so it must NOT go straight into
	 * dispatch (a rejoined stale mirror serves old blocks to half the
	 * reads).  Install it parked in NEEDS_RESYNC: the pointer, slot,
	 * and DMA pool come back, but no live bit — only a completed
	 * resync flips it. */
	if (rc_volume_disk) {
		u64 member_cap = get_capacity(rc_volume_disk);

		/* RAID10's volume capacity is stretched over rc_volume_raid10_cols
		 * columns (mirrors add none), so the per-member user-data size
		 * must be compared against the volume size DIVIDED by the column
		 * count, not the whole volume.  Comparing against the whole
		 * volume would reject every RAID10 re-add up front. */
		if (rc_volume_raid_level == RC_LDT_RAID10 &&
		    rc_volume_raid10_cols)
			member_cap = div_u64(member_cap, rc_volume_raid10_cols);

		if (!have_ld) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s probed while /dev/rcraid0 is live but has no parseable LD — not re-admitting via the legacy fallback\n",
				  pci_name(adapter->pdev));
			goto out;
		}
		if (rc_volume_raid_level != RC_LDT_RAID1 &&
		    rc_volume_raid_level != RC_LDT_RAID10) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s probed while a %s volume is live — re-add is a mirror concept, ignoring\n",
				  pci_name(adapter->pdev),
				  rc_volume_raid_level == RC_LDT_RAID0 ?
					"RAID0" : "non-mirror");
			goto out;
		}
		pos = nvme->ld_my_position;
		if (pos < 0 || pos >= (int)expected ||
		    pos >= RC_VOLUME_MAX_MEMBERS) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s re-add LD position %d out of range [0,%u) — ignoring\n",
				  pci_name(adapter->pdev), pos, expected);
			goto out;
		}
		if (rc_volume_members[pos]) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s re-add position %d still occupied by %s — ignoring\n",
				  pci_name(adapter->pdev), pos,
				  pci_name(rc_volume_members[pos]->pdev));
			goto out;
		}
		if (nvme->nr_io_queues < rc_volume_nr_hw) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s re-add rejected — grants %u I/O queues, volume tagset needs %u\n",
				  pci_name(adapter->pdev),
				  nvme->nr_io_queues, rc_volume_nr_hw);
			goto out;
		}
		/* A smaller replacement drive would pass every check above
		 * and only fail deep into the resync copy, when a chunk
		 * write lands past its LBA range — reject the capacity
		 * mismatch up front with a diagnosable message instead. */
		if (nvme->ld_userdata_size < member_cap) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s re-add rejected — capacity mismatch: member user-data %llu sectors < required %llu sectors\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)nvme->ld_userdata_size,
				  (unsigned long long)member_cap);
			goto out;
		}
		/* Install the pointer BEFORE the DMA alloc — the helper
		 * resolves the device through the registry slot.  Not yet
		 * visible to dispatch: the live bit stays clear throughout. */
		rc_volume_members[pos] = adapter;
		if (rc_volume_alloc_member_dma(pos, rc_volume_nr_hw)) {
			rc_volume_free_member_dma(pos, &adapter->pdev->dev);
			rc_volume_members[pos] = NULL;
			rc_printk(RC_ERROR,
				  "rc_volume_register_member: %s re-add DMA pool allocation failed — ignoring\n",
				  pci_name(adapter->pdev));
			goto out;
		}
		rc_volume_member_phys_offset[pos] = nvme->ld_userdata_offset;
		nvme->volume_slot = pos;
		rc_volume_member_set_state(pos, RC_MEMBER_NEEDS_RESYNC);
		rc_printk(RC_NOTE,
			  "rc_volume_register_member: %s RE-ADMITTED at pos %d — parked needs-resync (stale until resynced), volume stays degraded\n",
			  pci_name(adapter->pdev), pos);
		rc_volume_member_readmitted(pos);
		goto out;
	}

	if (have_ld) {
		pos = nvme->ld_my_position;
		if (pos < 0 || pos >= (int)expected) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s LD position %d out of range [0,%u) — ignoring\n",
				  pci_name(adapter->pdev), pos, expected);
			goto out;
		}
		if (rc_volume_members[pos]) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s LD position %d already taken by %s — ignoring\n",
				  pci_name(adapter->pdev), pos,
				  pci_name(rc_volume_members[pos]->pdev));
			goto out;
		}
		rc_volume_members[pos] = adapter;
		rc_volume_member_phys_offset[pos] = nvme->ld_userdata_offset;
		rc_volume_member_count++;
		nvme->volume_slot = pos;
		rc_volume_member_set_state(pos, RC_MEMBER_LIVE);
	} else {
		/* Legacy BDF-sorted insertion.  The member order and phys
		 * offsets here are a guess (no on-disk LD to read them from),
		 * so the assembled geometry can't be trusted for writes. */
		rc_volume_geometry_untrust_reason =
			"a member was assembled via the legacy BDF fallback, not the on-disk LD, so member order/offsets are inferred — restore LD parsing (or set stripe_sectors_override and verify reads) before enabling writes";
		for (pos = 0; pos < rc_volume_member_count; pos++) {
			if (rc_volume_bdf_cmp(adapter, rc_volume_members[pos]) < 0)
				break;
		}
		memmove(&rc_volume_members[pos + 1], &rc_volume_members[pos],
			(rc_volume_member_count - pos) *
			sizeof(rc_volume_members[0]));
		memmove(&rc_volume_member_phys_offset[pos + 1],
			&rc_volume_member_phys_offset[pos],
			(rc_volume_member_count - pos) *
			sizeof(rc_volume_member_phys_offset[0]));
		rc_volume_members[pos] = adapter;
		rc_volume_member_phys_offset[pos] = 0;
		rc_volume_member_count++;
		/* The insertion shifted slots — rebuild every member's cached
		 * slot index and (re)assert LIVE state for all of them.  Only
		 * reachable pre-disk (assembly time), so no I/O is in flight
		 * against the old indices. */
		for (i = 0; i < rc_volume_member_count; i++) {
			rc_volume_members[i]->ctx.nvme.volume_slot = i;
			rc_volume_member_set_state(i, RC_MEMBER_LIVE);
		}
	}

	rc_printk(RC_INFO,
		  "rc_volume_register_member: %s registered at pos %d (%d/%u) phys_offset=%llu sectors\n",
		  pci_name(adapter->pdev), pos,
		  rc_volume_member_count, expected,
		  (unsigned long long)rc_volume_member_phys_offset[pos]);

	if ((u32)rc_volume_member_count == expected) {
		/* All slots populated? */
		for (i = 0; i < (int)expected; i++) {
			if (!rc_volume_members[i]) {
				rc_printk(RC_WARN,
					  "rc_volume_register_member: count reached %u but slot %d empty — not creating disk\n",
					  expected, i);
				goto out;
			}
		}
		rc_volume_demo_reads();
		{
			int rc = rc_volume_create_disk();
			if (rc)
				rc_printk(RC_WARN,
					  "rc_volume_register_member: create_disk failed (%d)\n",
					  rc);
		}
	}

	/* Degraded assembly (opt-in): (re)arm the fallback timer on every
	 * registration while the volume is still incomplete.  If no further
	 * member shows up before it fires, the worker assembles degraded. */
	if (!rc_volume_disk && rc_volume_allow_degraded &&
	    rc_volume_raid_level == RC_LDT_RAID1)
		mod_delayed_work(system_wq, &rc_volume_assemble_work,
				 msecs_to_jiffies(RC_VOLUME_ASSEMBLE_DELAY_MS));
out:
	mutex_unlock(&rc_volume_lock);
}

/* DMA direction for the data phase of a READ/WRITE request. */
static inline enum dma_data_direction rc_volume_dma_dir(u8 op)
{
	return (op == REQ_OP_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
}

/* --- Bounce fallback for layouts NVMe PRPs cannot encode --------------
 *
 * The fast paths build scatterlists whose entries after the first start
 * on page boundaries.  A request can still arrive with a mid-page
 * fragment in a later entry: XFS log recovery writes a 256 KiB buffer
 * whose per-member fragments start at arbitrary in-page offsets, and a
 * request can legally join bios with a page gap.  PRP lists cannot
 * express those, so such a request is copied through a bounce buffer
 * whose runs are laid out per member in member-LBA order.  Each run's
 * SG list has at most one mid-page start (PRP1), which is expressible.
 *
 * queue_rq is non-blocking for RAID0/RAID1/RAID10 (only parity levels set
 * BLK_MQ_F_BLOCKING), so the buffers come from a small preallocated
 * mempool and the copy uses kmap_local only.  A request that cannot get a
 * buffer is retried as BLK_STS_RESOURCE rather than failed. */
#define RC_VOLUME_BOUNCE_BUFS	8

static mempool_t *rc_volume_bounce_pool;

static int rc_volume_bounce_pool_init(void)
{
	rc_volume_bounce_pool = mempool_create_kmalloc_pool(RC_VOLUME_BOUNCE_BUFS,
							   RC_VOLUME_DATA_BYTES);
	return rc_volume_bounce_pool ? 0 : -ENOMEM;
}

static void rc_volume_bounce_pool_free(void)
{
	if (!rc_volume_bounce_pool)
		return;
	mempool_destroy(rc_volume_bounce_pool);
	rc_volume_bounce_pool = NULL;
}

static void *rc_volume_bounce_alloc(void)
{
	if (!rc_volume_bounce_pool)
		return NULL;
	return mempool_alloc(rc_volume_bounce_pool, GFP_ATOMIC);
}

static void rc_volume_bounce_free(void *buf)
{
	if (buf && rc_volume_bounce_pool)
		mempool_free(buf, rc_volume_bounce_pool);
}

/* True when every SG entry after the first starts on a page boundary. */
static bool rc_volume_sg_prp_ready(const struct scatterlist *sgl, u32 used)
{
	u32 i;

	for (i = 1; i < used; i++)
		if (sgl[i].offset)
			return false;
	return true;
}

/* Resolve the page backing a byte in a bounce buffer.  mempool buffers
 * come from kmalloc, but keep the vmalloc case correct for future users. */
static struct page *rc_volume_bounce_page(void *addr)
{
	return is_vmalloc_addr(addr) ? vmalloc_to_page(addr) : virt_to_page(addr);
}

/* Build an SG list over an arbitrary run of a bounce buffer.  Only the
 * first entry may carry an in-page offset, so the result is always
 * PRP-expressible.  Returns 0 or -EINVAL when the array is too small. */
static int rc_volume_bounce_sg(struct scatterlist *sgl, u32 max,
			       void *buf, u32 len, u32 *used)
{
	u32 off = offset_in_page(buf);
	u32 done = 0;
	u32 n = 0;

	if (!len)
		return -EINVAL;
	sg_init_table(sgl, max);
	while (done < len) {
		u32 chunk = min_t(u32, len - done, PAGE_SIZE - off);
		struct page *page;

		if (n >= max)
			return -EINVAL;
		page = rc_volume_bounce_page(buf + done);
		if (!page)
			return -EINVAL;
		sg_set_page(&sgl[n++], page, chunk, off);
		done += chunk;
		off = 0;
	}
	sg_mark_end(&sgl[n - 1]);
	*used = n;
	return 0;
}

/* Copy the whole request to/from a linear buffer in request order. */
static void rc_volume_bounce_copy(struct request *req, void *buf, bool to_buf)
{
	struct req_iterator iter;
	struct bio_vec bv;
	u8 *p = buf;

	rq_for_each_segment(bv, req, iter) {
		u8 *v = kmap_local_page(bv.bv_page);

		if (to_buf)
			memcpy(p, v + bv.bv_offset, bv.bv_len);
		else
			memcpy(v + bv.bv_offset, p, bv.bv_len);
		kunmap_local(v);
		p += bv.bv_len;
	}
}

/* Copy the request to/from per-member slices of a multi-stripe bounce
 * buffer.  The chunk geometry is the same walk the dispatcher uses, so
 * slice[m] + done[m] always names the same byte on the member. */
static int rc_volume_ms_bounce_copy(struct request *req, void *buf,
				    const u32 *slice, const u32 *slice_len,
				    u32 slots, bool to_buf)
{
	const u32 stripe = rc_volume_stripe_sectors;
	sector_t cur_lba = blk_rq_pos(req);
	struct req_iterator iter;
	struct bio_vec bv;
	u32 done[RC_VOLUME_MAX_MEMBERS] = {0};
	int m;

	if (!stripe || !slots || slots > RC_VOLUME_MAX_MEMBERS)
		return -EINVAL;

	rq_for_each_segment(bv, req, iter) {
		u32 seg_len_sectors = bv.bv_len / 512;
		u32 seg_remaining = bv.bv_len;
		u32 seg_off = bv.bv_offset;
		struct page *page = bv.bv_page;

		while (seg_remaining) {
			sector_t stripe_idx = cur_lba / stripe;
			u32 to_boundary = (u32)((stripe_idx + 1) * stripe -
						cur_lba);
			u32 chunk_sectors = min(seg_len_sectors, to_boundary);
			u32 chunk_bytes = chunk_sectors * 512u;
			int mbr = (int)(stripe_idx % (u64)slots);
			u8 *v;

			if (!chunk_bytes || chunk_bytes > seg_remaining ||
			    done[mbr] + chunk_bytes > slice_len[mbr])
				return -EINVAL;
			v = kmap_local_page(page);
			if (to_buf)
				memcpy(buf + slice[mbr] + done[mbr],
				       v + seg_off, chunk_bytes);
			else
				memcpy(v + seg_off,
				       buf + slice[mbr] + done[mbr],
				       chunk_bytes);
			kunmap_local(v);
			done[mbr] += chunk_bytes;
			seg_off += chunk_bytes;
			seg_remaining -= chunk_bytes;
			seg_len_sectors -= chunk_sectors;
			cur_lba += chunk_sectors;
		}
	}

	for (m = 0; m < (int)slots; m++)
		if (done[m] != slice_len[m])
			return -EINVAL;
	return 0;
}

/* Map the request's bvecs into a scatterlist DMA-mapped on the target
 * member's pdev.  Stashes nents in the pdu so .complete (and the
 * timeout safety-net) can unmap.  Returns 0 on success or a blk_status_t
 * suitable for ending the request. */
static blk_status_t rc_volume_map_request_sg(struct request *req,
					      struct rc_volume_pdu *pdu,
					      int member_idx)
{
	struct device *dma_dev = &rc_volume_members[member_idx]->pdev->dev;
	int input_nents, mapped_nents;

	sg_init_table(pdu->sg, RC_VOLUME_DATA_PAGES);
	input_nents = blk_rq_map_sg(req, pdu->sg);
	if (input_nents <= 0)
		return BLK_STS_IOERR;
	if (input_nents > RC_VOLUME_DATA_PAGES) {
		/* virt_boundary + max_segments should make this impossible;
		 * keep it as a defence-in-depth check rather than trusting
		 * blk-mq's accounting silently. */
		rc_printk(RC_ERROR,
			  "rc_volume_map_request_sg: %d input segments > %d max — request rejected\n",
			  input_nents, RC_VOLUME_DATA_PAGES);
		return BLK_STS_IOERR;
	}

	if (!rc_volume_sg_prp_ready(pdu->sg, input_nents)) {
		u32 bytes = blk_rq_bytes(req);
		u32 used;

		if (!bytes || bytes > RC_VOLUME_DATA_BYTES)
			return BLK_STS_IOERR;
		pdu->bounce = rc_volume_bounce_alloc();
		if (!pdu->bounce) {
			printk_ratelimited(KERN_ERR
				"rcraid: %s: no bounce buffer for a %u-byte non-PRP request — retrying\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?",
				bytes);
			return BLK_STS_RESOURCE;
		}
		if (pdu->op == REQ_OP_WRITE)
			rc_volume_bounce_copy(req, pdu->bounce, true);
		if (rc_volume_bounce_sg(pdu->sg, RC_VOLUME_DATA_PAGES,
					pdu->bounce, bytes, &used)) {
			rc_volume_bounce_free(pdu->bounce);
			pdu->bounce = NULL;
			return BLK_STS_IOERR;
		}
		input_nents = (int)used;
	}

	mapped_nents = dma_map_sg(dma_dev, pdu->sg, input_nents,
				  rc_volume_dma_dir(pdu->op));
	if (mapped_nents == 0) {
		rc_volume_bounce_free(pdu->bounce);
		pdu->bounce = NULL;
		return BLK_STS_IOERR;
	}

	pdu->nents = mapped_nents;
	return BLK_STS_OK;
}

/* Idempotent dma_unmap_sg.  Safe to call when no mapping is outstanding
 * (nents=0 — true for FLUSH, DISCARD, or early-error paths).  Handles
 * both single-member (pdu->sg + pdu->nents) and multi-stripe
 * (pdu->ms_sg[m] + pdu->ms_nents[m]) paths. */
static void rc_volume_unmap_request_sg(struct rc_volume_pdu *pdu)
{
	struct device *dma_dev;
	int m;

	/* Every completion path funnels through here exactly once (the
	 * completion claim guarantees a single completer), so this is the
	 * one place the write-generation count comes back down. */
	if (pdu->wgen_counted) {
		pdu->wgen_counted = false;
		atomic_dec(&rc_volume_wgen_count[pdu->wgen & 1]);
	}

	/* Return the parity buffer to the pool.  Idempotent: queue_rq sets
	 * parity_buf to -1, the parity dispatch acquires it, and the first
	 * completion (or an error unwind before submission) releases it. */
	if (pdu->parity_buf >= 0) {
		int slot = pdu->parity_buf;

		pdu->parity_buf = -1;
		if (slot < RC_VOLUME_PARITY_BUFS)
			clear_bit(slot, &rc_volume_parity_inuse);
	}
	/* Released from the completion path (ISR for the async full-row
	 * write), never from queue_rq once the request is in flight. */
	if (pdu->parity_locked) {
		pdu->parity_locked = false;
		up(&rc_volume_parity_lock);
	}

	if (pdu->ms_active) {
		for (m = 0; m < rc_volume_member_count &&
			    m < RC_VOLUME_MAX_MEMBERS; m++) {
			if (!pdu->ms_nents[m])
				continue;
			/* NULL slot = the member was hot-removed while this
			 * request was in flight (remove_member freezes the
			 * queue before NULLing, so this is belt-and-braces).
			 * Its IOMMU domain is being torn down with the
			 * device — skip the unmap rather than deref NULL. */
			if (!rc_volume_members[m]) {
				pdu->ms_nents[m] = 0;
				continue;
			}
			dma_dev = &rc_volume_members[m]->pdev->dev;
			dma_unmap_sg(dma_dev, pdu->ms_sg[m], pdu->ms_nents[m],
				     rc_volume_dma_dir(pdu->op));
			pdu->ms_nents[m] = 0;
		}
		/* Copy a bounced READ back only after a real completion: the
		 * buffer is uninitialised until the controller wrote it, and a
		 * pre-submit error must not leak those pages into the bio. */
		if (pdu->ms_bounce) {
			if (pdu->io_started && pdu->op == REQ_OP_READ &&
			    !pdu->sc_sct &&
			    !atomic_read(&pdu->members_pending) &&
				rc_volume_ms_bounce_copy(
					blk_mq_rq_from_pdu(pdu), pdu->ms_bounce,
					pdu->ms_bounce_off, pdu->ms_bounce_len,
					pdu->ms_bounce_slots, false))
				rc_printk(RC_ERROR,
					  "rc_volume_unmap_request_sg: bounced READ copy-back failed (pos=%llu len=%u) — the bio keeps its old contents; this is a layout bug, not a device error\n",
					  (unsigned long long)blk_rq_pos(
						  blk_mq_rq_from_pdu(pdu)),
					  blk_rq_bytes(blk_mq_rq_from_pdu(pdu)));
			rc_volume_bounce_free(pdu->ms_bounce);
			pdu->ms_bounce = NULL;
		}
		pdu->ms_active = false;
		return;
	}

	if (!pdu->nents) {
		rc_volume_bounce_free(pdu->bounce);
		pdu->bounce = NULL;
		return;
	}
	if (pdu->member_idx < 0 || pdu->member_idx >= rc_volume_member_count ||
	    !rc_volume_members[pdu->member_idx]) {
		pdu->nents = 0;
		rc_volume_bounce_free(pdu->bounce);
		pdu->bounce = NULL;
		return;
	}
	dma_dev = &rc_volume_members[pdu->member_idx]->pdev->dev;
	dma_unmap_sg(dma_dev, pdu->sg, pdu->nents, rc_volume_dma_dir(pdu->op));
	pdu->nents = 0;
	if (pdu->bounce) {
		if (pdu->io_started && pdu->op == REQ_OP_READ && !pdu->sc_sct &&
		    !atomic_read(&pdu->members_pending))
			rc_volume_bounce_copy(blk_mq_rq_from_pdu(pdu), pdu->bounce,
					      false);
		rc_volume_bounce_free(pdu->bounce);
		pdu->bounce = NULL;
	}
}

/* Enumerate DMA-mapped scatterlist into NVMe PRP1 / PRP2 / PRP list.
 *
 * Rules:
 *   - PRP1 may have an in-page offset.
 *   - PRP2 onwards must point at the start of a page.  blk-mq's
 *     virt_boundary_mask=PAGE_SIZE-1 enforces page-aligned segment starts
 *     after the first, so a single scatterlist entry can span multiple
 *     pages only when dma_map_sg coalesces them — in which case
 *     successive page-aligned addresses inside that entry are still
 *     valid PRP entries.
 *
 * The page-walk handles both cases: for the first sg entry, peel off
 * the partial first page into PRP1 then walk page-aligned chunks; for
 * subsequent entries, every PAGE_SIZE chunk is a fresh PRP entry.
 *
 * For >2 pages, PRP2 is replaced with the address of the per-tag PRP
 * list buffer (filled in along the way).  Two-page transfers use PRP2
 * directly.  Single-page transfers leave PRP2 zero.
 *
 * Returns 0, or -EINVAL for a scatterlist PRPs cannot express (a
 * non-first segment starting at an in-page offset).  The sg builders
 * merge contiguous fragments precisely so this can't happen — the check
 * turns any future regression into one clean I/O error instead of the
 * controller silently rejecting every command with SC 0x13. */
static int rc_volume_build_prp(struct rc_nvme_sqe *cmd, unsigned int hctx_idx,
			       int member_idx, u32 tag,
			       struct scatterlist *sgl, int nents)
{
	__le64 *prp_list      = rc_volume_prp_va[hctx_idx][member_idx][tag];
	dma_addr_t prp_list_pa = rc_volume_prp_pa[hctx_idx][member_idx][tag];
	struct scatterlist *sg;
	unsigned int n_pages = 0;
	int i;

	cmd->prp1 = 0;
	cmd->prp2 = 0;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);

		if (i > 0 && offset_in_page(addr)) {
			rc_printk(RC_ERROR,
				  "rc_volume_build_prp: sg entry %d starts at in-page offset %lu — unexpressible as PRPs, rejecting\n",
				  i, (unsigned long)offset_in_page(addr));
			return -EINVAL;
		}

		if (i == 0) {
			unsigned int off = offset_in_page(addr);
			unsigned int first = min_t(unsigned int,
						   len, PAGE_SIZE - off);

			cmd->prp1 = cpu_to_le64(addr);
			n_pages++;
			addr += first;
			len  -= first;
			/* addr is now page-aligned (either the next page
			 * after the offset-adjusted first page, or right at
			 * len==0 if the transfer fit in the first page). */
		}

		while (len) {
			unsigned int chunk = min_t(unsigned int, len, PAGE_SIZE);

			if (n_pages == 1)
				cmd->prp2 = cpu_to_le64(addr);
			prp_list[n_pages - 1] = cpu_to_le64(addr);

			n_pages++;
			addr += PAGE_SIZE;
			len  -= chunk;
		}
	}

	if (n_pages > 2)
		cmd->prp2 = cpu_to_le64(prp_list_pa);
	return 0;
}

/* Build an NVMe READ/WRITE SQE.  CID = tag.  FUA passed through when the
 * request has REQ_FUA set.  Caller has already dma_map_sg'd the bvecs
 * into pdu->sg / pdu->nents; rc_volume_build_prp enumerates those pages
 * into PRP1/PRP2 (and the per-tag PRP list if > 2 pages).  Returns 0 or
 * -EINVAL from the PRP expressibility check — the caller must fail the
 * request instead of submitting. */
static int rc_volume_build_io_sqe(struct rc_nvme_sqe *cmd, unsigned int hctx_idx,
				  int member_idx,
				  u32 tag, u8 opc, u64 slba, u16 nlb_zbased,
				  bool fua, struct scatterlist *sgl, int nents)
{
	u32 cdw12 = nlb_zbased;

	if (fua)
		cdw12 |= (1u << 30);	/* CDW12.FUA */

	memset(cmd, 0, sizeof(*cmd));
	cmd->opc   = opc;
	cmd->cid   = cpu_to_le16((u16)tag);
	cmd->nsid  = cpu_to_le32(1);
	cmd->cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd->cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd->cdw12 = cpu_to_le32(cdw12);

	return rc_volume_build_prp(cmd, hctx_idx, member_idx, tag, sgl, nents);
}

/* Build an NVMe FLUSH SQE for one member.  No data, no PRPs, just opcode +
 * NSID + CID.  Fanned out to all members for RAID0 — the request only
 * completes when every member has acknowledged its flush. */
static void rc_volume_build_flush_sqe(struct rc_nvme_sqe *cmd, u32 tag)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->opc  = RC_NVME_NVM_OP_FLUSH;
	cmd->cid  = cpu_to_le16((u16)tag);
	cmd->nsid = cpu_to_le32(1);
}

/* Build an NVMe DSM Deallocate SQE for one member.  Writes one DSM range
 * entry into the per-tag PRP buffer (reused — same buffer that holds the
 * PRP list for >2-page transfers, but for DSM there's no data and no PRP
 * list, so the buffer is free for the range list).  Range count = 1 (the
 * blk-mq split at chunk_sectors keeps each discard request to one stripe,
 * which maps to one member). */
static void rc_volume_build_discard_sqe(struct rc_nvme_sqe *cmd, unsigned int hctx_idx,
					int member_idx,
					u32 tag, u64 slba, u32 nlb)
{
	struct rc_nvme_dsm_range *range = (struct rc_nvme_dsm_range *)
					   rc_volume_prp_va[hctx_idx][member_idx][tag];

	range->context_attrs = cpu_to_le32(0);
	range->nlb           = cpu_to_le32(nlb);
	range->slba          = cpu_to_le64(slba);

	memset(cmd, 0, sizeof(*cmd));
	cmd->opc   = RC_NVME_NVM_OP_DSM;
	cmd->cid   = cpu_to_le16((u16)tag);
	cmd->nsid  = cpu_to_le32(1);
	cmd->prp1  = cpu_to_le64(rc_volume_prp_pa[hctx_idx][member_idx][tag]);
	cmd->cdw10 = cpu_to_le32(0);              /* NR = 0 means 1 range */
	cmd->cdw11 = cpu_to_le32(RC_NVME_DSM_AD); /* Deallocate */
}

/* Hot-path availability check, called from rc_volume_queue_rq.
 *
 * First folds any freshly-set `dead` flags into the member state machine
 * (the flag writers — ISR CSTS check, timeout, PM, reset — don't know
 * about volume state; this is the single sync point).  Then:
 *
 *   RAID0: any non-live member makes the volume unusable — every stripe
 *          crosses every member.
 *   RAID1: the volume is fatal only when NO live member remains; a
 *          surviving mirror keeps serving (degraded mode).
 *   RAID10: fatal only when SOME mirror pair has lost both copies; one
 *          failure per pair keeps every column readable (degraded). */
static inline bool rc_volume_fatal(void)
{
	int i;

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		struct rc_adapter *m = rc_volume_members[i];

		/* A RESYNCING member is write-eligible but never live, so
		 * gate on the state as well as the live bit: a resync target
		 * whose controller dies must be failed by this fast path too,
		 * not left in write_mask stalling every new write behind the
		 * 30s blk-mq timeout. */
		if (m && READ_ONCE(m->ctx.nvme.dead) &&
		    ((atomic_read(&rc_volume_live_mask) & BIT(i)) ||
		     READ_ONCE(rc_volume_member_state[i]) ==
		     RC_MEMBER_RESYNCING))
			rc_volume_member_mark_failed(i);
	}

	return !rc_volume_can_serve();
}

/* blk-mq request handler.  chunk_sectors=stripe_sectors in queue_limits
 * keeps each request inside one stripe, so it maps to exactly one member.
 *
 * Async submission: dma_map_sg the bio's pages, build PRPs from the
 * resulting scatterlist, submit SQE with CID=tag, return BLK_STS_OK.
 * Completion lands in rc_nvme_irq, which calls blk_mq_complete_request;
 * rc_volume_complete then runs in softirq, dma_unmap_sg's the pages, and
 * ends the request.  Hardware DMAs directly to/from the bio's user
 * pages — no bounce buffers, no memcpy on either side. */

/* Append (page, len, offset) to a scatterlist under construction, merging
 * into the previous entry when physically contiguous — the same coalescing
 * blk_rq_map_sg performs for the single-member path.  The hand-rolled bvec
 * walks (mirror fan-out, multi-stripe split) MUST do this too: buffer-head
 * writeback (e.g. mkfs metadata) legally fills a bio with runs of sub-page
 * bvecs that are contiguous within a page, and virt_boundary allows them
 * precisely because they are gap-free.  One sg entry per bvec puts every
 * non-first segment at an in-page offset — which an NVMe PRP list cannot
 * express — so the controller rejects the command.  Observed on hardware:
 * every RAID1 mirror write of mke2fs metadata failed with SC 0x13 ("PRP
 * Offset Invalid") on both members, killing the OS install at the
 * create-filesystem step, while page-aligned I/O (dd, the QEMU rig) sailed
 * through.
 *
 * Returns the new entry count, or 0 when a fresh entry is needed but the
 * array is full (caller rejects the request). */
static u32 rc_volume_sg_append(struct scatterlist *sgl, u32 used, u32 max,
			       struct page *page, u32 len, u32 offset)
{
	if (used) {
		struct scatterlist *prev = &sgl[used - 1];

		if (page_to_phys(page) + offset ==
		    page_to_phys(sg_page(prev)) + prev->offset + prev->length) {
			prev->length += len;
			return used;
		}
	}
	if (used >= max)
		return 0;
	sg_set_page(&sgl[used], page, len, offset);
	return used + 1;
}

/* Dispatch a READ/WRITE that spans multiple RAID0 stripes (and thus
 * multiple members).  Walks the request's bvec stripe-by-stripe, building
 * per-member scatterlists; the pages belonging to member 0 are
 * non-contiguous in the bio (stripes 0,2,4,…) but contiguous on member 0's
 * drive once mapped to per-member phys LBAs.  We then dma_map_sg each
 * member's portion, build PRPs, and submit one NVMe command per affected
 * member.  members_pending is set to the count so the ISR's
 * atomic_dec_and_test completes the request when both members finish.
 *
 * This is the path that gives parity with Windows on SEQ writes:
 * a 1 MiB write becomes 2 NVMe commands (one per member, each 512 KiB =
 * MDTS) instead of the 4 we'd issue if blk-mq split at stripe boundary.
 *
 * The blk-mq queue_limits enforce that nr_sectors fits in
 * RC_VOLUME_DATA_BYTES (1 MiB by default) and that no member's portion
 * exceeds MDTS — combined with the per-member sg array sizing
 * (RC_VOLUME_MS_PAGES_PER_MEMBER), neither overflow is possible from
 * within these limits. */
/* ---------------------------------------------------------------------
 * RAID 5 full-row write path
 *
 * A complete row holds one chunk on every data member (0..N-2) plus the
 * dedicated parity chunk on member N-1.  Because every data chunk of the
 * row is present in the request, P is simply the XOR of them: no
 * read-modify-write and no old-parity read is needed.  The row is written
 * with one NVMe WRITE per member and the request completes when the last
 * one acks, exactly like the RAID1/RAID10 fan-outs.
 *
 * A request that covers less than a full row goes through
 * rc_volume_parity_rmw_write() instead: it reads the row's data chunks,
 * overlays the request, regenerates P and rewrites the touched chunks plus
 * parity.  Both paths take rc_volume_parity_lock, so two writes can never
 * update one row's parity from different old bytes.  Discards are refused
 * because they would leave parity stale.  A lost member is readable: the
 * degraded path XORs P with the surviving chunks, and the resync engine
 * rebuilds the missing member the same way.  There is still no write-hole
 * journal: a power cut between the data and parity commands can leave a row
 * inconsistent, the same exposure mdadm has without PPL.
 * --------------------------------------------------------------------- */

static void rc_volume_rmw_free(void)
{
	u32 bytes = rc_volume_rmw_bytes;
	int i;

	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++) {
		struct rc_volume_rmw_buf *b = &rc_volume_rmw[i];

		if (!b->dev)
			continue;
		if (b->va)
			dma_free_coherent(b->dev, bytes, b->va, b->pa);
		if (b->prp)
			dma_free_coherent(b->dev, PAGE_SIZE, b->prp,
					  b->prp_pa);
		memset(b, 0, sizeof(*b));
	}
	rc_volume_rmw_bytes = 0;
}

static int rc_volume_rmw_alloc(u32 chunk_bytes)
{
	int i;

	rc_volume_rmw_free();
	/* Publish the allocation size before the loop: the failure path
	 * calls rc_volume_rmw_free(), which frees each buffer with this
	 * size, so leaving it 0 until success would free the already-
	 * allocated members with a zero length (and the DMA API would not
	 * release them). */
	rc_volume_rmw_bytes = chunk_bytes;
	for (i = 0; i < rc_volume_member_count &&
		    i < RC_VOLUME_MAX_MEMBERS; i++) {
		struct rc_volume_rmw_buf *b = &rc_volume_rmw[i];
		struct device *dev;

		if (!rc_volume_members[i])
			continue;
		dev = &rc_volume_members[i]->pdev->dev;
		b->dev = dev;
		b->va = dma_alloc_coherent(dev, chunk_bytes, &b->pa,
					   GFP_KERNEL);
		b->prp = dma_alloc_coherent(dev, PAGE_SIZE, &b->prp_pa,
					    GFP_KERNEL);
		if (!b->va || !b->prp) {
			rc_printk(RC_WARN,
				  "rc_volume_rmw_alloc: cannot allocate RMW buffers for member %d — partial-stripe writes stay disabled\n",
				  i);
			rc_volume_rmw_free();
			return -ENOMEM;
		}
	}
	return 0;
}

static void rc_volume_parity_pool_free(void)
{
	int i;

	for (i = 0; i < RC_VOLUME_PARITY_BUFS; i++) {
		kfree(rc_volume_parity_va[i]);
		rc_volume_parity_va[i] = NULL;
	}
	rc_volume_parity_inuse = 0;
	rc_volume_parity_buf_bytes = 0;
	rc_volume_rmw_free();
}

static int rc_volume_parity_pool_alloc(u32 chunk_bytes)
{
	int i;

	rc_volume_parity_pool_free();
	for (i = 0; i < RC_VOLUME_PARITY_BUFS; i++) {
		rc_volume_parity_va[i] = kzalloc(chunk_bytes, GFP_KERNEL);
		if (!rc_volume_parity_va[i]) {
			rc_printk(RC_WARN,
				  "rc_volume_parity_pool_alloc: cannot allocate %u-byte parity buffers — RAID 5 writes stay disabled\n",
				  chunk_bytes);
			rc_volume_parity_pool_free();
			return -ENOMEM;
		}
	}
	rc_volume_parity_buf_bytes = chunk_bytes;
	return 0;
}

/* Allocate the pool when this is a RAID 5 volume with parity writes
 * requested and a chunk that fits one per-member scatterlist.  Failure is
 * not fatal: rc_volume_parity_write_ready() then reports false and the
 * volume comes up read-only, the same as before this path existed. */
static void rc_volume_parity_pool_prepare(void)
{
	u32 bytes;

	if (rc_volume_raid_level != RC_LDT_RAID5 &&
	    rc_volume_raid_level != RC_LDT_RAID6)
		return;
	bytes = rc_volume_stripe_sectors * 512u;
	if (!bytes || bytes > RC_VOLUME_MS_PAGES_PER_MEMBER * PAGE_SIZE)
		return;
	/* parity_pool_alloc() frees the rmw pool as part of its own reset, so
	 * it must run first.  The per-member coherent buffers then serve
	 * three consumers, not just writes: degraded read reconstruction,
	 * parity rebuild, and the read-modify-write path.  Allocate them even
	 * when writes are off, or a read-only parity volume could not survive
	 * a member loss. */
	if (rc_volume_raid_level == RC_LDT_RAID5 && rc_volume_parity_writes &&
	    rc_volume_parity_pool_alloc(bytes))
		return;
	if (!rc_volume_rmw_bytes)
		rc_volume_rmw_alloc(bytes);
}

static int rc_volume_parity_buf_acquire(void)
{
	int i;

	if (!rc_volume_parity_buf_bytes)
		return -ENOSPC;
	for (i = 0; i < RC_VOLUME_PARITY_BUFS; i++) {
		if (!rc_volume_parity_va[i])
			continue;
		if (!test_and_set_bit(i, &rc_volume_parity_inuse))
			return i;
	}
	return -ENOSPC;
}

/* May this volume accept the RAID 5 full-row write path at all?  True for
 * every non-parity level (they have their own write paths), false for
 * RAID 6 (no dual-parity writes), and for RAID 5 only when the operator
 * opted in, the pool exists, the chunk fits one per-member sg array and a
 * whole row fits one request. */
static bool rc_volume_parity_write_ready(void)
{
	u32 row;

	if (rc_volume_raid_level != RC_LDT_RAID5)
		return rc_volume_raid_level != RC_LDT_RAID6;
	if (!rc_volume_parity_writes || !rc_volume_parity_buf_bytes ||
	    !rc_volume_rmw_bytes)
		return false;
	if (rc_volume_member_count < 3)
		return false;
	if (rc_volume_stripe_sectors * 512u >
	    RC_VOLUME_MS_PAGES_PER_MEMBER * PAGE_SIZE)
		return false;
	row = rc_volume_stripe_sectors * (u32)(rc_volume_member_count - 1);
	return row && row <= (RC_VOLUME_DATA_BYTES / 512);
}

/* Does [pos, pos + nr_sectors) cover exactly one complete RAID 5 row? */
static bool rc_volume_parity_full_row(sector_t pos, u32 nr_sectors)
{
	u32 row;

	if (!rc_volume_parity_write_ready())
		return false;
	row = rc_volume_stripe_sectors * (u32)(rc_volume_member_count - 1);
	return row && nr_sectors == row && !(pos % row);
}

/* Synchronous chunk I/O for the read-modify-write path, out of the
 * member's coherent chunk buffer.  The SQE is built here rather than via
 * rc_nvme_read_lba/write_lba because those cap a transfer at two pages;
 * a chunk can be 512 KiB, so this uses the buffer's PRP list. */
static int rc_volume_rmw_io(int member, u8 opc, u64 slba, u32 sectors)
{
	struct rc_volume_rmw_buf *b;
	struct rc_nvme_sqe cmd;
	u32 bytes, pages, i;
	int ret = -ENODEV;
	int retry;

	if (member < 0 || member >= rc_volume_member_count ||
	    member >= RC_VOLUME_MAX_MEMBERS || !rc_volume_members[member])
		return -ENODEV;
	b = &rc_volume_rmw[member];
	if (!b->va || !b->prp || !rc_volume_rmw_bytes)
		return -ENODEV;
	bytes = sectors * 512u;
	if (!bytes || bytes > rc_volume_rmw_bytes ||
	    (b->pa & (PAGE_SIZE - 1)))
		return -EINVAL;
	/* The transfer may end in the middle of its last page (a rebuild
	 * tail), so count pages with DIV_ROUND_UP and point the PRP entries
	 * at whole pages: the NVMe byte count trims the final page. */
	pages = DIV_ROUND_UP(bytes, PAGE_SIZE);
	if (pages > 1 + (u32)(PAGE_SIZE / sizeof(__le64)))
		return -E2BIG;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = opc;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(b->pa);
	if (pages == 2) {
		cmd.prp2 = cpu_to_le64(b->pa + PAGE_SIZE);
	} else if (pages > 2) {
		for (i = 1; i < pages; i++)
			b->prp[i - 1] = cpu_to_le64(b->pa +
						   (dma_addr_t)i * PAGE_SIZE);
		cmd.prp2 = cpu_to_le64(b->prp_pa);
	}
	cmd.cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd.cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd.cdw12 = cpu_to_le32(sectors - 1);

	/* The sync helper borrows io_queues[0], which live READ traffic also
	 * uses.  A full queue is transient; the blocking path can wait. */
	for (retry = 0; retry < 1000; retry++) {
		ret = rc_nvme_io_cmd_sync(rc_volume_members[member], &cmd);
		if (ret != -EBUSY)
			break;
		usleep_range(1000, 2000);
	}
	return ret;
}

/* dst ^= src over a whole chunk.  Both buffers are coherent per-member
 * chunk buffers, so the length is a multiple of 8. */
static void rc_volume_xor(void *dst, const void *src, u32 bytes)
{
	u64 *d = dst;
	const u64 *s = src;
	u32 i;

	for (i = 0; i < bytes / sizeof(u64); i++)
		d[i] ^= s[i];
}

/* Reconstruct one missing chunk into rc_volume_rmw[target].va.
 *
 * For every RAID 5 row the N chunks XOR to zero (parity is the XOR of the
 * data chunks), so the missing chunk is the XOR of all the row's surviving
 * chunks.  The caller holds rc_volume_parity_lock, which also owns the
 * per-member buffers. */
static int rc_volume_parity_reconstruct_chunk(u64 stripe_lba, int target)
{
	const u32 chunk = rc_volume_stripe_sectors;
	const u32 chunk_bytes = chunk * 512u;
	const int nm = rc_volume_member_count;
	/* stripe_lba is chunk-aligned; a row spans one chunk on each of the
	 * (nm - 1) data members, so divide by the logical row size. */
	const u64 row = div_u64(stripe_lba, (u64)chunk * (nm - 1));
	u8 *dst;
	u32 live;
	int m, rc;

	if (nm < 3 || target < 0 || target >= nm ||
	    target >= RC_VOLUME_MAX_MEMBERS || !chunk || !chunk_bytes)
		return -EINVAL;
	dst = rc_volume_rmw[target].va;
	if (!dst)
		return -ENODEV;
	live = (u32)atomic_read(&rc_volume_live_mask);

	memset(dst, 0, chunk_bytes);
	for (m = 0; m < nm; m++) {
		if (m == target)
			continue;
		if (!(live & BIT(m)))
			return -EIO;
		rc = rc_volume_rmw_io(m, RC_NVME_NVM_OP_READ,
				      row * chunk +
				      rc_volume_member_phys_offset[m], chunk);
		if (rc)
			return rc;
		rc_volume_xor(dst, rc_volume_rmw[m].va, chunk_bytes);
	}
	return 0;
}

/* Copy @len bytes into the request's bvecs starting at logical byte
 * @req_off.  Used by the blocking degraded-read path, which has already
 * assembled the bytes in a chunk buffer. */
static int rc_volume_copy_to_request(struct request *req, u32 req_off,
				     const void *src, u32 len)
{
	struct req_iterator iter;
	struct bio_vec bv;
	u32 pos = 0;

	rq_for_each_segment(bv, req, iter) {
		u32 end = pos + bv.bv_len;

		if (req_off < end) {
			u32 skip = req_off - pos;
			u32 n = min(len, bv.bv_len - skip);
			void *kaddr = kmap_local_page(bv.bv_page);

			memcpy(kaddr + bv.bv_offset + skip, src, n);
			kunmap_local(kaddr);
			src = (const u8 *)src + n;
			len -= n;
			req_off += n;
			if (!len)
				return 0;
		}
		pos = end;
	}
	return -EIO;
}

/* Degraded RAID 5 read: one member is missing, so chunks that map to it
 * are reconstructed from parity and the survivors.  Runs synchronously in
 * queue_rq (the tagset is BLK_MQ_F_BLOCKING for parity levels) and takes
 * rc_volume_parity_lock so it cannot race the per-member buffers used by
 * writes or a rebuild.  Writes are refused while degraded, so no
 * read-modify-write can be in flight here. */
static blk_status_t rc_volume_parity_degraded_read(struct request *req)
{
	const u32 chunk = rc_volume_stripe_sectors;
	const u32 nr = blk_rq_sectors(req);
	const sector_t pos = blk_rq_pos(req);
	const u32 live = (u32)atomic_read(&rc_volume_live_mask);
	u32 done = 0;
	blk_status_t st = BLK_STS_OK;

	if (rc_volume_member_count < 3 || !chunk || !nr ||
	    rc_volume_rmw_bytes < chunk * 512u) {
		rc_printk(RC_ERROR,
			  "rc_volume: degraded RAID5 read at lba %llu cannot reconstruct — chunk buffer missing (chunk=%u bytes=%u)\n",
			  (unsigned long long)pos, chunk,
			  rc_volume_rmw_bytes);
		return BLK_STS_IOERR;
	}

	down(&rc_volume_parity_lock);
	while (done < nr) {
		const sector_t cur = pos + done;
		const sector_t stripe_lba = cur - (cur % chunk);
		const u32 off = (u32)(cur - stripe_lba);
		const u32 nsec = min(nr - done, chunk - off);
		int tgt, rc;
		u64 phys;
		u8 *src;

		rc_volume_map_lba(cur, &tgt, &phys);
		if (tgt < 0 || tgt >= rc_volume_member_count) {
			st = BLK_STS_IOERR;
			break;
		}
		if (live & BIT(tgt)) {
			rc = rc_volume_rmw_io(tgt, RC_NVME_NVM_OP_READ,
					      phys - off, chunk);
		} else {
			rc = rc_volume_parity_reconstruct_chunk(stripe_lba, tgt);
		}
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume: degraded RAID5 read failed at lba %llu (member %d, row chunk %llu): %d\n",
				  (unsigned long long)cur, tgt,
				  (unsigned long long)stripe_lba, rc);
			st = BLK_STS_IOERR;
			break;
		}
		src = (u8 *)rc_volume_rmw[tgt].va + off * 512u;
		if (rc_volume_copy_to_request(req, done * 512u, src,
					      nsec * 512u)) {
			rc_printk(RC_ERROR,
				  "rc_volume: degraded RAID5 read could not copy lba %llu len %u into the request\n",
				  (unsigned long long)cur, nsec);
			st = BLK_STS_IOERR;
			break;
		}
		done += nsec;
	}
	up(&rc_volume_parity_lock);
	return st;
}

/* Partial-row RAID 5 write.  Only the chunks the slice touches are read,
 * plus the old parity, and P becomes P_old ^ D_old ^ D_new (XORing the
 * whole rewritten chunk is safe: bytes outside the slice are unchanged, so
 * their old and new values cancel).  Runs synchronously under
 * rc_volume_parity_lock, which serializes it against every other RAID 5
 * write (a second write that read the same old row would be lost).
 *
 * A request may cross a row boundary; each row is handled in turn.  The
 * write hole remains: a power cut between the data and parity commands
 * leaves that row's P stale, exactly as mdadm without a journal. */
static blk_status_t rc_volume_parity_rmw_write(struct request *req,
					       sector_t pos, u32 nr_sectors)
{
	const u32 chunk = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	const int parity = nm - 1;
	const u32 row_sectors = chunk * (u32)parity;
	const u32 chunk_bytes = chunk * 512u;
	sector_t cur = pos;
	u32 remaining = nr_sectors;
	blk_status_t st = BLK_STS_OK;

	if (nm < 3 || nm > RC_VOLUME_MAX_MEMBERS || !chunk || !row_sectors ||
	    !chunk_bytes)
		return BLK_STS_IOERR;

	down(&rc_volume_parity_lock);
	if (rc_volume_fatal()) {
		rc_printk(RC_ERROR,
			  "rc_volume_parity_rmw_write: volume can no longer serve — rejected\n");
		st = BLK_STS_IOERR;
		goto out;
	}

	while (remaining) {
		const u64 row = div_u64(cur, row_sectors);
		const u32 row_off = (u32)(cur - row * row_sectors);
		const u32 nsec = min(remaining, row_sectors - row_off);
		struct req_iterator iter;
		struct bio_vec bv;
		sector_t lba = pos;
		u64 *pd;
		u32 words, i;
		int first, last, m, rc;

		/* Classic read-modify-write: only the chunks this slice
		 * touches are read, plus the old parity.
		 *   P_new = P_old ^ D_old ^ D_new
		 */
		first = (int)(row_off / chunk);
		last = (int)((row_off + nsec - 1) / chunk);

		rc = rc_volume_rmw_io(parity, RC_NVME_NVM_OP_READ,
				      row * chunk +
				      rc_volume_member_phys_offset[parity], chunk);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_parity_rmw_write: read row %llu parity failed (%d) — pos=%llu len=%u\n",
				  row, rc, (u64)pos, nr_sectors);
			st = BLK_STS_IOERR;
			goto out;
		}
		pd = (u64 *)(void *)rc_volume_rmw[parity].va;
		words = chunk_bytes / sizeof(u64);

		for (m = first; m <= last; m++) {
			const u64 *sd;

			rc = rc_volume_rmw_io(m, RC_NVME_NVM_OP_READ,
					      row * chunk +
					      rc_volume_member_phys_offset[m],
					      chunk);
			if (rc) {
				rc_printk(RC_ERROR,
					  "rc_volume_parity_rmw_write: read row %llu member %d failed (%d) — pos=%llu len=%u\n",
					  row, m, rc, (u64)pos, nr_sectors);
				st = BLK_STS_IOERR;
				goto out;
			}
			sd = (const u64 *)(void *)rc_volume_rmw[m].va;
			for (i = 0; i < words; i++)
				pd[i] ^= sd[i];	/* P_old ^= D_old */
		}

		/* Overlay the part of the request that falls in this row.
		 * A bvec does not have to start on a chunk boundary, so split it
		 * where it runs into the next chunk: one memcpy of the whole
		 * segment into chunk @mbr would run past the end of that chunk's
		 * coherent buffer and fault on the unmapped page after it (seen
		 * mounting XFS on a 4-member RAID 5, 2026-11-14). */
		rq_for_each_segment(bv, req, iter) {
			sector_t seg_start = lba;
			sector_t seg_end = lba + bv.bv_len / 512;
			sector_t from = max_t(sector_t, seg_start, cur);
			sector_t to = min_t(sector_t, seg_end, cur + nsec);

			if (from < to) {
				u8 *src = kmap_local_page(bv.bv_page);

				while (from < to) {
					u32 rpos = row_off + (u32)(from - cur);
					u32 mbr = rpos / chunk;
					u32 coff = rpos % chunk;
					u32 nsplit = min_t(u32,
							   (u32)(to - from),
							   chunk - coff);
					u32 skip = (u32)(from - seg_start);

					/* The parity chunk holds no request bytes. */
					if (mbr >= (u32)parity)
						break;
					memcpy((u8 *)rc_volume_rmw[mbr].va +
					       (size_t)coff * 512u,
					       src + bv.bv_offset +
					       (size_t)skip * 512u,
					       nsplit * 512u);
					from += nsplit;
				}
				kunmap_local(src);
			}
			lba = seg_end;
		}

		/* P ^= D_new for every touched chunk. */
		for (m = first; m <= last; m++) {
			const u64 *sd =
				(const u64 *)(void *)rc_volume_rmw[m].va;

			for (i = 0; i < words; i++)
				pd[i] ^= sd[i];
		}

		/* Rewrite only the chunks this row's slice touches, then P. */
		for (m = first; m <= last; m++) {
			rc = rc_volume_rmw_io(m, RC_NVME_NVM_OP_WRITE,
					      row * chunk +
					      rc_volume_member_phys_offset[m],
					      chunk);
			if (rc) {
				rc_printk(RC_ERROR,
					  "rc_volume_parity_rmw_write: write row %llu member %d failed (%d) — parity now stale for that row\n",
					  row, m, rc);
				st = BLK_STS_IOERR;
				goto out;
			}
		}

		rc = rc_volume_rmw_io(parity, RC_NVME_NVM_OP_WRITE,
				      row * chunk +
				      rc_volume_member_phys_offset[parity], chunk);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_parity_rmw_write: write row %llu parity failed (%d)\n",
				  row, rc);
			st = BLK_STS_IOERR;
			goto out;
		}

		if (req->cmd_flags & REQ_FUA) {
			for (m = 0; m < nm; m++) {
				rc = rc_nvme_flush(rc_volume_members[m]);
				if (rc) {
					rc_printk(RC_ERROR,
						  "rc_volume_parity_rmw_write: FUA flush member %d failed (%d)\n",
						  m, rc);
					st = BLK_STS_IOERR;
					goto out;
				}
			}
		}

		cur += nsec;
		remaining -= nsec;
	}

out:
	up(&rc_volume_parity_lock);
	return st;
}

/* XOR len bytes of src into dst.  The CPU cost is bounded by one chunk
 * per row written; the parity path is an opt-in first step, not the
 * tuned hot path. */
static void rc_volume_xor_bytes(u8 *dst, const u8 *src, u32 len)
{
	u32 i;

	for (i = 0; i < len; i++)
		dst[i] ^= src[i];
}

static blk_status_t rc_volume_dispatch_parity_write(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors)
{
	const u32 chunk = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	const int parity = nm - 1;
	const u32 row_sectors = chunk * (u32)parity;
	const u32 chunk_bytes = chunk * 512u;
	const u64 row = div_u64(pos, row_sectors);
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t cur = pos;
	u32 sg_used[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 pages, i;
	int slot, m;
	u8 *parity_va;

	if (nm < 3 || nm > RC_VOLUME_MAX_MEMBERS || !chunk ||
	    !row_sectors || nr_sectors != row_sectors || (pos % row_sectors) ||
	    chunk_bytes > RC_VOLUME_MS_PAGES_PER_MEMBER * PAGE_SIZE) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_parity_write: bad geometry (members=%d chunk=%u row=%u len=%u pos=%llu) — rejected\n",
			  nm, chunk, row_sectors, nr_sectors, (u64)pos);
		return BLK_STS_IOERR;
	}

	/* Every RAID 5 write is serialized so a partial-row RMW and a
	 * full-row write can never update one row's P from different old
	 * bytes.  Waiting is legal because the tagset carries
	 * BLK_MQ_F_BLOCKING for every parity level, and it has to be a real
	 * wait: BLK_STS_DEV_RESOURCE promises blk-mq that the driver reruns
	 * the queue once the resource frees, and nothing here does.  A busy
	 * lock handed back that way parked a buffered write in
	 * folio_wait_writeback forever — 2026-11-13, 3-member RAID 5. */
	down(&rc_volume_parity_lock);
	pdu->parity_locked = true;

	slot = rc_volume_parity_buf_acquire();
	if (slot < 0) {
		/* Only this path takes a pool buffer and it now holds the
		 * lock, so an empty pool is a leak.  Fail it loudly rather
		 * than requeue a request nothing would restart. */
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_parity_write: parity buffer pool empty under the parity lock — pos=%llu len=%u rejected\n",
			  (u64)pos, nr_sectors);
		rc_volume_unmap_request_sg(pdu);
		return BLK_STS_IOERR;
	}
	parity_va = rc_volume_parity_va[slot];
	pdu->parity_buf = slot;
	memset(parity_va, 0, chunk_bytes);

	for (m = 0; m < nm; m++)
		sg_init_table(pdu->ms_sg[m], RC_VOLUME_MS_PAGES_PER_MEMBER);

	/* Walk the row once: place each fragment on its data member's sg and
	 * XOR the same bytes into the parity buffer.  A fragment never spans
	 * a chunk boundary on a full row, so the member is
	 * (offset / chunk) and the parity offset is (offset % chunk). */
	rq_for_each_segment(bv, req, iter) {
		u32 seg_off = bv.bv_offset;
		u32 seg_len_sectors = bv.bv_len / 512;
		struct page *seg_page = bv.bv_page;
		u32 seg_remaining = bv.bv_len;

		if (bv.bv_len & 511u) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_parity_write: non-sector-aligned segment (%u bytes) — rejected\n",
				  bv.bv_len);
			goto err_unmap;
		}

		while (seg_remaining) {
			u32 off_sectors = (u32)(cur - pos);
			u32 mbr = off_sectors / chunk;
			u32 chunk_off = off_sectors - mbr * chunk;
			u32 nsec = min(seg_len_sectors, chunk - chunk_off);
			u32 nbytes = nsec * 512u;
			u8 *src;

			if (!nbytes || nbytes > seg_remaining ||
			    mbr >= (u32)parity) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_parity_write: row split failed (member=%u off=%u nbytes=%u remaining=%u) — rejected\n",
					  mbr, chunk_off, nbytes, seg_remaining);
				goto err_unmap;
			}

			sg_used[mbr] = rc_volume_sg_append(pdu->ms_sg[mbr],
						sg_used[mbr],
						RC_VOLUME_MS_PAGES_PER_MEMBER,
						seg_page, nbytes, seg_off);
			if (!sg_used[mbr]) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_parity_write: member %u sg overflow (>%u entries) — rejected\n",
					  mbr, RC_VOLUME_MS_PAGES_PER_MEMBER);
				goto err_unmap;
			}

			src = kmap_local_page(seg_page);
			rc_volume_xor_bytes(parity_va + (size_t)chunk_off * 512u,
					    src + seg_off, nbytes);
			kunmap_local(src);

			seg_off         += nbytes;
			seg_remaining   -= nbytes;
			seg_len_sectors -= nsec;
			cur             += nsec;
		}
	}

	if (cur != pos + row_sectors) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_parity_write: short request (covered %llu of %u sectors) — rejected\n",
			  (unsigned long long)(cur - pos), row_sectors);
		goto err_unmap;
	}

	/* The parity chunk is written straight out of its pool buffer.  Each
	 * page is page-aligned, which is what rc_volume_build_prp requires
	 * after the first entry. */
	pages = chunk_bytes / PAGE_SIZE;
	if ((chunk_bytes % PAGE_SIZE) ||
	    pages > RC_VOLUME_MS_PAGES_PER_MEMBER) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_parity_write: %u-byte parity chunk is not page-expressible — rejected\n",
			  chunk_bytes);
		goto err_unmap;
	}
	for (i = 0; i < pages; i++)
		sg_set_page(&pdu->ms_sg[parity][i],
			    virt_to_page(parity_va + (size_t)i * PAGE_SIZE),
			    PAGE_SIZE, 0);
	sg_used[parity] = pages;

	for (m = 0; m < nm; m++) {
		if (!sg_used[m]) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_parity_write: member %d has no data in the row — rejected\n",
				  m);
			goto err_unmap;
		}
		sg_mark_end(&pdu->ms_sg[m][sg_used[m] - 1]);
	}

	pdu->ms_active = true;
	for (m = 0; m < nm; m++) {
		int mapped;

		if (!rc_volume_members[m]) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_parity_write: member %d absent — rejected\n",
				  m);
			goto err_unmap;
		}
		mapped = dma_map_sg(&rc_volume_members[m]->pdev->dev,
				    pdu->ms_sg[m], sg_used[m], DMA_TO_DEVICE);
		if (!mapped) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_parity_write: dma_map_sg failed (member %d, %u entries) — pos=%llu len=%u rejected\n",
				  m, sg_used[m], (u64)pos, nr_sectors);
			goto err_unmap;
		}
		pdu->ms_nents[m] = mapped;
	}

	for (m = 0; m < nm; m++) {
		struct rc_nvme_io_queue *q =
			rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num];
		int tries;

		/* SQE slots come back from completions, and BLK_MQ_F_BLOCKING
		 * lets this context wait for them.  BLK_STS_RESOURCE is the
		 * fallback: the block layer retries that one on its own. */
		for (tries = 0; tries < 1000; tries++) {
			if (rc_nvme_sq_reserve(q, 1))
				break;
			usleep_range(50, 100);
		}
		if (tries == 1000) {
			int u;

			for (u = 0; u < m; u++)
				rc_nvme_sq_unreserve(
					rc_volume_members[u]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_parity_write: member %d SQ full for 75 ms — pos=%llu len=%u requeued\n",
				  m, (u64)pos, nr_sectors);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_RESOURCE;
		}
	}

	pdu->member_idx = -1;	/* fan-out — see member_mask */
	pdu->member_mask = (1u << nm) - 1u;
	atomic_set(&pdu->members_pending, nm);

	{
		struct rc_nvme_sqe cmds[RC_VOLUME_MAX_MEMBERS];
		u32 tag = req->tag;

		/* Build every SQE before starting/submitting, so a PRP
		 * expressibility failure can still reject the request. */
		for (m = 0; m < nm; m++) {
			if (rc_volume_build_io_sqe(&cmds[m], hctx->queue_num, m,
					tag, RC_NVME_NVM_OP_WRITE,
					row * chunk +
						rc_volume_member_phys_offset[m],
					(u16)(chunk - 1),
					(req->cmd_flags & REQ_FUA) != 0,
					pdu->ms_sg[m], pdu->ms_nents[m])) {
				int u;

				for (u = 0; u < nm; u++)
					rc_nvme_sq_unreserve(rc_volume_members[u]->ctx.nvme.io_queues[hctx->queue_num], 1);
				rc_volume_unmap_request_sg(pdu);
				return BLK_STS_IOERR;
			}
		}

		blk_mq_start_request(req);
		for (m = 0; m < nm; m++)
			rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
					  &cmds[m]);
	}

	return BLK_STS_OK;

err_unmap:
	rc_volume_unmap_request_sg(pdu);
	return BLK_STS_IOERR;
}

/* Rebuild the per-member SG lists over slices of one bounce buffer.
 * Slices follow member-LBA order, so each member's run is contiguous even
 * when the request's bvec runs are not.  Returns 0, -ENOMEM when no pool
 * buffer is available, or -EINVAL when the layout does not add up. */
static int rc_volume_ms_bounce_build(struct request *req,
				     struct rc_volume_pdu *pdu,
				     u32 *sg_used,
				     const u32 *member_sectors,
				     const bool *member_has_data,
				     u32 slots, enum req_op op)
{
	u32 bytes = blk_rq_bytes(req);
	u32 off = 0;
	int m;

	if (!bytes || bytes > RC_VOLUME_DATA_BYTES)
		return -EINVAL;
	pdu->ms_bounce = rc_volume_bounce_alloc();
	if (!pdu->ms_bounce)
		return -ENOMEM;

	for (m = 0; m < (int)slots; m++) {
		pdu->ms_bounce_off[m] = off;
		pdu->ms_bounce_len[m] = member_sectors[m] * 512u;
		off += pdu->ms_bounce_len[m];
	}
	if (off != bytes)
		goto err;
	pdu->ms_bounce_slots = (u8)slots;

	if (op == REQ_OP_WRITE &&
	    rc_volume_ms_bounce_copy(req, pdu->ms_bounce,
				     pdu->ms_bounce_off, pdu->ms_bounce_len,
				     slots, true))
		goto err;

	for (m = 0; m < (int)slots; m++) {
		if (!member_has_data[m])
			continue;
		if (rc_volume_bounce_sg(pdu->ms_sg[m],
					RC_VOLUME_MS_PAGES_PER_MEMBER,
					pdu->ms_bounce + pdu->ms_bounce_off[m],
					pdu->ms_bounce_len[m], &sg_used[m]))
			goto err;
	}
	return 0;

err:
	rc_volume_bounce_free(pdu->ms_bounce);
	pdu->ms_bounce = NULL;
	return -EINVAL;
}

/* Rebuild RAID10's per-member SG lists over COLUMN slices of one bounce
 * buffer.  Both copies of a mirror pair hold the same bytes, so a column
 * has ONE slice and both members' SG lists map it — which is why this
 * cannot reuse rc_volume_ms_bounce_build (that one lays out one slice per
 * member).  Slices follow column order, matching the member-LBA order the
 * per-member SQE lengths were computed from.  Returns 0, -ENOMEM when no
 * pool buffer is available, or -EINVAL when the layout does not add up. */
static int rc_volume_raid10_bounce_build(struct request *req,
					 struct rc_volume_pdu *pdu,
					 u32 *sg_used,
					 const u32 *member_sectors,
					 const bool *member_has_data,
					 u32 cols, enum req_op op)
{
	u32 bytes = blk_rq_bytes(req);
	u32 off = 0;
	u32 c;
	int m;

	if (!bytes || bytes > RC_VOLUME_DATA_BYTES || !cols ||
	    cols > RC_VOLUME_MAX_MEMBERS)
		return -EINVAL;

	pdu->ms_bounce = rc_volume_bounce_alloc();
	if (!pdu->ms_bounce)
		return -ENOMEM;

	/* One slice per column.  A degraded column has only one live copy,
	 * so take whichever copy carries the data. */
	for (c = 0; c < cols; c++) {
		u32 len = 0;

		for (m = 0; m < (int)(cols * 2u); m++) {
			if (member_has_data[m] && (u32)m / 2u == c) {
				len = member_sectors[m] * 512u;
				break;
			}
		}
		pdu->ms_bounce_off[c] = off;
		pdu->ms_bounce_len[c] = len;
		off += len;
	}
	if (off != bytes)
		goto err;
	/* The completion path copies a bounced READ back with this geometry:
	 * one slice per column, positions 0..cols-1. */
	pdu->ms_bounce_slots = (u8)cols;

	if (op == REQ_OP_WRITE &&
	    rc_volume_ms_bounce_copy(req, pdu->ms_bounce, pdu->ms_bounce_off,
				     pdu->ms_bounce_len, cols, true))
		goto err;

	for (m = 0; m < (int)(cols * 2u); m++) {
		if (!member_has_data[m])
			continue;
		c = (u32)m / 2u;
		if (rc_volume_bounce_sg(pdu->ms_sg[m],
					RC_VOLUME_MS_PAGES_PER_MEMBER,
					pdu->ms_bounce + pdu->ms_bounce_off[c],
					pdu->ms_bounce_len[c], &sg_used[m]))
			goto err;
	}
	return 0;

err:
	rc_volume_bounce_free(pdu->ms_bounce);
	pdu->ms_bounce = NULL;
	return -EINVAL;
}

static blk_status_t rc_volume_dispatch_multi_stripe(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors,
		enum req_op op, bool parity_read)
{
	const u32 stripe = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	/* RAID0 uses every member as a stripe column.  A parity READ uses
	 * only the data members 0..N-2: the parity member holds no logical
	 * data, and rc_volume_map_lba already routes a read to a data
	 * member, so the modulus is the only difference. */
	const int slots = parity_read ?
		(rc_volume_raid_level == RC_LDT_RAID6 ? nm - 2 : nm - 1) : nm;
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t cur_lba = pos;
	u32 sg_used[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 member_sectors[RC_VOLUME_MAX_MEMBERS] = {0};
	u64 member_start_lba[RC_VOLUME_MAX_MEMBERS] = {0};
	bool member_has_data[RC_VOLUME_MAX_MEMBERS] = {0};
	int members_with_data = 0;
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || slots <= 0 || !stripe) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_multi_stripe: bad volume state (members=%d slots=%d stripe=%u) — pos=%llu len=%u rejected\n",
			  nm, slots, stripe, (u64)pos, nr_sectors);
		return BLK_STS_IOERR;
	}

	for (m = 0; m < nm; m++)
		sg_init_table(pdu->ms_sg[m], RC_VOLUME_MS_PAGES_PER_MEMBER);

	/* Walk the bvec, assigning pages to per-member sgs based on which
	 * stripe each section of the bvec lands in.  A single bvec entry can
	 * span a stripe boundary — split it at the boundary into two sg
	 * entries on different members. */
	rq_for_each_segment(bv, req, iter) {
		u32 seg_len_sectors = bv.bv_len / 512;
		u32 seg_off_in_page = bv.bv_offset;
		struct page *seg_page = bv.bv_page;
		u32 seg_remaining = bv.bv_len;

		while (seg_remaining) {
			sector_t stripe_idx = cur_lba / stripe;
			sector_t stripe_end = (stripe_idx + 1) * stripe;
			u32 sectors_to_boundary =
				(u32)(stripe_end - cur_lba);
			u32 chunk_sectors = min(seg_len_sectors,
						sectors_to_boundary);
			u32 chunk_bytes = chunk_sectors * 512u;
			int mbr = (int)(stripe_idx % (u64)slots);

			if (chunk_bytes == 0 || chunk_bytes > seg_remaining) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_multi_stripe: bvec split error (chunk_bytes=%u seg_remaining=%u bv_len=%u) — pos=%llu len=%u rejected\n",
					  chunk_bytes, seg_remaining, bv.bv_len,
					  (u64)pos, nr_sectors);
				return BLK_STS_IOERR;
			}
			if (!member_has_data[mbr]) {
				u64 phys_lba;
				int phys_member;
				rc_volume_map_lba(cur_lba, &phys_member,
						  &phys_lba);
				if (phys_member != mbr) {
					rc_printk(RC_ERROR,
						  "rc_volume_dispatch_multi_stripe: member mismatch (map_lba=%d stripe_mod=%d cur_lba=%llu) — pos=%llu len=%u rejected\n",
						  phys_member, mbr,
						  (u64)cur_lba, (u64)pos,
						  nr_sectors);
					return BLK_STS_IOERR;
				}
				member_start_lba[mbr] = phys_lba;
				member_has_data[mbr] = true;
				members_with_data++;
			}

			/* Merge physically-contiguous fragments (sub-page
			 * bvec runs, or consecutive stripes owned by the
			 * same member) — non-first sg entries at in-page
			 * offsets are unexpressible as PRPs. */
			sg_used[mbr] = rc_volume_sg_append(
					pdu->ms_sg[mbr], sg_used[mbr],
					RC_VOLUME_MS_PAGES_PER_MEMBER,
					seg_page, chunk_bytes,
					seg_off_in_page);
			if (!sg_used[mbr]) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_multi_stripe: member %d sg overflow (>%u entries) — pos=%llu len=%u rejected\n",
					  mbr, RC_VOLUME_MS_PAGES_PER_MEMBER,
					  (u64)pos, nr_sectors);
				return BLK_STS_IOERR;
			}
			member_sectors[mbr] += chunk_sectors;

			seg_off_in_page += chunk_bytes;
			seg_remaining   -= chunk_bytes;
			seg_len_sectors -= chunk_sectors;
			cur_lba         += chunk_sectors;
		}
	}

	if (members_with_data < 2) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_multi_stripe: only %d member with data — pos=%llu len=%u rejected\n",
			  members_with_data, (u64)pos, nr_sectors);
		return BLK_STS_IOERR;	/* shouldn't reach here for single-member */
	}

	/* A member's run can start mid-page when the request joins bvec runs
	 * with a page gap (XFS log recovery does exactly this).  Rebuild all
	 * members' SG lists over a bounce buffer in that case. */
	for (m = 0; m < nm; m++) {
		if (member_has_data[m] &&
		    !rc_volume_sg_prp_ready(pdu->ms_sg[m], sg_used[m]))
			break;
	}
	if (m < nm) {
		int bret = rc_volume_ms_bounce_build(req, pdu, sg_used,
						      member_sectors,
						      member_has_data, slots, op);

		if (bret == -ENOMEM)
			return BLK_STS_RESOURCE;
		if (bret) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_multi_stripe: bounce layout failed — pos=%llu len=%u rejected\n",
				  (u64)pos, nr_sectors);
			return BLK_STS_IOERR;
		}
	}

	/* Mark each member's sg array as the active subset. */
	for (m = 0; m < nm; m++) {
		if (!member_has_data[m])
			continue;
		sg_mark_end(&pdu->ms_sg[m][sg_used[m] - 1]);
	}

	/* dma_map_sg each member's portion.  On failure, unwind whatever
	 * already mapped. */
	pdu->ms_active = true;
	for (m = 0; m < nm; m++) {
		struct device *dma_dev;
		int mapped;

		if (!member_has_data[m]) {
			pdu->ms_nents[m] = 0;
			continue;
		}
		dma_dev = &rc_volume_members[m]->pdev->dev;
		mapped = dma_map_sg(dma_dev, pdu->ms_sg[m], sg_used[m],
				    rc_volume_dma_dir(op));
		if (mapped == 0) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_multi_stripe: dma_map_sg failed (member %d, %u entries) — pos=%llu len=%u rejected\n",
				  m, sg_used[m], (u64)pos, nr_sectors);
			/* Unwind: undo previously mapped members. */
			pdu->ms_nents[m] = 0;
			for (--m; m >= 0; m--) {
				if (!pdu->ms_nents[m])
					continue;
				dma_dev = &rc_volume_members[m]->pdev->dev;
				dma_unmap_sg(dma_dev, pdu->ms_sg[m],
					     pdu->ms_nents[m],
					     rc_volume_dma_dir(op));
				pdu->ms_nents[m] = 0;
			}
			pdu->ms_active = false;
			rc_volume_bounce_free(pdu->ms_bounce);
			pdu->ms_bounce = NULL;
			return BLK_STS_IOERR;
		}
		pdu->ms_nents[m] = mapped;
	}

	/* One SQE on each involved member's queue — reserve or requeue
	 * (C-4).  Runs before blk_mq_start_request so DEV_RESOURCE is a
	 * clean retry. */
	for (m = 0; m < nm; m++) {
		if (!pdu->ms_nents[m])
			continue;
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				if (pdu->ms_nents[m])
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	pdu->member_idx = -1;	/* multi-member — no single member to attribute */
	pdu->member_mask = 0;
	for (m = 0; m < nm; m++)
		if (pdu->ms_nents[m])
			pdu->member_mask |= BIT(m);
	atomic_set(&pdu->members_pending, members_with_data);

	/* Build every member's SQE BEFORE starting/submitting anything, so a
	 * PRP-expressibility failure can still cleanly reject the request
	 * (after the first doorbell there is no unsubmitting). */
	{
		struct rc_nvme_sqe cmds[RC_VOLUME_MAX_MEMBERS];
		u32 tag = req->tag;

		for (m = 0; m < nm; m++) {
			if (!pdu->ms_nents[m])
				continue;
			if (rc_volume_build_io_sqe(&cmds[m], hctx->queue_num,
					m, tag,
					op == REQ_OP_WRITE ?
					  RC_NVME_NVM_OP_WRITE :
					  RC_NVME_NVM_OP_READ,
					member_start_lba[m],
					(u16)(member_sectors[m] - 1),
					(req->cmd_flags & REQ_FUA) != 0,
					pdu->ms_sg[m], pdu->ms_nents[m])) {
				int u;

				for (u = 0; u < nm; u++)
					if (pdu->ms_nents[u])
						rc_nvme_sq_unreserve(rc_volume_members[u]->ctx.nvme.io_queues[hctx->queue_num], 1);
				rc_volume_unmap_request_sg(pdu);
				return BLK_STS_IOERR;
			}
		}

		/* Must call blk_mq_start_request before the first submit (an
		 * ISR completion racing us would otherwise call
		 * blk_mq_complete on a request blk-mq doesn't yet consider
		 * started). */
		pdu->io_started = true;
		blk_mq_start_request(req);
		for (m = 0; m < nm; m++) {
			if (!pdu->ms_nents[m])
				continue;
			rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
					  &cmds[m]);
		}
	}

	return BLK_STS_OK;
}

/* Dispatch a DISCARD that spans multiple RAID0 stripes.
 *
 * The key observation: on RAID0, the stripes owned by member m within a
 * logical range form ONE contiguous physical extent on m's drive.  So a
 * multi-stripe discard becomes exactly nm one-range DSM Deallocates — never
 * one DSM per stripe.  This is what cuts a `mkfs.xfs` discard pass from
 * ~70 minutes (one DSM per 256 KiB stripe × 14.9M stripes) to seconds (one
 * DSM per ~1 GiB chunk × ~3600 chunks × nm members).
 *
 * Per-member math is O(1) — no stripe-by-stripe walk.  For each member m we
 * compute the first and last stripes it owns inside [start_stripe, end_stripe]
 * and from those derive a single (slba, nlb) pair.  Partial-stripe coverage
 * at the discard's start/end is folded into start_off / end_off.
 */
static blk_status_t rc_volume_dispatch_multi_stripe_discard(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors)
{
	const u32 stripe = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	const u64 start_stripe = pos / stripe;
	const u64 end_lba = pos + (u64)nr_sectors - 1;
	const u64 end_stripe = end_lba / stripe;
	const u32 start_off_global = (u32)(pos - start_stripe * stripe);
	const u32 end_off_global   = (u32)(end_lba - end_stripe * stripe);
	u64 member_start_lba[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 member_sectors[RC_VOLUME_MAX_MEMBERS] = {0};
	bool member_has_data[RC_VOLUME_MAX_MEMBERS] = {0};
	int members_with_data = 0;
	u32 tag = req->tag;
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || !stripe)
		return BLK_STS_IOERR;

	for (m = 0; m < nm; m++) {
		u32 start_stripe_owner = (u32)(start_stripe % (u32)nm);
		u32 rel = ((u32)m + (u32)nm - start_stripe_owner) % (u32)nm;
		u64 first_owned = start_stripe + rel;
		u64 last_owned;
		u32 count;
		u32 m_start_off, m_end_off;
		u64 first_phys_stripe, last_phys_stripe;

		if (first_owned > end_stripe)
			continue;

		count = (u32)((end_stripe - first_owned) / (u32)nm) + 1;
		last_owned = first_owned + (u64)(count - 1) * (u32)nm;

		m_start_off = (first_owned == start_stripe) ? start_off_global : 0;
		m_end_off   = (last_owned  == end_stripe)   ? end_off_global   : (stripe - 1);

		first_phys_stripe = first_owned / (u32)nm;
		last_phys_stripe  = last_owned  / (u32)nm;

		member_start_lba[m] = first_phys_stripe * stripe + m_start_off +
				      rc_volume_member_phys_offset[m];
		member_sectors[m]   = (u32)((last_phys_stripe - first_phys_stripe) * stripe
					    + m_end_off - m_start_off + 1);
		member_has_data[m]  = true;
		members_with_data++;
	}

	if (members_with_data == 0)
		return BLK_STS_IOERR;

	/* One DSM per involved member — reserve the SQE slots or requeue
	 * (C-4). */
	for (m = 0; m < nm; m++) {
		if (!member_has_data[m])
			continue;
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				if (member_has_data[m])
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	/* Discard has no DMA mapping — pdu->nents and pdu->ms_active stay zero
	 * from queue_rq's memset, so rc_volume_unmap_request_sg no-ops on
	 * completion.  Use member_idx=-1 + members_pending=N so the ISR's
	 * atomic_dec_and_test completes only on the last member's DSM. */
	pdu->member_idx = -1;
	pdu->member_mask = 0;
	for (m = 0; m < nm; m++)
		if (member_has_data[m])
			pdu->member_mask |= BIT(m);
	atomic_set(&pdu->members_pending, members_with_data);

	blk_mq_start_request(req);
	for (m = 0; m < nm; m++) {
		struct rc_nvme_sqe cmd;

		if (!member_has_data[m])
			continue;

		rc_volume_build_discard_sqe(&cmd, hctx->queue_num, m, tag,
					    member_start_lba[m],
					    member_sectors[m]);
		rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
				  &cmd);
	}

	return BLK_STS_OK;
}

/* Resync write-exclusion handshake for one application WRITE/DISCARD.
 *
 * Counts the request into the current write generation and checks the
 * published exclusion window (LOGICAL sector coordinates).  On success the
 * request is "counted": pdu->wgen/wgen_counted are published and the single
 * completion path (rc_volume_unmap_request_sg) will decrement it.  On
 * overlap the count is rolled back and false is returned — the caller must
 * release its SQ reservations / DMA mappings and return BLK_STS_DEV_RESOURCE
 * so blk-mq retries once the window has moved.
 *
 * MUST be the last action before blk_mq_start_request: nothing may fail
 * after the count is published.  See the protocol comment on the mirror
 * dispatch for why the generation is counted BEFORE the window is read. */
static bool rc_volume_resync_gate(struct rc_volume_pdu *pdu, sector_t pos,
				  u32 nr_sectors)
{
	int g;
	u64 wstart;

	/* Seqlock-style retry: the bucket we count into must be the
	 * generation live AT increment time.  Without the re-check, a task
	 * preempted between reading wgen and incrementing the counter for
	 * long enough to span a generation flip (2-bucket parity can even
	 * wrap back to the same g) would land its count in a bucket the
	 * resync thread already drained — letting a chunk copy proceed
	 * without waiting for this write. */
	for (;;) {
		g = atomic_read(&rc_volume_wgen) & 1;
		atomic_inc(&rc_volume_wgen_count[g]);
		smp_mb__after_atomic();
		if ((atomic_read(&rc_volume_wgen) & 1) == g)
			break;
		atomic_dec(&rc_volume_wgen_count[g]);
	}
	/* Pairs with the resync thread's smp_mb() between publishing the
	 * window and flipping the generation: having observed the flipped
	 * generation above, the window read below must not be satisfied by
	 * an older cached value (message-passing gap on non-TSO
	 * architectures). */
	smp_rmb();
	wstart = (u64)atomic64_read(&rc_volume_resync_window);
	if (wstart != RC_RESYNC_WINDOW_NONE &&
	    (u64)pos < wstart + RC_RESYNC_XFER_SECTORS &&
	    (u64)pos + nr_sectors > wstart) {
		atomic_dec(&rc_volume_wgen_count[g]);
		return false;
	}
	pdu->wgen = (u8)g;
	pdu->wgen_counted = true;
	return true;
}

/* RAID1: fan a WRITE or DISCARD out to every mirror member.
 *
 * Mirrors hold identical LBA spaces, so every member receives the SAME
 * logical range (offset by its own userdata base) — for WRITE that means
 * the same bio pages DMA'd to each member, for DISCARD the same DSM range.
 * The request completes when the LAST member ACKs (members_pending, same
 * contract as FLUSH and the RAID0 multi-stripe path); rc_volume_complete /
 * the ISR fast path unmap via ms_active.
 *
 * DATA-INTEGRITY INVARIANT — a mirror write must not complete until EVERY
 * member has it: completing on the first ACK would let an fsync pass while
 * one mirror still holds stale data, which a later degraded-mode read (or
 * the read balancer, TODAY) would happily serve.
 *
 * WRITE size is bounded by queue_limits.chunk_sectors (512 sectors for
 * RAID1) so one NVMe command per member always suffices, and segment count
 * by the RAID1 max_segments cap (= RC_VOLUME_MS_PAGES_PER_MEMBER, set in
 * rc_volume_create_disk) so the per-member sg arrays can't overflow — the
 * runtime guard below is a belt-and-braces backstop, not the contract.
 * DISCARD is NOT chunk-split by blk-mq, but a
 * mirror discard of any size is still one contiguous DSM range per member —
 * identity mapping keeps every logical range physically contiguous. */
static blk_status_t rc_volume_dispatch_mirror(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors,
		enum req_op op)
{
	const int nm = rc_volume_member_count;
	u32 tag = req->tag;
	unsigned long mask;
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_mirror: bad volume state (members=%d) — pos=%llu len=%u rejected\n",
			  nm, (u64)pos, nr_sectors);
		return BLK_STS_IOERR;
	}

	/* Snapshot the WRITE mask ONCE and use it for the whole dispatch —
	 * sg build, dma_map, SQE reservation, submit, and the pending
	 * count must all agree on the same member set even if a member
	 * fails concurrently.  A member dying after this snapshot is the
	 * in-flight-death case the drain path already handles; a member
	 * dying before it is simply skipped (degraded write to the
	 * survivor only, which is the whole point).  The write mask also
	 * covers a RESYNCING member: it takes every application write so
	 * already-copied regions stay current while the resync cursor
	 * sweeps. */
	mask = (unsigned long)(unsigned int)atomic_read(&rc_volume_write_mask) &
	       (BIT(nm) - 1);
	if (!mask) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_mirror: no live member — pos=%llu len=%u rejected\n",
			  (u64)pos, nr_sectors);
		return BLK_STS_IOERR;
	}

	if (op == REQ_OP_WRITE) {
		struct req_iterator iter;
		struct bio_vec bv;
		u32 used = 0;

		for (m = 0; m < nm; m++)
			sg_init_table(pdu->ms_sg[m], RC_VOLUME_MS_PAGES_PER_MEMBER);

		/* Same pages onto every member's scatterlist.  Separate sg
		 * arrays are mandatory even though the contents match:
		 * dma_map_sg writes each member's IOVAs into the entries,
		 * and every member owns a distinct IOMMU domain.
		 *
		 * Physically-contiguous bvecs must MERGE into one sg entry
		 * (see rc_volume_sg_append) — sub-page bvec runs from
		 * buffer-head writeback otherwise land non-first segments at
		 * in-page offsets that PRPs can't express.  The layout is
		 * identical across members, so the merge decision from
		 * member 0's array applies to all. */
		rq_for_each_segment(bv, req, iter) {
			u32 newused = rc_volume_sg_append(
					pdu->ms_sg[0], used,
					RC_VOLUME_MS_PAGES_PER_MEMBER,
					bv.bv_page, bv.bv_len, bv.bv_offset);

			if (!newused) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_mirror: sg overflow (>%u entries) — pos=%llu len=%u rejected\n",
					  RC_VOLUME_MS_PAGES_PER_MEMBER,
					  (u64)pos, nr_sectors);
				return BLK_STS_IOERR;
			}
			if (newused == used) {
				/* merged into the previous entry */
				for (m = 1; m < nm; m++)
					pdu->ms_sg[m][used - 1].length +=
						bv.bv_len;
			} else {
				for (m = 1; m < nm; m++)
					sg_set_page(&pdu->ms_sg[m][used],
						    bv.bv_page, bv.bv_len,
						    bv.bv_offset);
				used = newused;
			}
		}
		if (!used)
			return BLK_STS_IOERR;

		/* The mirror payload is identical for every target, so one
		 * whole-request bounce serves every member when the bvec layout
		 * is not PRP-expressible (same XFS log-recovery class as the
		 * RAID0 path).  Writes only; mirror READs use the single-member
		 * path. */
		if (!rc_volume_sg_prp_ready(pdu->ms_sg[0], used)) {
			u32 bytes = blk_rq_bytes(req);
			u32 n;
			void *buf;

			if (!bytes || bytes > RC_VOLUME_DATA_BYTES)
				return BLK_STS_IOERR;
			buf = rc_volume_bounce_alloc();
			if (!buf)
				return BLK_STS_RESOURCE;
			rc_volume_bounce_copy(req, buf, true);
			for (m = 0; m < nm; m++) {
				pdu->ms_bounce_off[m] = 0;
				pdu->ms_bounce_len[m] = bytes;
				if (rc_volume_bounce_sg(
					    pdu->ms_sg[m],
					    RC_VOLUME_MS_PAGES_PER_MEMBER,
					    buf, bytes, &n)) {
					rc_volume_bounce_free(buf);
					return BLK_STS_IOERR;
				}
			}
			pdu->ms_bounce = buf;
			used = n;
		}

		for (m = 0; m < nm; m++)
			sg_mark_end(&pdu->ms_sg[m][used - 1]);

		pdu->ms_active = true;
		for (m = 0; m < nm; m++) {
			struct device *dma_dev;
			int mapped;

			if (!(mask & BIT(m)))
				continue;	/* ms_nents stays 0 → skipped */
			dma_dev = &rc_volume_members[m]->pdev->dev;
			mapped = dma_map_sg(dma_dev, pdu->ms_sg[m], used,
					    DMA_TO_DEVICE);

			if (mapped == 0) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_mirror: dma_map_sg failed (member %d) — pos=%llu len=%u rejected\n",
					  m, (u64)pos, nr_sectors);
				pdu->ms_nents[m] = 0;
				for (--m; m >= 0; m--) {
					if (!pdu->ms_nents[m])
						continue;
					dma_dev = &rc_volume_members[m]->pdev->dev;
					dma_unmap_sg(dma_dev, pdu->ms_sg[m],
						     pdu->ms_nents[m],
						     DMA_TO_DEVICE);
					pdu->ms_nents[m] = 0;
				}
				pdu->ms_active = false;
				rc_volume_bounce_free(pdu->ms_bounce);
				pdu->ms_bounce = NULL;
				return BLK_STS_IOERR;
			}
			pdu->ms_nents[m] = mapped;
		}
	}
	/* DISCARD: no DMA mapping — nents/ms_active stay zero from queue_rq's
	 * memset, so unmap no-ops; the DSM range lives in the per-tag PRP
	 * buffer, one per (hctx, member, tag). */

	/* One SQE per LIVE member — reserve them all or requeue (C-4).
	 * Runs before blk_mq_start_request so BLK_STS_DEV_RESOURCE is a
	 * clean "try again" for blk-mq. */
	for (m = 0; m < nm; m++) {
		if (!(mask & BIT(m)))
			continue;
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				if (mask & BIT(m))
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	pdu->member_idx = -1;	/* multi-member — see member_mask */
	pdu->member_mask = (unsigned int)mask;
	atomic_set(&pdu->members_pending, hweight_long(mask));

	/* Build all SQEs before starting/submitting so a PRP-expressibility
	 * failure can still cleanly reject the request. */
	{
		struct rc_nvme_sqe cmds[RC_VOLUME_MAX_MEMBERS];

		for (m = 0; m < nm; m++) {
			u64 phys = (u64)pos + rc_volume_member_phys_offset[m];

			if (!(mask & BIT(m)))
				continue;
			if (op == REQ_OP_WRITE) {
				if (rc_volume_build_io_sqe(&cmds[m],
						hctx->queue_num, m, tag,
						RC_NVME_NVM_OP_WRITE,
						phys, (u16)(nr_sectors - 1),
						(req->cmd_flags & REQ_FUA) != 0,
						pdu->ms_sg[m],
						pdu->ms_nents[m])) {
					int u;

					for (u = 0; u < nm; u++)
						if (mask & BIT(u))
							rc_nvme_sq_unreserve(rc_volume_members[u]->ctx.nvme.io_queues[hctx->queue_num], 1);
					rc_volume_unmap_request_sg(pdu);
					return BLK_STS_IOERR;
				}
			} else {
				rc_volume_build_discard_sqe(&cmds[m],
						hctx->queue_num, m,
						tag, phys, nr_sectors);
			}
		}

		/* Resync write-exclusion — MUST be the last thing before
		 * blk_mq_start_request: nothing may fail after the count.
		 * On a window hit the request was never started, so hand it
		 * back to blk-mq (DEV_RESOURCE) for a retry once the resync
		 * chunk has moved on. */
		if (!rc_volume_resync_gate(pdu, pos, nr_sectors)) {
			for (m = 0; m < nm; m++)
				if (mask & BIT(m))
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}

		blk_mq_start_request(req);
		for (m = 0; m < nm; m++)
			if (mask & BIT(m))
				rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
						  &cmds[m]);
	}

	return BLK_STS_OK;
}

/* RAID10: stripe across the mirror pairs, then mirror each stripe to both
 * members of its pair.
 *
 * The logical space is striped over rc_volume_raid10_cols columns; column c
 * is the mirror pair {2c, 2c+1}.  Logical stripe N lives in column
 * (N mod cols) at physical stripe (N / cols) on BOTH copies.  A request may
 * span several stripes (chunk_sectors is 0), so the bvec is walked once and
 * split at stripe boundaries:
 *
 *   WRITE: every strip is fanned to both live copies of its column
 *          (degraded: to whichever copy survives).
 *   READ:  every strip is placed on ONE live copy, round-robin, so the pair
 *          shares the read load and a dead copy is never read.
 *
 * This is rc_volume_dispatch_mirror (same pages to both copies) fused with
 * rc_volume_dispatch_multi_stripe (per-stripe physical runs).  Returns
 * BLK_STS_DEV_RESOURCE (nothing started) on a full SQ, or BLK_STS_IOERR
 * before blk_mq_start_request on a build error. */
static blk_status_t rc_volume_dispatch_raid10(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors,
		enum req_op op)
{
	const u32 stripe = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	const u32 cols = rc_volume_raid10_cols;
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t cur_lba = pos;
	u32 sg_used[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 member_sectors[RC_VOLUME_MAX_MEMBERS] = {0};
	u64 member_start_lba[RC_VOLUME_MAX_MEMBERS] = {0};
	bool member_has_data[RC_VOLUME_MAX_MEMBERS] = {0};
	int members_with_data = 0;
	/* READ copy choice is per COLUMN, fixed for the whole request — see
	 * the READ branch below for why. */
	bool read_chosen[RC_VOLUME_MAX_MEMBERS] = { false };
	int read_member[RC_VOLUME_MAX_MEMBERS];
	unsigned int live =
		(unsigned int)atomic_read(&rc_volume_live_mask);
	unsigned int wmask =
		(unsigned int)atomic_read(&rc_volume_write_mask) &
		(unsigned int)(BIT(nm) - 1);
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || !stripe || !cols ||
	    (u32)nm != cols * 2) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_raid10: bad volume state (members=%d stripe=%u cols=%u) — pos=%llu len=%u rejected\n",
			  nm, stripe, cols, (u64)pos, nr_sectors);
		return BLK_STS_IOERR;
	}

	for (m = 0; m < nm; m++)
		sg_init_table(pdu->ms_sg[m], RC_VOLUME_MS_PAGES_PER_MEMBER);

	rq_for_each_segment(bv, req, iter) {
		u32 seg_len_sectors = bv.bv_len / 512;
		u32 seg_off_in_page = bv.bv_offset;
		u32 seg_remaining   = bv.bv_len;
		struct page *seg_page = bv.bv_page;

		while (seg_remaining) {
			sector_t stripe_idx = cur_lba / stripe;
			sector_t stripe_end = (stripe_idx + 1) * stripe;
			u32 sectors_to_boundary =
				(u32)(stripe_end - cur_lba);
			u32 chunk_sectors = min(seg_len_sectors,
						sectors_to_boundary);
			u32 chunk_bytes = chunk_sectors * 512u;
			u32 col = (u32)(stripe_idx % cols);
			int c0 = (int)(2u * col);
			int c1 = c0 + 1;
			int targets[2];
			int ntargets = 0;
			int t;

			if (chunk_bytes == 0 || chunk_bytes > seg_remaining) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_raid10: bvec split error (chunk_bytes=%u seg_remaining=%u bv_len=%u) — pos=%llu len=%u rejected\n",
					  chunk_bytes, seg_remaining, bv.bv_len,
					  (u64)pos, nr_sectors);
				return BLK_STS_IOERR;
			}

			if (op == REQ_OP_WRITE) {
				if (wmask & BIT(c0))
					targets[ntargets++] = c0;
				if (wmask & BIT(c1))
					targets[ntargets++] = c1;
			} else {
				/* Pick ONE copy per COLUMN for the whole request.
				 * The walk splits a stripe into several chunks (bvec
				 * boundaries, sub-page lengths), so re-rolling the
				 * round-robin per chunk scatters that stripe across
				 * both copies — observed in the QEMU rig as
				 * duplicated pages on (D0,D1,D1,D2,D2,…).
				 *
				 * Per-stripe is not enough either: a member
				 * command covers every stripe this request places
				 * on it as ONE physically contiguous run.  If a
				 * column's stripes alternate copies (A, B, A),
				 * member A's single command reads its stripes
				 * back-to-back and files the middle stripe's data
				 * into the third stripe's pages.  Fixing the copy
				 * per column makes each chosen member's stripes
				 * physically contiguous (column strides by the
				 * member count), which is what the command assumes. */
				if (!read_chosen[col]) {
					bool l0 = (live & BIT(c0)) != 0;
					bool l1 = (live & BIT(c1)) != 0;

					read_chosen[col] = true;
					if (l0 && l1)
						read_member[col] =
							(atomic_inc_return(&rc_volume_rr_next) & 1)
							? c1 : c0;
					else if (l0)
						read_member[col] = c0;
					else if (l1)
						read_member[col] = c1;
					else
						/* fatal catches first */
						read_member[col] = -1;
				}
				if (read_member[col] >= 0)
					targets[ntargets++] = read_member[col];
			}

			if (!ntargets) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_raid10: column %u has no usable copy (op=%u) — pos=%llu len=%u rejected\n",
					  col, op, (u64)pos, nr_sectors);
				return BLK_STS_IOERR;
			}

			for (t = 0; t < ntargets; t++) {
				m = targets[t];
				if (!member_has_data[m]) {
					member_start_lba[m] =
						rc_volume_phys_for_member(m,
								cur_lba);
					member_has_data[m] = true;
					members_with_data++;
				}
				sg_used[m] = rc_volume_sg_append(
						pdu->ms_sg[m], sg_used[m],
						RC_VOLUME_MS_PAGES_PER_MEMBER,
						seg_page, chunk_bytes,
						seg_off_in_page);
				if (!sg_used[m]) {
					rc_printk(RC_ERROR,
						  "rc_volume_dispatch_raid10: member %d sg overflow (>%u entries) — pos=%llu len=%u rejected\n",
						  m,
						  RC_VOLUME_MS_PAGES_PER_MEMBER,
						  (u64)pos, nr_sectors);
					return BLK_STS_IOERR;
				}
				member_sectors[m] += chunk_sectors;
			}

			seg_off_in_page += chunk_bytes;
			seg_remaining   -= chunk_bytes;
			seg_len_sectors -= chunk_sectors;
			cur_lba         += chunk_sectors;
		}
	}

	if (!members_with_data) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_raid10: no member with data — pos=%llu len=%u rejected\n",
			  (u64)pos, nr_sectors);
		return BLK_STS_IOERR;
	}

	/* A member's run starts mid-page when the request itself starts
	 * mid-page and its chunk boundary falls inside a page (XFS log
	 * recovery: a 256 KiB write beginning 58 sectors into a chunk), or
	 * when the bio joins runs with a page gap.  blk-mq's virt_boundary
	 * splitter only pushes LATER mid-page bvecs into a request of their
	 * own; a mid-page FIRST bvec keeps the whole run in one request, and
	 * the column that also owns a later chunk then gets a non-first SG
	 * entry PRPs cannot encode.  Rebuild every member's list over a
	 * bounce buffer in that case; the fast path stays zero-copy. */
	for (m = 0; m < nm; m++) {
		if (member_has_data[m] &&
		    !rc_volume_sg_prp_ready(pdu->ms_sg[m], sg_used[m]))
			break;
	}
	if (m < nm) {
		int bret = rc_volume_raid10_bounce_build(req, pdu, sg_used,
							member_sectors,
							member_has_data,
							cols, op);

		if (bret == -ENOMEM)
			return BLK_STS_RESOURCE;
		if (bret) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_raid10: bounce layout failed — pos=%llu len=%u rejected\n",
				  (u64)pos, nr_sectors);
			return BLK_STS_IOERR;
		}
	}

	for (m = 0; m < nm; m++) {
		if (!member_has_data[m])
			continue;
		sg_mark_end(&pdu->ms_sg[m][sg_used[m] - 1]);
	}

	pdu->ms_active = true;
	for (m = 0; m < nm; m++) {
		struct device *dma_dev;
		int mapped;

		if (!member_has_data[m]) {
			pdu->ms_nents[m] = 0;
			continue;
		}
		dma_dev = &rc_volume_members[m]->pdev->dev;
		mapped = dma_map_sg(dma_dev, pdu->ms_sg[m], sg_used[m],
				    rc_volume_dma_dir(op));
		if (mapped == 0) {
			rc_printk(RC_ERROR,
				  "rc_volume_dispatch_raid10: dma_map_sg failed (member %d, %u entries) — pos=%llu len=%u rejected\n",
				  m, sg_used[m], (u64)pos, nr_sectors);
			pdu->ms_nents[m] = 0;
			for (--m; m >= 0; m--) {
				if (!pdu->ms_nents[m])
					continue;
				dma_dev = &rc_volume_members[m]->pdev->dev;
				dma_unmap_sg(dma_dev, pdu->ms_sg[m],
					     pdu->ms_nents[m],
					     rc_volume_dma_dir(op));
				pdu->ms_nents[m] = 0;
			}
			pdu->ms_active = false;
			rc_volume_bounce_free(pdu->ms_bounce);
			pdu->ms_bounce = NULL;
			return BLK_STS_IOERR;
		}
		pdu->ms_nents[m] = mapped;
	}

	for (m = 0; m < nm; m++) {
		if (!pdu->ms_nents[m])
			continue;
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				if (pdu->ms_nents[m])
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	pdu->member_idx = -1;
	pdu->member_mask = 0;
	for (m = 0; m < nm; m++)
		if (pdu->ms_nents[m])
			pdu->member_mask |= BIT(m);
	atomic_set(&pdu->members_pending, members_with_data);

	{
		struct rc_nvme_sqe cmds[RC_VOLUME_MAX_MEMBERS];
		u32 tag = req->tag;

		for (m = 0; m < nm; m++) {
			if (!pdu->ms_nents[m])
				continue;
			if (rc_volume_build_io_sqe(&cmds[m], hctx->queue_num,
					m, tag,
					op == REQ_OP_WRITE ?
					  RC_NVME_NVM_OP_WRITE :
					  RC_NVME_NVM_OP_READ,
					member_start_lba[m],
					(u16)(member_sectors[m] - 1),
					(req->cmd_flags & REQ_FUA) != 0,
					pdu->ms_sg[m], pdu->ms_nents[m])) {
				int u;

				for (u = 0; u < nm; u++)
					if (pdu->ms_nents[u])
						rc_nvme_sq_unreserve(rc_volume_members[u]->ctx.nvme.io_queues[hctx->queue_num], 1);
				rc_volume_unmap_request_sg(pdu);
				return BLK_STS_IOERR;
			}
		}

		/* Resync write-exclusion — the last thing before start.  A
		 * RAID10 rebuild copies a column's stripes partner -> target;
		 * a write overlapping the chunk being copied must bounce until
		 * the window moves.  READs don't modify data, so they are
		 * ungated (and a single-stripe READ doesn't even come here). */
		if (op == REQ_OP_WRITE &&
		    !rc_volume_resync_gate(pdu, pos, nr_sectors)) {
			for (m = 0; m < nm; m++)
				if (pdu->ms_nents[m])
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}

		/* Marks the request as handed to hardware: the completion path
		 * only copies a bounced READ back into the bio's pages when
		 * this is set, so a pre-submit error can never leak bounce
		 * buffer contents into a failed READ. */
		pdu->io_started = true;
		blk_mq_start_request(req);
		for (m = 0; m < nm; m++)
			if (pdu->ms_nents[m])
				rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
						  &cmds[m]);
	}

	return BLK_STS_OK;
}

/* RAID10 DISCARD: one DSM per live member, covering the physical extent of
 * the logical stripes that member's column owns inside [pos, pos+nr).  Both
 * copies of a column own the SAME logical stripes, so they receive the same
 * range offset by their own user-data base.  This is the per-member range
 * collapse of rc_volume_dispatch_multi_stripe_discard with the member's
 * owner computed modulo the column count instead of the member count. */
static blk_status_t rc_volume_dispatch_raid10_discard(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors)
{
	const u32 stripe = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	const u32 cols = rc_volume_raid10_cols;
	const u64 start_stripe = pos / stripe;
	const u64 end_lba = pos + (u64)nr_sectors - 1;
	const u64 end_stripe = end_lba / stripe;
	const u32 start_off_global = (u32)(pos - start_stripe * stripe);
	const u32 end_off_global   = (u32)(end_lba - end_stripe * stripe);
	unsigned int wmask =
		(unsigned int)atomic_read(&rc_volume_write_mask) &
		(unsigned int)(BIT(nm) - 1);
	u64 member_start_lba[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 member_sectors[RC_VOLUME_MAX_MEMBERS] = {0};
	bool member_has_data[RC_VOLUME_MAX_MEMBERS] = {0};
	int members_with_data = 0;
	u32 tag = req->tag;
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || !stripe || !cols ||
	    (u32)nm != cols * 2)
		return BLK_STS_IOERR;

	for (m = 0; m < nm; m++) {
		u32 col = (u32)m / 2u;
		u32 start_owner = (u32)(start_stripe % cols);
		u32 rel = (col + cols - start_owner) % cols;
		u64 first_owned = start_stripe + rel;
		u64 last_owned;
		u32 count, m_start_off, m_end_off;
		u64 first_phys_stripe, last_phys_stripe;

		if (!(wmask & BIT(m)))
			continue;	/* dead copy; survivor covers the range */
		if (first_owned > end_stripe)
			continue;

		count = (u32)((end_stripe - first_owned) / cols) + 1;
		last_owned = first_owned + (u64)(count - 1) * cols;

		m_start_off = (first_owned == start_stripe) ? start_off_global : 0;
		m_end_off   = (last_owned  == end_stripe)   ? end_off_global   : (stripe - 1);

		first_phys_stripe = first_owned / cols;
		last_phys_stripe  = last_owned  / cols;

		member_start_lba[m] = first_phys_stripe * stripe + m_start_off +
				      rc_volume_member_phys_offset[m];
		member_sectors[m]   = (u32)((last_phys_stripe - first_phys_stripe) * stripe
					    + m_end_off - m_start_off + 1);
		member_has_data[m]  = true;
		members_with_data++;
	}

	if (!members_with_data)
		return BLK_STS_IOERR;

	for (m = 0; m < nm; m++) {
		if (!member_has_data[m])
			continue;
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				if (member_has_data[m])
					rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	pdu->member_idx = -1;
	pdu->member_mask = 0;
	for (m = 0; m < nm; m++)
		if (member_has_data[m])
			pdu->member_mask |= BIT(m);
	atomic_set(&pdu->members_pending, members_with_data);

	/* Resync write-exclusion — a DISCARD must not race the chunk copy
	 * either: it deallocates the very ranges the rebuild is copying.
	 * Bouncing to blk-mq and retrying after the window moves is safe.
	 * Last thing before blk_mq_start_request. */
	if (!rc_volume_resync_gate(pdu, pos, nr_sectors)) {
		for (m = 0; m < nm; m++)
			if (member_has_data[m])
				rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
		return BLK_STS_DEV_RESOURCE;
	}

	blk_mq_start_request(req);
	for (m = 0; m < nm; m++) {
		struct rc_nvme_sqe cmd;

		if (!member_has_data[m])
			continue;
		rc_volume_build_discard_sqe(&cmd, hctx->queue_num, m, tag,
					    member_start_lba[m],
					    member_sectors[m]);
		rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
				  &cmd);
	}

	return BLK_STS_OK;
}

static blk_status_t rc_volume_queue_rq(struct blk_mq_hw_ctx *hctx,
				       const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	sector_t pos = blk_rq_pos(req);
	unsigned int nr_sectors = blk_rq_sectors(req);
	enum req_op op = req_op(req);
	int member_idx;
	u64 phys_lba;
	u32 tag = req->tag;
	struct rc_nvme_sqe cmd;
	blk_status_t st;
	int i;

	pdu->op = op;
	pdu->sc_sct = 0;
	pdu->nents = 0;
	pdu->bounce = NULL;
	pdu->ms_bounce = NULL;
	pdu->ms_bounce_slots = 0;
	pdu->io_started = false;
	pdu->ms_active = false;
	pdu->parity_buf = -1;
	pdu->parity_locked = false;
	memset(pdu->ms_nents, 0, sizeof(pdu->ms_nents));
	atomic_set(&pdu->completed, 0);
	pdu->member_mask = 0;
	pdu->wgen_counted = false;
	pdu->eh_retries = 0;
	atomic_set(&pdu->acked, 0);
	atomic_set(&pdu->err_members, 0);
	pdu->hctx_idx = (u8)(hctx->queue_num < RC_VOLUME_MAX_HCTX ?
			     hctx->queue_num : 0xff);

	/* Fold freshly-set dead flags into the member state machine and
	 * check availability.  RAID0 fails on any dead member (every stripe
	 * needs every member); RAID1 keeps serving degraded from the
	 * surviving mirror; RAID10 keeps serving until a mirror PAIR loses
	 * both copies.  Only when the level can no longer serve is I/O
	 * fast-failed until reset/reload. */
	if (rc_volume_fatal()) {
		printk_ratelimited(KERN_ERR
			"rcraid: %s: failing I/O — %s (all I/O fails until reset/reload)\n",
			rc_volume_disk ? rc_volume_disk->disk_name : "?",
			rc_volume_raid_level == RC_LDT_RAID1 ?
				"no live mirror member remains" :
			rc_volume_raid_level == RC_LDT_RAID10 ?
				"a mirror pair lost both copies" :
			rc_volume_raid_level == RC_LDT_RAID5 ?
				"more than one RAID5 member is dead" :
			rc_volume_raid_level == RC_LDT_RAID6 ?
				"a RAID6 member is dead and dual-parity reconstruction is not implemented" :
				"a member controller is dead and RAID0 has no degraded mode");
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu)) {
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_STS_OK;
	}

	/* FLUSH fans out to every member: set the pending counter to the
	 * member count, build a single FLUSH SQE template (CID=tag, no
	 * data), and submit it on every member's I/O SQ.  Each member's
	 * ISR atomically decrements; the last one calls blk_mq_complete.
	 *
	 * DATA-INTEGRITY INVARIANT — do NOT "optimize" this to flush only a
	 * subset of members (e.g. skip members with no writes since the last
	 * flush, or short-circuit an "empty" flush).  A RAID0 stripe is spread
	 * across every member, so a REQ_OP_FLUSH must not complete until EVERY
	 * member has committed its volatile cache.  Any dirty-tracking scheme
	 * races the write submission path; getting it wrong ACKs an fsync while
	 * a member still holds unflushed data, silently corrupting the (root!)
	 * filesystem on power loss.  The fan-out already runs in parallel and
	 * completes on the last ack — that is the correct and optimal shape.
	 * fsync cost is NVMe flush latency (physics), not a driver defect. */
	if (op == REQ_OP_FLUSH) {
		/* Snapshot the live mask once for the whole fan-out (same
		 * contract as dispatch_mirror).  For RAID0 this is always
		 * every member — a non-live RAID0 member fast-failed above.
		 * For degraded RAID1 the flush goes to the survivor(s) only:
		 * the failed mirror is stale by definition, its cache state
		 * is irrelevant. */
		/* Write mask, not live mask: a RESYNCING member is receiving
		 * writes, so an fsync must flush ITS volatile cache too. */
		unsigned long fmask =
			(unsigned long)(unsigned int)atomic_read(&rc_volume_write_mask) &
			(BIT(rc_volume_member_count) - 1);

		if (!fmask) {
			printk_ratelimited(KERN_ERR
				"rcraid: %s: FLUSH rejected — no live member to flush\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?");
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu))
				blk_mq_end_request(req, BLK_STS_IOERR);
			return BLK_STS_OK;
		}

		/* Reserve one SQE per live member queue before starting; on
		 * a full SQ hand the request back to blk-mq for a retry
		 * instead of overrunning the ring (C-4). */
		for (i = 0; i < rc_volume_member_count; i++) {
			if (!(fmask & BIT(i)))
				continue;
			if (!rc_nvme_sq_reserve(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
				while (i-- > 0)
					if (fmask & BIT(i))
						rc_nvme_sq_unreserve(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num], 1);
				return BLK_STS_DEV_RESOURCE;
			}
		}

		atomic_set(&pdu->members_pending, hweight_long(fmask));
		pdu->member_idx = -1;
		pdu->member_mask = (unsigned int)fmask;
		rc_volume_build_flush_sqe(&cmd, tag);

		blk_mq_start_request(req);
		/* Use this hctx's NVMe queue on every member.  Completions
		 * land on the same hctx so blk_mq_tag_to_rq's tags[hctx_idx]
		 * lookup resolves to the right request. */
		for (i = 0; i < rc_volume_member_count; i++)
			if (fmask & BIT(i))
				rc_nvme_io_submit(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num],
						  &cmd);
		return BLK_STS_OK;
	}

	if (op != REQ_OP_READ && op != REQ_OP_WRITE && op != REQ_OP_DISCARD) {
		rc_printk(RC_ERROR,
			  "rc_volume_queue_rq: unsupported op=%u rejected\n", op);
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu)) {
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_STS_OK;
	}

	if (nr_sectors == 0 ||
	    pos + nr_sectors > get_capacity(rc_volume_disk) ||
	    (op != REQ_OP_DISCARD &&
	     nr_sectors > (RC_VOLUME_DATA_BYTES / 512))) {
		rc_printk(RC_ERROR,
			  "rc_volume_queue_rq: op=%u pos=%llu len=%u out of bounds (capacity=%llu max_io=%lu) rejected\n",
			  op, (u64)pos, nr_sectors,
			  (u64)get_capacity(rc_volume_disk),
			  RC_VOLUME_DATA_BYTES / 512);
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu)) {
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_STS_OK;
	}

	/* RAID 5 with one dead member is degraded and READ-ONLY.  Reads
	 * reconstruct the missing chunks from parity plus the survivors;
	 * writes and discards are refused with a reason because the RMW
	 * path would need the dead member's old bytes to regenerate parity. */
	if (rc_volume_raid_level == RC_LDT_RAID5) {
		unsigned int live =
			(unsigned int)atomic_read(&rc_volume_live_mask);
		unsigned int full =
			(unsigned int)(BIT(rc_volume_member_count) - 1);

		if (live != full) {
			if (op != REQ_OP_READ) {
				printk_ratelimited(KERN_ERR
					"rcraid: %s: op=%u pos=%llu len=%u refused — RAID 5 is DEGRADED (%u of %d members live); writes and discards stay disabled until the member is rebuilt\n",
					rc_volume_disk ? rc_volume_disk->disk_name : "?",
					op, (unsigned long long)pos, nr_sectors,
					hweight32(live), rc_volume_member_count);
				blk_mq_start_request(req);
				if (rc_volume_claim_completion(pdu))
					blk_mq_end_request(req, BLK_STS_IOERR);
				return BLK_STS_OK;
			}
			st = rc_volume_parity_degraded_read(req);
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu))
				blk_mq_end_request(req, st);
			return BLK_STS_OK;
		}
	}

	/* A RAID 5/6 read that crosses a chunk boundary now fans out one
	 * READ per data member (chunk_sectors is 0 in parity-write mode; a
	 * row is not a power of two, so it cannot be the split unit).
	 * Single-chunk reads fall through to the exact single-member path
	 * below.  The parity member is never read: a healthy array serves
	 * data from its data members alone. */
	if ((rc_volume_raid_level == RC_LDT_RAID5 ||
	     rc_volume_raid_level == RC_LDT_RAID6) && op == REQ_OP_READ &&
	    rc_volume_stripe_sectors && nr_sectors > 0 &&
	    (pos / rc_volume_stripe_sectors) !=
		((pos + nr_sectors - 1) / rc_volume_stripe_sectors)) {
		st = rc_volume_dispatch_multi_stripe(hctx, req, pdu, pos,
						     nr_sectors, op, true);
		if (st == BLK_STS_RESOURCE || st == BLK_STS_DEV_RESOURCE)
			return st;
		if (st != BLK_STS_OK) {
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu))
				blk_mq_end_request(req, st);
		}
		return BLK_STS_OK;
	}

	/* RAID5/6: reads for both; writes only for RAID 5.  A complete row
	 * carries every data chunk, so P is a plain XOR of the request's own
	 * bytes (async fan-out below).  A partial row takes the blocking
	 * read-modify-write path: read the row, overlay the request, then
	 * rewrite the touched chunks and parity.  Discards are refused because
	 * they would leave parity stale; RAID 6 has no write path at all.
	 * Everything refused here is refused with a reason and an error —
	 * never silently accepted. */
	if ((rc_volume_raid_level == RC_LDT_RAID5 ||
	     rc_volume_raid_level == RC_LDT_RAID6) && op != REQ_OP_READ) {
		if (op == REQ_OP_WRITE &&
		    rc_volume_parity_full_row(pos, nr_sectors)) {
			st = rc_volume_dispatch_parity_write(hctx, req, pdu,
							     pos, nr_sectors);
			if (st == BLK_STS_RESOURCE ||
			    st == BLK_STS_DEV_RESOURCE)
				return st;
			if (st != BLK_STS_OK) {
				blk_mq_start_request(req);
				if (rc_volume_claim_completion(pdu))
					blk_mq_end_request(req, st);
			}
			return BLK_STS_OK;
		}

		if (op == REQ_OP_WRITE && rc_volume_parity_writes &&
		    rc_volume_parity_write_ready()) {
			/* Partial row: read the row, overlay this request,
			 * regenerate P, rewrite the touched chunks + parity.
			 * Blocking, so the tagset is BLK_MQ_F_BLOCKING for
			 * this mode. */
			st = rc_volume_parity_rmw_write(req, pos, nr_sectors);
			blk_mq_start_request(req);
			blk_mq_end_request(req, st);
			return BLK_STS_OK;
		}

		if (rc_volume_raid_level == RC_LDT_RAID6)
			printk_ratelimited(KERN_ERR
				"rcraid: %s: op=%u pos=%llu len=%u rejected — RAID 6 writes are not implemented (dual-parity P/Q); volume is READ-ONLY (issue #6)\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?",
				op, (unsigned long long)pos, nr_sectors);
		else
			printk_ratelimited(KERN_ERR
				"rcraid: %s: op=%u pos=%llu len=%u rejected — RAID 5 discards are refused (they would leave parity stale; issue #6)\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?",
				op, (unsigned long long)pos, nr_sectors);
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu))
			blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	/* RAID10: stripe over mirror pairs, writing BOTH copies of each
	 * column.  Handled BEFORE the RAID0 multi-stripe/stripe-math paths:
	 *   - every WRITE and DISCARD must mirror to both copies;
	 *   - a multi-stripe READ needs the pair-aware fan-out too, because
	 *     chunk_sectors is 0 and a read may span several columns.
	 * A single-stripe READ falls through to the normal single-member path
	 * below; rc_volume_map_lba picks a live copy of the pair for it. */
	if (rc_volume_raid_level == RC_LDT_RAID10) {
		bool multi = rc_volume_stripe_sectors && nr_sectors > 0 &&
			(pos / rc_volume_stripe_sectors) !=
			((pos + nr_sectors - 1) / rc_volume_stripe_sectors);
		bool pair_dispatch = op == REQ_OP_DISCARD ||
				     op == REQ_OP_WRITE ||
				     (op == REQ_OP_READ && multi);

		if (pair_dispatch) {
			if (op == REQ_OP_DISCARD)
				st = rc_volume_dispatch_raid10_discard(
						hctx, req, pdu, pos,
						nr_sectors);
			else
				st = rc_volume_dispatch_raid10(hctx, req, pdu,
							       pos,
							       nr_sectors, op);
			if (st == BLK_STS_RESOURCE ||
			    st == BLK_STS_DEV_RESOURCE) {
				/* SQ full or a bounce buffer was unavailable
				 * — hand back to blk-mq untouched for a retry
				 * (request never started). */
				return st;
			}
			if (st != BLK_STS_OK) {
				blk_mq_start_request(req);
				if (rc_volume_claim_completion(pdu)) {
					blk_mq_end_request(req, st);
				}
			}
			return BLK_STS_OK;
		}
		/* single-stripe READ: fall through to the single-member path */
	}

	/* RAID1 WRITE/DISCARD: every mirror must receive the data, so these
	 * never take the single-member or multi-stripe paths.  Checked BEFORE
	 * the multi-stripe branches because blk-mq does not split DISCARD at
	 * chunk_sectors — a large mirror discard would otherwise wander into
	 * the RAID0 stripe-math discard fan-out.  READs fall through: the
	 * normal single-member path works as-is once rc_volume_map_lba picks
	 * a mirror. */
	if (rc_volume_raid_level == RC_LDT_RAID1 &&
	    (op == REQ_OP_WRITE || op == REQ_OP_DISCARD)) {
		st = rc_volume_dispatch_mirror(hctx, req, pdu, pos,
					       nr_sectors, op);
		if (st == BLK_STS_RESOURCE || st == BLK_STS_DEV_RESOURCE) {
			/* SQ full or a bounce buffer was unavailable —
			 * hand back to blk-mq untouched for a retry
			 * (the request was never started). */
			return st;
		}
		if (st != BLK_STS_OK) {
			/* Errors return before dispatch_mirror's internal
			 * blk_mq_start_request — start before ending. */
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu)) {
				blk_mq_end_request(req, st);
			}
		}
		return BLK_STS_OK;
	}

	/* Multi-stripe READ/WRITE: dispatch one NVMe cmd per affected member
	 * instead of the 1-cmd-per-stripe split blk-mq would otherwise force.
	 * For a 1 MiB write on a 2-member RAID0 with 256 KiB stripe, this
	 * cuts 4 cmds → 2 cmds and halves the per-byte queue_rq /
	 * dma_map / completion overhead.
	 *
	 * RAID1 READs are the exception: every mirror holds the whole logical
	 * range, so one member can serve a request that spans stripes.  The
	 * fan-out below assigns chunks by stripe modulo member count, which is
	 * wrong for mirrors (both members map every stripe) and made a read
	 * larger than one stripe fail with BLK_STS_IOERR.  Let those reads
	 * fall through to the single-member path, which maps them through
	 * rc_volume_map_lba's live-mirror selection. */
	if ((op == REQ_OP_READ || op == REQ_OP_WRITE) &&
	    rc_volume_stripe_sectors &&
	    nr_sectors > 0 &&
	    !(op == REQ_OP_READ && rc_volume_raid_level == RC_LDT_RAID1) &&
	    (pos / rc_volume_stripe_sectors) !=
		((pos + nr_sectors - 1) / rc_volume_stripe_sectors)) {
		blk_status_t st = rc_volume_dispatch_multi_stripe(hctx, req,
								  pdu, pos,
								  nr_sectors,
								  op, false);
		if (st == BLK_STS_RESOURCE || st == BLK_STS_DEV_RESOURCE) {
			/* SQ full or bounce unavailable — requeue;
			 * request never started. */
			return st;
		}
		if (st != BLK_STS_OK) {
			/* dispatch_multi_stripe returns errors from paths
			 * BEFORE its internal blk_mq_start_request, so we
			 * have to start the request before ending it —
			 * blk-mq requires every request to be started exactly
			 * once between queue_rq and end. */
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu)) {
				blk_mq_end_request(req, st);
			}
		}
		return BLK_STS_OK;
	}

	/* Multi-stripe DISCARD: one DSM per AFFECTED MEMBER (not per stripe).
	 * Each member's owned stripes form a single contiguous phys extent on
	 * its drive, so the fanout is bounded at nm regardless of how many
	 * stripes the request covers.  Lets us advertise a much larger
	 * max_hw_discard_sectors and turn mkfs.xfs / fstrim into a
	 * seconds-scale operation. */
	if (op == REQ_OP_DISCARD &&
	    rc_volume_stripe_sectors &&
	    nr_sectors > 0 &&
	    (pos / rc_volume_stripe_sectors) !=
		((pos + nr_sectors - 1) / rc_volume_stripe_sectors)) {
		blk_status_t st = rc_volume_dispatch_multi_stripe_discard(
				hctx, req, pdu, pos, nr_sectors);
		if (st == BLK_STS_RESOURCE || st == BLK_STS_DEV_RESOURCE) {
			/* SQ full — requeue; request never started. */
			return st;
		}
		if (st != BLK_STS_OK) {
			blk_mq_start_request(req);
			if (rc_volume_claim_completion(pdu)) {
				blk_mq_end_request(req, st);
			}
		}
		return BLK_STS_OK;
	}

	rc_volume_map_lba(pos, &member_idx, &phys_lba);
	/* rc_volume_fatal() ran lock-free at the top of this function; the
	 * last live member can die (or be hot-removed, NULLing its slot)
	 * between that check and the map.  map_lba's mask==0 fallback then
	 * returns member 0, which may be absent — dereferencing it below
	 * would oops.  Fail the request cleanly instead. */
	if (member_idx < 0 || member_idx >= RC_VOLUME_MAX_MEMBERS ||
	    !rc_volume_members[member_idx]) {
		printk_ratelimited(KERN_ERR
			"rcraid: %s: op=%u lba=%llu rejected — mapped member %d is absent (member died mid-dispatch)\n",
			rc_volume_disk ? rc_volume_disk->disk_name : "?",
			op, (unsigned long long)pos, member_idx);
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu))
			blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}
	pdu->member_idx = member_idx;
	pdu->member_mask = BIT(member_idx);
	atomic_set(&pdu->members_pending, 1);

	if (op == REQ_OP_DISCARD) {
		if (!rc_nvme_sq_reserve(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			return BLK_STS_DEV_RESOURCE;
		}
		rc_volume_build_discard_sqe(&cmd, hctx->queue_num, member_idx, tag,
					    phys_lba, nr_sectors);
		blk_mq_start_request(req);
		rc_nvme_io_submit(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num],
				  &cmd);
		return BLK_STS_OK;
	}

	/* READ/WRITE: dma_map_sg the request's bvecs onto the target member,
	 * then build the SQE with PRPs derived from the mapped scatterlist. */
	st = rc_volume_map_request_sg(req, pdu, member_idx);
	if (st == BLK_STS_RESOURCE)
		return st;   /* bounce pool drained — never started */
	if (st != BLK_STS_OK) {
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu)) {
			blk_mq_end_request(req, st);
		}
		return BLK_STS_OK;
	}

	if (rc_volume_build_io_sqe(&cmd, hctx->queue_num, member_idx, tag,
			       op == REQ_OP_WRITE ? RC_NVME_NVM_OP_WRITE
						  : RC_NVME_NVM_OP_READ,
			       phys_lba, (u16)(nr_sectors - 1),
			       (req->cmd_flags & REQ_FUA) != 0,
			       pdu->sg, pdu->nents)) {
		blk_mq_start_request(req);
		if (rc_volume_claim_completion(pdu)) {
			rc_volume_unmap_request_sg(pdu);
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_STS_OK;
	}

	/* Reserve the SQE slot (C-4); requeue on a full SQ.  Nothing can
	 * fail between here and the doorbell, so the reservation cannot
	 * leak. */
	if (!rc_nvme_sq_reserve(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
		rc_volume_unmap_request_sg(pdu);
		return BLK_STS_DEV_RESOURCE;
	}

	/* Must call blk_mq_start_request BEFORE submitting — otherwise the
	 * ISR could complete the request before blk-mq considers it started. */
	pdu->io_started = true;
	blk_mq_start_request(req);
	rc_nvme_io_submit(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num],
			  &cmd);

	return BLK_STS_OK;
}

/* Tagset .complete callback — runs in softirq after blk_mq_complete_request
 * is called from the ISR (or from the drain path).  dma_unmap_sg releases
 * the IOMMU mapping for the bio's user pages; the data is already in place
 * (hardware DMA'd to/from the user pages directly).  Ends the request with
 * the recorded status. */
static void rc_volume_complete(struct request *req)
{
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	blk_status_t st;

	rc_volume_unmap_request_sg(pdu);

	/* Degraded-RAID1 policy (partial mirror failure = success + degrade)
	 * lives in rc_volume_finish_status, shared with the ISR fast path. */
	st = rc_volume_finish_status(req);
	if (st == BLK_STS_OK) {
		blk_mq_end_request(req, BLK_STS_OK);
		return;
	}

	if (pdu->sc_sct) {
		/* pdu->sc_sct holds CQE.status >> 1, so:
		 *   bits  7:0  = SC (Status Code)
		 *   bits 10:8  = SCT (Status Code Type)
		 *   bit  13    = M (More — extended info available via Get Log)
		 *   bit  14    = DNR (Do Not Retry) */
		u8   sc   = pdu->sc_sct & 0xff;
		u8   sct  = (pdu->sc_sct >> 8) & 0x7;
		bool more = (pdu->sc_sct >> 13) & 1;
		bool dnr  = (pdu->sc_sct >> 14) & 1;

		if (pdu->sc_sct == RC_VOLUME_SC_DEAD) {
			printk_ratelimited(KERN_ERR
				"rcraid: %s failed: controller dead (op=%u, lba=%llu, nsec=%u)\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?",
				pdu->op,
				(unsigned long long)blk_rq_pos(req),
				blk_rq_sectors(req));
		} else {
			printk_ratelimited(KERN_WARNING
				"rcraid: %s failed: NVMe SCT=0x%x SC=0x%02x%s%s (op=%u, lba=%llu, nsec=%u)\n",
				rc_volume_disk ? rc_volume_disk->disk_name : "?",
				sct, sc,
				dnr  ? " DNR"  : "",
				more ? " More" : "",
				pdu->op,
				(unsigned long long)blk_rq_pos(req),
				blk_rq_sectors(req));
		}
		blk_mq_end_request(req, BLK_STS_IOERR);
		return;
	}

	blk_mq_end_request(req, BLK_STS_OK);
}

struct rc_volume_drain_ctx {
	unsigned int dead_mask;	/* bit i set => member i is dead */
};

/* H-1: disable a dead member's controller (clear CC.EN, bounded wait for
 * CSTS.RDY=0) BEFORE any in-flight request's DMA mappings are reclaimed.
 * The .timeout path only flags `dead`; without this, drain would
 * dma_unmap_sg + end requests whose commands are still live in hardware —
 * a late device DMA then hits unmapped IOVAs (IOMMU fault) or, without an
 * IOMMU, recycled pages (silent corruption of unrelated memory).  A later
 * auto/manual reset re-enables the controller from the EN=0 state.
 * Process context only (sleeps). */
static void rc_nvme_disable_dead_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	unsigned long deadline;
	u32 csts, cc;

	if (!base)
		return;
	csts = readl(base + RC_NVME_REG_CSTS);
	if (csts == 0xffffffff)
		return;		/* device gone — it can't DMA anything */

	cc = readl(base + RC_NVME_REG_CC);
	if (cc & RC_NVME_CC_EN)
		writel(cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);

	/* Bounded wait — the controller is already misbehaving, so don't
	 * trust CAP.TO to be honoured.  3 s covers spec-compliant disables;
	 * on expiry we proceed anyway (nothing better to do) but say so. */
	deadline = jiffies + msecs_to_jiffies(3000);
	for (;;) {
		csts = readl(base + RC_NVME_REG_CSTS);
		if (csts == 0xffffffff || !(csts & RC_NVME_CSTS_RDY))
			return;
		if (time_after(jiffies, deadline)) {
			rc_printk(RC_ERROR,
				  "rc_nvme_disable_dead_controller: %s CSTS.RDY stuck at 1 (CSTS=0x%08x) — reclaiming DMA anyway\n",
				  pci_name(adapter->pdev), csts);
			return;
		}
		msleep(1);
	}
}

/* Does this request need `member` to complete?  Every dispatch path
 * records the members it actually submitted to in pdu->member_mask, so
 * involvement is an exact bit test — no more "member_idx==-1 means
 * everyone" conservatism.  A zero mask (request failed before any
 * submission) involves nobody. */
static bool rc_volume_req_involves(const struct rc_volume_pdu *pdu, int member)
{
	return (pdu->member_mask & BIT(member)) != 0;
}

/* blk_mq_tagset_busy_iter callback.  For each in-flight request, decide
 * whether it can no longer make progress (a member it needs is dead) and
 * complete it with the dead-controller sentinel if so.  blk-mq makes
 * blk_mq_complete_request atomic against the ISR's competing call, so a
 * naturally-arriving CQE during this iteration races safely. */
static bool rc_volume_drain_iter(struct request *req, void *priv)
{
	struct rc_volume_drain_ctx *ctx = priv;
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	unsigned long involved = pdu->member_mask & ctx->dead_mask;
	bool last = false;
	int i;

	if (!involved)
		return true;

	/* Synthesize a completion for each dead involved member whose real
	 * CQE was never consumed.  The acked bitmap makes the
	 * members_pending decrement exactly-once per (request, member)
	 * regardless of how this interleaves with the ISR: whoever sets the
	 * bit first owns that member's decrement, and a late CQE from a
	 * revived controller (queues are zeroed on reset, but belt and
	 * braces) finds the bit already set and backs off.
	 *
	 * A request whose OTHER members are alive is NOT force-failed
	 * anymore: their ACKs (or these synthesized ones) drive
	 * members_pending to zero, and rc_volume_finish_status turns a
	 * partial mirror failure into a degraded success. */
	for_each_set_bit(i, &involved, RC_VOLUME_MAX_MEMBERS) {
		int prev = atomic_fetch_or(BIT(i), &pdu->acked);

		if (prev & BIT(i))
			continue;	/* CQE already consumed for this member */
		atomic_or(BIT(i), &pdu->err_members);
		if (!READ_ONCE(pdu->sc_sct))
			WRITE_ONCE(pdu->sc_sct, RC_VOLUME_SC_DEAD);
		if (atomic_dec_and_test(&pdu->members_pending))
			last = true;
	}

	if (last && !blk_mq_request_completed(req) &&
	    rc_volume_claim_completion(pdu)) {
		/* The claim is what makes this safe against the ISR's
		 * direct-end fast path, which bypasses the
		 * blk_mq_complete_request state machine entirely — the
		 * completed-state check alone leaves a double-completion
		 * window there. */
		blk_mq_complete_request(req);
	}
	return true;
}

/* Schedule the per-adapter auto-reset work for one member, but only if
 * the adapter is actually dead and auto-reset hasn't been latched off
 * by a prior failed attempt.  schedule_work is a no-op if the work is
 * already queued, so multiple .timeout invocations during one death
 * episode coalesce into a single reset attempt. */
static void rc_nvme_schedule_auto_reset(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;

	if (READ_ONCE(nvme->dead) && !READ_ONCE(nvme->auto_reset_disabled))
		schedule_work(&nvme->auto_reset_work);
}

/* Walk the request's involved members and schedule auto-reset for each
 * that is currently dead.  Called from .timeout after marking adapters
 * dead and draining in-flight. */
static void rc_volume_schedule_auto_reset_for_req(struct rc_volume_pdu *pdu)
{
	int i;

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++)
		if (rc_volume_req_involves(pdu, i) && rc_volume_members[i])
			rc_nvme_schedule_auto_reset(rc_volume_members[i]);
}

/* Build the dead-member mask and run the drain iterator.  No-op if no
 * member is currently flagged dead or the disk hasn't been created yet. */
static void rc_volume_drain_dead(void)
{
	struct rc_volume_drain_ctx ctx = {};
	int i;

	if (!rc_volume_disk)
		return;
	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		struct rc_adapter *m = rc_volume_members[i];

		/* A NULL slot is a hot-removed member — gone, and by
		 * definition unable to complete anything: treat as dead so
		 * in-flight requests still waiting on it get synthesized
		 * completions. */
		if (!m || READ_ONCE(m->ctx.nvme.dead))
			ctx.dead_mask |= BIT(i);
	}
	if (!ctx.dead_mask)
		return;
	/* H-1: quiesce the dead controllers' DMA engines BEFORE ending
	 * requests — ending a request unmaps its scatterlist, and a still-
	 * enabled controller may still be working the command. */
	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++)
		if ((ctx.dead_mask & BIT(i)) && rc_volume_members[i])
			rc_nvme_disable_dead_controller(rc_volume_members[i]);
	blk_mq_tagset_busy_iter(&rc_volume_tagset, rc_volume_drain_iter, &ctx);
}

/* Deferred degrade handler (scheduled by rc_volume_member_mark_failed from
 * IRQ/hot-path context).  Drains in-flight requests waiting on the failed
 * member(s) — without this they'd sit until the 30 s blk-mq timeout — and
 * kicks auto-reset for any dead controller so a transient wedge can come
 * back (as NEEDS_RESYNC, not silently live). */
static void rc_volume_degrade_fn(struct work_struct *w)
{
	int i;

	rc_volume_drain_dead();
	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		struct rc_adapter *m = rc_volume_members[i];

		if (m && READ_ONCE(m->ctx.nvme.dead))
			rc_nvme_schedule_auto_reset(m);
	}
}

/* blk-mq .timeout callback.  Fires at the default 30 s when a request's
 * CQE hasn't landed.
 *
 *   1. If completion raced in just before us, do nothing.
 *   2. Probe CSTS on the involved member(s).  CSTS.CFS or CSTS=~0 means
 *      the controller is gone — drain everyone touching that adapter and
 *      end this request with BLK_STS_IOERR.
 *   3. Otherwise issue NVMe Abort (best-effort) and re-check.
 *   4. With no controller-reset path we still can't safely recycle this
 *      CID: a late CQE from the controller would land against a request
 *      that has since been freed and the tag reused, corrupting unrelated
 *      I/O.  Mark the involved adapter(s) dead, drain, and end the
 *      request with BLK_STS_TIMEOUT.  One stuck command disables the
 *      volume; this is intentional until reset/recovery lands. */
static enum blk_eh_timer_return rc_volume_timeout(struct request *req)
{
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	bool any_dead = false;
	unsigned long waiting;
	int i;

	if (blk_mq_request_completed(req))
		return BLK_EH_DONE;

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		if (!rc_volume_req_involves(pdu, i))
			continue;
		/* A NULL slot (hot-removed member) can never complete. */
		if (!rc_volume_members[i] ||
		    rc_nvme_check_dead(rc_volume_members[i]))
			any_dead = true;
	}

	if (any_dead) {
		rc_printk(RC_ERROR,
			  "rc_volume_timeout: tag=%u op=%u: member dead — draining in-flight\n",
			  req->tag, pdu->op);
		rc_volume_drain_dead();
		rc_volume_schedule_auto_reset_for_req(pdu);
		if (blk_mq_request_completed(req))
			return BLK_EH_DONE;
		/* Members still owed a completion after the drain LOOK alive
		 * (the drain synthesized every confirmed-dead one) — their
		 * CQEs are probably coming.  Give them the timer back instead
		 * of force-failing a request that will complete degraded-OK —
		 * but only a bounded number of times: a controller can drop a
		 * command without ever asserting CSTS.CFS, and re-arming
		 * forever would hang the request with no recovery.  After two
		 * full extra timeout periods the silent member is treated as
		 * dead and the drain finishes the request. */
		waiting = pdu->member_mask &
			  ~(unsigned long)(unsigned int)atomic_read(&pdu->acked);
		if (waiting) {
			if (pdu->eh_retries < 2) {
				pdu->eh_retries++;
				return BLK_EH_RESET_TIMER;
			}
			rc_printk(RC_ERROR,
				  "rc_volume_timeout: tag=%u members 0x%lx never completed across %u timeout periods without asserting CFS — declaring dead\n",
				  req->tag, waiting, pdu->eh_retries + 1u);
			for_each_set_bit(i, &waiting, RC_VOLUME_MAX_MEMBERS)
				if (rc_volume_members[i])
					WRITE_ONCE(rc_volume_members[i]->ctx.nvme.dead,
						   true);
			rc_volume_drain_dead();
			rc_volume_schedule_auto_reset_for_req(pdu);
			if (blk_mq_request_completed(req))
				return BLK_EH_DONE;
		}
		if (rc_volume_claim_completion(pdu)) {
			/* Safety net — drain should have completed it. */
			rc_volume_unmap_request_sg(pdu);
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_EH_DONE;
	}

	rc_printk(RC_WARN,
		  "rc_volume_timeout: tag=%u op=%u: issuing NVMe Abort\n",
		  req->tag, pdu->op);
	/* Abort must name the SQ the command was actually submitted on:
	 * hctx i maps to NVMe qid i+1 on every member.  Always aborting
	 * SQ 1 could never abort hctx>0 requests and could kill an
	 * unrelated live command on queue 1 that shares the tag value. */
	{
		u16 sqid = (pdu->hctx_idx < RC_VOLUME_MAX_HCTX)
				? (u16)(pdu->hctx_idx + 1) : RC_NVME_IO_QID;

		for (i = 0; i < rc_volume_member_count &&
			    i < RC_VOLUME_MAX_MEMBERS; i++)
			if (rc_volume_req_involves(pdu, i) &&
			    rc_volume_members[i])
				(void)rc_nvme_abort(rc_volume_members[i],
						    sqid, (u16)req->tag);
	}

	if (blk_mq_request_completed(req))
		return BLK_EH_DONE;

	/* Only the members that never ACKed this request are stuck — a
	 * member whose CQE already arrived is demonstrably alive, and on a
	 * RAID1 fan-out killing BOTH mirrors for one stuck command would
	 * throw away the availability degraded mode exists to provide. */
	{
		unsigned long stuck = pdu->member_mask &
			~(unsigned long)(unsigned int)atomic_read(&pdu->acked);

		if (!stuck)
			stuck = pdu->member_mask;	/* defensive */
		for_each_set_bit(i, &stuck, RC_VOLUME_MAX_MEMBERS)
			if (rc_volume_members[i])
				WRITE_ONCE(rc_volume_members[i]->ctx.nvme.dead,
					   true);
	}
	rc_volume_drain_dead();
	rc_volume_schedule_auto_reset_for_req(pdu);
	if (!blk_mq_request_completed(req) &&
	    rc_volume_claim_completion(pdu)) {
		rc_volume_unmap_request_sg(pdu);
		blk_mq_end_request(req, BLK_STS_TIMEOUT);
	}
	return BLK_EH_DONE;
}

/* Map CPUs to hctxs using member 0's MSI-X vector affinity.  Vector 0
 * is admin (skipped via offset=1); vectors 1..N are the I/O queues,
 * already affinitized by the kernel to disjoint CPU sets during
 * pci_alloc_irq_vectors_affinity.  Mirroring blk-mq's hctx-to-CPU
 * mapping onto that affinity means each request's submission CPU
 * matches the CPU its completion will land on.
 *
 * We use member 0 as the affinity source for the whole tagset.  In
 * practice all members get the same affinity layout because each
 * controller goes through the same pci_alloc_irq_vectors_affinity
 * call, and CPU set assignment for the i-th vector is the same
 * regardless of which device requested it.  If they ever diverge,
 * the worst case is cross-CPU completion landings (loss of cache
 * locality), not correctness. */
static void rc_volume_map_queues(struct blk_mq_tag_set *set)
{
	/* member 0 can be NULL after a RAID1 hot-removal; map_queues can
	 * re-run on CPU hotplug, so fall back to the generic mapping then
	 * (worst case is lost completion locality, not correctness). */
	if (rc_volume_member_count > 0 && rc_volume_members[0]) {
		struct device *dev = &rc_volume_members[0]->pdev->dev;
		blk_mq_map_hw_queues(&set->map[HCTX_TYPE_DEFAULT], dev, 1);
	} else {
		blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);
	}
}

static const struct blk_mq_ops rc_volume_mq_ops = {
	.queue_rq   = rc_volume_queue_rq,
	.complete   = rc_volume_complete,
	.timeout    = rc_volume_timeout,
	.map_queues = rc_volume_map_queues,
};

static const struct block_device_operations rc_volume_bops = {
	.owner = THIS_MODULE,
};

/* Allocate (or free) one member's slice of the per-tag PRP/DSM pool.
 * Factored out of create_disk so member re-add can rebuild the slice a
 * hot-removal freed: the buffers hold per-member IOVAs, so they can only
 * exist while the owning device does. */
static int rc_volume_alloc_member_dma(int slot, u32 nr_hw)
{
	struct device *dev = &rc_volume_members[slot]->pdev->dev;
	u32 h, t;

	for (h = 0; h < nr_hw; h++) {
		for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
			rc_volume_prp_va[h][slot][t] =
				dma_alloc_coherent(dev, PAGE_SIZE,
						   &rc_volume_prp_pa[h][slot][t],
						   GFP_KERNEL);
			if (!rc_volume_prp_va[h][slot][t])
				return -ENOMEM;
		}
	}
	return 0;
}

static void rc_volume_free_member_dma(int slot, struct device *dev)
{
	u32 h, t;

	for (h = 0; h < RC_VOLUME_MAX_HCTX; h++) {
		for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
			if (!rc_volume_prp_va[h][slot][t])
				continue;
			dma_free_coherent(dev, PAGE_SIZE,
					  rc_volume_prp_va[h][slot][t],
					  rc_volume_prp_pa[h][slot][t]);
			rc_volume_prp_va[h][slot][t] = NULL;
			rc_volume_prp_pa[h][slot][t] = 0;
		}
	}
}

/* Allocate the gendisk + tagset, set up sizes, expose /dev/rcraid0.
 * Tolerates NULL (absent) slots for degraded RAID1 assembly — every
 * per-member computation runs over the PRESENT members. */
/*
 * Persist a DELETE: rewrite every member's active config generation without
 * the array's LogicalDevice record, then re-commit it.  The ring slot and
 * generation length are reused, and the commit block is rewritten only after
 * the generation body is on media, so a torn write leaves the old commit
 * timestamp pointing at a generation whose header no longer matches it — the
 * parser rejects that generation instead of assembling half a deleted array.
 *
 * Every member carries its own copy of the generation; leaving one behind
 * would let the next assembly re-form the array from it, so all members must
 * be rewritten before the volume is torn down.  Returns 0 only when each
 * member's rewrite was written, flushed and read back.
 */
int rc_volume_delete_array(void)
{
	int i, ret = 0, rewritten = 0;
	u32 members;

	mutex_lock(&rc_volume_lock);
	if (!rc_volume_disk) {
		ret = -ENODEV;
		goto out_unlock;
	}
	members = (u32)rc_volume_member_count;

	for (i = 0; i < rc_volume_member_count; i++) {
		struct rc_adapter *a = rc_volume_members[i];
		struct rc_nvme_state *nvme;
		struct device *dev;
		u8 *gen, *commit, *probe;
		dma_addr_t gen_dma, commit_dma, probe_dma;
		u64 gen_lba, gen_ts, new_ts;
		u32 gen_bytes, seq, off, pkt;
		bool removed = false;
		int rc;

		if (!a)
			continue;
		nvme = &a->ctx.nvme;
		dev = &a->pdev->dev;
		if (!nvme->md_valid || !nvme->md_config_commit_lba)
			continue;

		gen = dma_alloc_coherent(dev, PAGE_SIZE, &gen_dma, GFP_KERNEL);
		commit = dma_alloc_coherent(dev, PAGE_SIZE, &commit_dma,
					    GFP_KERNEL);
		probe = dma_alloc_coherent(dev, PAGE_SIZE, &probe_dma,
					   GFP_KERNEL);
		if (!gen || !commit || !probe) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s cannot allocate DMA buffers\n",
				  pci_name(a->pdev));
			ret = -ENOMEM;
			goto free_bufs;
		}

		rc = rc_volume_read_commit(a, commit, commit_dma, &gen_lba,
					   &gen_bytes, &gen_ts);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s cannot read the active commit (%d)\n",
				  pci_name(a->pdev), rc);
			ret = rc;
			goto free_bufs;
		}
		if (gen_bytes > PAGE_SIZE) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s generation of %u bytes exceeds one page — refusing\n",
				  pci_name(a->pdev), gen_bytes);
			ret = -EOPNOTSUPP;
			goto free_bufs;
		}
		seq = get_unaligned_le32(commit + RC_COMMIT_GEN_SEQ_OFFSET);

		rc = rc_nvme_read_lba(a, gen_lba, (u16)(gen_bytes / 512u - 1),
				      gen_dma);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s read generation @0x%llx failed (%d)\n",
				  pci_name(a->pdev),
				  (unsigned long long)gen_lba, rc);
			ret = rc;
			goto free_bufs;
		}
		if (get_unaligned_le64(gen + RC_GEN_HEADER_TS_OFFSET) != gen_ts) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s generation header ts does not match the commit — refusing to edit\n",
				  pci_name(a->pdev));
			ret = -EBADMSG;
			goto free_bufs;
		}

		/* Walk records in firmware order: PD (0x80), LD (PacketSize at
		 * +0x90), CTRL (0x88).  Drop the volume LD for this member set. */
		for (off = RC_MB_GEN_HEADER;
		     off + RC_MB_LD_ELEM_ARRAY <= gen_bytes; ) {
			u32 tag = get_unaligned_le32(gen + off);

			if (!tag)
				break;
			if (tag == RC_MB_TAG_LOGICAL_DEVICE) {
				pkt = get_unaligned_le32(gen + off +
							RC_MB_LD_PACKET_SIZE);
				if (pkt < RC_MB_LD_ELEM_ARRAY ||
				    off + pkt > gen_bytes)
					break;
				if (rc_mb_is_volume_type(
						get_unaligned_le32(gen + off +
							RC_MB_LD_DEVICE_TYPE)) &&
				    get_unaligned_le32(gen + off +
							RC_MB_LD_DEVICES) == members) {
					memmove(gen + off, gen + off + pkt,
						gen_bytes - off - pkt);
					memset(gen + gen_bytes - pkt, 0, pkt);
					removed = true;
					break;
				}
				off += pkt;
				continue;
			}
			if (tag == RC_DST_PHYSICAL_DEVICE) {
				off += RC_MB_PD_BYTES;
				continue;
			}
			if (tag == RC_DST_CONTROLLER_DEVICE) {
				off += RC_MB_CTRL_BYTES;
				continue;
			}
			break;
		}
		if (!removed) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s generation has no %u-member volume LD to remove — refusing\n",
				  pci_name(a->pdev), members);
			ret = -ENOENT;
			goto free_bufs;
		}

		new_ts = gen_ts + 1;
		rc_mb_put64(gen + RC_MB_GEN_TS, new_ts);
		rc = rc_nvme_write_lba(a, gen_lba,
				      (u16)(gen_bytes / 512u - 1), gen_dma);
		if (!rc)
			rc = rc_nvme_flush(a);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s generation write/flush failed (%d) — commit untouched\n",
				  pci_name(a->pdev), rc);
			ret = rc;
			goto free_bufs;
		}

		rc_mb_commit_block(commit, (u32)gen_lba, gen_bytes, new_ts,
				   seq + 1);
		rc = rc_nvme_write_lba(a, nvme->md_config_commit_lba, 0,
				      commit_dma);
		if (!rc)
			rc = rc_nvme_flush(a);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s commit write/flush failed (%d)\n",
				  pci_name(a->pdev), rc);
			ret = rc;
			goto free_bufs;
		}

		/* Read both back: the delete is only accepted when the commit
		 * names the new timestamp.  Logged either way. */
		rc = rc_nvme_read_lba(a, nvme->md_config_commit_lba, 0,
				     probe_dma);
		if (!rc && get_unaligned_le64(probe +
				RC_COMMIT_GEN_TS_OFFSET) != new_ts) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s commit readback ts 0x%016llx != 0x%016llx\n",
				  pci_name(a->pdev),
				  (unsigned long long)get_unaligned_le64(
					probe + RC_COMMIT_GEN_TS_OFFSET),
				  (unsigned long long)new_ts);
			rc = -EIO;
		}
		if (!rc)
			rc = rc_nvme_read_lba(a, gen_lba, 0, probe_dma);
		if (!rc && get_unaligned_le64(probe) != new_ts) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s generation readback ts 0x%016llx != 0x%016llx\n",
				  pci_name(a->pdev),
				  (unsigned long long)get_unaligned_le64(probe),
				  (unsigned long long)new_ts);
			rc = -EIO;
		}
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_delete_array: %s readback did not confirm the new generation (%d)\n",
				  pci_name(a->pdev), rc);
			ret = rc;
			goto free_bufs;
		}

		rewritten++;
		rc_printk(RC_NOTE,
			  "rc_volume_delete_array: %s array LD removed, generation @0x%llx re-committed ts=0x%016llx seq=%u\n",
			  pci_name(a->pdev),
			  (unsigned long long)gen_lba,
			  (unsigned long long)new_ts, seq + 1);
free_bufs:
		if (gen)
			dma_free_coherent(dev, PAGE_SIZE, gen, gen_dma);
		if (commit)
			dma_free_coherent(dev, PAGE_SIZE, commit, commit_dma);
		if (probe)
			dma_free_coherent(dev, PAGE_SIZE, probe, probe_dma);
		if (ret)
			break;
	}

	if (ret && rewritten)
		rc_printk(RC_ERROR,
			  "rc_volume_delete_array: aborted after rewriting %d of %u member(s) — the array is PARTIALLY deleted; rerun DELETE once the failing member is usable\n",
			  rewritten, members);
	if (!ret && !rewritten)
		ret = -ENOENT;

	if (!ret) {
		for (i = 0; i < rc_volume_member_count; i++) {
			struct rc_nvme_state *nvme;

			if (!rc_volume_members[i])
				continue;
			nvme = &rc_volume_members[i]->ctx.nvme;
			nvme->ld_valid = false;
			nvme->ld_record_seen = false;
			nvme->ld_raw_len = 0;
			nvme->ld_my_position = -1;
		}
		rc_printk(RC_NOTE,
			  "rc_volume_delete_array: array deleted on %d member(s), tearing the volume down\n",
			  rewritten);
	}

out_unlock:
	mutex_unlock(&rc_volume_lock);
	if (!ret)
		rc_volume_teardown();
	return ret;
}

/*
 * Persist a CREATE: stamp every bound member with a fresh RC_MetaData block,
 * a config generation holding one array LogicalDevice record, and a
 * RC_ConfigCommit that names it, then flush.  This is the on-disk half only —
 * the volume appears when the driver next assembles (module reload or next
 * boot), which is also the proof that the written generation is readable.
 *
 * Members are the bound adapters with valid metadata, in PCI order; rcadm's
 * request names a count and a level, and the four members of this box are
 * exactly what it is given.  Free-member enumeration beyond that (disks the
 * driver does not bind) is still not possible: the shim can only offer what
 * it probed.
 */
/*
 * Data columns a CREATE geometry contributes, or 0 when the writer has no
 * record for it.  Mirrors repeat data, so they add no capacity: RAID 1 is
 * one column and RAID 10 is FirstCount columns, which is exactly what
 * rc_ld_level_from() reconstructs from the record when it is read back.
 * RAID 5 adds SecondCount - 1 data columns and needs the parity_create
 * opt-in (metadata only: the data/parity region is not initialised).
 * DeviceType 0x1BF7 (single-disk volume) and RAID 6 stay unwritten.
 */
static u32 rc_create_data_cols(u32 raid_type, u32 first, u32 second,
			       u32 count)
{
	if (raid_type == RC_MB_DEVTYPE_RAID5) {
		if (!rc_volume_parity_create || count < 3u ||
		    first != 1u || second != count)
			return 0;
		return count - 1u;
	}
	if (raid_type != RC_MB_DEVTYPE_VOLUME)
		return 0;
	if (second == 1u && first >= 2u && first == count)
		return first;			/* RAID 0 */
	if (first == 1u && second == 2u && count == 2u)
		return 1u;			/* RAID 1 */
	if (first >= 2u && second == 2u && (u64)first * second == count)
		return first;			/* RAID 10 */
	return 0;
}

int rc_volume_create_array(u32 raid_type, u32 first, u32 second,
			   u32 device_count, u32 size_mb, u64 requested_id,
			   const u64 *device_ids, u32 nr_ids)
{
	const u32 chunk_sectors = 512u;	/* chunk_index 3 = 256 KiB */
	const u32 gen_lba = RC_MB_RING_LBA + 0x92u;
	struct rc_adapter *list[RC_VOLUME_MAX_MEMBERS];
	struct rc_mb_member_meta metas[RC_VOLUME_MAX_MEMBERS];
	u64 ids[RC_VOLUME_MAX_MEMBERS], volume_id;
	u8 *rec, *md, *commit;
	u64 gen_ts, user_sz = ~0ULL, cap, element;
	u32 rec_len, gen_len, i, j, n = 0, data_cols;
	int ret = 0;

	data_cols = rc_create_data_cols(raid_type, first, second,
					device_count);
	if (!data_cols)
		return -EOPNOTSUPP;
	if (!device_count || device_count > RC_VOLUME_MAX_MEMBERS)
		return -EINVAL;
	if (nr_ids && (nr_ids != device_count || !device_ids))
		return -EINVAL;

	rec = kzalloc(PAGE_SIZE, GFP_KERNEL);
	md = kzalloc(RC_MB_SECTOR, GFP_KERNEL);
	commit = kzalloc(RC_MB_SECTOR, GFP_KERNEL);
	if (!rec || !md || !commit) {
		ret = -ENOMEM;
		goto out_free;
	}

	mutex_lock(&rc_state.lock);
	if (rc_volume_disk) {
		rc_printk(RC_ERROR,
			  "rc_volume_create_array: a volume is already assembled — refusing to stamp over it\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	for (i = 0; i < RC_MAX_ADAPTERS && n < RC_VOLUME_MAX_MEMBERS; i++) {
		struct rc_adapter *a = NULL;

		if (!nr_ids) {
			/* No selection from the CLI: every usable member. */
			struct rc_adapter *cand = rc_state.adapters[i];

			if (!cand || !cand->ctx.nvme.md_valid ||
			    !cand->ctx.nvme.ns1_nsze)
				continue;
			a = cand;
		} else if (i < nr_ids) {
			for (j = 0; j < RC_MAX_ADAPTERS; j++) {
				struct rc_adapter *cand = rc_state.adapters[j];

				if (cand && cand->ctx.nvme.md_valid &&
				    cand->ctx.nvme.ns1_nsze &&
				    cand->ctx.nvme.md_device_id == device_ids[i]) {
					a = cand;
					break;
				}
			}
			if (!a) {
				rc_printk(RC_ERROR,
					  "rc_volume_create_array: member %u (DeviceID 0x%016llx) is not bound or has no metadata\n",
					  i,
					  (unsigned long long)device_ids[i]);
				ret = -ENODEV;
				goto out_unlock;
			}
		} else {
			break;
		}
		list[n] = a;
		ids[n] = a->ctx.nvme.md_device_id;
		metas[n].device_id = a->ctx.nvme.md_device_id;
		metas[n].disk_sectors = a->ctx.nvme.ns1_nsze;
		metas[n].sub_vid = a->pdev->subsystem_vendor;
		metas[n].sub_dev = a->pdev->subsystem_device;
		metas[n].pci_loc = ((u32)a->pdev->bus->number << 16) |
				   ((u32)PCI_SLOT(a->pdev->devfn) << 8) |
				   (u32)PCI_FUNC(a->pdev->devfn);
		n++;
	}
	if (n != device_count) {
		rc_printk(RC_ERROR,
			  "rc_volume_create_array: request names %u member(s), %u bound and initialised disk(s) are available\n",
			  device_count, n);
		ret = -EINVAL;
		goto out_unlock;
	}

	for (i = 0; i < n; i++) {
		u64 u = rc_mb_user_sectors(list[i]->ctx.nvme.ns1_nsze,
					   chunk_sectors);

		if (!u) {
			rc_printk(RC_ERROR,
				  "rc_volume_create_array: %s offers no user sectors\n",
				  pci_name(list[i]->pdev));
			ret = -EINVAL;
			goto out_unlock;
		}
		user_sz = min(user_sz, u);
	}

	cap = user_sz * data_cols;
	cap = cap / chunk_sectors * chunk_sectors;
	if (size_mb) {
		u64 want = (u64)size_mb << 11;

		if (want < cap)
			cap = want / chunk_sectors * chunk_sectors;
	}
	cap = cap / data_cols * data_cols;
	element = cap / data_cols;
	if (!cap || !element) {
		ret = -EINVAL;
		goto out_unlock;
	}

	gen_ts = get_jiffies_64();
	if (!gen_ts)
		gen_ts = 1;

	/*
	 * Every element covers cap/data_cols sectors, not the member's whole
	 * user area: the LD's total capacity must equal per-member slice x
	 * data columns (mirrors add none) or rc_volume_register_member() marks
	 * the geometry untrusted and forces the volume read-only.  (The full
	 * user_sz is only the ceiling computed above.)
	 *
	 * The record set is firmware-shaped: controller PD, one PD per member,
	 * the array LD, the raw-disk LD, then one ControllerDevice record per
	 * member.  A generation holding only the LD assembles here but is not
	 * a layout the UEFI utility's parser is known to walk.
	 */
	/*
	 * rcadm picks the logical-device Id itself, sends it as PreferDeviceID
	 * and, once the config write returns, looks the array up by that same
	 * Id (RC_Core::RC_FindLogicalHandle).  Stamping an Id of our own makes
	 * that lookup return LOGICAL_DEVICE_NOT_FOUND, which is the "error
	 * creating array, check arguments" exit 1 — the array is on disk and
	 * assembles, but the CLI cannot recognise it.  Honour the request;
	 * derive one only when the payload carries none.
	 */
	volume_id = requested_id ? requested_id :
		    ((ids[0] ^ (ids[n > 1 ? 1 : 0] << 1)) | 1ULL);
	rec_len = rc_mb_records(rec, PAGE_SIZE, raid_type, metas, n, first,
				second, cap, element, 3, volume_id);
	if (!rec_len || RC_MB_GEN_HEADER + rec_len > PAGE_SIZE) {
		rc_printk(RC_ERROR,
			  "rc_volume_create_array: record set does not fit (%u bytes)\n",
			  rec_len);
		ret = -EINVAL;
		goto out_unlock;
	}
	for (i = 0; i < n; i++) {
		struct rc_adapter *a = list[i];
		struct rc_nvme_state *nvme = &a->ctx.nvme;
		struct device *dev = &a->pdev->dev;
		u8 *gen;
		dma_addr_t gen_dma;
		int rc;

		gen = dma_alloc_coherent(dev, PAGE_SIZE, &gen_dma, GFP_KERNEL);
		if (!gen) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		rc_mb_metadata_block_ex(md, ids[i], nvme->md_mbr_checksum);
		rc = rc_nvme_write_verify_lba(a, RC_MB_METADATA_LBA, md,
					      "create metadata");
		if (!rc) {
			gen_len = rc_mb_generation(gen, PAGE_SIZE, gen_ts, rec,
						   rec_len);
			rc = rc_nvme_write_lba(a, gen_lba,
					       (u16)(gen_len / 512u - 1), gen_dma);
			if (!rc)
				rc = rc_nvme_flush(a);
		}
		if (!rc) {
			rc_mb_commit_block(commit, gen_lba, gen_len, gen_ts, 1);
			rc = rc_nvme_write_verify_lba(a, RC_MB_COMMIT_LBA,
						      commit, "create commit");
		}
		dma_free_coherent(dev, PAGE_SIZE, gen, gen_dma);
		if (rc) {
			rc_printk(RC_ERROR,
				  "rc_volume_create_array: %s stamping failed (%d) — %u member(s) were written\n",
				  pci_name(a->pdev), rc, i);
			ret = rc;
			goto out_unlock;
		}
		rc_printk(RC_NOTE,
			  "rc_volume_create_array: %s stamped (gen @0x%x len %u ts=0x%016llx)\n",
			  pci_name(a->pdev), gen_lba, gen_len,
			  (unsigned long long)gen_ts);
	}

	/*
	 * Assemble immediately: rcadm queries the array right after CREATE and
	 * treats an absent one as a failed create.  Re-parse each member's LD
	 * from the generation just written, then feed the normal registration
	 * path — the same calls boot uses after reading metadata.
	 */
	for (i = 0; i < n; i++) {
		rc_volume_parse_logical_device(list[i]);
		rc_volume_register_member(list[i]);
	}

	rc_printk(RC_NOTE,
		  "rc_volume_create_array: %u-member array stamped, capacity %llu sectors\n",
		  n, (unsigned long long)cap);

out_unlock:
	mutex_unlock(&rc_state.lock);
out_free:
	kfree(rec);
	kfree(md);
	kfree(commit);
	return ret;
}

/*
 * Map a disk_index (the enumeration rc_volume_get_inventory() publishes) to
 * an adapter.  Caller holds rc_state.lock.
 */
static struct rc_adapter *rc_adapter_by_disk_index(u32 index)
{
	u32 n = 0;
	int i;

	for (i = 0; i < RC_MAX_ADAPTERS; i++) {
		struct rc_adapter *a = rc_state.adapters[i];

		if (!a)
			continue;
		if (n++ == index)
			return a;
	}
	return NULL;
}

/*
 * CREATE entry point for the control plane (/dev/amd_ctl0).  Builds the
 * DeviceID list from disk indices under rc_state.lock, then hands it to the
 * ordinary writer, which resolves each ID and takes the locks itself.
 *
 * RAID 5 is metadata-only and behind the parity_create opt-in: the writer
 * emits a firmware-shaped 0x1BF5 record, but it does not zero the members'
 * user areas, so the caller must supply erased disks and zero the volume
 * before use.  RAID 6 is refused (no Q write path).  See docs/STATUS.md.
 */
int rc_volume_create_array_indexed(u32 level, const u32 *indices,
				   u32 count, u32 size_mb)
{
	u64 ids[RC_VOLUME_MAX_MEMBERS];
	u32 first, second, devtype, i, j;
	int ret;

	if (!indices || count < 2 || count > RC_VOLUME_MAX_MEMBERS)
		return -EINVAL;
	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (indices[i] == indices[j])
				return -EINVAL;

	switch (level) {
	case AMD_ARRAY_LEVEL_RAID0:
		devtype = RC_MB_DEVTYPE_VOLUME;
		first = count;
		second = 1;
		break;
	case AMD_ARRAY_LEVEL_RAID1:
		if (count != 2)
			return -EINVAL;
		devtype = RC_MB_DEVTYPE_VOLUME;
		first = 1;
		second = 2;
		break;
	case AMD_ARRAY_LEVEL_RAID10:
		if (count < 4 || (count & 1))
			return -EINVAL;
		devtype = RC_MB_DEVTYPE_VOLUME;
		first = count / 2;
		second = 2;
		break;
	case AMD_ARRAY_LEVEL_RAID5:
		if (!rc_volume_parity_create) {
			rc_printk(RC_WARN,
				  "rcraid: RAID 5 CREATE refused: the metadata writer does not initialise data/parity, so CREATE needs parity_create=1 and members with an empty user area\n");
			return -EOPNOTSUPP;
		}
		if (count < 3)
			return -EINVAL;
		devtype = RC_MB_DEVTYPE_RAID5;
		first = 1;
		second = count;
		break;
	case AMD_ARRAY_LEVEL_RAID6:
		rc_printk(RC_WARN,
			  "rcraid: RAID 6 CREATE refused: there is no Q write or initialisation path\n");
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}

	mutex_lock(&rc_state.lock);
	for (i = 0; i < count; i++) {
		struct rc_adapter *a = rc_adapter_by_disk_index(indices[i]);

		if (!a || !a->ctx.nvme.md_valid || !a->ctx.nvme.ns1_nsze) {
			mutex_unlock(&rc_state.lock);
			rc_printk(RC_WARN,
				  "rcraid: disk %u is not a bound, initialised member\n",
				  indices[i]);
			return -ENODEV;
		}
		ids[i] = a->ctx.nvme.md_device_id;
	}
	mutex_unlock(&rc_state.lock);

	ret = rc_volume_create_array(devtype, first, second,
				     count, size_mb, 0, ids, count);
	if (ret)
		rc_printk(RC_WARN,
			  "rcraid: CREATE level %u over %u disk(s) failed (%d)\n",
			  level, count, ret);
	return ret;
}

static int rc_volume_create_disk(void)
{
	struct rc_adapter *m0 = NULL;
	u64 total_sectors;
	int i, ret;
	u32 nr_hw, h;

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		if (rc_volume_members[i]) {
			m0 = rc_volume_members[i];
			break;
		}
	}
	if (!m0)
		return -ENODEV;

	/* Allocate the parity-write resources before the tagset: the tagset
	 * flag and the queue limits both depend on whether the path is
	 * usable. */
	rc_volume_parity_pool_prepare();
	sema_init(&rc_volume_parity_lock, 1);

	/* nr_hw_queues = min(member->nr_io_queues) across present members.
	 * Each hctx i routes to io_queues[i] on every member, so we can't
	 * ask blk-mq for more hctxs than the smallest member supports.  All
	 * T700s grant 128 (we cap at RC_NVME_IO_QUEUE_TARGET=4) so this
	 * just resolves to 4 in practice, but the min is a future-proofing
	 * safety net.  Computed up front because it dimensions the PRP pool
	 * allocated just below.  Clamped to RC_VOLUME_MAX_HCTX so the [hctx]
	 * index into that pool (and every other per-hctx array) is always in
	 * bounds.  A member re-added later must support >= this many queues
	 * (checked at readmit) — the tagset is never resized. */
	nr_hw = m0->ctx.nvme.nr_io_queues;
	for (i = 0; i < rc_volume_member_count; i++) {
		u32 m;

		if (!rc_volume_members[i])
			continue;
		m = rc_volume_members[i]->ctx.nvme.nr_io_queues;
		if (m < nr_hw)
			nr_hw = m;
	}
	if (nr_hw == 0)
		nr_hw = 1;
	if (nr_hw > RC_VOLUME_MAX_HCTX)
		nr_hw = RC_VOLUME_MAX_HCTX;
	rc_volume_nr_hw = nr_hw;

	/* Per-hctx, per-member, per-tag PRP-list / DSM buffer (PAGE_SIZE each).
	 * The hctx dimension is mandatory — req->tag is unique only within a
	 * single hctx, so two concurrent commands on different queues that draw
	 * the same tag must not share a buffer (see the pool declaration near
	 * the top of this file for the full corruption rationale).  Only the
	 * live nr_hw × member_count × queue_depth subset is allocated; the rest
	 * stay NULL and are skipped by the free paths. */
	for (i = 0; i < rc_volume_member_count; i++) {
		if (!rc_volume_members[i])
			continue;
		ret = rc_volume_alloc_member_dma(i, nr_hw);
		if (ret)
			goto err_free_dma;
	}

	{
		/* Tag pool sizing (C-4): tags + worst-case internal commands
		 * must stay ≤ min(actual SQ depth across all member queues)
		 * − 1 (an NVMe SQ is full at depth−1).  The queues are
		 * clamped to CAP.MQES at create time, which can be < the
		 * static RC_NVME_IO_QUEUE_DEPTH — the tagset must follow the
		 * REAL depth, not the compile-time constant. */
		u32 depth = RC_VOLUME_QUEUE_DEPTH;

		for (i = 0; i < rc_volume_member_count; i++) {
			if (!rc_volume_members[i])
				continue;
			for (h = 0; h < nr_hw; h++) {
				struct rc_nvme_io_queue *q =
					rc_volume_members[i]->ctx.nvme.io_queues[h];

				if (q && (u32)q->sq_depth >
					 RC_VOLUME_SQ_INTERNAL_HEADROOM &&
				    depth > (u32)q->sq_depth - 1 -
					    RC_VOLUME_SQ_INTERNAL_HEADROOM)
					depth = (u32)q->sq_depth - 1 -
						RC_VOLUME_SQ_INTERNAL_HEADROOM;
			}
		}
		if (depth < 1)
			depth = 1;

		memset(&rc_volume_tagset, 0, sizeof(rc_volume_tagset));
		rc_volume_tagset.ops          = &rc_volume_mq_ops;
		rc_volume_tagset.nr_hw_queues = nr_hw;
		rc_volume_tagset.queue_depth  = depth;
		rc_volume_tagset.numa_node    = NUMA_NO_NODE;
		rc_volume_tagset.cmd_size     = sizeof(struct rc_volume_pdu);
		/* Full-row writes are async (completed by the ISR), but the
		 * partial-row read-modify-write path, degraded RAID 5 reads and
		 * the parity rebuild all run synchronous NVMe commands in
		 * process context.  BLOCKING tells blk-mq that queue_rq may
		 * sleep; it is set for every parity level, even a read-only one. */
		rc_volume_tagset.flags        =
			(rc_volume_raid_level == RC_LDT_RAID5 ||
			 rc_volume_raid_level == RC_LDT_RAID6) ?
			BLK_MQ_F_BLOCKING : 0;

		rc_printk(RC_NOTE,
			  "rc_volume_create_disk: blk-mq nr_hw_queues=%u (queue_depth=%u → %u total outstanding, %u SQEs/queue reserved for internal cmds)\n",
			  nr_hw, depth, nr_hw * depth,
			  RC_VOLUME_SQ_INTERNAL_HEADROOM);
	}

	ret = blk_mq_alloc_tag_set(&rc_volume_tagset);
	if (ret)
		goto err_free_dma;

	/* Optional: without it, layouts PRPs cannot encode are rejected as
	 * before.  Never fail volume creation over it. */
	if (rc_volume_bounce_pool_init())
		rc_printk(RC_WARN,
			  "rc_volume_create_disk: bounce pool unavailable — non-PRP layouts will be rejected\n");

	{
		struct queue_limits lim = {
			.logical_block_size  = 512,
			.physical_block_size = 512,
			/* PRP list lets us span up to RC_VOLUME_DATA_BYTES per
			 * request.  Caller's bvecs sum into one NVMe READ (or,
			 * for multi-stripe requests, into one NVMe cmd per
			 * member via rc_volume_dispatch_multi_stripe). */
			.max_hw_sectors      = RC_VOLUME_DATA_BYTES / 512,
			/* RAID1 and RAID10 cap segments at the per-member sg
			 * array size: both fan-outs copy the request's segments
			 * into ms_sg[member][RC_VOLUME_MS_PAGES_PER_MEMBER],
			 * so blk-mq must never legally hand queue_rq more
			 * segments than that array holds.  (virt_boundary
			 * already bounds a 256 KiB request to ~65 segments in
			 * practice, but the advertised contract is what
			 * matters — don't rely on the implicit math.) */
			.max_segments        = (rc_volume_raid_level == RC_LDT_RAID1 ||
						rc_volume_raid_level == RC_LDT_RAID10)
						? RC_VOLUME_MS_PAGES_PER_MEMBER
						: RC_VOLUME_DATA_PAGES,
			.max_segment_size    = PAGE_SIZE,
			/* NVMe PRP semantics: PRP1 may carry an in-page offset,
			 * PRP2 and PRP-list entries must point at page starts.
			 * virt_boundary forces blk-mq to split bvecs so every
			 * segment after the first is page-aligned, which is what
			 * rc_volume_build_prp relies on. */
			.virt_boundary_mask  = PAGE_SIZE - 1,
			/* chunk_sectors = 0: blk-mq does NOT split READ/WRITE
			 * at stripe boundaries, so a request may span several
			 * stripes.  rc_volume_queue_rq folds that into ONE NVMe
			 * command per affected member via
			 * rc_volume_dispatch_multi_stripe (RAID0) or
			 * rc_volume_dispatch_raid10 (RAID10), which split the
			 * bvec at stripe boundaries into per-member sg arrays
			 * and honour the virt_boundary_mask above.  This caps
			 * the per-request command count at the member count
			 * instead of one per stripe.
			 *
			 * Discards are separately split by
			 * max_hw_discard_sectors, so the one-DSM-per-member
			 * fan-outs (multi_stripe_discard / raid10_discard) still
			 * see genuinely multi-stripe requests. */
			.chunk_sectors       = 0,
			/* Advertise that the underlying controllers have a volatile
			 * write cache and that we honour FUA — filesystems will
			 * now route REQ_OP_FLUSH and REQ_FUA writes through. */
			.features            = BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA,
			/* DISCARD via NVMe DSM Deallocate.  rc_volume_queue_rq
			 * fans a single multi-stripe discard out as one DSM per
			 * AFFECTED MEMBER (see rc_volume_dispatch_multi_stripe_discard),
			 * so we can take requests far larger than one stripe.
			 * 1 GiB = 2^21 sectors keeps the per-request kernel work
			 * trivial while collapsing fstrim's ~14.9M-stripe sweep
			 * into thousands of commands instead of millions. */
			.max_hw_discard_sectors  = 1U << 21,
			.discard_granularity     = 512,
			.max_discard_segments    = 1,
		};

		/* RAID5/6 reads place one data chunk on one member.  By default
		 * the queue is split at the chunk, which makes every read exactly
		 * one member's worth.  With parity writes enabled the write split
		 * must be the ROW (a whole row is one WRITE per member plus the
		 * parity write), and one row is rarely a power of two, which
		 * chunk_sectors requires — so the split is disabled and queue_rq
		 * fans multi-chunk READS out per member
		 * (rc_volume_dispatch_multi_stripe with parity_read).  A partial
		 * row is refused in queue_rq.  If a row cannot fit one request
		 * the chunk split stays and writes remain off —
		 * rc_volume_parity_write_ready(). */
		if (rc_volume_raid_level == RC_LDT_RAID5 &&
		    rc_volume_parity_write_ready()) {
			lim.max_hw_sectors = rc_volume_stripe_sectors *
					     (u32)(rc_volume_member_count - 1);
			lim.chunk_sectors  = 0;
		} else if (rc_volume_raid_level == RC_LDT_RAID5 ||
			   rc_volume_raid_level == RC_LDT_RAID6) {
			lim.max_hw_sectors = rc_volume_stripe_sectors;
			lim.chunk_sectors  = rc_volume_stripe_sectors;
		}

		rc_volume_disk = blk_mq_alloc_disk(&rc_volume_tagset, &lim, NULL);
	}
	if (IS_ERR(rc_volume_disk)) {
		ret = PTR_ERR(rc_volume_disk);
		rc_volume_disk = NULL;
		goto err_free_tagset;
	}

	/* Prefer the volume capacity from the on-disk LogicalDevice record
	 * — it accounts for the per-member reserved metadata regions.  Fall
	 * back to NSZE * member_count when no LD was parsed (legacy path). */
	if (m0->ctx.nvme.ld_valid && m0->ctx.nvme.ld_capacity_sectors)
		total_sectors = m0->ctx.nvme.ld_capacity_sectors;
	else
		total_sectors = m0->ctx.nvme.ns1_nsze *
				(u64)rc_volume_member_count;
	set_capacity(rc_volume_disk, total_sectors);
	snprintf(rc_volume_disk->disk_name, DISK_NAME_LEN, "rcraid0");
	rc_volume_disk->fops        = &rc_volume_bops;
	rc_volume_disk->major       = rc_major;
	/* 256 minors per disk reserves /dev/rcraid0 + p1..p255.  Matches the
	 * NVMe driver convention and is enough to enumerate every entry in a
	 * GPT (the spec caps at 128). */
	rc_volume_disk->minors      = 256;
	rc_volume_disk->first_minor = 0;
	/* No flag — let add_disk() scan the partition table at end-of-disk.
	 * rc_volume_map_lba is pure modular arithmetic and handles any LBA in
	 * [0, capacity), including the GPT secondary header at LBA(N-1) and the
	 * partition array at LBA(N-33..N-2), without any tail-special-casing. */
	rc_volume_disk->flags       = 0;
	/* Read-only unless the operator explicitly opted in via the module
	 * parameter.  Note the parameter is 0444 (read-only sysfs) so it
	 * can only be set at load time — re-load to switch modes.
	 *
	 * Additionally veto writes when the geometry is untrusted.  Two
	 * distinct triggers can revoke trust (legacy BDF fallback, or an
	 * unrecognized RAID0 chunk_index), each with its own cause and remedy
	 * recorded in rc_volume_geometry_untrust_reason.  The gate is purely
	 * additive — it can only force read-only, never grant writes the
	 * operator didn't request — and the untrusted reason is surfaced
	 * UNCONDITIONALLY (not only when enable_writes=1), so an operator
	 * running the default enable_writes=0 learns, on the box that hit it,
	 * why a later enable_writes=1 reload will still come up read-only. */
	{
		bool geom_trusted = (rc_volume_geometry_untrust_reason == NULL);

		if ((rc_volume_raid_level == RC_LDT_RAID5 ||
		     rc_volume_raid_level == RC_LDT_RAID6) &&
		    !rc_volume_parity_write_ready())
			rc_printk(RC_NOTE,
				  "rc_volume_create_disk: RAID 5/6 is assembled READ-ONLY: parity writes need enable_writes=1, parity_writes=1 and a full-row-capable geometry (issue #6)\n");

		if (!rc_volume_enable_writes || !geom_trusted ||
		    !rc_volume_parity_write_ready())
			set_disk_ro(rc_volume_disk, 1);

		if (!geom_trusted && rc_volume_enable_writes)
			rc_printk(RC_WARN,
				  "rc_volume_create_disk: enable_writes=1 but geometry is UNTRUSTED: %s — forcing READ-ONLY to avoid corrupting a mis-assembled array\n",
				  rc_volume_geometry_untrust_reason);
		else if (!geom_trusted)
			rc_printk(RC_WARN,
				  "rc_volume_create_disk: geometry is UNTRUSTED: %s — volume is READ-ONLY; loading with enable_writes=1 will NOT help until this is resolved\n",
				  rc_volume_geometry_untrust_reason);
		else if (!rc_volume_enable_writes)
			rc_printk(RC_NOTE,
				  "rc_volume_create_disk: writes disabled (load with enable_writes=1 to allow)\n");
		else if (!rc_volume_parity_write_ready())
			rc_printk(RC_WARN,
				  "rc_volume_create_disk: writes stay READ-ONLY for this level — see the RAID 5/6 note above\n");
		else
			rc_printk(RC_NOTE,
				  "rc_volume_create_disk: writes ENABLED\n");
	}

	ret = add_disk(rc_volume_disk);
	if (ret)
		goto err_put_disk;

	rc_volume_sysfs_register();

	rc_printk(RC_NOTE,
		  "rc_volume_create_disk: /dev/%s up, %llu sectors (%llu MiB, %s)\n",
		  rc_volume_disk->disk_name,
		  (unsigned long long)total_sectors,
		  (unsigned long long)(total_sectors >> 11),
		  (rc_volume_enable_writes &&
		   rc_volume_geometry_untrust_reason == NULL &&
		   rc_volume_parity_write_ready()) ?
			"read-write" : "read-only");
	return 0;

err_put_disk:
	put_disk(rc_volume_disk);
	rc_volume_disk = NULL;
err_free_tagset:
	blk_mq_free_tag_set(&rc_volume_tagset);
	rc_volume_bounce_pool_free();
err_free_dma:
	rc_volume_parity_pool_free();
	for (i = 0; i < rc_volume_member_count; i++)
		if (rc_volume_members[i])
			rc_volume_free_member_dma(i,
				&rc_volume_members[i]->pdev->dev);
	return ret;
}

/* Delayed degraded-assembly worker (allow_degraded=1).  Armed/re-armed by
 * every member registration; fires RC_VOLUME_ASSEMBLE_DELAY_MS after the
 * LAST registration.  If the volume still hasn't assembled because a
 * RAID1 member never showed up, mark the empty slot(s) failed/absent and
 * bring the volume up degraded. */
static void rc_volume_assemble_fn(struct work_struct *w)
{
	int i, present = 0;
	u32 expected;

	mutex_lock(&rc_volume_lock);
	expected = rc_volume_expected_members;
	if (rc_volume_disk || !expected || expected > RC_VOLUME_MAX_MEMBERS)
		goto out;
	if (rc_volume_raid_level != RC_LDT_RAID1 &&
	    rc_volume_raid_level != RC_LDT_RAID10)
		goto out;	/* RAID0 can't run without every member */
	if (rc_volume_geometry_untrust_reason) {
		rc_printk(RC_WARN,
			  "rc_volume_assemble_fn: not assembling degraded — geometry untrusted: %s\n",
			  rc_volume_geometry_untrust_reason);
		goto out;
	}

	for (i = 0; i < (int)expected; i++)
		if (rc_volume_members[i])
			present++;
	if (!present || present >= (int)expected)
		goto out;	/* nothing here, or complete (normal path) */

	/* RAID10 comes up degraded only while every mirror pair keeps at
	 * least one copy.  A whole pair missing leaves that half of the
	 * logical space unmapped — rc_volume_fatal() would reject every
	 * request, so refuse to expose a permanently-failing volume and
	 * leave the registry parked for a later full assembly.
	 * rc_volume_can_serve() reads the live mask, which has bits ONLY for
	 * the members that registered, so this is exactly the "no pair lost
	 * both copies" test. */
	if (rc_volume_raid_level == RC_LDT_RAID10 && !rc_volume_can_serve()) {
		rc_printk(RC_WARN,
			  "rc_volume_assemble_fn: RAID10 degraded assembly refused — a mirror pair has NO member present (only %d of %u present)\n",
			  present, expected);
		goto out;
	}

	rc_printk(RC_WARN,
		  "rc_volume_assemble_fn: only %d of %u RAID%d members present after %u ms — assembling DEGRADED (allow_degraded=1)\n",
		  present, expected,
		  rc_volume_raid_level == RC_LDT_RAID10 ? 10 : 1,
		  RC_VOLUME_ASSEMBLE_DELAY_MS);

	{
		int saved_count = rc_volume_member_count;
		int rc;

		for (i = 0; i < (int)expected; i++)
			if (!rc_volume_members[i])
				rc_volume_member_set_state(i, RC_MEMBER_FAILED);
		/* member_count becomes the SLOT count (== expected) from here
		 * on, same invariant a runtime hot-removal maintains. */
		rc_volume_member_count = (int)expected;

		rc = rc_volume_create_disk();
		if (rc) {
			/* Roll the registry back so a late-arriving member
			 * (or the next timer shot) can still take the normal
			 * full-assembly path — leaving count forced to
			 * `expected` with phantom FAILED slots would block
			 * regular assembly forever. */
			rc_printk(RC_ERROR,
				  "rc_volume_assemble_fn: degraded create_disk failed (%d) — reverting registry for a later attempt\n",
				  rc);
			rc_volume_member_count = saved_count;
			for (i = 0; i < (int)expected; i++) {
				if (rc_volume_members[i])
					continue;
				/* Back to the pre-registration default:
				 * state LIVE with NO live-mask bit (the bit
				 * is only ever set when a real member
				 * registers into the slot). */
				rc_volume_member_state[i] = RC_MEMBER_LIVE;
				atomic_andnot(BIT(i), &rc_volume_live_mask);
			}
		}
	}
out:
	mutex_unlock(&rc_volume_lock);
}

/* One synchronous NVMe READ/WRITE of up to RC_RESYNC_XFER_BYTES against
 * one member, using the boot/reset-time sync-command machinery (reserved
 * CID range, single-consumer CQE handshake — safe on a queue shared with
 * live blk-mq traffic).  `data` is the DMA address of a physically
 * contiguous buffer on this member's IOMMU domain; `prp_va/prp_pa` is a
 * per-member coherent page for the PRP list (>2-page transfers). */
static int rc_volume_resync_rw(struct rc_adapter *m, u8 opc, u64 slba,
			       u32 nlb, u32 bytes, dma_addr_t data,
			       __le64 *prp_va, dma_addr_t prp_pa)
{
	struct rc_nvme_sqe cmd;
	u32 pages = DIV_ROUND_UP(bytes, (u32)PAGE_SIZE);
	u32 i;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = opc;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(data);
	if (pages == 2) {
		cmd.prp2 = cpu_to_le64(data + PAGE_SIZE);
	} else if (pages > 2) {
		for (i = 1; i < pages; i++)
			prp_va[i - 1] = cpu_to_le64(data + (u64)i * PAGE_SIZE);
		cmd.prp2 = cpu_to_le64(prp_pa);
	}
	cmd.cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd.cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd.cdw12 = cpu_to_le32(nlb - 1);

	return rc_nvme_io_cmd_sync(m, &cmd);
}

/* Pick the read source for the resync.  Returns slot or -1.
 *
 * RAID1: any LIVE member other than the target holds an identical copy,
 *        so the first one found works.
 * RAID10: only the target's mirror partner holds a copy of the SAME
 *        logical stripes at the SAME physical offsets; copying from any
 *        other member would write another column's data onto the target.
 *        Adjacent pairing {2c, 2c+1} (see rc_amd_map_nested_raid10), so
 *        the partner is target ^ 1.  If the partner is not live the
 *        member cannot be rebuilt and is left parked. */
static int rc_volume_resync_pick_survivor(int target)
{
	unsigned long live =
		(unsigned long)(unsigned int)atomic_read(&rc_volume_live_mask);
	int i;

	if (rc_volume_raid_level == RC_LDT_RAID10) {
		int partner = target ^ 1;

		if (partner >= 0 && partner < RC_VOLUME_MAX_MEMBERS &&
		    (live & BIT(partner)) && rc_volume_members[partner])
			return partner;
		return -1;
	}

	for_each_set_bit(i, &live, RC_VOLUME_MAX_MEMBERS)
		if (i != target && rc_volume_members[i])
			return i;
	return -1;
}

/* The resync kthread: full copy survivor → target over the volume's
 * logical LBA space (RAID1 mapping is identity + per-member phys offset,
 * so this copies exactly the user-data region and never touches the
 * firmware-owned metadata below UserDataOffset). */
static int rc_volume_resync_fn(void *arg)
{
	int target = (int)(long)arg;
	u64 total, cursor = 0;
	void *buf = NULL;
	__le64 *list_va[2] = { NULL, NULL };
	dma_addr_t list_pa[2] = { 0, 0 };
	struct rc_adapter *tgt, *srv;
	int srv_slot = -1;
	int ret = -EIO;
	int retries = 0;
	unsigned int order = get_order(RC_RESYNC_XFER_BYTES);
	/* RAID10 rebuilds the target from its mirror partner only, one
	 * column's stripes at a time.  cursor/total are PER-MEMBER sectors
	 * (the caller sets total = volume / cols), and the exclusion window
	 * is published in LOGICAL sectors so the write gate works unchanged. */
	bool raid10 = (rc_volume_raid_level == RC_LDT_RAID10);
	bool raid5 = (rc_volume_raid_level == RC_LDT_RAID5);
	u32 stripe = rc_volume_stripe_sectors;
	u32 cols = rc_volume_raid10_cols;
	u32 tcol = cols ? (u32)(target / 2u) : 0u;

	total = rc_volume_resync_total;

	buf = (void *)__get_free_pages(GFP_KERNEL, order);
	if (!buf) {
		rc_printk(RC_ERROR,
			  "rc_volume_resync: failed to allocate %u-byte copy buffer (order %u) — aborting\n",
			  RC_RESYNC_XFER_BYTES, order);
		goto done;
	}

	rc_printk(RC_NOTE,
		  "rc_volume_resync: starting — member %d, %llu sectors (%llu MiB)\n",
		  target, (unsigned long long)total,
		  (unsigned long long)(total >> 11));

	while (cursor < total) {
		u32 nlb;
		u32 bytes;
		u64 logical, srv_lba, tgt_lba;
		dma_addr_t dma;
		int old_gen, err;

		if (kthread_should_stop())
			goto done;

		/* Both endpoints must still be usable.  The target must
		 * still be RESYNCING (a failure path may have demoted it);
		 * the survivor must still be live. */
		tgt = rc_volume_members[target];
		if (!tgt || READ_ONCE(rc_volume_member_state[target]) !=
			    RC_MEMBER_RESYNCING) {
			rc_printk(RC_ERROR,
				  "rc_volume_resync: target member %d went away/failed at %llu/%llu — aborting\n",
				  target, (unsigned long long)cursor,
				  (unsigned long long)total);
			goto done;
		}
		if (raid5) {
			u32 live, nlb5, bytes5;
			u8 *dst;
			int m, err5;

			nlb5 = (u32)min_t(u64, RC_RESYNC_XFER_SECTORS,
					  total - cursor);
			/* rc_volume_rmw[] holds one chunk per member, so the
			 * rebuild must never XOR more than that into it.  A
			 * 64 KiB chunk against a 128 KiB transfer used to
			 * memset past the DMA buffer and fail for good. */
			if (rc_volume_rmw_bytes)
				nlb5 = min_t(u32, nlb5,
					     rc_volume_rmw_bytes / 512u);
			bytes5 = nlb5 * 512u;
			if (!nlb5) {
				rc_printk(RC_ERROR,
					  "rc_volume_resync: RAID5 zero-length chunk at %llu — aborting\n",
					  (unsigned long long)cursor);
				goto done;
			}
			live = (u32)atomic_read(&rc_volume_live_mask);
			down(&rc_volume_parity_lock);
			dst = rc_volume_rmw[target].va;
			if (!dst) {
				up(&rc_volume_parity_lock);
				rc_printk(RC_ERROR,
					  "rc_volume_resync: no chunk buffer for member %d — aborting\n",
					  target);
				goto done;
			}
			memset(dst, 0, bytes5);
			err5 = 0;
			for (m = 0; m < rc_volume_member_count; m++) {
				if (m == target)
					continue;
				if (!(live & BIT(m))) {
					err5 = -EIO;
					break;
				}
				err5 = rc_volume_rmw_io(m, RC_NVME_NVM_OP_READ,
						cursor + rc_volume_member_phys_offset[m],
						nlb5);
				if (err5)
					break;
				rc_volume_xor(dst, rc_volume_rmw[m].va, bytes5);
			}
			if (!err5)
				err5 = rc_volume_rmw_io(target,
						RC_NVME_NVM_OP_WRITE,
						cursor + rc_volume_member_phys_offset[target],
						nlb5);
			up(&rc_volume_parity_lock);
			if (err5) {
				if ((err5 == -EBUSY || err5 == -ETIMEDOUT) &&
				    ++retries <= RC_RESYNC_MAX_RETRIES) {
					rc_printk(RC_WARN,
						  "rc_volume_resync: transient RAID5 error (%d) at lba %llu — retry %d/%d\n",
						  err5, (unsigned long long)cursor,
						  retries, RC_RESYNC_MAX_RETRIES);
					msleep(20);
					continue;
				}
				rc_printk(RC_ERROR,
					  "rc_volume_resync: RAID5 rebuild of member %d failed (%d) at lba %llu — aborting\n",
					  target, err5,
					  (unsigned long long)cursor);
				goto done;
			}
			retries = 0;
			cursor += nlb5;
			atomic64_set(&rc_volume_resync_cursor, cursor);
			if (rc_volume_resync_delay_ms)
				msleep(rc_volume_resync_delay_ms);
			cond_resched();
			continue;
		}
		srv_slot = rc_volume_resync_pick_survivor(target);
		if (srv_slot < 0) {
			rc_printk(RC_ERROR,
				  "rc_volume_resync: no live survivor to read from — aborting\n");
			goto done;
		}
		srv = rc_volume_members[srv_slot];

		/* Translate this chunk's per-member offset into the logical
		 * sector range it represents.  RAID1 is identity.  RAID10's
		 * cursor walks ONE member's user-data region, so the logical
		 * LBA is the cursor's stripe rounded up to this column's
		 * logical stripe, plus the in-stripe offset.  Cap the chunk
		 * at the stripe boundary: a chunk that crossed it would map
		 * to a NON-contiguous logical range (the next physical stripe
		 * on this member is cols logical stripes later), which the
		 * single-range write-exclusion window cannot describe — the
		 * gate would then under-cover and a racing write could be
		 * overwritten by the copy. */
		if (raid10) {
			u32 in_stripe, room;

			if (!stripe || !cols) {
				rc_printk(RC_ERROR,
					  "rc_volume_resync: RAID10 resync with bad geometry (stripe=%u cols=%u) — aborting\n",
					  stripe, cols);
				goto done;
			}
			in_stripe = (u32)(cursor % stripe);
			room = stripe - in_stripe;
			nlb = (u32)min_t(u64, RC_RESYNC_XFER_SECTORS,
					 min_t(u64, room, total - cursor));
			logical = div_u64(cursor, stripe) *
				  ((u64)stripe * cols) +
				  (u64)tcol * stripe + in_stripe;
		} else {
			nlb = (u32)min_t(u64, RC_RESYNC_XFER_SECTORS,
					 total - cursor);
			logical = cursor;
		}
		bytes   = nlb * 512u;
		srv_lba = cursor + rc_volume_member_phys_offset[srv_slot];
		tgt_lba = cursor + rc_volume_member_phys_offset[target];

		/* Lazily allocate the per-member PRP-list pages the first
		 * time each endpoint is known (they are per-IOMMU-domain). */
		if (!list_va[0]) {
			list_va[0] = dma_alloc_coherent(&srv->pdev->dev,
							PAGE_SIZE, &list_pa[0],
							GFP_KERNEL);
			if (!list_va[0]) {
				rc_printk(RC_ERROR,
					  "rc_volume_resync: PRP-list page alloc failed for survivor %s — aborting\n",
					  pci_name(srv->pdev));
				goto done;
			}
		}
		if (!list_va[1]) {
			list_va[1] = dma_alloc_coherent(&tgt->pdev->dev,
							PAGE_SIZE, &list_pa[1],
							GFP_KERNEL);
			if (!list_va[1]) {
				rc_printk(RC_ERROR,
					  "rc_volume_resync: PRP-list page alloc failed for target %s — aborting\n",
					  pci_name(tgt->pdev));
				goto done;
			}
		}

		/* Publish the exclusion window, flip the write generation,
		 * and drain writes counted under the old generation — they
		 * may have checked the window before publication.  After
		 * the drain, no application write overlapping this chunk is
		 * in flight, and none can start until the window moves. */
		atomic64_set(&rc_volume_resync_window, logical);
		smp_mb();
		old_gen = atomic_fetch_inc(&rc_volume_wgen) & 1;
		while (atomic_read(&rc_volume_wgen_count[old_gen])) {
			if (kthread_should_stop())
				goto done;
			usleep_range(100, 500);
		}

		/* Copy: survivor → buffer → target. */
		dma = dma_map_single(&srv->pdev->dev, buf, bytes,
				     DMA_FROM_DEVICE);
		if (dma_mapping_error(&srv->pdev->dev, dma)) {
			rc_printk(RC_ERROR,
				  "rc_volume_resync: DMA map failed on survivor %s at lba %llu — aborting\n",
				  pci_name(srv->pdev),
				  (unsigned long long)cursor);
			goto done;
		}
		err = rc_volume_resync_rw(srv, RC_NVME_NVM_OP_READ,
					  srv_lba, nlb, bytes, dma,
					  list_va[0], list_pa[0]);
		dma_unmap_single(&srv->pdev->dev, dma, bytes, DMA_FROM_DEVICE);
		if (err) {
			/* -EBUSY (shared SQ momentarily full) and -ETIMEDOUT
			 * are expected transients under concurrent blk-mq
			 * write traffic on the same queue — retry the chunk
			 * instead of abandoning the whole resync to a manual
			 * re-plug. */
			if ((err == -EBUSY || err == -ETIMEDOUT) &&
			    ++retries <= RC_RESYNC_MAX_RETRIES) {
				rc_printk(RC_WARN,
					  "rc_volume_resync: transient read error (%d) from survivor %d at lba %llu — retry %d/%d\n",
					  err, srv_slot,
					  (unsigned long long)cursor,
					  retries, RC_RESYNC_MAX_RETRIES);
				msleep(20);
				continue;
			}
			rc_printk(RC_ERROR,
				  "rc_volume_resync: read from survivor %d failed (%d) at lba %llu — aborting\n",
				  srv_slot, err, (unsigned long long)cursor);
			goto done;
		}

		dma = dma_map_single(&tgt->pdev->dev, buf, bytes,
				     DMA_TO_DEVICE);
		if (dma_mapping_error(&tgt->pdev->dev, dma)) {
			rc_printk(RC_ERROR,
				  "rc_volume_resync: DMA map failed on target %s at lba %llu — aborting\n",
				  pci_name(tgt->pdev),
				  (unsigned long long)cursor);
			goto done;
		}
		err = rc_volume_resync_rw(tgt, RC_NVME_NVM_OP_WRITE,
					  tgt_lba, nlb, bytes, dma,
					  list_va[1], list_pa[1]);
		dma_unmap_single(&tgt->pdev->dev, dma, bytes, DMA_TO_DEVICE);
		if (err) {
			if ((err == -EBUSY || err == -ETIMEDOUT) &&
			    ++retries <= RC_RESYNC_MAX_RETRIES) {
				rc_printk(RC_WARN,
					  "rc_volume_resync: transient write error (%d) to target %d at lba %llu — retry %d/%d\n",
					  err, target,
					  (unsigned long long)cursor,
					  retries, RC_RESYNC_MAX_RETRIES);
				msleep(20);
				continue;
			}
			rc_printk(RC_ERROR,
				  "rc_volume_resync: write to target %d failed (%d) at lba %llu — aborting\n",
				  target, err, (unsigned long long)cursor);
			goto done;
		}

		retries = 0;
		cursor += nlb;
		atomic64_set(&rc_volume_resync_cursor, cursor);

		if (rc_volume_resync_delay_ms)
			msleep(rc_volume_resync_delay_ms);
		cond_resched();
	}
	ret = 0;

done:
	atomic64_set(&rc_volume_resync_window, RC_RESYNC_WINDOW_NONE);
	if (buf)
		free_pages((unsigned long)buf, order);
	/* Free the PRP-list pages while the endpoints still exist.  The
	 * stop paths (remove_member/teardown) kthread_stop() BEFORE
	 * releasing either adapter, so these devices are valid here. */
	if (list_va[0] && srv_slot >= 0 && rc_volume_members[srv_slot])
		dma_free_coherent(&rc_volume_members[srv_slot]->pdev->dev,
				  PAGE_SIZE, list_va[0], list_pa[0]);
	if (list_va[1] && rc_volume_members[target])
		dma_free_coherent(&rc_volume_members[target]->pdev->dev,
				  PAGE_SIZE, list_va[1], list_pa[1]);

	mutex_lock(&rc_volume_lock);
	/* cmpxchg, not check-then-set: rc_volume_member_mark_failed() writes
	 * this state element lock-free from ISR context, and a FAILED it
	 * installs in the window between a plain check and store must not be
	 * clobbered back to LIVE (the volume would report optimal while
	 * routing I/O to a dead member).  If mark_failed wins, it already
	 * cleared the mask bits and scheduled the degrade work — losing the
	 * cmpxchg means there is nothing left for us to do. */
	if (ret == 0) {
		/* Masks first, state second, and revert the masks if the
		 * cmpxchg loses: whichever way a concurrent lock-free
		 * mark_failed() interleaves, exactly one side's bookkeeping
		 * survives.  mark_failed's not-live branch retries against
		 * LIVE, so a promotion it raced still gets failed rather
		 * than swallowed. */
		atomic_or(BIT(target), &rc_volume_write_mask);
		atomic_or(BIT(target), &rc_volume_live_mask);
		if (cmpxchg(&rc_volume_member_state[target],
			    RC_MEMBER_RESYNCING, RC_MEMBER_LIVE) !=
		    RC_MEMBER_RESYNCING) {
			/* A failure won the race — undo our mask bits (its
			 * andnots may have preceded our ors). */
			atomic_andnot(BIT(target), &rc_volume_write_mask);
			atomic_andnot(BIT(target), &rc_volume_live_mask);
			goto completion_settled;
		}
		rc_printk(RC_NOTE,
			  "rc_volume_resync: member %d resynced (%llu MiB) — volume OPTIMAL\n",
			  target,
			  (unsigned long long)(rc_volume_resync_total >> 11));
	} else if (raid5) {
		/* A parity rebuild writes the target's data, so a failure
		 * leaves it FAILED rather than parked: retrying a member
		 * that just failed the copy would spin the scheduler.  The
		 * operator replaces/resets it and calls START_RESYNC again. */
		if (cmpxchg(&rc_volume_member_state[target],
			    RC_MEMBER_RESYNCING, RC_MEMBER_FAILED) ==
		    RC_MEMBER_RESYNCING) {
			atomic_andnot(BIT(target), &rc_volume_write_mask);
			rc_printk(RC_WARN,
				  "rc_volume_resync: member %d rebuild aborted at %llu/%llu sectors — left FAILED\n",
				  target, (unsigned long long)cursor,
				  (unsigned long long)rc_volume_resync_total);
		}
	} else if (ret != 0 &&
		   cmpxchg(&rc_volume_member_state[target],
			   RC_MEMBER_RESYNCING, RC_MEMBER_NEEDS_RESYNC) ==
		   RC_MEMBER_RESYNCING) {
		atomic_andnot(BIT(target), &rc_volume_write_mask);
		rc_printk(RC_WARN,
			  "rc_volume_resync: member %d resync aborted at %llu/%llu sectors — parked needs-resync\n",
			  target, (unsigned long long)cursor,
			  (unsigned long long)rc_volume_resync_total);
	}
completion_settled:
	rc_volume_resync_slot = -1;
	rc_volume_resync_done = true;
	mutex_unlock(&rc_volume_lock);

	/* Exit outright — no parking for kthread_stop().  The starter holds
	 * a task_struct reference, so the reapers' kthread_stop() is safe
	 * after this return: on an already-exited kthread it just collects
	 * the exit code without blocking.  Parking here instead would leave
	 * a blocked rcraid-resync thread lingering after every successful
	 * resync until an unrelated readmit/remove/teardown reaped it. */
	/* A clean rebuild frees the single resync slot; let the scheduler
	 * pick up the next parked member (RAID10 can have several stale
	 * copies at once).  A FAILED rebuild is deliberately not retried
	 * automatically — a member that just failed the copy would spin
	 * read -> park -> retry forever. */
	if (ret == 0)
		schedule_work(&rc_volume_rebuild_work);
	return ret;
}

/* Reap a finished (or running) resync thread.  Caller must NOT hold
 * rc_volume_lock if the thread may still be running (its exit path takes
 * the lock).  With the lock held, only reap when rc_volume_resync_done. */
static void rc_volume_resync_reap_locked(void)
{
	struct task_struct *t = rc_volume_resync_thread;

	if (!t || !rc_volume_resync_done)
		return;
	rc_volume_resync_thread = NULL;
	rc_volume_resync_done = false;
	kthread_stop(t);	/* thread already exited; collects exit code */
	put_task_struct(t);
}

/* Stop a possibly-RUNNING resync.  Never called with rc_volume_lock held. */
static void rc_volume_resync_stop(void)
{
	struct task_struct *t;

	mutex_lock(&rc_volume_lock);
	t = rc_volume_resync_thread;
	rc_volume_resync_thread = NULL;
	rc_volume_resync_done = false;
	mutex_unlock(&rc_volume_lock);
	if (t) {
		kthread_stop(t);
		put_task_struct(t);
	}
}

/* Per-member sectors the resync engine copies: RAID1 spans the whole
 * (mirror) volume; RAID10 one column (volume / cols); RAID 5 one member's
 * data region (volume / (members - 1)).  Caller holds rc_volume_lock. */
static u64 rc_volume_per_member_sectors(void)
{
	u64 cap = rc_volume_disk ? get_capacity(rc_volume_disk) : 0;

	if (rc_volume_raid_level == RC_LDT_RAID10 && rc_volume_raid10_cols)
		cap = div_u64(cap, rc_volume_raid10_cols);
	else if (rc_volume_raid_level == RC_LDT_RAID5 &&
		 rc_volume_member_count > 1)
		cap = div_u64(cap, rc_volume_member_count - 1);
	return cap;
}

/* A member just entered NEEDS_RESYNC (re-plug, recovered controller, or
 * boot-time degraded assembly).  Caller holds rc_volume_lock.
 *
 * Members are rebuilt one at a time (the engine has a single resync slot),
 * so this only nudges the serialized scheduler below; the scheduler picks
 * the next parked slot whenever the current rebuild finishes.  That matters
 * for RAID10: unlike a 2-member RAID1, several columns can independently
 * have a stale copy, and each must be rebuilt in turn. */
static void rc_volume_member_readmitted(int slot)
{
	/* A teardown in progress dropped rc_volume_lock for the kernfs
	 * unregister — it already stopped the resync it knew about and will
	 * not look again, so spawning one now would orphan a kthread against
	 * adapters whose queues are about to be freed. */
	if (rc_volume_tearing_down) {
		rc_printk(RC_WARN,
			  "rc_volume: member %d needs resync but the volume is tearing down — leaving parked\n",
			  slot);
		return;
	}
	schedule_work(&rc_volume_rebuild_work);
}

/* Serialized rebuild scheduler.  Reaps a finished resync, then starts at
 * most one parked member whose survivor is live.  Re-armed by every
 * member_readmitted() and by the resync thread on clean completion, so N
 * stale members rebuild one after another.  Takes rc_volume_lock itself. */
static void rc_volume_rebuild_fn(struct work_struct *w)
{
	int slot, pick = -1;
	struct task_struct *t;

	mutex_lock(&rc_volume_lock);

	rc_volume_resync_reap_locked();

	if (rc_volume_tearing_down || rc_volume_resync_thread ||
	    !rc_volume_disk ||
	    (get_disk_ro(rc_volume_disk) &&
	     rc_volume_raid_level != RC_LDT_RAID5))
		goto out;

	for (slot = 0;
	     slot < rc_volume_member_count && slot < RC_VOLUME_MAX_MEMBERS;
	     slot++) {
		enum rc_member_state state;

		if (!rc_volume_members[slot])
			continue;
		state = READ_ONCE(rc_volume_member_state[slot]);
		if (rc_volume_raid_level == RC_LDT_RAID5) {
			/* Rebuild a failed member (or a re-plugged one that
			 * came back parked) from the other N-1 live members.
			 * Two dead members are fatal and never reach here. */
			if (state != RC_MEMBER_FAILED &&
			    state != RC_MEMBER_NEEDS_RESYNC)
				continue;
			if (hweight32((u32)atomic_read(&rc_volume_live_mask)) !=
			    (u32)rc_volume_member_count - 1)
				continue;
			if (READ_ONCE(rc_volume_members[slot]->ctx.nvme.dead))
				continue;
			pick = slot;
			break;
		}
		if (state != RC_MEMBER_NEEDS_RESYNC)
			continue;
		if (rc_volume_resync_pick_survivor(slot) < 0)
			continue;
		pick = slot;
		break;
	}

	if (pick < 0)
		goto out;

	rc_volume_member_set_state(pick, RC_MEMBER_RESYNCING);
	rc_volume_resync_slot = pick;
	rc_volume_resync_total = rc_volume_per_member_sectors();
	atomic64_set(&rc_volume_resync_cursor, 0);

	t = kthread_run(rc_volume_resync_fn, (void *)(long)pick,
			"rcraid-resync");
	if (IS_ERR(t)) {
		rc_printk(RC_ERROR,
			  "rc_volume: failed to start resync thread (%ld)\n",
			  PTR_ERR(t));
		rc_volume_member_set_state(pick,
			(rc_volume_raid_level == RC_LDT_RAID5) ?
			RC_MEMBER_FAILED : RC_MEMBER_NEEDS_RESYNC);
		rc_volume_resync_slot = -1;
		goto out;
	}
	get_task_struct(t);
	rc_volume_resync_thread = t;
	/* Clear any stale done-flag: rc_volume_resync_stop() NULLs the
	 * thread pointer before kthread_stop(), so the stopped thread's
	 * exit path re-set done = true after the stopper cleared it, and
	 * reap_locked() above skips the reset when the pointer is NULL.
	 * Left stale, the next reap would kthread_stop() THIS live thread
	 * mid-copy as if it had finished. */
	rc_volume_resync_done = false;
out:
	mutex_unlock(&rc_volume_lock);
}

/* Tear down everything rc_volume_create_disk allocated.  Called from
 * rc_exit().  Safe to call when the disk was never created (state is
 * initialised to NULL/0). */
/* Serializes whole-teardown against whole-teardown (two members surprise-
 * removed at once, or hot-unplug racing rc_exit).  Without it, one caller
 * can del_gendisk/put_disk the disk while the other is still inside
 * device_remove_group() on it.  NOT rc_volume_lock, so the sysfs show/
 * store handlers (which take rc_volume_lock) can still drain during
 * kernfs removal. */
static DEFINE_MUTEX(rc_volume_teardown_lock);

void rc_volume_teardown(void)
{
	int i;

	mutex_lock(&rc_volume_teardown_lock);

	/* Block NEW resyncs before stopping the current one: once the lock
	 * is dropped for rc_volume_sysfs_unregister() below, a per-adapter
	 * auto_reset_work queued earlier can still run and would otherwise
	 * call rc_volume_member_readmitted() and have the rebuild scheduler
	 * spawn a fresh resync kthread this function never learns about —
	 * which then races the adapters' own queue teardown (use-after-free). */
	mutex_lock(&rc_volume_lock);
	rc_volume_tearing_down = true;
	mutex_unlock(&rc_volume_lock);

	/* The degrade work walks the registry — let any queued run finish
	 * before the registry (and the adapters it points at) go away.  The
	 * delayed degraded-assembly work must likewise not fire into a
	 * dismantled registry. */
	flush_work(&rc_volume_degrade_work);
	cancel_delayed_work_sync(&rc_volume_assemble_work);
	/* Stop a running resync before the members it reads/writes go away. */
	rc_volume_resync_stop();
	/* The rebuild scheduler may have been re-armed by the resync's exit
	 * path or by a registration racing the teardown.  tearing_down (set
	 * above) makes any run that starts from here on bail before spawning;
	 * this drops a run queued earlier that has not started.  A run already
	 * in flight is serialized against the tearing_down store by
	 * rc_volume_lock, so it either bailed or had its kthread caught by
	 * rc_volume_resync_stop() just above. */
	cancel_work_sync(&rc_volume_rebuild_work);

	/* Remove the sysfs group BEFORE taking rc_volume_lock: kernfs
	 * removal blocks until in-flight ->show()/->store() callbacks
	 * return, and those callbacks take rc_volume_lock — removing under
	 * the lock is an ABBA deadlock with any concurrent attribute read.
	 * Concurrent teardown callers are serialized by
	 * rc_volume_teardown_lock (held above), and the unregister itself is
	 * idempotent via rc_volume_sysfs_up, so the group is removed exactly
	 * once and the disk outlives the removal. */
	rc_volume_sysfs_unregister();

	mutex_lock(&rc_volume_lock);
	if (rc_volume_disk) {
		del_gendisk(rc_volume_disk);
		put_disk(rc_volume_disk);
		rc_volume_disk = NULL;
		blk_mq_free_tag_set(&rc_volume_tagset);
		rc_volume_bounce_pool_free();
	}
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++) {
		if (rc_volume_members[i]) {
			rc_volume_free_member_dma(i,
				&rc_volume_members[i]->pdev->dev);
			/* Ending the volume ends the membership.  DELETE keeps
			 * the adapters bound, and the inventory reports
			 * is_assigned from volume_slot: a slot left set here
			 * would make the deleted array's disks look like members
			 * of nothing and lock them out of the next CREATE. */
			rc_volume_members[i]->ctx.nvme.volume_slot = -1;
		}
		rc_volume_members[i] = NULL;
		rc_volume_member_phys_offset[i] = 0;
		WRITE_ONCE(rc_volume_member_state[i], RC_MEMBER_LIVE);
	}
	atomic_set(&rc_volume_live_mask, 0);
	atomic_set(&rc_volume_write_mask, 0);
	atomic64_set(&rc_volume_resync_window, RC_RESYNC_WINDOW_NONE);
	atomic64_set(&rc_volume_resync_cursor, 0);
	/* Symmetry/defense-in-depth: the queue is drained here so these are
	 * already 0 in any correct execution, but a count leaked across a
	 * teardown/reassemble cycle would hang the next volume's resync in
	 * its generation-drain loop with no diagnostic. */
	atomic_set(&rc_volume_wgen, 0);
	atomic_set(&rc_volume_wgen_count[0], 0);
	atomic_set(&rc_volume_wgen_count[1], 0);
	rc_volume_resync_total = 0;
	rc_volume_resync_slot = -1;
	rc_volume_member_count = 0;
	rc_volume_stripe_sectors = 0;
	rc_volume_raid10_cols = 0;
	rc_volume_parity_pool_free();
	/* Re-open resyncs: this path also runs for a mid-runtime last-member
	 * removal, and a later re-probe must be able to rebuild. */
	rc_volume_tearing_down = false;
	mutex_unlock(&rc_volume_lock);
	mutex_unlock(&rc_volume_teardown_lock);
}

/* debugfs `rcraid/volume` — volume-level state for operators and the QEMU
 * failure-injection rig.  Pure readers only (no dead-flag folding here;
 * the hot path owns state transitions). */
int rc_volume_debugfs_show(struct seq_file *m, void *unused)
{
	unsigned int live;
	int nlive = 0;
	int i;

	mutex_lock(&rc_volume_lock);
	live = (unsigned int)atomic_read(&rc_volume_live_mask);

	seq_printf(m, "volume: %s\n",
		   rc_volume_disk ? rc_volume_disk->disk_name : "none");
	seq_printf(m, "level: %s\n",
		   rc_volume_raid_level == RC_LDT_RAID1 ? "raid1" :
		   rc_volume_raid_level == RC_LDT_RAID10 ? "raid10" :
		   rc_volume_raid_level == RC_LDT_RAID5 ? "raid5" :
		   rc_volume_raid_level == RC_LDT_RAID6 ? "raid6" :
		   rc_volume_raid_level == RC_LDT_RAID0 ? "raid0" : "unknown");
	seq_printf(m, "members: %d\n", rc_volume_member_count);
	seq_printf(m, "live_mask: 0x%x\n", live);

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		struct rc_adapter *a = rc_volume_members[i];

		if (live & BIT(i))
			nlive++;
		seq_printf(m, "member%d: %s %s%s\n", i,
			   a ? pci_name(a->pdev) : "absent",
			   rc_member_state_name(READ_ONCE(rc_volume_member_state[i])),
			   (a && READ_ONCE(a->ctx.nvme.dead)) ? " (dead)" : "");
	}

	if (rc_volume_resync_slot >= 0) {
		u64 cur = (u64)atomic64_read(&rc_volume_resync_cursor);
		u64 tot = rc_volume_resync_total;

		seq_printf(m, "resync: member%d %llu/%llu sectors (%llu%%)\n",
			   rc_volume_resync_slot,
			   (unsigned long long)cur, (unsigned long long)tot,
			   tot ? (unsigned long long)div64_u64(cur * 100, tot)
			       : 0ULL);
	}

	if (!rc_volume_disk)
		seq_puts(m, "state: unassembled\n");
	else if (nlive == 0)
		seq_puts(m, "state: failed\n");
	else if (rc_volume_resync_slot >= 0)
		seq_puts(m, "state: resyncing\n");
	else if (nlive < rc_volume_member_count &&
		 (rc_volume_raid_level == RC_LDT_RAID1 ||
		  (rc_volume_raid_level == RC_LDT_RAID10 &&
		   rc_volume_can_serve())))
		seq_puts(m, "state: degraded\n");
	else if (nlive < rc_volume_member_count)
		seq_puts(m, "state: failed\n");
	else
		seq_puts(m, "state: optimal\n");
	mutex_unlock(&rc_volume_lock);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Volume-level sysfs: /sys/block/rcraid0/rcraid/{state,members,
 * resync_progress,fail_member}.  The stable operator surface (debugfs
 * carries the same data plus internals, but debugfs is not ABI and may
 * not be mounted).  Group added after add_disk, removed at teardown.
 * ------------------------------------------------------------------ */

static const char *rc_volume_state_str(void)
{
	unsigned int live = (unsigned int)atomic_read(&rc_volume_live_mask);
	int nlive = hweight32(live);

	if (!rc_volume_disk)
		return "unassembled";
	if (nlive == 0)
		return "failed";
	if (READ_ONCE(rc_volume_resync_slot) >= 0)
		return "resyncing";
	if (nlive < rc_volume_member_count) {
		if (rc_volume_raid_level == RC_LDT_RAID1)
			return "degraded";
		if (rc_volume_raid_level == RC_LDT_RAID10 &&
		    rc_volume_can_serve())
			return "degraded";
		if (rc_volume_raid_level == RC_LDT_RAID5 &&
		    rc_volume_can_serve())
			return "degraded";
		return "failed";
	}
	return "optimal";
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	ssize_t n;

	mutex_lock(&rc_volume_lock);
	n = sysfs_emit(buf, "%s\n", rc_volume_state_str());
	mutex_unlock(&rc_volume_lock);
	return n;
}
static DEVICE_ATTR_RO(state);

static ssize_t members_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	ssize_t n = 0;
	int i;

	mutex_lock(&rc_volume_lock);
	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		struct rc_adapter *a = rc_volume_members[i];

		n += sysfs_emit_at(buf, n, "%d %s %s\n", i,
				   a ? pci_name(a->pdev) : "absent",
				   rc_member_state_name(rc_volume_member_state[i]));
	}
	mutex_unlock(&rc_volume_lock);
	return n;
}
static DEVICE_ATTR_RO(members);

static ssize_t resync_progress_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	u64 cur, tot;
	int slot;
	ssize_t n;

	mutex_lock(&rc_volume_lock);
	slot = rc_volume_resync_slot;
	cur = (u64)atomic64_read(&rc_volume_resync_cursor);
	tot = rc_volume_resync_total;
	if (slot < 0)
		n = sysfs_emit(buf, "none\n");
	else
		n = sysfs_emit(buf, "member%d %llu/%llu %llu%%\n", slot,
			       (unsigned long long)cur,
			       (unsigned long long)tot,
			       tot ? (unsigned long long)div64_u64(cur * 100,
								   tot) : 0ULL);
	mutex_unlock(&rc_volume_lock);
	return n;
}
static DEVICE_ATTR_RO(resync_progress);

/* UAPI raid-level spelling.  The kernel's RC_LDT_* values are on-disk
 * DeviceType tags, not the numbers userspace expects. */
static u32 rc_volume_uapi_raid_level(void)
{
	switch (rc_volume_raid_level) {
	case RC_LDT_RAID0:  return 0;
	case RC_LDT_RAID1:  return 1;
	case RC_LDT_RAID10: return 10;
	case RC_LDT_RAID5:  return 5;
	case RC_LDT_RAID6:  return 6;
	default:            return 0xff;
	}
}

/* Live status for AMD_IOCTL_GET_STATUS.  Bit 0 only says the control
 * plane is up; FAILED/DEGRADED come from the assembled volume. */
int rc_volume_get_status(u32 *mask)
{
	u32 m = RC_AMD_STATUS_LIVE;
	bool failed_member = false;
	bool can_serve;
	int i;

	if (!mask)
		return -EINVAL;

	mutex_lock(&rc_volume_lock);
	can_serve = rc_volume_disk && rc_volume_can_serve();
	if (!can_serve)
		m |= RC_AMD_STATUS_FAILED;
	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		if (READ_ONCE(rc_volume_member_state[i]) == RC_MEMBER_FAILED) {
			failed_member = true;
			break;
		}
	}
	if (failed_member)
		m |= RC_AMD_STATUS_FAULTS;
	if (failed_member && can_serve)
		m |= RC_AMD_STATUS_DEGRADED;
	if (rc_volume_resync_slot >= 0)
		m |= RC_AMD_STATUS_RESYNCING;
	mutex_unlock(&rc_volume_lock);

	*mask = m;
	return 0;
}
EXPORT_SYMBOL_GPL(rc_volume_get_status);

/* Fill the rcraidXpert inventory payload from the live adapter/volume
 * registries.  Both locks are held for the snapshot; the lock order is
 * state -> volume, and no path takes them in the opposite order. */
int rc_volume_get_inventory(struct AmdHardwarePayload *out)
{
	int slot_to_drive[RC_VOLUME_MAX_MEMBERS];
	int i, n = 0;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++)
		slot_to_drive[i] = -1;

	/*
	 * Hold both registries while the snapshot is taken.  The lock order is
	 * state -> volume; detach takes state only, and the volume paths take
	 * volume only, so there is no reverse nesting.
	 */
	mutex_lock(&rc_state.lock);
	mutex_lock(&rc_volume_lock);
	for (i = 0; i < RC_MAX_ADAPTERS && n < 64; i++) {
		struct rc_adapter *a = rc_state.adapters[i];
		struct AmdDriveTelemetry *d;
		int slot;

		if (!a)
			continue;
		d = &out->drives[n];
		d->port_id = a->pdev->bus->number;
		d->slot_id = (__u8)PCI_SLOT(a->pdev->devfn);
		d->disk_index = n;
		d->total_sectors = a->ctx.nvme.ns1_nsze;
		slot = READ_ONCE(a->ctx.nvme.volume_slot);
		d->is_assigned = slot >= 0;
		if (slot >= 0 && slot < RC_VOLUME_MAX_MEMBERS)
			slot_to_drive[slot] = n;
		n++;
	}
	out->total_discovered_drives = n;

	if (rc_volume_disk) {
		struct AmdArrayTelemetry *arr = &out->arrays[0];

		arr->array_id = 0;
		arr->raid_level = rc_volume_uapi_raid_level();
		arr->stripe_sectors = rc_volume_stripe_sectors;
		arr->total_capacity_bytes =
			(u64)get_capacity(rc_volume_disk) * 512u;
		arr->member_disk_count = rc_volume_member_count;
		for (i = 0; i < rc_volume_member_count && i < 32; i++)
			arr->member_indices[i] = slot_to_drive[i] >= 0 ?
				(__u8)slot_to_drive[i] : 0xff;
		strscpy(arr->name, rc_volume_disk->disk_name,
			sizeof(arr->name));
		out->total_discovered_arrays = 1;
	}
	mutex_unlock(&rc_volume_lock);
	mutex_unlock(&rc_state.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(rc_volume_get_inventory);

/* Snapshot the assembled volume for the /dev/rc_api packet builder.
 * Everything is copied out under rc_volume_lock; the builder never holds
 * a pointer into driver state.  Degraded assembly leaves NULL slots, so
 * absent members are simply not reported. */
int rc_api_snapshot_get(struct rc_api_snapshot *out)
{
	u32 i;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	mutex_lock(&rc_state.lock);
	mutex_lock(&rc_volume_lock);
	out->level = rc_volume_raid_level;
	out->stripe_sectors = rc_volume_stripe_sectors;
	if (rc_volume_disk)
		out->capacity_sectors = get_capacity(rc_volume_disk);

	/*
	 * Report every bound member with valid metadata, not only the ones in
	 * the assembled volume: after DELETE the adapters stay bound and rcadm
	 * still needs them in the disk list before it can be asked to CREATE.
	 * Volume members keep their slot state; free disks report online with
	 * no array position.
	 */
	for (i = 0; i < RC_MAX_ADAPTERS && out->nr_members < RC_API_PD_MAX;
	     i++) {
		struct rc_adapter *a = rc_state.adapters[i];
		struct rc_api_member *m;
		struct rc_nvme_state *nvme;
		int slot;

		if (!a || !a->ctx.nvme.md_valid)
			continue;
		nvme = &a->ctx.nvme;
		m = &out->member[out->nr_members++];
		m->device_id = nvme->md_device_id;
		m->capacity_sectors = nvme->ns1_nsze;
		m->ld_position = nvme->ld_my_position < 0 ? 0 :
			(u32)nvme->ld_my_position;
		m->online = 1;
		slot = READ_ONCE(nvme->volume_slot);
		if (slot >= 0 && slot < rc_volume_member_count)
			m->online = READ_ONCE(rc_volume_member_state[slot]) ==
				    RC_MEMBER_LIVE;
		/* A member of the assembled volume has no space to offer a
		 * CREATE; a free disk offers everything past the firmware
		 * reservation.  Reporting zero here made `rcadm -C` size a
		 * new array at 16384 MiB on 16 TB of free disks. */
		m->free_sectors = 0;
		if (!rc_volume_disk || slot < 0 ||
		    slot >= rc_volume_member_count)
			m->free_sectors = rc_mb_user_sectors(nvme->ns1_nsze, 1);
		m->vid = nvme->id_vid;
		m->ssvid = nvme->id_ssvid;
		m->sub_vid = a->pdev->subsystem_vendor;
		m->sub_dev = a->pdev->subsystem_device;
		m->pci_location = ((u32)a->pdev->bus->number << 16) |
				  ((u32)PCI_SLOT(a->pdev->devfn) << 8) |
				  (u32)PCI_FUNC(a->pdev->devfn);
		memcpy(m->firmware, nvme->id_firmware, sizeof(m->firmware));
		memcpy(m->model, nvme->id_model, sizeof(m->model));
		memcpy(m->serial, nvme->id_serial,
		       sizeof(nvme->id_serial));

		if (!out->have_ld && nvme->ld_valid && nvme->ld_raw_len) {
			out->have_ld = 1;
			out->ld_len = min_t(u32, nvme->ld_raw_len,
					    RC_API_LD_MAX);
			memcpy(out->ld_raw, nvme->ld_raw, out->ld_len);
		}
	}
	mutex_unlock(&rc_volume_lock);
	mutex_unlock(&rc_state.lock);

	return 0;
}

/* Fail a live member by volume slot.  Shared by sysfs and the amd_ctl0
 * bridge so both paths have identical bounds checks and locking. */
int rc_volume_fail_member_slot(int slot)
{
	int ret = 0;

	if (slot < 0 || slot >= RC_VOLUME_MAX_MEMBERS)
		return -EINVAL;

	mutex_lock(&rc_volume_lock);
	/* member_count is lock-protected state — bounds-check it here. */
	if (!rc_volume_disk) {
		ret = -ENODEV;
	} else if (slot >= rc_volume_member_count) {
		ret = -EINVAL;
	} else if (!rc_volume_members[slot]) {
		ret = -ENODEV;
	} else {
		rc_printk(RC_WARN,
			  "rc_volume: member %d (%s) FAILED by operator\n",
			  slot, pci_name(rc_volume_members[slot]->pdev));
		/* mark_failed itself is lock-free (ISR-safe), so holding
		 * rc_volume_lock keeps validate+fail atomic against a
		 * concurrent remove/re-add reusing this slot. */
		rc_volume_member_mark_failed(slot);
	}
	mutex_unlock(&rc_volume_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rc_volume_fail_member_slot);

/* Nudge the live resync scheduler.  RAID1/RAID10 pick a parked
 * NEEDS_RESYNC member; RAID 5 picks a FAILED (or re-plugged NEEDS_RESYNC)
 * member while the other N-1 are live.  RAID 0 and RAID 6 have no rebuild
 * path.  A parity volume is allowed to rebuild while its block device is
 * read-only: the engine writes the member directly, not through the disk. */
int rc_volume_request_resync(void)
{
	int ret = 0;
	int i;
	bool parked = false;
	bool raid5;

	mutex_lock(&rc_volume_lock);
	raid5 = (rc_volume_raid_level == RC_LDT_RAID5);
	if (!rc_volume_disk) {
		ret = -ENODEV;
	} else if (rc_volume_raid_level != RC_LDT_RAID1 &&
		   rc_volume_raid_level != RC_LDT_RAID10 && !raid5) {
		ret = -EOPNOTSUPP;
	} else if (!raid5 && get_disk_ro(rc_volume_disk)) {
		ret = -EROFS;
	} else {
		for (i = 0;
		     i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
		     i++) {
			enum rc_member_state state;

			if (!rc_volume_members[i])
				continue;
			state = READ_ONCE(rc_volume_member_state[i]);
			if (raid5) {
				if (state != RC_MEMBER_FAILED &&
				    state != RC_MEMBER_NEEDS_RESYNC)
					continue;
				if (hweight32((u32)atomic_read(&rc_volume_live_mask)) !=
				    (u32)rc_volume_member_count - 1)
					continue;
				if (READ_ONCE(rc_volume_members[i]->ctx.nvme.dead))
					continue;
				parked = true;
				break;
			}
			if (state == RC_MEMBER_NEEDS_RESYNC) {
				parked = true;
				break;
			}
		}
		if (!parked)
			ret = -ENOENT;
		else
			schedule_work(&rc_volume_rebuild_work);
	}
	mutex_unlock(&rc_volume_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rc_volume_request_resync);

/* Operator/testing control: fail a member by slot number.  Same code path
 * as an ISR-detected death — the volume degrades (RAID1) or fails
 * (RAID0), and a subsequent manual reset of the member parks it
 * needs-resync for the engine to pick up. */
static ssize_t fail_member_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int slot, ret;

	ret = kstrtoint(buf, 0, &slot);
	if (ret)
		return ret;
	ret = rc_volume_fail_member_slot(slot);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(fail_member);

static struct attribute *rc_volume_sysfs_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_members.attr,
	&dev_attr_resync_progress.attr,
	&dev_attr_fail_member.attr,
	NULL,
};

static const struct attribute_group rc_volume_sysfs_group = {
	.name  = "rcraid",
	.attrs = rc_volume_sysfs_attrs,
};

/* Guards the register/unregister pair so device_remove_group() fires
 * exactly once.  Deliberately NOT rc_volume_lock: unregister must run
 * outside that lock (kernfs removal drains ->show()/->store() callbacks
 * which take it), yet concurrent rc_volume_teardown() callers — two
 * members surprise-removed at once, or hot-unplug racing rc_exit() —
 * would otherwise both see rc_volume_disk non-NULL and double-remove
 * the group (kernfs WARNs on the second call). */
static atomic_t rc_volume_sysfs_up = ATOMIC_INIT(0);

/* Called with rc_volume_lock held, right after add_disk. */
static void rc_volume_sysfs_register(void)
{
	int ret;

	if (!rc_volume_disk)
		return;
	ret = device_add_group(disk_to_dev(rc_volume_disk),
			       &rc_volume_sysfs_group);
	if (ret)
		rc_printk(RC_WARN,
			  "rc_volume: sysfs group creation failed (%d) — /sys/block/%s/rcraid absent\n",
			  ret, rc_volume_disk->disk_name);
	else
		atomic_set(&rc_volume_sysfs_up, 1);
}

/* Called WITHOUT rc_volume_lock (see rc_volume_teardown).  Idempotent:
 * only the caller that wins the flag actually removes the group. */
static void rc_volume_sysfs_unregister(void)
{
	if (atomic_cmpxchg(&rc_volume_sysfs_up, 1, 0) != 1)
		return;
	device_remove_group(disk_to_dev(rc_volume_disk),
			    &rc_volume_sysfs_group);
}

/* PCI-remove hook (also used by probe error paths).  A member adapter is
 * about to be kfree'd; if it is registered in rc_volume_members[] the
 * assembled volume still dereferences it on every I/O — surprise-remove
 * or sysfs unbind of one member with /dev/rcraid0 mounted was a
 * use-after-free on the hot path.
 *
 * RAID1 with a surviving live mirror: per-member detach — mark the slot
 * FAILED, drain I/O waiting on it, freeze the queue to flush lingering
 * completions, NULL the slot, and keep the volume serving degraded.
 *
 * Everything else (RAID0, last mirror): the whole volume is torn down —
 * mark the departing member dead, fail the in-flight I/O cleanly (drain
 * disables the dead controller first, per H-1), then dismantle the
 * gendisk + DMA pools.  No-op when the adapter never registered. */
void rc_volume_remove_member(struct rc_adapter *adapter)
{
	bool keep_volume;
	int slot = -1;
	int i;

	/* Bail before disturbing anything if this adapter never registered
	 * as a member (foreign NVMe device under allow_foreign_nvme, or a
	 * probe failure before registration).  This path runs for EVERY
	 * NVMe-mode adapter removal; unconditionally stopping the resync
	 * here let churn on an unrelated device abort an in-progress
	 * rebuild — with no automatic restart, stranding the volume
	 * degraded. */
	mutex_lock(&rc_volume_lock);
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++)
		if (rc_volume_members[i] == adapter)
			slot = i;
	mutex_unlock(&rc_volume_lock);
	if (slot < 0) {
		/* Not currently registered — but an adapter that WAS a
		 * member (volume_slot still set: e.g. the registry was
		 * cleared by a concurrent teardown) may still be referenced
		 * by a resync kthread's cached pointers.  Stop it before our
		 * caller frees this adapter's I/O queues out from under it.
		 * Never-registered foreign devices (volume_slot -1) skip
		 * this, so unrelated churn still can't abort a rebuild. */
		if (READ_ONCE(adapter->ctx.nvme.volume_slot) >= 0)
			rc_volume_resync_stop();
		return;
	}
	slot = -1;

	/* The degraded-assembly timer walks the registry and can create the
	 * disk from a member that is mid-removal — cancel it before any
	 * member departs.  A later registration of a remaining member
	 * re-arms it. */
	cancel_delayed_work_sync(&rc_volume_assemble_work);

	/* A running resync holds pointers to (up to) two members and issues
	 * sync commands against them — stop it before ANY member departs.
	 * Cheap no-op when no resync is active.  If the departing member
	 * was the resync target, its state reverts to needs-resync (or is
	 * overwritten to failed below); a future re-add restarts the copy
	 * from scratch. */
	rc_volume_resync_stop();

	mutex_lock(&rc_volume_lock);
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++)
		if (rc_volume_members[i] == adapter)
			slot = i;
	/* RAID1 with another live member keeps serving degraded; RAID10 keeps
	 * serving as long as no mirror pair loses BOTH copies.  Everything
	 * else (RAID0, a defeated RAID10 pair, the last mirror, pre-disk
	 * assembly) tears down as before.  Decided under the lock so a
	 * concurrent removal of the other copy can't leave both paths
	 * thinking a survivor exists. */
	keep_volume = slot >= 0 && rc_volume_disk &&
		      (rc_volume_raid_level == RC_LDT_RAID1 ||
		       rc_volume_raid_level == RC_LDT_RAID10) &&
		      rc_volume_can_serve_without(slot);
	if (keep_volume)
		rc_volume_member_set_state(slot, RC_MEMBER_FAILED);
	mutex_unlock(&rc_volume_lock);

	if (slot < 0)
		return;

	if (!keep_volume) {
		rc_printk(RC_WARN,
			  "rc_volume_remove_member: %s is a live volume member with no surviving copy — tearing down /dev/rcraid0 before releasing the adapter\n",
			  pci_name(adapter->pdev));

		/* Fail fast: no new dispatches reach this adapter, and every
		 * in-flight request touching it is ended before the teardown
		 * (del_gendisk would otherwise wait forever on requests whose
		 * CQEs can no longer arrive). */
		WRITE_ONCE(adapter->ctx.nvme.dead, true);
		rc_volume_drain_dead();
		rc_volume_teardown();
		return;
	}

	rc_printk(RC_WARN,
		  "rc_volume_remove_member: %s removed — %s volume continues DEGRADED on the surviving copy/mirror\n",
		  pci_name(adapter->pdev),
		  rc_volume_raid_level == RC_LDT_RAID10 ? "RAID10" : "RAID1");

	/* Order matters: dead + drain BEFORE the slot is NULLed (the drain
	 * disables the departing controller and synthesizes completions for
	 * requests waiting on it — both need the pointer), and the slot is
	 * NULLed BEFORE our caller frees the adapter (after that, every
	 * registry consumer must see NULL, not a dangling pointer).
	 * Gendisk, tagset, and the volume-lifetime PRP pool stay intact —
	 * a NULL slot is simply never dispatched to. */
	WRITE_ONCE(adapter->ctx.nvme.dead, true);
	rc_volume_drain_dead();

	/* Freeze waits for every in-flight request to fully END (drain only
	 * initiated their completion) — after this, no .complete/unmap path
	 * can still hold the departing member's device pointer.  Then the
	 * slot can be NULLed and the adapter freed safely. */
	{
		unsigned int memflags =
			blk_mq_freeze_queue(rc_volume_disk->queue);

		mutex_lock(&rc_volume_lock);
		rc_volume_members[slot] = NULL;
		adapter->ctx.nvme.volume_slot = -1;
		/* Free this member's PRP-pool pages NOW, while its device
		 * (and IOMMU domain) still exists — the volume-teardown path
		 * can't dma_free_coherent against a departed device, and the
		 * freeze above guarantees nothing references them anymore.
		 * The NULL slot is never dispatched to; a re-add reallocates
		 * the slice via rc_volume_alloc_member_dma. */
		rc_volume_free_member_dma(slot, &adapter->pdev->dev);
		mutex_unlock(&rc_volume_lock);
		blk_mq_unfreeze_queue(rc_volume_disk->queue, memflags);
	}

	/* The degrade work (scheduled by mark_failed / set_state paths) may
	 * have snapshot the old pointer — let any in-flight run finish
	 * before the caller kfree's the adapter. */
	flush_work(&rc_volume_degrade_work);
	cancel_work_sync(&adapter->ctx.nvme.auto_reset_work);
}

/* Counterpart to create_io_queues; called from cleanup_controller and
 * from the reset path.  Iterates the queue array and tears each one
 * down via rc_nvme_destroy_one_io_queue (handles Delete SQ + CQ +
 * free_irq + DMA release per queue). */
static void rc_nvme_destroy_io_queues(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u16 i;

	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		if (nvme->io_queues[i]) {
			rc_nvme_destroy_one_io_queue(adapter, nvme->io_queues[i]);
			nvme->io_queues[i] = NULL;
		}
	}
}

/* workqueue handler scheduled from rc_volume_timeout when an adapter
 * is flagged dead.  Resolves rc_adapter via container_of, race-checks
 * `dead` (a manual reset may have already recovered it), attempts the
 * reset, and latches auto_reset_disabled=true if it fails so we don't
 * thrash a genuinely-fried controller.  One auto-reset attempt per
 * death episode; the next .timeout for a fresh death still fires after
 * a successful recovery clears the latch. */
static void rc_nvme_auto_reset_fn(struct work_struct *w)
{
	struct rc_nvme_state *nvme =
		container_of(w, struct rc_nvme_state, auto_reset_work);
	struct rc_dev_context *ctx =
		container_of(nvme, struct rc_dev_context, nvme);
	struct rc_adapter *adapter =
		container_of(ctx, struct rc_adapter, ctx);
	int ret;

	if (!READ_ONCE(nvme->dead)) {
		rc_printk(RC_INFO,
			  "rc_nvme_auto_reset_fn: %s already recovered — skipping\n",
			  pci_name(adapter->pdev));
		return;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_auto_reset_fn: %s attempting automatic reset\n",
		  pci_name(adapter->pdev));

	ret = rc_nvme_reset_controller(adapter);
	if (ret) {
		WRITE_ONCE(nvme->auto_reset_disabled, true);
		rc_printk(RC_ERROR,
			  "rc_nvme_auto_reset_fn: %s reset failed (%d); auto-reset disabled — manual sysfs reset required\n",
			  pci_name(adapter->pdev), ret);
	}
}

/* Manual recovery for a stuck or dead adapter.  Triggered via the sysfs
 * `reset` attribute.  Brings the controller down, re-initialises the
 * admin queue against the existing DMA buffers, re-creates the I/O queue
 * pair, and clears the dead flag.  Per-tag I/O DMA buffers in
 * rc_volume_dma_va[][] are untouched and get reused as blk-mq
 * re-dispatches.
 *
 * Sequence:
 *   1. Flag dead + quiesce + drain so no .queue_rq runs and any
 *      in-flight request whose CID is about to be wiped fails cleanly.
 *   2. Take admin_mutex for the whole bring-up so a concurrent
 *      .timeout-issued Abort can't collide.
 *   3. disable_irq → wait for any in-flight ISR to finish before we
 *      touch hardware registers.
 *   4. Mask INTMS, CC.EN=0, wait CSTS.RDY=0.
 *   5. Zero SQ/CQ buffers + reset tail/head/phase so the next CQE
 *      written by the controller is recognised.
 *   6. Re-program AQA/ASQ/ACQ, CC.EN=1, wait CSTS.RDY=1.
 *   7. Unmask INTMC + enable_irq.  Done before the admin commands so
 *      the submitter wakes via the ISR rather than the 1 ms poll
 *      fallback.
 *   8. Re-issue Create I/O CQ + Create I/O SQ on the same DMA buffers
 *      (the controller forgot them across CC.EN=0).
 *   9. Clear dead, unquiesce — new I/O resumes.
 *
 * On any step's failure the adapter is left flagged dead and the
 * function returns -EIO.  Module reload is then required.
 *
 * Identify is intentionally not re-issued: a successful reset on the
 * same physical controller should leave CAP/MDTS/namespace values
 * unchanged.  If a later validation step disagrees, that's a separate
 * recovery problem (different firmware, hot-swap, etc.).
 */
int rc_nvme_reset_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd;
	u32 cc, csts, aqa, cur_cc;
	u64 asq_rb, acq_rb;
	u16 saved_nr_io_queues;
	u16 i;
	int ret;

	if (!base) {
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s no MMIO base\n",
			  pci_name(adapter->pdev));
		return -EINVAL;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_reset_controller: %s reset requested\n",
		  pci_name(adapter->pdev));

	/* Flag dead unconditionally — even an operator-initiated reset
	 * on a still-alive controller wipes outstanding CIDs across the
	 * CC.EN=0 cycle, so any in-flight requests must be failed. */
	WRITE_ONCE(nvme->dead, true);

	if (rc_volume_disk) {
		blk_mq_quiesce_queue(rc_volume_disk->queue);
		rc_volume_drain_dead();
	}

	mutex_lock(&nvme->admin_mutex);

	/* Disable all per-queue MSI-X vectors (and the admin vector via
	 * adapter->irq_vector) so no ISR runs while we touch registers. */
	if (adapter->irq_vector >= 0)
		disable_irq(adapter->irq_vector);
	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];
		if (q && q->irq_vector >= 0)
			disable_irq(q->irq_vector);
	}

	writel(0xffffffffu, base + RC_NVME_REG_INTMS);

	cur_cc = readl(base + RC_NVME_REG_CC);
	if (cur_cc & RC_NVME_CC_EN) {
		writel(cur_cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);
		wmb();
	}
	ret = rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, 0);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s disable failed (%d) — adapter remains dead\n",
			  pci_name(adapter->pdev), ret);
		goto err_irq_disabled;
	}

	/* Reset our software view of the queues.  DMA buffers are kept;
	 * we just zero the contents and reset head/tail/phase so the next
	 * CQE the controller writes is recognised at phase 1. */
	if (nvme->admin_sq)
		memset(nvme->admin_sq, 0,
		       (size_t)nvme->admin_sq_depth * RC_NVME_SQ_ENTRY_SIZE);
	if (nvme->admin_cq)
		memset(nvme->admin_cq, 0,
		       (size_t)nvme->admin_cq_depth * RC_NVME_CQ_ENTRY_SIZE);
	nvme->admin_sq_tail  = 0;
	nvme->admin_cq_head  = 0;
	nvme->admin_cq_phase = 1;

	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];
		unsigned long qflags;

		if (!q)
			continue;
		/* Under q->lock: a queue_rq that passed the dead-check just
		 * before the quiesce could still be inside rc_nvme_io_submit
		 * — memset'ing the SQ under its feet corrupts the SQE it is
		 * writing.  The lock closes that window. */
		spin_lock_irqsave(&q->lock, qflags);
		if (q->sq)
			memset(q->sq, 0,
			       (size_t)q->sq_depth * RC_NVME_SQ_ENTRY_SIZE);
		if (q->cq)
			memset(q->cq, 0,
			       (size_t)q->cq_depth * RC_NVME_CQ_ENTRY_SIZE);
		q->sq_tail  = 0;
		q->cq_head  = 0;
		q->cq_phase = 1;
		/* Reservations held by commands the reset wiped (their CQEs
		 * will never arrive) must not leak SQ capacity forever. */
		q->sq_inflight = 0;
		spin_unlock_irqrestore(&q->lock, qflags);
	}

	aqa = RC_NVME_AQA(nvme->admin_sq_depth, nvme->admin_cq_depth);
	writel(aqa, base + RC_NVME_REG_AQA);
	rc_nvme_writeq(nvme->admin_sq_dma, base, RC_NVME_REG_ASQ);
	rc_nvme_writeq(nvme->admin_cq_dma, base, RC_NVME_REG_ACQ);
	wmb();

	asq_rb = rc_nvme_readq(base, RC_NVME_REG_ASQ);
	acq_rb = rc_nvme_readq(base, RC_NVME_REG_ACQ);
	if (asq_rb != nvme->admin_sq_dma || acq_rb != nvme->admin_cq_dma) {
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s admin queue writeback failed (ASQ rb=0x%llx ACQ rb=0x%llx)\n",
			  pci_name(adapter->pdev),
			  (unsigned long long)asq_rb,
			  (unsigned long long)acq_rb);
		ret = -EIO;
		goto err_irq_disabled;
	}

	cc = RC_NVME_CC_DEFAULT | RC_NVME_CC_EN;
	writel(cc, base + RC_NVME_REG_CC);
	wmb();
	ret = rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, RC_NVME_CSTS_RDY);
	if (ret) {
		csts = readl(base + RC_NVME_REG_CSTS);
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s did not become ready (CSTS=0x%08x) — adapter remains dead\n",
			  pci_name(adapter->pdev), csts);
		goto err_irq_disabled;
	}

	/* Unmask + re-enable IRQs before issuing admin commands so the
	 * submitter wakes via the ISR rather than the 1 ms poll fallback. */
	writel(0xffffffffu, base + RC_NVME_REG_INTMC);
	if (adapter->irq_vector >= 0)
		enable_irq(adapter->irq_vector);
	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];
		if (q && q->irq_vector >= 0)
			enable_irq(q->irq_vector);
	}

	/* Re-issue Set Features Number of Queues.  CC.EN=0 may have wiped
	 * the controller's saved count; without re-issuing, Create I/O CQ
	 * for qid > granted-count would fail.  Save the existing
	 * nr_io_queues so a smaller grant doesn't silently shrink our
	 * queue array — if the controller can't give us the same count,
	 * fail the reset. */
	saved_nr_io_queues = nvme->nr_io_queues;
	ret = __rc_nvme_set_num_queues_locked(adapter, saved_nr_io_queues);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s Set Features Number of Queues failed (%d) — adapter remains dead\n",
			  pci_name(adapter->pdev), ret);
		goto err_irq_enabled;
	}
	if (nvme->nr_io_queues < saved_nr_io_queues) {
		/* Deliberately fail-closed rather than degrade: the volume's
		 * blk-mq tagset was sized with nr_hw_queues = min(members'
		 * queue counts) at create time and every hctx routes to
		 * io_queues[hctx] on EVERY member — accepting a smaller
		 * grant here would leave hctxs > granted dispatching into
		 * NULL queues.  Recovering by reshaping the tagset is a
		 * bigger change than a reset path should make; a module
		 * reload re-negotiates cleanly. */
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s granted only %u queues, had %u before reset — cannot shrink a live volume's queue map, adapter remains dead (reload the module to renegotiate)\n",
			  pci_name(adapter->pdev),
			  nvme->nr_io_queues, saved_nr_io_queues);
		ret = -EIO;
		goto err_irq_enabled;
	}

	/* Re-assert write-back cache mode: a Controller Level Reset can revert
	 * WCE to its power-on default.  Non-fatal — never fail the reset over
	 * cache mode (admin_mutex is held for the whole reset sequence). */
	(void)__rc_nvme_enable_write_cache_locked(adapter);

	/* Re-issue Create I/O CQ + Create I/O SQ for every existing queue.
	 * DMA buffers and IRQ vectors are still ours; we just need to tell
	 * the controller about them again. */
	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];
		if (!q)
			continue;

		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_CQ;
		cmd.prp1  = cpu_to_le64(q->cq_dma);
		cmd.cdw10 = cpu_to_le32(((u32)(q->cq_depth - 1) << 16) | q->qid);
		/* IV mirrors original creation: vec_index == qid for queues
		 * with a dedicated MSI-X vector, 0 for the single-vector
		 * fallback (shared with admin). */
		cmd.cdw11 = cpu_to_le32(((u32)(q->shares_admin_vector ?
					       0 : q->qid) << 16) | 0x3u);
		ret = __rc_nvme_admin_cmd_locked(adapter, &cmd, NULL);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_nvme_reset_controller: %s qid=%u Create I/O CQ failed (%d) — adapter remains dead\n",
				  pci_name(adapter->pdev), q->qid, ret);
			goto err_irq_enabled;
		}

		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_SQ;
		cmd.prp1  = cpu_to_le64(q->sq_dma);
		cmd.cdw10 = cpu_to_le32(((u32)(q->sq_depth - 1) << 16) | q->qid);
		cmd.cdw11 = cpu_to_le32(((u32)q->qid << 16) | 0x1u);
		ret = __rc_nvme_admin_cmd_locked(adapter, &cmd, NULL);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_nvme_reset_controller: %s qid=%u Create I/O SQ failed (%d) — adapter remains dead\n",
				  pci_name(adapter->pdev), q->qid, ret);
			goto err_irq_enabled;
		}
	}

	WRITE_ONCE(nvme->dead, false);
	/* A successful reset (auto or manual) re-enables future auto-reset
	 * attempts.  If the controller dies again later, .timeout will get
	 * to try recovery once more before giving up. */
	WRITE_ONCE(nvme->auto_reset_disabled, false);
	mutex_unlock(&nvme->admin_mutex);
	if (rc_volume_disk)
		blk_mq_unquiesce_queue(rc_volume_disk->queue);

	/* Rejoin policy for a recovered member (CORRECTNESS-CRITICAL for the
	 * mirrored levels): while this member was FAILED the volume kept
	 * accepting writes on the surviving copy, so this member's data is
	 * STALE.  Clearing `dead` must NOT put it back into dispatch — a
	 * rejoined stale mirror serves old data to half the reads.  Park it in
	 * NEEDS_RESYNC; the resync engine then rebuilds it from its live
	 * partner (RAID1: any survivor; RAID10: the same column's mirror
	 * copy), so it stays out of read AND write dispatch until the copy
	 * lands.
	 *
	 * RAID0 is the opposite: with any member down the volume failed
	 * EVERY request (no partial writes possible), so nothing diverged —
	 * restore LIVE and the volume resumes, which is the pre-degraded-
	 * mode recovery behavior. */
	{
		/* Single READ_ONCE snapshot: rc_volume_remove_member() writes
		 * volume_slot = -1 under rc_volume_lock while this function can
		 * be running from auto_reset_work, so a plain double-read could
		 * see >= 0 in the branch and -1 in the index.  Re-validate the
		 * slot still belongs to THIS adapter under the lock — a
		 * concurrent remove + re-register could have reassigned it. */
		int slot = READ_ONCE(nvme->volume_slot);

		mutex_lock(&rc_volume_lock);
		if (slot >= 0 && slot < RC_VOLUME_MAX_MEMBERS &&
		    rc_volume_members[slot] == adapter &&
		    READ_ONCE(rc_volume_member_state[slot]) == RC_MEMBER_FAILED) {
			if (rc_volume_raid_level == RC_LDT_RAID1 ||
			    rc_volume_raid_level == RC_LDT_RAID10) {
				rc_volume_member_set_state(slot,
						RC_MEMBER_NEEDS_RESYNC);
				rc_printk(RC_WARN,
					  "rc_nvme_reset_controller: %s recovered but STALE (missed writes while failed) — parked needs-resync\n",
					  pci_name(adapter->pdev));
				rc_volume_member_readmitted(slot);
			} else {
				rc_volume_member_set_state(slot,
						RC_MEMBER_LIVE);
			}
		}
		mutex_unlock(&rc_volume_lock);
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_reset_controller: %s back online\n",
		  pci_name(adapter->pdev));
	return 0;

err_irq_disabled:
	if (adapter->irq_vector >= 0)
		enable_irq(adapter->irq_vector);
	for (i = 0; i < RC_NVME_IO_QUEUE_TARGET; i++) {
		struct rc_nvme_io_queue *q = nvme->io_queues[i];
		if (q && q->irq_vector >= 0)
			enable_irq(q->irq_vector);
	}
err_irq_enabled:
	mutex_unlock(&nvme->admin_mutex);
	if (rc_volume_disk)
		blk_mq_unquiesce_queue(rc_volume_disk->queue);
	return ret;
}

/* PM suspend for one adapter.
 *
 * Called from the .suspend PCI hook for each member.  Disables interrupt
 * generation, clears CC.EN, and waits for CSTS.RDY to clear so the
 * controller stops touching DMA buffers before the system transitions to
 * D3 (S3) or has its RAM image written out (S4).
 *
 * DMA buffers (admin SQ/CQ, per-queue SQ/CQ, PRP lists) stay allocated.
 * For S3 their RAM contents are preserved across the power transition;
 * for S4 the hibernation snapshot captures them.  Either way, the resume
 * path can reuse them.  We just need the controller to forget any
 * in-flight CIDs on its side — disabling CC.EN does that.
 *
 * Before touching the controller we blk_mq_freeze_queue the volume:
 * freeze blocks new submitters AND waits for every in-flight request to
 * complete.  Without the drain, commands outstanding at CC.EN=0 time
 * lose their completions forever, their bios never end, and the suspend
 * transition blocks on them indefinitely — a hard hang with the screen
 * already off (observed in the field with the rootfs on the volume:
 * journal writeback is almost always in flight when systemd-sleep runs).
 * Failing the in-flight I/O instead (reset_controller's drain_dead)
 * would be wrong here — this is healthy I/O, it must land.  If the
 * controller is genuinely wedged, .timeout fires and fails the stragglers,
 * so the freeze cannot wait forever.  Freeze is refcounted: each member's
 * suspend takes one reference (the first does the actual drain, the
 * second returns immediately) and each member's resume drops one.
 *
 * Sets nvme->dead so the upper layer fails any blk-mq dispatch that
 * could race the freeze.  Cleared again by rc_nvme_pm_resume_adapter
 * via reset_controller. */
int rc_nvme_pm_suspend_adapter(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u32 cur_cc;
	int ret;

	if (!base) {
		rc_printk(RC_INFO,
			  "rc_nvme_pm_suspend_adapter: %s no MMIO base — already shut down\n",
			  pci_name(adapter->pdev));
		return 0;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_pm_suspend_adapter: %s quiescing for S3/S4\n",
		  pci_name(adapter->pdev));

	/* Drain the volume BEFORE taking admin_mutex: a straggler that
	 * times out during the wait needs auto-reset, which takes the
	 * mutex itself. */
	/* Pin-then-freeze: snapshot rc_volume_disk and take a device
	 * reference under rc_volume_lock (teardown del_gendisk/put_disk/
	 * NULLs the pointer under the same lock, so the pin closes the
	 * free-between-check-and-freeze TOCTOU) — but run the freeze wait
	 * itself OUTSIDE the lock.  The drain can last up to the full
	 * timeout/eh_retries budget (~90s) on a wedged member, and holding
	 * the global mutex that long would stall teardown, sysfs, and
	 * resync administration system-wide.  The pinned reference keeps
	 * the gendisk (and its queue) alive even if teardown wins the
	 * race; freezing a dying queue is safe (refcounted, del_gendisk
	 * does its own freeze/unfreeze). */
	mutex_lock(&rc_volume_lock);
	if (rc_volume_disk && !nvme->pm_frozen_disk) {
		/* Pin the disk for the WHOLE frozen window (released only
		 * at unfreeze), and START the freeze while still under the
		 * lock: the pin keeps the gendisk/queue memory alive, but
		 * not the driver-owned rc_volume_tagset, which teardown
		 * (same lock) frees unconditionally — with the freeze
		 * already started inside the locked region, teardown can
		 * never interleave between pin and freeze.
		 * blk_freeze_queue_start is non-blocking; only the drain
		 * wait runs outside the lock (it can last the full
		 * timeout/eh_retries budget on a wedged member, and holding
		 * the global mutex that long would stall teardown, sysfs,
		 * and resync administration system-wide). */
		nvme->pm_frozen_disk = rc_volume_disk;
		get_device(disk_to_dev(nvme->pm_frozen_disk));
		blk_freeze_queue_start(nvme->pm_frozen_disk->queue);
		nvme->pm_freeze_memflags = memalloc_noio_save();
	}
	mutex_unlock(&rc_volume_lock);
	if (nvme->pm_frozen_disk)
		blk_mq_freeze_queue_wait(nvme->pm_frozen_disk->queue);

	/* A straggler that timed out DURING the freeze wait has already
	 * queued auto_reset_work — and its bail-out check (!dead) won't
	 * help, because this function is about to set dead for the whole
	 * suspend.  Left alone it would re-enable the controller (CC.EN,
	 * IRQs, dead=false) concurrently with or after the D3 transition,
	 * undoing the quiesce.  The freeze has completed, so no further
	 * timeouts can queue it again; cancel before taking admin_mutex
	 * (safe: the mutex isn't held, so a running reset can finish). */
	cancel_work_sync(&nvme->auto_reset_work);

	/* Serialize against rc_nvme_reset_controller, which can run from
	 * .timeout → rc_nvme_auto_reset_fn if a request times out mid-PM.
	 * Both paths drive CC.EN and INTMS; without this mutex they can
	 * race and leave the controller in an inconsistent state. */
	mutex_lock(&nvme->admin_mutex);

	WRITE_ONCE(nvme->dead, true);

	/* Mask all NVMe interrupt vectors via INTMS so the controller stops
	 * generating MSI completions while we tear down.  We do NOT call
	 * disable_irq here: enable_irq/disable_irq is refcounted, and
	 * rc_nvme_reset_controller (which the .resume hook delegates to)
	 * does its own paired disable_irq/enable_irq cycle.  Doubling up
	 * here would leave the IRQ depth>0 after resume → no I/O completions
	 * ever fire → every command times out.  The PM core's suspend_noirq
	 * phase masks the Linux IRQ side cleanly on the host's behalf. */
	writel(0xffffffffu, base + RC_NVME_REG_INTMS);

	/* Spec-conformant shutdown BEFORE dropping CC.EN — the members run
	 * with WCE enabled, and skipping CC.SHN made every suspend an
	 * "unsafe shutdown" from the drive's point of view. */
	rc_nvme_shutdown_notify(adapter);

	cur_cc = readl(base + RC_NVME_REG_CC);
	if (cur_cc & RC_NVME_CC_EN) {
		writel(cur_cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);
		wmb();
	}
	ret = rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, 0);
	mutex_unlock(&nvme->admin_mutex);

	if (ret) {
		/* Surface the failure to the PM core, but first roll the
		 * controller back to a usable state.  If suspend aborts the
		 * system transition, .resume isn't guaranteed to fire for the
		 * failing device — leaving dead=true + INTMS masked +
		 * CC.EN=0 would brick the controller until module reload.
		 * reset_controller is the existing recovery path; calling it
		 * here is the same machinery we'd use from .resume, just
		 * earlier.  Safe w.r.t. admin_mutex (already unlocked above)
		 * and idempotent if .resume does still fire. */
		rc_printk(RC_WARN,
			  "rc_nvme_pm_suspend_adapter: %s did not become idle in time (%d) — restoring before returning\n",
			  pci_name(adapter->pdev), ret);
		(void)rc_nvme_reset_controller(adapter);
		/* Suspend is aborting; .resume won't fire for this device,
		 * so drop our freeze reference here or the volume stays
		 * blocked forever. */
		if (nvme->pm_frozen_disk) {
			/* Unfreeze via the pinned pointer, NOT the global —
			 * the volume may have been torn down and reassembled
			 * (fresh queue we never froze) during wait_csts. */
			blk_mq_unfreeze_queue(nvme->pm_frozen_disk->queue,
					      nvme->pm_freeze_memflags);
			put_device(disk_to_dev(nvme->pm_frozen_disk));
			nvme->pm_frozen_disk = NULL;
		}
		return ret;
	}
	return 0;
}

/* PM resume for one adapter.
 *
 * Called from the .resume PCI hook for each member after the PCI core
 * has restored config space + put the device back in D0.  Re-enabling
 * the controller, re-programming the admin queue, and re-creating the
 * I/O queues is exactly what reset_controller already does, so we just
 * delegate.  Reusing that path means the recovery code that handles a
 * mid-operation controller wedge is the same as the code that handles
 * S3/S4 resume — fewer codepaths to keep correct.
 *
 * Drops the freeze reference the suspend hook took on the volume queue.
 * Done even when the reset fails: dead stays set in that case, so
 * unfrozen I/O fails fast with an error instead of blocking every
 * submitter (rootfs included) in blk_queue_enter forever. */
int rc_nvme_pm_resume_adapter(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	int ret;

	rc_printk(RC_NOTE,
		  "rc_nvme_pm_resume_adapter: %s reviving controller\n",
		  pci_name(adapter->pdev));
	ret = rc_nvme_reset_controller(adapter);

	/* Unfreeze via the pinned pointer, never the global rc_volume_disk:
	 * a teardown + REASSEMBLY while this member was suspended leaves
	 * the global pointing at a fresh queue this adapter never froze —
	 * unfreezing that one would corrupt its freeze depth.  The pin
	 * keeps our (possibly dying) queue valid; unfreezing a dying queue
	 * is safe, and the pinned reference is dropped after. */
	if (nvme->pm_frozen_disk) {
		blk_mq_unfreeze_queue(nvme->pm_frozen_disk->queue,
				      nvme->pm_freeze_memflags);
		put_device(disk_to_dev(nvme->pm_frozen_disk));
		nvme->pm_frozen_disk = NULL;
	}
	return ret;
}

/* Initialize the NVMe-state members that an interrupt handler can touch,
 * independent of any hardware access.  Called from rc_bottom_alloc_adapter
 * BEFORE request_irq: on a legacy shared INTx line, another device's
 * interrupt can invoke rc_nvme_irq() (once ctrl_mode is set) before
 * rc_nvme_init_controller runs — waking a zeroed waitqueue is a NULL
 * deref in hard-IRQ context.  cancel_work_sync in the cleanup path also
 * requires auto_reset_work to be initialized regardless of how far the
 * controller bring-up got. */
void rc_nvme_early_init(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;

	init_waitqueue_head(&nvme->admin_cq_wait);
	mutex_init(&nvme->admin_mutex);
	INIT_WORK(&nvme->auto_reset_work, rc_nvme_auto_reset_fn);
	/* 0 is a valid registry slot — not-a-member must be explicit. */
	nvme->volume_slot = -1;
}

int rc_nvme_init_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u32 cc, csts, aqa, vs;
	u64 cap, asq_rb, acq_rb;
	int ret;

	if (!base) {
		rc_printk(RC_ERROR, "rc_nvme_init_controller: no MMIO base\n");
		return -EINVAL;
	}

	cap = rc_nvme_readq(base, RC_NVME_REG_CAP);
	vs = readl(base + RC_NVME_REG_VS);
	if (cap == ~0ULL || cap == 0ULL) {
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: invalid CAP=0x%016llx — not an NVMe controller?\n",
			  (unsigned long long)cap);
		return -ENODEV;
	}
	nvme->cap = cap;
	nvme->mqes = RC_NVME_CAP_MQES(cap) + 1;
	nvme->dstrd = RC_NVME_CAP_DSTRD(cap);
	nvme->timeout_500ms = RC_NVME_CAP_TO(cap);
	nvme->sq_doorbell_base = base + RC_NVME_REG_DBL_BASE;
	/* admin_cq_wait / admin_mutex / auto_reset_work are initialized in
	 * rc_nvme_early_init (called from probe BEFORE request_irq — see
	 * that function's comment).  Per-I/O-queue waitqueue + lock are
	 * initialized in rc_nvme_create_one_io_queue when each queue is
	 * allocated. */

	rc_printk(RC_NOTE,
		  "rc_nvme_init_controller: CAP=0x%016llx VS=0x%08x MQES=%u DSTRD=%u TO=%u (%u ms)\n",
		  (unsigned long long)cap, vs, nvme->mqes, nvme->dstrd,
		  nvme->timeout_500ms,
		  nvme->timeout_500ms ? nvme->timeout_500ms * 500 : 0);

	ret = rc_nvme_disable_controller(adapter);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: controller refused to disable\n");
		return ret;
	}

	nvme->admin_sq_depth = RC_NVME_ADMIN_QUEUE_DEPTH;
	nvme->admin_cq_depth = RC_NVME_ADMIN_QUEUE_DEPTH;
	if (nvme->admin_sq_depth > nvme->mqes)
		nvme->admin_sq_depth = nvme->mqes;
	if (nvme->admin_cq_depth > nvme->mqes)
		nvme->admin_cq_depth = nvme->mqes;

	ret = rc_nvme_alloc_admin_queues(adapter);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: admin queue alloc failed\n");
		return ret;
	}

	/* Mask all interrupts while we program the queues. */
	writel(0xffffffff, base + RC_NVME_REG_INTMS);

	aqa = RC_NVME_AQA(nvme->admin_sq_depth, nvme->admin_cq_depth);
	writel(aqa, base + RC_NVME_REG_AQA);
	rc_nvme_writeq(nvme->admin_sq_dma, base, RC_NVME_REG_ASQ);
	rc_nvme_writeq(nvme->admin_cq_dma, base, RC_NVME_REG_ACQ);
	wmb();

	/* Readback verifies the writes landed. On the broken AHCI-style code
	 * path the analogous 0x100+ writes don't persist for DEV_B000 — this
	 * is the canary that confirms we're using the right register set. */
	asq_rb = rc_nvme_readq(base, RC_NVME_REG_ASQ);
	acq_rb = rc_nvme_readq(base, RC_NVME_REG_ACQ);
	rc_printk(RC_NOTE,
		  "rc_nvme_init_controller: AQA=0x%08x (rb 0x%08x) ASQ=0x%016llx (rb 0x%016llx) ACQ=0x%016llx (rb 0x%016llx)\n",
		  aqa, readl(base + RC_NVME_REG_AQA),
		  (unsigned long long)nvme->admin_sq_dma,
		  (unsigned long long)asq_rb,
		  (unsigned long long)nvme->admin_cq_dma,
		  (unsigned long long)acq_rb);

	if (asq_rb != nvme->admin_sq_dma || acq_rb != nvme->admin_cq_dma) {
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: admin queue register writes did not persist — wrong register layout?\n");
		rc_nvme_free_admin_queues(adapter);
		return -EIO;
	}

	cc = RC_NVME_CC_DEFAULT | RC_NVME_CC_EN;
	writel(cc, base + RC_NVME_REG_CC);
	wmb();

	ret = rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, RC_NVME_CSTS_RDY);
	if (ret) {
		csts = readl(base + RC_NVME_REG_CSTS);
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: controller did not become ready (CSTS=0x%08x CC=0x%08x)\n",
			  csts, readl(base + RC_NVME_REG_CC));
		rc_nvme_free_admin_queues(adapter);
		return ret;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_init_controller: ready — admin SQ depth=%u CQ depth=%u, doorbell stride=%u\n",
		  nvme->admin_sq_depth, nvme->admin_cq_depth, nvme->dstrd);

	/* First real command — proves the admin queue plumbing works. Non-fatal
	 * because reaching CSTS.RDY=1 is still progress worth keeping visible. */
	ret = rc_nvme_identify_controller(adapter);
	if (ret)
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: identify controller failed (%d) — admin queue is up but the controller didn't answer cleanly\n",
			  ret);

	/* Namespace inventory. Only NSID 1 is exercised — Identify Controller
	 * reports NN=1 on the Crucial drives behind 1022:b000. */
	ret = rc_nvme_identify_namespace(adapter, 1);
	if (ret)
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: identify namespace nsid=1 failed (%d)\n",
			  ret);

	/* Tell the controller how many I/O queue pairs we plan to use.  Per
	 * NVMe spec this must happen before Create I/O CQ/SQ.  Request only
	 * what we can actually drive — min(IRQ vectors - 1, online CPUs,
	 * RC_NVME_IO_QUEUE_TARGET), the documented Windows-matching cap —
	 * rather than the raw target: on a vector-constrained host asking
	 * for 4 and then binding CQs to nonexistent vectors used to roll
	 * back to zero I/O queues.  A 0 return means no dedicated vectors
	 * at all; still ask for 1 queue (it will share the admin vector). */
	{
		u16 want = rc_nvme_usable_io_queues(adapter,
						    RC_NVME_IO_QUEUE_TARGET);

		if (want == 0)
			want = 1;
		ret = rc_nvme_set_num_queues(adapter, want);
	}
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: Set Features Number of Queues failed (%d) — falling back to 1 I/O queue\n",
			  ret);
		nvme->nr_io_queues = 1;
	}

	/* Put the member into a deterministic write-back cache state rather
	 * than inheriting the power-on/Windows-left setting.  Non-fatal (see
	 * __rc_nvme_enable_write_cache_locked): a failure here must not block
	 * bring-up of the root-fs adapter. */
	(void)rc_nvme_enable_write_cache(adapter);

	/* Create nvme->nr_io_queues queue pairs, each with its own MSI-X
	 * vector (or one pair sharing the admin vector on the single-vector
	 * fallback), so the upper layer can submit NVM commands. */
	ret = rc_nvme_create_io_queues(adapter);
	if (ret) {
		/* All queues were rolled back — zero the advertised count so
		 * nothing downstream (volume nr_hw_queues, sync I/O helpers)
		 * trusts a count whose io_queues[] slots are NULL.
		 *
		 * FATAL: an adapter with no I/O queues can never serve the
		 * volume or read metadata; treating this as success left a
		 * half-dead adapter exported through sysfs/debugfs.  Return
		 * the error — probe unwinds via rc_nvme_cleanup_controller,
		 * which releases the admin queue + disables the controller. */
		nvme->nr_io_queues = 0;
		rc_printk(RC_ERROR,
			  "rc_nvme_init_controller: I/O queue creation failed (%d) — failing init\n",
			  ret);
		rc_nvme_cleanup_controller(adapter);
		return ret;
	}

	/* Unmask all interrupt vectors so the MSI handler starts firing on
	 * admin + I/O queue completions.  INTMC is "Interrupt Mask Clear"
	 * (write-1-to-clear); writing all-1s unmasks every vector.  With one
	 * MSI vector configured per adapter, both queues land on vector 0
	 * and our handler wakes both admin_cq_wait and io_cq_wait. */
	writel(0xffffffffu, base + RC_NVME_REG_INTMC);
	rc_printk(RC_INFO,
		  "rc_nvme_init_controller: interrupt mask cleared (INTMS=0x%08x)\n",
		  readl(base + RC_NVME_REG_INTMS));

	/* Read + validate the AMD RAIDCore metadata block at LBA 0x5000. */
	ret = rc_nvme_read_validate_metadata(adapter);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: metadata validation failed (%d) — member treated as orphan\n",
			  ret);
		return 0;
	}
	rc_volume_register_member(adapter);

	return 0;
}

void rc_nvme_cleanup_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;

	/* Cancel any in-flight or queued auto-reset before we start tearing
	 * down — otherwise the work could race with destroy_io_queues and
	 * try to re-bind queues we just freed. */
	cancel_work_sync(&adapter->ctx.nvme.auto_reset_work);

	/* A dead-flagged controller gets no admin Delete commands (see
	 * rc_nvme_destroy_one_io_queue), so its queue DMA is about to be
	 * freed while CC.EN may still be 1 — disable it first so it can't
	 * DMA into freed memory. */
	if (READ_ONCE(adapter->ctx.nvme.dead))
		rc_nvme_disable_dead_controller(adapter);

	/* Tear down I/O queues *before* disabling the controller so the
	 * Delete SQ / Delete CQ admin commands still go through. */
	rc_nvme_destroy_io_queues(adapter);

	if (base) {
		/* Best-effort graceful shutdown: CC.SHN notification first
		 * (the members run write-back — a bare CC.EN drop is an
		 * unsafe shutdown per spec), then disable. */
		u32 cc;

		rc_nvme_shutdown_notify(adapter);
		cc = readl(base + RC_NVME_REG_CC);
		if (cc != 0xffffffff && (cc & RC_NVME_CC_EN)) {
			writel(cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);
			rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, 0);
		}
	}
	rc_nvme_free_admin_queues(adapter);
}

/**
 * ==============================================================================
 * REVERSE-ENGINEERED EXTENSION: RAID 10 NESTED GEOMETRY TRANSLATION PLANE
 * IMPLEMENTING TWO-TIER PARENT-CHILD LOGICAL BLOCK MAPPING
 * ==============================================================================
 *
 * This function maps a logical LBA to (target_member, physical LBA on that
 * member) for a RAID 10 array. The logical space is striped across
 * `num_drives / 2` columns, each column being a 2-way mirror pair
 * {2c, 2c+1}.
 *
 * Mapping algorithm:
 *   - Logical stripe N lives in column (N % cols) at physical stripe
 *     (N / cols) on BOTH members of the pair.
 *   - For READs: pick one live copy of the pair (round-robin via external
 *     balancer). This function returns the first copy (2c); the caller
 *     should apply the live-member selection.
 *   - For WRITEs/DISCARDs: the caller must fan out to both copies
 *     (2c and 2c+1).
 *
 * The column count is derived from num_drives (must be even, >= 4).
 * No legacy block boundaries or single-stripe bottlenecks remain — this
 * path is optimized for enterprise flash throughput with branch-predictor
 * friendly modulo arithmetic and 64-bit safe div_u64.
 */
u64 rc_amd_map_nested_raid10(u64 sector_lba, u32 chunk_sectors, int *target_member, int num_drives)
{
    u32 cols;
    u64 stripe_num;
    u32 stripe_off;
    u32 col;
    int c0, c1;

    if (unlikely(num_drives < 4 || (num_drives & 1) != 0 || !chunk_sectors)) {
        /* Minimum topology boundary constraint failure fallback coordinate */
        *target_member = 0;
        return sector_lba;
    }

    cols = num_drives / 2;  /* Each column is a 2-way mirror pair */

    /* Step 1: Decompose logical LBA into stripe number + offset within stripe.
     * Use div_u64 for 64-bit safe division — chunk_sectors is u32. */
    stripe_num  = div_u64(sector_lba, chunk_sectors);
    stripe_off  = (u32)(sector_lba - stripe_num * chunk_sectors);

    /* Step 2: Column selection via modulo (branch-predictor friendly).
     * Logical stripe N maps to column (N % cols). */
    col = (u32)(stripe_num % cols);

    /* Step 3: Map column to the mirror pair {2c, 2c+1}.
     * Return the first copy (c0) — caller handles live-member selection
     * for reads, and fans to both for writes/discards. */
    c0 = 2 * col;
    c1 = c0 + 1;
    *target_member = c0;

    /* Step 4: Physical stripe within the column is the quotient.
     * Reconstruct physical LBA on the target member. */
    return (div_u64(stripe_num, cols) * chunk_sectors) + stripe_off;
}
EXPORT_SYMBOL_GPL(rc_amd_map_nested_raid10);

void rc_amd_route_io_nested_raid10(u64 *lba, int *mbr, u32 chunk_sectors, int num_drives) {
    *lba = rc_amd_map_nested_raid10(*lba, chunk_sectors, mbr, num_drives);
    /* mbr now holds the first copy of the mirror pair (2c).
     * Caller must:
     *   - For READ: check live mask and pick c0 or c1 (round-robin)
     *   - For WRITE/DISCARD: fan out to both c0 and c1
     */
}
EXPORT_SYMBOL_GPL(rc_amd_route_io_nested_raid10);

/**
 * ==============================================================================
 * EXTENSION: HIGH-AVAILABILITY TRANSIENT NVME ERROR RETRY STATE MACHINE
 * PREVENTS FATAL SYSTEM BLK_STS_IOERR BURSTS ON RETRYABLE CHIP TIMEOUTS
 * ==============================================================================
 */
blk_status_t rc_amd_handle_transient_retry(struct bio *bio, int retry_count, int max_retries, int device_status)
{
    /* Check if the error code is a transient, retryable status bit */
    /* 0x01: Transient Timeout, 0x02: Busy Queue, 0x04: Intermittent Bus Flaky */
    if (device_status == 0x01 || device_status == 0x02 || device_status == 0x04) {
        if (retry_count < max_retries) {
            pr_warn("rcraid: Intercepted retryable hardware timeout status (0x%X). Queueing attempt %d of %d...\\n",
                    device_status, retry_count + 1, max_retries);

            /* Freeze request queue and yield execution slice to allow hardware bus settle time */
            yield();

            /* Return resource block signal to blk-mq to force a non-destructive re-dispatch query */
            return BLK_STS_RESOURCE;
        }
    }

    /* Fallback to original terminal error code if retry thresholds are exhausted */
    pr_err("rcraid: Fault threshold exhausted or unrecoverable error (0x%X). Dropping array volume layer.\\n", device_status);
    return BLK_STS_IOERR;
}
EXPORT_SYMBOL_GPL(rc_amd_handle_transient_retry);
