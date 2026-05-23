/****************************************************************************
 * AMD RAID Driver for Linux - NVMe controller init path
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
 * Until we decode the explicit position field in the metadata, this is the
 * escape hatch if reads come back wrong on a populated array. */
static int rc_volume_reverse_order;
module_param_named(reverse_member_order, rc_volume_reverse_order, int, 0644);
MODULE_PARM_DESC(reverse_member_order,
		 "If non-zero, sort members by descending PCI BDF instead of ascending");

/* gendisk state — at most one volume right now. */
static struct gendisk *rc_volume_disk;
static struct blk_mq_tag_set rc_volume_tagset;

/* Per-member persistent DMA buffer + PRP list buffer (allocated once at
 * volume create).  Queue depth is 1 so a single buffer per member is safe.
 *
 *   data buf is 512 KiB (128 pages) — the largest single NVMe READ this
 *   controller allows (Crucial T700 reports Identify Controller MDTS=7
 *   at CAP.MPSMIN=0, so 2^7 * 4 KiB = 512 KiB).  A full 1 MiB stripe
 *   becomes two READs, which blk-mq dispatches back-to-back.
 *   PRP list buf is PAGE_SIZE — holds 512 8-byte entries, well above the
 *   127 needed for 128 pages (PRP1 covers page 0, list covers 1..127).
 */
#define RC_VOLUME_DATA_PAGES	128
#define RC_VOLUME_DATA_BYTES	(RC_VOLUME_DATA_PAGES * PAGE_SIZE)

static void       *rc_volume_dma_va[RC_VOLUME_MAX_MEMBERS];
static dma_addr_t  rc_volume_dma_pa[RC_VOLUME_MAX_MEMBERS];
static __le64     *rc_volume_prp_va[RC_VOLUME_MAX_MEMBERS];
static dma_addr_t  rc_volume_prp_pa[RC_VOLUME_MAX_MEMBERS];

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

/* Poll the next admin CQE slot until its phase bit flips to the expected
 * value or the timeout expires. No CQ-head advance is done here; the caller
 * must consume the entry and ring the doorbell. */
static int rc_nvme_wait_admin_completion(struct rc_adapter *adapter)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_cqe *cqe;
	u8 expected_phase = nvme->admin_cq_phase;
	unsigned long deadline;

	cqe = (struct rc_nvme_cqe *)nvme->admin_cq + nvme->admin_cq_head;
	deadline = jiffies + msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);

	while (time_before(jiffies, deadline)) {
		u16 status = le16_to_cpu(READ_ONCE(cqe->status));
		if ((status & 1) == expected_phase)
			return 0;
		usleep_range(10, 50);
	}
	return -ETIMEDOUT;
}

/* Copy a caller-built SQE into the admin SQ tail slot, ring the doorbell,
 * poll for completion, advance the CQ head, and return 0 on success or
 * -errno (timeout, controller error). The caller fills opc/nsid/prp1/cdwN;
 * we manage CID = 0 since only one admin command is in flight during init.
 */
static int rc_nvme_admin_cmd(struct rc_adapter *adapter, struct rc_nvme_sqe *cmd)
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
 * commands.  Interrupts stay disabled — we'll poll the CQ for now. */
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
	cmd.cdw11 = cpu_to_le32(0x1u);	/* PC=1, IEN=0 (polled), IV=0 */
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

