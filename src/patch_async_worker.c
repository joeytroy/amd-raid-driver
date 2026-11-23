// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — asynchronous mirror completion + rebuild engine
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries (rcbottom.sys,
 * rcraid.sys, rccfg.sys) under DMCA §1201(f) interoperability
 * protections.  No code is copied from AMD source distributions.
 * See docs/RE_METHODOLOGY.md for the full process and legal record.
 *
 * =========================================================================
 * WHAT THIS FILE IS
 * =========================================================================
 * Two cooperating halves of the out-of-tree execution engine.
 *
 * 1. The mirror completion pump.  A mirrored bio fans out to N member
 *    bios and the parent may not be ended until all N have reported.
 *    Ending it inline from the member completion callbacks means a
 *    single member that retires slowly (interrupt coalescing, a
 *    queueing hiccup, a retry) stalls the block-layer queue feeding
 *    it.  So the parent endio is parked on a kthread queue —
 *    "kamd_async_worker", see RC_ASYNC_WORKER_NAME — and the hardware
 *    context returns immediately.  Submitter and completer never share
 *    a CPU wait.  The queue carries one worker, so a burst of
 *    completions drains in the order the parents reached it.
 *
 * 2. The rebuild pump ("kamd_resync", RC_ASYNC_RESYNC_NAME).  A resync
 *    walks the array a chunk at a time for minutes or hours.  Running
 *    that inside the completion worker would do precisely what (1)
 *    exists to prevent, so it gets a thread of its own.  It is fully
 *    cooperative: it sleeps on a waitqueue rather than spinning, it
 *    re-reads its cancel flag at every chunk boundary, it re-checks
 *    kthread_should_stop() inside the copy loop, and it never holds a
 *    lock across a media call.
 *
 * =========================================================================
 * THE ONE RULE — COMPLETION CONTEXTS NEVER BLOCK
 * =========================================================================
 * Everything reachable from rc_amd_mirror_complete*() runs in the
 * context of whoever finished the member's bio.  On the NVMe path that
 * is rc_nvme_irq() on the local_bh/softirq path, so the call can
 * arrive with interrupts disabled and preemption off.  Therefore, on
 * that path:
 *
 *   - no sleeping, no wait_event, no flush, no completion wait, no
 *     kthread_stop, no cond_resched;
 *   - no kmalloc(GFP_KERNEL) — the mirror context is a fixed-size leaf
 *     object taken with GFP_ATOMIC, and failing to get one is isolated
 *     rather than propagated (see FAULT ISOLATION below);
 *   - every lock in this file is a spinlock taken with _irqsave, and
 *     none is held across bio_endio(), a media callback, or
 *     kthread_queue_work();
 *   - the aggregate status of a mirrored parent is plain field
 *     arithmetic — no allocation, no sorting, no walk of a
 *     dynamically sized structure.
 *
 * The only blocking wait in the file is in
 * rc_amd_exit_async_subsystem().  Module unload has to guarantee that
 * no member completion can still reference a context we are about to
 * free, and that guarantee costs a wait.  It runs from module_exit(),
 * in process context, after the module is already unbound — and it is
 * bounded (RC_ASYNC_DRAIN_TIMEOUT_MS) so a wedged member delays rmmod
 * rather than hanging it.  Note the ordering there: contexts drain
 * first, kthread_stop() second, because kthread_worker_fn() abandons
 * work still queued once its stop flag is set.
 *
 * =========================================================================
 * FAULT ISOLATION
 * =========================================================================
 * Every allocation on the submit path can fail under memory pressure,
 * and an array must not lose a write because the driver's own slab ran
 * dry.  A context we cannot build is answered with BLK_STS_RESOURCE,
 * never BLK_STS_IOERR: the block layer owns the retry, the parent is
 * still ended exactly once, and the filesystem above never sees a
 * false I/O error.  BLK_STS_RESOURCE is also the answer for a parent
 * still in flight when unload starts, so teardown degrades to "re-
 * dispatch me later" instead of "hand me a silent hole".
 *
 * =========================================================================
 * WIRING
 * =========================================================================
 *   volume submit path        -> rc_amd_queue_async_completion()
 *   member endio (any ctx)    -> rc_amd_mirror_complete[_member]()
 *   member dispatch failure   -> rc_amd_mirror_abort()
 *   amd_ctl0 START_RESYNC     -> rc_amd_resync_request()
 *   amd_ctl0 FAIL_MEMBER      -> rc_amd_fail_member()
 *   amd_ctl0 GET_STATUS       -> rc_amd_get_status()
 *
 * Public declarations live in patch_prototypes.h.
 */

#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/atomic.h>
#include <linux/blk_types.h>
#include <linux/bio.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "patch_prototypes.h"

/* =========================================================================
 * TUNABLES
 * ========================================================================= */

/*
 * The kernel caps a task's comm at TASK_COMM_LEN - 1 == 15 visible
 * characters, so these two names are truncated in ps/top output
 * ("kamd_async_worke", "kamd_resync").  The full names are kept because
 * they are the identifiers operators and the amd_ctl0 bridge refer to,
 * and the init log prints them in full so a thread can still be matched
 * from dmesg.
 */
#define RC_ASYNC_WORKER_NAME	"kamd_async_worker"
#define RC_ASYNC_RESYNC_NAME	"kamd_resync"

/* Member slots addressable by rc_amd_fail_member() and friends. */
#define RC_ASYNC_MAX_MEMBERS	32

/*
 * Unload drain budget.  Contexts in flight must be allowed to retire
 * before the worker is stopped, but a member that never completes must
 * not be able to hold rmmod open forever — so the wait is bounded and
 * the stragglers are then failed with BLK_STS_RESOURCE.
 *
 * Overridable so the host-side harness can exercise the timeout path in
 * milliseconds instead of waiting out the real budget on every run.
 */
#ifndef RC_ASYNC_DRAIN_TIMEOUT_MS
#define RC_ASYNC_DRAIN_TIMEOUT_MS	5000
#endif

/* Resync thread idle poll; the waitqueue wakes it early on a request.
 * This is only the backstop for a lost wakeup. */
#define RC_ASYNC_RESYNC_IDLE_MS		1000

/* How long the resync thread sleeps waiting for the media layer's
 * outstanding-copy count to fall back under its window. */
#define RC_ASYNC_THROTTLE_MS		200

/* Default in-flight chunk window if the media layer picks none. */
#define RC_ASYNC_DEFAULT_WINDOW		8

/* In-flight parents above which the status mask reports overload. */
#define RC_ASYNC_OVERLOAD_HIGH_WATER	1024

/*
 * How many finished contexts are kept as tombstones.
 *
 * A member path that completes a bio twice, or reports after an abort, is a
 * driver bug — but "the engine counts it and carries on" is only true if the
 * memory is still there when the extra report arrives.  Freed immediately, a
 * duplicate reads a released context; instead a finished context is retired
 * out of the live registry (so the unload drain still completes) but its
 * memory is retained here for a while.  Over-reporting costs a few kilobytes
 * instead of a use-after-free, and the oldest tombstone is dropped once the
 * pool is full.
 */
#ifndef RC_ASYNC_GRAVEYARD_MAX
#define RC_ASYNC_GRAVEYARD_MAX		256
#endif

/* The rebuild is routed through the nested RAID 10 matrix only for an
 * array big enough to have one (rc_amd_map_nested_raid10 wants >= 4
 * members and an even count). */
#define RC_ASYNC_MIN_NESTED_DRIVES	4

/*
 * Status mask reported to amd_ctl0 by AMD_IOCTL_GET_STATUS.  Bit 0 keeps
 * the "array healthy" baseline the bridge already hands to user-space, so
 * an unmodified reader keeps seeing a healthy array while every other bit
 * is additive information.
 */
#define RC_AMD_STATUS_LIVE		(1u << 0)	/* engine is up        */
#define RC_AMD_STATUS_DEGRADED		(1u << 1)	/* a member is failed */
#define RC_AMD_STATUS_RESYNCING		(1u << 2)	/* rebuild in flight  */
#define RC_AMD_STATUS_OVERLOAD		(1u << 3)	/* resource injected  */
#define RC_AMD_STATUS_FAULTS		(1u << 4)	/* media errors seen  */

/* Per-context flags, guarded by ctx->lock. */
#define RC_CTX_COMPLETING	(1u << 0)	/* an endio owner exists     */
#define RC_CTX_RETIRED		(1u << 1)	/* endio already issued     */
#define RC_CTX_RESOURCE		(1u << 2)	/* forced BLK_STS_RESOURCE */
#define RC_CTX_GRAVE		(1u << 3)	/* parked as a tombstone    */

/* =========================================================================
 * TYPES
 * ========================================================================= */

struct rc_member_state {
	atomic_t	failed;		/* rc_amd_fail_member() latch         */
	atomic64_t	completed;	/* member bios that retired           */
	atomic64_t	errors;		/* member bios that retired in error  */
};

