// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — NVMe controller + RAID0 volume I/O path
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
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

/****************************************************************************
 * NVMe controller boot sequence (kept inline as documentation)
 *
 * For DEV_B000 (and any other CC_0108 device) the Windows AMD driver takes
 * its nvme.c code path, not the ahci.c one. See docs/GHIDRA_FINDINGS_2026.md.
 * This file implements the standard NVMe 1.4 controller boot sequence:
 *
 *   1. Read CAP, snapshot MQES / DSTRD / TO.
 *   2. Disable controller (CC.EN = 0); wait for CSTS.RDY = 0.
 *   3. Allocate admin SQ + CQ (DMA-coherent, 4 KiB aligned, zeroed).
 *   4. Program AQA / ASQ / ACQ.
 *   5. Set CC (4 KiB pages, NVM cmd set, IOSQES=6, IOCQES=4, EN=1).
 *   6. Poll CSTS until RDY = 1 (or CFS = 1, or CAP.TO * 500 ms elapses).
 *
 * No commands are submitted from here. Once the controller is ready the
 * higher layers can issue Identify / Set Features / I/O queue creation.
 ****************************************************************************/

#include "rc_linux.h"
#include <linux/unaligned.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/highmem.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

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
static DEFINE_MUTEX(rc_volume_lock);

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
/* blk-mq tag pool size per hctx.  Matches Windows per-queue SQ depth.
 * Total in-flight requests = nr_hw_queues × this. */
#define RC_VOLUME_QUEUE_DEPTH	256

static __le64     *rc_volume_prp_va[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static dma_addr_t  rc_volume_prp_pa[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];

/* Sequential read prefetch.
 *
 * Per-hctx state with multiple ping-pong slots.  When queue_rq sees a
 * READ that's contiguous with the previous READ on this hctx, it
 * speculatively issues a stripe-sized NVMe READ for the next stripe
 * (whose data lives on the OTHER member by RAID0 striping) into a
 * free slot's DMA buffer.  Subsequent app READs that fall within any
 * slot's [lba, lba+sectors) range are served from that buffer via
 * memcpy without touching the device.
 *
 * Two slots per hctx so a fresh prefetch can be in flight while the
 * previous slot is still being consumed by app reads.  Without the
 * second slot, the per-stripe consume → "now issue next prefetch" cycle
 * leaves a ~140 µs hole every other stripe (the time it takes the new
 * prefetch to complete).
 *
 * Each prefetch covers one stripe (1 MiB) and is dispatched as multiple
 * NVMe READ commands — each capped at the controller's MDTS (= 512 KiB
 * on the T700 dev box), so a 1 MiB prefetch issues 2 cmds.
 *
 * CID encoding (16-bit):
 *   bit 8        = prefetch marker (always set for PF CIDs)
 *   bit 7        = slot index (0..1)
 *   bits 6..4    = hctx_idx (0..7)
 *   bits 3..0    = cmd index within the prefetch (0..15)
 * Blk-mq tags (0..255) never set bit 8, so the dispatch is unambiguous. */
#define RC_VOLUME_PREFETCH_BYTES	(1u * 1024 * 1024)
#define RC_VOLUME_PREFETCH_SECTORS	(RC_VOLUME_PREFETCH_BYTES / 512u)
#define RC_VOLUME_PREFETCH_PAGES	(RC_VOLUME_PREFETCH_BYTES / PAGE_SIZE)
#define RC_VOLUME_PREFETCH_CID_BASE	0x100u
#define RC_VOLUME_MAX_HCTX		8u
#define RC_VOLUME_PREFETCH_SLOTS	2u

enum rc_prefetch_state {
	RC_PF_IDLE = 0,
	RC_PF_INFLIGHT,
	RC_PF_COMPLETE,
};

/* Sectors per NVMe command.  Bounded by the controller's MDTS — at
 * MDTS=7 with MPSMIN=0 this is 2^7 × 4 KiB = 512 KiB = 1024 sectors.
 * The 1 MiB prefetch buffer is filled by 2 of these commands. */
#define RC_VOLUME_PREFETCH_CMD_SECTORS	1024u
#define RC_VOLUME_PREFETCH_CMDS \
	(RC_VOLUME_PREFETCH_SECTORS / RC_VOLUME_PREFETCH_CMD_SECTORS)

struct rc_volume_prefetch_slot {
	enum rc_prefetch_state	state;
	u16			status;		/* NVMe SC/SCT, 0 = success */
	atomic_t		cmds_pending;	/* NVMe cmds still outstanding */
	sector_t		lba;		/* volume LBA prefetched (1 MiB-aligned) */
	u32			sectors;	/* total prefetch size in sectors */
	u32			served_sectors;	/* slot recycles when this reaches sectors */
	int			member;		/* member the prefetch landed on */
	/* Per-member DMA buffer + PRP list page (per member because IOMMU). */
	void			*buf_va[RC_VOLUME_MAX_MEMBERS];
	dma_addr_t		buf_pa[RC_VOLUME_MAX_MEMBERS];
	__le64			*prp_va[RC_VOLUME_MAX_MEMBERS];
	dma_addr_t		prp_pa[RC_VOLUME_MAX_MEMBERS];
};

struct rc_volume_prefetch {
	spinlock_t			lock;
	struct rc_volume_prefetch_slot	slots[RC_VOLUME_PREFETCH_SLOTS];
	/* Sequential-stream tracking. */
	sector_t			last_lba;
	u32				last_sectors;
	/* Stats. */
	u64				hits;
	u64				misses;
	u64				issued;
	u64				discarded;
};

static struct rc_volume_prefetch rc_volume_prefetch_state[RC_VOLUME_MAX_HCTX];

static int rc_volume_prefetch_enabled;
module_param_named(prefetch, rc_volume_prefetch_enabled, int, 0644);
MODULE_PARM_DESC(prefetch,
		 "Speculatively pre-issue the next stripe on sequential reads. "
		 "Off by default — the buffer size is hardcoded to 1 MiB which "
		 "over-prefetches on arrays with smaller stripes (256 KiB on the "
		 "test array) and the baseline driver already saturates the "
		 "device.  Set to 1 to opt in.");

/* ============================ Read cache ===========================
 *
 * Stripe-granularity LRU buffer cache, modeled after the buffer pool we
 * decoded in rcraid.sys (FUN_140013234).  Sits below the FS layer so
 * O_DIRECT / FILE_FLAG_NO_BUFFERING reads still hit it, matching what
 * Windows does.  Read path: hash-lookup by stripe LBA; on hit, memcpy
 * from buffer to bio pages; on miss, allocate a free entry (evicting
 * LRU if full), dispatch the read into that entry, populate on
 * completion.  Write path: invalidate any cache entries that overlap.
 *
 * Each entry holds one full stripe (1 MiB) of data.  The backing buffer
 * is allocated per-member via dma_alloc_coherent so we can DMA fill it
 * from whichever member owns the stripe; memcpy-out uses the per-member
 * KVA of the member that holds the data.
 *
 * Memory cost: nr_entries × stripe × nr_members.  Default 64 entries =
 * 128 MiB on a 2-member array; tunable via insmod cache_entries=.
 *
 * Locking: one global spinlock for hash + LRU.  Held briefly across
 * lookup + allocate/touch.  Memcpy and DMA happen with lock released. */
#define RC_VOLUME_CACHE_HASH_BITS	10
#define RC_VOLUME_CACHE_HASH_SIZE	(1u << RC_VOLUME_CACHE_HASH_BITS)
#define RC_VOLUME_CACHE_HASH_MASK	(RC_VOLUME_CACHE_HASH_SIZE - 1)

enum rc_cache_state {
	RC_CE_FREE = 0,
	RC_CE_LOADING,
	RC_CE_VALID,
};

struct rc_cache_entry {
	enum rc_cache_state	state;
	sector_t		lba;		/* volume LBA, stripe-aligned */
	u16			status;		/* NVMe SC/SCT from populating cmd, 0 = ok */
	atomic_t		cmds_pending;	/* NVMe cmds still in flight for LOADING entries */
	int			member;		/* member the data is read from */
	/* True when rc_cache_invalidate_range hit this entry while it was
	 * still LOADING.  The fill's data must be discarded (a write made it
	 * stale), so the ISR transitions to FREE instead of VALID on
	 * completion.  Cleared on each LOADING transition. */
	bool			abandoned;
	/* Request that triggered the fill, waiting for it to complete.
	 * Single waiter per entry — concurrent same-stripe requests just
	 * dispatch normally (rare, suboptimal but correct).  ISR walks
	 * this when transitioning LOADING → VALID. */
	struct request		*waiter;
	struct hlist_node	hash_node;
	struct list_head	lru_node;
	/* Per-member DMA buffer + PRP list.  buf_va[member] is the KVA we
	 * memcpy from when serving a hit.  buf_pa[member] feeds NVMe READ
	 * PRPs.  Only the member that actually holds the stripe is read
	 * from / written to per entry, but the buffer is allocated for
	 * both members so entries can be reused across members. */
	void			*buf_va[RC_VOLUME_MAX_MEMBERS];
	dma_addr_t		buf_pa[RC_VOLUME_MAX_MEMBERS];
	__le64			*prp_va[RC_VOLUME_MAX_MEMBERS];
	dma_addr_t		prp_pa[RC_VOLUME_MAX_MEMBERS];
};

static struct rc_cache_entry *rc_cache_entries;
static u32 rc_cache_nr_entries;
static spinlock_t rc_cache_lock;
static struct hlist_head *rc_cache_buckets;
static struct list_head rc_cache_lru;
static u64 rc_cache_hits;
static u64 rc_cache_misses;
static u64 rc_cache_evictions;

static int rc_cache_entries_param;
module_param_named(cache_entries, rc_cache_entries_param, int, 0444);
MODULE_PARM_DESC(cache_entries,
		 "Number of stripe cache entries (each entry uses up to 1 MiB "
		 "per member of DMA memory).  Off by default — measured against "
		 "the actual stripe size, the cache hurts read throughput on "
		 "non-repeating workloads and only the multi-member dispatch is "
		 "responsible for the measured improvements.  Set to a non-zero "
		 "value (e.g., 64, 128, 1100) to opt in for repeat-heavy "
		 "workloads.");

/* CIDs 0x2000..0x2FFF reserved for cache fills.  Each fill issues 2 cmds
 * (1 MiB / MDTS=512 KiB).  Layout: bit 13 = cache marker, bits 12..1 =
 * entry_idx (up to 4096 entries), bit 0 = cmd index within the fill. */
#define RC_VOLUME_CACHE_CID_BASE	0x2000u
#define RC_VOLUME_CACHE_CID_MASK	0x2000u
#define RC_VOLUME_CACHE_MAX_ENTRIES	4096u
#define RC_VOLUME_CACHE_CMD_SECTORS	1024u	/* 512 KiB per NVMe cmd, MDTS limit */

/* "Ghost" cache: small direct-mapped hash of recently-seen stripe LBAs
 * used to gate real cache population.  A stripe is only promoted to the
 * real cache on its SECOND access — first access just records the LBA
 * here and dispatches the read normally.  This avoids the cache fill's
 * memcpy overhead on workloads that read each stripe once (cold streams,
 * boot-time loads) while still letting CDM-style "read the same region
 * 5 times" benchmarks ramp into cache for loops 2+.
 *
 * Direct-mapped + collisions overwrite — false-negatives are fine
 * (data still served correctly, just no caching that round) and false
 * positives are rare with the bucket count chosen.  Memory: 64 KiB. */
#define RC_GHOST_HASH_BITS	13
#define RC_GHOST_HASH_SIZE	(1u << RC_GHOST_HASH_BITS)
#define RC_GHOST_HASH_MASK	(RC_GHOST_HASH_SIZE - 1)
static sector_t *rc_ghost_table;	/* RC_GHOST_HASH_SIZE entries, 0 = empty */

/* Per-hctx in-flight counter — cheap atomic proxy for "current queue
 * depth on this hctx".  Cache lookups still happen at any QD (hits are
 * fast), but cache FILLS are gated on QD < threshold: at high QD the
 * device itself outpaces the cache fill's memcpy overhead, so we'd be
 * adding work for no win.  Without this gate, SEQ1M Q8T1 drops from
 * ~19 GB/s native to ~14 GB/s cache-bound. */
static atomic_t rc_cache_hctx_inflight[RC_VOLUME_MAX_HCTX];
static int rc_cache_fill_max_qd = 2;
module_param_named(cache_fill_max_qd, rc_cache_fill_max_qd, int, 0644);
MODULE_PARM_DESC(cache_fill_max_qd,
		 "Only populate cache when per-hctx in-flight count <= this (default 2). "
		 "Cache HITS always serve regardless of QD.");

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
	u8                 hctx_idx;       /* for inflight counter decrement */
	atomic_t           members_pending;
	int                nents;          /* dma_map_sg output count, 0 = not mapped */
	/* Multi-stripe (multi-member) dispatch.  When ms_active is true, the
	 * request was split across multiple members in queue_rq; ms_nents[m]
	 * and ms_sg[m] hold each member's portion.  members_pending is set to
	 * the number of members with non-zero ms_nents.  The pdu->sg/nents
	 * fields above are unused in this path. */
	bool               ms_active;
	int                ms_nents[RC_VOLUME_MAX_MEMBERS];
	/* Non-NULL when this request is waiting on a cache fill.  .complete
	 * copies from cache_entry->buf_va[member] into the bio and ends the
	 * request without dma_unmap_sg (we never dma_map'd). */
	struct rc_cache_entry *cache_entry;
	struct scatterlist sg[RC_VOLUME_DATA_PAGES];
	struct scatterlist ms_sg[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_MS_PAGES_PER_MEMBER];
};

