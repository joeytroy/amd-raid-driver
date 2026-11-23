// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side concurrency harness for the async mirror engine.
 *
 * NOT part of the kernel module and not built by the kbuild Makefile.  It
 * compiles the real src/patch_async_worker.c unmodified and links it against
 * a userspace stand-in for the handful of kernel primitives the engine
 * touches.  The point is to run the engine's actual locking and accounting
 * code under real threads and real preemption, which is where bugs in this
 * kind of code live.
 *
 *   make test-async          # build + run
 *   make test-async-asan     # memory errors, undefined behaviour, leaks
 *   make test-async-tsan     # data races
 *
 * Those targets materialize the empty "linux/..." header stand-ins the engine
 * #includes into .async-test/shim and pass the include path; this file only
 * needs -Isrc.  -Wno-unused-parameter because engine code follows the kernel's
 * unnamed-parameter convention, which -Wextra flags but the kernel build does
 * not.  Drain budget is cut to 300 ms (engine default 5000 ms) because the
 * stuck-member unload test is deliberately slower than any real drain.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * KERNEL SHIM
 * ========================================================================= */

/* Kernel fixed-width typedefs the engine's sources expect. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* Kernel u64 is unsigned long long on every LP64 target this harness runs on. */
typedef unsigned long long u64;
typedef int8_t   s8;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t blk_status_t;

#define BLK_STS_OK			0
#define BLK_STS_NOTSUPP			1
#define BLK_STS_TIMEOUT			2
#define BLK_STS_NOSPC			3
#define BLK_STS_TRANSPORT		4
#define BLK_STS_TARGET			5
#define BLK_STS_RESV_CONFLICT		6
#define BLK_STS_MEDIUM			7
#define BLK_STS_PROTECTION		8
#define BLK_STS_RESOURCE		9
#define BLK_STS_IOERR			10
#define BLK_STS_AGAIN			12
#define BLK_STS_DEV_RESOURCE		13
#define BLK_STS_ZONE_OPEN_RESOURCE	14
#define BLK_STS_ZONE_ACTIVE_RESOURCE	15
#define BLK_STS_OFFLINE			16
#define BLK_STS_DURATION_LIMIT		17
#define BLK_STS_INVAL			19

#define SECTOR_SHIFT		9

typedef struct { pthread_mutex_t m; } spinlock_t;
typedef struct { atomic_llong v; } atomic_t;
typedef struct { atomic_llong v; } atomic64_t;

/*
 * Note the kernel convention: these take a POINTER to the atomic.
 * The macro parameters are deliberately not named `v` — a parameter named
 * `v` would be substituted into the `(ptr)->v` member access in the body.
 */
#define atomic_read(p)		((int)atomic_load(&(p)->v))
#define atomic_set(p, val)	atomic_store(&(p)->v, (long long)(val))
#define atomic_inc(p)		atomic_fetch_add(&(p)->v, 1)
#define atomic_dec(p)		atomic_fetch_sub(&(p)->v, 1)
/* The kernel's atomic_dec_return() hands back the value *after* the
 * decrement — that is the whole point of it, and the engine's "last member
 * reports" transition tests for exactly 0.  atomic_fetch_sub() returns the
 * old value, so subtract one here or every countdown is off by one. */
#define atomic_dec_return(p)	((int)(atomic_fetch_sub(&(p)->v, 1) - 1))
#define atomic64_read(p)	atomic_load(&(p)->v)
#define atomic64_set(p, val)	atomic_store(&(p)->v, (long long)(val))
#define atomic64_inc(p)		atomic_fetch_add(&(p)->v, 1)
#define atomic64_add(i, p)	atomic_fetch_add(&(p)->v, (long long)(i))

#define shim_container_of(ptr, type, member)				\
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define container_of(ptr, type, member)	shim_container_of(ptr, type, member)

#define spin_lock_init(l)	pthread_mutex_init(&(l)->m, NULL)
#define spin_lock(l)		pthread_mutex_lock(&(l)->m)
#define spin_unlock(l)		pthread_mutex_unlock(&(l)->m)
#define spin_lock_irqsave(l, f)	do { (f) = 1; pthread_mutex_lock(&(l)->m); } while (0)
#define spin_unlock_irqrestore(l, f)	do { (void)(f); pthread_mutex_unlock(&(l)->m); } while (0)

#define unlikely(x)		(x)

static int shim_log_quiet;

#define pr_info(fmt, ...)	do { if (!shim_log_quiet) printf("KINFO  " fmt, ##__VA_ARGS__); } while (0)
#define pr_warn(fmt, ...)	do { if (!shim_log_quiet) printf("KWARN  " fmt, ##__VA_ARGS__); } while (0)
#define pr_err(fmt, ...)	do { printf("KERR   " fmt, ##__VA_ARGS__); } while (0)
#define pr_warn_ratelimited(fmt, ...)	pr_warn(fmt, ##__VA_ARGS__)

#define msecs_to_jiffies(ms)	((long)(ms))

typedef int gfp_t;
#define GFP_ATOMIC	0
#define GFP_KERNEL	1

static atomic_int shim_alloc_fail;

static void *shim_alloc(size_t n)
{
	if (atomic_exchange(&shim_alloc_fail, 0))
		return NULL;
	return calloc(1, n);
}

#define kmalloc(n, f)	shim_alloc((n))
#define kzalloc(n, f)	shim_alloc((n))
#define kfree(p)	free(p)

#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)

/*
 * The kernel's READ_ONCE()/WRITE_ONCE() are a volatile access plus a compiler
 * barrier: they stop the compiler tearing, splitting or hoisting the access,
 * and they order nothing.  The engine uses them for one deliberate purpose —
 * the rebuild thread samples @pending_job in its wait condition to narrow the
 * lost-wakeup window — and the matching WRITE_ONCE() on the publishing side is
 * what keeps the pair well defined at both ends.
 *
 * A plain (x) would compile to the same load but carry no marker, and
 * ThreadSanitizer would report the mutex-protected store as racing with it.
 * The relaxed atomics say exactly what the kernel macros mean: one
 * indivisible access, no ordering, visible to the sanitizer.
 */
#define READ_ONCE(x)		__atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define min_t(t, a, b)	((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define cond_resched()	sched_yield()

/* The shim's kthread_run returns NULL on failure rather than an ERR_PTR, so
 * IS_ERR/PTR_ERR collapse to the userspace error convention. */
#define IS_ERR(p)		((p) == NULL)
#define PTR_ERR(p)		(-ENOMEM)

/* --- struct bio ----------------------------------------------------------- */

struct bio {
	blk_status_t	bi_status;
	int		endio_calls;
	int		endio_in_hw_ctx;
	int		unique;
};

/* Set while a thread is emulating a hardware completion context, so we can
 * assert that the engine never ends a parent bio from one. */
static _Thread_local int in_hardware_context;

/*
 * Suite-wide tally of endio calls made from a hardware context.  This is the
 * single invariant the whole design exists to protect: a mirrored parent is
 * only ever ended by the completion worker, never on a member's softirq
 * path.  Checked once at the end of the run, so no individual test can
 * quietly stop covering it.
 */
static atomic_llong hw_ctx_endios;

static void bio_endio(struct bio *bio)
{
	bio->endio_calls++;
	bio->endio_in_hw_ctx = in_hardware_context;
	if (in_hardware_context)
		atomic_fetch_add(&hw_ctx_endios, 1);
}

