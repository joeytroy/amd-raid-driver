// SPDX-License-Identifier: GPL-2.0-only
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
/* RAID level of the assembled volume — the NORMALIZED level (RC_LDT_RAID0 /
 * RC_LDT_RAID1) derived by rc_ld_level_from() from the on-disk DeviceType +
 * FirstCount x SecondCount, NOT the raw DeviceType (real firmware writes
 * 0x1BF6 for both levels).  Seeded by the first registered member
 * (RC_LDT_RAID0 for the legacy no-LD fallback); later members must match or
 * are rejected, same contract as stripe_sectors/expected_members.
 * Everything level-specific (map_lba, mirror fan-out in queue_rq) keys off
 * this. */
static u32 rc_volume_raid_level;
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
	atomic_t           members_pending;
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
	/* Multi-stripe (multi-member) dispatch.  When ms_active is true, the
	 * request was split across multiple members in queue_rq; ms_nents[m]
	 * and ms_sg[m] hold each member's portion.  members_pending is set to
	 * the number of members with non-zero ms_nents.  The pdu->sg/nents
	 * fields above are unused in this path. */
	bool               ms_active;
	int                ms_nents[RC_VOLUME_MAX_MEMBERS];
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
			req = blk_mq_tag_to_rq(tags, cid);
			if (req) {
				u16 sc = (status >> 1) & 0x7fff;
				pdu = blk_mq_rq_to_pdu(req);
				if (sc) {
					/* First-error-wins, single aligned
					 * store: concurrent member ISRs on a
					 * fan-out op race here, but each
					 * store is atomic so the field holds
					 * ONE of the members' statuses (never
					 * a torn mix), and error-vs-success
					 * is always preserved. */
					if (!READ_ONCE(pdu->sc_sct))
						WRITE_ONCE(pdu->sc_sct, sc);
					rc_printk(RC_ERROR,
						  "rc_nvme_io_queue_irq: %s qid=%u CID=%u op=%u pos=%llu len=%u failed SC/SCT=0x%04x\n",
						  pci_name(adapter->pdev), q->qid,
						  cid, pdu->op,
						  (u64)blk_rq_pos(req),
						  blk_rq_sectors(req), sc);
				}
				if (atomic_dec_and_test(&pdu->members_pending) &&
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
			if (pdu->sc_sct)
				blk_mq_end_request(req, BLK_STS_IOERR);
			else
				blk_mq_end_request(req, BLK_STS_OK);
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

/* Chunking for the active-generation scan.  rc_nvme_read_lba caps a transfer
 * at 2 pages (8 KiB = 16 sectors).  Consecutive chunks overlap by
 * RC_LD_SCAN_OVERLAP_SECTORS so a record that starts near the end of one
 * chunk is re-read whole at the head of the next: new record tags are only
 * accepted in the first (CHUNK - OVERLAP) sectors of a non-final chunk.  The
 * overlap (1 KiB) exceeds the largest possible LD record
 * (0x130 fixed bytes + RC_VOLUME_MAX_MEMBERS * RC_LE_BYTES). */
#define RC_LD_SCAN_CHUNK_SECTORS	16u
#define RC_LD_SCAN_OVERLAP_SECTORS	2u

/* Derive the effective RAID level from the on-disk DeviceType +
 * FirstCount (stripe width) x SecondCount (mirror count).  Real firmware
 * writes DeviceType 0x1BF6 for BOTH RAID0 and RAID1 and encodes the layout
 * in the counts (verified on TRX50 hardware dumps, 2026-07-10 — see the
 * RC_LDT_* note in rc_linux.h).  Returns a normalized RC_LDT_RAID0 /
 * RC_LDT_RAID1, or 0 for any layout this driver has no dispatch path for
 * (RAID10, >2-way mirrors, count/device mismatches) — the caller must
 * refuse to assemble those rather than guess. */
static u32 rc_ld_level_from(u32 devtype, u32 first, u32 second, u32 devices)
{
	if (devtype == RC_LDT_RAID1)
		/* Explicit encoding (kept in case some firmware uses it) —
		 * but held to the same 2-way-mirror policy as the
		 * counts-derived path: exactly 2 members, and counts (when
		 * present) must agree.  first/second == 0 is tolerated
		 * because encodings that set an explicit RAID1 DeviceType
		 * may not populate the counts at all. */
		return (devices == 2 &&
			(!first || !second ||
			 (first == 1 && second == 2))) ? RC_LDT_RAID1 : 0;
	/* RC_LDT_SINGLE (0x1BF9) is deliberately NOT accepted: those are the
	 * per-physical-disk "raw disk" LDs firmware publishes for non-array
	 * disks.  Their element array is exactly this disk's own DeviceID, so
	 * they'd match the parser's ownership check and assemble a bogus
	 * 1-member volume over the raw disk.  The parser also skips any
	 * devices < 2 record for the same reason (belt and braces). */
	if (devtype != RC_LDT_RAID0)
		return 0;
	if (!first || !second || first * second != devices)
		return 0;
	if (second == 1)
		return RC_LDT_RAID0;	/* pure stripe */
	if (first == 1 && second == 2)
		return RC_LDT_RAID1;	/* 2-way mirror */
	return 0;
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
		    get_unaligned_le64(buf + RC_COMMIT_TS_OFFSET) != gen_ts) {
			rc_printk(RC_WARN,
				  "rc_volume_parse_logical_device: %s generation header ts 0x%016llx != committed ts 0x%016llx — commit/journal mismatch, refusing to parse\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)get_unaligned_le64(buf + RC_COMMIT_TS_OFFSET),
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
				  level == RC_LDT_RAID1 ? "RAID1" :
				  level == RC_LDT_RAID0 ? "RAID0" :
							  "UNSUPPORTED",
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
		member_idx = (u32)((u32)atomic_inc_return(&rc_volume_rr_next) %
				   (u32)nmembers);
		*out_member = (int)member_idx;
		*out_phys   = logical_lba +
			      rc_volume_member_phys_offset[member_idx];
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
		/* Refuse layouts with no dispatch path (RAID10, >2-way
		 * mirrors, FirstCount x SecondCount != Devices).  Guessing a
		 * mapping for them turns every read into garbage and every
		 * write into corruption, so the member is not registered at
		 * all and the volume never assembles. */
		if (!nvme->ld_level) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s unsupported RAID layout (DeviceType=0x%x FirstCount=%u SecondCount=%u Devices=%u) — ignoring member\n",
				  pci_name(adapter->pdev), nvme->ld_device_type,
				  nvme->ld_first_count, nvme->ld_second_count,
				  nvme->ld_devices);
			goto out;
		}
		/* Cross-check: total capacity must equal the per-member
		 * user-data size times the stripe width (mirrors don't add
		 * capacity).  A mismatch means we misread the geometry
		 * somewhere — keep the volume readable but veto writes.
		 * Runs here rather than in the parser so the write to the
		 * lock-protected untrust reason happens under rc_volume_lock
		 * like every other writer. */
		if (nvme->ld_capacity_sectors !=
		    nvme->ld_userdata_size * nvme->ld_first_count) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s capacity %llu != user_sz %llu x first_count %u — geometry UNTRUSTED, writes will be vetoed\n",
				  pci_name(adapter->pdev),
				  (unsigned long long)nvme->ld_capacity_sectors,
				  (unsigned long long)nvme->ld_userdata_size,
				  nvme->ld_first_count);
			rc_volume_geometry_untrust_reason =
				"the LD record's total capacity doesn't equal per-member user size x stripe width — the on-disk geometry doesn't add up, so some field is being misread";
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
		if (nvme->ld_level == RC_LDT_RAID0 &&
		    nvme->ld_chunk_sectors == 0 && nvme->ld_chunk_index > 3) {
			rc_printk(RC_WARN,
				  "rc_volume_register_member: %s RAID0 chunk_index=%u not understood (only 0..3 map to a known stripe) — geometry UNTRUSTED, writes will be vetoed\n",
				  pci_name(adapter->pdev), nvme->ld_chunk_index);
			rc_volume_geometry_untrust_reason =
				"the on-disk RAID0 chunk_index is not one this driver maps to a stripe size (see the earlier chunk_index warning) — an on-disk encoding this driver doesn't recognize, not a parse failure";
		}
		/* Sanity-bound a verbatim on-disk ChunkSize.  Every dispatch
		 * assumption (per-member sg arrays, one NVMe cmd per member,
		 * the 1 MiB max request) is derived from sane power-of-two
		 * stripes; an implausible value means we're misreading the
		 * field, so keep the array readable but veto writes. */
		if (nvme->ld_level == RC_LDT_RAID0 && nvme->ld_chunk_sectors &&
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

	/* First member: seed expected count + stripe + RAID level (the
	 * NORMALIZED level from rc_ld_level_from — the raw on-disk DeviceType
	 * is 0x1BF6 for both RAID0 and RAID1).  Later members must match. */
	if (rc_volume_member_count == 0) {
		rc_volume_expected_members = expected;
		rc_volume_stripe_sectors   = chunk_sectors;
		rc_volume_raid_level       = have_ld ? nvme->ld_level
						     : RC_LDT_RAID0;
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

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS || !stripe) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_multi_stripe: bad volume state (members=%d stripe=%u) — pos=%llu len=%u rejected\n",
			  nm, stripe, (u64)pos, nr_sectors);
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
			int mbr = (int)(stripe_idx % nm);

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
	int m;

	if (nm <= 0 || nm > RC_VOLUME_MAX_MEMBERS) {
		rc_printk(RC_ERROR,
			  "rc_volume_dispatch_mirror: bad volume state (members=%d) — pos=%llu len=%u rejected\n",
			  nm, (u64)pos, nr_sectors);
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
		for (m = 0; m < nm; m++)
			sg_mark_end(&pdu->ms_sg[m][used - 1]);

		pdu->ms_active = true;
		for (m = 0; m < nm; m++) {
			struct device *dma_dev =
				&rc_volume_members[m]->pdev->dev;
			int mapped = dma_map_sg(dma_dev, pdu->ms_sg[m], used,
						DMA_TO_DEVICE);

			if (mapped == 0) {
				rc_printk(RC_ERROR,
					  "rc_volume_dispatch_mirror: dma_map_sg failed (member %d) — pos=%llu len=%u rejected\n",
					  m, (u64)pos, nr_sectors);
				pdu->ms_nents[m] = 0;
				for (--m; m >= 0; m--) {
					dma_dev = &rc_volume_members[m]->pdev->dev;
					dma_unmap_sg(dma_dev, pdu->ms_sg[m],
						     pdu->ms_nents[m],
						     DMA_TO_DEVICE);
					pdu->ms_nents[m] = 0;
				}
				pdu->ms_active = false;
				return BLK_STS_IOERR;
			}
			pdu->ms_nents[m] = mapped;
		}
	}
	/* DISCARD: no DMA mapping — nents/ms_active stay zero from queue_rq's
	 * memset, so unmap no-ops; the DSM range lives in the per-tag PRP
	 * buffer, one per (hctx, member, tag). */

	/* One SQE per member — reserve them all or requeue (C-4).  Runs
	 * before blk_mq_start_request so BLK_STS_DEV_RESOURCE is a clean
	 * "try again" for blk-mq. */
	for (m = 0; m < nm; m++) {
		if (!rc_nvme_sq_reserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
			while (m-- > 0)
				rc_nvme_sq_unreserve(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num], 1);
			rc_volume_unmap_request_sg(pdu);
			return BLK_STS_DEV_RESOURCE;
		}
	}

	pdu->member_idx = -1;	/* multi-member — timeout/drain treat as all */
	atomic_set(&pdu->members_pending, nm);

	/* Build all SQEs before starting/submitting so a PRP-expressibility
	 * failure can still cleanly reject the request. */
	{
		struct rc_nvme_sqe cmds[RC_VOLUME_MAX_MEMBERS];

		for (m = 0; m < nm; m++) {
			u64 phys = (u64)pos + rc_volume_member_phys_offset[m];

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

		blk_mq_start_request(req);
		for (m = 0; m < nm; m++)
			rc_nvme_io_submit(rc_volume_members[m]->ctx.nvme.io_queues[hctx->queue_num],
					  &cmds[m]);
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
	atomic_set(&pdu->completed, 0);
	pdu->hctx_idx = (u8)(hctx->queue_num < RC_VOLUME_MAX_HCTX ?
			     hctx->queue_num : 0xff);

	/* If any member adapter has been flagged dead (by the ISR's CSTS
	 * check, by a prior timeout, or by drain), the RAID0 volume can't
	 * serve I/O.  Fail every incoming request rather than wedging more
	 * CIDs against a controller we no longer trust. */
	if (rc_volume_any_member_dead()) {
		/* NO DEGRADED MODE (yet): one dead member fails the whole
		 * volume — including RAID1, which therefore provides
		 * redundancy of data at rest but not availability.  Say so
		 * explicitly instead of failing I/O silently. */
		printk_ratelimited(KERN_ERR
			"rcraid: %s: failing I/O — a member controller is dead and the driver has no degraded mode (all I/O fails until reset/reload)\n",
			rc_volume_disk ? rc_volume_disk->disk_name : "?");
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
		/* Reserve one SQE per member queue before starting; on a
		 * full SQ hand the request back to blk-mq for a retry
		 * instead of overrunning the ring (C-4). */
		for (i = 0; i < rc_volume_member_count; i++) {
			if (!rc_nvme_sq_reserve(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num], 1)) {
				while (i-- > 0)
					rc_nvme_sq_unreserve(rc_volume_members[i]->ctx.nvme.io_queues[hctx->queue_num], 1);
				return BLK_STS_DEV_RESOURCE;
			}
		}

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
		if (st == BLK_STS_DEV_RESOURCE) {
			/* SQ full — hand back to blk-mq untouched for a
			 * retry (the request was never started). */
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
		if (st == BLK_STS_DEV_RESOURCE) {
			/* SQ full — requeue; request never started. */
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
		if (st == BLK_STS_DEV_RESOURCE) {
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
	pdu->member_idx = member_idx;
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

/* Does this request need `member` to complete?  FLUSH and every
 * multi-member dispatch (member_idx == -1: RAID0 multi-stripe R/W, RAID0
 * multi-member discard, RAID1 mirror write/discard) involve all members —
 * for a multi-stripe request that only touched a subset this is
 * conservative, but a volume with any dead member is failing those
 * requests anyway.  Single-member requests involve exactly member_idx. */
static bool rc_volume_req_involves(const struct rc_volume_pdu *pdu, int member)
{
	if (pdu->op == REQ_OP_FLUSH || pdu->member_idx < 0)
		return true;
	return pdu->member_idx == member;
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
	bool kill = false;
	int i;

	for (i = 0; i < rc_volume_member_count && i < RC_VOLUME_MAX_MEMBERS;
	     i++) {
		if ((ctx->dead_mask & BIT(i)) &&
		    rc_volume_req_involves(pdu, i)) {
			kill = true;
			break;
		}
	}

	if (kill && !blk_mq_request_completed(req) &&
	    rc_volume_claim_completion(pdu)) {
		/* The claim is what makes this safe against the ISR's
		 * direct-end fast path, which bypasses the
		 * blk_mq_complete_request state machine entirely — the
		 * completed-state check alone leaves a double-completion
		 * window there. */
		WRITE_ONCE(pdu->sc_sct, RC_VOLUME_SC_DEAD);
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

	for (i = 0; i < rc_volume_member_count; i++)
		if (rc_volume_req_involves(pdu, i))
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
	for (i = 0; i < rc_volume_member_count; i++)
		if (READ_ONCE(rc_volume_members[i]->ctx.nvme.dead))
			ctx.dead_mask |= BIT(i);
	if (!ctx.dead_mask)
		return;
	/* H-1: quiesce the dead controllers' DMA engines BEFORE ending
	 * requests — ending a request unmaps its scatterlist, and a still-
	 * enabled controller may still be working the command. */
	for (i = 0; i < rc_volume_member_count; i++)
		if (ctx.dead_mask & BIT(i))
			rc_nvme_disable_dead_controller(rc_volume_members[i]);
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

	for (i = 0; i < rc_volume_member_count; i++)
		if (rc_volume_req_involves(pdu, i) &&
		    rc_nvme_check_dead(rc_volume_members[i]))
			any_dead = true;

	if (any_dead) {
		rc_printk(RC_ERROR,
			  "rc_volume_timeout: tag=%u op=%u: member dead — draining in-flight\n",
			  req->tag, pdu->op);
		rc_volume_drain_dead();
		rc_volume_schedule_auto_reset_for_req(pdu);
		if (!blk_mq_request_completed(req) &&
		    rc_volume_claim_completion(pdu)) {
			/* Drain should have caught it; if it didn't (e.g.,
			 * member_idx out of range for some reason) the
			 * direct end path bypasses .complete, so unmap here.
			 * A lost claim means the drain or ISR owns it. */
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

		for (i = 0; i < rc_volume_member_count; i++)
			if (rc_volume_req_involves(pdu, i))
				(void)rc_nvme_abort(rc_volume_members[i],
						    sqid, (u16)req->tag);
	}

	if (blk_mq_request_completed(req))
		return BLK_EH_DONE;

	for (i = 0; i < rc_volume_member_count; i++)
		if (rc_volume_req_involves(pdu, i))
			WRITE_ONCE(rc_volume_members[i]->ctx.nvme.dead, true);
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
	if (rc_volume_member_count > 0) {
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

/* Allocate the gendisk + tagset, set up sizes, expose /dev/rcraid0. */
static int rc_volume_create_disk(void)
{
	struct rc_adapter *m0 = rc_volume_members[0];
	u64 total_sectors;
	int i, ret;
	u32 nr_hw, h;

	/* nr_hw_queues = min(member->nr_io_queues) across all members.  Each
	 * hctx i routes to io_queues[i] on every member, so we can't ask
	 * blk-mq for more hctxs than the smallest member supports.  All
	 * T700s grant 128 (we cap at RC_NVME_IO_QUEUE_TARGET=4) so this
	 * just resolves to 4 in practice, but the min is a future-proofing
	 * safety net.  Computed up front because it dimensions the PRP pool
	 * allocated just below.  Clamped to RC_VOLUME_MAX_HCTX so the [hctx]
	 * index into that pool (and every other per-hctx array) is always in
	 * bounds. */
	nr_hw = m0->ctx.nvme.nr_io_queues;
	for (i = 1; i < rc_volume_member_count; i++) {
		u32 m = rc_volume_members[i]->ctx.nvme.nr_io_queues;
		if (m < nr_hw)
			nr_hw = m;
	}
	if (nr_hw == 0)
		nr_hw = 1;
	if (nr_hw > RC_VOLUME_MAX_HCTX)
		nr_hw = RC_VOLUME_MAX_HCTX;

	/* Per-hctx, per-member, per-tag PRP-list / DSM buffer (PAGE_SIZE each).
	 * The hctx dimension is mandatory — req->tag is unique only within a
	 * single hctx, so two concurrent commands on different queues that draw
	 * the same tag must not share a buffer (see the pool declaration near
	 * the top of this file for the full corruption rationale).  Only the
	 * live nr_hw × member_count × queue_depth subset is allocated; the rest
	 * stay NULL and are skipped by the free paths. */
	for (h = 0; h < nr_hw; h++) {
		for (i = 0; i < rc_volume_member_count; i++) {
			struct device *dev = &rc_volume_members[i]->pdev->dev;
			u32 t;

			for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
				rc_volume_prp_va[h][i][t] =
					dma_alloc_coherent(dev, PAGE_SIZE,
							   &rc_volume_prp_pa[h][i][t],
							   GFP_KERNEL);
				if (!rc_volume_prp_va[h][i][t]) {
					ret = -ENOMEM;
					goto err_free_dma;
				}
			}
		}
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
		/* Async dispatch — queue_rq returns BLK_STS_OK immediately
		 * and completion lands via the per-queue ISR → blk_mq_complete
		 * → rc_volume_complete.  No sleeping on the dispatch path → no
		 * BLK_MQ_F_BLOCKING required. */
		rc_volume_tagset.flags        = 0;

		rc_printk(RC_NOTE,
			  "rc_volume_create_disk: blk-mq nr_hw_queues=%u (queue_depth=%u → %u total outstanding, %u SQEs/queue reserved for internal cmds)\n",
			  nr_hw, depth, nr_hw * depth,
			  RC_VOLUME_SQ_INTERNAL_HEADROOM);
	}

	ret = blk_mq_alloc_tag_set(&rc_volume_tagset);
	if (ret)
		goto err_free_dma;

	{
		struct queue_limits lim = {
			.logical_block_size  = 512,
			.physical_block_size = 512,
			/* PRP list lets us span up to RC_VOLUME_DATA_BYTES per
			 * request.  Caller's bvecs sum into one NVMe READ (or,
			 * for multi-stripe requests, into one NVMe cmd per
			 * member via rc_volume_dispatch_multi_stripe). */
			.max_hw_sectors      = RC_VOLUME_DATA_BYTES / 512,
			/* RAID1 caps segments at the per-member sg array size:
			 * mirror writes copy the request's segments into
			 * ms_sg[member][RC_VOLUME_MS_PAGES_PER_MEMBER], so
			 * blk-mq must never legally hand queue_rq more
			 * segments than that array holds.  (virt_boundary
			 * already bounds a 256 KiB request to ~65 segments in
			 * practice, but the advertised contract is what
			 * matters — don't rely on the implicit math.) */
			.max_segments        = rc_volume_raid_level == RC_LDT_RAID1
						? RC_VOLUME_MS_PAGES_PER_MEMBER
						: RC_VOLUME_DATA_PAGES,
			.max_segment_size    = PAGE_SIZE,
			/* NVMe PRP semantics: PRP1 may carry an in-page offset,
			 * PRP2 and PRP-list entries must point at page starts.
			 * virt_boundary forces blk-mq to split bvecs so every
			 * segment after the first is page-aligned, which is what
			 * rc_volume_build_prp relies on. */
			.virt_boundary_mask  = PAGE_SIZE - 1,
			/* Split READ/WRITE at stripe boundaries so each request
			 * maps to exactly one member.  blk-mq guarantees no
			 * request crosses a chunk_sectors-aligned boundary, so
			 * every READ/WRITE that reaches rc_volume_queue_rq is
			 * single-member and takes the single-stripe path.
			 *
			 * NOTE: rc_volume_dispatch_multi_stripe IS wired up in
			 * rc_volume_queue_rq (its bvec walk splits at stripe
			 * boundaries into per-member sg arrays and honours the
			 * virt_boundary_mask above, so the old PRP-alignment /
			 * sg-overflow problems no longer apply).  But with
			 * chunk_sectors set to the stripe size that fan-out is
			 * currently UNREACHABLE for READ/WRITE — no request ever
			 * spans two stripes.  It's retained (not deleted) as the
			 * ready path for a future change that raises chunk_sectors
			 * to fold a multi-stripe request into one NVMe cmd per
			 * member.  Not worth enabling today: 256 KiB single-member
			 * commands already saturate the members (~18-20 GB/s R/W).
			 *
			 * Discards are unaffected — blk-mq splits them by
			 * max_hw_discard_sectors, not chunk_sectors, so the
			 * one-DSM-per-member fan-out in
			 * rc_volume_dispatch_multi_stripe_discard still sees
			 * genuinely multi-stripe requests. */
			.chunk_sectors       = rc_volume_stripe_sectors,
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

		if (!rc_volume_enable_writes || !geom_trusted)
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
		else
			rc_printk(RC_NOTE,
				  "rc_volume_create_disk: writes ENABLED\n");
	}

	ret = add_disk(rc_volume_disk);
	if (ret)
		goto err_put_disk;

	rc_printk(RC_NOTE,
		  "rc_volume_create_disk: /dev/%s up, %llu sectors (%llu MiB, %s)\n",
		  rc_volume_disk->disk_name,
		  (unsigned long long)total_sectors,
		  (unsigned long long)(total_sectors >> 11),
		  (rc_volume_enable_writes &&
		   rc_volume_geometry_untrust_reason == NULL) ?
			"read-write" : "read-only");
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

		for (h = 0; h < RC_VOLUME_MAX_HCTX; h++) {
			for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
				if (rc_volume_prp_va[h][i][t]) {
					dma_free_coherent(dev, PAGE_SIZE,
							  rc_volume_prp_va[h][i][t],
							  rc_volume_prp_pa[h][i][t]);
					rc_volume_prp_va[h][i][t] = NULL;
					rc_volume_prp_pa[h][i][t] = 0;
				}
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
			u32 t, h;

			for (h = 0; h < RC_VOLUME_MAX_HCTX; h++) {
				for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
					if (rc_volume_prp_va[h][i][t]) {
						dma_free_coherent(dev, PAGE_SIZE,
								  rc_volume_prp_va[h][i][t],
								  rc_volume_prp_pa[h][i][t]);
						rc_volume_prp_va[h][i][t] = NULL;
						rc_volume_prp_pa[h][i][t] = 0;
					}
				}
			}
		}
		rc_volume_members[i] = NULL;
		rc_volume_member_phys_offset[i] = 0;
	}
	rc_volume_member_count = 0;
	rc_volume_stripe_sectors = 0;
	mutex_unlock(&rc_volume_lock);
}

/* PCI-remove hook (also used by probe error paths).  A member adapter is
 * about to be kfree'd; if it is registered in rc_volume_members[] the
 * assembled volume still dereferences it on every I/O — surprise-remove
 * or sysfs unbind of one member with /dev/rcraid0 mounted was a
 * use-after-free on the hot path.  There is no per-member detach (no
 * degraded mode), so the whole volume is torn down: mark the departing
 * member dead, fail the in-flight I/O cleanly (drain disables the dead
 * controller first, per H-1), then dismantle the gendisk + DMA pools.
 * No-op when the adapter never registered. */
void rc_volume_remove_member(struct rc_adapter *adapter)
{
	bool member = false;
	int i;

	mutex_lock(&rc_volume_lock);
	for (i = 0; i < RC_VOLUME_MAX_MEMBERS; i++)
		if (rc_volume_members[i] == adapter)
			member = true;
	mutex_unlock(&rc_volume_lock);

	if (!member)
		return;

	rc_printk(RC_WARN,
		  "rc_volume_remove_member: %s is a live volume member — tearing down /dev/rcraid0 before releasing the adapter (no degraded mode)\n",
		  pci_name(adapter->pdev));

	/* Fail fast: no new dispatches reach this adapter, and every
	 * in-flight request touching it is ended before the teardown
	 * (del_gendisk would otherwise wait forever on requests whose
	 * CQEs can no longer arrive). */
	WRITE_ONCE(adapter->ctx.nvme.dead, true);
	rc_volume_drain_dead();
	rc_volume_teardown();
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
 * Sets nvme->dead so the upper layer fails any blk-mq dispatch that
 * sneaks in between this call and the higher-level blk-mq freeze.
 * Cleared again by rc_nvme_pm_resume_adapter via reset_controller. */
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
 * S3/S4 resume — fewer codepaths to keep correct. */
int rc_nvme_pm_resume_adapter(struct rc_adapter *adapter)
{
	rc_printk(RC_NOTE,
		  "rc_nvme_pm_resume_adapter: %s reviving controller\n",
		  pci_name(adapter->pdev));
	return rc_nvme_reset_controller(adapter);
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
