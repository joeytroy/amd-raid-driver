/****************************************************************************
 * AMD RAID Driver for Linux - Hardware Layer
 * Real hardware communication implementation
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

static void rc_hw_program_queues(struct rc_hw_queue_context *hw)
{
    void __iomem *base = hw->mmio_base;
    u32 offsets[] = {
        RC_REG_CMD_Q_BASE_LO,
        RC_REG_CMD_Q_BASE_HI,
        RC_REG_CMD_Q_SIZE,
        RC_REG_COMP_Q_BASE_LO,
        RC_REG_COMP_Q_BASE_HI,
        RC_REG_COMP_Q_SIZE,
    };
    const char *labels[] = {
        "cmd_base_lo",
        "cmd_base_hi",
        "cmd_size",
        "comp_base_lo",
        "comp_base_hi",
        "comp_size",
    };
    u32 values[] = {
        lower_32_bits(hw->cmd_queue_dma),
        upper_32_bits(hw->cmd_queue_dma),
        hw->cmd_queue_size,
        lower_32_bits(hw->comp_queue_dma),
        upper_32_bits(hw->comp_queue_dma),
        hw->comp_queue_size,
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        u32 before = readl(base + offsets[i]);
        writel(values[i], base + offsets[i]);
        rc_printk(RC_INFO,
                  "rc_hw_program_queues: %s offset=0x%03x before=0x%08x write=0x%08x after=0x%08x\n",
                  labels[i], offsets[i], before, values[i], readl(base + offsets[i]));
    }

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        rc_printk(RC_INFO,
                  "rc_hw_program_queues: verify offset=0x%03x value=0x%08x\n",
                  offsets[i], readl(base + offsets[i]));
    }
}

static int rc_hw_start_controller(struct rc_hw_queue_context *hw)
{
    u32 ghc;
    int timeout;

    if (!hw || !hw->mmio_base)
        return -EINVAL;

    /* Wait for any in-progress HBA reset to complete (GHC.HR clear). */
    timeout = 1000;
    do {
        ghc = readl(hw->mmio_base + RC_REG_CONTROL);
        if (ghc == 0xffffffff) {
            rc_printk(RC_ERROR, "rc_hw_start_controller: invalid control register value\n");
            return -EIO;
        }
        if (!(ghc & BIT(1)))
            break;
        udelay(100);
    } while (--timeout);

    if (timeout <= 0) {
        rc_printk(RC_ERROR,
                  "rc_hw_start_controller: HBA reset did not clear (GHC=0x%08x)\n",
                  ghc);
        return -EBUSY;
    }

    /* Enable AHCI mode (AE) and interrupts (IE) without disturbing other bits. */
    ghc |= RC_CONTROL_RUN | BIT(0);
    writel(ghc, hw->mmio_base + RC_REG_CONTROL);
    wmb();

    return 0;
}