/**
 * struct rc_mirror_context - one fan-out parent bio
 * @registry:		link on rc_amd_engine.contexts while live, on
 *			rc_amd_engine.graveyard once RC_CTX_GRAVE is set,
 *			both under engine->lock
 * @completion_work:	the kthread_work parked on the completion queue
 * @parent_bio:		the bio to end once every member has reported
 * @lock:		guards @status and @flags
 * @remaining_mirrors:	members still owed a report
 * @flags:		RC_CTX_*; whoever sets RC_CTX_COMPLETING owns the endio
 * @status:		aggregate status for the parent
 *
 * Lifetime: built by rc_amd_queue_async_completion(), registered on the
 * engine list, and retired once the parent endio has been issued *and* all
 * @remaining_mirrors reports have arrived (rc_amd_context_dead()).  Those two
 * normally coincide — the last report is what triggers the endio — but after
 * rc_amd_mirror_abort() the parent is ended while members are still in flight,
 * and those members are still entitled to report, so the context then outlives
 * its own endio and the last straggler retires it.
 *
 * Exactly @remaining_mirrors reports are owed, one per dispatched member bio.
 * The block layer guarantees one endio per bio, so a report can arrive more
 * than once only through a bug in a member path, and the engine does not track
 * which member has reported — a handle carries no member identity, so a repeat
 * before the last real report is indistinguishable from that report.  What it
 * can do is make the extra report harmless rather than try to detect it:
 * anything past the promised count lands on an already-retired context and is
 * counted in rc_async_stats.stale_reports, and the tombstone pool keeps that
 * memory mapped so the count, not a use-after-free, is what it reads.
 */
struct rc_mirror_context {
	struct list_head		registry;
	struct kthread_work		completion_work;
	struct bio			*parent_bio;
	spinlock_t			lock;
	atomic_t			remaining_mirrors;
	unsigned int			flags;
	blk_status_t			status;
};

/**
 * struct rc_resync_job - one rebuild request
 * @start_sector:	first logical sector of the rebuild range
 * @end_sector:		one past the last logical sector
 * @cursor:		next logical sector to copy
 * @chunk_sectors:	rebuild granularity
 * @source_member:	-1 to let the media layer pick the surviving copy
 * @target_member:	-1 to route every chunk through the nested matrix
 * @chunks, @bytes:	progress, mirrored into struct rc_resync_progress
 * @state, @result:	terminal bookkeeping for the finish log line
 *
 * Allocated and populated by rc_amd_resync_request() in the caller's
 * context, then owned by the resync thread, which frees it.  Nothing
 * outside that thread holds a pointer across the handoff, which is why the
 * cancel path sets a flag in the engine instead of touching this object.
 */
struct rc_resync_job {
	u64	start_sector;
	u64	end_sector;
	u64	cursor;
	u64	chunks;
	u64	bytes;
	u32	chunk_sectors;
	int	source_member;
	int	target_member;
	int	state;
	int	result;
};

/**
 * struct rc_async_engine - singleton backing both pumps
 * @completion_worker:	kthread queue that owns parent endio
 * @completion_task:	thread draining @completion_worker
 * @resync_task:	thread draining rebuild jobs
 * @lock:		guards @contexts, @graveyard, @graves, @pending_job,
 *			@resync, @alive, @resync_cancel and @geometry_*
 * @contexts:	live rc_mirror_context objects
 * @graveyard:	tombstones of finished contexts, kept mapped so an over-report
 *		reads a dead context instead of freed memory
 * @graves:	@graveyard length, so the oldest tombstone can be dropped
 * @pending_job:	request accepted but not yet claimed by the thread.
 *		Every store to it is WRITE_ONCE(), because the rebuild
 *		thread samples it with READ_ONCE() in its wait condition
 *		to narrow the lost-wakeup window; pairing the two is what
 *		makes that sample well defined on both sides
 * @resync:		live (or last) progress of the rebuild
 * @inflight:	parent bios outstanding; drains to zero on unload
 * @alive:		false once rc_amd_exit_async_subsystem() has begun
 * @resync_cancel:	latched by rc_amd_resync_cancel(), polled per chunk
 * @drain_wq:		woken when @inflight reaches zero
 * @resync_wq:		woken on a new request or a cancel
 * @throttle_wq:	woken by the media layer as copies retire
 * @members:		per-slot fault state and counters
 * @stats:		engine telemetry
 * @geometry_drives:	member count for the nested RAID 10 router, 0 = off
 * @geometry_chunk:	chunk size matching that router
 * @chunk_fn:		registered media copy hook (see patch_prototypes.h)
 * @outstanding_fn:	registered in-flight copy counter
 * @io_cookie:		opaque pointer handed back to both hooks
 * @io_window:		in-flight chunk budget
 *
 * Lock order, globally: engine->lock may be taken while holding
 * ctx->lock is never done, and ctx->lock is only ever taken with
 * engine->lock already held or with no other lock held.  kthread_queue_work()
 * touches the worker's own raw spinlock underneath both, and the worker
 * body only ever takes engine->lock to unlink itself, so no cycle exists.
 */
struct rc_async_engine {
	struct kthread_worker	completion_worker;
	struct task_struct	*completion_task;
	struct task_struct	*resync_task;

	spinlock_t		lock;
	struct list_head	contexts;
	struct list_head	graveyard;
	unsigned int		graves;
	struct rc_resync_job	*pending_job;
	struct rc_resync_progress resync;
	atomic_t		inflight;
	bool			alive;
	bool			resync_cancel;

	wait_queue_head_t	drain_wq;
	wait_queue_head_t	resync_wq;
	wait_queue_head_t	throttle_wq;

	struct rc_member_state	members[RC_ASYNC_MAX_MEMBERS];
	struct rc_async_stats	stats;

	int			geometry_drives;
	u32			geometry_chunk;

	rc_resync_chunk_fn		chunk_fn;
	rc_resync_outstanding_fn	outstanding_fn;
	void				*io_cookie;
	u32				io_window;
};

static struct rc_async_engine rc_amd_engine;

/* =========================================================================
 * SMALL HELPERS
 * ========================================================================= */

static bool rc_amd_member_valid(int member)
{
	return member >= 0 && member < RC_ASYNC_MAX_MEMBERS;
}

static bool rc_amd_member_failed(int member)
{
	if (!rc_amd_member_valid(member))
		return true;
	return atomic_read(&rc_amd_engine.members[member].failed) != 0;
}

static const char *rc_resync_state_name(int state)
{
	switch (state) {
	case RC_RESYNC_IDLE:
		return "idle";
	case RC_RESYNC_PENDING:
		return "pending";
	case RC_RESYNC_RUNNING:
		return "running";
	case RC_RESYNC_DONE:
		return "complete";
	case RC_RESYNC_FAILED:
		return "failed";
	case RC_RESYNC_CANCELLED:
		return "cancelled";
	default:
		return "unknown";
	}
}

/* =========================================================================
 * STATUS AGGREGATION
 *
 * blk_status_t is an ordered u8, not a bitmask, so member statuses are
 * merged by severity rank rather than OR-ed: OR-ing BLK_STS_IOERR (10)
 * with BLK_STS_ZONE_ACTIVE_RESOURCE (15) would invent a status that does
 * not exist.  The rank is only ever compared within one context, under
 * ctx->lock, and needs no allocation — a requirement, since the
 * completion path may be atomic.
 */
static unsigned int rc_status_rank(blk_status_t status)
{
	switch (status) {
	case BLK_STS_OK:
		return 0;
	/* "Come back later" — the block layer owns the retry.  These lose to
	 * anything that means the operation genuinely failed. */
	case BLK_STS_RESOURCE:
	case BLK_STS_DEV_RESOURCE:
		return 1;
	/* Exhausted resources: the request failed for now and must not be
	 * reported as merely deferred. */
	case BLK_STS_ZONE_OPEN_RESOURCE:
	case BLK_STS_ZONE_ACTIVE_RESOURCE:
	case BLK_STS_NOSPC:
	case BLK_STS_RESV_CONFLICT:
		return 2;
	case BLK_STS_NOTSUPP:
	case BLK_STS_TARGET:
	case BLK_STS_TRANSPORT:
	case BLK_STS_MEDIUM:
	case BLK_STS_PROTECTION:
		return 3;
	default:
		/* IOERR, TIMEOUT, INVAL, OFFLINE, DURATION_LIMIT, AGAIN,
		 * DM_REQUEUE, and anything a future kernel adds: terminal. */
		return 4;
	}
}

/*
 * Ties go to the incoming status: it is the most recently observed member
 * failure, so it is the most informative one to report.  OK never wins a
 * merge in either direction — a successful member cannot mask a failure.
 */
static blk_status_t rc_status_merge(blk_status_t have, blk_status_t incoming)
{
	if (incoming == BLK_STS_OK)
		return have;
	if (have == BLK_STS_OK)
		return incoming;

	return rc_status_rank(incoming) >= rc_status_rank(have) ?
		incoming : have;
}

/*
 * End a bio exactly once with a chosen status.  Kept in one place so the
 * bi_status/bi_error transition for pre-6.x kernels is stated once.  Not
 * called from a spinlock context anywhere in this file.
 */
static void rc_amd_bio_finish(struct bio *bio, blk_status_t status)
{
	if (unlikely(!bio))
		return;

	/*
	 * Modern block layer: blk_status_t is a field on the bio and
	 * bio_endio() is the single completion entry point.  There is no
	 * pre-5.5 bio_error() fallback — this driver targets kernel 7.x
	 * and the out-of-tree floor is well past that split.
	 */
	bio->bi_status = status;
	bio_endio(bio);
}

