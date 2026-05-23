// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — legacy queue management (StorPort port)
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Pre-NVMe-rewrite scaffolding that approximated the Windows
 * StorPort service slots for the AHCI command path.  Most functions
 * here are not exercised on the active NVMe code path; slated for
 * removal or rewrite when SATA RAID support lands.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

#include "rc_linux.h"
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/byteorder/generic.h>

#define RC_AHCI_CMD_HEADER_SIZE	0x20
#define RC_AHCI_CMD_LIST_BYTES	0x400
#define RC_AHCI_CMD_TABLE_STRIDE	0x2080
#define RC_AHCI_FIS_LEN_DWORDS	5
#define RC_FIS_TYPE_REG_H2D		0x27

/*------------------------------------------------------------------
 * Queue descriptor structures (mirrors Windows devExt layouts)
 *------------------------------------------------------------------*/

struct rc_queue_descriptor {
    void *control_block;        // Host command list buffer (0x400 bytes)
    dma_addr_t control_dma;
    u32 queue_index;
    u32 queue_depth;
    u32 flags;
    u32 state;
    void *completion_ptr;
    dma_addr_t completion_dma;
    void *cmd_list;             // alias of control_block for clarity
    dma_addr_t cmd_list_dma;
    u32 cmd_list_len;
    void *fis;                  // received FIS buffer
    dma_addr_t fis_dma;
    u32 fis_len;
    u32 completion_depth;
    void *cmd_table;            // command table/PRDT region
    dma_addr_t cmd_table_dma;
    u32 cmd_table_stride;
    unsigned long slot_in_use;
    spinlock_t lock;
    u32 head;
    u32 tail;
    u32 comp_head;
    u32 comp_tail;
    atomic_t pending;
} __packed;

struct rc_queue_table {
    struct rc_queue_descriptor *descriptors[RC_MAX_QUEUE_DESCRIPTORS];
    spinlock_t table_lock;
    u32 num_allocated;
};

struct rc_ahci_cmd_header {
	__le16 flags;
	__le16 prdtl;
	__le32 prdbc;
	__le32 ctba;
	__le32 ctbau;
	__le32 reserved[4];
} __packed;

struct rc_ahci_prdt_entry {
	__le32 dba;
	__le32 dbau;
	__le32 reserved;
	__le32 dbc;
} __packed;
static void rc_ahci_build_fis(const struct rc_hw_command *cmd, u8 *cfis)
{
	memset(cfis, 0, 0x80);
	cfis[0] = RC_FIS_TYPE_REG_H2D;
	cfis[1] = BIT(7); /* Command bit */
	cfis[2] = 0xB0;   /* Vendor/SMART command */
	cfis[3] = cmd->opcode & 0xff; /* Features (7:0) */
	cfis[4] = cmd->lba & 0xff;
	cfis[5] = (cmd->lba >> 8) & 0xff;
	cfis[6] = (cmd->lba >> 16) & 0xff;
	cfis[7] = 0x40 | (cmd->channel_id & 0x0f); /* Device: LBA mode + target */
	cfis[8] = (cmd->lba >> 24) & 0xff;
	cfis[9] = (cmd->lba >> 32) & 0xff;
	cfis[10] = (cmd->lba >> 40) & 0xff;
	cfis[11] = (cmd->opcode >> 8) & 0xff; /* Features (15:8) */
	cfis[12] = cmd->sector_count & 0xff;
	cfis[13] = (cmd->sector_count >> 8) & 0xff;
	cfis[14] = 0; /* ICC / control - unused */
	cfis[15] = 0;
}

/*
 * rc_ahci_build_mailbox - Build vendor mailbox for TRX50 firmware
 * 
 * Mirrors Windows rcbottom.sys FUN_140001008 logic exactly.
 * The mailbox is embedded in the AHCI command table at offsets 0x10e-0x154.
 * 
 * @table: Pointer to command table base (param_2 from Windows driver)
 * @cmd: Command structure (used to build descriptor/payload)
 * @extended_mode: Enable extended mode (0x14-byte payload vs 0x8-byte) (param_5)
 * @set_extended_flag: Controls flag bit 0x20000 at +0x154 (param_6)
 * 
 * Windows driver signature: FUN_140001008(RCX=param_1, RDX=param_2, R8=param_3, 
 *                                          R9=param_4, stack[0x28]=param_5, 
 *                                          stack[0x30]=param_6)
 */