/* --- list_head ------------------------------------------------------------ */

struct list_head {
	struct list_head *next, *prev;
};

static void list_init_head(struct list_head *l)
{
	l->next = l->prev = l;
}

#define INIT_LIST_HEAD(l)		list_init_head(l)

static void list_add_tail(struct list_head *n, struct list_head *head)
{
	n->prev = head->prev;
	n->next = head;
	head->prev->next = n;
	head->prev = n;
}

static void list_del(struct list_head *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n->prev = NULL;
}

#define list_empty(head)	((head)->next == (head))

#define list_first_entry(head, type, member)				\
	list_entry((head)->next, type, member)

/* Same relocation the kernel does, down to the pointer fixups: the entries
 * have to end up pointing at @head, not at the list they were spliced out of,
 * or the receiving head is not a usable iteration sentinel. */
static void list_splice_init(struct list_head *list, struct list_head *head)
{
	struct list_head *first, *last, *at;

	if (list_empty(list))
		return;

	first = list->next;
	last = list->prev;
	at = head;

	first->prev = at;
	last->next = at->next;
	at->next->prev = last;
	at->next = first;

	INIT_LIST_HEAD(list);
}

/* The engine's node is the first member of both embedded structs, but do it
 * properly anyway so the harness cannot accidentally rely on that. */
#define list_entry(ptr, type, member)	container_of(ptr, type, member)

#define list_for_each_entry_safe(pos, tmp, head, member)			\
	for ((pos) = list_entry((head)->next, __typeof__(*(pos)), member),	\
	     (tmp) = list_entry((pos)->member.next, __typeof__(*(tmp)), member); \
	     &(pos)->member != (head);					\
	     (pos) = (tmp),						\
	     (tmp) = list_entry((tmp)->member.next, __typeof__(*(tmp)), member))

/* --- kthread_worker ------------------------------------------------------- */

/* Declared here because the bounded kthread_flush_worker() below reports
 * into the same counter the CHECK macro uses. */
static int failures;

struct kthread_worker {
	struct list_head	work_list;
	pthread_mutex_t		lock;
	pthread_cond_t		cond;
	/*
	 * kthread_should_stop() is a per-task flag in the kernel, read by the
	 * kthread itself with no lock and set by kthread_stop() through the
	 * scheduler.  A plain int read outside the lock is exactly right for
	 * the semantics and exactly wrong for ThreadSanitizer, so it is a
	 * relaxed atomic: the value is only ever a "stop soon" hint that the
	 * caller re-checks after every sleep, never a handoff of data.
	 */
	atomic_int		should_stop;
};

struct kthread_work {
	struct list_head	node;
	void			(*func)(struct kthread_work *);
	int			queued;
};

/*
 * Per-thread, exactly like the kernel's `current`: kthread_should_stop() has
 * to answer for the task doing the asking, not for whichever thread last
 * touched this.  A single global here would let the completion worker
 * answer the rebuild thread's stop check, and the rebuild thread would never
 * observe kthread_stop().
 */
static _Thread_local struct kthread_worker *current_worker;

static void kthread_init_work(struct kthread_work *w,
			      void (*fn)(struct kthread_work *))
{
	list_init_head(&w->node);
	w->func = fn;
	w->queued = 0;
}

static void kthread_init_worker(struct kthread_worker *w)
{
	list_init_head(&w->work_list);
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->cond, NULL);
	atomic_store(&w->should_stop, 0);
}

static inline int kthread_should_stop(void)
{
	return current_worker ? atomic_load(&current_worker->should_stop) : 0;
}

static bool kthread_queue_work(struct kthread_worker *w, struct kthread_work *work)
{
	bool queued = true;

	pthread_mutex_lock(&w->lock);
	if (work->queued) {
		queued = false;
	} else {
		work->queued = 1;
		list_add_tail(&work->node, &w->work_list);
		pthread_cond_broadcast(&w->cond);
	}
	pthread_mutex_unlock(&w->lock);
	return queued;
}

/*
 * Work items that have been taken off the queue but whose callback has not
 * returned.  Counted inside shim_take_one(), under the worker's lock, so the
 * queue and this counter can be sampled together: an item is either still on
 * the list or already counted here, never in limbo between the two.
 */
static atomic_int shim_work_active;

/* Caller holds w->lock. */
static struct kthread_work *shim_take_one(struct kthread_worker *w)
{
	struct kthread_work *wk;

	if (list_empty(&w->work_list))
		return NULL;

	wk = list_entry(w->work_list.next, struct kthread_work, node);
	list_del(&wk->node);
	wk->queued = 0;
	atomic_fetch_add(&shim_work_active, 1);
	return wk;
}

static void shim_run_work(struct kthread_work *wk)
{
	wk->func(wk);
	atomic_fetch_sub(&shim_work_active, 1);
}

/*
 * Mirrors kthread_worker_fn(): drain what is queued, sleep when idle, and
 * on kthread_stop() finish the queue before returning.  The engine relies on
 * that last part — it must drain contexts before stopping the worker.
 */
static int kthread_worker_fn(void *data)
{
	struct kthread_worker *w = data;
	struct kthread_work *wk;

	current_worker = w;
	for (;;) {
		pthread_mutex_lock(&w->lock);
		wk = shim_take_one(w);
		if (!wk && atomic_load(&w->should_stop))
			break;
		if (!wk) {
			pthread_cond_wait(&w->cond, &w->lock);
			pthread_mutex_unlock(&w->lock);
			continue;
		}
		pthread_mutex_unlock(&w->lock);

		shim_run_work(wk);
	}
	pthread_mutex_unlock(&w->lock);

	current_worker = NULL;
	return 0;
}

/* Defined after the engine is included. */
static void shim_usleep(long usec);

/*
 * The engine's real worker thread runs concurrently, so a work item can be
 * off the list and still inside its callback — waiting on the list alone
 * would return early.  Wait until the queue is empty *and* nothing is running,
 * sampling both under the worker's lock so no item can slip through the gap.
 * Use the worker purely as a pump to help it drain.
 *
 * Note this deliberately does not wait on the engine's context count: an
 * aborted context legitimately outlives its own endio while its members are
 * still entitled to report, so "no work left" is the right condition here and
 * "no contexts left" is not.
 *
 * Bounded, so an engine bug fails the run instead of wedging it.
 */
static void kthread_flush_worker(struct kthread_worker *w)
{
	long spins = 0;

	for (;;) {
		int pumped = 0;

		pthread_mutex_lock(&w->lock);
		while (!list_empty(&w->work_list)) {
			struct kthread_work *wk = shim_take_one(w);

			pthread_mutex_unlock(&w->lock);
			shim_run_work(wk);
			pthread_mutex_lock(&w->lock);
			pumped++;
		}

		if (!pumped && !atomic_load(&shim_work_active)) {
			pthread_mutex_unlock(&w->lock);
			break;
		}
		pthread_mutex_unlock(&w->lock);

		shim_usleep(200);
		if (++spins > 20000) {		/* ~4 s */
			fprintf(stderr, "  FAIL: flush stuck, %d work item(s) still running\n",
				atomic_load(&shim_work_active));
			failures++;
			return;
		}
	}
}

static void shim_usleep(long usec)
{
	struct timespec ts = { usec / 1000000, (usec % 1000000) * 1000 };

	nanosleep(&ts, NULL);
}