/* =========================================================================
 * CONTEXT LIFECYCLE
 * ========================================================================= */

/**
 * rc_amd_context_dead - last-observer test for a context's lifetime
 *
 * A context may be retired only once its parent endio has been issued *and*
 * every report it promised has arrived.  Normally those two moments coincide:
 * the last report is what triggers the endio.  They part company after
 * rc_amd_mirror_abort(), which ends the parent while members are still in
 * flight — and those members are still entitled to report, so the context
 * has to outlive its own endio and be retired by the last straggler instead.
 * Retiring on endio alone is a use-after-free waiting for a member that
 * behaves exactly as documented.
 *
 * Both transitions are latched under @ctx->lock, so exactly one of the two
 * observers (the completion worker and the last straggler) sees the
 * condition become true and exactly one reaps.
 *
 * Called with @ctx->lock held; the caller must drop the lock and then call
 * rc_amd_context_reap().
 */
static bool rc_amd_context_dead(struct rc_mirror_context *ctx)
{
	return (ctx->flags & RC_CTX_RETIRED) &&
	       atomic_read(&ctx->remaining_mirrors) <= 0;
}

/**
 * rc_amd_context_reap - unregister a finished context and tombstone it
 * @ctx: context that rc_amd_context_dead() accepted
 *
 * Called with @ctx->lock dropped.  Takes only engine->lock, so it is safe
 * from the completion worker, from a member's completion path, and from the
 * unload drain.
 *
 * Dropping the context from @contexts is what lets the unload drain
 * complete; parking it on @graveyard instead of freeing it is what makes an
 * over-report survivable.  A handle is only ever passed back into this file,
 * and the only thing a report arriving after this point can legitimately do
 * is be counted and dropped, so the memory has to still be mapped when it
 * arrives.  @graves caps that: once the pool is full the oldest tombstone is
 * freed and the guarantee degrades to "a recent over-report is safe", rather
 * than the driver retaining every context it has ever created.
 */
static void rc_amd_context_reap(struct rc_mirror_context *ctx)
{
	struct rc_mirror_context *oldest = NULL;
	unsigned long flags;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	list_del(&ctx->registry);

	/*
	 * RC_CTX_GRAVE is latched here rather than by the caller so the flag
	 * and the unlink publish together: an over-report either sees GRAVE
	 * and stops, or ran entirely before this point and finds RETIRED
	 * already set.  It never sees a half-published context.
	 */
	ctx->flags |= RC_CTX_GRAVE;
	list_add_tail(&ctx->registry, &rc_amd_engine.graveyard);

	/*
	 * The eviction has to unlink and account in the same critical
	 * section.  Deferring the list_del() to a second pass would let two
	 * concurrent reaps both pick the same victim and then both unlink it,
	 * and the loser would corrupt the list it thinks it owns.
	 */
	if (unlikely(++rc_amd_engine.graves > RC_ASYNC_GRAVEYARD_MAX)) {
		oldest = list_entry(rc_amd_engine.graveyard.next,
				    struct rc_mirror_context, registry);
		list_del(&oldest->registry);
		rc_amd_engine.graves--;
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	atomic_dec(&rc_amd_engine.inflight);
	wake_up(&rc_amd_engine.drain_wq);

	/* kfree() outside the lock: the victim is off every list now, so the
	 * only thing that can still reach it is an over-report, and that is
	 * exactly the case the bound trades away. */
	kfree(oldest);
}

/**
 * rc_amd_straggler_report - absorb a report that arrived after the endio
 * @ctx: context whose parent was already ended, by an abort or by the drain
 *
 * Counted as stale and otherwise ignored: the endio owner was latched before
 * the parent was ended, so there is nothing left to merge or decide.  The one
 * thing that matters is the countdown, because when it reaches zero this
 * report is the last one the context will ever see and it has to be the one
 * that reaps.
 *
 * Two ways to arrive here.  A report that is still *owed* — a member of an
 * aborted set — takes the countdown and, if it was the last, reaps.  A report
 * that is not owed at all, because the context is already a tombstone, only
 * gets counted: the countdown is already at or past zero and taking it again
 * would make the count meaningless, and reaping again would unlink a list
 * entry that has since been reused.
 *
 * Called with @ctx->lock NOT held, and safe from any context including atomic.
 */
static void rc_amd_straggler_report(struct rc_mirror_context *ctx)
{
	unsigned long flags;
	bool dead;
	int remaining;

	spin_lock_irqsave(&ctx->lock, flags);
	if (unlikely(ctx->flags & RC_CTX_GRAVE)) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		atomic64_inc(&rc_amd_engine.stats.stale_reports);
		pr_warn_ratelimited("rcraid: mirror context reported after it was retired\n");
		return;
	}
	remaining = atomic_dec_return(&ctx->remaining_mirrors);
	dead = rc_amd_context_dead(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);

	atomic64_inc(&rc_amd_engine.stats.stale_reports);

	if (unlikely(remaining < 0))
		pr_warn_ratelimited("rcraid: mirror context got more reports than members\n");

	if (dead)
		rc_amd_context_reap(ctx);
}

/**
 * rc_amd_mirror_completion_work - completion-worker body
 * @work: the parked kthread_work
 *
 * The only place the parent bio is ended.  Running here rather than in
 * the member's completion context is the entire point: by the time this
 * runs, the hardware interrupt that produced the last report is long
 * gone, so bio_endio() and the block-layer teardown behind it cannot sit
 * on top of a device's softirq path.
 */
static void rc_amd_mirror_completion_work(struct kthread_work *work)
{
	struct rc_mirror_context *ctx =
		container_of(work, struct rc_mirror_context, completion_work);
	blk_status_t status;
	unsigned long flags;
	unsigned int ctx_flags;
	bool free_now;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx_flags = ctx->flags;
	status = ctx->status;
	ctx->flags |= RC_CTX_RETIRED;
	free_now = rc_amd_context_dead(ctx);
	spin_unlock_irqrestore(&ctx->lock, flags);

	/*
	 * RC_CTX_RESOURCE means unload gave up waiting for a member.
	 * BLK_STS_RESOURCE is the safe answer there: the data was never
	 * reported written, so the block layer retries instead of handing a
	 * silent hole to the filesystem.
	 */
	if (ctx_flags & RC_CTX_RESOURCE)
		status = BLK_STS_RESOURCE;

	atomic64_inc(&rc_amd_engine.stats.completed);
	if (status == BLK_STS_OK) {
		atomic64_inc(&rc_amd_engine.stats.mirror_ok);
	} else {
		if (status == BLK_STS_RESOURCE)
			atomic64_inc(&rc_amd_engine.stats.resource_injections);
		else
			atomic64_inc(&rc_amd_engine.stats.mirror_errors);
		pr_warn_ratelimited("rcraid: mirrored bio ended with status %u\n",
				    status);
	}

	rc_amd_bio_finish(ctx->parent_bio, status);

	/*
	 * Retire only when nothing more is owed.  After an abort members are
	 * still in flight and are entitled to report, so the context stays on
	 * the registry and the last of them reaps it.
	 */
	if (free_now)
		rc_amd_context_reap(ctx);
}

/**
 * rc_amd_context_claim - take ownership of the parent endio and queue it
 * @ctx: context to finish
 * @extra_flags: additional RC_CTX_* flags to latch first
 * @forced: status to merge in before claiming (BLK_STS_OK to leave alone)
 *
 * Atomic-context safe: takes @ctx->lock (a spinlock) and then
 * kthread_queue_work(), which touches only the worker's own raw
 * spinlock.
 *
 * The RC_CTX_COMPLETING latch is set *before* the work is queued, so a
 * member report racing the unload drain can never double-end the parent:
 * whichever side gets the lock first wins, and the loser is told so by the
 * false return.
 *
 * Return: %true if this call took ownership, %false if someone already had
 * it (the caller must not touch @ctx->status or free anything).
 */
static bool rc_amd_context_claim(struct rc_mirror_context *ctx,
				 unsigned int extra_flags,
				 blk_status_t forced)
{
	unsigned long flags;
	bool claimed;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->flags & RC_CTX_COMPLETING) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return false;
	}
	if (forced != BLK_STS_OK)
		ctx->status = rc_status_merge(ctx->status, forced);
	ctx->flags |= RC_CTX_COMPLETING | extra_flags;
	spin_unlock_irqrestore(&ctx->lock, flags);

	/*
	 * Unreachable in normal operation: the latch above guarantees this
	 * work is not on the queue yet.  If a future refactor breaks that,
	 * refuse rather than run the completion body twice.
	 */
	claimed = kthread_queue_work(&rc_amd_engine.completion_worker,
				     &ctx->completion_work);
	if (unlikely(!claimed))
		pr_warn_ratelimited("rcraid: completion work already queued, claim dropped\n");

	return claimed;
}

/* =========================================================================
 * MIRROR API
 * ========================================================================= */

