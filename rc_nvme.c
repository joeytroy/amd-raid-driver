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
		const u8 *b = id_buf;
		u16 vid   = le16_to_cpup((const __le16 *)(b + RC_NVME_ID_CTRL_VID));
		u16 ssvid = le16_to_cpup((const __le16 *)(b + RC_NVME_ID_CTRL_SSVID));
		u32 nn    = le32_to_cpup((const __le32 *)(b + RC_NVME_ID_CTRL_NN));
		char sn[21], mn[41], fr[9];

		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_SN, 20, sn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_MN, 40, mn);
		rc_nvme_ascii_field(b + RC_NVME_ID_CTRL_FR, 8,  fr);

		rc_printk(RC_NOTE,
			  "rc_nvme_identify_controller: VID=0x%04x SSVID=0x%04x SN='%s' MN='%s' FR='%s' NN=%u\n",
			  vid, ssvid, sn, mn, fr, nn);
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

/* Forward decl — defined below; read_validate_metadata calls it. */
static int rc_nvme_read_lba(struct rc_adapter *adapter, u64 slba,
			    u16 nlb_zbased, dma_addr_t buf_dma);

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

	nvme->md_member_uuid     = le64_to_cpu(md->member_uuid);
	nvme->md_fld_18          = le64_to_cpu(md->fld_18);
	nvme->md_fld_20          = le64_to_cpu(md->fld_20);
	nvme->md_stripe_sectors  = le32_to_cpu(md->stripe_sectors);
	nvme->md_version         = version;
	nvme->md_fld_30          = le64_to_cpu(md->fld_30);
	nvme->md_fld_38          = le64_to_cpu(md->fld_38);
	nvme->md_valid           = true;

	rc_printk(RC_NOTE,
		  "rc_nvme_read_validate_metadata: RAIDCore v0x%08x stripe=%u sectors (%u KiB) member=0x%016llx fld18=0x%016llx fld20=0x%016llx fld30=0x%016llx fld38=0x%016llx\n",
		  version, nvme->md_stripe_sectors,
		  (nvme->md_stripe_sectors * nvme->ns1_lba_bytes) >> 10,
		  (unsigned long long)nvme->md_member_uuid,
		  (unsigned long long)nvme->md_fld_18,
		  (unsigned long long)nvme->md_fld_20,
		  (unsigned long long)nvme->md_fld_30,
		  (unsigned long long)nvme->md_fld_38);
	ret = 0;

out:
	dma_free_coherent(dev, PAGE_SIZE, buf, buf_dma);
	return ret;
}

/* Read 8 LBAs starting at slba into the given buffer (must be DMA-coherent
 * and at least 8 * lba_bytes large; PAGE_SIZE is plenty for 512 B LBAs). */
static int rc_nvme_read_lba(struct rc_adapter *adapter, u64 slba, u16 nlb_zbased,
			    dma_addr_t buf_dma)
{
	struct rc_nvme_sqe cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc   = RC_NVME_NVM_OP_READ;
	cmd.nsid  = cpu_to_le32(1);
	cmd.prp1  = cpu_to_le64(buf_dma);
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
	return (int)abdf - (int)bbdf;
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
 * is wiring up physical reads to the expected member. */
static void rc_volume_demo_reads(void)
{
	/* Use member 0's device for the DMA buffer; either works. */
	struct rc_adapter *adapter = rc_volume_members[0];
	struct device *dev = &adapter->pdev->dev;
	void *buf;
	dma_addr_t buf_dma;
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

	buf = dma_alloc_coherent(dev, PAGE_SIZE, &buf_dma, GFP_KERNEL);
	if (!buf)
		return;

	rc_printk(RC_NOTE,
		  "rc_volume_demo_reads: volume up — %d members, stripe=%u sectors\n",
		  rc_volume_member_count, stripe);

	for (i = 0; i < (int)ARRAY_SIZE(probes); i++) {
		int member_idx;
		u64 phys_lba;
		int ret;

		rc_volume_map_lba(probes[i], &member_idx, &phys_lba);
		memset(buf, 0, 16);
		ret = rc_volume_read_lba(probes[i], buf_dma);
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
			       16, 1, buf, 16, true);
	}

	dma_free_coherent(dev, PAGE_SIZE, buf, buf_dma);
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

	if (rc_volume_member_count == RC_VOLUME_EXPECTED_MEMBERS)
		rc_volume_demo_reads();
out:
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