static void rc_ahci_build_mailbox(u8 *table, const struct rc_hw_command *cmd,
                                  bool extended_mode, bool set_extended_flag)
{
	u8 *mailbox;
	u8 *completion_flags;
	u8 *cmd_type;
	u8 *control_flags;
	u8 *payload_length;
	u8 *secondary_control;
	u32 *payload;
	u32 *extended_flags;
	u16 completion_value = 0;
	bool set_completion = false;

	/* Mailbox spans table + 0x10e through table + 0x154 (70 bytes) */
	mailbox = table + 0x10e;
	memset(mailbox, 0, 0x47);  /* Clear mailbox region (0x10e to 0x154) */

	/* Set up pointers to mailbox fields */
	completion_flags = mailbox + 0x00;  /* +0x10e */
	cmd_type = mailbox + 0x02;          /* +0x110 */
	control_flags = mailbox + 0x03;     /* +0x111 */
	payload_length = mailbox + 0x04;    /* +0x112 */
	secondary_control = mailbox + 0x05; /* +0x113 */
	payload = (u32 *)(mailbox + 0x06);  /* +0x114 */
	extended_flags = (u32 *)(mailbox + 0x46); /* +0x154 */

	/* Build command descriptor (8 bytes) - param_3 equivalent */
	/* For now, we'll use the 0x34 path since we have command opcode */
	/* In real usage, caller would provide proper descriptor/payload */

	/* Command payload path (param_4 != NULL && param_4[0] == 0x34) */
	/* This is the most common path for RAID commands */
	payload[0] = cpu_to_le32(0x34);  /* Magic byte */
	payload[1] = cpu_to_le32(0x00);  /* Reserved */
	*control_flags = 0x00;            /* Default control flags */
	*secondary_control = 0x00;        /* Default secondary control */
	*payload_length = 0x14;           /* 20-byte payload */

	/* Copy command opcode into payload */
	payload[1] = cpu_to_le32(cmd->opcode);
	if (cmd->opcode != 0) {
		/* Set control flags based on command type */
		*control_flags = 0x01;  /* Enable command processing */
		*secondary_control = 0x00;
	}

	/* Extended flags (param_5 == true && param_6 == false) */
	if (extended_mode && !set_extended_flag) {
		*extended_flags |= cpu_to_le32(0x20000);  /* Set bit 17 */
	}

	/* Completion flags calculation (complex conditional logic) */
	if (*control_flags & 0x20) {
		/* If control_flags & 0x20: write 0x4400 */
		completion_value = cpu_to_le16(0x4400);
		set_completion = true;
	} else if (*control_flags & 0x01) {
		/* Else if control_flags & 0x01: */
		if (*secondary_control & 0x40) {
			/* If secondary_control & 0x40: write 0x1100 */
			completion_value = cpu_to_le16(0x1100);
			set_completion = true;
		} else if (*secondary_control & 0x10) {
			/* Else if secondary_control & 0x10: write 0x1400 */
			completion_value = cpu_to_le16(0x1400);
			set_completion = true;
		} else if ((*secondary_control & 0x04) || ((s8)*secondary_control < 0)) {
			/* Else if secondary_control & 0x04 or secondary_control < 0: write 0x4703 */
			completion_value = cpu_to_le16(0x4703);
			set_completion = true;
		} else {
			/* Else: write 0x0000 */
			completion_value = 0;
			set_completion = true;
		}
	} else if (!extended_mode) {
		/* Else if param_5 == false: write 0x0000 */
		completion_value = 0;
		set_completion = true;
	} else {
		/* Else: write 0x0002 to cmd_type only (not completion_flags) */
		*cmd_type = 0x02;
		set_completion = false;
	}

	if (set_completion) {
		*(u16 *)completion_flags = completion_value;
		*cmd_type = 0x02;  /* Set cmd_type when flags are set */
	} else {
		*cmd_type = 0x00;  /* Clear cmd_type when flags not set */
	}

	rc_printk(RC_DEBUG, "Mailbox @0x10e: completion_flags=0x%04x cmd_type=0x%02x "
	          "control=0x%02x secondary=0x%02x length=0x%02x opcode=0x%08x "
	          "extended=0x%08x\n",
	          le16_to_cpu(*(u16 *)completion_flags), *cmd_type,
	          *control_flags, *secondary_control, *payload_length,
	          le32_to_cpu(payload[1]), le32_to_cpu(*extended_flags));
}

static void rc_ahci_prepare_slot(struct rc_queue_descriptor *desc,
                                 const struct rc_hw_command *cmd,
                                 u32 slot, bool data_in, bool data_out)
{
    struct rc_ahci_cmd_header *hdr;
    u8 *table;
    u64 tbl_dma;
    u16 hdr_flags = RC_AHCI_FIS_LEN_DWORDS & 0x1f;

    hdr = (struct rc_ahci_cmd_header *)((u8 *)desc->cmd_list +
                                        slot * RC_AHCI_CMD_HEADER_SIZE);
    table = (u8 *)desc->cmd_table + (slot * desc->cmd_table_stride);
    tbl_dma = desc->cmd_table_dma + (slot * desc->cmd_table_stride);

    memset(hdr, 0, sizeof(*hdr));
    memset(table, 0, desc->cmd_table_stride);

    if (data_out)
        hdr_flags |= BIT(6);
    hdr->flags = cpu_to_le16(hdr_flags);
    if (cmd->sector_count && cmd->data_addr)
        hdr->prdtl = cpu_to_le16(1);
    hdr->ctba = cpu_to_le32(lower_32_bits(tbl_dma));
    hdr->ctbau = cpu_to_le32(upper_32_bits(tbl_dma));

    rc_ahci_build_fis(cmd, table);

    /* Build vendor mailbox for TRX50 firmware */
    /* Match Windows caller: FUN_140001180 calls with param_5=1, param_6=0 */
    rc_ahci_build_mailbox(table, cmd, true, false);

    if (cmd->sector_count && cmd->data_addr) {
        struct rc_ahci_prdt_entry *prdt;
        u32 byte_count = cmd->sector_count * 512;

        prdt = (struct rc_ahci_prdt_entry *)(table + 0x80);
        prdt->dba = cpu_to_le32(lower_32_bits(cmd->data_addr));
        prdt->dbau = cpu_to_le32(upper_32_bits(cmd->data_addr));
        if (byte_count)
            prdt->dbc = cpu_to_le32((byte_count - 1) | BIT(31));
    }
}