/* Helper: decrement the per-hctx inflight counter that was incremented at
 * the top of queue_rq.  Called from every code path that ends a request. */
static inline void rc_cache_dec_inflight(struct rc_volume_pdu *pdu)
{
	if (pdu->hctx_idx < RC_VOLUME_MAX_HCTX)
		atomic_dec(&rc_cache_hctx_inflight[pdu->hctx_idx]);
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

/* Forward declarations used inside the per-queue ISR. */
static void rc_volume_prefetch_isr(unsigned int hctx_idx,
				   unsigned int slot_idx, u16 status);
static void rc_volume_unmap_request_sg(struct rc_volume_pdu *pdu);
static void rc_cache_isr(unsigned int entry_idx, u16 status);

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
		if (cid & RC_VOLUME_CACHE_CID_MASK) {
			/* Cache fill completion (CID 0x2000..0x2FFF):
			 * bits 12..1 = entry_idx, bit 0 = cmd index. */
			u16 sc = (status >> 1) & 0x7fff;
			unsigned int entry_idx = (cid >> 1) & 0xfff;
			rc_cache_isr(entry_idx, sc);
		} else if (cid & RC_VOLUME_PREFETCH_CID_BASE) {
			u16 sc = (status >> 1) & 0x7fff;
			unsigned int slot_idx = (cid >> 7) & 1;
			unsigned int hctx_idx = (cid >> 4) & 7;
			rc_volume_prefetch_isr(hctx_idx, slot_idx, sc);
		} else {
			req = blk_mq_tag_to_rq(tags, cid);
			if (req) {
				u16 sc = (status >> 1) & 0x7fff;
				pdu = blk_mq_rq_to_pdu(req);
				if (sc)
					pdu->sc_sct = sc;
				if (atomic_dec_and_test(&pdu->members_pending)) {
					/* Try to defer to local batch; fall back
					 * to the softirq path if the batch is full. */
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

		q->cq_head = (q->cq_head + 1) % q->cq_depth;
		if (q->cq_head == 0)
			q->cq_phase ^= 1;
		advanced = true;
	}
	if (advanced)
		rc_nvme_ring_cq_doorbell(adapter, q->qid, q->cq_head);
	spin_unlock(&q->lock);

	/* Finish requests inline (no softirq round-trip).  Each call does
	 * dma_unmap_sg + blk_mq_end_request.  Saves the .complete softirq
	 * dispatch latency, which on Q1T1 dominates 5-15 us of clat. */
	{
		unsigned int i;
		for (i = 0; i < done_n; i++) {
			struct request *req = done_reqs[i];
			struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
			rc_volume_unmap_request_sg(pdu);
			rc_cache_dec_inflight(pdu);
			if (pdu->sc_sct)
				blk_mq_end_request(req, BLK_STS_IOERR);
			else
				blk_mq_end_request(req, BLK_STS_OK);
		}
	}
	return IRQ_HANDLED;
}

/* Back-compat shim: rc_hw_interrupt_handler still calls rc_nvme_irq for
 * the admin vector when ctrl_mode == NVMe.  The I/O vectors get
 * rc_nvme_io_queue_irq registered directly by rc_nvme_create_io_queues. */
irqreturn_t rc_nvme_irq(struct rc_adapter *adapter)
{
	return rc_nvme_admin_irq(0, adapter);
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
	u16 status, sc_sct;
	int ret;

	slot = (struct rc_nvme_sqe *)nvme->admin_sq + nvme->admin_sq_tail;
	cmd->cid = cpu_to_le16(0);
	memcpy(slot, cmd, sizeof(*cmd));

	tail = (nvme->admin_sq_tail + 1) % nvme->admin_sq_depth;
	nvme->admin_sq_tail = tail;
	wmb();
	rc_nvme_ring_sq_doorbell(adapter, 0, tail);

	ret = rc_nvme_wait_admin_completion(adapter);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_admin_cmd: timeout waiting for CQE (opc=0x%02x)\n",
			  cmd->opc);
		return ret;
	}

	cqe = (struct rc_nvme_cqe *)nvme->admin_cq + nvme->admin_cq_head;
	status = le16_to_cpu(cqe->status);
	sc_sct = (status >> 1) & 0x7fff;
	if (out_result)
		*out_result = le32_to_cpu(cqe->result);

	nvme->admin_cq_head = (nvme->admin_cq_head + 1) % nvme->admin_cq_depth;
	if (nvme->admin_cq_head == 0)
		nvme->admin_cq_phase ^= 1;
	rc_nvme_ring_cq_doorbell(adapter, 0, nvme->admin_cq_head);

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
		char sn[21], mn[41], fr[9];

		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_SN, 20, sn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_MN, 40, mn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_FR, 8,  fr);

		nvme->mdts = mdts;
		nvme->max_transfer_bytes = mdts ?
			(1u << mdts) * (1u << (12 + mpsmin)) : 0;

		rc_printk(RC_NOTE,
			  "rc_nvme_identify_controller: VID=0x%04x SSVID=0x%04x SN='%s' MN='%s' FR='%s' NN=%u MDTS=%u MPSMIN=%u max_xfer=%u B\n",
			  vid, ssvid, sn, mn, fr, nn,
			  mdts, mpsmin, nvme->max_transfer_bytes);
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

/* Allocate one I/O queue pair: DMA-coherent SQ + CQ, wire it up via
 * Create I/O CQ + Create I/O SQ admin commands, register its MSI-X
 * vector with rc_nvme_io_queue_irq.  qid = 1-based.  vec_index is the
 * MSI-X vector slot (also 1-based — vector 0 is admin). */
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
	 * vector's interrupts to rc_nvme_io_queue_irq with `q` as dev_id. */
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

	nvme->io_queues[qid - 1] = q;
	rc_printk(RC_NOTE,
		  "rc_nvme_create_one_io_queue: %s qid=%u up — SQ/CQ depth=%u IV=%u IRQ=%d\n",
		  pci_name(adapter->pdev), qid, depth, vec_index, q->irq_vector);
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
	struct rc_nvme_sqe cmd;
	size_t sq_bytes, cq_bytes;

	if (!q)
		return;

	if (q->irq_vector >= 0) {
		free_irq(q->irq_vector, q);
		q->irq_vector = -1;
	}

	if (q->sq) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_SQ;
		cmd.cdw10 = cpu_to_le32(q->qid);
		(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
		sq_bytes = (size_t)q->sq_depth * RC_NVME_SQ_ENTRY_SIZE;
		dma_free_coherent(dev, sq_bytes, q->sq, q->sq_dma);
		q->sq = NULL;
	}
	if (q->cq) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_CQ;
		cmd.cdw10 = cpu_to_le32(q->qid);
		(void)rc_nvme_admin_cmd(adapter, &cmd, NULL);
		cq_bytes = (size_t)q->cq_depth * RC_NVME_CQ_ENTRY_SIZE;
		dma_free_coherent(dev, cq_bytes, q->cq, q->cq_dma);
		q->cq = NULL;
	}
	kfree(q);
}

/* Allocate nvme->nr_io_queues queue pairs.  qid = i+1, vec_index = i+1
 * (vector 0 is reserved for admin).  On partial failure, rolls back
 * everything created so far. */