/**
 * rc_amd_queue_async_completion - dispatch a mirrored parent bio
 * @parent_bio: bio to end after all members report
 * @total_mirrors: number of member bios about to be dispatched, must be > 0
 * @status_flag: 0 for a clean dispatch, non-zero to start the aggregate
 *		  already failed (the historical "this is an error" argument)
 *
 * Registers a context and returns it immediately; the caller then dispatches
 * @total_mirrors member bios and reports each one through
 * rc_amd_mirror_complete[_member](), passing this handle back.  The parent
 * is ended by the completion worker once the last member has reported — it
 * is NOT ended here, and the caller must not end it either.
 *
 * The handle is the caller's only way to report completions, so it must come
 * back from this call rather than be looked up later.
 *
 * Non-blocking: valid from any context, including atomic, and never sleeps.
 *
 * Fault isolation: when the engine is unloading, the arguments are bad, or
 * the GFP_ATOMIC allocation fails, the parent is ended immediately with
 * BLK_STS_RESOURCE (or BLK_STS_IOERR for a caller error) and NULL is
 * returned.  A NULL return means the bio is already finished and NO member
 * bios may be dispatched — the caller must not call any other function in
 * this file with a NULL context.  Data is never reported written on a path
 * where a member write may not have landed.
 *
 * Return: the context handle, or NULL if the parent was already ended.
 */
struct rc_mirror_context *rc_amd_queue_async_completion(struct bio *parent_bio,
							 int total_mirrors,
							 int status_flag)
{
	struct rc_mirror_context *ctx;
	unsigned long flags;
	bool online;

	if (unlikely(!parent_bio)) {
		pr_err("rcraid: async dispatch called without a parent bio\n");
		return NULL;
	}

	if (unlikely(total_mirrors <= 0)) {
		pr_err("rcraid: async dispatch with %d mirrors\n", total_mirrors);
		atomic64_inc(&rc_amd_engine.stats.rejected);
		rc_amd_bio_finish(parent_bio, BLK_STS_IOERR);
		return NULL;
	}

	/*
	 * GFP_ATOMIC, deliberately.  This sits on the submit path and may be
	 * reached with preemption disabled, where reclaimable memory would
	 * mean sleeping.  A mirror context is a small fixed-size leaf
	 * object, so the atomic-reserve path is the cheap one anyway.
	 */
	ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
	if (unlikely(!ctx)) {
		atomic64_inc(&rc_amd_engine.stats.faults);
		pr_warn_ratelimited("rcraid: no mirror context, deferring bio (BLK_STS_RESOURCE)\n");
		rc_amd_bio_finish(parent_bio, BLK_STS_RESOURCE);
		return NULL;
	}

	kthread_init_work(&ctx->completion_work, rc_amd_mirror_completion_work);
	spin_lock_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->registry);
	ctx->parent_bio = parent_bio;
	ctx->flags = 0;
	ctx->status = status_flag ? BLK_STS_IOERR : BLK_STS_OK;
	atomic_set(&ctx->remaining_mirrors, total_mirrors);

	/*
	 * Register under engine->lock and re-test @alive there, so a submit
	 * racing the start of unload either lands before the drain (and the
	 * drain waits for it) or is answered with BLK_STS_RESOURCE.  There
	 * is no window in which a context exists but is unaccounted for.
	 */
	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	online = rc_amd_engine.alive;
	if (online) {
		list_add_tail(&ctx->registry, &rc_amd_engine.contexts);
		atomic_inc(&rc_amd_engine.inflight);
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	if (!online) {
		atomic64_inc(&rc_amd_engine.stats.rejected);
		rc_amd_bio_finish(parent_bio, BLK_STS_RESOURCE);
		kfree(ctx);
		return NULL;
	}

	atomic64_inc(&rc_amd_engine.stats.dispatched);
	return ctx;
}
EXPORT_SYMBOL_GPL(rc_amd_queue_async_completion);

/**
 * rc_amd_mirror_complete - report one member bio's completion
 * @ctx: context from rc_amd_queue_async_completion()
 * @status: BLK_STS_OK if the member bio succeeded
 *
 * Call once per dispatched member bio, from that member's own completion
 * context — for NVMe, the MSI/softirq path.  Strictly non-blocking: this
 * only merges a status, bumps a counter, and possibly parks the parent
 * endio on the kthread queue.  No sleep, no allocation beyond the one
 * already taken at dispatch, no lock held across bio_endio().
 *
 * The last member to report is the one that queues the endio, so the
 * parent is ended exactly once, and never by a hardware context.
 *
 * Lifetime of @ctx: exactly the @total_mirrors reports promised at dispatch
 * are owed, and the last of them either queues the parent endio or, after an
 * abort, retires the context.  A report past that count is a bug in a member
 * path; the tombstone pool keeps the retired context readable so such a report
 * is counted rather than fatal.  Detect it as a caller, not just survive it.
 *
 * Return: 0 if the report was accepted, -EALREADY if the parent is already
 * being ended or has been retired (a duplicate report from a buggy member
 * path, or a late report from a set that was aborted), -EINVAL on a NULL
 * context.
 */
int rc_amd_mirror_complete(struct rc_mirror_context *ctx, blk_status_t status)
{
	unsigned long flags;
	int remaining;

	if (unlikely(!ctx))
		return -EINVAL;

	/*
	 * The merge, the countdown and the endio decision are one step under
	 * @ctx->lock.  Counting down outside it would let the completion
	 * worker observe zero, retire the context, and leave this report to
	 * merge a status into memory the engine no longer owns.
	 */
	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->flags & (RC_CTX_COMPLETING | RC_CTX_RETIRED | RC_CTX_GRAVE)) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		/* Owed by an aborted set, or not owed at all: counted,
		 * ignored, and — when it was the last one still owed — this is
		 * where the context gets retired. */
		rc_amd_straggler_report(ctx);
		return -EALREADY;
	}
	ctx->status = rc_status_merge(ctx->status, status);
	remaining = atomic_dec_return(&ctx->remaining_mirrors);
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (unlikely(remaining < 0)) {
		/* More reports than mirrors were promised.  The latch above
		 * should have caught this; count it rather than let a
		 * corrupted count escape into the next context. */
		atomic64_inc(&rc_amd_engine.stats.stale_reports);
		return -EALREADY;
	}

	if (status != BLK_STS_OK) {
		atomic64_inc(&rc_amd_engine.stats.member_errors);
		atomic64_inc(&rc_amd_engine.stats.media_faults);
	}

	if (remaining > 0)
		return 0;

	rc_amd_context_claim(ctx, 0, BLK_STS_OK);
	return 0;
}
EXPORT_SYMBOL_GPL(rc_amd_mirror_complete);

/**
 * rc_amd_mirror_complete_member - report a completion with member identity
 * @ctx: context from rc_amd_queue_async_completion()
 * @member: member slot index, for per-member counters and deprecation
 * @status: member bio status
 *
 * As rc_amd_mirror_complete(), but also attributes the report to a member
 * slot so amd_ctl0 and debugfs can show which member is slow or failing.
 * Same context rules, same non-blocking guarantees.
 */
int rc_amd_mirror_complete_member(struct rc_mirror_context *ctx, int member,
				  blk_status_t status)
{
	if (unlikely(!ctx))
		return -EINVAL;

	if (rc_amd_member_valid(member))
		atomic64_inc(&rc_amd_engine.members[member].completed);

	return rc_amd_mirror_complete(ctx, status);
}
EXPORT_SYMBOL_GPL(rc_amd_mirror_complete_member);

/**
 * rc_amd_mirror_abort - end a mirrored parent before all members reported
 * @ctx: context to fail
 * @status: status to report to the block layer
 *
 * Use when dispatching a member failed, so the parent is released
 * immediately instead of waiting on a member set that will never be
 * complete.  Members already in flight still report through
 * rc_amd_mirror_complete*(); those late reports are counted as stale and
 * change nothing, because the endio owner is latched here first.
 *
 * Safe from any context, including atomic.
 *
 * Return: 0 if this call ended the parent, -EALREADY if it was already
 * owned, -EINVAL on a NULL context.
 */
int rc_amd_mirror_abort(struct rc_mirror_context *ctx, blk_status_t status)
{
	if (unlikely(!ctx))
		return -EINVAL;

	return rc_amd_context_claim(ctx, 0, status) ? 0 : -EALREADY;
}
EXPORT_SYMBOL_GPL(rc_amd_mirror_abort);

/**
 * rc_amd_inflight_contexts - number of live mirror contexts
 *
 * Registered contexts that have not been freed yet.  Normally that is also
 * the number of parents not yet ended, but after rc_amd_mirror_abort() the
 * parent is gone while the members that were promised it are still entitled
 * to report, so the context outlives its own endio and this count stays
 * high.  "Parents not yet ended" is rc_async_stats.dispatched minus
 * rc_async_stats.completed.
 *
 * Exposed for debugfs and for the amd_ctl0 status mask.  An unload path
 * watching this fall to zero is the only correct way to know it is safe to
 * stop the completion worker.
 */
int rc_amd_inflight_contexts(void)
{
	return atomic_read(&rc_amd_engine.inflight);
}
EXPORT_SYMBOL_GPL(rc_amd_inflight_contexts);

/* =========================================================================
 * MEMBER FAULT LATCHES (amd_ctl0 FAIL_MEMBER)
 * ========================================================================= */