/*----------------------------------------------------------------------
 * StorPort slot +0x1F8: Allocate queue descriptor (FUN_14000d66c)
 *----------------------------------------------------------------------*/

static struct rc_queue_descriptor *
rc_alloc_queue_descriptor(struct rc_adapter *adapter, u32 queue_index)
{
    struct rc_queue_descriptor *desc;
    struct rc_queue_table *table = adapter->ctx.descriptor_table;
    unsigned long flags;

    if (!table) {
        rc_printk(RC_ERROR, "rc_alloc_queue_descriptor: table not initialized\n");
        return NULL;
    }

    if (queue_index >= RC_MAX_QUEUE_DESCRIPTORS) {
        rc_printk(RC_ERROR, "rc_alloc_queue_descriptor: index %u out of range\n",
                  queue_index);
        return NULL;
    }

    desc = kzalloc(sizeof(*desc), GFP_KERNEL);
    if (!desc)
        return NULL;

    desc->queue_index = queue_index;
    desc->queue_depth = 32;  // Default depth from Windows
    desc->flags = 0;
    desc->state = 0;
    desc->control_block = NULL;
    desc->control_dma = 0;
    desc->completion_ptr = NULL;
    desc->completion_dma = 0;
    desc->cmd_list = NULL;
    desc->cmd_list_dma = 0;
    desc->cmd_list_len = 0;
    desc->fis = NULL;
    desc->fis_dma = 0;
    desc->fis_len = 0;
    desc->completion_depth = 0;
    desc->cmd_table = NULL;
    desc->cmd_table_dma = 0;
    desc->cmd_table_stride = 0;
    desc->slot_in_use = 0;
    spin_lock_init(&desc->lock);
    atomic_set(&desc->pending, 0);

    spin_lock_irqsave(&table->table_lock, flags);
    if (table->descriptors[queue_index]) {
        spin_unlock_irqrestore(&table->table_lock, flags);
        rc_printk(RC_WARN, "rc_alloc_queue_descriptor: slot %u already allocated\n",
                  queue_index);
        kfree(desc);
        return table->descriptors[queue_index];
    }

    table->descriptors[queue_index] = desc;
    table->num_allocated++;
    spin_unlock_irqrestore(&table->table_lock, flags);

    rc_printk(RC_INFO, "rc_alloc_queue_descriptor: allocated queue %u\n", queue_index);
    return desc;
}

/*----------------------------------------------------------------------
 * StorPort slot +0x3F0: Allocate DMA control block (FUN_14000655a)
 *----------------------------------------------------------------------*/

static int
rc_alloc_queue_control_block(struct rc_adapter *adapter,
                              struct rc_queue_descriptor *desc)
{
    struct pci_dev *pdev = adapter->pdev;
    void *block;
    dma_addr_t dma_addr;

    if (!desc) {
        rc_printk(RC_ERROR, "rc_alloc_queue_control_block: null descriptor\n");
        return -EINVAL;
    }

    // Allocate 0x400 bytes (1024) coherent DMA memory
    block = dma_alloc_coherent(&pdev->dev, 0x400, &dma_addr, GFP_KERNEL);
    if (!block) {
        rc_printk(RC_ERROR, "rc_alloc_queue_control_block: DMA alloc failed\n");
        return -ENOMEM;
    }

    memset(block, 0, 0x400);

    desc->control_block = block;
    desc->control_dma = dma_addr;
    desc->cmd_list = block;
    desc->cmd_list_dma = dma_addr;
    desc->cmd_list_len = RC_AHCI_CMD_LIST_BYTES;

    desc->fis_len = 0x100;
    desc->fis = dma_alloc_coherent(&pdev->dev, desc->fis_len,
                                   &desc->fis_dma, GFP_KERNEL);
    if (!desc->fis) {
        rc_printk(RC_ERROR, "rc_alloc_queue_control_block: FIS alloc failed\n");
        dma_free_coherent(&pdev->dev, 0x400, block, dma_addr);
        desc->control_block = NULL;
        desc->control_dma = 0;
        desc->cmd_list = NULL;
        desc->cmd_list_dma = 0;
        desc->cmd_list_len = 0;
        desc->fis_dma = 0;
        desc->fis_len = 0;
        return -ENOMEM;
    }
    memset(desc->fis, 0, desc->fis_len);

    desc->cmd_table_stride = RC_AHCI_CMD_TABLE_STRIDE;
    {
        size_t table_len = desc->queue_depth * desc->cmd_table_stride;

        desc->cmd_table = dma_alloc_coherent(&pdev->dev, table_len,
                                             &desc->cmd_table_dma, GFP_KERNEL);
        if (!desc->cmd_table) {
            rc_printk(RC_ERROR, "rc_alloc_queue_control_block: command table alloc failed\n");
            dma_free_coherent(&pdev->dev, desc->fis_len, desc->fis, desc->fis_dma);
            desc->fis = NULL;
            desc->fis_dma = 0;
            desc->fis_len = 0;
            dma_free_coherent(&pdev->dev, 0x400, block, dma_addr);
            desc->control_block = NULL;
            desc->control_dma = 0;
            desc->cmd_list = NULL;
            desc->cmd_list_dma = 0;
            desc->cmd_list_len = 0;
            return -ENOMEM;
        }
        memset(desc->cmd_table, 0, table_len);
    }