static int rc_nvme_create_io_queues(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u16 i;
	int ret;

	if (nvme->nr_io_queues == 0)
		nvme->nr_io_queues = 1;
	if (nvme->nr_io_queues > RC_NVME_IO_QUEUE_TARGET)
		nvme->nr_io_queues = RC_NVME_IO_QUEUE_TARGET;

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

/* Submit one I/O command to a specific NVMe I/O queue asynchronously.
 * Caller (blk-mq queue_rq) returns BLK_STS_OK immediately; completion
 * lands in rc_nvme_io_queue_irq → blk_mq_complete_request → .complete. */
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

/* Synchronous I/O command used at boot time to read metadata before the
 * blk-mq disk is up.  Always runs against I/O queue 0 (qid=1).  Submits
 * a single SQE with CID=0, waits on q->cq_wait until the CQE arrives
 * (woken by rc_nvme_io_queue_irq), consumes the CQE, advances head +
 * doorbell.  Safe because the ISR returns early — without consuming
 * CQEs through blk_mq_complete_request — while rc_volume_disk is NULL. */
static int rc_nvme_io_cmd_sync(struct rc_adapter *adapter,
			       struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_io_queue *q;
	struct rc_nvme_cqe *cqe;
	unsigned long deadline;
	u8 expected_phase;
	u16 status, sc_sct;

	if (!nvme->io_queues[0])
		return -ENODEV;
	q = nvme->io_queues[0];

	cmd->cid = cpu_to_le16(0);
	rc_nvme_io_submit(q, cmd);

	cqe = (struct rc_nvme_cqe *)q->cq + q->cq_head;
	expected_phase = q->cq_phase;
	deadline = jiffies + msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);
	while (time_before(jiffies, deadline)) {
		long left = deadline - jiffies;
		long tick = min_t(long, left, max_t(long, 1, msecs_to_jiffies(1)));

		if (wait_event_timeout(q->cq_wait,
				       (le16_to_cpu(READ_ONCE(cqe->status)) & 1) ==
					       expected_phase,
				       tick) > 0)
			goto have_cqe;
	}
	rc_printk(RC_ERROR,
		  "rc_nvme_io_cmd_sync: timeout waiting for CQE (opc=0x%02x)\n",
		  cmd->opc);
	return -ETIMEDOUT;

have_cqe:
	status = le16_to_cpu(READ_ONCE(cqe->status));
	sc_sct = (status >> 1) & 0x7fff;

	q->cq_head = (q->cq_head + 1) % q->cq_depth;
	if (q->cq_head == 0)
		q->cq_phase ^= 1;
	rc_nvme_ring_cq_doorbell(adapter, q->qid, q->cq_head);

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

/* How far into the config ring to scan for the first RC_LogicalDevice
 * record.  On the dev box the active LD is at ring+0x1d sectors; 128
 * sectors is plenty of headroom even for arrays with chains of older
 * configs preceding the current one. */
#define RC_LD_SCAN_CHUNK_SECTORS	16u	/* 8 KiB = max 2-page rc_nvme_read_lba */
#define RC_LD_SCAN_MAX_CHUNKS		8u	/* total = 128 sectors = 64 KiB */

/* Read sectors from the config ring and locate the first valid
 * RC_LogicalDevice record (tag 0x25BD).  Parse Devices, DeviceType,
 * Capacity, ChunkSize, and walk the LogicalElement array to find this
 * adapter's position (where md_device_id matches an element's DeviceID).
 * Results land in adapter->ctx.nvme.ld_*.  Failure leaves ld_valid=false
 * — the caller falls back to the legacy BDF-ordered registration path. */
static void rc_volume_parse_logical_device(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	u8 *buf;
	dma_addr_t buf_dma;
	const u32 chunk_bytes = RC_LD_SCAN_CHUNK_SECTORS * 512u;
	u64 ring_lba = nvme->md_config_ring_lba;
	u32 scan;

	nvme->ld_valid = false;
	nvme->ld_my_position = -1;

	if (!ring_lba) {
		rc_printk(RC_WARN,
			  "rc_volume_parse_logical_device: %s no config_ring_lba in metadata\n",
			  pci_name(adapter->pdev));
		return;
	}

	buf = dma_alloc_coherent(dev, chunk_bytes, &buf_dma, GFP_KERNEL);
	if (!buf)
		return;

	for (scan = 0; scan < RC_LD_SCAN_MAX_CHUNKS; scan++) {
		u64 chunk_lba = ring_lba + scan * RC_LD_SCAN_CHUNK_SECTORS;
		u32 i;
		int ret;

		ret = rc_nvme_read_lba(adapter, chunk_lba,
				       (u16)(RC_LD_SCAN_CHUNK_SECTORS - 1),
				       buf_dma);
		if (ret) {
			rc_printk(RC_WARN,
				  "rc_volume_parse_logical_device: %s read LBA 0x%llx failed (%d)\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)chunk_lba, ret);
			break;
		}

		for (i = 0; i + 4 <= chunk_bytes; i += 4) {
			u8 *ld;
			u32 devtype, devices, elem_off, chunk, chunk_index;
			u64 capacity;
			int my_pos = -1;
			u32 j;

			if (get_unaligned_le32(buf + i) !=
			    RC_DST_LOGICAL_DEVICE)
				continue;

			ld = buf + i;
			devtype  = get_unaligned_le32(ld + RC_LD_DEVICETYPE_OFFSET);
			devices  = get_unaligned_le32(ld + RC_LD_DEVICES_OFFSET);
			elem_off = get_unaligned_le32(ld + RC_LD_ELEMENTOFFSET_OFFSET);
			chunk    = get_unaligned_le32(ld + RC_LD_CHUNKSIZE_OFFSET);
			chunk_index = get_unaligned_le32(ld + RC_LD_CHUNKINDEX_OFFSET);
			capacity = get_unaligned_le64(ld + RC_LD_CAPACITY_OFFSET);

			if (devices < 1 || devices > RC_VOLUME_MAX_MEMBERS)
				continue;
			if ((u64)i + elem_off +
			    (u64)devices * RC_LE_BYTES > chunk_bytes)
				continue;

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

			rc_printk(RC_INFO,
				  "rc_volume_parse_logical_device: %s LD@ring+%u.%u devtype=0x%04x devices=%u chunk=%u chunk_idx=%u capacity=%llu my_pos=%d alloc_off=%llu alloc_sz=%llu user_off=%llu user_sz=%llu%s\n",
				  pci_name(adapter->pdev),
				  scan * RC_LD_SCAN_CHUNK_SECTORS + (i / 512),
				  i % 512,
				  devtype, devices, chunk, chunk_index,
				  (unsigned long long)capacity, my_pos,
				  (unsigned long long)alloc_off,
				  (unsigned long long)alloc_sz,
				  (unsigned long long)user_off,
				  (unsigned long long)user_sz,
				  my_pos < 0 ? " (skip — not our member)" :
					       " (match)");

			if (my_pos < 0)
				continue;

			nvme->ld_device_type      = devtype;
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
		  "rc_volume_parse_logical_device: %s no matching LogicalDevice record found in first %u sectors of config ring\n",
		  pci_name(adapter->pdev),
		  RC_LD_SCAN_MAX_CHUNKS * RC_LD_SCAN_CHUNK_SECTORS);

done:
	dma_free_coherent(dev, chunk_bytes, buf, buf_dma);
}

/* Map a logical RAID0 LBA to (member_index, physical LBA on that member).
 * Caller must hold rc_volume_lock (or be in a single-threaded init path). */
static void rc_volume_map_lba(u64 logical_lba, int *out_member, u64 *out_phys)
{
	u32 stripe = rc_volume_stripe_sectors;
	int nmembers = rc_volume_member_count;
	u64 stripe_num = div_u64(logical_lba, stripe);
	u32 stripe_off = (u32)(logical_lba - stripe_num * stripe);
	u32 member_idx = (u32)(stripe_num % (u32)nmembers);
	u64 phys_stripe = div_u64(stripe_num, (u32)nmembers);

	*out_member = (int)member_idx;
	*out_phys   = phys_stripe * stripe + stripe_off
		    + rc_volume_member_phys_offset[member_idx];
}

/* Read one LBA from the assembled RAID0 volume. */
static int rc_volume_read_lba(u64 logical_lba, dma_addr_t buf_dma)
{
	int member_idx;
	u64 phys_lba;
	struct rc_adapter *adapter;

	if (rc_volume_member_count != RC_VOLUME_EXPECTED_MEMBERS ||
	    !rc_volume_stripe_sectors)
		return -ENODEV;

	rc_volume_map_lba(logical_lba, &member_idx, &phys_lba);
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
		memset(bufs[member_idx], 0, 16);
		ret = rc_volume_read_lba(probes[i], buf_dmas[member_idx]);
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
 *     encodes the actual stripe size for RAID0.  Confirmed by reverse-
 *     engineering rcraid.sys 9.3.2: FUN_140052404 and FUN_140055760
 *     both dispatch on this value (read from in-memory LD[0xC8], which
 *     is populated from on-disk LD[0x110]) and pick a sector count:
 *
 *         index   stripe
 *         -----   ------
 *         3       512 sectors (256 KiB)   <- our test array
 *         2       256 sectors (128 KiB)
 *         else    128 sectors (64 KiB)
 *
 *     The pattern is consistent with chunk_sectors = 64 << index for
 *     index >= 1, which would extrapolate to 1024 sectors (512 KiB)
 *     for index 4 and 2048 sectors (1 MiB) for index 5; the observed
 *     Windows code only handles 2 and 3 explicitly and falls back to
 *     64 KiB otherwise, so we mirror that to match Windows behavior
 *     bit-for-bit on this hardware. */
static u32 rc_volume_chunk_sectors_for(u32 devtype, u32 ld_chunk,
				       u32 ld_chunk_index)
{
	if (ld_chunk)
		return ld_chunk;
	switch (devtype) {
	case RC_LDT_RAID0:
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
		expected = nvme->ld_devices;
		chunk_sectors = rc_volume_chunk_sectors_for(nvme->ld_device_type,
							    nvme->ld_chunk_sectors,
							    nvme->ld_chunk_index);
		if (!chunk_sectors) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s unknown DeviceType=0x%x with ChunkSize=0 — ignoring\n",
				  pci_name(adapter->pdev), nvme->ld_device_type);
			goto out;
		}
	} else {
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

	/* First member: seed expected count + stripe.  Later members must match. */
	if (rc_volume_member_count == 0) {
		rc_volume_expected_members = expected;
		rc_volume_stripe_sectors   = chunk_sectors;
	} else {
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
	} else {
		/* Legacy BDF-sorted insertion. */
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
out:
	mutex_unlock(&rc_volume_lock);
}

/* DMA direction for the data phase of a READ/WRITE request. */
static inline enum dma_data_direction rc_volume_dma_dir(u8 op)
{
	return (op == REQ_OP_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
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

	mapped_nents = dma_map_sg(dma_dev, pdu->sg, input_nents,
				  rc_volume_dma_dir(pdu->op));
	if (mapped_nents == 0)
		return BLK_STS_IOERR;

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

	if (pdu->ms_active) {
		for (m = 0; m < rc_volume_member_count &&
			    m < RC_VOLUME_MAX_MEMBERS; m++) {
			if (!pdu->ms_nents[m])
				continue;
			dma_dev = &rc_volume_members[m]->pdev->dev;
			dma_unmap_sg(dma_dev, pdu->ms_sg[m], pdu->ms_nents[m],
				     rc_volume_dma_dir(pdu->op));
			pdu->ms_nents[m] = 0;
		}
		pdu->ms_active = false;
		return;
	}

	if (!pdu->nents)
		return;
	if (pdu->member_idx < 0 || pdu->member_idx >= rc_volume_member_count) {
		pdu->nents = 0;
		return;
	}
	dma_dev = &rc_volume_members[pdu->member_idx]->pdev->dev;
	dma_unmap_sg(dma_dev, pdu->sg, pdu->nents, rc_volume_dma_dir(pdu->op));
	pdu->nents = 0;
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
 * directly.  Single-page transfers leave PRP2 zero. */
static void rc_volume_build_prp(struct rc_nvme_sqe *cmd, int member_idx, u32 tag,
				struct scatterlist *sgl, int nents)
{
	__le64 *prp_list      = rc_volume_prp_va[member_idx][tag];
	dma_addr_t prp_list_pa = rc_volume_prp_pa[member_idx][tag];
	struct scatterlist *sg;
	unsigned int n_pages = 0;
	int i;

	cmd->prp1 = 0;
	cmd->prp2 = 0;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);

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
}

/* Build an NVMe READ/WRITE SQE.  CID = tag.  FUA passed through when the
 * request has REQ_FUA set.  Caller has already dma_map_sg'd the bvecs
 * into pdu->sg / pdu->nents; rc_volume_build_prp enumerates those pages
 * into PRP1/PRP2 (and the per-tag PRP list if > 2 pages). */
static void rc_volume_build_io_sqe(struct rc_nvme_sqe *cmd, int member_idx,
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

	rc_volume_build_prp(cmd, member_idx, tag, sgl, nents);
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
static void rc_volume_build_discard_sqe(struct rc_nvme_sqe *cmd, int member_idx,
					u32 tag, u64 slba, u32 nlb)
{
	struct rc_nvme_dsm_range *range = (struct rc_nvme_dsm_range *)
					   rc_volume_prp_va[member_idx][tag];

	range->context_attrs = cpu_to_le32(0);
	range->nlb           = cpu_to_le32(nlb);
	range->slba          = cpu_to_le64(slba);

	memset(cmd, 0, sizeof(*cmd));
	cmd->opc   = RC_NVME_NVM_OP_DSM;
	cmd->cid   = cpu_to_le16((u16)tag);
	cmd->nsid  = cpu_to_le32(1);
	cmd->prp1  = cpu_to_le64(rc_volume_prp_pa[member_idx][tag]);
	cmd->cdw10 = cpu_to_le32(0);              /* NR = 0 means 1 range */
	cmd->cdw11 = cpu_to_le32(RC_NVME_DSM_AD); /* Deallocate */
}

/* Returns true if any member adapter has been flagged dead.  Hot-path check
 * called from rc_volume_queue_rq; if true the RAID0 volume is unusable and
 * every incoming request fast-fails. */
static inline bool rc_volume_any_member_dead(void)
{
	int i;

	for (i = 0; i < rc_volume_member_count; i++)
		if (READ_ONCE(rc_volume_members[i]->ctx.nvme.dead))
			return true;
	return false;
}

/* Forward declarations for prefetch + cache helpers used in queue_rq. */
static void rc_volume_prefetch_submit(unsigned int hctx_idx,
				      unsigned int slot_idx,
				      struct rc_volume_prefetch *pf,
				      u64 phys_lba);
static void rc_volume_prefetch_copy_out(struct request *req, void *src);
static void rc_volume_prefetch_invalidate(unsigned int hctx_idx);
static u32 rc_cache_bucket_for(sector_t lba);
static struct rc_cache_entry *rc_cache_lookup_valid(sector_t stripe_lba);
static bool rc_cache_has_or_loading(sector_t stripe_lba);
static struct rc_cache_entry *rc_cache_alloc_entry(void);
static void rc_cache_invalidate_range(sector_t pos, u32 nr_sectors);
static bool rc_ghost_check_and_mark(sector_t stripe_lba);
static void rc_cache_submit_fill(unsigned int hctx_idx,
				 u32 entry_idx,
				 sector_t stripe_lba,
				 u64 phys_lba);

/* blk-mq request handler.  chunk_sectors=stripe_sectors in queue_limits
 * keeps each request inside one stripe, so it maps to exactly one member.
 *
 * Async submission: dma_map_sg the bio's pages, build PRPs from the
 * resulting scatterlist, submit SQE with CID=tag, return BLK_STS_OK.
 * Completion lands in rc_nvme_irq, which calls blk_mq_complete_request;
 * rc_volume_complete then runs in softirq, dma_unmap_sg's the pages, and
 * ends the request.  Hardware DMAs directly to/from the bio's user
 * pages — no bounce buffers, no memcpy on either side. */

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
static blk_status_t rc_volume_dispatch_multi_stripe(
		struct blk_mq_hw_ctx *hctx, struct request *req,
		struct rc_volume_pdu *pdu, sector_t pos, u32 nr_sectors,
		enum req_op op)
{
	const u32 stripe = rc_volume_stripe_sectors;
	const int nm = rc_volume_member_count;
	struct req_iterator iter;
	struct bio_vec bv;
	sector_t cur_lba = pos;
	u32 sg_used[RC_VOLUME_MAX_MEMBERS] = {0};
	u32 member_sectors[RC_VOLUME_MAX_MEMBERS] = {0};
	u64 member_start_lba[RC_VOLUME_MAX_MEMBERS] = {0};
	bool member_has_data[RC_VOLUME_MAX_MEMBERS] = {0};
	int members_with_data = 0;
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || !stripe)
		return BLK_STS_IOERR;

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
			int mbr = (int)(stripe_idx % nm);

			if (chunk_bytes == 0 || chunk_bytes > seg_remaining)
				return BLK_STS_IOERR;
			if (sg_used[mbr] >= RC_VOLUME_MS_PAGES_PER_MEMBER)
				return BLK_STS_IOERR;

			if (!member_has_data[mbr]) {
				u64 phys_lba;
				int phys_member;
				rc_volume_map_lba(cur_lba, &phys_member,
						  &phys_lba);
				if (phys_member != mbr)
					return BLK_STS_IOERR;
				member_start_lba[mbr] = phys_lba;
				member_has_data[mbr] = true;
				members_with_data++;
			}

			sg_set_page(&pdu->ms_sg[mbr][sg_used[mbr]],
				    seg_page, chunk_bytes, seg_off_in_page);
			sg_used[mbr]++;
			member_sectors[mbr] += chunk_sectors;

			seg_off_in_page += chunk_bytes;
			seg_remaining   -= chunk_bytes;
			seg_len_sectors -= chunk_sectors;
			cur_lba         += chunk_sectors;
		}
	}

	if (members_with_data < 2)
		return BLK_STS_IOERR;	/* shouldn't reach here for single-member */

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
			return BLK_STS_IOERR;
		}
		pdu->ms_nents[m] = mapped;
	}

	pdu->member_idx = -1;	/* multi-member — no single member to attribute */
	atomic_set(&pdu->members_pending, members_with_data);

	/* Submit one NVMe cmd per member.  Must call blk_mq_start_request
	 * before the first submit (an ISR completion racing us would otherwise
	 * call blk_mq_complete on a request blk-mq doesn't yet consider
	 * started). */
	blk_mq_start_request(req);
	for (m = 0; m < nm; m++) {
		struct rc_nvme_sqe cmd;
		u32 tag = req->tag;
		u32 nvme_nlb = member_sectors[m];

		if (!pdu->ms_nents[m])
			continue;

		rc_volume_build_io_sqe(&cmd, m, tag,
				       op == REQ_OP_WRITE ?
					 RC_NVME_NVM_OP_WRITE :
					 RC_NVME_NVM_OP_READ,
				       member_start_lba[m],
				       (u16)(nvme_nlb - 1),
				       (req->cmd_flags & REQ_FUA) != 0,
				       pdu->ms_sg[m], pdu->ms_nents[m]);
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
	pdu->ms_active = false;
	memset(pdu->ms_nents, 0, sizeof(pdu->ms_nents));
	pdu->cache_entry = NULL;
	pdu->hctx_idx = (u8)(hctx->queue_num < RC_VOLUME_MAX_HCTX ?
			     hctx->queue_num : 0xff);
	if (pdu->hctx_idx < RC_VOLUME_MAX_HCTX)
		atomic_inc(&rc_cache_hctx_inflight[pdu->hctx_idx]);

	/* If any member adapter has been flagged dead (by the ISR's CSTS
	 * check, by a prior timeout, or by drain), the RAID0 volume can't
	 * serve I/O.  Fail every incoming request rather than wedging more
	 * CIDs against a controller we no longer trust. */
	if (rc_volume_any_member_dead()) {
		blk_mq_start_request(req);
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	/* FLUSH fans out to every member: set the pending counter to the
	 * member count, build a single FLUSH SQE template (CID=tag, no
	 * data), and submit it on every member's I/O SQ.  Each member's
	 * ISR atomically decrements; the last one calls blk_mq_complete. */
	if (op == REQ_OP_FLUSH) {
		atomic_set(&pdu->members_pending, rc_volume_member_count);
		pdu->member_idx = -1;
		rc_volume_build_flush_sqe(&cmd, tag);

		blk_mq_start_request(req);
		/* Use this hctx's NVMe queue on every member.  Completions
		 * land on the same hctx so blk_mq_tag_to_rq's tags[hctx_idx]
		 * lookup resolves to the right request. */
		for (i = 0; i < rc_volume_member_count; i++)
			rc_nvme_io_submit(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num],
					  &cmd);
		return BLK_STS_OK;
	}

	if (op != REQ_OP_READ && op != REQ_OP_WRITE && op != REQ_OP_DISCARD) {
		blk_mq_start_request(req);
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	if (nr_sectors == 0 ||
	    pos + nr_sectors > get_capacity(rc_volume_disk) ||
	    (op != REQ_OP_DISCARD &&
	     nr_sectors > (RC_VOLUME_DATA_BYTES / 512))) {
		blk_mq_start_request(req);
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	/* Multi-stripe READ/WRITE: dispatch one NVMe cmd per affected member
	 * instead of the 1-cmd-per-stripe split blk-mq would otherwise force.
	 * For a 1 MiB write on a 2-member RAID0 with 256 KiB stripe, this
	 * cuts 4 cmds → 2 cmds and halves the per-byte queue_rq /
	 * dma_map / completion overhead. */
	if ((op == REQ_OP_READ || op == REQ_OP_WRITE) &&
	    rc_volume_stripe_sectors &&
	    nr_sectors > 0 &&
	    (pos / rc_volume_stripe_sectors) !=
		((pos + nr_sectors - 1) / rc_volume_stripe_sectors)) {
		blk_status_t st = rc_volume_dispatch_multi_stripe(hctx, req,
								  pdu, pos,
								  nr_sectors,
								  op);
		if (st != BLK_STS_OK) {
			/* dispatch_multi_stripe returns errors from paths
			 * BEFORE its internal blk_mq_start_request, so we
			 * have to start the request before ending it —
			 * blk-mq requires every request to be started exactly
			 * once between queue_rq and end. */
			blk_mq_start_request(req);
			rc_cache_dec_inflight(pdu);
			blk_mq_end_request(req, st);
		}
		return BLK_STS_OK;
	}

	/* Read cache — fastest path: stripe-granularity LRU cache.
	 * Check before prefetch.  On miss, allocate an entry and dispatch
	 * the NVMe READ INTO the cache buffer (not the bio pages).  The
	 * request registers as the entry's waiter; when the fill completes,
	 * the ISR calls blk_mq_complete_request on the waiter and .complete
	 * memcpys from the cache buffer into the bio.  Future reads of the
	 * same stripe hit synchronously here. */
	if (op == REQ_OP_READ && rc_cache_entries && rc_volume_stripe_sectors) {
		sector_t stripe = rc_volume_stripe_sectors;
		sector_t stripe_lba = (pos / stripe) * stripe;

		if (pos + nr_sectors <= stripe_lba + stripe) {
			struct rc_cache_entry *e;
			unsigned long flags;
			void *src = NULL;
			u16 ce_status = 0;
			bool issued_fill = false;
			u32 fill_entry_idx = 0;
			int fill_member = -1;
			u64 fill_phys_lba = 0;

			spin_lock_irqsave(&rc_cache_lock, flags);
			e = rc_cache_lookup_valid(stripe_lba);
			if (e) {
				/* HIT — copy out synchronously. */
				ce_status = e->status;
				src = (u8 *)e->buf_va[e->member] +
				      (u32)(pos - stripe_lba) * 512u;
				rc_cache_hits++;
			} else if (atomic_read(&rc_cache_hctx_inflight[pdu->hctx_idx]) <=
				   rc_cache_fill_max_qd &&
				   !rc_cache_has_or_loading(stripe_lba) &&
				   rc_ghost_check_and_mark(stripe_lba)) {
				/* Low queue depth + ghost hit (2nd+ access).
				 * Worth caching now.  At high QD, skip fill
				 * because the device's native parallelism beats
				 * cache-fill memcpy. */
				e = rc_cache_alloc_entry();
				if (e) {
					rc_volume_map_lba(stripe_lba,
							  &fill_member, &fill_phys_lba);
					if (stripe_lba + stripe <= get_capacity(rc_volume_disk)) {
						e->state = RC_CE_LOADING;
						e->abandoned = false;
						e->member = fill_member;
						e->lba = stripe_lba;
						e->status = 0;
						e->waiter = req;
						pdu->cache_entry = e;
						pdu->member_idx = fill_member;
						pdu->nents = 0;
						atomic_set(&pdu->members_pending, 1);
						hlist_add_head(&e->hash_node,
							       &rc_cache_buckets[rc_cache_bucket_for(stripe_lba)]);
						list_add_tail(&e->lru_node, &rc_cache_lru);
						fill_entry_idx = (u32)(e - rc_cache_entries);
						issued_fill = true;
						rc_cache_misses++;
					} else {
						/* Past end of disk — put back. */
						list_add_tail(&e->lru_node, &rc_cache_lru);
						rc_cache_misses++;
					}
				} else {
					rc_cache_misses++;
				}
			} else {
				/* Already LOADING — fall through to normal
				 * dispatch (suboptimal duplicate, but correct). */
				rc_cache_misses++;
			}
			spin_unlock_irqrestore(&rc_cache_lock, flags);

			if (src && ce_status == 0) {
				rc_volume_prefetch_copy_out(req, src);
				blk_mq_start_request(req);
				rc_cache_dec_inflight(pdu);
				blk_mq_end_request(req, BLK_STS_OK);
				return BLK_STS_OK;
			}
			if (issued_fill) {
				blk_mq_start_request(req);
				rc_cache_submit_fill(hctx->queue_num,
						     fill_entry_idx, stripe_lba,
						     fill_phys_lba);
				return BLK_STS_OK;
			}
		}
	} else if (op == REQ_OP_WRITE && rc_cache_entries) {
		rc_cache_invalidate_range(pos, nr_sectors);
	}

	/* Prefetch — fast path: serve READ from any prefetch slot whose
	 * cached range fully covers the request.  Non-READ ops invalidate.
	 *
	 * Sequential detection + speculation happens later in this function
	 * after the normal dispatch path, using last_lba/last_sectors. */
	if (rc_volume_prefetch_enabled) {
		unsigned int hctx_idx = hctx->queue_num;
		if (op == REQ_OP_READ && hctx_idx < RC_VOLUME_MAX_HCTX) {
			struct rc_volume_prefetch *pf =
				&rc_volume_prefetch_state[hctx_idx];
			unsigned long flags;
			void *src = NULL;
			u16 pf_status = 0;
			bool hit = false;
			unsigned int s;

			int hit_slot = -1;
			spin_lock_irqsave(&pf->lock, flags);
			for (s = 0; s < RC_VOLUME_PREFETCH_SLOTS; s++) {
				struct rc_volume_prefetch_slot *sl = &pf->slots[s];
				if (sl->state == RC_PF_COMPLETE &&
				    pos >= sl->lba &&
				    pos + nr_sectors <= sl->lba + sl->sectors) {
					u32 off_sectors = (u32)(pos - sl->lba);
					pf_status = sl->status;
					src = (u8 *)sl->buf_va[sl->member] +
					      off_sectors * 512u;
					/* Leave the slot in COMPLETE state until after
					 * the memcpy.  Marking IDLE here would let a
					 * speculative prefetch reuse the buffer and
					 * DMA over our source mid-copy. */
					hit = true;
					hit_slot = (int)s;
					pf->hits++;
					break;
				}
			}
			spin_unlock_irqrestore(&pf->lock, flags);

			if (hit && pf_status == 0) {
				rc_volume_prefetch_copy_out(req, src);
				blk_mq_start_request(req);
				/* Now safe to release the slot, then try to issue
				 * a new prefetch for any stripe we don't already
				 * have cached. */
				spin_lock_irqsave(&pf->lock, flags);
				{
					struct rc_volume_prefetch_slot *hsl =
						&pf->slots[hit_slot];
					hsl->served_sectors += nr_sectors;
					if (hsl->served_sectors >= hsl->sectors) {
						hsl->state = RC_PF_IDLE;
						hsl->served_sectors = 0;
					}
				}
				{
					sector_t prev_last_lba = pf->last_lba;
					u32 prev_last_sectors = pf->last_sectors;
					bool was_seq = (prev_last_sectors &&
							pos == prev_last_lba + prev_last_sectors);
					pf->last_lba = pos;
					pf->last_sectors = nr_sectors;
					if (was_seq && rc_volume_stripe_sectors) {
						u64 cur_stripe = pos / rc_volume_stripe_sectors;
						u64 want_lba;
						int idle_slot = -1;
						bool already_have = false;
						unsigned int ss;
						/* Aim 2 stripes ahead: by the time we
						 * consume the current cached stripe and
						 * the next one (slot N+1), slot N+2 needs
						 * to be in flight or already done. */
						want_lba = (cur_stripe + 2) * rc_volume_stripe_sectors;
						for (ss = 0; ss < RC_VOLUME_PREFETCH_SLOTS; ss++) {
							struct rc_volume_prefetch_slot *sl = &pf->slots[ss];
							if ((sl->state == RC_PF_INFLIGHT ||
							     sl->state == RC_PF_COMPLETE) &&
							    sl->lba == want_lba) {
								already_have = true;
								break;
							}
							if (sl->state == RC_PF_IDLE && idle_slot < 0)
								idle_slot = (int)ss;
						}
						if (!already_have && idle_slot >= 0 &&
						    want_lba + rc_volume_stripe_sectors <=
						    get_capacity(rc_volume_disk)) {
							struct rc_volume_prefetch_slot *sl =
								&pf->slots[idle_slot];
							int nm;
							u64 nphys;
							rc_volume_map_lba(want_lba, &nm, &nphys);
							sl->state = RC_PF_INFLIGHT;
							sl->lba = want_lba;
							sl->sectors = rc_volume_stripe_sectors;
							sl->member = nm;
							sl->served_sectors = 0;
							rc_volume_prefetch_submit(hctx_idx,
										  (unsigned int)idle_slot,
										  pf, nphys);
						}
					}
				}
				spin_unlock_irqrestore(&pf->lock, flags);
				rc_cache_dec_inflight(pdu);
				blk_mq_end_request(req, BLK_STS_OK);
				return BLK_STS_OK;
			}
		} else if (op != REQ_OP_READ && hctx_idx < RC_VOLUME_MAX_HCTX) {
			rc_volume_prefetch_invalidate(hctx_idx);
		}
	}

	rc_volume_map_lba(pos, &member_idx, &phys_lba);
	pdu->member_idx = member_idx;
	atomic_set(&pdu->members_pending, 1);

	if (op == REQ_OP_DISCARD) {
		rc_volume_build_discard_sqe(&cmd, member_idx, tag,
					    phys_lba, nr_sectors);
		blk_mq_start_request(req);
		rc_nvme_io_submit(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num],
				  &cmd);
		return BLK_STS_OK;
	}

	/* READ/WRITE: dma_map_sg the request's bvecs onto the target member,
	 * then build the SQE with PRPs derived from the mapped scatterlist. */
	st = rc_volume_map_request_sg(req, pdu, member_idx);
	if (st != BLK_STS_OK) {
		blk_mq_start_request(req);
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, st);
		return BLK_STS_OK;
	}

	rc_volume_build_io_sqe(&cmd, member_idx, tag,
			       op == REQ_OP_WRITE ? RC_NVME_NVM_OP_WRITE
						  : RC_NVME_NVM_OP_READ,
			       phys_lba, (u16)(nr_sectors - 1),
			       (req->cmd_flags & REQ_FUA) != 0,
			       pdu->sg, pdu->nents);

	/* Must call blk_mq_start_request BEFORE submitting — otherwise the
	 * ISR could complete the request before blk-mq considers it started. */
	blk_mq_start_request(req);
	rc_nvme_io_submit(rc_volume_members[member_idx]->ctx.nvme.io_queues[hctx->queue_num],
			  &cmd);

	/* Post-dispatch: if this READ continues a sequential stream and is
	 * stripe-sized + stripe-aligned, speculatively issue the next stripe
	 * to the OTHER member.  The result lands in the per-hctx prefetch
	 * buffer; the next matching app READ skips the device entirely.
	 * No-op if a prefetch is still in flight or no stream is detected. */
	if (rc_volume_prefetch_enabled && op == REQ_OP_READ) {
		unsigned int hctx_idx = hctx->queue_num;
		if (hctx_idx < RC_VOLUME_MAX_HCTX) {
			struct rc_volume_prefetch *pf =
				&rc_volume_prefetch_state[hctx_idx];
			unsigned long flags;

			spin_lock_irqsave(&pf->lock, flags);
			{
				sector_t prev_last_lba = pf->last_lba;
				u32 prev_last_sectors = pf->last_sectors;
				bool was_seq = (prev_last_sectors &&
						pos == prev_last_lba + prev_last_sectors);
				pf->last_lba = pos;
				pf->last_sectors = nr_sectors;
				if (was_seq && rc_volume_stripe_sectors) {
					/* On a miss, fill IDLE slots aiming 1 and
					 * 2 stripes ahead so the upcoming app reads
					 * find data either complete or in flight. */
					u64 cur_stripe = pos / rc_volume_stripe_sectors;
					unsigned int ahead;
					for (ahead = 1; ahead <= 2; ahead++) {
						u64 want_lba = (cur_stripe + ahead)
							* rc_volume_stripe_sectors;
						int idle_slot = -1;
						bool already_have = false;
						unsigned int ss;
						for (ss = 0; ss < RC_VOLUME_PREFETCH_SLOTS; ss++) {
							struct rc_volume_prefetch_slot *sl = &pf->slots[ss];
							if ((sl->state == RC_PF_INFLIGHT ||
							     sl->state == RC_PF_COMPLETE) &&
							    sl->lba == want_lba) {
								already_have = true;
								break;
							}
							if (sl->state == RC_PF_IDLE && idle_slot < 0)
								idle_slot = (int)ss;
						}
						if (already_have || idle_slot < 0)
							continue;
						if (want_lba + rc_volume_stripe_sectors >
						    get_capacity(rc_volume_disk))
							continue;
						{
							struct rc_volume_prefetch_slot *sl =
								&pf->slots[idle_slot];
							int nm;
							u64 nphys;
							rc_volume_map_lba(want_lba, &nm, &nphys);
							sl->state = RC_PF_INFLIGHT;
							sl->lba = want_lba;
							sl->sectors = rc_volume_stripe_sectors;
							sl->member = nm;
							sl->served_sectors = 0;
							pf->misses++;
							rc_volume_prefetch_submit(hctx_idx,
										  (unsigned int)idle_slot,
										  pf, nphys);
						}
					}
				} else if (!was_seq) {
					pf->misses++;
				}
			}
			spin_unlock_irqrestore(&pf->lock, flags);
		}
	}
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

	/* Cache-fill waiter: memcpy from cache buffer, then transition the
	 * entry to VALID for future readers.  No dma_unmap (we didn't map). */
	if (pdu->cache_entry) {
		struct rc_cache_entry *e = pdu->cache_entry;
		unsigned long flags;

		if (e->status) {
			spin_lock_irqsave(&rc_cache_lock, flags);
			if (!hlist_unhashed(&e->hash_node)) {
				hlist_del(&e->hash_node);
				INIT_HLIST_NODE(&e->hash_node);
				list_move(&e->lru_node, &rc_cache_lru);
			}
			e->state = RC_CE_FREE;
			spin_unlock_irqrestore(&rc_cache_lock, flags);
			pdu->cache_entry = NULL;
			rc_cache_dec_inflight(pdu);
			blk_mq_end_request(req, BLK_STS_IOERR);
			return;
		}

		{
			sector_t pos = blk_rq_pos(req);
			void *src = (u8 *)e->buf_va[e->member] +
				    (u32)(pos - e->lba) * 512u;
			rc_volume_prefetch_copy_out(req, src);
		}
		spin_lock_irqsave(&rc_cache_lock, flags);
		if (e->state == RC_CE_LOADING) {
			if (e->abandoned) {
				/* Write came in mid-fill; the data we just
				 * memcpy'd into the bio was the device's
				 * pre-write read, which is the correct result
				 * for this READ.  Drop the entry rather than
				 * publishing so subsequent readers go back to
				 * the device. */
				e->state = RC_CE_FREE;
				e->abandoned = false;
			} else {
				e->state = RC_CE_VALID;
				list_move_tail(&e->lru_node, &rc_cache_lru);
			}
		}
		spin_unlock_irqrestore(&rc_cache_lock, flags);
		pdu->cache_entry = NULL;
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, BLK_STS_OK);
		return;
	}

	rc_volume_unmap_request_sg(pdu);

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
		rc_cache_dec_inflight(pdu);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return;
	}

	rc_cache_dec_inflight(pdu);
	blk_mq_end_request(req, BLK_STS_OK);
}