/**
 * rc_amd_fail_member - latch a member slot as failed
 * @member: member slot index
 *
 * Evicts a member from rebuild targeting and raises RC_AMD_STATUS_DEGRADED.
 * Latching is deliberate: the engine never evicts a member on its own from
 * an I/O error count, because silently degrading an array is worse than
 * reporting the error.  This is the explicit operator action the ioctl
 * bridge exposes.
 *
 * Return: 0, or -EINVAL for an out-of-range slot.
 */
int rc_amd_fail_member(int member)
{
	if (!rc_amd_member_valid(member))
		return -EINVAL;

	atomic_set(&rc_amd_engine.members[member].failed, 1);
	pr_warn("rcraid: member slot %d marked failed, rebuild routing will avoid it\n",
		member);
	return 0;
}
EXPORT_SYMBOL_GPL(rc_amd_fail_member);

/**
 * rc_amd_reinstate_member - clear a member's failed latch
 * @member: member slot index
 *
 * The first half of a re-add: a subsequent rc_amd_resync_request() then
 * routes chunks back onto this slot.
 *
 * Return: 0, or -EINVAL for an out-of-range slot.
 */
int rc_amd_reinstate_member(int member)
{
	if (!rc_amd_member_valid(member))
		return -EINVAL;

	atomic_set(&rc_amd_engine.members[member].failed, 0);
	pr_info("rcraid: member slot %d reinstated for rebuild routing\n", member);
	return 0;
}
EXPORT_SYMBOL_GPL(rc_amd_reinstate_member);

bool rc_amd_member_is_failed(int member)
{
	return rc_amd_member_failed(member);
}
EXPORT_SYMBOL_GPL(rc_amd_member_is_failed);

int rc_amd_failed_member_count(void)
{
	int i, failed = 0;

	for (i = 0; i < RC_ASYNC_MAX_MEMBERS; i++)
		if (atomic_read(&rc_amd_engine.members[i].failed))
			failed++;

	return failed;
}
EXPORT_SYMBOL_GPL(rc_amd_failed_member_count);

/* =========================================================================
 * REBUILD PUMP
 * ========================================================================= */

/**
 * rc_amd_set_resync_io - register the media layer's rebuild hooks
 * @chunk_fn: issue one chunk copy; must not wait for the member to finish
 * @outstanding_fn: in-flight copy count, polled with a sleep by this thread
 * @cookie: opaque pointer handed back to both hooks
 * @window: in-flight chunk budget, 0 selects RC_ASYNC_DEFAULT_WINDOW
 *
 * Register from the volume layer, before or while the engine is quiesced.
 * With no @chunk_fn registered, rc_amd_resync_request() succeeds but the
 * rebuild fails fast with -ENOSYS rather than pretending to have copied
 * anything — the engine drives the pump, the media layer does the copying.
 */
void rc_amd_set_resync_io(rc_resync_chunk_fn chunk_fn,
			  rc_resync_outstanding_fn outstanding_fn,
			  void *cookie, u32 window)
{
	unsigned long flags;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	rc_amd_engine.chunk_fn = chunk_fn;
	rc_amd_engine.outstanding_fn = outstanding_fn;
	rc_amd_engine.io_cookie = cookie;
	rc_amd_engine.io_window = window ? window : RC_ASYNC_DEFAULT_WINDOW;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);
}
EXPORT_SYMBOL_GPL(rc_amd_set_resync_io);

/**
 * rc_amd_set_resync_geometry - configure nested RAID 10 rebuild routing
 * @num_drives: member count, or 0 to disable and use @target_member verbatim
 * @chunk_sectors: chunk size matching the array's stripe geometry
 *
 * With a geometry configured, every rebuild chunk is resolved through
 * rc_amd_map_nested_raid10(), which returns both the parent/child address
 * and the mirror pair that owns the logical chunk.  The rebuild then writes
 * the replica of exactly the data that stripe holds, instead of the same
 * byte offset on a different pair.
 */
void rc_amd_set_resync_geometry(int num_drives, u32 chunk_sectors)
{
	unsigned long flags;

	if (num_drives && (num_drives < RC_ASYNC_MIN_NESTED_DRIVES ||
			   (num_drives & 1))) {
		pr_warn("rcraid: rejecting resync geometry of %d members, using explicit routing\n",
			num_drives);
		return;
	}

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	rc_amd_engine.geometry_drives = num_drives;
	rc_amd_engine.geometry_chunk = chunk_sectors;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	pr_info("rcraid: resync geometry %s (%d drives, chunk %u sectors)\n",
		num_drives ? "nested RAID 10" : "disabled", num_drives,
		chunk_sectors);
}
EXPORT_SYMBOL_GPL(rc_amd_set_resync_geometry);

/**
 * rc_amd_resync_kick - nudge the rebuild pump
 *
 * For the media layer to call after a copy retires, so the throttle wait
 * in rc_amd_resync_throttle() ends promptly instead of polling out its
 * full timeout.  Cheap and idempotent; safe from any context.
 */
void rc_amd_resync_kick(void)
{
	wake_up(&rc_amd_engine.throttle_wq);
	wake_up(&rc_amd_engine.resync_wq);
}
EXPORT_SYMBOL_GPL(rc_amd_resync_kick);

/**
 * rc_amd_resync_publish - snapshot job progress into the engine
 * @job: running job
 * @target_member: member this chunk was routed to
 */
static void rc_amd_resync_publish(const struct rc_resync_job *job,
				  int target_member)
{
	unsigned long flags;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	rc_amd_engine.resync.cursor = job->cursor;
	rc_amd_engine.resync.chunks = job->chunks;
	rc_amd_engine.resync.bytes = job->bytes;
	rc_amd_engine.resync.target_member = target_member;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);
}

/**
 * rc_amd_resync_pick_healthy - first non-failed member slot
 * @limit: highest slot to consider
 *
 * Return: slot index, or -1 if every candidate is failed.
 */
static int rc_amd_resync_pick_healthy(int limit)
{
	int i;

	if (limit < 1 || !rc_amd_member_valid(limit))
		limit = RC_ASYNC_MAX_MEMBERS;

	for (i = 0; i < limit; i++)
		if (!rc_amd_member_failed(i))
			return i;

	return -1;
}

/**
 * rc_amd_resync_route - resolve one logical chunk to a member and address
 * @job: running job
 * @logical: logical sector of the chunk
 * @target_sector: out: address handed to the media layer
 *
 * Return: member slot to copy from, or -1 if no healthy member is left.
 */
static int rc_amd_resync_route(const struct rc_resync_job *job, u64 logical,
			       u64 *target_sector)
{
	int drives = READ_ONCE(rc_amd_engine.geometry_drives);
	int member = job->target_member;
	u64 mapped = logical;

	if (drives >= RC_ASYNC_MIN_NESTED_DRIVES && !(drives & 1))
		mapped = rc_amd_map_nested_raid10(logical, job->chunk_sectors,
						   &member, drives);

	if (member < 0 || rc_amd_member_failed(member))
		member = rc_amd_resync_pick_healthy(drives);

	*target_sector = mapped;
	return member;
}

/**
 * rc_amd_resync_throttle - stay inside the media layer's in-flight window
 * @window: chunk budget registered via rc_amd_set_resync_io()
 *
 * Sleeps — never spins — while the media layer is more than @window chunks
 * ahead.  Sleeping is free here: this is the rebuild thread, no hardware
 * context is waiting on us, and the mirror completion pump runs on its own
 * worker and is entirely unaffected by this wait.
 */
static void rc_amd_resync_throttle(u32 window)
{
	rc_resync_outstanding_fn outstanding =
		READ_ONCE(rc_amd_engine.outstanding_fn);

	if (!outstanding)
		return;

	wait_event_timeout(rc_amd_engine.throttle_wq,
			   kthread_should_stop() ||
			   READ_ONCE(outstanding)(rc_amd_engine.io_cookie) < window,
			   msecs_to_jiffies(RC_ASYNC_THROTTLE_MS));
}

/**
 * rc_amd_resync_run - drive one job to completion or cancellation
 * @job: job owned by the resync thread
 */
static void rc_amd_resync_run(struct rc_resync_job *job)
{
	rc_resync_chunk_fn chunk_fn = READ_ONCE(rc_amd_engine.chunk_fn);
	unsigned long flags;
	u32 window;

	if (!chunk_fn) {
		job->result = -ENOSYS;
		return;
	}

	window = READ_ONCE(rc_amd_engine.io_window);

	while (job->cursor < job->end_sector) {
		u64 target_sector;
		u32 nr_sectors;
		int target_member;
		int rc;

		/* Unload lands here, not after the whole array is walked. */
		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&rc_amd_engine.lock, flags);
		if (rc_amd_engine.resync_cancel) {
			spin_unlock_irqrestore(&rc_amd_engine.lock, flags);
			job->result = -ECANCELED;
			break;
		}
		spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

		nr_sectors = min_t(u32, job->chunk_sectors,
				   job->end_sector - job->cursor);

		target_member = rc_amd_resync_route(job, job->cursor,
						    &target_sector);
		if (unlikely(target_member < 0)) {
			job->result = -ENODEV;
			break;
		}

		/*
		 * The one call that can touch hardware.  It runs unlocked,
		 * in sleepable context, and by contract it only *issues*
		 * the copy — the throttle below, not a wait here, is what
		 * keeps the rebuild from outrunning the member.
		 */
		rc = chunk_fn(rc_amd_engine.io_cookie, job->source_member,
			      target_member, target_sector, nr_sectors);
		if (unlikely(rc)) {
			job->result = rc;
			break;
		}

		job->cursor += nr_sectors;
		job->chunks++;
		job->bytes += (u64)nr_sectors << SECTOR_SHIFT;

		rc_amd_resync_publish(job, target_member);
		rc_amd_resync_throttle(window);

		/* Rebuild yields after every chunk: it is background work
		 * and must stay out of the way of foreground I/O. */
		cond_resched();
	}
}