/* Same shape as rc_nvme_admin_cmd, but for I/O queue 1 (polled). */
static int rc_nvme_io_cmd(struct rc_adapter *adapter, struct rc_nvme_sqe *cmd)
{
	struct rc_nvme_state *nvme = &adapter->ctx.nvme;
	struct rc_nvme_sqe *slot;
	struct rc_nvme_cqe *cqe;
	unsigned long deadline;
	u8 expected_phase;
	u32 tail;
	u16 status, sc_sct;

	slot = (struct rc_nvme_sqe *)nvme->io_sq + nvme->io_sq_tail;
	cmd->cid = cpu_to_le16(0);
	memcpy(slot, cmd, sizeof(*cmd));

	tail = (nvme->io_sq_tail + 1) % nvme->io_sq_depth;
	nvme->io_sq_tail = tail;
	wmb();
	rc_nvme_ring_sq_doorbell(adapter, RC_NVME_IO_QID, tail);

	cqe = (struct rc_nvme_cqe *)nvme->io_cq + nvme->io_cq_head;
	expected_phase = nvme->io_cq_phase;
	deadline = jiffies + msecs_to_jiffies(RC_NVME_ADMIN_TIMEOUT_MS);
	while (time_before(jiffies, deadline)) {
		status = le16_to_cpu(READ_ONCE(cqe->status));
		if ((status & 1) == expected_phase)
			goto have_cqe;
		usleep_range(10, 50);
	}
	rc_printk(RC_ERROR,
		  "rc_nvme_io_cmd: timeout waiting for CQE (opc=0x%02x)\n",
		  cmd->opc);
	return -ETIMEDOUT;

have_cqe:
	sc_sct = (status >> 1) & 0x7fff;

	nvme->io_cq_head = (nvme->io_cq_head + 1) % nvme->io_cq_depth;
	if (nvme->io_cq_head == 0)
		nvme->io_cq_phase ^= 1;
	rc_nvme_ring_cq_doorbell(adapter, RC_NVME_IO_QID, nvme->io_cq_head);

	if (sc_sct) {
		rc_printk(RC_ERROR,
			  "rc_nvme_io_cmd: opc=0x%02x SC/SCT=0x%04x (status=0x%04x)\n",
			  cmd->opc, sc_sct, status);
		return -EIO;
	}
	return 0;
}

/* Forward decls — defined below. */
static int rc_nvme_read_lba(struct rc_adapter *adapter, u64 slba,
			    u16 nlb_zbased, dma_addr_t buf_dma);
static int rc_volume_create_disk(void);
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
	return rc_nvme_io_cmd(adapter, &cmd);
}