struct rc_volume_drain_ctx {
	unsigned int dead_mask;	/* bit i set => member i is dead */
};

/* blk_mq_tagset_busy_iter callback.  For each in-flight request, decide
 * whether it can no longer make progress (it targeted a now-dead adapter,
 * or it's a FLUSH that needs every member and at least one is dead) and
 * complete it with the dead-controller sentinel if so.  blk-mq makes
 * blk_mq_complete_request atomic against the ISR's competing call, so a
 * naturally-arriving CQE during this iteration races safely. */
static bool rc_volume_drain_iter(struct request *req, void *priv)
{
	struct rc_volume_drain_ctx *ctx = priv;
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	bool kill;

	if (pdu->op == REQ_OP_FLUSH)
		kill = ctx->dead_mask != 0;
	else if (pdu->member_idx >= 0 &&
		 pdu->member_idx < RC_VOLUME_MAX_MEMBERS)
		kill = (ctx->dead_mask & BIT(pdu->member_idx)) != 0;
	else
		kill = false;

	if (kill && !blk_mq_request_completed(req)) {
		pdu->sc_sct = RC_VOLUME_SC_DEAD;
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

	if (pdu->op == REQ_OP_FLUSH) {
		for (i = 0; i < rc_volume_member_count; i++)
			rc_nvme_schedule_auto_reset(rc_volume_members[i]);
	} else if (pdu->member_idx >= 0 &&
		   pdu->member_idx < rc_volume_member_count) {
		rc_nvme_schedule_auto_reset(rc_volume_members[pdu->member_idx]);
	}
}

/* Build the dead-member mask and run the drain iterator.  No-op if no
 * member is currently flagged dead or the disk hasn't been created yet. */
static void rc_volume_drain_dead(void)
{
	struct rc_volume_drain_ctx ctx = {};
	int i;

	if (!rc_volume_disk)
		return;
	for (i = 0; i < rc_volume_member_count; i++)
		if (READ_ONCE(rc_volume_members[i]->ctx.nvme.dead))
			ctx.dead_mask |= BIT(i);
	if (!ctx.dead_mask)
		return;
	blk_mq_tagset_busy_iter(&rc_volume_tagset, rc_volume_drain_iter, &ctx);
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
	int i;

	if (blk_mq_request_completed(req))
		return BLK_EH_DONE;

	if (pdu->op == REQ_OP_FLUSH) {
		for (i = 0; i < rc_volume_member_count; i++)
			if (rc_nvme_check_dead(rc_volume_members[i]))
				any_dead = true;
	} else if (pdu->member_idx >= 0 &&
		   pdu->member_idx < rc_volume_member_count) {
		any_dead = rc_nvme_check_dead(rc_volume_members[pdu->member_idx]);
	}

	if (any_dead) {
		rc_printk(RC_ERROR,
			  "rc_volume_timeout: tag=%u op=%u: member dead — draining in-flight\n",
			  req->tag, pdu->op);
		rc_volume_drain_dead();
		rc_volume_schedule_auto_reset_for_req(pdu);
		if (!blk_mq_request_completed(req)) {
			/* Drain should have caught it; if it didn't (e.g.,
			 * member_idx out of range for some reason) the
			 * direct end path bypasses .complete, so unmap here. */
			rc_volume_unmap_request_sg(pdu);
			rc_cache_dec_inflight(pdu);
			blk_mq_end_request(req, BLK_STS_IOERR);
		}
		return BLK_EH_DONE;
	}

	rc_printk(RC_WARN,
		  "rc_volume_timeout: tag=%u op=%u: issuing NVMe Abort\n",
		  req->tag, pdu->op);
	if (pdu->op == REQ_OP_FLUSH) {
		for (i = 0; i < rc_volume_member_count; i++)
			(void)rc_nvme_abort(rc_volume_members[i],
					    RC_NVME_IO_QID, (u16)req->tag);
	} else if (pdu->member_idx >= 0 &&
		   pdu->member_idx < rc_volume_member_count) {
		(void)rc_nvme_abort(rc_volume_members[pdu->member_idx],
				    RC_NVME_IO_QID, (u16)req->tag);
	}

	if (blk_mq_request_completed(req))
		return BLK_EH_DONE;

	if (pdu->op == REQ_OP_FLUSH) {
		for (i = 0; i < rc_volume_member_count; i++)
			WRITE_ONCE(rc_volume_members[i]->ctx.nvme.dead, true);
	} else if (pdu->member_idx >= 0 &&
		   pdu->member_idx < rc_volume_member_count) {
		WRITE_ONCE(rc_volume_members[pdu->member_idx]->ctx.nvme.dead, true);
	}
	rc_volume_drain_dead();
	rc_volume_schedule_auto_reset_for_req(pdu);
	if (!blk_mq_request_completed(req)) {
		rc_volume_unmap_request_sg(pdu);
		rc_cache_dec_inflight(pdu);
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
	if (rc_volume_member_count > 0) {
		struct device *dev = &rc_volume_members[0]->pdev->dev;
		blk_mq_map_hw_queues(&set->map[HCTX_TYPE_DEFAULT], dev, 1);
	} else {
		blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);
	}
}

/* ============================== prefetch ============================== */

/* Free all prefetch buffers + PRP pages.  Idempotent: skips slots that
 * never got allocated.  Walks every (hctx, slot, member) tuple. */
static void rc_volume_prefetch_free_all(void)
{
	int h, m;
	unsigned int s;

	for (h = 0; h < (int)RC_VOLUME_MAX_HCTX; h++) {
		struct rc_volume_prefetch *pf = &rc_volume_prefetch_state[h];
		for (s = 0; s < RC_VOLUME_PREFETCH_SLOTS; s++) {
			struct rc_volume_prefetch_slot *sl = &pf->slots[s];
			for (m = 0; m < rc_volume_member_count; m++) {
				struct device *dev;
				if (!rc_volume_members[m])
					continue;
				dev = &rc_volume_members[m]->pdev->dev;
				if (sl->buf_va[m]) {
					dma_free_coherent(dev, RC_VOLUME_PREFETCH_BYTES,
							  sl->buf_va[m], sl->buf_pa[m]);
					sl->buf_va[m] = NULL;
					sl->buf_pa[m] = 0;
				}
				if (sl->prp_va[m]) {
					dma_free_coherent(dev, PAGE_SIZE,
							  sl->prp_va[m], sl->prp_pa[m]);
					sl->prp_va[m] = NULL;
					sl->prp_pa[m] = 0;
				}
			}
			sl->state = RC_PF_IDLE;
		}
		pf->last_lba = ~(sector_t)0;
		pf->last_sectors = 0;
	}
}

/* Allocate per-(hctx,member) prefetch DMA buffer + PRP list page, and
 * fill the PRP list with the buffer's per-member DMA addresses.  The
 * 1 MiB buffer is allocated with dma_alloc_coherent (so the kernel
 * memcpy from KVA is safe with no extra dma_sync needed) and the PRP
 * list mirrors what rc_volume_build_prp would have produced for that
 * physical layout. */
static int rc_volume_prefetch_alloc_all(unsigned int nr_hw)
{
	int h, m;
	unsigned int s, p;

	if (nr_hw > RC_VOLUME_MAX_HCTX)
		nr_hw = RC_VOLUME_MAX_HCTX;

	for (h = 0; h < (int)nr_hw; h++) {
		struct rc_volume_prefetch *pf = &rc_volume_prefetch_state[h];
		spin_lock_init(&pf->lock);
		pf->last_lba = ~(sector_t)0;
		pf->last_sectors = 0;

		for (s = 0; s < RC_VOLUME_PREFETCH_SLOTS; s++) {
			struct rc_volume_prefetch_slot *sl = &pf->slots[s];
			sl->state = RC_PF_IDLE;
			atomic_set(&sl->cmds_pending, 0);
			sl->served_sectors = 0;
			for (m = 0; m < rc_volume_member_count; m++) {
				struct device *dev = &rc_volume_members[m]->pdev->dev;

				sl->buf_va[m] = dma_alloc_coherent(dev,
								   RC_VOLUME_PREFETCH_BYTES,
								   &sl->buf_pa[m],
								   GFP_KERNEL);
				if (!sl->buf_va[m]) {
					rc_printk(RC_ERROR,
						  "rc_volume_prefetch_alloc_all: hctx %d slot %u member %d buf alloc failed\n",
						  h, s, m);
					rc_volume_prefetch_free_all();
					return -ENOMEM;
				}
				sl->prp_va[m] = dma_alloc_coherent(dev, PAGE_SIZE,
								   &sl->prp_pa[m],
								   GFP_KERNEL);
				if (!sl->prp_va[m]) {
					rc_volume_prefetch_free_all();
					return -ENOMEM;
				}
				for (p = 0; p < RC_VOLUME_PREFETCH_PAGES - 1; p++)
					sl->prp_va[m][p] =
						cpu_to_le64(sl->buf_pa[m] + (p + 1) * PAGE_SIZE);
			}
		}
	}
	return 0;
}

/* ============================ Read cache impl ===========================
 *
 * See header comment near struct rc_cache_entry.  All helpers below
 * assume rc_cache_lock is held by the caller unless noted. */

static inline u32 rc_cache_bucket_for(sector_t lba)
{
	return hash_64((u64)lba, RC_VOLUME_CACHE_HASH_BITS) & RC_VOLUME_CACHE_HASH_MASK;
}

/* Check ghost cache: if the stripe LBA was recently seen, return true
 * (and leave it marked).  Otherwise mark it seen and return false.
 * Caller hold rc_cache_lock. */
static bool rc_ghost_check_and_mark(sector_t stripe_lba)
{
	u32 h;

	if (!rc_ghost_table || !stripe_lba)
		return false;	/* never promote if disabled or sentinel */
	h = (u32)(hash_64((u64)stripe_lba, RC_GHOST_HASH_BITS) & RC_GHOST_HASH_MASK);
	if (rc_ghost_table[h] == stripe_lba)
		return true;
	rc_ghost_table[h] = stripe_lba;
	return false;
}

/* Look up a VALID entry for stripe_lba.  Returns NULL if not present or
 * the entry is still LOADING.  Touch-on-hit (move to MRU). */
static struct rc_cache_entry *rc_cache_lookup_valid(sector_t stripe_lba)
{
	u32 h = rc_cache_bucket_for(stripe_lba);
	struct rc_cache_entry *e;

	hlist_for_each_entry(e, &rc_cache_buckets[h], hash_node) {
		if (e->state == RC_CE_VALID && e->lba == stripe_lba) {
			list_move_tail(&e->lru_node, &rc_cache_lru);
			return e;
		}
	}
	return NULL;
}

/* Look up any entry (VALID or LOADING) for stripe_lba.  Used to suppress
 * duplicate fills when one is already in flight. */
static bool rc_cache_has_or_loading(sector_t stripe_lba)
{
	u32 h = rc_cache_bucket_for(stripe_lba);
	struct rc_cache_entry *e;

	hlist_for_each_entry(e, &rc_cache_buckets[h], hash_node) {
		if (e->lba == stripe_lba)
			return true;
	}
	return false;
}

/* Allocate a FREE entry.  If none free, evict LRU VALID entry.  Returns
 * NULL only if every entry is currently LOADING (unusual). */
static struct rc_cache_entry *rc_cache_alloc_entry(void)
{
	struct rc_cache_entry *e;

	list_for_each_entry(e, &rc_cache_lru, lru_node) {
		if (e->state == RC_CE_FREE) {
			list_del(&e->lru_node);
			return e;
		}
	}
	list_for_each_entry(e, &rc_cache_lru, lru_node) {
		if (e->state == RC_CE_VALID) {
			hlist_del(&e->hash_node);
			list_del(&e->lru_node);
			rc_cache_evictions++;
			e->state = RC_CE_FREE;
			return e;
		}
	}
	return NULL;
}

/* Free all cache resources.  Idempotent. */
static void rc_cache_teardown(void)
{
	u32 i, m;

	rc_printk(RC_NOTE,
		  "rc_cache_teardown: hits=%llu misses=%llu evictions=%llu\n",
		  (unsigned long long)rc_cache_hits,
		  (unsigned long long)rc_cache_misses,
		  (unsigned long long)rc_cache_evictions);

	if (!rc_cache_entries)
		goto free_hash;

	for (i = 0; i < rc_cache_nr_entries; i++) {
		struct rc_cache_entry *e = &rc_cache_entries[i];
		for (m = 0; m < (u32)rc_volume_member_count; m++) {
			struct device *dev;
			if (!rc_volume_members[m])
				continue;
			dev = &rc_volume_members[m]->pdev->dev;
			if (e->buf_va[m]) {
				dma_free_coherent(dev, RC_VOLUME_PREFETCH_BYTES,
						  e->buf_va[m], e->buf_pa[m]);
				e->buf_va[m] = NULL;
				e->buf_pa[m] = 0;
			}
			if (e->prp_va[m]) {
				dma_free_coherent(dev, PAGE_SIZE,
						  e->prp_va[m], e->prp_pa[m]);
				e->prp_va[m] = NULL;
				e->prp_pa[m] = 0;
			}
		}
	}
	kfree(rc_cache_entries);
	rc_cache_entries = NULL;
	rc_cache_nr_entries = 0;

free_hash:
	if (rc_cache_buckets) {
		kfree(rc_cache_buckets);
		rc_cache_buckets = NULL;
	}
	if (rc_ghost_table) {
		kvfree(rc_ghost_table);
		rc_ghost_table = NULL;
	}
	INIT_LIST_HEAD(&rc_cache_lru);
}

/* Allocate cache pool: nr entries with per-member DMA buffer + PRP list. */
static int rc_cache_init(u32 nr_entries)
{
	u32 i, m, p;

	spin_lock_init(&rc_cache_lock);
	INIT_LIST_HEAD(&rc_cache_lru);
	rc_cache_hits = 0;
	rc_cache_misses = 0;
	rc_cache_evictions = 0;

	if (nr_entries == 0)
		return 0;

	/* Clamp to the CID-space we actually have.  The cache-fill CID is
	 *
	 *   cid = RC_VOLUME_CACHE_CID_BASE
	 *         | ((entry_idx & 0xfff) << 1)
	 *         | (i & 1)
	 *
	 * so we have 12 bits of entry_idx = 4096 entries max.  Beyond that
	 * the mask wraps and entries N and N+4096 would share a CID, routing
	 * the wrong completion to the wrong entry — silent data corruption.
	 * Clamp loudly so the user can see they've over-asked. */
	if (nr_entries > RC_VOLUME_CACHE_MAX_ENTRIES) {
		rc_printk(RC_WARN,
			  "rc_cache_init: cache_entries=%u exceeds CID-space limit of %u — clamping\n",
			  nr_entries, RC_VOLUME_CACHE_MAX_ENTRIES);
		nr_entries = RC_VOLUME_CACHE_MAX_ENTRIES;
	}

	rc_ghost_table = kvzalloc(RC_GHOST_HASH_SIZE * sizeof(sector_t),
				  GFP_KERNEL);
	if (!rc_ghost_table)
		return -ENOMEM;

	rc_cache_buckets = kcalloc(RC_VOLUME_CACHE_HASH_SIZE,
				sizeof(*rc_cache_buckets), GFP_KERNEL);
	if (!rc_cache_buckets)
		return -ENOMEM;
	for (i = 0; i < RC_VOLUME_CACHE_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&rc_cache_buckets[i]);

	rc_cache_entries = kcalloc(nr_entries, sizeof(*rc_cache_entries),
				   GFP_KERNEL);
	if (!rc_cache_entries) {
		kfree(rc_cache_buckets);
		rc_cache_buckets = NULL;
		return -ENOMEM;
	}
	rc_cache_nr_entries = nr_entries;

	for (i = 0; i < nr_entries; i++) {
		struct rc_cache_entry *e = &rc_cache_entries[i];
		e->state = RC_CE_FREE;
		atomic_set(&e->cmds_pending, 0);
		INIT_HLIST_NODE(&e->hash_node);
		INIT_LIST_HEAD(&e->lru_node);
		list_add_tail(&e->lru_node, &rc_cache_lru);

		for (m = 0; m < (u32)rc_volume_member_count; m++) {
			struct device *dev = &rc_volume_members[m]->pdev->dev;
			e->buf_va[m] = dma_alloc_coherent(dev,
							  RC_VOLUME_PREFETCH_BYTES,
							  &e->buf_pa[m],
							  GFP_KERNEL);
			if (!e->buf_va[m]) {
				rc_printk(RC_ERROR,
					  "rc_cache_init: entry %u member %u buf alloc failed\n",
					  i, m);
				rc_cache_teardown();
				return -ENOMEM;
			}
			e->prp_va[m] = dma_alloc_coherent(dev, PAGE_SIZE,
							  &e->prp_pa[m],
							  GFP_KERNEL);
			if (!e->prp_va[m]) {
				rc_cache_teardown();
				return -ENOMEM;
			}
			for (p = 0; p < RC_VOLUME_PREFETCH_PAGES - 1; p++)
				e->prp_va[m][p] =
					cpu_to_le64(e->buf_pa[m] + (p + 1) * PAGE_SIZE);
		}
	}
	rc_printk(RC_NOTE,
		  "rc_cache_init: %u entries × 1 MiB × %d members = %u MiB cache\n",
		  nr_entries, rc_volume_member_count,
		  nr_entries * (1u) * rc_volume_member_count);
	return 0;
}

/* Submit the multi-NVMe-cmd fill for a cache entry (1 MiB → 2 cmds at MDTS=7).
 * Caller must have set entry->state = RC_CE_LOADING and entry->member.
 * Caller holds rc_cache_lock during dispatch (kept short — submission is fast). */
static void rc_cache_submit_fill(unsigned int hctx_idx,
				 u32 entry_idx,
				 sector_t stripe_lba,
				 u64 phys_lba)
{
	struct rc_cache_entry *e = &rc_cache_entries[entry_idx];
	int m = e->member;
	u32 cmds = RC_VOLUME_PREFETCH_BYTES / (RC_VOLUME_CACHE_CMD_SECTORS * 512u);
	u32 i;

	atomic_set(&e->cmds_pending, (int)cmds);
	e->status = 0;
	e->lba = stripe_lba;

	for (i = 0; i < cmds; i++) {
		struct rc_nvme_sqe cmd;
		u32 sec_in    = i * RC_VOLUME_CACHE_CMD_SECTORS;
		u32 byte_off  = sec_in * 512u;
		u32 page_off  = byte_off / (u32)PAGE_SIZE;
		/* CID = CACHE_BASE | (entry_idx << 1) | cmd_idx.
		 * Distinct per cmd so NVMe doesn't reject as duplicate. */
		u16 cid       = (u16)(RC_VOLUME_CACHE_CID_BASE |
				      ((entry_idx & 0xfff) << 1) | (i & 1));
		u64 cmd_slba  = phys_lba + sec_in;

		(void)i;
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_NVM_OP_READ;
		cmd.cid   = cpu_to_le16(cid);
		cmd.nsid  = cpu_to_le32(1);
		cmd.cdw10 = cpu_to_le32((u32)(cmd_slba & 0xffffffff));
		cmd.cdw11 = cpu_to_le32((u32)(cmd_slba >> 32));
		cmd.cdw12 = cpu_to_le32(RC_VOLUME_CACHE_CMD_SECTORS - 1);
		cmd.prp1  = cpu_to_le64(e->buf_pa[m] + byte_off);
		cmd.prp2  = cpu_to_le64(e->prp_pa[m] + page_off * sizeof(__le64));

		rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx_idx],
				  &cmd);
	}
}