struct task_struct {
	pthread_t		thread;
	struct kthread_worker	*worker;
	bool			owns_worker;	/* @worker was calloc'd here */
};

/*
 * Every kthread gets a small control block so kthread_should_stop() works
 * for both thread kinds: kthread_worker_fn() owns its worker's lock, and
 * the rebuild kthread has no queue of its own but still needs a stop flag
 * that kthread_stop() can latch.
 */
struct shim_thread_args {
	int			(*fn)(void *);
	void			*arg;
	struct kthread_worker	*ctrl;
};

static void *shim_thread_entry(void *raw)
{
	struct shim_thread_args *a = raw;
	int (*fn)(void *) = a->fn;
	void *arg = a->arg;

	current_worker = a->ctrl;
	free(a);
	return (void *)(long)fn(arg);
}

static struct task_struct *kthread_run(int (*fn)(void *), void *arg,
					const char *namefmt, ...)
{
	struct task_struct *t = calloc(1, sizeof(*t));
	struct shim_thread_args *a = calloc(1, sizeof(*a));

	(void)namefmt;

	/*
	 * kthread_worker_fn() is handed the worker it drains, so that
	 * worker IS its stop flag — adopt it rather than allocating a
	 * second control block, or kthread_stop() would latch a flag the
	 * thread never reads.  Every other kthread gets a fresh block.
	 */
	if (fn == kthread_worker_fn) {
		t->worker = arg;
	} else {
		t->worker = calloc(1, sizeof(*t->worker));
		list_init_head(&t->worker->work_list);
		pthread_mutex_init(&t->worker->lock, NULL);
		pthread_cond_init(&t->worker->cond, NULL);
		t->owns_worker = true;
	}

	a->fn = fn;
	a->arg = arg;
	a->ctrl = t->worker;
	pthread_create(&t->thread, NULL, shim_thread_entry, a);
	return t;
}

/*
 * Release the control block.  LeakSanitizer runs as part of the ASan build,
 * and a leaked task_struct per thread would bury a real engine leak in noise.
 * The worker itself is only freed when kthread_run() allocated it: the
 * completion worker is embedded in struct rc_async_engine and belongs to it.
 */
static int kthread_stop(struct task_struct *t)
{
	if (t->worker) {
		pthread_mutex_lock(&t->worker->lock);
		atomic_store(&t->worker->should_stop, 1);
		pthread_cond_broadcast(&t->worker->cond);
		pthread_mutex_unlock(&t->worker->lock);
	}
	pthread_join(t->thread, NULL);

	if (t->owns_worker) {
		pthread_mutex_destroy(&t->worker->lock);
		pthread_cond_destroy(&t->worker->cond);
		free(t->worker);
	}
	free(t);
	return 0;
}

/* --- waitqueue ------------------------------------------------------------ */

typedef struct {
	pthread_mutex_t		lock;
	pthread_cond_t		cond;
} wait_queue_head_t;

static void init_waitqueue_head(wait_queue_head_t *wq)
{
	pthread_mutex_init(&wq->lock, NULL);
	pthread_cond_init(&wq->cond, NULL);
}

static void wake_up(wait_queue_head_t *wq)
{
	pthread_mutex_lock(&wq->lock);
	pthread_cond_broadcast(&wq->cond);
	pthread_mutex_unlock(&wq->lock);
}

#define set_current_state(s)		do { } while (0)
#define __set_current_state(s)		do { } while (0)
#define TASK_IDLE			0

/*
 * The engine only uses the simple single-condition forms of these macros
 * (nothing else wakes drain_wq/throttle_wq mid-wait except a member
 * completion, which the test triggers explicitly), so a mutex-guarded poll
 * is a faithful stand-in.
 *
 * The condition is re-evaluated on every pass, exactly as the kernel's
 * wait_event() does — a single evaluation would let the engine spin on a
 * stale snapshot.  Every wait is bounded: the timeout forms expire after
 * the interval they were given (at a 1 ms poll), and the untimed form after
 * SHIM_WAIT_ROUNDS, so a real bug shows up as a failure rather than a hang.
 *
 * An expiry on a *timed* wait is normal — the rebuild's throttle and idle
 * polls are meant to time out — so those are only counted.  An expiry on an
 * untimed wait means the engine is waiting for something that will never
 * happen, which is always a bug, so that one is reported.
 */
#define SHIM_WAIT_ROUNDS	5000		/* ~5 s at the 1 ms poll below */
#define SHIM_WAIT_POLL_US	1000

/*
 * Per-thread, for the same reason current_worker is: the engine's wait loops
 * sample the budget of the thread that is actually waiting, and a shared
 * counter would have the rebuild thread's budget reset by the main thread's
 * wait — which is both a wrong answer and a data race.
 */
static _Thread_local int shim_wait_rounds;
static long shim_wait_timeouts;

static void shim_wait_once(wait_queue_head_t *wq)
{
	struct timespec ts;

	pthread_mutex_lock(&wq->lock);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += SHIM_WAIT_POLL_US * 1000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&wq->cond, &wq->lock, &ts);
	pthread_mutex_unlock(&wq->lock);
	shim_wait_rounds++;
}

/* Returns nonzero once the condition held, zero if the budget ran out. */
#define shim_wait_loop(__wq, c, budget, timed)				\
	({								\
		int __hit = 0;						\
									\
		shim_wait_rounds = 0;					\
		for (;;) {						\
			if (c) {					\
				__hit = 1;				\
				break;					\
			}						\
			if (shim_wait_rounds >= (budget))		\
				break;					\
			shim_wait_once(__wq);				\
		}							\
		if (!__hit && !(timed)) {				\
			/* An untimed wait that never completes is	\
			 * always an engine bug; a timed one expiring is	\
			 * ordinary, and test_unload_stuck drives two on	\
			 * purpose.  Only the former fails the run. */	\
			fprintf(stderr,					\
				"  FAIL: wait never satisfied (%s)\n",	\
				#c);					\
			failures++;					\
		}							\
		if (!__hit)						\
			shim_wait_timeouts++;				\
		__hit;							\
	})

#define wait_event(wq, c)						\
	({								\
		shim_wait_loop(&(wq), c, SHIM_WAIT_ROUNDS, 0);		\
		0;							\
	})

/* @to is in msecs_to_jiffies() units, which the shim maps to milliseconds,
 * so it can be used directly as the round budget at a 1 ms poll. */
#define wait_event_timeout(wq, c, to)					\
	({								\
		long __budget = (long)(to);				\
									\
		shim_wait_loop(&(wq), c, __budget > 0 ? __budget : 1, 1); \
	})

#define wait_event_interruptible(wq, c)					\
	({								\
		shim_wait_loop(&(wq), c, SHIM_WAIT_ROUNDS, 0);		\
		0;							\
	})

#define wait_event_interruptible_timeout(wq, c, to)			\
	({								\
		long __budget = (long)(to);				\
									\
		shim_wait_loop(&(wq), c, __budget > 0 ? __budget : 1, 1); \
		0;							\
	})

/* The engine calls this for RAID 10 rebuild routing. */
static u64 rc_amd_map_nested_raid10(u64 sector_lba, u32 chunk_sectors,
				    int *target_member, int num_drives)
{
	uint64_t row = sector_lba / chunk_sectors;
	uint32_t off = sector_lba % chunk_sectors;
	int spans = num_drives / 2;

	row /= spans;
	*target_member = (int)(row % spans) * 2;
	return row * chunk_sectors + off;
}

/* =========================================================================
 * THE ENGINE UNDER TEST — included verbatim, not reimplemented.
 * ========================================================================= */

#include "patch_async_worker.c"

/* =========================================================================
 * TESTS
 * ========================================================================= */

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			failures++;					\
			fprintf(stderr, "  FAIL (line %d): ", __LINE__); \
			fprintf(stderr, __VA_ARGS__);			\
			fprintf(stderr, "\n");				\
		}							\
	} while (0)

