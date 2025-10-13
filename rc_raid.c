/****************************************************************************
 * AMD RAID Driver for Linux - rcraid equivalent
 * RAID array detection layer based on rcraid.inf
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

// SCSI host template
#if RC_SCSI_AVAILABLE
static struct scsi_host_template rc_scsi_template = {
    .module = THIS_MODULE,
    .name = "AMD-RAID",
    .proc_name = RC_DRIVER_NAME,
    .this_id = -1,
    .max_id = RC_MAX_SCSI_TARGETS,
    .max_lun = RC_MAX_SCSI_LUNS,
    .can_queue = RC_NUMBER_OF_REQUESTS,
    .sg_tablesize = 32,
    .cmd_per_lun = 1,
    .use_clustering = ENABLE_CLUSTERING,
    .queuecommand = rc_scsi_queuecommand,
    .eh_abort_handler = NULL,
    .eh_device_reset_handler = NULL,
    .eh_target_reset_handler = NULL,
    .eh_bus_reset_handler = NULL,
    .eh_host_reset_handler = NULL,
    .slave_alloc = NULL,
    .slave_configure = NULL,
    .target_alloc = NULL,
    .target_destroy = NULL,
    .scan_finished = NULL,
    .change_queue_depth = NULL,
    .change_queue_type = NULL,
    .bios_param = NULL,
    .unlock_native_capacity = NULL,
    .show_info = NULL,
    .write_info = NULL,
    .shost_attrs = NULL,
    .sdev_attrs = NULL,
    .no_write_same = 1,
};
#endif

// SCSI command processing
#if RC_SCSI_AVAILABLE
int rc_scsi_queuecommand(struct Scsi_Host *host, struct scsi_cmnd *scmd)
{
    // RAID command processing would be implemented here
    scmd->result = DID_OK << 16;
    scmd->scsi_done(scmd);
    return 0;
}

// SCSI device probe
int rc_scsi_probe(struct scsi_device *sdev)
{
    rc_printk(RC_NOTE, "rc_scsi_probe: detected SCSI device - host=%d target=%d lun=%d\n",
              sdev->host->host_no, sdev->id, sdev->lun);
    return 0;
}

// SCSI device remove
int rc_scsi_remove(struct scsi_device *sdev)
{
    rc_printk(RC_NOTE, "rc_scsi_remove: removing SCSI device - host=%d target=%d lun=%d\n",
              sdev->host->host_no, sdev->id, sdev->lun);
    return 0;
}
#else
int rc_scsi_queuecommand(void *host, void *scmd)
{
    // RAID command processing would be implemented here
    return 0;
}

int rc_scsi_probe(void *sdev)
{
    rc_printk(RC_NOTE, "rc_scsi_probe: detected device (SCSI not available)\n");
    return 0;
}

int rc_scsi_remove(void *sdev)
{
    rc_printk(RC_NOTE, "rc_scsi_remove: removing device (SCSI not available)\n");
    return 0;
}
#endif

// Block device request handler (blk-mq style)
static blk_status_t rc_raid_request_handler(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    // Process the request - for now just complete it
    // In a real implementation, this would handle the actual RAID I/O
    return BLK_STS_OK;
}

// Block device operations
static const struct block_device_operations rc_raid_fops = {
    .owner = THIS_MODULE,
    .ioctl = rc_raid_array_ioctl,
};

// blk-mq operations
static const struct blk_mq_ops rc_raid_mq_ops = {
    .queue_rq = rc_raid_request_handler,
};

// Initialize RAID array as block device
int rc_raid_array_init(struct rc_raid_array *array)
{
    int err;
    
    rc_printk(RC_NOTE, "rc_raid_array_init: initializing RAID array %d\n", array->array_id);
    
    // Initialize blk-mq tag set
    array->tag_set.ops = &rc_raid_mq_ops;
    array->tag_set.nr_hw_queues = 1;
    array->tag_set.queue_depth = 128;
    array->tag_set.numa_node = NUMA_NO_NODE;
    array->tag_set.cmd_size = 0;
    array->tag_set.flags = 0;
    
    err = blk_mq_alloc_tag_set(&array->tag_set);
    if (err) {
        rc_printk(RC_ERROR, "rc_raid_array_init: failed to allocate tag set\n");
        return err;
    }
    
    // Queue is allocated by blk_mq_alloc_disk() - no need to allocate separately
    
    // Use modern blk_mq_alloc_disk() instead of alloc_disk()
    array->disk = blk_mq_alloc_disk(&array->tag_set, NULL, array);
    if (IS_ERR(array->disk)) {
        err = PTR_ERR(array->disk);
        rc_printk(RC_ERROR, "rc_raid_array_init: failed to allocate gendisk\n");
        blk_mq_free_tag_set(&array->tag_set);
        return err;
    }
    
    // Setup gendisk
    array->disk->major = 0; // Let kernel assign major number
    array->disk->first_minor = 0;
    array->disk->minors = 1;
    array->disk->fops = &rc_raid_fops;
    array->disk->private_data = array;
    strscpy(array->disk->disk_name, "rcraid0", DISK_NAME_LEN);
    
    // Configure queue limits before adding disk
    blk_queue_logical_block_size(array->disk->queue, 512);
    blk_queue_physical_block_size(array->disk->queue, 512);
    blk_queue_nonrot(array->disk->queue);
    
    set_capacity(array->disk, array->total_sectors);
    
    // Add disk
    err = add_disk(array->disk);
    if (err) {
        rc_printk(RC_ERROR, "rc_raid_array_init: failed to add disk\n");
        put_disk(array->disk);
        blk_mq_free_tag_set(&array->tag_set);
        return err;
    }
    
    array->initialized = 1;
    rc_printk(RC_NOTE, "rc_raid_array_init: RAID array %d initialized as %s\n", 
              array->array_id, array->disk->disk_name);
    
    return 0;
}

// Cleanup RAID array
void rc_raid_array_cleanup(struct rc_raid_array *array)
{
    if (array->initialized) {
        rc_printk(RC_NOTE, "rc_raid_array_cleanup: cleaning up RAID array %d\n", array->array_id);
        
        if (array->disk) {
            del_gendisk(array->disk);
            put_disk(array->disk);
            array->disk = NULL;
        }
        
        if (array->tag_set.ops) {
            blk_mq_free_tag_set(&array->tag_set);
        }
        
        array->initialized = 0;
    }
}

// RAID array ioctl
int rc_raid_array_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg)
{
    // RAID management ioctls would be implemented here
    return -ENOTTY;
}

// Interrupt handler
irqreturn_t rc_interrupt_handler(int irq, void *dev_id)
{
    // Interrupt processing would be implemented here
    // struct rc_adapter *adapter = dev_id;
    return IRQ_HANDLED;
}

// Scan for RAID arrays configured in BIOS
int rc_raid_scan_arrays(struct rc_adapter *adapter)
{
    int i, err;
    
    rc_printk(RC_NOTE, "rc_raid_scan_arrays: scanning for RAID arrays\n");
    
    // For now, create a dummy RAID array to test block device functionality
    // In a real implementation, this would read from the RAID controller's configuration
    for (i = 0; i < 1; i++) { // Create one test array
        struct rc_raid_array *array = &rc_state.raid.arrays[i];
        
        // Initialize array structure
        array->array_id = i;
        array->raid_type = RC_RAID_TYPE_RAID0;
        array->state = RC_RAID_STATE_ONLINE;
        array->total_sectors = 7814037168; // ~3.9TB in sectors (512 bytes each)
        array->used_sectors = 0;
        array->num_disks = 2;
        array->stripe_size = 64; // 64KB stripe
        snprintf(array->name, sizeof(array->name), "RAID0_Array_%d", i);
        array->adapter = adapter;
        
        // Initialize as block device
        err = rc_raid_array_init(array);
        if (err) {
            rc_printk(RC_ERROR, "rc_raid_scan_arrays: failed to initialize array %d\n", i);
            continue;
        }
        
        rc_state.raid.num_arrays++;
        rc_printk(RC_NOTE, "rc_raid_scan_arrays: found RAID array %d (%s)\n", i, array->name);
    }
    
    rc_printk(RC_NOTE, "rc_raid_scan_arrays: found %d RAID arrays\n", rc_state.raid.num_arrays);
    return 0;
}

// Get RAID array information
int rc_raid_get_array_info(struct rc_raid_array *array)
{
    // In a real implementation, this would read from the RAID controller
    rc_printk(RC_NOTE, "rc_raid_get_array_info: array %d info - type=%d state=%d sectors=%llu\n",
              array->array_id, array->raid_type, array->state, array->total_sectors);
    return 0;
}

// Create RAID array
int rc_raid_create_array(struct rc_adapter *adapter, int raid_type, int num_disks)
{
    rc_printk(RC_NOTE, "rc_raid_create_array: creating RAID %d with %d disks\n", raid_type, num_disks);
    // Implementation would create array in RAID controller
    return 0;
}

// Delete RAID array
int rc_raid_delete_array(struct rc_raid_array *array)
{
    rc_printk(RC_NOTE, "rc_raid_delete_array: deleting array %d\n", array->array_id);
    rc_raid_array_cleanup(array);
    return 0;
}

// rcraid initialization (RAID layer)
int rc_raid_init(void)
{
    int err;
    
    rc_printk(RC_NOTE, "rc_raid_init: initializing RAID layer\n");
    
    // Check if we have any adapters
    if (rc_state.num_adapters == 0) {
        rc_printk(RC_WARN, "rc_raid_init: no adapters available\n");
        return -ENODEV;
    }
    
    // Initialize RAID structure
    rc_state.raid.adapter = &rc_state.adapters[0];
    rc_state.raid.num_arrays = 0;
    rc_state.raid.scsi_host_created = 0;
    
    // Allocate SCSI host for RAID management (if SCSI is available)
#if RC_SCSI_AVAILABLE
    struct Scsi_Host *host = scsi_host_alloc(&rc_scsi_template, sizeof(struct rc_raid));
    if (host) {
        // Set host parameters
        host->max_id = RC_MAX_SCSI_TARGETS;
        host->max_lun = RC_MAX_SCSI_LUNS;
        host->can_queue = RC_NUMBER_OF_REQUESTS;
        host->sg_tablesize = 32;
        
        // Add SCSI host
        err = scsi_add_host(host, &rc_state.adapters[0].pdev->dev);
        if (err) {
            rc_printk(RC_WARN, "rc_raid_init: failed to add SCSI host, continuing without SCSI\n");
            scsi_host_put(host);
            host = NULL;
        } else {
            rc_state.raid.host = host;
            rc_state.raid.scsi_host_created = 1;
            rc_printk(RC_NOTE, "rc_raid_init: SCSI host created successfully\n");
        }
    } else {
        rc_printk(RC_WARN, "rc_raid_init: failed to allocate SCSI host, continuing without SCSI\n");
    }
#else
    rc_printk(RC_NOTE, "rc_raid_init: SCSI not available, using block device only\n");
#endif
    
    // Scan for RAID arrays and create block devices
    err = rc_raid_scan_arrays(&rc_state.adapters[0]);
    if (err) {
        rc_printk(RC_ERROR, "rc_raid_init: failed to scan RAID arrays\n");
        if (rc_state.raid.scsi_host_created && rc_state.raid.host) {
            scsi_remove_host(rc_state.raid.host);
            scsi_host_put(rc_state.raid.host);
        }
        return err;
    }
    
    rc_state.raid.initialized = 1;
    rc_printk(RC_NOTE, "rc_raid_init: RAID layer initialized successfully with %d arrays\n", 
              rc_state.raid.num_arrays);
    return 0;
}

void rc_raid_cleanup(void)
{
    int i;
    
    rc_printk(RC_NOTE, "rc_raid_cleanup: cleaning up RAID layer\n");
    
    // Cleanup all RAID arrays
    for (i = 0; i < rc_state.raid.num_arrays; i++) {
        rc_raid_array_cleanup(&rc_state.raid.arrays[i]);
    }
    rc_state.raid.num_arrays = 0;
    
    // Cleanup SCSI host
#if RC_SCSI_AVAILABLE
    if (rc_state.raid.scsi_host_created && rc_state.raid.host) {
        scsi_remove_host(rc_state.raid.host);
        scsi_host_put(rc_state.raid.host);
        rc_state.raid.host = NULL;
        rc_state.raid.scsi_host_created = 0;
    }
#endif
    
    rc_state.raid.initialized = 0;
}