/**
 * rc_amd_resync_finish - record terminal state and free a job
 * @job: job the resync thread has finished with
 *
 * Also re-arms the cancel brake.  By the time this runs no job is in
 * progress, so leaving rc_amd_engine.resync_cancel set would keep
 * rc_amd_resync_request() refusing work forever.  The whole block is under
 * engine->lock, so a cancel arriving concurrently either latched before this
 * (harmless: the job it targeted has already stopped and there is nothing
 * left for it to interrupt) or after (and correctly hits the new job).
 */
static void rc_amd_resync_finish(struct rc_resync_job *job)
{
	unsigned long flags;
	int state;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	if (job->result == -ECANCELED)
		state = RC_RESYNC_CANCELLED;
	else if (job->result)
		state = RC_RESYNC_FAILED;
	else
		state = RC_RESYNC_DONE;

	rc_amd_engine.resync.state = state;
	rc_amd_engine.resync.result = job->result;
	rc_amd_engine.resync.cursor = job->cursor;
	rc_amd_engine.resync.chunks = job->chunks;
	rc_amd_engine.resync.bytes = job->bytes;
	job->state = state;
	rc_amd_engine.resync_cancel = false;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	if (state == RC_RESYNC_DONE) {
		atomic64_inc(&rc_amd_engine.stats.resync_done);
		pr_info("rcraid: resync %s, %llu chunks, %llu MiB\n",
			rc_resync_state_name(state), job->chunks,
			job->bytes >> 20);
	} else if (state == RC_RESYNC_CANCELLED) {
		atomic64_inc(&rc_amd_engine.stats.resync_cancelled);
		pr_info("rcraid: resync %s at sector %llu/%llu\n",
			rc_resync_state_name(state), job->cursor,
			job->end_sector);
	} else {
		atomic64_inc(&rc_amd_engine.stats.resync_failed);
		pr_err("rcraid: resync %s at sector %llu/%llu (rc %d)\n",
		       rc_resync_state_name(state), job->cursor,
		       job->end_sector, job->result);
	}

	kfree(job);
}

/**
 * rc_amd_resync_claim - take ownership of a pending job
 *
 * Return: the job to run, or NULL if none is queued.
 */