#define MAX_ISSUES	300000

struct issue {
	struct bio			*bio;
	struct rc_mirror_context	*ctx;
};

static struct issue issues[MAX_ISSUES];
static atomic_int issue_count;

static struct bio *new_bio(void)
{
	struct bio *b = calloc(1, sizeof(*b));

	assert(b);
	return b;
}

/* Dispatch a parent and remember the context handle the engine returned. */
static struct issue *dispatch(struct bio *bio, int mirrors, int flag)
{
	int slot = atomic_fetch_add(&issue_count, 1);

	if (slot >= MAX_ISSUES) {
		fprintf(stderr, "issue table full\n");
		exit(1);
	}
	issues[slot].bio = bio;
	issues[slot].ctx = rc_amd_queue_async_completion(bio, mirrors, flag);
	return &issues[slot];
}

static void reap_all(void)
{
	kthread_flush_worker(&rc_amd_engine.completion_worker);
}

/*
 * Give a parent bio back.  Only legal once every report it owed has been
 * delivered and the completion queue has drained — the engine owns the bio
 * from rc_amd_queue_async_completion() until it issues the endio, exactly as
 * the block layer owns one until bio_endio().  Clearing the table entry
 * matters too: the storm test sweeps the whole table, and a stale pointer
 * left behind here would turn the harness's own use-after-free into a
 * confusing heap-corruption report.
 */
static void release(struct issue *it)
{
	free(it->bio);
	it->bio = NULL;
	it->ctx = NULL;
}

/* ---- 1: status merge ranking -------------------------------------------- */

static void test_status_merge(void)
{
	static const struct {
		blk_status_t a, b, want;
	} cases[] = {
		{ BLK_STS_OK,			BLK_STS_OK,			BLK_STS_OK },
		{ BLK_STS_OK,			BLK_STS_IOERR,			BLK_STS_IOERR },
		{ BLK_STS_IOERR,		BLK_STS_OK,			BLK_STS_IOERR },
		{ BLK_STS_IOERR,		BLK_STS_RESOURCE,		BLK_STS_IOERR },
		{ BLK_STS_RESOURCE,		BLK_STS_IOERR,			BLK_STS_IOERR },
		{ BLK_STS_RESOURCE,		BLK_STS_ZONE_ACTIVE_RESOURCE,
		  BLK_STS_ZONE_ACTIVE_RESOURCE },
		{ BLK_STS_NOTSUPP,		BLK_STS_TARGET,		BLK_STS_TARGET },
		{ BLK_STS_NOSPC,		BLK_STS_RESV_CONFLICT,		BLK_STS_RESV_CONFLICT },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK(rc_status_merge(cases[i].a, cases[i].b) == cases[i].want,
		      "merge(%u, %u) = %u, want %u", cases[i].a, cases[i].b,
		      rc_status_merge(cases[i].a, cases[i].b), cases[i].want);
}

/* ---- 2: dispatch / report round trip ------------------------------------ */

static void test_basic_flow(void)
{
	struct bio *bio = new_bio();
	struct issue *it = dispatch(bio, 2, 0);

	CHECK(rc_amd_inflight_contexts() == 1, "inflight = %d, want 1",
	      rc_amd_inflight_contexts());
	CHECK(bio->endio_calls == 0, "parent ended at dispatch time");
	CHECK(it->ctx != NULL, "dispatch did not return a context handle");

	/* One of two mirrors: must NOT end the parent yet. */
	in_hardware_context = 1;
	rc_amd_mirror_complete(it->ctx, BLK_STS_OK);
	in_hardware_context = 0;
	CHECK(bio->endio_calls == 0, "parent ended after 1 of 2 members");

	/* Last mirror: endio is queued, not issued inline. */
	in_hardware_context = 1;
	rc_amd_mirror_complete(it->ctx, BLK_STS_OK);
	in_hardware_context = 0;
	CHECK(bio->endio_calls == 0, "parent ended inline from the member context");
	CHECK(rc_amd_inflight_contexts() == 1, "context released before endio ran");

	kthread_flush_worker(&rc_amd_engine.completion_worker);

	CHECK(bio->endio_calls == 1, "parent ended %d times, want 1",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_OK, "status %u, want OK", bio->bi_status);
	CHECK(bio->endio_in_hw_ctx == 0, "parent ended from a hardware context");
	CHECK(rc_amd_inflight_contexts() == 0, "inflight = %d, want 0",
	      rc_amd_inflight_contexts());

	release(it);
}

/* ---- 3: worst-status-wins across members ------------------------------- */

static void test_error_aggregation(void)
{
	struct bio *bio = new_bio();
	struct issue *it = dispatch(bio, 3, 0);
	struct rc_async_stats s;
	int before_ok, before_err;

	rc_amd_get_async_stats(&s);
	before_ok = (int)atomic64_read(&s.mirror_errors);
	before_err = before_ok;

	/* A retryable status then a terminal one: terminal must win. */
	rc_amd_mirror_complete_member(it->ctx, 0, BLK_STS_RESOURCE);
	rc_amd_mirror_complete_member(it->ctx, 1, BLK_STS_IOERR);
	rc_amd_mirror_complete_member(it->ctx, 2, BLK_STS_OK);
	kthread_flush_worker(&rc_amd_engine.completion_worker);

	CHECK(bio->endio_calls == 1, "ended %d times, want 1", bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_IOERR,
	      "aggregate status %u, want IOERR (terminal must beat retryable)",
	      bio->bi_status);

	/* All-OK must stay OK, not be polluted by the resource counter. */
	{
		struct bio *ok_bio = new_bio();
		struct issue *ok = dispatch(ok_bio, 2, 0);

		rc_amd_mirror_complete_member(ok->ctx, 0, BLK_STS_OK);
		rc_amd_mirror_complete_member(ok->ctx, 1, BLK_STS_OK);
		kthread_flush_worker(&rc_amd_engine.completion_worker);
		CHECK(ok_bio->bi_status == BLK_STS_OK,
		      "clean set ended %u, want OK", ok_bio->bi_status);
		release(ok);
	}

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.mirror_errors) == before_err + 1,
	      "mirror_errors = %lld, want %d",
	      atomic64_read(&s.mirror_errors), before_err + 1);
	release(it);
}

/* ---- 4: fault isolation ------------------------------------------------- */

static void test_alloc_fault(void)
{
	struct bio *bio = new_bio();
	struct rc_async_stats before, after;
	int inflight_before;

	rc_amd_get_async_stats(&before);
	inflight_before = rc_amd_inflight_contexts();

	atomic_store(&shim_alloc_fail, 1);
	CHECK(rc_amd_queue_async_completion(bio, 2, 0) == NULL,
	      "faulted dispatch should return NULL");

	CHECK(bio->endio_calls == 1, "faulted dispatch ended parent %d times",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_RESOURCE,
	      "faulted dispatch status %u, want BLK_STS_RESOURCE",
	      bio->bi_status);
	CHECK(rc_amd_inflight_contexts() == inflight_before,
	      "faulted dispatch registered a context");

	rc_amd_get_async_stats(&after);
	CHECK(atomic64_read(&after.faults) == atomic64_read(&before.faults) + 1,
	      "fault counter did not advance");

	free(bio);	/* ended inline on the fault path: we already own it */
}