    rc_printk(RC_INFO, "rc_alloc_queue_control_block: queue %u block at 0x%llx\n",
              desc->queue_index, (unsigned long long)dma_addr);

    return 0;
}

/*----------------------------------------------------------------------
 * StorPort slot +0x9D8: Program completion register sets (FUN_1400023BB)
 *----------------------------------------------------------------------*/

static int
rc_program_completion_registers(struct rc_adapter *adapter,
                                 struct rc_queue_descriptor *desc)
{
    void __iomem *base = adapter->ctx.mmio_base;
    u32 queue_idx = desc->queue_index;
    u32 offsets[4];
    const char *labels[] = {
        "comp_base_lo",
        "comp_base_hi",
        "comp_size",
        "comp_enable",
    };
    u32 values[4];
    size_t i;

    if (!base) {
        rc_printk(RC_ERROR, "rc_program_completion_registers: no MMIO base\n");
        return -EINVAL;
    }

    offsets[0] = 0x100 + (queue_idx * 0x10);
    offsets[1] = 0x104 + (queue_idx * 0x10);
    offsets[2] = 0x108 + (queue_idx * 0x10);
    offsets[3] = 0x10C + (queue_idx * 0x10);

    values[0] = lower_32_bits(desc->completion_dma);
    values[1] = upper_32_bits(desc->completion_dma);
    values[2] = desc->queue_depth;
    values[3] = 0x1;

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        u32 before = readl(base + offsets[i]);
        writel(values[i], base + offsets[i]);
        rc_printk(RC_INFO,
                  "rc_program_completion_registers: q%u %s offset=0x%03x before=0x%08x write=0x%08x after=0x%08x\n",
                  queue_idx, labels[i], offsets[i], before, values[i], readl(base + offsets[i]));
    }

    rc_printk(RC_INFO, "rc_program_completion_registers: queue %u configured\n",
              queue_idx);

    return 0;
}

static int rc_program_port_registers(struct rc_adapter *adapter,
                                     struct rc_queue_descriptor *desc)
{
    void __iomem *base = adapter->ctx.mmio_base;
    void __iomem *port;
    u32 cmd;
    int timeout;

    if (!base || !desc || !desc->cmd_list || !desc->fis)
        return -EINVAL;

    port = base + RC_PORT_REG_BASE(desc->queue_index);

    /* Ensure the port is idle before reprogramming. */
    cmd = readl(port + RC_PORT_CMD);
    if (cmd & RC_PORT_CMD_ST) {
        cmd &= ~RC_PORT_CMD_ST;
        writel(cmd, port + RC_PORT_CMD);
        wmb();
    }

    timeout = 1000;
    while ((readl(port + RC_PORT_CMD) & RC_PORT_CMD_CR) && --timeout)
        udelay(50);
    if (timeout <= 0) {
        rc_printk(RC_ERROR, "rc_program_port_registers: port %u busy (CR set)\n",
                  desc->queue_index);
        return -EBUSY;
    }

    cmd = readl(port + RC_PORT_CMD);
    if (cmd & RC_PORT_CMD_FRE) {
        cmd &= ~RC_PORT_CMD_FRE;
        writel(cmd, port + RC_PORT_CMD);
        wmb();
    }

    timeout = 1000;
    while ((readl(port + RC_PORT_CMD) & RC_PORT_CMD_FR) && --timeout)
        udelay(50);
    if (timeout <= 0) {
        rc_printk(RC_ERROR, "rc_program_port_registers: port %u busy (FR set)\n",
                  desc->queue_index);
        return -EBUSY;
    }

    writel(lower_32_bits(desc->cmd_list_dma), port + RC_PORT_CLB);
    writel(upper_32_bits(desc->cmd_list_dma), port + RC_PORT_CLBU);
    writel(lower_32_bits(desc->fis_dma), port + RC_PORT_FB);
    writel(upper_32_bits(desc->fis_dma), port + RC_PORT_FBU);

    desc->slot_in_use = 0;
    memset(desc->cmd_list, 0, desc->cmd_list_len);
    if (desc->cmd_table)
        memset(desc->cmd_table, 0, desc->queue_depth * desc->cmd_table_stride);

    writel(0xffffffff, port + RC_PORT_IS);
    writel(0, port + RC_PORT_SACT);
    writel(0, port + RC_PORT_CI);

    wmb();

    cmd = readl(port + RC_PORT_CMD);
    cmd |= RC_PORT_CMD_FRE;
    writel(cmd, port + RC_PORT_CMD);
    wmb();

    cmd |= RC_PORT_CMD_ST;
    writel(cmd, port + RC_PORT_CMD);
    wmb();

    return 0;
}

static void rc_queue_stop_port(struct rc_adapter *adapter,
                               struct rc_queue_descriptor *desc)
{
    void __iomem *base = adapter->ctx.mmio_base;
    void __iomem *port;
    u32 cmd;
    int timeout;

    if (!base || !desc)
        return;

