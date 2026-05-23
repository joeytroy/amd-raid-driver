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

/* Trivial global volume registry — assumes a single RAID0 volume.  Members
 * are inserted as their metadata validates.  Ordered by PCI BDF so the
 * stripe mapping is deterministic; this is a guess at member position
 * until the metadata field that encodes the explicit position is decoded
 * via further RE.  Tracked under rc_volume_lock. */
#define RC_VOLUME_MAX_MEMBERS	8
static struct rc_adapter *rc_volume_members[RC_VOLUME_MAX_MEMBERS];
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

/* gendisk state — at most one volume right now. */
static struct gendisk *rc_volume_disk;
static struct blk_mq_tag_set rc_volume_tagset;

/* Per-tag-per-member persistent DMA buffer + PRP list buffer (allocated
 * once at volume create).  We size the blk-mq tag pool to match — each
 * tag has a dedicated 512 KiB data buffer on every member, since at
 * submit time we don't yet know which member the request will route to
 * (and the same tag can route to either member on different requests).
 *
 *   data buf is 512 KiB per tag — the largest single NVMe READ this
 *   controller allows (Crucial T700 reports Identify Controller MDTS=7
 *   at CAP.MPSMIN=0, so 2^7 * 4 KiB = 512 KiB).  A full 1 MiB stripe
 *   becomes two READs.
 *   PRP list buf is PAGE_SIZE per tag — holds 512 8-byte entries, well
 *   above the 127 needed for 128 pages.
 *   Total: QD * 2 members * (512K + 4K) = QD * 1.03 MB.  At QD=32 that's
 *   ~33 MB on the dev box.
 */
#define RC_VOLUME_DATA_PAGES	128
#define RC_VOLUME_DATA_BYTES	(RC_VOLUME_DATA_PAGES * PAGE_SIZE)
#define RC_VOLUME_QUEUE_DEPTH	32