/* ---- 5: abort + late reports ------------------------------------------- */

/*
 * An aborted set has already ended its parent but its members are still in
 * flight and still entitled to report.  The context must survive them — that
 * is the whole point of this test — and the last straggler must release it.
 * Exactly the three promised reports are delivered; a fourth would be a
 * caller contract violation with no handle left to answer it.
 */
static void test_abort(void)
{
	struct bio *bio = new_bio();
	struct issue *it = dispatch(bio, 3, 0);
	struct rc_async_stats s;
	int stale_before;

	rc_amd_get_async_stats(&s);
	stale_before = (int)atomic64_read(&s.stale_reports);

	CHECK(rc_amd_mirror_abort(it->ctx, BLK_STS_IOERR) == 0,
	      "abort failed to claim ownership");
	CHECK(rc_amd_mirror_abort(it->ctx, BLK_STS_IOERR) == -EALREADY,
	      "second abort should be -EALREADY");

	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(bio->endio_calls == 1, "abort ended parent %d times, want 1",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_IOERR, "abort status %u, want IOERR",
	      bio->bi_status);

	/* The parent is gone, but the context must outlive it while members
	 * are still owed — this is where freeing on endio alone would be a
	 * use-after-free on the very next line. */
	CHECK(rc_amd_inflight_contexts() == 1,
	      "aborted context was freed with %d member(s) still owed a report",
	      3);

	in_hardware_context = 1;
	CHECK(rc_amd_mirror_complete_member(it->ctx, 0, BLK_STS_OK) == -EALREADY,
	      "late report after abort should be rejected");
	CHECK(rc_amd_inflight_contexts() == 1,
	      "context released before its last promised report");
	CHECK(rc_amd_mirror_complete_member(it->ctx, 1, BLK_STS_OK) == -EALREADY,
	      "second late report should be rejected");
	CHECK(rc_amd_inflight_contexts() == 1,
	      "context released before its last promised report");
	CHECK(rc_amd_mirror_complete_member(it->ctx, 2, BLK_STS_IOERR) == -EALREADY,
	      "last late report should be rejected");
	in_hardware_context = 0;

	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(bio->endio_calls == 1, "late reports re-ended parent (%d calls)",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_IOERR,
	      "late reports changed the status to %u", bio->bi_status);
	CHECK(rc_amd_inflight_contexts() == 0,
	      "the last straggler did not release the context");

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.stale_reports) == stale_before + 3,
	      "stale_reports = %lld, want %d (3 late reports)",
	      atomic64_read(&s.stale_reports), stale_before + 3);

	release(it);
}

/* ---- 5b: duplicate report from a buggy member path ---------------------- */

/*
 * A member bio that completes twice.
 *
 * The engine cannot detect this: a context handle carries no member identity,
 * so a second report for member 0 is indistinguishable from the report still
 * owed for member 1.  What it must guarantee instead is that the parent still
 * ends exactly once and the context is still accounted for.  The duplicate
 * consumes one of the outstanding slots, which is not detectable either way
 * and does not matter: the engine promised to end the parent after a fixed
 * number of reports, and that is what it does.
 *
 * The second half is the case that used to be fatal: a report arriving after
 * the context has been retired.  The tombstone pool keeps that memory mapped,
 * so the over-report is counted and dropped instead of reading freed memory.
 */
static void test_duplicate_report(void)
{
	struct bio *bio = new_bio();
	struct issue *it = dispatch(bio, 3, 0);
	struct rc_async_stats s;
	int stale_before;

	rc_amd_get_async_stats(&s);
	stale_before = (int)atomic64_read(&s.stale_reports);

	/* Three promises, three reports — but member 0 reports twice. */
	CHECK(rc_amd_mirror_complete_member(it->ctx, 0, BLK_STS_OK) == 0,
	      "first report rejected");
	CHECK(rc_amd_mirror_complete_member(it->ctx, 0, BLK_STS_OK) == 0,
	      "duplicate rejected: it is indistinguishable from the report "
	      "still owed for another member");
	CHECK(rc_amd_inflight_contexts() == 1,
	      "a duplicate consumed the context instead of a slot");

	/* Two reports in, one still owed: the parent must not have ended. */
	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(bio->endio_calls == 0,
	      "parent ended on %d call(s) with a member still owed",
	      bio->endio_calls);

	CHECK(rc_amd_mirror_complete_member(it->ctx, 1, BLK_STS_OK) == 0,
	      "third report rejected");

	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(bio->endio_calls == 1, "parent ended %d times, want 1",
	      bio->endio_calls);
	CHECK(rc_amd_inflight_contexts() == 0, "context never retired");

	/* Now the genuinely over-reported case: the context is a tombstone. */
	CHECK(rc_amd_mirror_complete_member(it->ctx, 2, BLK_STS_OK) == -EALREADY,
	      "report after retirement should be -EALREADY");
	CHECK(rc_amd_mirror_complete(it->ctx, BLK_STS_OK) == -EALREADY,
	      "report after retirement should be -EALREADY");

	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(bio->endio_calls == 1, "over-report re-ended parent (%d calls)",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_OK,
	      "over-report changed the final status to %u", bio->bi_status);
	CHECK(rc_amd_inflight_contexts() == 0, "over-report re-registered ctx");

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.stale_reports) == stale_before + 2,
	      "stale_reports = %lld, want %d (2 post-retirement reports)",
	      atomic64_read(&s.stale_reports), stale_before + 2);

	release(it);
}

/* ---- 6: bad arguments -------------------------------------------------- */

static void test_bad_args(void)
{
	struct bio *bio = new_bio();

	CHECK(rc_amd_queue_async_completion(NULL, 2, 0) == NULL,
	      "NULL bio should return NULL");
	CHECK(rc_amd_queue_async_completion(bio, 0, 0) == NULL,
	      "zero-mirror dispatch should return NULL");
	CHECK(bio->endio_calls == 1, "zero-mirror dispatch ended %d times",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_IOERR, "zero-mirror status %u, want IOERR",
	      bio->bi_status);

	CHECK(rc_amd_mirror_complete(NULL, BLK_STS_OK) == -EINVAL,
	      "NULL context should be -EINVAL");
	CHECK(rc_amd_mirror_complete_member(NULL, 0, BLK_STS_OK) == -EINVAL,
	      "NULL context should be -EINVAL");
	CHECK(rc_amd_mirror_abort(NULL, BLK_STS_OK) == -EINVAL,
	      "NULL context should be -EINVAL");

	CHECK(rc_amd_fail_member(-1) == -EINVAL, "fail_member(-1) should be -EINVAL");
	CHECK(rc_amd_fail_member(9999) == -EINVAL, "fail_member(9999) should be -EINVAL");

	free(bio);	/* rejected inline: we already own it */
}

/* ---- 7: concurrent storm ----------------------------------------------- */

#define STORM_THREADS		8
#ifndef STORM_PER_THREAD
#define STORM_PER_THREAD	3000
#endif