    port = base + RC_PORT_REG_BASE(desc->queue_index);
    cmd = readl(port + RC_PORT_CMD);
    if (cmd & (RC_PORT_CMD_ST | RC_PORT_CMD_FRE)) {
        cmd &= ~(RC_PORT_CMD_ST | RC_PORT_CMD_FRE);
        writel(cmd, port + RC_PORT_CMD);
        wmb();
    }

    timeout = 1000;
    while ((readl(port + RC_PORT_CMD) & (RC_PORT_CMD_CR | RC_PORT_CMD_FR)) && --timeout)
        udelay(50);
    if (timeout <= 0)
        rc_printk(RC_WARN, "rc_queue_stop_port: port %u did not quiesce\n",
                  desc->queue_index);

    desc->slot_in_use = 0;
}

/*----------------------------------------------------------------------
 * StorPort slot +0x188: Ring firmware doorbells (indices 1-4)
 *----------------------------------------------------------------------*/

static void
rc_ring_doorbell(struct rc_adapter *adapter, u32 doorbell_index)
{
    void __iomem *base = adapter->ctx.mmio_base;
    void __iomem *doorbell_page = adapter->ctx.doorbell.doorbell_page;

    if (!base) {
        rc_printk(RC_ERROR, "rc_ring_doorbell: no MMIO base\n");
        return;
    }

    // Use doorbell_page if configured, otherwise fall back to base offset
    if (doorbell_page) {
        writel(doorbell_index, doorbell_page + (doorbell_index * 4));
    } else {
        writel(doorbell_index, base + RC_REG_DOORBELL + (doorbell_index * 4));
    }

    rc_printk(RC_DEBUG, "rc_ring_doorbell: rang doorbell %u\n", doorbell_index);
}

/*----------------------------------------------------------------------
 * Queue initialization & teardown
 *----------------------------------------------------------------------*/

int rc_queue_init(struct rc_adapter *adapter)
{
    struct rc_queue_table *table;
    struct rc_queue_descriptor *desc;
    int ret;

    rc_printk(RC_INFO, "rc_queue_init: initializing queue subsystem\n");

    // Allocate descriptor table (devExt+0x1C2A0)
    table = kzalloc(sizeof(*table), GFP_KERNEL);
    if (!table)
        return -ENOMEM;

    spin_lock_init(&table->table_lock);
    table->num_allocated = 0;
    adapter->ctx.descriptor_table = table;

    // Allocate primary queue (index 0)
    desc = rc_alloc_queue_descriptor(adapter, 0);
    if (!desc) {
        ret = -ENOMEM;
        goto err_free_table;
    }

    // Allocate control block for primary queue
    ret = rc_alloc_queue_control_block(adapter, desc);
    if (ret)
        goto err_free_desc;

    desc->completion_ptr = adapter->hw.comp_queue;
    desc->completion_dma = adapter->hw.comp_queue_dma;
    desc->completion_depth = adapter->hw.comp_queue_size;
    desc->queue_depth = adapter->hw.cmd_queue_size;

    // Program completion registers
    ret = rc_program_completion_registers(adapter, desc);
    if (ret)
        goto err_free_control;

    ret = rc_program_port_registers(adapter, desc);
    if (ret)
        goto err_free_control;

    // Store primary queue handle (devExt+0x6B8)
    adapter->ctx.irq.primary_queue = desc;

    // Allocate queue handle array (devExt+0x6C0)
    adapter->ctx.irq.queue_table = kzalloc(
        sizeof(void *) * RC_MAX_QUEUE_DESCRIPTORS, GFP_KERNEL);
    if (!adapter->ctx.irq.queue_table) {
        ret = -ENOMEM;
        goto err_free_control;
    }
    adapter->ctx.irq.queue_table[0] = desc;

    rc_printk(RC_INFO, "rc_queue_init: initialized %u queues\n",
              table->num_allocated);

    return 0;

err_free_control:
    if (desc->cmd_table)
        dma_free_coherent(&adapter->pdev->dev,
                          desc->queue_depth * desc->cmd_table_stride,
                          desc->cmd_table, desc->cmd_table_dma);
    if (desc->fis)
        dma_free_coherent(&adapter->pdev->dev, desc->fis_len,
                          desc->fis, desc->fis_dma);
    if (desc->control_block)
        dma_free_coherent(&adapter->pdev->dev, 0x400,
                          desc->control_block, desc->control_dma);
err_free_desc:
    kfree(desc);
err_free_table:
    kfree(table);
    adapter->ctx.descriptor_table = NULL;
    return ret;
}

void rc_queue_cleanup(struct rc_adapter *adapter)
{
    struct rc_queue_table *table = adapter->ctx.descriptor_table;
    u32 i;

    if (!table)
        return;

    rc_printk(RC_INFO, "rc_queue_cleanup: cleaning up %u queues\n",
              table->num_allocated);

    for (i = 0; i < RC_MAX_QUEUE_DESCRIPTORS; i++) {
        struct rc_queue_descriptor *desc = table->descriptors[i];
        if (!desc)
            continue;

        rc_queue_stop_port(adapter, desc);

        if (desc->cmd_table)
            dma_free_coherent(&adapter->pdev->dev,
                              desc->queue_depth * desc->cmd_table_stride,
                              desc->cmd_table, desc->cmd_table_dma);

        if (desc->fis) {
            dma_free_coherent(&adapter->pdev->dev, desc->fis_len,
                              desc->fis, desc->fis_dma);
            desc->fis = NULL;
        }

        if (desc->control_block)
            dma_free_coherent(&adapter->pdev->dev, 0x400,
                              desc->control_block, desc->control_dma);
        kfree(desc);
        table->descriptors[i] = NULL;
    }

    kfree(adapter->ctx.irq.queue_table);
    adapter->ctx.irq.queue_table = NULL;
    adapter->ctx.irq.primary_queue = NULL;

    kfree(table);
    adapter->ctx.descriptor_table = NULL;
}