static void       *rc_volume_dma_va[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static dma_addr_t  rc_volume_dma_pa[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static __le64     *rc_volume_prp_va[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];
static dma_addr_t  rc_volume_prp_pa[RC_VOLUME_MAX_MEMBERS][RC_VOLUME_QUEUE_DEPTH];

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
 * are racy under concurrency but error-vs-not is preserved correctly.) */
struct rc_volume_pdu {
	int       member_idx;
	u8        op;             /* req_op() value */
	u16       sc_sct;         /* NVMe SC/SCT, 0 = success */
	atomic_t  members_pending;
};

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

/* MSI handler called from rc_hw_interrupt_handler when ctrl_mode == NVMe.
 *
 * Admin path: just wake admin_cq_wait.  The single admin submitter that
 * may be in flight at boot consumes its own CQE.
 *
 * I/O path: walk io_cq for all CQEs at the expected phase; for each one
 * look up the in-flight request via blk_mq_tag_to_rq(cid) and complete
 * it with blk_mq_complete_request — the .complete callback then runs in
 * softirq, does the bvec memcpy (for READ) and blk_mq_end_request. */
irqreturn_t rc_nvme_irq(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct blk_mq_tags *tags;
	bool advanced = false;

	/* Health canary: if the controller has gone fatal or the device has
	 * been hot-unplugged (CSTS reads as ~0), don't trust the CQ contents.
	 * Mark the adapter dead so queue_rq fast-fails and the next .timeout
	 * pass drains everything in flight.  Wakeups still fire so admin /
	 * sync-init waiters return promptly via their wait_event_timeout. */
	if (nvme->io_cq && adapter->ctx.mmio_base) {
		u32 csts = readl(adapter->ctx.mmio_base + RC_NVME_REG_CSTS);
		if (csts == 0xffffffff || (csts & RC_NVME_CSTS_CFS)) {
			if (!READ_ONCE(nvme->dead)) {
				rc_printk(RC_ERROR,
					  "rc_nvme_irq: %s controller dead (CSTS=0x%08x) — failing in-flight I/O\n",
					  pci_name(adapter->pdev), csts);
				WRITE_ONCE(nvme->dead, true);
			}
			wake_up(&nvme->admin_cq_wait);
			wake_up(&nvme->io_cq_wait);
			return IRQ_HANDLED;
		}
	}

	/* Always wake admin + io waiters first — the boot-time sync I/O
	 * helper (rc_nvme_io_cmd_sync) sleeps on io_cq_wait and consumes
	 * its own CQE before the disk exists.  Only after rc_volume_disk
	 * is up do we own the CQ in the ISR. */
	wake_up(&nvme->admin_cq_wait);
	wake_up(&nvme->io_cq_wait);

	if (!nvme->io_cq || !rc_volume_disk)
		return IRQ_HANDLED;

	tags = rc_volume_tagset.tags[0];

	spin_lock(&nvme->io_lock);
	for (;;) {
		struct rc_nvme_cqe *cqe = (struct rc_nvme_cqe *)nvme->io_cq +
					  nvme->io_cq_head;
		u16 status = le16_to_cpu(READ_ONCE(cqe->status));
		u16 cid;
		struct request *req;
		struct rc_volume_pdu *pdu;

		if ((status & 1) != nvme->io_cq_phase)
			break;

		cid = le16_to_cpu(cqe->cid);
		req = blk_mq_tag_to_rq(tags, cid);
		if (req) {
			u16 sc = (status >> 1) & 0x7fff;
			pdu = blk_mq_rq_to_pdu(req);
			/* Preserve any prior error from a sibling member's
			 * completion (relevant only for multi-member FLUSH);
			 * single-shot READ/WRITE only land here once. */
			if (sc)
				pdu->sc_sct = sc;
			if (atomic_dec_and_test(&pdu->members_pending))
				blk_mq_complete_request(req);
		} else {
			rc_printk(RC_WARN,
				  "rc_nvme_irq: %s CQE for unknown CID=%u (status=0x%04x)\n",
				  pci_name(adapter->pdev), cid, status);
		}

		nvme->io_cq_head = (nvme->io_cq_head + 1) % nvme->io_cq_depth;
		if (nvme->io_cq_head == 0)
			nvme->io_cq_phase ^= 1;
		advanced = true;
	}
	if (advanced)
		rc_nvme_ring_cq_doorbell(adapter, RC_NVME_IO_QID,
					 nvme->io_cq_head);
	spin_unlock(&nvme->io_lock);
	return IRQ_HANDLED;
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

/* Copy a caller-built SQE into the admin SQ tail slot, ring the doorbell,
 * poll for completion, advance the CQ head, and return 0 on success or
 * -errno (timeout, controller error). The caller fills opc/nsid/prp1/cdwN;
 * we manage CID = 0 since only one admin command is in flight at a time —
 * the admin_mutex serialises against teardown and against timeout-issued
 * Aborts that run from a different process context.
 */
static int rc_nvme_admin_cmd(struct rc_adapter *adapter, struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe *slot;
	struct rc_nvme_cqe *cqe;
	u32 tail;
	u16 status, sc_sct;
	int ret;

	mutex_lock(&nvme->admin_mutex);

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
		goto out;
	}

	cqe = (struct rc_nvme_cqe *)nvme->admin_cq + nvme->admin_cq_head;
	status = le16_to_cpu(cqe->status);
	sc_sct = (status >> 1) & 0x7fff;

	nvme->admin_cq_head = (nvme->admin_cq_head + 1) % nvme->admin_cq_depth;
	if (nvme->admin_cq_head == 0)
		nvme->admin_cq_phase ^= 1;
	rc_nvme_ring_cq_doorbell(adapter, 0, nvme->admin_cq_head);

	if (sc_sct) {
		rc_printk(RC_ERROR,
			  "rc_nvme_admin_cmd: opc=0x%02x SC/SCT=0x%04x (status=0x%04x)\n",
			  cmd->opc, sc_sct, status);
		ret = -EIO;
		goto out;
	}
	ret = 0;
out:
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
	return rc_nvme_admin_cmd(adapter, &cmd);
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

	ret = rc_nvme_admin_cmd(adapter, &cmd);
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

	ret = rc_nvme_admin_cmd(adapter, &cmd);
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

/* Allocate one I/O completion + submission queue pair (qid=1) and ask the
 * controller to wire them up via Create I/O CQ then Create I/O SQ admin
 * commands.  Create I/O CQ sets IEN=1 + IV=0 so the controller raises an
 * interrupt on our single MSI vector when a CQE is posted. */
static int rc_nvme_create_io_queues(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd;
	size_t sq_bytes, cq_bytes;
	u16 depth;
	int ret;

	depth = RC_NVME_IO_QUEUE_DEPTH;
	if (depth > nvme->mqes)
		depth = nvme->mqes;

	sq_bytes = (size_t)depth * RC_NVME_SQ_ENTRY_SIZE;
	cq_bytes = (size_t)depth * RC_NVME_CQ_ENTRY_SIZE;

	nvme->io_sq = dma_alloc_coherent(dev, sq_bytes, &nvme->io_sq_dma, GFP_KERNEL);
	if (!nvme->io_sq)
		return -ENOMEM;
	nvme->io_cq = dma_alloc_coherent(dev, cq_bytes, &nvme->io_cq_dma, GFP_KERNEL);
	if (!nvme->io_cq) {
		ret = -ENOMEM;
		goto err_free_sq;
	}
	memset(nvme->io_sq, 0, sq_bytes);
	memset(nvme->io_cq, 0, cq_bytes);
	nvme->io_sq_depth = depth;
	nvme->io_cq_depth = depth;
	nvme->io_sq_tail = 0;
	nvme->io_cq_head = 0;
	nvme->io_cq_phase = 1;

	/* Create I/O Completion Queue (NVMe 1.4 §5.3, Figure 110).
	 * CDW10[31:16] = QSIZE-1 (0's based), [15:0] = QID
	 * CDW11[31:16] = IV, [1] = IEN, [0] = PC */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_CQ;
	cmd.prp1  = cpu_to_le64(nvme->io_cq_dma);
	cmd.cdw10 = cpu_to_le32(((u32)(depth - 1) << 16) | RC_NVME_IO_QID);
	/* CDW11[31:16]=IV (vector 0), [1]=IEN (raise IRQ on completion), [0]=PC. */
	cmd.cdw11 = cpu_to_le32((0u << 16) | 0x3u);
	ret = rc_nvme_admin_cmd(adapter, &cmd);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_create_io_queues: Create I/O CQ failed (%d)\n", ret);
		goto err_free_cq;
	}

	/* Create I/O Submission Queue (NVMe 1.4 §5.4).
	 * CDW10[31:16] = QSIZE-1, [15:0] = QID
	 * CDW11[31:16] = CQID, [2:1] = QPRIO (0=urgent for admin/0=medium for I/O,
	 *                                     value 0 means "Medium" which is fine),
	 *               [0] = PC */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_CREATE_IO_SQ;
	cmd.prp1  = cpu_to_le64(nvme->io_sq_dma);
	cmd.cdw10 = cpu_to_le32(((u32)(depth - 1) << 16) | RC_NVME_IO_QID);
	cmd.cdw11 = cpu_to_le32(((u32)RC_NVME_IO_QID << 16) | 0x1u);  /* CQID, PC=1 */
	ret = rc_nvme_admin_cmd(adapter, &cmd);
	if (ret) {
		rc_printk(RC_ERROR,
			  "rc_nvme_create_io_queues: Create I/O SQ failed (%d)\n", ret);
		goto err_delete_cq;
	}

	rc_printk(RC_NOTE,
		  "rc_nvme_create_io_queues: qid=%u up — SQ depth=%u CQ depth=%u\n",
		  RC_NVME_IO_QID, depth, depth);
	return 0;

err_delete_cq:
	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_CQ;
	cmd.cdw10 = cpu_to_le32(RC_NVME_IO_QID);
	(void)rc_nvme_admin_cmd(adapter, &cmd);
err_free_cq:
	dma_free_coherent(dev, cq_bytes, nvme->io_cq, nvme->io_cq_dma);
	nvme->io_cq = NULL;
err_free_sq:
	dma_free_coherent(dev, sq_bytes, nvme->io_sq, nvme->io_sq_dma);
	nvme->io_sq = NULL;
	return ret;
}

/* Submit one I/O command to adapter's qid=1 SQ asynchronously.  Caller
 * (blk-mq queue_rq) returns BLK_STS_OK immediately; completion lands in
 * rc_nvme_irq → blk_mq_complete_request → .complete callback. */
static void rc_nvme_io_submit(struct rc_adapter *adapter,
			      struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe *slot;
	unsigned long flags;
	u32 tail;

	spin_lock_irqsave(&nvme->io_lock, flags);
	slot = (struct rc_nvme_sqe *)nvme->io_sq + nvme->io_sq_tail;
	memcpy(slot, cmd, sizeof(*cmd));

	tail = (nvme->io_sq_tail + 1) % nvme->io_sq_depth;
	nvme->io_sq_tail = tail;
	wmb();
	rc_nvme_ring_sq_doorbell(adapter, RC_NVME_IO_QID, tail);
	spin_unlock_irqrestore(&nvme->io_lock, flags);
}

/* Synchronous I/O command used at boot time to read metadata before the
 * blk-mq disk is up.  Submits a single SQE with CID=0, waits on
 * io_cq_wait until the CQE arrives (woken by rc_nvme_irq), consumes the
 * CQE, advances head + doorbell.  Safe because rc_nvme_irq returns
 * early — without touching the CQ — while rc_volume_disk is NULL. */
static int rc_nvme_io_cmd_sync(struct rc_adapter *adapter,
			       struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_cqe *cqe;
	unsigned long deadline;
	u8 expected_phase;
	u16 status, sc_sct;

	cmd->cid = cpu_to_le16(0);
	rc_nvme_io_submit(adapter, cmd);

	cqe = (struct rc_nvme_cqe *)nvme->io_cq + nvme->io_cq_head;
	expected_phase = nvme->io_cq_phase;
	deadline = jiffies + msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);
	while (time_before(jiffies, deadline)) {
		long left = deadline - jiffies;
		long tick = min_t(long, left, max_t(long, 1, msecs_to_jiffies(1)));

		if (wait_event_timeout(nvme->io_cq_wait,
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

	nvme->io_cq_head = (nvme->io_cq_head + 1) % nvme->io_cq_depth;
	if (nvme->io_cq_head == 0)
		nvme->io_cq_phase ^= 1;
	rc_nvme_ring_cq_doorbell(adapter, RC_NVME_IO_QID, nvme->io_cq_head);

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
			u32 devtype, devices, elem_off, chunk;
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
			for (j = 0; j < devices; j++) {
				u8 *elem = ld + elem_off + j * RC_LE_BYTES;
				u64 eid =
					get_unaligned_le64(elem + RC_LE_DEVICEID_OFFSET);
				if (eid == nvme->md_device_id) {
					my_pos = (int)j;
					break;
				}
			}

			rc_printk(RC_INFO,
				  "rc_volume_parse_logical_device: %s LD@ring+%u.%u devtype=0x%04x devices=%u chunk=%u capacity=%llu my_pos=%d%s\n",
				  pci_name(adapter->pdev),
				  scan * RC_LD_SCAN_CHUNK_SECTORS + (i / 512),
				  i % 512,
				  devtype, devices, chunk,
				  (unsigned long long)capacity, my_pos,
				  my_pos < 0 ? " (skip — not our member)" :
					       " (match)");

			if (my_pos < 0)
				continue;

			nvme->ld_device_type      = devtype;
			nvme->ld_devices          = devices;
			nvme->ld_chunk_sectors    = chunk;
			nvme->ld_capacity_sectors = capacity;
			nvme->ld_my_position      = my_pos;
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
	*out_phys   = phys_stripe * stripe + stripe_off;
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

/* Stripe size for the assembled volume in sectors.  Sourced from
 * RC_LogicalDevice.ChunkSize when non-zero; otherwise falls back to the
 * RAID-level firmware default (2048 sectors / 1 MiB for RAID0). */
static u32 rc_volume_chunk_sectors_for(u32 devtype, u32 ld_chunk)
{
	if (ld_chunk)
		return ld_chunk;
	switch (devtype) {
	case RC_LDT_RAID0:
		/* Confirmed via RC_BuildConfigMetadataFromMemory in rcblob:
		 * RAID0 never writes a non-zero ChunkSize; the firmware
		 * defaults to 1 MiB. */
		return 2048u;
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
							    nvme->ld_chunk_sectors);
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
		rc_volume_members[pos] = adapter;
		rc_volume_member_count++;
	}

	rc_printk(RC_INFO,
		  "rc_volume_register_member: %s registered at pos %d (%d/%u)\n",
		  pci_name(adapter->pdev), pos,
		  rc_volume_member_count, expected);

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

/* Build an NVMe READ/WRITE SQE for one volume member.  PRP1 is the per-tag
 * data buffer on that member's pdev; PRP list (also per-tag-per-member) is
 * filled in for transfers spanning more than 2 pages.  CID = tag.  FUA is
 * passed through when the request has REQ_FUA set. */
static void rc_volume_build_io_sqe(struct rc_nvme_sqe *cmd, int member_idx,
				   u32 tag, u8 opc, u64 slba, u16 nlb_zbased,
				   bool fua)
{
	u32 bytes = ((u32)nlb_zbased + 1) * 512u;
	u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	dma_addr_t data_dma = rc_volume_dma_pa[member_idx][tag];
	u32 cdw12 = nlb_zbased;

	if (fua)
		cdw12 |= (1u << 30);	/* CDW12.FUA */

	memset(cmd, 0, sizeof(*cmd));
	cmd->opc   = opc;
	cmd->cid   = cpu_to_le16((u16)tag);
	cmd->nsid  = cpu_to_le32(1);
	cmd->prp1  = cpu_to_le64(data_dma);
	cmd->cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd->cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd->cdw12 = cpu_to_le32(cdw12);

	if (pages == 2) {
		cmd->prp2 = cpu_to_le64(data_dma + PAGE_SIZE);
	} else if (pages > 2) {
		__le64 *list = rc_volume_prp_va[member_idx][tag];
		u32 i;
		for (i = 0; i + 1 < pages; i++)
			list[i] = cpu_to_le64(data_dma + (i + 1) * PAGE_SIZE);
		cmd->prp2 = cpu_to_le64(rc_volume_prp_pa[member_idx][tag]);
	}
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

/* blk-mq request handler.  chunk_sectors=stripe_sectors in queue_limits
 * keeps each request inside one stripe, so it maps to exactly one member.
 *
 * Async submission: stage the bvec for WRITE → submit SQE with CID=tag →
 * return BLK_STS_OK.  Completion lands in rc_nvme_irq, which calls
 * blk_mq_complete_request(req); rc_volume_complete then runs in softirq
 * and finishes the request (memcpy out for READ, blk_mq_end_request). */
static blk_status_t rc_volume_queue_rq(struct blk_mq_hw_ctx *hctx,
				       const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	struct bio_vec bvec;
	struct req_iterator iter;
	sector_t pos = blk_rq_pos(req);
	unsigned int nr_sectors = blk_rq_sectors(req);
	enum req_op op = req_op(req);
	int member_idx;
	u64 phys_lba;
	u32 tag = req->tag;
	struct rc_nvme_sqe cmd;
	size_t off;
	int i;

	pdu->op = op;
	pdu->sc_sct = 0;

	/* If any member adapter has been flagged dead (by the ISR's CSTS
	 * check, by a prior timeout, or by drain), the RAID0 volume can't
	 * serve I/O.  Fail every incoming request rather than wedging more
	 * CIDs against a controller we no longer trust. */
	if (rc_volume_any_member_dead()) {
		blk_mq_start_request(req);
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
		for (i = 0; i < rc_volume_member_count; i++)
			rc_nvme_io_submit(rc_volume_members[i], &cmd);
		return BLK_STS_OK;
	}

	if (op != REQ_OP_READ && op != REQ_OP_WRITE && op != REQ_OP_DISCARD) {
		blk_mq_start_request(req);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	if (nr_sectors == 0 ||
	    pos + nr_sectors > get_capacity(rc_volume_disk) ||
	    (op != REQ_OP_DISCARD &&
	     nr_sectors > (RC_VOLUME_DATA_BYTES / 512))) {
		blk_mq_start_request(req);
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	rc_volume_map_lba(pos, &member_idx, &phys_lba);
	pdu->member_idx = member_idx;
	atomic_set(&pdu->members_pending, 1);

	if (op == REQ_OP_DISCARD) {
		rc_volume_build_discard_sqe(&cmd, member_idx, tag,
					    phys_lba, nr_sectors);
		blk_mq_start_request(req);
		rc_nvme_io_submit(rc_volume_members[member_idx], &cmd);
		return BLK_STS_OK;
	}

	if (op == REQ_OP_WRITE) {
		off = 0;
		rq_for_each_segment(bvec, req, iter) {
			u8 *kaddr = kmap_local_page(bvec.bv_page);
			memcpy((u8 *)rc_volume_dma_va[member_idx][tag] + off,
			       kaddr + bvec.bv_offset,
			       bvec.bv_len);
			kunmap_local(kaddr);
			off += bvec.bv_len;
		}
	}

	rc_volume_build_io_sqe(&cmd, member_idx, tag,
			       op == REQ_OP_WRITE ? RC_NVME_NVM_OP_WRITE
						  : RC_NVME_NVM_OP_READ,
			       phys_lba, (u16)(nr_sectors - 1),
			       (req->cmd_flags & REQ_FUA) != 0);

	/* Must call blk_mq_start_request BEFORE submitting — otherwise the
	 * ISR could complete the request before blk-mq considers it started. */
	blk_mq_start_request(req);
	rc_nvme_io_submit(rc_volume_members[member_idx], &cmd);
	return BLK_STS_OK;
}

/* Tagset .complete callback — runs in softirq after blk_mq_complete_request
 * is called from the ISR.  Copy the per-tag DMA buffer back out to bvecs
 * for READ; end the request with the recorded status. */
static void rc_volume_complete(struct request *req)
{
	struct rc_volume_pdu *pdu = blk_mq_rq_to_pdu(req);
	struct bio_vec bvec;
	struct req_iterator iter;
	u32 tag = req->tag;
	size_t off;

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

	if (pdu->op == REQ_OP_READ) {
		off = 0;
		rq_for_each_segment(bvec, req, iter) {
			u8 *kaddr = kmap_local_page(bvec.bv_page);
			memcpy(kaddr + bvec.bv_offset,
			       (u8 *)rc_volume_dma_va[pdu->member_idx][tag] + off,
			       bvec.bv_len);
			kunmap_local(kaddr);
			off += bvec.bv_len;
		}
	}

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
		if (!blk_mq_request_completed(req))
			blk_mq_end_request(req, BLK_STS_IOERR);
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
	if (!blk_mq_request_completed(req))
		blk_mq_end_request(req, BLK_STS_TIMEOUT);
	return BLK_EH_DONE;
}

static const struct blk_mq_ops rc_volume_mq_ops = {
	.queue_rq = rc_volume_queue_rq,
	.complete = rc_volume_complete,
	.timeout  = rc_volume_timeout,
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

	/* Per-tag, per-member: a 512 KiB data buffer + PAGE_SIZE PRP list.
	 * Tag = NVMe CID; the same tag may route to different members on
	 * different requests, so each member needs its own buffer pool. */
	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		u32 t;

		for (t = 0; t < RC_VOLUME_QUEUE_DEPTH; t++) {
			rc_volume_dma_va[i][t] =
				dma_alloc_coherent(dev, RC_VOLUME_DATA_BYTES,
						   &rc_volume_dma_pa[i][t],
						   GFP_KERNEL);
			if (!rc_volume_dma_va[i][t]) {
				ret = -ENOMEM;
				goto err_free_dma;
			}
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

	memset(&rc_volume_tagset, 0, sizeof(rc_volume_tagset));
	rc_volume_tagset.ops          = &rc_volume_mq_ops;
	rc_volume_tagset.nr_hw_queues = 1;
	rc_volume_tagset.queue_depth  = RC_VOLUME_QUEUE_DEPTH;
	rc_volume_tagset.numa_node    = NUMA_NO_NODE;
	rc_volume_tagset.cmd_size     = sizeof(struct rc_volume_pdu);
	/* Async dispatch — queue_rq returns BLK_STS_OK immediately and
	 * completion lands via rc_nvme_irq → blk_mq_complete_request →
	 * rc_volume_complete.  No sleeping on the dispatch path → no
	 * BLK_MQ_F_BLOCKING required. */
	rc_volume_tagset.flags        = 0;

	ret = blk_mq_alloc_tag_set(&rc_volume_tagset);
	if (ret)
		goto err_free_dma;

	{
		struct queue_limits lim = {
			.logical_block_size  = 512,
			.physical_block_size = 512,
			/* PRP list lets us span up to RC_VOLUME_DATA_BYTES per
			 * request.  Caller's bvecs sum into one NVMe READ. */
			.max_hw_sectors      = RC_VOLUME_DATA_BYTES / 512,
			.max_segments        = RC_VOLUME_DATA_PAGES,
			.max_segment_size    = PAGE_SIZE,
			/* Requests never cross a RAID0 stripe boundary, so every
			 * non-flush request maps to one member and one NVMe READ
			 * or WRITE.  FLUSH is fanned out separately. */
			.chunk_sectors       = rc_volume_stripe_sectors,
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
			if (rc_volume_dma_va[i][t]) {
				dma_free_coherent(dev, RC_VOLUME_DATA_BYTES,
						  rc_volume_dma_va[i][t],
						  rc_volume_dma_pa[i][t]);
				rc_volume_dma_va[i][t] = NULL;
			}
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
				if (rc_volume_dma_va[i][t]) {
					dma_free_coherent(dev,
							  RC_VOLUME_DATA_BYTES,
							  rc_volume_dma_va[i][t],
							  rc_volume_dma_pa[i][t]);
					rc_volume_dma_va[i][t] = NULL;
					rc_volume_dma_pa[i][t] = 0;
				}
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
	}
	rc_volume_member_count = 0;
	rc_volume_stripe_sectors = 0;
	mutex_unlock(&rc_volume_lock);
}

/* Counterpart to create_io_queues; called from cleanup_controller. Sends
 * Delete SQ then Delete CQ (order matters per spec) before freeing DMA. */
static void rc_nvme_destroy_io_queues(struct rc_adapter *adapter)
{
	struct device *dev = &adapter->pdev->dev;
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe cmd;

	if (nvme->io_sq) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_SQ;
		cmd.cdw10 = cpu_to_le32(RC_NVME_IO_QID);
		(void)rc_nvme_admin_cmd(adapter, &cmd);
		dma_free_coherent(dev,
				  (size_t)nvme->io_sq_depth * RC_NVME_SQ_ENTRY_SIZE,
				  nvme->io_sq, nvme->io_sq_dma);
		nvme->io_sq = NULL;
	}
	if (nvme->io_cq) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opc   = RC_NVME_ADMIN_OP_DELETE_IO_CQ;
		cmd.cdw10 = cpu_to_le32(RC_NVME_IO_QID);
		(void)rc_nvme_admin_cmd(adapter, &cmd);
		dma_free_coherent(dev,
				  (size_t)nvme->io_cq_depth * RC_NVME_CQ_ENTRY_SIZE,
				  nvme->io_cq, nvme->io_cq_dma);
		nvme->io_cq = NULL;
	}
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
	init_waitqueue_head(&nvme->io_cq_wait);
	spin_lock_init(&nvme->io_lock);
	mutex_init(&nvme->admin_mutex);

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

	/* One I/O queue pair so the upper layer can submit NVM commands. */
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
