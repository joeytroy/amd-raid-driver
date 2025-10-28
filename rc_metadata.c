/****************************************************************************
 * AMD RAID Driver for Linux - Metadata Discovery
 * Real RAID array discovery from firmware
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

// RAID metadata structures are defined in rc_linux.h

// Discover RAID arrays from firmware
int rc_discover_arrays(struct rc_adapter *adapter)
{
    struct rc_hw_command cmd = {0};
    struct rc_hw_completion comp = {0};
    dma_addr_t dma_addr;
    void *dma_buf;
    u32 bytes;
    int ret;
	
	rc_printk(RC_INFO, "rc_discover_arrays: discovering RAID arrays from firmware\n");
	
	// Allocate DMA buffer for metadata
	dma_buf = dma_pool_alloc(adapter->hw.dma_pool, GFP_KERNEL, &dma_addr);
	if (!dma_buf) {
		rc_printk(RC_ERROR, "rc_discover_arrays: failed to allocate DMA buffer\n");
		return -ENOMEM;
	}
	
	// Prepare metadata read command
	cmd.command_id = atomic_inc_return(&adapter->hw.cmd_sequence);
	cmd.opcode = RC_CMD_METADATA_READ;
	cmd.flags = RC_CMD_FLAG_SYNC;
	cmd.channel_id = 0;
	cmd.lba = 0; // Read from beginning
	cmd.sector_count = 1; // One sector of metadata
	cmd.data_addr = dma_addr;
	cmd.completion_addr = 0;
	cmd.generation_number = 0;
	
	rc_printk(RC_DEBUG, "rc_discover_arrays: submitting metadata read command\n");
	
	// Submit command to hardware
    ret = rc_hw_submit_sync_command(&adapter->hw, &cmd, &comp, 5000);
    if (ret < 0) {
        rc_printk(RC_ERROR, "rc_discover_arrays: command timeout (%d)\n", ret);
        dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);
        return ret;
    }

    if (comp.status != RC_STATUS_SUCCESS) {
        rc_printk(RC_WARN,
                  "rc_discover_arrays: firmware status=%u error=0x%x\n",
                  comp.status, comp.error_code);
        dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);
        return -EIO;
    }

    bytes = min(comp.bytes_transferred, (u32)0x1000);
    rc_printk(RC_INFO,
              "rc_discover_arrays: firmware metadata retrieved (%u bytes)\n",
              bytes);

    // TODO: Parse OSIC metadata layout and populate arrays.

    dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);

    return 0;
}

// Read specific array metadata
int rc_read_array_metadata(struct rc_adapter *adapter, int array_id, 
			   struct rc_raid_metadata *metadata)
{
    struct rc_hw_command cmd = {0};
    struct rc_hw_completion comp = {0};
    dma_addr_t dma_addr;
    void *dma_buf;
    int ret;
	
	rc_printk(RC_DEBUG, "rc_read_array_metadata: reading metadata for array %d\n", array_id);
	
	// Allocate DMA buffer for metadata
	dma_buf = dma_pool_alloc(adapter->hw.dma_pool, GFP_KERNEL, &dma_addr);
	if (!dma_buf) {
		rc_printk(RC_ERROR, "rc_read_array_metadata: failed to allocate DMA buffer\n");
		return -ENOMEM;
	}
	
	// Prepare metadata read command
	cmd.command_id = atomic_inc_return(&adapter->hw.cmd_sequence);
	cmd.opcode = RC_CMD_METADATA_READ;
	cmd.flags = RC_CMD_FLAG_SYNC;
	cmd.channel_id = array_id;
	cmd.lba = 0;
	cmd.sector_count = 1;
	cmd.data_addr = dma_addr;
	cmd.completion_addr = 0;
	cmd.generation_number = 0;
	
	// Submit command to hardware
    ret = rc_hw_submit_sync_command(&adapter->hw, &cmd, &comp, 5000);
    if (ret < 0) {
        rc_printk(RC_ERROR, "rc_read_array_metadata: command timeout (%d)\n", ret);
        dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);
        return ret;
    }

    if (comp.status != RC_STATUS_SUCCESS) {
        rc_printk(RC_WARN,
                  "rc_read_array_metadata: firmware status=%u error=0x%x\n",
                  comp.status, comp.error_code);
        dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);
        return -EIO;
    }

    if (comp.bytes_transferred < sizeof(*metadata)) {
        rc_printk(RC_WARN,
                  "rc_read_array_metadata: insufficient data (%u bytes)\n",
                  comp.bytes_transferred);
        dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);
        return -EMSGSIZE;
    }

    memcpy(metadata, dma_buf, sizeof(*metadata));
    rc_printk(RC_DEBUG,
              "rc_read_array_metadata: array %d metadata copied (%u bytes)\n",
              array_id, comp.bytes_transferred);

    dma_pool_free(adapter->hw.dma_pool, dma_buf, dma_addr);

    return 0;
}

// Scan for physical disks
int rc_scan_physical_disks(struct rc_adapter *adapter)
{
	struct rc_hw_command cmd = {0};
    struct rc_hw_completion comp = {0};
    int ret;
	
	rc_printk(RC_INFO, "rc_scan_physical_disks: scanning for physical disks\n");
	
	// Prepare disk scan command
	cmd.command_id = atomic_inc_return(&adapter->hw.cmd_sequence);
	cmd.opcode = RC_CMD_SCAN_DISKS;
	cmd.flags = RC_CMD_FLAG_SYNC;
	cmd.channel_id = 0;
	cmd.lba = 0;
	cmd.sector_count = 0;
	cmd.data_addr = 0;
	cmd.completion_addr = 0;
	cmd.generation_number = 0;
	
	// Submit command to hardware
    ret = rc_hw_submit_sync_command(&adapter->hw, &cmd, &comp, 5000);
    if (ret < 0) {
        rc_printk(RC_ERROR, "rc_scan_physical_disks: command timeout (%d)\n", ret);
        return ret;
    }

    if (comp.status != RC_STATUS_SUCCESS) {
        rc_printk(RC_WARN,
                  "rc_scan_physical_disks: firmware status=%u error=0x%x\n",
                  comp.status, comp.error_code);
        return -EIO;
    }

    rc_printk(RC_INFO, "rc_scan_physical_disks: disk scan completed\n");

    return 0;
}