/*----------------------------------------------------------------------
 * Doorbell activation sequence (FUN_14000924c)
 *----------------------------------------------------------------------*/

int rc_activate_doorbells(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;

    rc_printk(RC_INFO, "rc_activate_doorbells: activating queue doorbells\n");

    // Set adapter active flag (devExt+0x16054)
    ctx->doorbell.adapter_active = true;

    // 5 µs stall before first doorbell sequence
    udelay(5);

    // Ring doorbells in order: 1, 4, 2, 3 (per FUN_140008638 analysis)
    // Windows driver calls service slot +0x188 with this exact order
    rc_ring_doorbell(adapter, 1);
    rc_ring_doorbell(adapter, 4);
    rc_ring_doorbell(adapter, 2);
    rc_ring_doorbell(adapter, 3);

    // Mark firmware capability active (devExt+0x1C2DC)
    ctx->doorbell.fast_path_enabled = true;

    rc_printk(RC_INFO, "rc_activate_doorbells: doorbells activated\n");

    return 0;
}

int rc_queue_issue_sync(struct rc_adapter *adapter,
                       struct rc_hw_command *cmd,
                       bool data_in, bool data_out,
                       unsigned int timeout_ms,
                       struct rc_hw_completion *completion)
{
    struct rc_queue_descriptor *desc = adapter->ctx.irq.primary_queue;
    void __iomem *port;
    u32 slot = 0;
    u32 mask = BIT(slot);
    unsigned long deadline;
    u32 bytes = cmd->sector_count ? cmd->sector_count * 512 : 0;
    int ret = 0;

    if (!desc || !adapter->ctx.mmio_base)
        return -ENODEV;

    if (!desc->cmd_list || !desc->cmd_table)
        return -EINVAL;

    port = adapter->ctx.mmio_base + RC_PORT_REG_BASE(desc->queue_index);

    if (!timeout_ms)
        timeout_ms = 5000;

    deadline = jiffies + msecs_to_jiffies(timeout_ms);
    while (readl(port + RC_PORT_CI) & mask) {
        if (time_after(jiffies, deadline))
            return -EBUSY;
        usleep_range(200, 500);
    }

    rc_ahci_prepare_slot(desc, cmd, slot, data_in, data_out);

    {
        struct rc_ahci_cmd_header *hdr_dbg =
            (struct rc_ahci_cmd_header *)((u8 *)desc->cmd_list +
                                          slot * RC_AHCI_CMD_HEADER_SIZE);
        u8 *fis_dbg = (u8 *)desc->cmd_table + (slot * desc->cmd_table_stride);

        rc_printk(RC_DEBUG,
                  "rc_queue_issue_sync: slot=%u flags=0x%04x prdtl=%u ctba=%08x ctbau=%08x fis=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  slot,
                  le16_to_cpu(hdr_dbg->flags),
                  le16_to_cpu(hdr_dbg->prdtl),
                  le32_to_cpu(hdr_dbg->ctba),
                  le32_to_cpu(hdr_dbg->ctbau),
                  fis_dbg[0], fis_dbg[1], fis_dbg[2], fis_dbg[3],
                  fis_dbg[4], fis_dbg[5], fis_dbg[6], fis_dbg[7],
                  fis_dbg[8], fis_dbg[9], fis_dbg[10], fis_dbg[11],
                  fis_dbg[12], fis_dbg[13], fis_dbg[14], fis_dbg[15]);
        if (le16_to_cpu(hdr_dbg->prdtl)) {
            struct rc_ahci_prdt_entry *prdt_dbg =
                (struct rc_ahci_prdt_entry *)(fis_dbg + 0x80);
            rc_printk(RC_DEBUG,
                      "  PRDT[0]: dba=%08x dbau=%08x dbc=%08x\n",
                      le32_to_cpu(prdt_dbg->dba),
                      le32_to_cpu(prdt_dbg->dbau),
                      le32_to_cpu(prdt_dbg->dbc));
        }
    }

    desc->slot_in_use |= mask;
    writel(0xffffffff, port + RC_PORT_IS);
    writel(0xffffffff, port + RC_PORT_SERR);
    wmb();

    writel(mask, port + RC_PORT_CI);