/* ISR helper for cache fill completion.  Same shape as the prefetch ISR:
 * decrement cmds_pending, transition LOADING → VALID on the last cmd,
 * insert into hash + LRU. */
static void rc_cache_isr(unsigned int entry_idx, u16 status)
{
	struct rc_cache_entry *e;
	struct request *waiter = NULL;
	unsigned long flags;
	bool last;

	if (entry_idx >= rc_cache_nr_entries)
		return;
	e = &rc_cache_entries[entry_idx];

	spin_lock_irqsave(&rc_cache_lock, flags);
	if (status)
		e->status = status;
	last = atomic_dec_and_test(&e->cmds_pending);
	if (last) {
		if (e->state == RC_CE_LOADING) {
			/* Defer the LOADING → final-state transition until the
			 * waiter's .complete handler has finished memcpy'ing
			 * out of the buffer — otherwise an evict could free
			 * the buffer mid-copy.  ISR just schedules the
			 * waiter; .complete handles the state transition
			 * (LOADING → VALID normally; LOADING → FREE if the
			 * entry was abandoned mid-fill by a write).
			 *
			 * If there is no waiter (rare — concurrent invalidate
			 * already extracted it, or fill was issued from a
			 * non-blocking path), do the final transition here. */
			waiter = e->waiter;
			e->waiter = NULL;
			if (!waiter) {
				if (e->abandoned) {
					e->state = RC_CE_FREE;
					e->abandoned = false;
				} else {
					e->state = RC_CE_VALID;
					list_move_tail(&e->lru_node, &rc_cache_lru);
				}
			}
		}
	}
	spin_unlock_irqrestore(&rc_cache_lock, flags);

	if (waiter)
		blk_mq_complete_request(waiter);
}

