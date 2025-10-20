/****************************************************************************
 * AMD RAID Driver for Linux - Debugfs Interface
 * Provides detailed queue inspection and hardware state debugging
 ****************************************************************************/

#include "rc_linux.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *rc_debugfs_root;

/*----------------------------------------------------------------------
 * Command queue dump
 *----------------------------------------------------------------------*/

static int rc_debugfs_cmd_queue_show(struct seq_file *m, void *v)
{
    struct rc_adapter *adapter = m->private;
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_hw_command *cmd;
    u32 i;

    seq_printf(m, "Command Queue Dump (Adapter %d)\n", adapter->instance);
    seq_printf(m, "================================\n\n");
    seq_printf(m, "Queue Size: %u\n", hw->cmd_queue_size);
    seq_printf(m, "Head: %u\n", hw->cmd_queue_head);
    seq_printf(m, "Tail: %u\n", hw->cmd_queue_tail);
    seq_printf(m, "DMA Address: 0x%llx\n\n", (unsigned long long)hw->cmd_queue_dma);

    seq_printf(m, "Entries:\n");
    seq_printf(m, "%-4s %-10s %-8s %-6s %-6s %-12s %-12s %-16s\n",
               "Idx", "CmdID", "Opcode", "Flags", "Chan", "LBA", "Sectors", "DataAddr");
    seq_printf(m, "%-4s %-10s %-8s %-6s %-6s %-12s %-12s %-16s\n",
               "---", "-----", "------", "-----", "----", "---", "-------", "--------");

    for (i = 0; i < hw->cmd_queue_size; i++) {
        cmd = &hw->cmd_queue[i];
        if (cmd->command_id == 0)
            continue;

        seq_printf(m, "%-4u %-10u 0x%-6x %-6u %-6u %-12llu %-12u 0x%-14llx\n",
                   i,
                   cmd->command_id,
                   cmd->opcode,
                   cmd->flags,
                   cmd->channel_id,
                   (unsigned long long)cmd->lba,
                   cmd->sector_count,
                   (unsigned long long)cmd->data_addr);
    }

    return 0;
}

static int rc_debugfs_cmd_queue_open(struct inode *inode, struct file *file)
{
    return single_open(file, rc_debugfs_cmd_queue_show, inode->i_private);
}