/* Compare two adapters by PCI BDF for deterministic member ordering. */
static int rc_volume_bdf_cmp(struct rc_adapter *a, struct rc_adapter *b)
{
	unsigned int abdf = (a->pdev->bus->number << 8) | a->pdev->devfn;
	unsigned int bbdf = (b->pdev->bus->number << 8) | b->pdev->devfn;
	int cmp = (int)abdf - (int)bbdf;
	return rc_volume_reverse_order ? -cmp : cmp;
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

/* Called once per adapter after its metadata block validates.  Inserts the
 * adapter into the global volume registry (sorted by PCI BDF) and, on
 * reaching RC_VOLUME_EXPECTED_MEMBERS, declares the volume up and runs
 * the demo reads. */
static void rc_volume_register_member(struct rc_adapter *adapter)
{
	int pos;

	mutex_lock(&rc_volume_lock);
	if (rc_volume_member_count >= RC_VOLUME_MAX_MEMBERS) {
		rc_printk(RC_WARN,
			  "rc_volume_register_member: too many members, ignoring\n");
		goto out;
	}

	/* Stash stripe size on first member; verify identical on subsequent. */
	if (rc_volume_member_count == 0) {
		rc_volume_stripe_sectors = adapter->ctx.nvme.md_stripe_sectors;
	} else if (rc_volume_stripe_sectors !=
		   adapter->ctx.nvme.md_stripe_sectors) {
		rc_printk(RC_WARN,
			  "rc_volume_register_member: stripe-size mismatch %u vs %u — ignoring this member\n",
			  rc_volume_stripe_sectors,
			  adapter->ctx.nvme.md_stripe_sectors);
		goto out;
	}

	/* Insert sorted by PCI BDF. */
	for (pos = 0; pos < rc_volume_member_count; pos++) {
		if (rc_volume_bdf_cmp(adapter, rc_volume_members[pos]) < 0)
			break;
	}
	memmove(&rc_volume_members[pos + 1], &rc_volume_members[pos],
		(rc_volume_member_count - pos) *
		sizeof(rc_volume_members[0]));
	rc_volume_members[pos] = adapter;
	rc_volume_member_count++;

	rc_printk(RC_INFO,
		  "rc_volume_register_member: %s registered at pos %d (%d/%d)\n",
		  pci_name(adapter->pdev), pos,
		  rc_volume_member_count, RC_VOLUME_EXPECTED_MEMBERS);

	if (rc_volume_member_count == RC_VOLUME_EXPECTED_MEMBERS) {
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

/* Submit a multi-page NVMe READ for one member, using PRP1/PRP2 directly or
 * a PRP list as needed.  Up to RC_VOLUME_DATA_BYTES per call.  Reads into
 * rc_volume_dma_va[member_idx]. */
static int rc_volume_read_member(int member_idx, u64 slba, u16 nlb_zbased)
{
	struct rc_adapter *adapter = rc_volume_members[member_idx];
	u32 bytes = ((u32)nlb_zbased + 1) * 512u;
	u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	dma_addr_t data_dma = rc_volume_dma_pa[member_idx];
	struct rc_nvme_sqe cmd;

	if (WARN_ON_ONCE(bytes > RC_VOLUME_DATA_BYTES))
		return -EINVAL;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_NVM_OP_READ;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(data_dma);
	cmd.cdw10 = cpu_to_le32((u32)(slba & 0xffffffff));
	cmd.cdw11 = cpu_to_le32((u32)(slba >> 32));
	cmd.cdw12 = cpu_to_le32(nlb_zbased);

	if (pages == 2) {
		cmd.prp2 = cpu_to_le64(data_dma + PAGE_SIZE);
	} else if (pages > 2) {
		__le64 *list = rc_volume_prp_va[member_idx];
		u32 i;
		for (i = 0; i + 1 < pages; i++)
			list[i] = cpu_to_le64(data_dma + (i + 1) * PAGE_SIZE);
		cmd.prp2 = cpu_to_le64(rc_volume_prp_pa[member_idx]);
	}
	return rc_nvme_io_cmd(adapter, &cmd);
}

/* blk-mq request handler.  With chunk_sectors == stripe_sectors set in the
 * queue limits, every request is guaranteed to stay within a single stripe
 * and therefore maps to one member.  So we issue exactly one NVMe READ per
 * request (NLB = nr_sectors-1) into the per-member persistent DMA buffer,
 * then copy out to the bvecs.  Read-only for now.  Sleep is OK here. */
static blk_status_t rc_volume_queue_rq(struct blk_mq_hw_ctx *hctx,
				       const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct bio_vec bvec;
	struct req_iterator iter;
	sector_t pos = blk_rq_pos(req);
	unsigned int nr_sectors = blk_rq_sectors(req);
	int member_idx;
	u64 phys_lba;
	int ret;
	size_t off;

	blk_mq_start_request(req);

	if (req_op(req) != REQ_OP_READ) {
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	if (nr_sectors == 0 || nr_sectors > (RC_VOLUME_DATA_BYTES / 512) ||
	    pos + nr_sectors >
	    rc_volume_members[0]->ctx.nvme.ns1_nsze * rc_volume_member_count) {
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	rc_volume_map_lba(pos, &member_idx, &phys_lba);
	ret = rc_volume_read_member(member_idx, phys_lba, (u16)(nr_sectors - 1));
	if (ret) {
		blk_mq_end_request(req, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	off = 0;
	rq_for_each_segment(bvec, req, iter) {
		u8 *kaddr = kmap_local_page(bvec.bv_page);
		memcpy(kaddr + bvec.bv_offset,
		       (u8 *)rc_volume_dma_va[member_idx] + off,
		       bvec.bv_len);
		kunmap_local(kaddr);
		off += bvec.bv_len;
	}

	blk_mq_end_request(req, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops rc_volume_mq_ops = {
	.queue_rq = rc_volume_queue_rq,
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

	/* Per-member: a 64 KiB data buffer plus a PAGE_SIZE PRP-list buffer.
	 * Allows one NVMe READ of up to 16 contiguous pages.  Queue depth = 1
	 * so a single buffer per member is safe. */
	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		rc_volume_dma_va[i] =
			dma_alloc_coherent(dev, RC_VOLUME_DATA_BYTES,
					   &rc_volume_dma_pa[i], GFP_KERNEL);
		if (!rc_volume_dma_va[i]) {
			ret = -ENOMEM;
			goto err_free_dma;
		}
		rc_volume_prp_va[i] =
			dma_alloc_coherent(dev, PAGE_SIZE,
					   &rc_volume_prp_pa[i], GFP_KERNEL);
		if (!rc_volume_prp_va[i]) {
			ret = -ENOMEM;
			goto err_free_dma;
		}
	}

	memset(&rc_volume_tagset, 0, sizeof(rc_volume_tagset));
	rc_volume_tagset.ops          = &rc_volume_mq_ops;
	rc_volume_tagset.nr_hw_queues = 1;
	rc_volume_tagset.queue_depth  = 1;
	rc_volume_tagset.numa_node    = NUMA_NO_NODE;
	rc_volume_tagset.cmd_size     = 0;
	/* queue_rq polls for NVMe CQE completion via usleep_range, so it
	 * must run in blocking dispatch context — not inside blk-mq's
	 * RCU/SRCU read-side critical section. */
	rc_volume_tagset.flags        = BLK_MQ_F_BLOCKING;

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
			 * request maps to one member and one NVMe READ. */
			.chunk_sectors       = rc_volume_stripe_sectors,
		};

		rc_volume_disk = blk_mq_alloc_disk(&rc_volume_tagset, &lim, NULL);
	}
	if (IS_ERR(rc_volume_disk)) {
		ret = PTR_ERR(rc_volume_disk);
		rc_volume_disk = NULL;
		goto err_free_tagset;
	}

	total_sectors = m0->ctx.nvme.ns1_nsze *
			(u64)rc_volume_member_count;
	set_capacity(rc_volume_disk, total_sectors);
	snprintf(rc_volume_disk->disk_name, DISK_NAME_LEN, "rcraid0");
	rc_volume_disk->fops        = &rc_volume_bops;
	rc_volume_disk->major       = rc_major;
	rc_volume_disk->minors      = 1;
	rc_volume_disk->first_minor = 0;
	rc_volume_disk->flags       = GENHD_FL_NO_PART;
	/* Read-only — writes would corrupt the array. */
	set_disk_ro(rc_volume_disk, 1);

	ret = add_disk(rc_volume_disk);
	if (ret)
		goto err_put_disk;

	rc_printk(RC_NOTE,
		  "rc_volume_create_disk: /dev/%s up, %llu sectors (%llu MiB, read-only)\n",
		  rc_volume_disk->disk_name,
		  (unsigned long long)total_sectors,
		  (unsigned long long)(total_sectors >> 11));
	return 0;

err_put_disk:
	put_disk(rc_volume_disk);
	rc_volume_disk = NULL;
err_free_tagset:
	blk_mq_free_tag_set(&rc_volume_tagset);
err_free_dma:
	for (i = 0; i < rc_volume_member_count; i++) {
		struct device *dev = &rc_volume_members[i]->pdev->dev;
		if (rc_volume_dma_va[i]) {
			dma_free_coherent(dev, RC_VOLUME_DATA_BYTES,
					  rc_volume_dma_va[i],
					  rc_volume_dma_pa[i]);
			rc_volume_dma_va[i] = NULL;
		}
		if (rc_volume_prp_va[i]) {
			dma_free_coherent(dev, PAGE_SIZE,
					  rc_volume_prp_va[i],
					  rc_volume_prp_pa[i]);
			rc_volume_prp_va[i] = NULL;
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
			if (rc_volume_dma_va[i]) {
				dma_free_coherent(dev, RC_VOLUME_DATA_BYTES,
						  rc_volume_dma_va[i],
						  rc_volume_dma_pa[i]);
				rc_volume_dma_va[i] = NULL;
				rc_volume_dma_pa[i] = 0;
			}
			if (rc_volume_prp_va[i]) {
				dma_free_coherent(dev, PAGE_SIZE,
						  rc_volume_prp_va[i],
						  rc_volume_prp_pa[i]);
				rc_volume_prp_va[i] = NULL;
				rc_volume_prp_pa[i] = 0;
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