/* Invalidate every cache entry overlapping [pos, pos+nr_sectors).
 * VALID entries are dropped immediately.  LOADING entries are marked
 * for drop on completion (state stays LOADING; rc_cache_isr will see
 * the lba mismatch... actually we set state = FREE so isr drops it). */
static void rc_cache_invalidate_range(sector_t pos, u32 nr_sectors)
{
	u32 i;
	unsigned long flags;

	if (!rc_cache_entries)
		return;

	spin_lock_irqsave(&rc_cache_lock, flags);
	for (i = 0; i < rc_cache_nr_entries; i++) {
		struct rc_cache_entry *e = &rc_cache_entries[i];
		if (e->state == RC_CE_FREE)
			continue;
		if (e->lba + (RC_VOLUME_PREFETCH_BYTES / 512u) <= pos ||
		    pos + nr_sectors <= e->lba)
			continue;
		/* Overlap.  Remove from hash so subsequent lookups miss.  For
		 * a VALID entry we can also free it immediately.  For LOADING
		 * we have to leave state==LOADING (NVMe cmds are still in
		 * flight referencing this entry's CID / buf_pa) — instead mark
		 * it abandoned so the ISR transitions it to FREE rather than
		 * VALID on completion, dropping the now-stale fill data.  The
		 * waiter still rides the fill to completion and gets the data
		 * it read from the device, which is correct: the WRITE that
		 * triggered this invalidation hadn't completed when the fill
		 * was issued, so the fill's data is a valid pre-write read. */
		hlist_del(&e->hash_node);
		INIT_HLIST_NODE(&e->hash_node);
		list_move(&e->lru_node, &rc_cache_lru);
		rc_cache_evictions++;
		if (e->state == RC_CE_LOADING)
			e->abandoned = true;
		else
			e->state = RC_CE_FREE;
	}
	spin_unlock_irqrestore(&rc_cache_lock, flags);
}

