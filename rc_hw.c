/****************************************************************************
 * AMD RAID Driver for Linux - Hardware Layer
 * Real hardware communication implementation
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

static void rc_hw_program_queues(struct rc_hw_queue_context *hw)
{
    void __iomem *base = hw->mmio_base;

    writel(lower_32_bits(hw->cmd_queue_dma), base + RC_REG_COMMAND_QUEUE);
    writel(upper_32_bits(hw->cmd_queue_dma), base + RC_REG_COMMAND_QUEUE + 4);
    writel(hw->cmd_queue_size, base + RC_REG_COMMAND_QUEUE + 8);

    writel(lower_32_bits(hw->comp_queue_dma), base + RC_REG_COMPLETION_QUEUE);
    writel(upper_32_bits(hw->comp_queue_dma), base + RC_REG_COMPLETION_QUEUE + 4);
    writel(hw->comp_queue_size, base + RC_REG_COMPLETION_QUEUE + 8);
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
    atomic_set(&hw->irq_count, 0);
    atomic_set(&hw->cmd_sequence, 0);

    hw->dma_pool = dma_pool_create("rcraid_hw", &pdev->dev,
                                   sizeof(struct rc_hw_command), 64, 0);
    if (!hw->dma_pool)
        return -ENOMEM;

    hw->cmd_queue_size = 32;
    hw->cmd_queue = dma_pool_alloc(hw->dma_pool, GFP_KERNEL, &hw->cmd_queue_dma);
    if (!hw->cmd_queue) {
        ret = -ENOMEM;
        goto err_destroy_pool;
    }
    memset(hw->cmd_queue, 0, sizeof(struct rc_hw_command) * hw->cmd_queue_size);

    hw->comp_queue = dma_pool_alloc(hw->dma_pool, GFP_KERNEL, &hw->comp_queue_dma);
    if (!hw->comp_queue) {
        ret = -ENOMEM;
        goto err_free_cmd;
    }
    memset(hw->comp_queue, 0, sizeof(struct rc_hw_completion) * hw->cmd_queue_size);

    hw->cmd_queue_head = 0;
    hw->cmd_queue_tail = 0;
    hw->comp_queue_head = 0;
    hw->comp_queue_tail = 0;

    rc_hw_program_queues(hw);

    writel(0x1, hw->mmio_base + RC_REG_INTERRUPT_MASK);

    rc_printk(RC_INFO, "rc_hw_init: queues programmed (cmd=0x%llx comp=0x%llx)\n",
              (unsigned long long)hw->cmd_queue_dma,
              (unsigned long long)hw->comp_queue_dma);

    return 0;

err_free_cmd:
    dma_pool_free(hw->dma_pool, hw->cmd_queue, hw->cmd_queue_dma);
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
        dma_pool_free(hw->dma_pool, hw->cmd_queue, hw->cmd_queue_dma);
        hw->cmd_queue = NULL;
    }

    if (hw->comp_queue) {
        dma_pool_free(hw->dma_pool, hw->comp_queue, hw->comp_queue_dma);
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
    unsigned long flags;
    u32 processed = 0;

    spin_lock_irqsave(&hw->irq_lock, flags);

    while (hw->comp_queue_head != hw->comp_queue_tail) {
        struct rc_hw_completion *comp = &hw->comp_queue[hw->comp_queue_head];

        if (!comp->command_id)
            break;

        memset(comp, 0, sizeof(*comp));
        hw->comp_queue_head = (hw->comp_queue_head + 1) % hw->cmd_queue_size;
        processed++;
    }

    spin_unlock_irqrestore(&hw->irq_lock, flags);

    return processed;
}

irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id)
{
    struct rc_adapter *adapter = dev_id;
    struct rc_hw_queue_context *hw = &adapter->hw;
    u32 status;

    status = readl(hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    if (!status)
        return IRQ_NONE;

    writel(status, hw->mmio_base + RC_REG_INTERRUPT_STATUS);
    atomic_inc(&hw->irq_count);

    rc_hw_process_completions(hw);

    return IRQ_HANDLED;
}