int rc_hw_init(struct rc_adapter *adapter)
{
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct pci_dev *pdev = adapter->pdev;
    int ret;

    rc_printk(RC_INFO, "rc_hw_init: initializing hardware on adapter %d\n",
              adapter->instance);

    memset(hw, 0, sizeof(*hw));
    hw->owner = adapter;
    hw->pdev = pdev;
    hw->mmio_base = adapter->ctx.mmio_base;
    hw->mmio_len = adapter->ctx.mmio_len;
    hw->mmio_phys = adapter->ctx.mmio_phys;
    hw->msi_vector = adapter->irq_vector;

    spin_lock_init(&hw->irq_lock);
    spin_lock_init(&hw->pending_lock);
    atomic_set(&hw->irq_count, 0);
    atomic_set(&hw->cmd_sequence, 0);
    atomic_set(&hw->completion_count, 0);
    atomic_set(&hw->sync_completion_count, 0);
    memset(hw->pending_reqs, 0, sizeof(hw->pending_reqs));
    spin_lock_init(&hw->sync_lock);
    hw->sync.active = false;
    hw->sync.completed = false;
    hw->sync.cmd_id = 0;
    memset(&hw->sync.completion, 0, sizeof(hw->sync.completion));

    // DMA pool for per-request data buffers (not for the queues themselves)
    hw->dma_pool = dma_pool_create("rcraid_data", &pdev->dev, 4096, 64, 0);
    if (!hw->dma_pool)
        return -ENOMEM;

    hw->cmd_queue_size = 32;
    hw->comp_queue_size = 32;  // Match Windows queue depth
    
    // Allocate command queue array (32 entries)
    hw->cmd_queue = dma_alloc_coherent(&pdev->dev,
                                       sizeof(struct rc_hw_command) * hw->cmd_queue_size,
                                       &hw->cmd_queue_dma, GFP_KERNEL);
    if (!hw->cmd_queue) {
        ret = -ENOMEM;
        goto err_destroy_pool;
    }
    memset(hw->cmd_queue, 0, sizeof(struct rc_hw_command) * hw->cmd_queue_size);

    // Allocate completion queue array (32 entries)
    hw->comp_queue = dma_alloc_coherent(&pdev->dev,
                                        sizeof(struct rc_hw_completion) * hw->comp_queue_size,
                                        &hw->comp_queue_dma, GFP_KERNEL);
    if (!hw->comp_queue) {
        ret = -ENOMEM;
        goto err_free_cmd;
    }
    memset(hw->comp_queue, 0, sizeof(struct rc_hw_completion) * hw->comp_queue_size);

    hw->cmd_queue_head = 0;
    hw->cmd_queue_tail = 0;
    hw->comp_queue_head = 0;

    rc_hw_program_queues(hw);

    ret = rc_hw_start_controller(hw);
    if (ret)
        goto err_free_cmd;

    /* Enable all interrupt sources we understand (vector 244). */
    writel(0xffffffff, hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    writel(0x0000000f, hw->mmio_base + RC_REG_INTERRUPT_MASK);

    rc_printk(RC_INFO, "rc_hw_init: queues programmed (cmd=0x%llx comp=0x%llx)\n",
              (unsigned long long)hw->cmd_queue_dma,
              (unsigned long long)hw->comp_queue_dma);

    /* Initialize work item queue (FUN_14000e960) */
    spin_lock_init(&adapter->ctx.doorbell.work_queue_lock);
    adapter->ctx.doorbell.work_queue_head = NULL;
    adapter->ctx.doorbell.work_queue_tail = NULL;

    /* Install callback slots (default to safe dispatcher until firmware detection) */
    ret = rc_install_callbacks(adapter, false);
    if (ret) {
        rc_printk(RC_WARN, "rc_hw_init: callback installation failed (non-fatal)\n");
        /* Continue anyway - callbacks are optional for basic functionality */
    }

    return 0;

err_free_cmd:
    dma_free_coherent(&pdev->dev,
                      sizeof(struct rc_hw_command) * hw->cmd_queue_size,
                      hw->cmd_queue, hw->cmd_queue_dma);
err_destroy_pool:
    dma_pool_destroy(hw->dma_pool);
    hw->dma_pool = NULL;
    return ret;
}

void rc_hw_cleanup(struct rc_adapter *adapter)
{
    struct rc_hw_queue_context *hw = &adapter->hw;

    rc_printk(RC_INFO, "rc_hw_cleanup: adapter %d\n", adapter->instance);

    /* Cleanup work item queue (FUN_14000e960) */
    rc_cleanup_work_item_queue(adapter);

    if (hw->mmio_base)
        writel(0x0, hw->mmio_base + RC_REG_INTERRUPT_MASK);

    if (hw->cmd_queue) {
        dma_free_coherent(&adapter->pdev->dev,
                          sizeof(struct rc_hw_command) * hw->cmd_queue_size,
                          hw->cmd_queue, hw->cmd_queue_dma);
        hw->cmd_queue = NULL;
    }

    if (hw->comp_queue) {
        dma_free_coherent(&adapter->pdev->dev,
                          sizeof(struct rc_hw_completion) * hw->comp_queue_size,
                          hw->comp_queue, hw->comp_queue_dma);
        hw->comp_queue = NULL;
    }

    if (hw->dma_pool) {
        dma_pool_destroy(hw->dma_pool);
        hw->dma_pool = NULL;
    }
}

int rc_hw_submit_command(struct rc_hw_queue_context *hw, struct rc_hw_command *cmd)
{
    return -EOPNOTSUPP;
}

int rc_hw_process_completions(struct rc_hw_queue_context *hw)
{
    return rc_hw_poll_completions(hw);
}

static u32 rc_hw_complete_locked(struct rc_adapter *adapter);

int rc_hw_poll_completions(struct rc_hw_queue_context *hw)
{
    struct rc_adapter *adapter;
    unsigned long flags;
    u32 processed;

    if (!hw)
        return 0;

    adapter = hw->owner;
    if (!adapter)
        return 0;

    spin_lock_irqsave(&hw->irq_lock, flags);
    processed = rc_hw_complete_locked(adapter);
    spin_unlock_irqrestore(&hw->irq_lock, flags);

    return processed;
}

int rc_hw_submit_sync_command(struct rc_hw_queue_context *hw,
                             struct rc_hw_command *cmd,
                             struct rc_hw_completion *completion,
                             unsigned int timeout_ms)
{
    struct rc_adapter *adapter;

    if (!hw || !cmd)
        return -EINVAL;

    adapter = hw->owner;
    if (!adapter)
        return -ENODEV;

    if (!cmd->command_id)
        cmd->command_id = atomic_inc_return(&hw->cmd_sequence);

    if (hw->cmd_queue_size)
        memcpy(&hw->cmd_queue[0], cmd, sizeof(*cmd));

    return rc_queue_issue_sync(adapter, cmd,
                               !!(cmd->flags & RC_CMD_FLAG_DATA_IN),
                               !!(cmd->flags & RC_CMD_FLAG_DATA_OUT),
                               timeout_ms, completion);
}

/*----------------------------------------------------------------------
 * Request tracking helpers
 *----------------------------------------------------------------------*/

static int rc_track_request(struct rc_hw_queue_context *hw, u32 cmd_id,
                             struct request *rq, dma_addr_t dma_addr,
                             void *dma_buf)
{
    unsigned long flags;
    u32 slot = cmd_id % RC_MAX_PENDING_REQUESTS;

    spin_lock_irqsave(&hw->pending_lock, flags);

    if (hw->pending_reqs[slot].rq) {
        spin_unlock_irqrestore(&hw->pending_lock, flags);
        rc_printk(RC_WARN, "rc_track_request: slot %u already occupied\n", slot);
        return -EBUSY;
    }

    hw->pending_reqs[slot].rq = rq;
    hw->pending_reqs[slot].dma_addr = dma_addr;
    hw->pending_reqs[slot].dma_buf = dma_buf;
    hw->pending_reqs[slot].cmd_id = cmd_id;

    spin_unlock_irqrestore(&hw->pending_lock, flags);
    return 0;
}

static struct rc_pending_request *
rc_find_pending_request(struct rc_hw_queue_context *hw, u32 cmd_id)
{
    unsigned long flags;
    u32 slot = cmd_id % RC_MAX_PENDING_REQUESTS;
    struct rc_pending_request *pending = NULL;

    spin_lock_irqsave(&hw->pending_lock, flags);

    if (hw->pending_reqs[slot].cmd_id == cmd_id && hw->pending_reqs[slot].rq) {
        pending = &hw->pending_reqs[slot];
    }

    spin_unlock_irqrestore(&hw->pending_lock, flags);
    return pending;
}

static void rc_clear_pending_request(struct rc_hw_queue_context *hw, u32 cmd_id)
{
    unsigned long flags;
    u32 slot = cmd_id % RC_MAX_PENDING_REQUESTS;

    spin_lock_irqsave(&hw->pending_lock, flags);

    if (hw->pending_reqs[slot].cmd_id == cmd_id) {
        hw->pending_reqs[slot].rq = NULL;
        hw->pending_reqs[slot].dma_addr = 0;
        hw->pending_reqs[slot].dma_buf = NULL;
        hw->pending_reqs[slot].cmd_id = 0;
    }

    spin_unlock_irqrestore(&hw->pending_lock, flags);
}

static bool rc_hw_try_complete_sync(struct rc_hw_queue_context *hw,
                                    struct rc_hw_completion *comp)
{
    unsigned long flags;
    bool handled = false;

    spin_lock_irqsave(&hw->sync_lock, flags);
    if (hw->sync.active && hw->sync.cmd_id == comp->command_id) {
        hw->sync.completion = *comp;
        hw->sync.completed = true;
        handled = true;
    }
    spin_unlock_irqrestore(&hw->sync_lock, flags);

    if (handled)
        atomic_inc(&hw->sync_completion_count);

    return handled;
}

static void rc_hw_handle_async_completion(struct rc_adapter *adapter,
                                          struct rc_hw_completion *comp)
{
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_pending_request *pending;
    blk_status_t blk_status;

    pending = rc_find_pending_request(hw, comp->command_id);
    if (pending && pending->rq) {
        switch (comp->status) {
        case RC_STATUS_SUCCESS:
            blk_status = BLK_STS_OK;
            break;
        case RC_STATUS_BUSY:
            blk_status = BLK_STS_AGAIN;
            break;
        default:
            blk_status = BLK_STS_IOERR;
            break;
        }

        if (pending->dma_buf)
            dma_pool_free(hw->dma_pool, pending->dma_buf, pending->dma_addr);

        blk_mq_end_request(pending->rq, blk_status);
        rc_clear_pending_request(hw, comp->command_id);

        rc_printk(RC_DEBUG, "rc_completion_pump: completed cmd_id=%u status=%u\n",
                  comp->command_id, comp->status);
    } else {
        rc_printk(RC_DEBUG, "rc_completion_pump: cmd_id=%u (no request)\n",
                  comp->command_id);
    }
}

static u32 rc_hw_complete_locked(struct rc_adapter *adapter)
{
    struct rc_hw_queue_context *hw = &adapter->hw;
    u32 processed = 0;

    while (true) {
        struct rc_hw_completion *comp = &hw->comp_queue[hw->comp_queue_head];

        if (!READ_ONCE(comp->command_id))
            break;

        if (!rc_hw_try_complete_sync(hw, comp))
            rc_hw_handle_async_completion(adapter, comp);

        memset(comp, 0, sizeof(*comp));
        hw->comp_queue_head = (hw->comp_queue_head + 1) % hw->comp_queue_size;
        hw->cmd_queue_head = (hw->cmd_queue_head + 1) % hw->cmd_queue_size;
        atomic_inc(&hw->completion_count);
        processed++;
    }

    return processed;
}

/*----------------------------------------------------------------------
 * StorPort slot +0x680: Completion pump logic (called by ISR)
 *----------------------------------------------------------------------*/

static void rc_completion_pump(struct rc_adapter *adapter)
{
    struct rc_hw_queue_context *hw = &adapter->hw;
    unsigned long flags;
    u32 processed;

    spin_lock_irqsave(&hw->irq_lock, flags);
    processed = rc_hw_complete_locked(adapter);
    spin_unlock_irqrestore(&hw->irq_lock, flags);

    if (processed)
        rc_printk(RC_DEBUG, "rc_completion_pump: processed %u completions\n",
                  processed);
}

/*----------------------------------------------------------------------
 * High-level request submission (for blk-mq integration)
 *----------------------------------------------------------------------*/

int rc_hw_submit_request(struct rc_adapter *adapter, struct request *rq,
                          u32 opcode, sector_t lba, u32 sector_count,
                          dma_addr_t dma_addr, void *dma_buf)
{
    return -EOPNOTSUPP;
}

/*----------------------------------------------------------------------
 * ISR sequence from FUN_14000d2b8
 *----------------------------------------------------------------------*/

irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id)
{
    struct rc_adapter *adapter = dev_id;
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_irq_state *irq_state = &adapter->ctx.irq;
    u32 status;
    u32 i;

    status = readl(hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    if (!status)
        return IRQ_NONE;

    // Acknowledge interrupt
    writel(status, hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    atomic_inc(&hw->irq_count);
    rc_printk(RC_DEBUG, "rc_hw_interrupt_handler: status=0x%08x\n", status);

    // FUN_14000d2b8 sequence:
    // 1. Process primary queue handle (devExt+0x6B8)
    if (irq_state->primary_queue)
        rc_completion_pump(adapter);

    // 2. Iterate queue table array (devExt+0x6C0) and process each
    if (irq_state->queue_table) {
        for (i = 0; i < RC_MAX_QUEUE_DESCRIPTORS; i++) {
            void *queue_handle = irq_state->queue_table[i];
            if (queue_handle && queue_handle != irq_state->primary_queue)
                rc_completion_pump(adapter);
        }
    }

    // 3. Update pending count (devExt+0x678)
    irq_state->pending = 0;

    // 4. Clear scratch pointers (devExt+0x670, devExt+0x698)
    irq_state->scratch_head = NULL;
    irq_state->scratch_tail = NULL;

    return IRQ_HANDLED;
}

/*----------------------------------------------------------------------
 * Callback slot implementations (devExt+0x16100–0x16168)
 * Safe dispatcher pattern - matches Windows FUN_1400102D8 behavior
 *----------------------------------------------------------------------*/

/* Safe dispatcher - installed when firmware variant doesn't match NVMe */
static void rc_safe_queue_dispatcher(struct rc_adapter *adapter, void *arg)
{
    /* Windows FUN_1400102D8 - thin dispatcher that forwards to steady-state handlers */
    rc_printk(RC_DEBUG, "rc_safe_queue_dispatcher: adapter %d\n", adapter->instance);
    /* For now, this is a no-op - will be expanded when we implement queue submission */
}

static void rc_safe_queue_toggle(struct rc_adapter *adapter, u8 mode, void *ptr)
{
    /* Windows FUN_140001438 / FUN_14000C0BC - trampoline that saves arguments */
    rc_printk(RC_DEBUG, "rc_safe_queue_toggle: adapter %d mode=0x%02x\n",
              adapter->instance, mode);
    /* For now, this is a no-op - will be expanded when we implement queue mode switching */
}

static void rc_safe_spinlock_callback(struct rc_adapter *adapter)
{
    /* Windows callback invoked after spinlock initialization */
    rc_printk(RC_DEBUG, "rc_safe_spinlock_callback: adapter %d\n", adapter->instance);
    /* For now, this is a no-op */
}

static void rc_safe_port_disable(struct rc_adapter *adapter, u32 port_id)
{
    /* Windows FUN_140003048 - port disable/quiesce handler */
    rc_printk(RC_INFO, "rc_safe_port_disable: adapter %d port %u\n",
              adapter->instance, port_id);
    /* For now, this is a no-op - will be expanded when we implement port management */
}

static void rc_safe_port_resume(struct rc_adapter *adapter, u32 port_id)
{
    /* Windows FUN_1400028f8 - port resume/enable handler */
    rc_printk(RC_INFO, "rc_safe_port_resume: adapter %d port %u\n",
              adapter->instance, port_id);
    /* For now, this is a no-op - will be expanded when we implement port management */
}

static void rc_safe_status_poll(struct rc_adapter *adapter, void *status_buf)
{
    /* Windows callback for status polling used by WMI set requests */
    rc_printk(RC_DEBUG, "rc_safe_status_poll: adapter %d\n", adapter->instance);
    /* For now, this is a no-op - will be expanded when we implement status polling */
}

static void rc_safe_secondary_queue(struct rc_adapter *adapter, void *arg)
{
    /* Windows secondary queue helper - dispatcher at +0x16148 */
    rc_printk(RC_DEBUG, "rc_safe_secondary_queue: adapter %d\n", adapter->instance);
    /* For now, this is a no-op - will be expanded when we implement secondary queues */
}

/*----------------------------------------------------------------------
 * Simple callback functions (from FUN_140007d40)
 *----------------------------------------------------------------------*/

/* FUN_140001ba4 - State getter (devExt+0x16138) */
u8 rc_get_queue_state(struct rc_adapter *adapter)
{
    /* Returns byte at devExt+0x15920 (queue state flag) */
    return adapter->ctx.queue_state;
}

/* FUN_140001bbc - Queue activity check (devExt+0x16140) */
int rc_check_queue_activity(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    void __iomem *mmio_base = ctx->mmio_base;
    u32 queue_mask;
    u32 active_mask;
    
    /* Checks devExt+0xb4 (BAR type flag) */
    /* If BAR type is 0, reads queue mask from devExt+0x158b8+8 */
    /* Stores mask at devExt+0x158f4 */
    /* Checks bitmask intersection with devExt+0x158e0 */
    /* If no active queues, sets register at devExt+0x158b8+4 to 0x80000000 */
    
    /* TODO: Implement full logic when we have queue mask structures */
    /* For now, return 1 (queues active) */
    return 1;
}

/* FUN_1400027a8 - Mode toggle (devExt+0x16128) */
void rc_toggle_queue_mode(struct rc_adapter *adapter, u8 mode)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    void __iomem *mmio_base = ctx->mmio_base;
    
    /* If param_3 != 0 and devExt+0xb4 == 0: */
    if (mode != 0 && ctx->bar_type[0] == 0) {
        /* Sets register at devExt+0x158b8+4 to 0x80000002 (mode toggle bit) */
        /* TODO: Implement when we have the exact register offset */
        rc_printk(RC_INFO, "rc_toggle_queue_mode: adapter %d mode=0x%02x\n",
                  adapter->instance, mode);
    }
}

/* FUN_14000303c - No-op helper (devExt+0x16118, +0x16158) */
void rc_noop_helper(struct rc_adapter *adapter)
{
    /* Empty function - returns immediately */
    /* Used as placeholder for optional callbacks */
}

/*----------------------------------------------------------------------
 * Install callback slots based on firmware capability detection
 *----------------------------------------------------------------------*/

int rc_install_callbacks(struct rc_adapter *adapter, bool fast_path)
{
    struct rc_adapter_callbacks *callbacks = &adapter->ctx.doorbell.callbacks;

    if (fast_path) {
        /* Fast-path callbacks for NVMe controllers */
        /* TODO: Implement fast-path callbacks when we have NVMe variant detection */
        rc_printk(RC_INFO, "rc_install_callbacks: fast-path mode (not yet implemented)\n");
        /* For now, fall through to safe dispatcher */
    }

    /* Safe dispatcher mode - default for AHCI/legacy controllers */
    callbacks->queue_dispatcher = rc_safe_queue_dispatcher;
    callbacks->queue_toggle = rc_safe_queue_toggle;
    callbacks->spinlock_callback = rc_safe_spinlock_callback;
    callbacks->port_disable = rc_safe_port_disable;
    callbacks->port_resume = rc_safe_port_resume;
    callbacks->status_poll = rc_safe_status_poll;
    callbacks->secondary_queue = rc_safe_secondary_queue;

    rc_printk(RC_INFO, "rc_install_callbacks: installed safe dispatcher callbacks\n");

    return 0;
}