/* Submit a multi-command prefetch.  Caller must hold pf->lock AND have
 * already set state = INFLIGHT and recorded {lba, sectors, member}.
 * Splits pf->sectors into chunks of RC_VOLUME_PREFETCH_CMD_SECTORS each
 * (= 1024 sectors = 512 KiB per cmd, matching the controller MDTS=7
 * limit of 2^7 × 4 KiB = 512 KiB), and submits one NVMe READ per chunk.
 *
 * CID scheme: bit 8 marks a prefetch CID.  Bits 7..4 carry hctx_idx
 * (0..15).  Bits 3..0 carry cmd index within this prefetch (0..15).
 * The ISR uses the hctx_idx to look up the slot and atomic_dec_and_test
 * cmds_pending across cmds to mark COMPLETE.
 *
 * PRP list layout (set up once at alloc time): prp_va[m][i] holds the
 * IOVA of buffer page i+1.  For cmd 0 (pages 0..127): PRP1 = page 0,
 * PRP2 = &prp_va[m][0] (entries 0..126 cover pages 1..127).  For cmd 1
 * (pages 128..255): PRP1 = page 128, PRP2 = &prp_va[m][128] (entries
 * 128..254 cover pages 129..255). */
static void rc_volume_prefetch_submit(unsigned int hctx_idx,
				      unsigned int slot_idx,
				      struct rc_volume_prefetch *pf,
				      u64 phys_lba)
{
	struct rc_volume_prefetch_slot *sl = &pf->slots[slot_idx];
	int m = sl->member;
	u32 cmds = (sl->sectors + RC_VOLUME_PREFETCH_CMD_SECTORS - 1) /
		   RC_VOLUME_PREFETCH_CMD_SECTORS;
	u32 i;

	atomic_set(&sl->cmds_pending, cmds);
	sl->status = 0;

	for (i = 0; i < cmds; i++) {
		struct rc_nvme_sqe cmd;
		u32 sec_in = i * RC_VOLUME_PREFETCH_CMD_SECTORS;
		u32 sec_remain = sl->sectors - sec_in;
		u32 sec_this = sec_remain < RC_VOLUME_PREFETCH_CMD_SECTORS
			       ? sec_remain
			       : RC_VOLUME_PREFETCH_CMD_SECTORS;
		u32 byte_off = sec_in * 512u;
		u32 page_off = byte_off / (u32)PAGE_SIZE;
		u16 cid = (u16)(RC_VOLUME_PREFETCH_CID_BASE |
				(slot_idx << 7) |
				(hctx_idx << 4) | i);
		u64 cmd_lba = phys_lba + sec_in;

		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_NVM_OP_READ;
		cmd.cid   = cpu_to_le16(cid);
		cmd.nsid  = cpu_to_le32(1);
		cmd.cdw10 = cpu_to_le32((u32)(cmd_lba & 0xffffffff));
		cmd.cdw11 = cpu_to_le32((u32)(cmd_lba >> 32));
		cmd.cdw12 = cpu_to_le32(sec_this - 1);
		cmd.prp1  = cpu_to_le64(sl->buf_pa[m] + byte_off);
		cmd.prp2  = cpu_to_le64(sl->prp_pa[m] + page_off * sizeof(__le64));

		pf->issued++;
		rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx_idx],
				  &cmd);
	}
}

/* ISR helper: handle a prefetch CID completion.  Called from
 * rc_nvme_io_queue_irq with q->lock held.  Records any error status
 * and decrements cmds_pending; the last command in transitions
 * INFLIGHT -> COMPLETE.  If the dispatch path marked the prefetch as
 * discarded (write invalidation), drop the buffer back to IDLE on the
 * last completion without retaining the data. */
static void rc_volume_prefetch_isr(unsigned int hctx_idx,
				   unsigned int slot_idx, u16 status)
{
	struct rc_volume_prefetch *pf;
	struct rc_volume_prefetch_slot *sl;
	unsigned long flags;
	bool last;

	if (hctx_idx >= RC_VOLUME_MAX_HCTX ||
	    slot_idx >= RC_VOLUME_PREFETCH_SLOTS)
		return;
	pf = &rc_volume_prefetch_state[hctx_idx];
	sl = &pf->slots[slot_idx];

	spin_lock_irqsave(&pf->lock, flags);
	if (status)
		sl->status = status;	/* sticky on first error */
	last = atomic_dec_and_test(&sl->cmds_pending);
	if (last) {
		if (sl->state == RC_PF_INFLIGHT) {
			sl->state = RC_PF_COMPLETE;
			sl->served_sectors = 0;
		} else {
			sl->state = RC_PF_IDLE;
			pf->discarded++;
		}
	}
	spin_unlock_irqrestore(&pf->lock, flags);
}

/* Copy data from the prefetch buffer into the request's bio pages.
 * Called from queue_rq (process context) on a cache hit.  The caller
 * is responsible for ensuring src remains valid for the duration of
 * the copy — true because we transitioned state to IDLE only AFTER
 * extracting src and pf->state IDLE means no new prefetch can land
 * (single-threaded per hctx). */