static struct rc_resync_job *rc_amd_resync_claim(void)
{
	struct rc_resync_job *job;
	unsigned long flags;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	job = rc_amd_engine.pending_job;
	if (job) {
		WRITE_ONCE(rc_amd_engine.pending_job, NULL);
		rc_amd_engine.resync.state = RC_RESYNC_RUNNING;
		job->state = RC_RESYNC_RUNNING;
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	return job;
}

/**
 * rc_amd_resync_thread - kamd_resync body
 * @unused: unused
 *
 * A kthread, not a kthread_worker: the rebuild needs set_current_state(),
 * cond_resched() and bounded sleeps between chunks, and none of those are
 * available in a kthread_work callback.  Keeping it separate from
 * kamd_async_worker is the point — a multi-hour array walk must never be
 * able to delay a parent bio's endio.
 */
static int rc_amd_resync_thread(void *unused)
{
	struct rc_resync_job *job;
	unsigned long flags;

	while (!kthread_should_stop()) {
		job = rc_amd_resync_claim();
		if (!job) {
			/*
			 * Sleep on the waitqueue, never spin.  A resync
			 * request wakes this immediately; the timeout is only
			 * a lost-wakeup backstop, and it is interruptible so
			 * kthread_stop() does not have to wait it out.
			 */
			set_current_state(TASK_IDLE);
			wait_event_interruptible_timeout(
				rc_amd_engine.resync_wq,
				unlikely(READ_ONCE(rc_amd_engine.pending_job)) ||
				kthread_should_stop(),
				msecs_to_jiffies(RC_ASYNC_RESYNC_IDLE_MS));
			__set_current_state(TASK_RUNNING);
			continue;
		}

		rc_amd_resync_run(job);
		rc_amd_resync_finish(job);
	}

	/* Unloading: do not leave a progress record claiming work is live. */
	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	if (rc_amd_engine.resync.state == RC_RESYNC_RUNNING) {
		rc_amd_engine.resync.state = RC_RESYNC_CANCELLED;
		rc_amd_engine.resync.result = -ECANCELED;
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	return 0;
}

/**
 * rc_amd_resync_request - start a background rebuild
 * @start_sector: first logical sector of the range
 * @end_sector: one past the last logical sector
 * @chunk_sectors: rebuild granularity in sectors, must be > 0
 * @source_member: source slot, or -1 to let the media layer choose
 * @target_member: explicit target slot, or -1 for per-chunk routing
 *
 * The amd_ctl0 START_RESYNC entry point.  Returns as soon as the job is
 * queued — it never waits for the rebuild and never sleeps on the member,
 * because the caller is a control thread and the copy belongs to
 * kamd_resync.
 *
 * Return: 0 on accept, -EBUSY if a rebuild is already queued or running,
 * -ENODEV if the engine is unloading, -EINVAL on a nonsensical range or
 * member slot.
 */
int rc_amd_resync_request(u64 start_sector, u64 end_sector, u32 chunk_sectors,
			  int source_member, int target_member)
{
	struct rc_resync_job *job;
	unsigned long flags;
	bool online, busy;

	if (unlikely(end_sector <= start_sector || !chunk_sectors))
		return -EINVAL;
	if (unlikely(!rc_amd_member_valid(source_member) && source_member != -1))
		return -EINVAL;
	if (unlikely(!rc_amd_member_valid(target_member) && target_member != -1))
		return -EINVAL;

	/*
	 * GFP_ATOMIC so the "never block a caller" promise of this entry
	 * point holds unconditionally, even if a future caller turns out to
	 * be in atomic context.
	 */
	job = kzalloc(sizeof(*job), GFP_ATOMIC);
	if (unlikely(!job)) {
		atomic64_inc(&rc_amd_engine.stats.faults);
		return -ENOMEM;
	}

	job->start_sector = start_sector;
	job->end_sector = end_sector;
	job->cursor = start_sector;
	job->chunk_sectors = chunk_sectors;
	job->source_member = source_member;
	job->target_member = target_member;
	job->state = RC_RESYNC_PENDING;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	online = rc_amd_engine.alive;
	busy = rc_amd_engine.pending_job ||
	       rc_amd_engine.resync.state == RC_RESYNC_RUNNING ||
	       rc_amd_engine.resync_cancel;
	if (online && !busy) {
		/*
		 * WRITE_ONCE, to pair with the READ_ONCE() the rebuild
		 * thread samples it with.  The store is already serialised by
		 * engine->lock, but the matching macro is what tells the
		 * compiler this field is deliberately readable without it,
		 * so neither side may be torn, split, or hoisted across the
		 * wake_up() below.
		 */
		WRITE_ONCE(rc_amd_engine.pending_job, job);
		rc_amd_engine.resync.start_sector = start_sector;
		rc_amd_engine.resync.end_sector = end_sector;
		rc_amd_engine.resync.cursor = start_sector;
		rc_amd_engine.resync.chunks = 0;
		rc_amd_engine.resync.bytes = 0;
		rc_amd_engine.resync.result = 0;
		rc_amd_engine.resync.target_member = target_member;
		rc_amd_engine.resync.state = RC_RESYNC_PENDING;
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	if (!online) {
		kfree(job);
		return -ENODEV;
	}
	if (busy) {
		kfree(job);
		return -EBUSY;
	}

	atomic64_inc(&rc_amd_engine.stats.resync_requests);
	pr_info("rcraid: resync queued, sectors %llu..%llu, chunk %u, src %d, dst %d\n",
		start_sector, end_sector, chunk_sectors, source_member,
		target_member);

	wake_up(&rc_amd_engine.resync_wq);
	return 0;
}
EXPORT_SYMBOL_GPL(rc_amd_resync_request);

/**
 * rc_amd_resync_cancel - stop a rebuild at the next chunk boundary
 *
 * The amd_ctl0 operator brake.  Returns immediately: it either detaches a
 * job the thread has not claimed yet, or latches a flag the running job
 * polls between chunks.  It deliberately does not wait for the member to
 * acknowledge — a cancel that blocked would stall exactly the I/O
 * completions the rebuild is competing with.
 *
 * The brake is consumable, not a latch for the lifetime of the module: a
 * cancel is only latched when there is a job to stop, and the job clears it
 * as it exits.  Latching it unconditionally would make rc_amd_resync_request()
 * answer -EBUSY to every rebuild after the first cancel, with no way to
 * recover short of reloading the driver.
 */
void rc_amd_resync_cancel(void)
{
	struct rc_resync_job *job = NULL;
	unsigned long flags;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	if (rc_amd_engine.pending_job) {
		/* Not claimed yet: retire it here; the thread never sees it. */
		job = rc_amd_engine.pending_job;
		WRITE_ONCE(rc_amd_engine.pending_job, NULL);
		job->result = -ECANCELED;
		rc_amd_engine.resync.state = RC_RESYNC_CANCELLED;
		rc_amd_engine.resync.result = -ECANCELED;
	} else if (rc_amd_engine.resync.state == RC_RESYNC_RUNNING) {
		/* Latched for the running job only; rc_amd_resync_finish()
		 * clears it, so the next request is accepted. */
		rc_amd_engine.resync_cancel = true;
		pr_info("rcraid: resync cancel latched, stopping at next chunk\n");
	} else {
		/* Nothing to stop.  Latching here would refuse every future
		 * rebuild, so say so instead of quietly disarming it. */
		pr_info("rcraid: resync cancel ignored, no rebuild in flight\n");
	}
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	if (job) {
		atomic64_inc(&rc_amd_engine.stats.resync_cancelled);
		pr_info("rcraid: resync cancelled before it started\n");
		kfree(job);
	}

	wake_up(&rc_amd_engine.resync_wq);
}
EXPORT_SYMBOL_GPL(rc_amd_resync_cancel);

/**
 * rc_amd_get_resync_progress - snapshot rebuild progress
 * @out: destination
 *
 * Return: the enum rc_resync_state value, or -EINVAL on a NULL @out.
 */
int rc_amd_get_resync_progress(struct rc_resync_progress *out)
{
	unsigned long flags;
	int state;

	if (unlikely(!out))
		return -EINVAL;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	*out = rc_amd_engine.resync;
	state = out->state;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	return state;
}
EXPORT_SYMBOL_GPL(rc_amd_get_resync_progress);

/* =========================================================================
 * STATUS AND TELEMETRY
 * ========================================================================= */

/**
 * rc_amd_get_status - engine status mask for amd_ctl0 GET_STATUS
 *
 * Bit 0 (RC_AMD_STATUS_LIVE) is the "array healthy" baseline the bridge
 * already reported, so an unmodified user-space reader is unaffected.  The
 * remaining bits are additive: a failed member, a rebuild in flight,
 * BLK_STS_RESOURCE pressure, or media errors seen since load.
 *
 * Non-blocking: takes a spinlock and reads counters, nothing else.
 */
u32 rc_amd_get_status(void)
{
	unsigned long flags;
	u32 mask = 0;
	bool live, resyncing, overload;
	int i;

	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	live = rc_amd_engine.alive;
	resyncing = rc_amd_engine.resync.state == RC_RESYNC_RUNNING;
	overload = atomic_read(&rc_amd_engine.inflight) >
		   RC_ASYNC_OVERLOAD_HIGH_WATER;
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);

	if (live)
		mask |= RC_AMD_STATUS_LIVE;
	if (resyncing)
		mask |= RC_AMD_STATUS_RESYNCING;
	if (overload)
		mask |= RC_AMD_STATUS_OVERLOAD;

	for (i = 0; i < RC_ASYNC_MAX_MEMBERS; i++) {
		if (atomic_read(&rc_amd_engine.members[i].failed)) {
			mask |= RC_AMD_STATUS_DEGRADED;
			break;
		}
	}

	/* Counters are read without the lock: each is a single atomic read. */
	if (atomic64_read(&rc_amd_engine.stats.member_errors) ||
	    atomic64_read(&rc_amd_engine.stats.faults) ||
	    atomic64_read(&rc_amd_engine.stats.resource_injections))
		mask |= RC_AMD_STATUS_FAULTS;

	return mask;
}
EXPORT_SYMBOL_GPL(rc_amd_get_status);

/**
 * rc_amd_get_async_stats - copy the engine counters out
 * @out: destination
 *
 * Snapshot only; no synchronisation with in-flight counters beyond each
 * individual atomic read.  For debugfs and for the ioctl bridge's status
 * query, not for control flow.
 */
void rc_amd_get_async_stats(struct rc_async_stats *out)
{
	unsigned long flags;

	if (unlikely(!out))
		return;

	/*
	 * Copy under engine->lock so the block is consistent as a set.  It is
	 * a leaf struct and the lock is never held across anything that can
	 * block, so this cannot become a contention point on the I/O path.
	 */
	spin_lock_irqsave(&rc_amd_engine.lock, flags);
	atomic64_set(&out->dispatched, atomic64_read(&rc_amd_engine.stats.dispatched));
	atomic64_set(&out->completed, atomic64_read(&rc_amd_engine.stats.completed));
	atomic64_set(&out->mirror_ok, atomic64_read(&rc_amd_engine.stats.mirror_ok));
	atomic64_set(&out->mirror_errors, atomic64_read(&rc_amd_engine.stats.mirror_errors));
	atomic64_set(&out->member_errors, atomic64_read(&rc_amd_engine.stats.member_errors));
	atomic64_set(&out->faults, atomic64_read(&rc_amd_engine.stats.faults));
	atomic64_set(&out->resource_injections, atomic64_read(&rc_amd_engine.stats.resource_injections));
	atomic64_set(&out->rejected, atomic64_read(&rc_amd_engine.stats.rejected));
	atomic64_set(&out->stale_reports, atomic64_read(&rc_amd_engine.stats.stale_reports));
	atomic64_set(&out->media_faults, atomic64_read(&rc_amd_engine.stats.media_faults));
	atomic64_set(&out->drain_forced, atomic64_read(&rc_amd_engine.stats.drain_forced));
	atomic64_set(&out->resync_requests, atomic64_read(&rc_amd_engine.stats.resync_requests));
	atomic64_set(&out->resync_done, atomic64_read(&rc_amd_engine.stats.resync_done));
	atomic64_set(&out->resync_failed, atomic64_read(&rc_amd_engine.stats.resync_failed));
	atomic64_set(&out->resync_cancelled, atomic64_read(&rc_amd_engine.stats.resync_cancelled));
	spin_unlock_irqrestore(&rc_amd_engine.lock, flags);
}
EXPORT_SYMBOL_GPL(rc_amd_get_async_stats);

/* =========================================================================
 * SUBSYSTEM LIFECYCLE
 * ========================================================================= */

/**
 * rc_amd_init_async_subsystem - start both pumps
 *
 * Called from rc_init() before the block major is registered.  Order
 * matters: the worker is initialised and marked alive before either thread
 * exists, so a thread can never observe a half-built engine, and no
 * submission can be accepted before the worker that will drain it.
 *
 * Return: 0, or a negative errno.  The completion worker is required — if
 * it cannot be created the engine cannot serve mirrors at all, so init
 * fails.  The rebuild thread is not: it drives background work, and a
 * system too short on threads to spare one should still run foreground
 * I/O.  A missing rebuild thread leaves rc_amd_resync_request() failing
 * with -ENODEV and is reported here.
 */
int rc_amd_init_async_subsystem(void)
{
	struct rc_async_engine *engine = &rc_amd_engine;
	unsigned long flags;
	int i, err;

	memset(engine, 0, sizeof(*engine));

	spin_lock_init(&engine->lock);
	INIT_LIST_HEAD(&engine->contexts);
	INIT_LIST_HEAD(&engine->graveyard);
	engine->graves = 0;
	init_waitqueue_head(&engine->drain_wq);
	init_waitqueue_head(&engine->resync_wq);
	init_waitqueue_head(&engine->throttle_wq);
	kthread_init_worker(&engine->completion_worker);
	atomic_set(&engine->inflight, 0);

	for (i = 0; i < RC_ASYNC_MAX_MEMBERS; i++) {
		atomic_set(&engine->members[i].failed, 0);
		atomic64_set(&engine->members[i].completed, 0);
		atomic64_set(&engine->members[i].errors, 0);
	}

	/* Publish before either thread runs: first accept, then serve. */
	spin_lock_irqsave(&engine->lock, flags);
	engine->alive = true;
	spin_unlock_irqrestore(&engine->lock, flags);

	engine->completion_task = kthread_run(kthread_worker_fn,
					      &engine->completion_worker,
					      RC_ASYNC_WORKER_NAME);
	if (IS_ERR(engine->completion_task)) {
		err = PTR_ERR(engine->completion_task);
		/* Clear the ERR_PTR: rc_amd_exit_async_subsystem() and every
		 * other reader test this field for truthiness, and a live
		 * ERR_PTR there would be kthread_stop()'d. */
		engine->completion_task = NULL;
		spin_lock_irqsave(&engine->lock, flags);
		engine->alive = false;
		spin_unlock_irqrestore(&engine->lock, flags);
		pr_err("rcraid: cannot create %s: %d\n", RC_ASYNC_WORKER_NAME, err);
		return err;
	}

	engine->resync_task = kthread_run(rc_amd_resync_thread, NULL,
					  RC_ASYNC_RESYNC_NAME);
	if (IS_ERR(engine->resync_task)) {
		err = PTR_ERR(engine->resync_task);
		engine->resync_task = NULL;
		pr_err("rcraid: cannot create %s: %d (rebuild disabled, mirrors unaffected)\n",
		       RC_ASYNC_RESYNC_NAME, err);
	}

	pr_info("rcraid: async engine up: worker \"%s\"%s, rebuild \"%s\"%s\n",
		RC_ASYNC_WORKER_NAME,
		engine->completion_task ? "" : " (MISSING)",
		RC_ASYNC_RESYNC_NAME, engine->resync_task ? "" : " (MISSING)");
	return 0;
}
EXPORT_SYMBOL_GPL(rc_amd_init_async_subsystem);

/**
 * rc_amd_drain_force - fail every context that outlived the drain budget
 *
 * Walks the registry under engine->lock and latches RC_CTX_RESOURCE on
 * each context, so the completion worker answers the survivors with
 * BLK_STS_RESOURCE instead of leaving their parents dangling.  Taking
 * engine->lock and then ctx->lock inside the claim helper is the one lock
 * order used in this file, so this cannot invert against a member report.
 *
 * Return: number of contexts forced.
 */
static int rc_amd_drain_force(void)
{
	struct rc_async_engine *engine = &rc_amd_engine;
	struct rc_mirror_context *ctx, *tmp;
	unsigned long flags;
	int forced = 0;

	/*
	 * engine->lock is held while claiming.  That is safe here and only
	 * here: rc_amd_context_claim() is the single place that nests ctx->lock
	 * under engine->lock, it never blocks, and the ctx->lock-then-engine->lock
	 * order is never taken (rc_amd_context_reap() and
	 * rc_amd_resync_publish() are both called with ctx->lock dropped).
	 * kthread_queue_work() underneath may make the worker run at once, but
	 * that worker then waits here on engine->lock — it holds nothing else,
	 * so the worst case is contention, never a deadlock.
	 */
	spin_lock_irqsave(&engine->lock, flags);
	list_for_each_entry_safe(ctx, tmp, &engine->contexts, registry) {
		if (rc_amd_context_claim(ctx, RC_CTX_RESOURCE, BLK_STS_OK))
			forced++;
	}
	spin_unlock_irqrestore(&engine->lock, flags);

	if (forced)
		atomic64_add(forced, &engine->stats.drain_forced);

	return forced;
}

/**
 * rc_amd_context_purge - drop every context still on the registry
 *
 * The last step of rc_amd_exit_async_subsystem(), after the bounded drain has
 * twice failed to reach zero.  At that point some member has never reported
 * and the wait is over: the contexts are unlinked and freed so nothing dangles
 * into an unloaded module.  A context that already issued its endio but is
 * still owed stragglers (an aborted set) lands here too — it cannot be forced,
 * because it has nothing left to end — which is exactly why this exists.
 *
 * The graveyard goes at the same time.  Tombstones exist to absorb a report
 * that races a retired context; once the module is going away there is nothing
 * left to protect them from, and the caller is contractually required to have
 * quiesced every member path before calling in.
 *
 * Every member path must already be quiesced by the caller before unload, or
 * a straggler arriving after this point touches freed memory; that ordering is
 * the caller's contract with the block layer, not something this can enforce.
 *
 * Return: number of live contexts purged, not counting tombstones.
 */
static int rc_amd_context_purge(void)
{
	struct rc_async_engine *engine = &rc_amd_engine;
	struct rc_mirror_context *ctx, *tmp;
	struct list_head dying, graves;
	unsigned long flags;
	int purged = 0, dropped = 0;

	INIT_LIST_HEAD(&dying);
	INIT_LIST_HEAD(&graves);

	/*
	 * Count before splicing.  A spliced list is walked through its own
	 * head, and that head has to be the one the entries point back at —
	 * copying a list_head out of the struct and iterating from the copy
	 * is the classic way to walk straight off the end, because nothing in
	 * the copied links ever refers to the copy.
	 */
	spin_lock_irqsave(&engine->lock, flags);
	list_for_each_entry_safe(ctx, tmp, &engine->contexts, registry)
		purged++;
	dropped = engine->graves;
	list_splice_init(&engine->contexts, &dying);
	list_splice_init(&engine->graveyard, &graves);
	engine->graves = 0;
	spin_unlock_irqrestore(&engine->lock, flags);

	list_for_each_entry_safe(ctx, tmp, &dying, registry) {
		list_del(&ctx->registry);
		atomic_dec(&engine->inflight);
		kfree(ctx);
	}

	/* Tombstones are already off the registry, so they do not touch
	 * @inflight: the drain is satisfied by the loop above, or was not. */
	list_for_each_entry_safe(ctx, tmp, &graves, registry) {
		list_del(&ctx->registry);
		kfree(ctx);
	}

	if (purged) {
		atomic64_add(purged, &engine->stats.drain_forced);
		wake_up(&engine->drain_wq);
	}

	if (dropped)
		pr_info("rcraid: released %d retired mirror context(s)\n", dropped);

	return purged;
}

/**
 * rc_amd_exit_async_subsystem - stop both pumps and free every context
 *
 * Called from rc_exit() and from rc_init()'s error paths, in process
 * context, after the module is unbound.  The sequence is deliberate:
 *
 *   1. clear @alive, so no new context or rebuild request is accepted;
 *   2. retire a rebuild request that no thread ever claimed;
 *   3. drain in-flight contexts, bounded — this is the only blocking wait on
 *      the hot path, and it exists so no member completion can reference a
 *      context we are about to free;
 *   4. force the stragglers to BLK_STS_RESOURCE and drain again;
 *   5. flush the completion queue, then purge whatever is still registered —
 *      members that never reported at all, and aborted sets whose parents are
 *      already ended — along with the tombstones;
 *   6. stop the rebuild thread;
 *   7. flush and stop the completion worker LAST, because
 *      kthread_worker_fn() abandons queued work once its stop flag is set,
 *      so any parent still parked on the queue must be released first.
 *
 * Idempotent: a second call finds no tasks and returns.
 */
void rc_amd_exit_async_subsystem(void)
{
	struct rc_async_engine *engine = &rc_amd_engine;
	struct rc_resync_job *pending;
	struct rc_async_stats stats;
	unsigned long flags;
	unsigned long left;
	int forced, purged, inflight;

	spin_lock_irqsave(&engine->lock, flags);
	if (!engine->alive && !engine->completion_task && !engine->resync_task) {
		spin_unlock_irqrestore(&engine->lock, flags);
		return;
	}
	engine->alive = false;
	engine->resync_cancel = true;
	pending = engine->pending_job;
	WRITE_ONCE(engine->pending_job, NULL);
	spin_unlock_irqrestore(&engine->lock, flags);

	if (pending) {
		pending->result = -ECANCELED;
		kfree(pending);
	}

	inflight = atomic_read(&engine->inflight);
	if (inflight) {
		left = wait_event_timeout(engine->drain_wq,
					  !atomic_read(&engine->inflight),
					  msecs_to_jiffies(RC_ASYNC_DRAIN_TIMEOUT_MS));
		if (!left) {
			forced = rc_amd_drain_force();
			pr_warn("rcraid: drain timed out with %d context(s) outstanding, failing %d with BLK_STS_RESOURCE\n",
				atomic_read(&engine->inflight), forced);
			left = wait_event_timeout(engine->drain_wq,
						  !atomic_read(&engine->inflight),
						  msecs_to_jiffies(RC_ASYNC_DRAIN_TIMEOUT_MS));
		}
	}

	/*
	 * Always reach the purge, not just the stuck path: on the clean path
	 * the registry is empty and this only releases the tombstones.
	 *
	 * The flush before it is what makes the purge safe on the stuck path.
	 * A context can be registered with its endio already queued but not
	 * yet run — the worker can simply lose the race to the timeout — and
	 * the force pass will not re-queue it, because RC_CTX_COMPLETING is
	 * already latched.  Freeing it under a still-queued kthread_work is a
	 * use-after-free the moment the worker wakes up.  This is the second
	 * blocking wait in the file; it is on the unload path, in process
	 * context, after the drain budget has already been spent.
	 */
	kthread_flush_worker(&engine->completion_worker);
	purged = rc_amd_context_purge();
	if (purged)
		pr_warn("rcraid: drain still has %d context(s) after the full budget, purging %d\n",
			atomic_read(&engine->inflight), purged);

	/* Rebuild first: it issues no completions, but stopping it before the
	 * completion worker keeps the shutdown sequence single-directional. */
	if (engine->resync_task) {
		kthread_stop(engine->resync_task);
		engine->resync_task = NULL;
	}

	if (engine->completion_task) {
		kthread_flush_worker(&engine->completion_worker);
		kthread_stop(engine->completion_task);
		engine->completion_task = NULL;
	}

	rc_amd_get_async_stats(&stats);
	pr_info("rcraid: async engine down: %lld dispatched, %lld completed (%lld ok, %lld errors), %lld BLK_STS_RESOURCE injected, %lld stale report(s), %lld forced or purged at drain\n",
		atomic64_read(&stats.dispatched),
		atomic64_read(&stats.completed),
		atomic64_read(&stats.mirror_ok),
		atomic64_read(&stats.mirror_errors),
		atomic64_read(&stats.resource_injections),
		atomic64_read(&stats.stale_reports),
		atomic64_read(&stats.drain_forced));
}
EXPORT_SYMBOL_GPL(rc_amd_exit_async_subsystem);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Independent Cleanroom Async Architecture Team");
MODULE_DESCRIPTION("Asynchronous mirror completion and rebuild pump for the AMD RAID array");