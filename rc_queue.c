/****************************************************************************
 * AMD RAID Driver for Linux - Queue Management
 * Implements StorPort service slots for queue/DMA allocation
 ****************************************************************************/

#include "rc_linux.h"

/*----------------------------------------------------------------------
 * Queue descriptor structures (mirrors Windows devExt layouts)
 *----------------------------------------------------------------------*/

// StorPort slot +0x1F8: Queue descriptor (0x78 bytes, FUN_14000d66c)
struct rc_queue_descriptor {
    void *control_block;        // +0x00: pointer to 0x400-byte DMA block
    dma_addr_t control_dma;     // +0x08: DMA address of control block
    u32 queue_index;            // +0x10: queue index (0-based)
    u32 queue_depth;            // +0x14: number of entries
    u32 flags;                  // +0x18: queue flags
    u32 state;                  // +0x1C: queue state
    void *completion_ptr;       // +0x20: completion queue pointer
    dma_addr_t completion_dma;  // +0x28: completion queue DMA
    spinlock_t lock;            // +0x30: per-queue lock
    u32 head;                   // +0x38: submission head
    u32 tail;                   // +0x3C: submission tail
    u32 comp_head;              // +0x40: completion head
    u32 comp_tail;              // +0x44: completion tail
    atomic_t pending;           // +0x48: pending command count
    u8 reserved[44];            // +0x4C: padding to 0x78
} __packed;

// Descriptor table state (devExt+0x1C2A0)
struct rc_queue_table {
    struct rc_queue_descriptor *descriptors[RC_MAX_QUEUE_DESCRIPTORS];
    spinlock_t table_lock;
    u32 num_allocated;
};

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

    // Program completion registers
    ret = rc_program_completion_registers(adapter, desc);
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

    // Ring doorbells 1-4 as per FUN_14000924c
    rc_ring_doorbell(adapter, 1);
    rc_ring_doorbell(adapter, 2);

    // 25 µs stall between doorbell pairs
    udelay(25);

    rc_ring_doorbell(adapter, 3);
    rc_ring_doorbell(adapter, 4);

    // Mark firmware capability active (devExt+0x1C2DC)
    ctx->doorbell.fast_path_enabled = true;

    rc_printk(RC_INFO, "rc_activate_doorbells: doorbells activated\n");

    return 0;
}