static void *storm_thread(void *arg)
{
	int id = (int)(long)arg;
	int i;

	for (i = 0; i < STORM_PER_THREAD; i++) {
		int mirrors = 2 + ((id + i) % 3);
		struct bio *bio;
		struct issue *it;
		int slot, m;

		slot = atomic_load(&issue_count);
		if (slot >= MAX_ISSUES - 8)
			break;

		bio = new_bio();

		/* Sprinkle in allocation failures so the fault-isolation
		 * path runs concurrently with the normal one. */
		if ((i % 97) == 0)
			atomic_store(&shim_alloc_fail, 1);

		/*
		 * The bio is deliberately NOT freed here: the last report only
		 * *queues* the parent endio, so ownership does not come back
		 * until the completion worker has run.  Freeing it inline — what
		 * a caller returning straight to the block layer would
		 * effectively be doing — is exactly the use-after-free this
		 * harness exists to make impossible in the engine, and here it
		 * would simply be the harness's own bug.  test_concurrent_storm()
		 * frees them all once the queue is drained.
		 */
		it = dispatch(bio, mirrors, 0);
		if (bio->endio_calls)
			continue;	/* fault-isolated: no reports owed */

		in_hardware_context = 1;
		for (m = 0; m < mirrors; m++) {
			blk_status_t st;

			switch ((i * 7 + m * 3 + id) % 13) {
			case 0:  st = BLK_STS_IOERR; break;
			case 1:  st = BLK_STS_RESOURCE; break;
			case 2:  st = BLK_STS_TIMEOUT; break;
			case 3:  st = BLK_STS_OFFLINE; break;
			default: st = BLK_STS_OK; break;
			}
			rc_amd_mirror_complete_member(it->ctx, (m + id) % mirrors, st);
		}
		in_hardware_context = 0;
		(void)slot;
	}
	return NULL;
}

static void test_concurrent_storm(void)
{
	pthread_t th[STORM_THREADS];
	struct rc_async_stats s, before;
	int i, n, first;

	rc_amd_get_async_stats(&before);
	first = atomic_load(&issue_count);

	for (i = 0; i < STORM_THREADS; i++)
		pthread_create(&th[i], NULL, storm_thread, (void *)(long)i);
	for (i = 0; i < STORM_THREADS; i++)
		pthread_join(th[i], NULL);

	reap_all();

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.dispatched) == atomic64_read(&s.completed),
	      "dispatched %lld != completed %lld (a context leaked or was never ended)",
	      atomic64_read(&s.dispatched), atomic64_read(&s.completed));
	/* The counter is cumulative — test_abort deliberately feeds it — so
	 * what matters is that the storm added none. */
	CHECK(atomic64_read(&s.stale_reports) ==
	      atomic64_read(&before.stale_reports),
	      "%lld stale report(s) during the storm (%lld before)",
	      atomic64_read(&s.stale_reports) -
		      atomic64_read(&before.stale_reports),
	      atomic64_read(&before.stale_reports));

	/* Only now that every endio has been issued does a bio belong to us. */
	n = atomic_load(&issue_count);
	for (i = first; i < n; i++) {
		CHECK(issues[i].bio->endio_calls == 1,
		      "storm issue %d ended %d times, want 1",
		      i, issues[i].bio->endio_calls);
		release(&issues[i]);
	}

	printf("       storm: %lld dispatched, %lld completed (%lld ok, "
	       "%lld errors, %lld resource-injected, %lld alloc faults)\n",
	       atomic64_read(&s.dispatched), atomic64_read(&s.completed),
	       atomic64_read(&s.mirror_ok), atomic64_read(&s.mirror_errors),
	       atomic64_read(&s.resource_injections),
	       atomic64_read(&s.faults));
}

/* ---- 8: rebuild pump ---------------------------------------------------- */

/*
 * A stand-in media layer with the same shape as the real one: the rebuild
 * thread only *issues* a copy through the chunk hook, and a separate
 * completion path retires it and kicks the throttle wait.  Modelling that
 * split is the whole point — an inline "issue and immediately retire" stub
 * would never let the rebuild outrun its in-flight window, which is the one
 * thing rc_amd_resync_throttle() exists to prevent.
 */
static atomic_llong chunk_issued;
static atomic_llong chunk_retired;
static atomic_llong chunk_bytes;
static atomic_int	chunk_inflight;		/* issued - retired */
static atomic_int	chunk_max_inflight;
static atomic_int	chunk_seen_fail_slot;
static atomic_int	chunk_retire_stop;

#define CHUNK_WINDOW	4

static int harness_chunk(void *cookie, int source, int target, u64 sector,
			 u32 nr)
{
	int now, seen;

	(void)cookie;
	(void)source;
	(void)sector;

	if (target < 0)
		atomic_store(&chunk_seen_fail_slot, 1);

	now = atomic_fetch_add(&chunk_inflight, 1) + 1;
	atomic_fetch_add(&chunk_issued, 1);
	atomic_fetch_add(&chunk_bytes, (long long)nr << SECTOR_SHIFT);

	for (;;) {
		seen = atomic_load(&chunk_max_inflight);
		if (now <= seen || atomic_compare_exchange_weak(&chunk_max_inflight,
								&seen, now))
			break;
	}
	return 0;
}

static u32 harness_outstanding(void *cookie)
{
	(void)cookie;
	return (u32)atomic_load(&chunk_inflight);
}

/* Stands in for the member's completion path.  Deliberately slower than the
 * rebuild so the throttle window actually fills and the wait is exercised. */
static void *harness_retire_thread(void *arg)
{
	(void)arg;

	while (!atomic_load(&chunk_retire_stop)) {
		if (atomic_load(&chunk_inflight) > 0) {
			atomic_fetch_sub(&chunk_inflight, 1);
			atomic_fetch_add(&chunk_retired, 1);
			/* Exactly what the real media layer owes the pump. */
			rc_amd_resync_kick();
		}
		shim_usleep(500);
	}
	return NULL;
}

/* Bring the fake device up from a clean slate and take it down again, so
 * each rebuild test starts and ends with an empty in-flight window. */
static void media_start(pthread_t *thread)
{
	atomic_store(&chunk_retire_stop, 0);
	atomic_store(&chunk_inflight, 0);
	pthread_create(thread, NULL, harness_retire_thread, NULL);
}

static void media_stop(pthread_t *thread)
{
	atomic_store(&chunk_retire_stop, 1);
	rc_amd_resync_kick();
	pthread_join(*thread, NULL);
}

