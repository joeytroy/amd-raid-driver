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

    /* Enable all interrupt sources we understand (vector 244). */
    writel(0xffffffff, hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    writel(0x0000000f, hw->mmio_base + RC_REG_INTERRUPT_MASK);

    rc_printk(RC_INFO, "rc_hw_init: queues programmed (cmd=0x%llx comp=0x%llx)\n",
              (unsigned long long)hw->cmd_queue_dma,
              (unsigned long long)hw->comp_queue_dma);

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
    unsigned long flags;
    u32 next_tail;
    u32 doorbell_value;

    spin_lock_irqsave(&hw->irq_lock, flags);

    next_tail = (hw->cmd_queue_tail + 1) % hw->cmd_queue_size;
    if (next_tail == hw->cmd_queue_head) {
        spin_unlock_irqrestore(&hw->irq_lock, flags);
        return -EBUSY;
    }

    if (cmd->opcode < RC_CMD_READ_DATA || cmd->opcode > RC_CMD_RESCAN) {
        spin_unlock_irqrestore(&hw->irq_lock, flags);
        return -EINVAL;
    }

    if (!cmd->command_id)
        cmd->command_id = atomic_inc_return(&hw->cmd_sequence);
    memcpy(&hw->cmd_queue[hw->cmd_queue_tail], cmd, sizeof(*cmd));
    hw->cmd_queue_tail = next_tail;

    doorbell_value = (cmd->opcode << 16) | hw->cmd_queue_tail;
    writel(doorbell_value, hw->mmio_base + RC_REG_DOORBELL);

    if (cmd->opcode == RC_CMD_CONFIG_WRITE || cmd->opcode == RC_CMD_CONFIG_READ) {
        writel(upper_32_bits(cmd->generation_number),
               hw->mmio_base + RC_REG_DOORBELL + 4);
        writel(lower_32_bits(cmd->generation_number),
               hw->mmio_base + RC_REG_DOORBELL + 8);
    }

    spin_unlock_irqrestore(&hw->irq_lock, flags);

    return 0;
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
    unsigned long flags;
    unsigned long timeout;
    struct rc_adapter *adapter;
    int ret;

    if (!hw || !cmd)
        return -EINVAL;

    adapter = hw->owner;
    if (!adapter)
        return -ENODEV;

    if (!timeout_ms)
        timeout_ms = 5000;

    spin_lock_irqsave(&hw->sync_lock, flags);
    if (hw->sync.active) {
        spin_unlock_irqrestore(&hw->sync_lock, flags);
        rc_printk(RC_WARN, "rc_hw_submit_sync_command: sync slot busy\n");
        return -EBUSY;
    }

    hw->sync.active = true;
    hw->sync.completed = false;
    hw->sync.cmd_id = 0;
    memset(&hw->sync.completion, 0, sizeof(hw->sync.completion));
    spin_unlock_irqrestore(&hw->sync_lock, flags);

    ret = rc_hw_submit_command(hw, cmd);
    if (ret)
        goto out_clear;

    spin_lock_irqsave(&hw->sync_lock, flags);
    hw->sync.cmd_id = cmd->command_id;
    spin_unlock_irqrestore(&hw->sync_lock, flags);

    timeout = jiffies + msecs_to_jiffies(timeout_ms);
    do {
        rc_hw_poll_completions(hw);

        spin_lock_irqsave(&hw->sync_lock, flags);
        if (hw->sync.completed) {
            if (completion)
                *completion = hw->sync.completion;
            hw->sync.active = false;
            spin_unlock_irqrestore(&hw->sync_lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&hw->sync_lock, flags);

        usleep_range(500, 2000);
    } while (time_before(jiffies, timeout));

    ret = -ETIMEDOUT;
    rc_printk(RC_WARN, "rc_hw_submit_sync_command: cmd_id=%u timed out\n",
              cmd->command_id);

out_clear:
    spin_lock_irqsave(&hw->sync_lock, flags);
    hw->sync.active = false;
    hw->sync.completed = false;
    hw->sync.cmd_id = 0;
    memset(&hw->sync.completion, 0, sizeof(hw->sync.completion));
    spin_unlock_irqrestore(&hw->sync_lock, flags);

    return ret;
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
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_hw_command cmd = {0};
    u32 cmd_id;
    int ret;

    cmd_id = atomic_inc_return(&hw->cmd_sequence);

    // Track this request for completion
    ret = rc_track_request(hw, cmd_id, rq, dma_addr, dma_buf);
    if (ret) {
        rc_printk(RC_ERROR, "rc_hw_submit_request: failed to track request\n");
        return ret;
    }

    // Build command
    cmd.command_id = cmd_id;
    cmd.opcode = opcode;
    cmd.flags = 0;  // Async
    cmd.channel_id = 0;  // TODO: map from array
    cmd.lba = lba;
    cmd.sector_count = sector_count;
    cmd.data_addr = dma_addr;
    cmd.completion_addr = 0;
    cmd.generation_number = 0;

    // Submit to hardware
    ret = rc_hw_submit_command(hw, &cmd);
    if (ret) {
        rc_printk(RC_ERROR, "rc_hw_submit_request: submit failed\n");
        rc_clear_pending_request(hw, cmd_id);
        return ret;
    }

    return 0;
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