static void rc_volume_prefetch_copy_out(struct request *req, void *src)
{
	struct bio *bio;
	size_t off = 0;

	__rq_for_each_bio(bio, req) {
		struct bio_vec bv;
		struct bvec_iter iter;

		bio_for_each_segment(bv, bio, iter) {
			void *dst = kmap_local_page(bv.bv_page) + bv.bv_offset;
			memcpy(dst, (u8 *)src + off, bv.bv_len);
			kunmap_local(dst - bv.bv_offset);
			off += bv.bv_len;
		}
	}
}

/* Invalidate a slot — used when a write/discard targets the same
 * volume region or the stream goes non-sequential.  If a prefetch is
 * in flight we can't free the buffer yet (the controller may still
 * DMA into it); flag it discarded so the ISR drops it on completion. */
static void rc_volume_prefetch_invalidate(unsigned int hctx_idx)
{
	struct rc_volume_prefetch *pf;
	unsigned long flags;
	unsigned int s;

	if (hctx_idx >= RC_VOLUME_MAX_HCTX)
		return;
	pf = &rc_volume_prefetch_state[hctx_idx];

	spin_lock_irqsave(&pf->lock, flags);
	for (s = 0; s < RC_VOLUME_PREFETCH_SLOTS; s++) {
		struct rc_volume_prefetch_slot *sl = &pf->slots[s];
		if (sl->state == RC_PF_COMPLETE) {
			sl->state = RC_PF_IDLE;
			pf->discarded++;
		}
		/* INFLIGHT slots: ISR will hand them back to COMPLETE
		 * normally; an out-of-range app read will not match and
		 * the slot will be reused on the next prefetch. */
	}
	spin_unlock_irqrestore(&pf->lock, flags);
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

/* Allocate the gendisk + tagset, set up sizes, expose /dev/rcraid0. */
static int rc_volume_create_disk(void)
{
	struct rc_adapter *m0 = rc_volume_members[0];
	u64 total_sectors;
	int i, ret;

	/* Per-tag, per-member PRP-list buffer (PAGE_SIZE each).  Tag = NVMe
	 * CID; the same tag may route to different members on different
	 * requests, so each member needs its own pool.  Data buffers no
	 * longer exist here — hardware DMAs directly to the bio's pages via
	 * dma_map_sg in rc_volume_queue_rq. */
	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		u32 t;

		for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
			rc_volume_prp_va[i][t] =
				dma_alloc_coherent(dev, PAGE_SIZE,
						   &rc_volume_prp_pa[i][t],
						   GFP_KERNEL);
			if (!rc_volume_prp_va[i][t]) {
				ret = -ENOMEM;
				goto err_free_dma;
			}
		}
	}

	/* nr_hw_queues = min(member->nr_io_queues) across all members.  Each
	 * hctx i routes to io_queues[i] on every member, so we can't ask
	 * blk-mq for more hctxs than the smallest member supports.  All
	 * T700s grant 128 (we cap at RC_NVME_IO_QUEUE_TARGET=4) so this
	 * just resolves to 4 in practice, but the min is a future-proofing
	 * safety net. */
	{
		u32 nr_hw = m0->ctx.nvme.nr_io_queues;
		for (i = 1; i < rc_volume_member_count; i++) {
			u32 m = rc_volume_members[i]->ctx.nvme.nr_io_queues;
			if (m < nr_hw)
				nr_hw = m;
		}
		if (nr_hw == 0)
			nr_hw = 1;

		memset(&rc_volume_tagset, 0, sizeof(rc_volume_tagset));
		rc_volume_tagset.ops          = &rc_volume_mq_ops;
		rc_volume_tagset.nr_hw_queues = nr_hw;
		rc_volume_tagset.queue_depth  = RC_VOLUME_QUEUE_DEPTH;
		rc_volume_tagset.numa_node    = NUMA_NO_NODE;
		rc_volume_tagset.cmd_size     = sizeof(struct rc_volume_pdu);
		/* Async dispatch — queue_rq returns BLK_STS_OK immediately
		 * and completion lands via the per-queue ISR → blk_mq_complete
		 * → rc_volume_complete.  No sleeping on the dispatch path → no
		 * BLK_MQ_F_BLOCKING required. */
		rc_volume_tagset.flags        = 0;

		rc_printk(RC_NOTE,
			  "rc_volume_create_disk: blk-mq nr_hw_queues=%u (queue_depth=%u → %u total outstanding)\n",
			  nr_hw, RC_VOLUME_QUEUE_DEPTH,
			  nr_hw * RC_VOLUME_QUEUE_DEPTH);
	}

	ret = blk_mq_alloc_tag_set(&rc_volume_tagset);
	if (ret)
		goto err_free_dma;

	ret = rc_volume_prefetch_alloc_all(rc_volume_tagset.nr_hw_queues);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_volume_create_disk: prefetch alloc failed (%d) — continuing with prefetch disabled\n",
			  ret);
		rc_volume_prefetch_enabled = 0;
		ret = 0;
	}

	if (rc_cache_entries_param > 0) {
		ret = rc_cache_init((u32)rc_cache_entries_param);
		if (ret) {
			rc_printk(RC_WARN,
				  "rc_volume_create_disk: cache init failed (%d) — continuing without cache\n",
				  ret);
			ret = 0;
		}
	}

	{
		struct queue_limits lim = {
			.logical_block_size  = 512,
			.physical_block_size = 512,
			/* PRP list lets us span up to RC_VOLUME_DATA_BYTES per
			 * request.  Caller's bvecs sum into one NVMe READ (or,
			 * for multi-stripe requests, into one NVMe cmd per
			 * member via rc_volume_dispatch_multi_stripe). */
			.max_hw_sectors      = RC_VOLUME_DATA_BYTES / 512,
			.max_segments        = RC_VOLUME_DATA_PAGES,
			.max_segment_size    = PAGE_SIZE,
			/* NVMe PRP semantics: PRP1 may carry an in-page offset,
			 * PRP2 and PRP-list entries must point at page starts.
			 * virt_boundary forces blk-mq to split bvecs so every
			 * segment after the first is page-aligned, which is what
			 * rc_volume_build_prp relies on. */
			.virt_boundary_mask  = PAGE_SIZE - 1,
			/* Don't force blk-mq to split at stripe boundaries — we
			 * fan out multi-stripe requests across members ourselves
			 * in rc_volume_dispatch_multi_stripe, which is cheaper
			 * (1 cmd per member instead of 1 cmd per stripe). */
			.chunk_sectors       = 0,
			/* Advertise that the underlying controllers have a volatile
			 * write cache and that we honour FUA — filesystems will
			 * now route REQ_OP_FLUSH and REQ_FUA writes through. */
			.features            = BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA,
			/* DISCARD via NVMe DSM Deallocate.  Each discard request
			 * is bounded by chunk_sectors (one stripe = one member's
			 * range), and we issue exactly one DSM range per command. */
			.max_hw_discard_sectors  = rc_volume_stripe_sectors,
			.discard_granularity     = 512,
			.max_discard_segments    = 1,
		};

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
	rc_volume_disk->minors      = 1;
	rc_volume_disk->first_minor = 0;
	rc_volume_disk->flags       = GENHD_FL_NO_PART;
	/* Read-only unless the operator explicitly opted in via the module
	 * parameter.  Note the parameter is 0444 (read-only sysfs) so it
	 * can only be set at load time — re-load to switch modes. */
	if (!rc_volume_enable_writes)
		set_disk_ro(rc_volume_disk, 1);
	rc_printk(RC_NOTE,
		  "rc_volume_create_disk: writes %s\n",
		  rc_volume_enable_writes ? "ENABLED" : "disabled (load with enable_writes=1 to allow)");

	ret = add_disk(rc_volume_disk);
	if (ret)
		goto err_put_disk;

	rc_printk(RC_NOTE,
		  "rc_volume_create_disk: /dev/%s up, %llu sectors (%llu MiB, %s)\n",
		  rc_volume_disk->disk_name,
		  (unsigned long long)total_sectors,
		  (unsigned long long)(total_sectors >> 11),
		  rc_volume_enable_writes ? "read-write" : "read-only");
	return 0;

err_put_disk:
	put_disk(rc_volume_disk);
	rc_volume_disk = NULL;
err_free_tagset:
	blk_mq_free_tag_set(&rc_volume_tagset);
err_free_dma:
	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		u32 t;

		for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
			if (rc_volume_prp_va[i][t]) {
				dma_free_coherent(dev, PAGE_SIZE,
						  rc_volume_prp_va[i][t],
						  rc_volume_prp_pa[i][t]);
				rc_volume_prp_va[i][t] = NULL;
			}
		}
	}
	return ret;
}

/* Tear down everything rc_volume_create_disk allocated.  Called from
 * rc_exit().  Safe to call when the disk was never created (state is
 * initialised to NULL/0). */
void rc_volume_teardown(void)
{
	int i;

	mutex_lock(&rc_volume_lock);
	if (rc_volume_disk) {
		del_gendisk(rc_volume_disk);
		put_disk(rc_volume_disk);
		rc_volume_disk = NULL;
		blk_mq_free_tag_set(&rc_volume_tagset);
	}
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++) {
		if (rc_volume_members[i]) {
			struct device *dev = &rc_volume_members[i]->pdev->dev;
			u32 t;

			for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
				if (rc_volume_prp_va[i][t]) {
					dma_free_coherent(dev, PAGE_SIZE,
							  rc_volume_prp_va[i][t],
							  rc_volume_prp_pa[i][t]);
					rc_volume_prp_va[i][t] = NULL;
					rc_volume_prp_pa[i][t] = 0;
				}
			}
		}
		rc_volume_members[i] = NULL;
		rc_volume_member_phys_offset[i] = 0;
	}
	rc_cache_teardown();
	rc_volume_prefetch_free_all();
	rc_volume_member_count = 0;
	rc_volume_stripe_sectors = 0;
	mutex_unlock(&rc_volume_lock);
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
		if (!q)
			continue;
		if (q->sq)
			memset(q->sq, 0,
			       (size_t)q->sq_depth * RC_NVME_SQ_ENTRY_SIZE);
		if (q->cq)
			memset(q->cq, 0,
			       (size_t)q->cq_depth * RC_NVME_CQ_ENTRY_SIZE);
		q->sq_tail  = 0;
		q->cq_head  = 0;
		q->cq_phase = 1;
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
		rc_printk(RC_ERROR,
			  "rc_nvme_reset_controller: %s granted only %u queues, had %u before reset — adapter remains dead\n",
			  pci_name(adapter->pdev),
			  nvme->nr_io_queues, saved_nr_io_queues);
		ret = -EIO;
		goto err_irq_enabled;
	}

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
		cmd.cdw11 = cpu_to_le32(((u32)q->qid << 16) | 0x3u);
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
	init_waitqueue_head(&nvme->admin_cq_wait);
	/* Per-I/O-queue waitqueue + lock are initialized in
	 * rc_nvme_create_one_io_queue when each queue is allocated. */
	mutex_init(&nvme->admin_mutex);
	INIT_WORK(&nvme->auto_reset_work, rc_nvme_auto_reset_fn);

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
	 * NVMe spec this must happen before Create I/O CQ/SQ.  Step 2 of
	 * the multi-queue work just requests RC_NVME_IO_QUEUE_TARGET and
	 * stores the granted count; step 3 will create that many queues. */
	ret = rc_nvme_set_num_queues(adapter, RC_NVME_IO_QUEUE_TARGET);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: Set Features Number of Queues failed (%d) — falling back to 1 I/O queue\n",
			  ret);
		nvme->nr_io_queues = 1;
	}

	/* One I/O queue pair so the upper layer can submit NVM commands.
	 * Step 3 will replace this with a loop creating nvme->nr_io_queues
	 * pairs, each with its own MSI-X vector. */
	ret = rc_nvme_create_io_queues(adapter);
	if (ret) {
		rc_printk(RC_WARN,
			  "rc_nvme_init_controller: I/O queue creation failed (%d) — admin still works\n",
			  ret);
		return 0;
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

	/* Tear down I/O queues *before* disabling the controller so the
	 * Delete SQ / Delete CQ admin commands still go through. */
	rc_nvme_destroy_io_queues(adapter);

	if (base) {
		/* Best-effort graceful shutdown. */
		u32 cc = readl(base + RC_NVME_REG_CC);
		if (cc & RC_NVME_CC_EN) {
			writel(cc & ~RC_NVME_CC_EN, base + RC_NVME_REG_CC);
			rc_nvme_wait_csts(adapter, RC_NVME_CSTS_RDY, 0);
		}
	}
	rc_nvme_free_admin_queues(adapter);
}