static void test_resync(void)
{
	struct rc_resync_progress p;
	pthread_t retire;
	long long issued, retired;
	int i, rc;

	rc_amd_set_resync_io(harness_chunk, harness_outstanding, NULL,
			     CHUNK_WINDOW);
	rc_amd_set_resync_geometry(4, 256);
	media_start(&retire);

	rc = rc_amd_resync_request(0, 64 * 1024, 256, -1, -1);
	CHECK(rc == 0, "resync request rejected: %d", rc);

	/* A second request while one is live must be refused, not queued. */
	CHECK(rc_amd_resync_request(0, 64 * 1024, 256, -1, -1) == -EBUSY,
	      "concurrent resync request should be -EBUSY");

	for (i = 0; i < 10000; i++) {
		rc_amd_get_resync_progress(&p);
		if (p.state == RC_RESYNC_DONE || p.state == RC_RESYNC_FAILED)
			break;
		shim_usleep(1000);
	}

	CHECK(p.state == RC_RESYNC_DONE, "resync ended in state %d (result %d)",
	      p.state, p.result);
	CHECK(atomic_load(&chunk_issued) == 256,
	      "issued %lld chunks, want 256 (64K sectors / 256-sector chunks)",
	      atomic_load(&chunk_issued));
	CHECK(atomic_load(&chunk_bytes) == (64 * 1024) << SECTOR_SHIFT,
	      "copied %lld bytes, want %d",
	      atomic_load(&chunk_bytes), (64 * 1024) << SECTOR_SHIFT);
	CHECK(atomic_load(&chunk_seen_fail_slot) == 0,
	      "a chunk was routed to a member slot < 0");
	CHECK(p.cursor == p.end_sector, "cursor %llu, want end %llu",
	      (unsigned long long)p.cursor, (unsigned long long)p.end_sector);

	/* The throttle must have bound the in-flight window — and must have
	 * been the thing that bound it, not a rebuild that simply never ran
	 * ahead.  Both halves matter: the first without the second would be a
	 * rebuild that ignored its window, the second without the first a
	 * throttle that never engaged. */
	CHECK(atomic_load(&chunk_max_inflight) == CHUNK_WINDOW,
	      "peak in-flight copies %d, want exactly the %d-chunk window",
	      atomic_load(&chunk_max_inflight), CHUNK_WINDOW);

	/* Drain the fake device, then shut it down. */
	for (i = 0; i < 10000; i++) {
		issued = atomic_load(&chunk_issued);
		retired = atomic_load(&chunk_retired);
		if (issued == retired)
			break;
		shim_usleep(1000);
	}
	CHECK(atomic_load(&chunk_issued) == atomic_load(&chunk_retired),
	      "%lld chunk(s) never retired",
	      atomic_load(&chunk_issued) - atomic_load(&chunk_retired));

	media_stop(&retire);

	printf("       resync: state=%d chunks=%llu bytes=%llu target=%d "
	       "peak in flight %d/%d\n",
	       p.state, (unsigned long long)p.chunks,
	       (unsigned long long)p.bytes, p.target_member,
	       atomic_load(&chunk_max_inflight), CHUNK_WINDOW);

	rc_amd_set_resync_io(NULL, NULL, NULL, 0);
	rc_amd_set_resync_geometry(0, 0);
}

/*
 * The operator brake.  Two shapes matter: cancelling a rebuild that is
 * already running, and cancelling one that is only queued.  In both cases
 * the engine must end up re-armed — a cancel that permanently refuses
 * further rebuilds would strand a degraded array with no way to retry.
 */
static void test_resync_cancel(void)
{
	struct rc_resync_progress p;
	pthread_t retire;
	long long issued_at_start;
	int i;

	rc_amd_set_resync_io(harness_chunk, harness_outstanding, NULL,
			     CHUNK_WINDOW);
	rc_amd_set_resync_geometry(4, 256);
	media_start(&retire);

	issued_at_start = atomic_load(&chunk_issued);

	CHECK(rc_amd_resync_request(0, 64 * 1024, 256, -1, -1) == 0,
	      "cancel setup: request rejected");

	for (i = 0; i < 10000; i++) {
		rc_amd_get_resync_progress(&p);
		if (p.state == RC_RESYNC_RUNNING)
			break;
		shim_usleep(1000);
	}
	CHECK(p.state == RC_RESYNC_RUNNING, "rebuild never reached RUNNING (state %d)",
	      p.state);

	rc_amd_resync_cancel();

	for (i = 0; i < 10000; i++) {
		rc_amd_get_resync_progress(&p);
		if (p.state == RC_RESYNC_CANCELLED)
			break;
		shim_usleep(1000);
	}
	CHECK(p.state == RC_RESYNC_CANCELLED,
	      "cancelled rebuild ended in state %d (result %d), want CANCELLED",
	      p.state, p.result);
	CHECK(p.result == -ECANCELED, "cancel result %d, want -ECANCELED", p.result);
	CHECK(atomic_load(&chunk_issued) - issued_at_start < 256,
	      "cancel did not stop the walk early: %lld of 256 chunks issued",
	      atomic_load(&chunk_issued) - issued_at_start);
	CHECK(p.cursor < p.end_sector, "cancelled rebuild ran to %llu of %llu",
	      (unsigned long long)p.cursor, (unsigned long long)p.end_sector);

	/* Re-armed: a fresh rebuild must be accepted. */
	CHECK(rc_amd_resync_request(0, 4096, 256, -1, -1) == 0,
	      "engine still refusing rebuilds after a cancel (brake not re-armed)");

	for (i = 0; i < 10000; i++) {
		rc_amd_get_resync_progress(&p);
		if (p.state == RC_RESYNC_DONE || p.state == RC_RESYNC_FAILED)
			break;
		shim_usleep(1000);
	}
	CHECK(p.state == RC_RESYNC_DONE,
	      "second rebuild ended in state %d (result %d), want DONE",
	      p.state, p.result);

	/* A cancel with nothing in flight must not disarm the engine either. */
	rc_amd_resync_cancel();
	CHECK(rc_amd_resync_request(0, 4096, 256, -1, -1) == 0,
	      "an idle cancel left the engine permanently busy");

	for (i = 0; i < 10000; i++) {
		rc_amd_get_resync_progress(&p);
		if (p.state == RC_RESYNC_DONE || p.state == RC_RESYNC_FAILED)
			break;
		shim_usleep(1000);
	}

	/*
	 * Cancel a job the thread has not claimed yet.  Whether the thread
	 * wins that race decides which of the two cancel paths runs, so the
	 * only portable assertion is that the rebuild is not left RUNNING.
	 */
	rc_amd_resync_request(0, 1 << 20, 256, -1, -1);
	rc_amd_resync_cancel();

	media_stop(&retire);

	printf("       cancel: state=%d result=%d chunks=%lld\n",
	       p.state, p.result, atomic_load(&chunk_issued));

	rc_amd_set_resync_io(NULL, NULL, NULL, 0);
	rc_amd_set_resync_geometry(0, 0);
}

static void test_resync_routing(void)
{
	int member = -1;
	u64 mapped;

	/* RAID 10 nested routing: an even member count must resolve to a
	 * valid pair base and a sane mapped address. */
	mapped = rc_amd_map_nested_raid10(12345, 256, &member, 4);
	CHECK(member >= 0 && member < 4, "routed to member %d of 4", member);
	CHECK(mapped < 12345, "nested map moved lba forward (%llu)", mapped);

	/* A geometry the matrix cannot express must be rejected. */
	rc_amd_set_resync_geometry(3, 256);	/* odd: below the minimum */
	rc_amd_get_resync_progress(NULL);
	CHECK(1, "odd geometry rejected without crashing");
}

/* ---- 9: unload drain ---------------------------------------------------- */

