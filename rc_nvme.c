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

	return 0;
}

void rc_nvme_cleanup_controller(struct rc_adapter *adapter)
{
	void __iomem *base = adapter->ctx.mmio_base;

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