    deadline = jiffies + msecs_to_jiffies(timeout_ms);
    for (;;) {
        u32 ci = readl(port + RC_PORT_CI);
        if (!(ci & mask))
            break;
        if (time_after(jiffies, deadline)) {
            if (completion) {
                completion->command_id = cmd->command_id;
                completion->status = RC_STATUS_ERROR;
            }
            rc_printk(RC_WARN,
                      "rc_queue_issue_sync: timeout waiting for CI clear (ci=0x%08x, is=0x%08x, serr=0x%08x, tfd=0x%08x)\n",
                      readl(port + RC_PORT_CI),
                      readl(port + RC_PORT_IS),
                      readl(port + RC_PORT_SERR),
                      readl(port + RC_PORT_TFD));
            if (desc->fis) {
                u8 *rfis = desc->fis;
                rc_printk(RC_WARN, "  rx FIS: %*ph\n", 32, rfis);
            }
            ret = -ETIMEDOUT;
            goto out_clear;
        }
        usleep_range(200, 500);
    }

    if (completion)
        memset(completion, 0, sizeof(*completion));

    if (completion) {
        completion->command_id = cmd->command_id;
        completion->bytes_transferred = data_in ? bytes : 0;
    }

    {
        u32 is = readl(port + RC_PORT_IS);
        u32 serr = readl(port + RC_PORT_SERR);
        u32 tfd = readl(port + RC_PORT_TFD);

        if (is)
            writel(is, port + RC_PORT_IS);
        if (serr)
            writel(serr, port + RC_PORT_SERR);

        if (tfd & BIT(0) || serr) {
            if (completion) {
                completion->status = RC_STATUS_ERROR;
                completion->error_code = serr ? serr : is;
                completion->bytes_transferred = 0;
            }
            ret = -EIO;
        } else {
            if (completion)
                completion->status = RC_STATUS_SUCCESS;
            ret = 0;
        }
    }

out_clear:
    desc->slot_in_use &= ~mask;
    return ret;
}

/*----------------------------------------------------------------------
 * FUN_14000e960: Queue full handler - deferred work item queue
 * 
 * Handles queue-full conditions by queuing work items for deferred processing.
 * Called when queue is full in command dispatchers.
 * 
 * Mirrors Windows driver behavior exactly:
 * - Allocates 0x28-byte work item structure
 * - Maintains linked list at devExt+0x15f88 (head) and +0x15f90 (tail)
 * - Protected by spinlock at devExt+0x15f80
 *----------------------------------------------------------------------*/

void rc_queue_full_handler(struct rc_adapter *adapter, void *srb,
                           void *param3, void (*completion)(void *),
                           void *completion_arg)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    struct rc_work_item *work_item;
    unsigned long flags;

    /* Allocate work item structure (0x28 bytes = 40 bytes) */
    work_item = kzalloc(sizeof(*work_item), GFP_ATOMIC);
    if (!work_item) {
        rc_printk(RC_ERROR,
                  "rc_queue_full_handler: failed to allocate work item\n");
        return;
    }

    /* Initialize work item structure */
    work_item->next = NULL;
    work_item->prev = NULL;
    work_item->srb = srb;
    work_item->param3 = param3;
    work_item->completion = completion;
    work_item->completion_arg = completion_arg;

    /* Insert into queue (protected by spinlock) */
    spin_lock_irqsave(&doorbell->work_queue_lock, flags);

    if (!doorbell->work_queue_head) {
        /* Queue is empty - set as head */
        doorbell->work_queue_head = work_item;
        doorbell->work_queue_tail = work_item;
    } else {
        /* Link to tail */
        doorbell->work_queue_tail->next = work_item;
        work_item->prev = doorbell->work_queue_tail;
        doorbell->work_queue_tail = work_item;
    }

    spin_unlock_irqrestore(&doorbell->work_queue_lock, flags);

    rc_printk(RC_DEBUG,
              "rc_queue_full_handler: queued work item for deferred processing\n");
}

/*----------------------------------------------------------------------
 * Process deferred work items from queue
 * 
 * Called when queue space becomes available (e.g., after completion processing)
 * or by a worker thread to retry command submission.
 *----------------------------------------------------------------------*/

int rc_process_deferred_work_items(struct rc_adapter *adapter)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    struct rc_work_item *work_item;
    unsigned long flags;
    int processed = 0;

    spin_lock_irqsave(&doorbell->work_queue_lock, flags);

    while (doorbell->work_queue_head) {
        /* Remove from head */
        work_item = doorbell->work_queue_head;
        doorbell->work_queue_head = work_item->next;
        
        if (!doorbell->work_queue_head)
            doorbell->work_queue_tail = NULL;
        else
            doorbell->work_queue_head->prev = NULL;

        spin_unlock_irqrestore(&doorbell->work_queue_lock, flags);

        /* Process work item - retry command submission */
        rc_printk(RC_DEBUG,
                  "rc_process_deferred_work_items: processing deferred work item\n");
        
        /* TODO: Retry command submission with work_item->srb */
        /* This will be implemented when we have full command routing */
        
        /* Call completion callback if provided */
        if (work_item->completion)
            work_item->completion(work_item->completion_arg);

        kfree(work_item);
        processed++;

        spin_lock_irqsave(&doorbell->work_queue_lock, flags);
    }

    spin_unlock_irqrestore(&doorbell->work_queue_lock, flags);

    return processed;
}

/*----------------------------------------------------------------------
 * Cleanup work item queue
 *----------------------------------------------------------------------*/

