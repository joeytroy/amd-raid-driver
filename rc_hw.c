/****************************************************************************
 * AMD RAID Driver for Linux - Hardware Layer
 * Real hardware communication implementation
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

// Global hardware adapter
struct rc_hw_adapter g_hw_adapter = {0};

// Hardware initialization using real TRX50 data
int rc_hw_init(struct pci_dev *pdev, struct rc_hw_adapter *hw)
{
	int ret;
	resource_size_t mmio_start, mmio_len;
	
	rc_printk(RC_INFO, "rc_hw_init: initializing real hardware communication\n");
	
	// Initialize hardware structure
	memset(hw, 0, sizeof(*hw));
	hw->pdev = pdev;
	hw->msi_vector = RC_MSI_VECTOR;
	
	// Map MMIO space (BAR 5 from TRX50 testing)
	mmio_start = pci_resource_start(pdev, 5);
	mmio_len = pci_resource_len(pdev, 5);
	
	if (mmio_len < RC_MMIO_SIZE) {
		rc_printk(RC_ERROR, "rc_hw_init: BAR 5 too small: %llu bytes\n", 
			  (unsigned long long)mmio_len);
		return -ENOMEM;
	}
	
	hw->mmio_base = ioremap(mmio_start, mmio_len);
	if (!hw->mmio_base) {
		rc_printk(RC_ERROR, "rc_hw_init: failed to map MMIO space\n");
		return -ENOMEM;
	}
	
	rc_printk(RC_INFO, "rc_hw_init: mapped MMIO at 0x%llx (len=%llu)\n",
		  (unsigned long long)mmio_start, (unsigned long long)mmio_len);
	
	// Initialize spinlocks
	spin_lock_init(&hw->irq_lock);
	atomic_set(&hw->irq_count, 0);
	
	// Allocate DMA pool for command/completion queues
	hw->dma_pool = dma_pool_create("rcraid_hw", &pdev->dev,
				       sizeof(struct rc_hw_command), 64, 0);
	if (!hw->dma_pool) {
		rc_printk(RC_ERROR, "rc_hw_init: failed to create DMA pool\n");
		ret = -ENOMEM;
		goto err_unmap;
	}
	
	// Allocate command queue (32 entries)
	hw->cmd_queue_size = 32;
	hw->cmd_queue = dma_pool_alloc(hw->dma_pool, GFP_KERNEL, &hw->cmd_queue_dma);
	if (!hw->cmd_queue) {
		rc_printk(RC_ERROR, "rc_hw_init: failed to allocate command queue\n");
		ret = -ENOMEM;
		goto err_dma_pool;
	}
	memset(hw->cmd_queue, 0, sizeof(struct rc_hw_command) * hw->cmd_queue_size);
	
	// Allocate completion queue (32 entries)
	hw->comp_queue = dma_pool_alloc(hw->dma_pool, GFP_KERNEL, &hw->comp_queue_dma);
	if (!hw->comp_queue) {
		rc_printk(RC_ERROR, "rc_hw_init: failed to allocate completion queue\n");
		ret = -ENOMEM;
		goto err_cmd_queue;
	}
	memset(hw->comp_queue, 0, sizeof(struct rc_hw_completion) * hw->cmd_queue_size);
	
	// Initialize queue pointers
	hw->cmd_queue_head = 0;
	hw->cmd_queue_tail = 0;
	hw->comp_queue_head = 0;
	hw->comp_queue_tail = 0;
	
	// Program hardware registers with queue addresses
	writel(lower_32_bits(hw->cmd_queue_dma), hw->mmio_base + RC_REG_COMMAND_QUEUE);
	writel(upper_32_bits(hw->cmd_queue_dma), hw->mmio_base + RC_REG_COMMAND_QUEUE + 4);
	writel(hw->cmd_queue_size, hw->mmio_base + RC_REG_COMMAND_QUEUE + 8);
	
	writel(lower_32_bits(hw->comp_queue_dma), hw->mmio_base + RC_REG_COMPLETION_QUEUE);
	writel(upper_32_bits(hw->comp_queue_dma), hw->mmio_base + RC_REG_COMPLETION_QUEUE + 4);
	writel(hw->cmd_queue_size, hw->mmio_base + RC_REG_COMPLETION_QUEUE + 8);
	
	// Enable interrupts
	writel(0x1, hw->mmio_base + RC_REG_INTERRUPT_MASK);
	
	rc_printk(RC_INFO, "rc_hw_init: hardware initialized successfully\n");
	rc_printk(RC_INFO, "rc_hw_init: command queue at 0x%llx\n", 
		  (unsigned long long)hw->cmd_queue_dma);
	rc_printk(RC_INFO, "rc_hw_init: completion queue at 0x%llx\n", 
		  (unsigned long long)hw->comp_queue_dma);
	
	return 0;
	
err_cmd_queue:
	dma_pool_free(hw->dma_pool, hw->cmd_queue, hw->cmd_queue_dma);
err_dma_pool:
	dma_pool_destroy(hw->dma_pool);
err_unmap:
	iounmap(hw->mmio_base);
	return ret;
}

// Hardware cleanup
void rc_hw_cleanup(struct rc_hw_adapter *hw)
{
	rc_printk(RC_INFO, "rc_hw_cleanup: cleaning up hardware resources\n");
	
	if (hw->mmio_base) {
		// Disable interrupts
		writel(0x0, hw->mmio_base + RC_REG_INTERRUPT_MASK);
		iounmap(hw->mmio_base);
		hw->mmio_base = NULL;
	}
	
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

// Submit command to hardware
int rc_hw_submit_command(struct rc_hw_adapter *hw, struct rc_hw_command *cmd)
{
	unsigned long flags;
	u32 next_tail;
	
	spin_lock_irqsave(&hw->irq_lock, flags);
	
	// Check if queue is full
	next_tail = (hw->cmd_queue_tail + 1) % hw->cmd_queue_size;
	if (next_tail == hw->cmd_queue_head) {
		spin_unlock_irqrestore(&hw->irq_lock, flags);
		rc_printk(RC_WARN, "rc_hw_submit_command: command queue full\n");
		return -EBUSY;
	}
	
	// Copy command to queue
	memcpy(&hw->cmd_queue[hw->cmd_queue_tail], cmd, sizeof(*cmd));
	
	// Update tail pointer
	hw->cmd_queue_tail = next_tail;
	
	// Ring doorbell to notify hardware
	writel(hw->cmd_queue_tail, hw->mmio_base + RC_REG_DOORBELL);
	
	spin_unlock_irqrestore(&hw->irq_lock, flags);
	
	rc_printk(RC_DEBUG, "rc_hw_submit_command: submitted cmd_id=%u opcode=%u\n",
		  cmd->command_id, cmd->opcode);
	
	return 0;
}

// Process completion queue
int rc_hw_process_completions(struct rc_hw_adapter *hw)
{
	unsigned long flags;
	u32 processed = 0;
	struct rc_hw_completion *comp;
	
	spin_lock_irqsave(&hw->irq_lock, flags);
	
	while (hw->comp_queue_head != hw->comp_queue_tail) {
		comp = &hw->comp_queue[hw->comp_queue_head];
		
		// Check if completion is valid
		if (comp->command_id == 0) {
			break;
		}
		
		rc_printk(RC_DEBUG, "rc_hw_process_completions: cmd_id=%u status=%u bytes=%u\n",
			  comp->command_id, comp->status, comp->bytes_transferred);
		
		// Process completion (TODO: notify upper layers)
		// This is where we'd call back to the block layer
		
		// Clear completion
		memset(comp, 0, sizeof(*comp));
		
		// Update head pointer
		hw->comp_queue_head = (hw->comp_queue_head + 1) % hw->cmd_queue_size;
		processed++;
	}
	
	spin_unlock_irqrestore(&hw->irq_lock, flags);
	
	if (processed > 0) {
		rc_printk(RC_DEBUG, "rc_hw_process_completions: processed %u completions\n", processed);
	}
	
	return processed;
}

// Real interrupt handler using MSI vector 244
irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id)
{
	struct rc_hw_adapter *hw = &g_hw_adapter;
	u32 int_status;
	
	// Read interrupt status
	int_status = readl(hw->mmio_base + RC_REG_INTERRUPT_STATUS);
	
	if (!int_status) {
		return IRQ_NONE;
	}
	
	// Acknowledge interrupt
	writel(int_status, hw->mmio_base + RC_REG_INTERRUPT_STATUS);
	
	atomic_inc(&hw->irq_count);
	
	rc_printk(RC_DEBUG, "rc_hw_interrupt_handler: status=0x%x count=%d\n",
		  int_status, atomic_read(&hw->irq_count));
	
	// Process completions
	rc_hw_process_completions(hw);
	
	return IRQ_HANDLED;
}