static void test_unload_drain(void)
{
	struct bio *bio = new_bio();
	struct issue *it = dispatch(bio, 2, 0);
	struct bio *late;
	struct rc_async_stats s;

	/* One mirror reported, one still in flight. */
	rc_amd_mirror_complete(it->ctx, BLK_STS_OK);
	CHECK(rc_amd_inflight_contexts() == 1, "expected 1 in flight, got %d",
	      rc_amd_inflight_contexts());

	/* Last member reports, then we unload: the drain must find zero and
	 * the worker must release the queued parent before it stops. */
	rc_amd_mirror_complete(it->ctx, BLK_STS_OK);

	rc_amd_exit_async_subsystem();

	CHECK(rc_amd_inflight_contexts() == 0, "drain left %d context(s) behind",
	      rc_amd_inflight_contexts());
	CHECK(bio->endio_calls == 1, "drained parent ended %d times, want 1",
	      bio->endio_calls);
	CHECK(bio->bi_status == BLK_STS_OK, "drained parent status %u, want OK",
	      bio->bi_status);

	/* Dispatch after unload: refused with a retryable status. */
	late = new_bio();
	CHECK(rc_amd_queue_async_completion(late, 2, 0) == NULL,
	      "post-unload dispatch should return NULL");
	CHECK(late->endio_calls == 1, "post-unload dispatch ended %d times",
	      late->endio_calls);
	CHECK(late->bi_status == BLK_STS_RESOURCE,
	      "post-unload dispatch status %u, want BLK_STS_RESOURCE",
	      late->bi_status);

	/* Rebuild request after unload: refused. */
	CHECK(rc_amd_resync_request(0, 1024, 256, -1, -1) == -ENODEV,
	      "post-unload resync should be -ENODEV");

	/* Second exit must be a no-op, not a crash. */
	rc_amd_exit_async_subsystem();

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.drain_forced) == 0,
	      "%lld context(s) had to be force-failed during drain",
	      atomic64_read(&s.drain_forced));

	free(bio);	/* ended inline on the refusal path */
	free(late);	/* ditto */
}

/* ---- 10: unload with a member that never reports ------------------------- */

/*
 * The unhappy unload.  Three shapes of stuck context, all of which must end
 * with zero live contexts and no parent left dangling:
 *
 *   a. a member that simply never reports — nothing has claimed the endio, so
 *      the drain force can answer it with BLK_STS_RESOURCE; it still owes a
 *      report afterwards, so the purge is what finally frees it;
 *   b. an aborted set whose parent is already ended but whose members never
 *      arrive — the force pass cannot claim it (RC_CTX_COMPLETING is latched)
 *      and there is nothing left to end, so only the purge clears it;
 *   c. a set that is merely slow — every member reports and the context
 *      retires on its own, without the drain having to touch it.
 *
 * The interesting part is that (a) and (b) reach the purge by different routes,
 * and that the purge is only reached because the completion queue was flushed
 * first: a context can be registered with its endio already queued and not yet
 * run, and the force pass will not re-queue it.
 *
 * Runs against its own engine instance because it unloads the module.
 */
static void test_unload_stuck(void)
{
	struct bio *a = new_bio();
	struct bio *b = new_bio();
	struct bio *c = new_bio();
	struct issue *ia, *ib, *ic;
	struct rc_async_stats s;

	CHECK(rc_amd_init_async_subsystem() == 0, "re-init failed");

	/* (a) two members, one reports, the other never does. */
	ia = dispatch(a, 2, 0);
	rc_amd_mirror_complete(ia->ctx, BLK_STS_OK);

	/* (b) aborted with both members still in flight. */
	ib = dispatch(b, 2, 0);
	CHECK(rc_amd_mirror_abort(ib->ctx, BLK_STS_IOERR) == 0, "abort failed");

	/* (c) complete but not yet run: the endio is parked on the queue. */
	ic = dispatch(c, 2, 0);
	rc_amd_mirror_complete(ic->ctx, BLK_STS_OK);
	rc_amd_mirror_complete(ic->ctx, BLK_STS_OK);

	/* Run (b)'s and (c)'s completion bodies, so what the drain has to deal
	 * with is exactly the two stuck contexts and one clean retirement. */
	kthread_flush_worker(&rc_amd_engine.completion_worker);
	CHECK(c->endio_calls == 1, "(c) parent ended %d times", c->endio_calls);
	CHECK(rc_amd_inflight_contexts() == 2,
	      "expected the two stuck sets to survive, got %d",
	      rc_amd_inflight_contexts());

	rc_amd_exit_async_subsystem();

	CHECK(rc_amd_inflight_contexts() == 0,
	      "stuck unload left %d context(s) behind",
	      rc_amd_inflight_contexts());

	/* (a) was forced.  Never reported written, so the block layer must be
	 * told to retry rather than handed a silent hole. */
	CHECK(a->endio_calls == 1, "forced parent ended %d times, want 1",
	      a->endio_calls);
	CHECK(a->bi_status == BLK_STS_RESOURCE,
	      "forced parent status %u, want BLK_STS_RESOURCE",
	      a->bi_status);

	/* (b) keeps the status the abort gave it: it was already ended, so the
	 * drain had nothing left to say about it. */
	CHECK(b->endio_calls == 1, "aborted parent ended %d times, want 1",
	      b->endio_calls);
	CHECK(b->bi_status == BLK_STS_IOERR,
	      "aborted parent status %u, want the abort's BLK_STS_IOERR",
	      b->bi_status);

	/* (c) was never touched by the drain. */
	CHECK(c->endio_calls == 1, "clean parent ended %d times, want 1",
	      c->endio_calls);
	CHECK(c->bi_status == BLK_STS_OK, "clean parent status %u, want OK",
	      c->bi_status);

	rc_amd_get_async_stats(&s);
	CHECK(atomic64_read(&s.drain_forced) == 3,
	      "drain_forced = %lld, want 3 (one forced, two purged)",
	      atomic64_read(&s.drain_forced));
	CHECK(atomic64_read(&s.dispatched) == atomic64_read(&s.completed),
	      "dispatched %lld != completed %lld: a parent was never ended",
	      atomic64_read(&s.dispatched), atomic64_read(&s.completed));

	printf("       stuck unload: forced=1 purged=2 clean=1, all parents ended\n");

	release(ia);
	release(ib);
	release(ic);
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
	/* Unbuffered: if a test hangs, the last progress line printed is the
	 * one that identifies where. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* The engine logs on every error path; keep the output readable
	 * unless something actually failed. */
	shim_log_quiet = 1;

	if (rc_amd_init_async_subsystem() != 0) {
		fprintf(stderr, "init failed\n");
		return 1;
	}

	printf("== status merge ranking ==\n");		test_status_merge();
	printf("== dispatch / report round trip ==\n");	test_basic_flow();
	printf("== error aggregation ==\n");		test_error_aggregation();
	printf("== fault isolation ==\n");		test_alloc_fault();
	printf("== abort + late reports ==\n");		test_abort();
	printf("== duplicate member report ==\n");	test_duplicate_report();
	printf("== bad arguments ==\n");			test_bad_args();
	printf("== concurrent storm ==\n");		test_concurrent_storm();
	printf("== rebuild pump ==\n");			test_resync();
	printf("== rebuild cancel ==\n");		test_resync_cancel();
	printf("== rebuild routing ==\n");		test_resync_routing();
	printf("== unload drain ==\n");			test_unload_drain();
	printf("== unload with a stuck member ==\n");	test_unload_stuck();

	printf("== no endio from a hardware context ==\n");
	CHECK(atomic_load(&hw_ctx_endios) == 0,
	      "%lld parent bio(s) were ended on a member completion path",
	      atomic_load(&hw_ctx_endios));

	printf("       %ld timed wait(s) expired (the stuck-unload drain budget)\n",
	       shim_wait_timeouts);

	if (failures) {
		printf("\nFAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("\nAll harness checks passed.\n");
	return 0;
}