void rc_cleanup_work_item_queue(struct rc_adapter *adapter)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    struct rc_work_item *work_item, *next;
    unsigned long flags;

    spin_lock_irqsave(&doorbell->work_queue_lock, flags);

    work_item = doorbell->work_queue_head;
    while (work_item) {
        next = work_item->next;
        kfree(work_item);
        work_item = next;
    }

    doorbell->work_queue_head = NULL;
    doorbell->work_queue_tail = NULL;

    spin_unlock_irqrestore(&doorbell->work_queue_lock, flags);
}

/*----------------------------------------------------------------------
 * FUN_1400093c4 - Queue state table lookup
 * Checks and clears queue initialization state in global table
 *----------------------------------------------------------------------*/

// Global queue state table (DAT_140031488)
// Index calculation: index = adapter_index * 0x1c878
#define RC_QUEUE_STATE_TABLE_ENTRY_SIZE 0x1c878
#define RC_MAX_ADAPTER_QUEUE_STATES 16
static u32 rc_queue_state_table[RC_MAX_ADAPTER_QUEUE_STATES * RC_QUEUE_STATE_TABLE_ENTRY_SIZE / sizeof(u32)];

int rc_queue_state_table_lookup(struct rc_adapter *adapter)
{
    u32 index;
    u32 old_value;
    
    // Calculate index: devExt+0x16050 * 0x1c878
    // devExt+0x16050 is adapter index (0-based)
    index = adapter->instance * RC_QUEUE_STATE_TABLE_ENTRY_SIZE / sizeof(u32);
    
    if (index >= ARRAY_SIZE(rc_queue_state_table)) {
        rc_printk(RC_ERROR, "rc_queue_state_table_lookup: adapter index %d out of range\n",
                  adapter->instance);
        return 0;
    }
    
    // Read old value
    old_value = rc_queue_state_table[index];
    
    // Clear to 0
    rc_queue_state_table[index] = 0;
    
    // Returns 0 if queue state was already clear, 1 if it was set (error condition)
    return old_value != 0 ? 1 : 0;
}

/*----------------------------------------------------------------------
 * FUN_140007978 - Global completion register programming
 * Programs completion registers using a global context
 *----------------------------------------------------------------------*/

// Global completion register context (DAT_140216880)
static struct rc_completion_register_context {
    u32 base_address_lo;
    u32 base_address_hi;
    u32 size;
    u32 flags;
    u32 reserved[12];
} rc_global_completion_context;

// Global completion register state (DAT_140014284)
static u32 rc_global_completion_state = 0;

int rc_program_global_completion_registers(struct rc_adapter *adapter)
{
    // Reset global state to 0
    rc_global_completion_state = 0;
    
    // TODO: Call service slot +0x9D8 with global address
    // This is a StorPort service call that programs completion registers
    // For now, we'll implement a placeholder that will be replaced when
    // we have the actual register programming logic
    
    rc_printk(RC_INFO, "rc_program_global_completion_registers: programming global completion registers\n");
    
    // The actual implementation would:
    // 1. Get the global completion context address
    // 2. Program MMIO registers based on the context
    // 3. Verify the programming succeeded
    
    return 0;
}

/*----------------------------------------------------------------------
 * FUN_140008a48 - Queue initialization for a BAR
 * Initializes queues and completion register contexts
 *----------------------------------------------------------------------*/

int rc_init_queue_bar(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    int ret;
    int i;
    
    rc_printk(RC_INFO, "rc_init_queue_bar: initializing queues for adapter %d\n",
              adapter->instance);
    
    // TODO: Call service slot +0x9D8 twice for completion register programming:
    // First call: devExt + 0x16288 (completion register context)
    // Second call: devExt + 0x1c2e8 (another completion register context)
    // These are per-BAR completion register contexts
    // For now, we'll implement placeholders
    
    // Initialize spinlocks for all ports (iterate through devExt+0xb0 count)
    // devExt+0xb0 is spinlock count (port count)
    // We'll initialize spinlocks for each port
    for (i = 0; i < ctx->port_count && i < RC_MAX_SCSI_TARGETS; i++) {
        // Spinlocks are already initialized in rc_hw_init, but we can add
        // per-port spinlocks here if needed
    }
    
    // Call global completion register programming
    ret = rc_program_global_completion_registers(adapter);
    if (ret) {
        rc_printk(RC_ERROR, "rc_init_queue_bar: global completion register programming failed\n");
        return ret;
    }
    
    // TODO: Call callback at devExt+0x16110 (spinlock callback - FUN_1400021d4)
    // This is the critical queue initialization function
    // We'll need to implement this separately
    
    // TODO: Use StorPort service +0x9f8 to write 0xffffffffffe17b80 to MMIO at devExt+0x16010
    // devExt+0x16010 is MMIO register address pointer
    // For now, we'll skip this as it requires the exact register offset
    
    // Clear devExt+0x16054 (adapter active flag) to 0
    ctx->doorbell.adapter_active = 0;
    
    // Call queue state table lookup to check/clear queue state
    ret = rc_queue_state_table_lookup(adapter);
    if (ret == 1) {
        // Queue state was set (error condition) - log error
        rc_printk(RC_ERROR, "rc_init_queue_bar: queue state indicates previous initialization error\n");
        // TODO: Call FUN_14000273c to log error (AHCI controller GHC register error)
        return -EIO;
    }
    
    rc_printk(RC_INFO, "rc_init_queue_bar: queue initialization complete\n");
    
    return 0;
}