static const struct file_operations rc_debugfs_cmd_queue_fops = {
    .open = rc_debugfs_cmd_queue_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/*----------------------------------------------------------------------
 * Completion queue dump
 *----------------------------------------------------------------------*/

static int rc_debugfs_comp_queue_show(struct seq_file *m, void *v)
{
    struct rc_adapter *adapter = m->private;
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_hw_completion *comp;
    u32 i;

    seq_printf(m, "Completion Queue Dump (Adapter %d)\n", adapter->instance);
    seq_printf(m, "=====================================\n\n");
    seq_printf(m, "Queue Size: %u\n", hw->comp_queue_size);
    seq_printf(m, "Head: %u\n", hw->comp_queue_head);
    seq_printf(m, "Tail: %u\n", hw->comp_queue_tail);
    seq_printf(m, "DMA Address: 0x%llx\n\n", (unsigned long long)hw->comp_queue_dma);

    seq_printf(m, "Entries:\n");
    seq_printf(m, "%-4s %-10s %-8s %-10s %-12s\n",
               "Idx", "CmdID", "Status", "BytesXfer", "ErrorCode");
    seq_printf(m, "%-4s %-10s %-8s %-10s %-12s\n",
               "---", "-----", "------", "---------", "---------");

    for (i = 0; i < hw->comp_queue_size; i++) {
        comp = &hw->comp_queue[i];
        if (comp->command_id == 0)
            continue;

        seq_printf(m, "%-4u %-10u %-8u %-10u 0x%-10x\n",
                   i,
                   comp->command_id,
                   comp->status,
                   comp->bytes_transferred,
                   comp->error_code);
    }

    return 0;
}

static int rc_debugfs_comp_queue_open(struct inode *inode, struct file *file)
{
    return single_open(file, rc_debugfs_comp_queue_show, inode->i_private);
}

static const struct file_operations rc_debugfs_comp_queue_fops = {
    .open = rc_debugfs_comp_queue_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/*----------------------------------------------------------------------
 * Pending requests dump
 *----------------------------------------------------------------------*/

static int rc_debugfs_pending_show(struct seq_file *m, void *v)
{
    struct rc_adapter *adapter = m->private;
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_pending_request *pending;
    u32 i, count = 0;
    unsigned long flags;

    seq_printf(m, "Pending Requests (Adapter %d)\n", adapter->instance);
    seq_printf(m, "================================\n\n");

    spin_lock_irqsave(&hw->pending_lock, flags);

    seq_printf(m, "%-4s %-10s %-18s %-16s %-18s\n",
               "Slot", "CmdID", "Request", "DMA Addr", "DMA Buffer");
    seq_printf(m, "%-4s %-10s %-18s %-16s %-18s\n",
               "----", "-----", "-------", "--------", "----------");

    for (i = 0; i < RC_MAX_PENDING_REQUESTS; i++) {
        pending = &hw->pending_reqs[i];
        if (!pending->rq)
            continue;

        seq_printf(m, "%-4u %-10u %p 0x%-14llx %p\n",
                   i,
                   pending->cmd_id,
                   pending->rq,
                   (unsigned long long)pending->dma_addr,
                   pending->dma_buf);
        count++;
    }

    spin_unlock_irqrestore(&hw->pending_lock, flags);

    seq_printf(m, "\nTotal Pending: %u / %u\n", count, RC_MAX_PENDING_REQUESTS);

    return 0;
}

static int rc_debugfs_pending_open(struct inode *inode, struct file *file)
{
    return single_open(file, rc_debugfs_pending_show, inode->i_private);
}

static const struct file_operations rc_debugfs_pending_fops = {
    .open = rc_debugfs_pending_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/*----------------------------------------------------------------------
 * IRQ state dump
 *----------------------------------------------------------------------*/

static int rc_debugfs_irq_show(struct seq_file *m, void *v)
{
    struct rc_adapter *adapter = m->private;
    struct rc_hw_queue_context *hw = &adapter->hw;
    struct rc_irq_state *irq = &adapter->ctx.irq;
    u32 i;

    seq_printf(m, "IRQ State (Adapter %d)\n", adapter->instance);
    seq_printf(m, "========================\n\n");

    seq_printf(m, "IRQ Mode: %s\n",
               adapter->irq_mode == RC_IRQ_MODE_MSI ? "MSI" :
               adapter->irq_mode == RC_IRQ_MODE_MSIX ? "MSI-X" : "Legacy");
    seq_printf(m, "IRQ Vector: %d\n", adapter->irq_vector);
    seq_printf(m, "IRQ Count: %d\n\n", atomic_read(&hw->irq_count));

    seq_printf(m, "Queue Handles:\n");
    seq_printf(m, "  Primary Queue: %p\n", irq->primary_queue);
    seq_printf(m, "  Pending Count: %u\n", irq->pending);
    seq_printf(m, "  Scratch Head: %p\n", irq->scratch_head);
    seq_printf(m, "  Scratch Tail: %p\n\n", irq->scratch_tail);

    seq_printf(m, "Queue Table (devExt+0x6C0):\n");
    for (i = 0; i < RC_MAX_QUEUE_DESCRIPTORS; i++) {
        if (irq->queue_table[i]) {
            seq_printf(m, "  [%u]: %p\n", i, irq->queue_table[i]);
        }
    }

    return 0;
}

static int rc_debugfs_irq_open(struct inode *inode, struct file *file)
{
    return single_open(file, rc_debugfs_irq_show, inode->i_private);
}

static const struct file_operations rc_debugfs_irq_fops = {
    .open = rc_debugfs_irq_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/*----------------------------------------------------------------------
 * Register dump
 *----------------------------------------------------------------------*/

static int rc_debugfs_regs_show(struct seq_file *m, void *v)
{
    struct rc_adapter *adapter = m->private;
    void __iomem *mmio = adapter->ctx.mmio_base;
    u32 i;

    seq_printf(m, "Hardware Registers (Adapter %d)\n", adapter->instance);
    seq_printf(m, "==================================\n\n");

    seq_printf(m, "MMIO Base: %p (Physical: 0x%llx, Length: 0x%llx)\n\n",
               mmio,
               (unsigned long long)adapter->ctx.mmio_phys,
               (unsigned long long)adapter->ctx.mmio_len);

    seq_printf(m, "Key Registers:\n");
    seq_printf(m, "%-24s  %-10s  %-10s\n", "Register", "Offset", "Value");
    seq_printf(m, "%-24s  %-10s  %-10s\n", "--------", "------", "-----");

    // Read key registers (adjust offsets based on real hardware)
    seq_printf(m, "%-24s  0x%-8x  0x%08x\n", "Status", 0x00, readl(mmio + 0x00));
    seq_printf(m, "%-24s  0x%-8x  0x%08x\n", "Control", 0x04, readl(mmio + 0x04));
    seq_printf(m, "%-24s  0x%-8x  0x%08x\n", "Interrupt Status", 0x08, readl(mmio + 0x08));
    seq_printf(m, "%-24s  0x%-8x  0x%08x\n", "Interrupt Mask", 0x0C, readl(mmio + 0x0C));
    seq_printf(m, "%-24s  0x%-8x  0x%08x\n", "Doorbell", 0x10, readl(mmio + 0x10));

    seq_printf(m, "\nFirst 256 bytes of MMIO (hex dump):\n");
    for (i = 0; i < 256; i += 16) {
        seq_printf(m, "%04x: %08x %08x %08x %08x\n",
                   i,
                   readl(mmio + i),
                   readl(mmio + i + 4),
                   readl(mmio + i + 8),
                   readl(mmio + i + 12));
    }

    return 0;
}

static int rc_debugfs_regs_open(struct inode *inode, struct file *file)
{
    return single_open(file, rc_debugfs_regs_show, inode->i_private);
}

static const struct file_operations rc_debugfs_regs_fops = {
    .open = rc_debugfs_regs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/*----------------------------------------------------------------------
 * Debugfs initialization & cleanup
 *----------------------------------------------------------------------*/

int rc_debugfs_init(void)
{
    rc_debugfs_root = debugfs_create_dir("rcraid", NULL);
    if (!rc_debugfs_root) {
        rc_printk(RC_WARN, "rc_debugfs_init: failed to create debugfs root\n");
        return -ENOMEM;
    }

    rc_printk(RC_INFO, "rc_debugfs_init: created debugfs root at /sys/kernel/debug/rcraid\n");
    return 0;
}

void rc_debugfs_cleanup(void)
{
    debugfs_remove_recursive(rc_debugfs_root);
    rc_debugfs_root = NULL;
}

int rc_debugfs_create_adapter(struct rc_adapter *adapter)
{
    char name[32];
    struct dentry *adapter_dir;

    if (!rc_debugfs_root)
        return -ENODEV;

    snprintf(name, sizeof(name), "adapter%d", adapter->instance);
    adapter_dir = debugfs_create_dir(name, rc_debugfs_root);
    if (!adapter_dir)
        return -ENOMEM;

    adapter->debugfs_dir = adapter_dir;

    // Create debugfs files
    debugfs_create_file("cmd_queue", 0400, adapter_dir, adapter,
                        &rc_debugfs_cmd_queue_fops);
    debugfs_create_file("comp_queue", 0400, adapter_dir, adapter,
                        &rc_debugfs_comp_queue_fops);
    debugfs_create_file("pending_requests", 0400, adapter_dir, adapter,
                        &rc_debugfs_pending_fops);
    debugfs_create_file("irq_state", 0400, adapter_dir, adapter,
                        &rc_debugfs_irq_fops);
    debugfs_create_file("registers", 0400, adapter_dir, adapter,
                        &rc_debugfs_regs_fops);

    rc_printk(RC_INFO, "rc_debugfs_create_adapter: created debugfs entries for adapter %d\n",
              adapter->instance);

    return 0;
}

void rc_debugfs_remove_adapter(struct rc_adapter *adapter)
{
    if (adapter->debugfs_dir) {
        debugfs_remove_recursive(adapter->debugfs_dir);
        adapter->debugfs_dir = NULL;
    }
}

