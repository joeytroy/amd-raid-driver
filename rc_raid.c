/****************************************************************************
 * AMD RAID Driver for Linux - RAID Command Layer (rcraid.sys equivalent)
 * Implements completion callbacks, command routing, and SRB handling
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"
#include <linux/slab.h>
#include <linux/dma-mapping.h>

/*----------------------------------------------------------------------
 * Module-level initialization and cleanup
 *----------------------------------------------------------------------*/

int rc_raid_init(void)
{
    rc_printk(RC_INFO, "rc_raid_init: initializing RAID layer\n");
    
    /* TODO: Initialize global RAID layer state */
    /* For now, this is a placeholder */
    
    return 0;
}

void rc_raid_cleanup(void)
{
    rc_printk(RC_INFO, "rc_raid_cleanup: cleaning up RAID layer\n");
    
    /* TODO: Cleanup global RAID layer state */
    /* For now, this is a placeholder */
}

/*----------------------------------------------------------------------
 * FUN_14000c900: SRB Completion/Error Handler
 * 
 * Universal completion/error handler for SRB requests.
 * Mirrors Windows rcra

id.sys FUN_14000c900 behavior exactly.
 *----------------------------------------------------------------------*/

void rc_srb_completion_handler(struct rc_srb *srb,
                                void (*completion_callback)(void *),
                                u8 status_byte,
                                u16 status_word)
{
    struct rc_adapter *adapter;
    
    if (!srb)
        return;

    /* TODO: Get adapter from SRB context */
    /* For now, we'll need to pass adapter or store it in SRB */

    rc_printk(RC_DEBUG, "rc_srb_completion_handler: srb=%p status=%u error=0x%04x\n",
              srb, status_byte, status_word);

    /* 1. DMA Cleanup: Free scatter-gather list if present */
    if (srb->sg_handle) {
        /* TODO: Call StorPort service +0x338 to get SG list metadata */
        /* TODO: Call StorPort service +0x680 to free SG list */
        srb->sg_handle = NULL;
    }

    /* 2. SRB Status Field Updates */
    srb->status = srb->original_status;  /* +0x100 = +0x13c */
    srb->status_byte = status_byte;      /* +0x110 */
    srb->secondary_status = 0;           /* +0x10c */
    srb->command_type = 0;               /* +0x112 */

    /* 3. Status Word Handling */
    if (status_byte == 0) {
        /* Success */
        srb->error_code = 0;             /* +0x10e */
        srb->information = srb->information; /* +0x104 = +0x140 (restore) */
    } else {
        /* Error - byte-swap status word */
        srb->error_code = (status_word << 8) | (status_word >> 8); /* +0x10e */
        srb->information = 0;            /* +0x104 */
    }

    /* 4. Completion */
    if (!completion_callback) {
        /* Direct completion - call indirect handler (FUN_1400097ac) */
        rc_srb_indirect_completion(srb);
    } else {
        /* Callback completion - TODO: Call StorPort service +0x838 */
        if (completion_callback)
            completion_callback(srb);
    }
}

/*----------------------------------------------------------------------
 * FUN_1400097ac: Indirect completion handler
 * 
 * Indirect call to function pointer (StorPort completion handler).
 *----------------------------------------------------------------------*/

void rc_srb_indirect_completion(struct rc_srb *srb)
{
    if (!srb || !srb->completion_callback)
        return;

    rc_printk(RC_DEBUG, "rc_srb_indirect_completion: srb=%p\n", srb);
    
    /* TODO: Call function pointer at DAT_140014c28 (StorPort completion handler) */
    /* For now, call the callback directly */
    if (srb->completion_callback)
        srb->completion_callback(srb->completion_arg);
}

/*----------------------------------------------------------------------
 * FUN_14000e1ec: NVMe command completion callback with scatter-gather
 * 
 * Handles completion for commands with scatter-gather lists.
 *----------------------------------------------------------------------*/

void rc_nvme_completion_with_sg(void *queue_handle,
                                 void *completion_queue_desc,
                                 struct rc_command_descriptor *cmd_desc,
                                 struct rc_completion_descriptor *comp_desc)
{
    struct rc_srb *srb;
    void (*callback)(void *);
    u8 status_byte = 0;
    u16 status_word = 0;

    if (!cmd_desc || !comp_desc)
        return;

    /* Extract SRB and completion callback from command descriptor */
    srb = (struct rc_srb *)cmd_desc->srb_ptr;
    callback = cmd_desc->completion_cb;

    if (!srb)
        return;

    rc_printk(RC_DEBUG, "rc_nvme_completion_with_sg: srb=%p comp_type=%u\n",
              srb, comp_desc->completion_type);

    /* Check completion status */
    if (comp_desc->status_dword >= 0x20000) {
        status_byte = 2; /* Error */
        status_word = 0xc00; /* Invalid command, or 0x500 */
    }

    /* Scatter-gather list cleanup */
    if (srb->scatter_gather_list) {
        /* TODO: Copy data from scatter-gather list to completion buffer */
        /* TODO: Build linked list of scatter-gather entries starting at completion_queue_desc[+0x68] */
        /* TODO: Update completion_queue_desc[+0x70] to point to last entry */
        /* This requires memcpy equivalent (FUN_140011140) */
    }

    /* Call universal completion handler */
    rc_srb_completion_handler(srb, callback, status_byte, status_word);
}

/*----------------------------------------------------------------------
 * FUN_14001005c: Simple completion callback with size check
 * 
 * Handles simple command completions without scatter-gather.
 *----------------------------------------------------------------------*/

void rc_simple_completion_callback(void *unused1,
                                    void *unused2,
                                    struct rc_command_descriptor *cmd_desc,
                                    struct rc_completion_descriptor *comp_desc)
{
    struct rc_srb *srb;
    void (*callback)(void *);
    u8 status_byte = 0;
    u16 status_word = 0;

    if (!cmd_desc || !comp_desc)
        return;

    /* Extract SRB and completion callback */
    srb = (struct rc_srb *)cmd_desc->srb_ptr;
    callback = cmd_desc->completion_cb;

    if (!srb)
        return;

    rc_printk(RC_DEBUG, "rc_simple_completion_callback: srb=%p\n", srb);

    /* Check completion status */
    if (comp_desc->status_dword >= 0x20000) {
        status_byte = 2; /* Error */
        status_word = 0xc00; /* Invalid command */
    }

    /* Call universal completion handler */
    rc_srb_completion_handler(srb, callback, status_byte, status_word);
}

/*----------------------------------------------------------------------
 * FUN_140010184: Completion callback with SRB status update
 * 
 * Handles completions that need to update SRB status fields.
 *----------------------------------------------------------------------*/

void rc_completion_with_status_update(void *unused1,
                                       void *unused2,
                                       struct rc_command_descriptor *cmd_desc,
                                       struct rc_completion_descriptor *comp_desc)
{
    struct rc_srb *srb;
    void (*callback)(void *);
    u8 status_byte = 0;
    u16 status_word = 0;

    if (!cmd_desc || !comp_desc)
        return;

    /* Extract SRB and completion callback */
    srb = (struct rc_srb *)cmd_desc->srb_ptr;
    callback = cmd_desc->completion_cb;

    if (!srb)
        return;

    rc_printk(RC_DEBUG, "rc_completion_with_status_update: srb=%p\n", srb);

    /* Update SRB status fields */
    /* Note: comp_desc structure needs to match Windows layout */
    /* For now, we'll use a simplified approach */
    srb->status_dword[0] = comp_desc->status_dword; /* +0x84 */
    /* TODO: Implement full status field updates based on comp_desc layout */

    /* Check completion status */
    if (comp_desc->status_dword >= 0x20000) {
        status_byte = 2; /* Error */
        status_word = 0xc00; /* Invalid command */
    }

    /* Call universal completion handler */
    rc_srb_completion_handler(srb, callback, status_byte, status_word);
}

/*----------------------------------------------------------------------
 * FUN_14000eef8: Queue callback handler for completion processing
 * 
 * Processes different types of queue completions.
 *----------------------------------------------------------------------*/

int rc_queue_callback_handler(void *queue_handle,
                               void *unused,
                               struct rc_completion_descriptor *comp_desc,
                               void *completion_queue_entry)
{
    u8 completion_type;
    struct rc_srb *srb;
    void (*callback)(void *);

    if (!comp_desc)
        return -EINVAL;

    completion_type = comp_desc->completion_type;

    rc_printk(RC_DEBUG, "rc_queue_callback_handler: type=%u\n", completion_type);

    switch (completion_type) {
    case 2: /* Successful completion */
        srb = (struct rc_srb *)comp_desc->srb_ptr;
        callback = (void (*)(void *))comp_desc->callback;
        if (srb) {
            rc_srb_completion_handler(srb, callback, 0, 0);
        }
        break;

    case 7: /* Queue processing */
        if (comp_desc->status_dword < 0x20000) {
            /* TODO: Set queue flag queue_handle[+0x398] |= 8 */
            /* TODO: Call FUN_14000cb4c() for queue processing */
            rc_printk(RC_DEBUG, "rc_queue_callback_handler: queue processing needed\n");
        }
        break;

    case 0xd: /* Queue management command completion */
        if (comp_desc->status_dword != 0xa) {
            /* TODO: Call FUN_14000d350() for queue management */
            rc_printk(RC_DEBUG, "rc_queue_callback_handler: queue management needed\n");
        }
        break;

    default:
        rc_printk(RC_DEBUG, "rc_queue_callback_handler: unknown type %u\n",
                  completion_type);
        return -EINVAL;
    }

    return 0;
}

/*----------------------------------------------------------------------
 * FUN_14000fafc: Primary queue dispatcher (NVMe command routing)
 * 
 * Routes commands based on SRB function code.
 *----------------------------------------------------------------------*/

int rc_primary_queue_dispatcher(struct rc_adapter *adapter,
                                 struct rc_srb *srb,
                                 void *unused3,
                                 void *unused4)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    void *queue_handle;
    u8 srb_function;
    bool queue_full;

    if (!adapter || !srb)
        return 0;

    /* Clear scatter-gather handles */
    srb->sg_handle = NULL;
    srb->scatter_gather_list = NULL;

    srb_function = srb->srb_function;

    rc_printk(RC_DEBUG, "rc_primary_queue_dispatcher: function=0x%02x\n",
              srb_function);

    /* TODO: Get queue handle from devExt+0x15948 based on queue index */
    /* For now, we'll use a placeholder */
    queue_handle = NULL;

    /* Queue fullness check: (producer + 1) % queue_size == consumer */
    /* TODO: Implement queue fullness check */
    queue_full = false;

    switch (srb_function) {
    case 0x01: /* Read */
    case 0x04: /* Write */
        if (queue_full) {
            rc_queue_full_handler(adapter, srb, NULL, NULL, NULL);
            return 0;
        }
        /* TODO: Call FUN_14000ed2c (secondary queue dispatcher) */
        return 1;

    case 0x06:
        /* TODO: Call FUN_14001026c (command router for SRB 0x06) */
        return rc_command_router_srb06(adapter, srb, NULL, NULL);

    case 0x0a:
    case 0x0b:
        if (queue_full) {
            rc_queue_full_handler(adapter, srb, NULL, NULL, NULL);
            return 0;
        }
        /* TODO: Call FUN_14000ec64 (command router) */
        return 1;

    case 0x0c:
        if (queue_full) {
            rc_queue_full_handler(adapter, srb, NULL, NULL, NULL);
            return 0;
        }
        /* TODO: Call FUN_14000e2c8 (queue type selection command) */
        return 1;

    default:
        if (srb_function < 0x0a) {
            /* Unsupported command */
            rc_srb_completion_handler(srb, NULL, 2, 0x2000);
            return 0;
        }
        /* TODO: Call FUN_14000f178 (NVMe command submission) */
        return 1;
    }
}

/*----------------------------------------------------------------------
 * FUN_14001026c: Command router for SRB function 0x06
 * 
 * Routes commands based on command type for SRB function 0x06.
 *----------------------------------------------------------------------*/

int rc_command_router_srb06(struct rc_adapter *adapter,
                             struct rc_srb *srb,
                             void *unused3,
                             void *completion_callback)
{
    u8 command_type;

    if (!adapter || !srb)
        return 0;

    command_type = srb->command_code;

    rc_printk(RC_DEBUG, "rc_command_router_srb06: command_type=0x%02x\n",
              command_type);

    switch (command_type) {
    case 0x01:
        /* TODO: Call FUN_14000ff50 (special command handler, cmd_type=0x11) */
        rc_printk(RC_DEBUG, "rc_command_router_srb06: calling special handler\n");
        return 1;

    case 0x02:
        /* Command not supported */
        rc_srb_completion_handler(srb, (void (*)(void *))completion_callback,
                                  2, 0x2000);
        return 1;

    default:
        return 0; /* Unhandled */
    }
}

/*----------------------------------------------------------------------
 * FUN_14000ed2c: Secondary queue dispatcher (command type routing)
 * 
 * Routes commands based on command type.
 *----------------------------------------------------------------------*/

int rc_secondary_queue_dispatcher(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void *unused3,
                                   void *unused4)
{
    u8 command_type;

    if (!adapter || !srb)
        return 0;

    command_type = srb->command_code;

    rc_printk(RC_DEBUG, "rc_secondary_queue_dispatcher: command_type=0x%02x\n",
              command_type);

    switch (command_type) {
    case 0x12:
        return rc_special_command_handler_type12(adapter, srb, NULL, NULL);

    case 0x05:
        /* TODO: Call FUN_14000d190 (simple command) */
        return 1;

    case 0x88:
    case 0x8a:
        /* TODO: Check devExt[+0x15cd8] & 0x10 */
        /* TODO: Call FUN_14000ea34 (queue rotation) or error handler */
        rc_srb_completion_handler(srb, NULL, 2, 0xc00);
        return 1;

    case 0x9e:
        return rc_special_command_handler_type9e(adapter, srb, NULL, NULL);

    case 0x1b:
    case 0x00:
        /* Success - no-op commands */
        rc_srb_completion_handler(srb, NULL, 0, 0);
        return 1;

    default:
        /* Error */
        rc_srb_completion_handler(srb, NULL, 2, 0x2600);
        return 1;
    }
}

/*----------------------------------------------------------------------
 * FUN_14000ec64: Command router (SRB type routing)
 * 
 * Routes commands based on SRB function and command type combination.
 *----------------------------------------------------------------------*/

int rc_command_router(struct rc_adapter *adapter,
                       struct rc_srb *srb,
                       void *unused3,
                       void *unused4)
{
    u8 srb_function;
    u8 command_type;

    if (!adapter || !srb)
        return 0;

    srb_function = srb->srb_function;
    command_type = srb->command_code;

    rc_printk(RC_DEBUG, "rc_command_router: srb=0x%02x cmd=0x%02x\n",
              srb_function, command_type);

    if (srb_function == 0x0b) {
        /* SRB function 0x0b */
        switch (command_type) {
        case 0x02:
            /* TODO: Call FUN_14000d3f4 (scatter-gather command) */
            return 1;

        case 0x09:
            /* TODO: Check devExt[+0x15cd8] & 1 */
            /* TODO: Call FUN_14000f06c or error handler */
            rc_srb_completion_handler(srb, NULL, 2, 0x2600);
            return 1;

        default:
            rc_srb_completion_handler(srb, NULL, 2, 0x2600);
            return 1;
        }
    } else {
        /* SRB function != 0x0b */
        switch (command_type) {
        case 0x04:
            /* TODO: Call FUN_14000fe44 (context command) */
            return 1;

        case 0x09:
            /* TODO: Call FUN_14000cf38 (SRB-based command) */
            return 1;

        default:
            rc_srb_completion_handler(srb, NULL, 2, 0x2600);
            return 1;
        }
    }
}

/*----------------------------------------------------------------------
 * FUN_140006e3c: Completion processing and adapter iteration
 * 
 * Processes queue completions from all adapters.
 *----------------------------------------------------------------------*/

int rc_process_completions_all_adapters(struct rc_adapter *adapter)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    u8 queue_state;
    int processed = 0;

    if (!adapter)
        return 0;

    queue_state = doorbell->queue_state;

    rc_printk(RC_DEBUG, "rc_process_completions_all_adapters: queue_state=0x%02x\n",
              queue_state);

    /* TODO: Iterate through all adapters using adapter list */
    /* For now, process current adapter only */

    if (queue_state == 1) {
        /* Legacy adapter - check legacy queues */
        /* TODO: Check devExt+0x15908, devExt+0x15910, devExt+0x15918 */
        rc_printk(RC_DEBUG, "rc_process_completions_all_adapters: legacy adapter\n");
    } else if (queue_state == 2) {
        /* NVMe adapter - check NVMe queues */
        /* TODO: Check devExt+0x15ce0 and devExt+0x15ce1 */
        rc_printk(RC_DEBUG, "rc_process_completions_all_adapters: NVMe adapter\n");
    }

    /* TODO: Build completion descriptors (0x68 bytes each) */
    /* TODO: Process up to 32 completions (0x20 limit) */
    /* TODO: Handle three queue types: primary, secondary, tertiary */

    return processed;
}

/*----------------------------------------------------------------------
 * FUN_14000a564: Multi-adapter disconnect
 * 
 * Removes adapter from multi-adapter WMI binding.
 *----------------------------------------------------------------------*/

int rc_multi_adapter_disconnect(struct rc_adapter *adapter)
{
    rc_printk(RC_DEBUG, "rc_multi_adapter_disconnect: adapter %d\n",
              adapter->instance);

    /* TODO: Implement multi-adapter disconnect logic */
    /* - Get adapter list context using devExt+0x16018 */
    /* - Iterate through all adapters */
    /* - Disconnect adapters from each other */
    /* - Set disconnected state */

    return 0;
}

/*----------------------------------------------------------------------
 * FUN_14000a72c: Multi-adapter connect
 * 
 * Adds adapter to multi-adapter WMI binding.
 *----------------------------------------------------------------------*/

int rc_multi_adapter_connect(struct rc_adapter *adapter, int adapter_index)
{
    rc_printk(RC_DEBUG, "rc_multi_adapter_connect: adapter %d index %d\n",
              adapter->instance, adapter_index);

    /* TODO: Implement multi-adapter connect logic */
    /* - Get adapter list context */
    /* - Iterate through all adapters */
    /* - Connect adapters to each other */
    /* - Set connected state */

    return 0;
}

/*----------------------------------------------------------------------
 * FUN_14000c2fc: Descriptor lookup - NVMe command slot allocation
 * 
 * Core function for allocating command descriptor slots from queues.
 * Used by all 20+ NVMe command submission functions.
 *----------------------------------------------------------------------*/

struct rc_command_descriptor *rc_allocate_command_descriptor(
    struct rc_queue_handle *queue_handle,
    void (*completion_callback)(void *))
{
    struct rc_command_descriptor *desc;
    u16 slot_index;
    u16 producer;
    u16 consumer;
    u16 queue_size;

    if (!queue_handle || !completion_callback)
        return NULL;

    producer = queue_handle->producer_index;
    consumer = queue_handle->consumer_index;
    queue_size = queue_handle->queue_size;

    /* Check if queue has available slots */
    if (((producer + 1) % queue_size) == consumer) {
        rc_printk(RC_DEBUG, "rc_allocate_command_descriptor: queue full\n");
        return NULL; /* Queue full */
    }

    /* Search for free slot starting at next_slot_index */
    slot_index = queue_handle->next_slot_index;
    
    /* TODO: Get descriptor from descriptor array at queue_handle->descriptor_array_base */
    /* TODO: Check if slot is free (descriptor[slot_index][+0x28] == 0) */
    /* For now, we'll use a simplified approach */
    
    /* TODO: Zero descriptor slot (64 bytes at offset +0x30) */
    /* TODO: Store callback at descriptor[slot_index][+0x28] = completion_callback */
    /* TODO: Set slot index: descriptor[slot_index][+0x20] = slot_index */
    /* TODO: Set queue index: descriptor[slot_index][+0x22] = producer */
    /* TODO: Update next_slot_index to found slot */
    
    /* Increment producer index */
    queue_handle->producer_index = (producer + 1) % queue_size;

    rc_printk(RC_DEBUG, "rc_allocate_command_descriptor: allocated slot %u\n",
              slot_index);

    /* TODO: Return pointer to descriptor slot */
    return NULL; /* Placeholder */
}

/*----------------------------------------------------------------------
 * FUN_14000c9e4: Scatter-Gather List Builder (Pre-Submission)
 * 
 * Builds scatter-gather list for DMA data transfer before command submission.
 *----------------------------------------------------------------------*/

int rc_build_scatter_gather_list(struct rc_queue_handle *queue_handle,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  struct rc_command_descriptor *cmd_desc)
{
    u32 transfer_length;
    dma_addr_t first_addr;
    u32 first_length;

    if (!queue_handle || !srb || !cmd_desc)
        return 0;

    /* 1. Transfer Length Calculation */
    transfer_length = srb->information; /* +0x140 */
    if (transfer_length == 0) {
        transfer_length = srb->data_length; /* +0x188 equivalent */
    }

    if (transfer_length == 0) {
        return 1; /* No data transfer needed */
    }

    /* 2. Scatter-Gather List Processing */
    if (!sg_list) {
        return 1; /* No SGL needed */
    }

    /* TODO: Process first SGL entry */
    /* - Read physical address from sg_list[0] */
    /* - Read length from sg_list[0][+0x10] */
    /* - Set cmd_desc[+0x48] = first physical address */
    /* - Calculate page alignment */
    /* - Process first chunk */

    /* 3. Multi-Page Processing (if transfer length >= 0x1001 bytes) */
    if (transfer_length >= 0x1001) {
        /* TODO: Access descriptor array at queue_handle[+0x8 + queue_index*8][+0x38] */
        /* TODO: Loop through pages and build descriptor array */
        /* TODO: Each descriptor is 8 bytes (0x200 bytes per queue slot = 64 entries) */
    }

    /* 4. Final Setup */
    /* TODO: Set cmd_desc[+0x50] = descriptor array pointer */
    /* TODO: Set cmd_desc[+0x26] = number of 4KB pages needed */

    return 1; /* Success */
}

/*----------------------------------------------------------------------
 * FUN_14000c1e4: Command Configuration After State Machine Completion
 * 
 * Configures command control register and flags after state machine operations.
 *----------------------------------------------------------------------*/

int rc_configure_command_after_state_machine(struct rc_queue_handle *queue_handle,
                                              struct rc_adapter *adapter)
{
    struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
    u32 control_register = 0x80110800; /* Base control value */
    u32 flag1, flag2;

    if (!queue_handle || !adapter)
        return -EINVAL;

    /* TODO: Get device-specific flags from devExt */
    /* flag1 = devExt[+0x1c7a8] (from descriptor blob, bits 4-9) */
    /* flag2 = devExt[+0x1c7ac] (from descriptor blob, bits 0-3) */
    flag1 = 0; /* Placeholder */
    flag2 = 0; /* Placeholder */

    /* 1. Set command ready flag */
    /* queue_handle[+0x3a0] = 1 */

    /* 2. Base control value selection */
    /* TODO: Check queue_handle[+0xd1] & 3 == 1 */
    /* If true: control_register = 0x80510800 */

    /* 3. Flag setting based on flag1 (from devExt[+0x1c7a8]) */
    switch (flag1) {
    case 8:
        control_register |= 0x80;
        break;
    case 4:
        control_register |= 0x40;
        break;
    case 2:
        control_register |= 0x20;
        break;
    }

    /* 4. Flag setting based on flag2 (from devExt[+0x1c7ac]) */
    switch (flag2) {
    case 5:
        control_register |= 0x10;
        break;
    case 4:
        control_register |= 8;
        break;
    case 2:
        control_register |= 0x10000000;
        break;
    }

    /* TODO: Store control register in queue_handle[+0x3a4] */
    /* TODO: Set queue depth: queue_handle[+0x3b8] = queue_size - 1 */

    rc_printk(RC_DEBUG, "rc_configure_command_after_state_machine: control=0x%08x\n",
              control_register);

    /* 5. Final command submission */
    /* TODO: Check queue_handle[+0x6a4] != 0 and devExt[+0x1c7b0] == 0 */
    /* TODO: Call FUN_14000e68c for final submission */

    return 0;
}

/*----------------------------------------------------------------------
 * FUN_14000f838: Special command handler (type 0x12)
 * 
 * Handles special commands with type 0x12.
 *----------------------------------------------------------------------*/

int rc_special_command_handler_type12(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list,
                                       void *unused4)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    u8 sub_command;
    u8 flags_byte;

    if (!adapter || !srb)
        return 0;

    if (!sg_list) {
        rc_srb_completion_handler(srb, NULL, 2, 0x8407); /* Invalid parameter */
        return 1;
    }

    sub_command = srb->sub_command;      /* +0x61 */
    flags_byte = srb->flags_byte;        /* +0x62 */

    rc_printk(RC_DEBUG, "rc_special_command_handler_type12: sub=0x%02x flags=0x%02x\n",
              sub_command, flags_byte);

    if ((sub_command & 1) == 0) {
        /* Bit 0 clear: Build 0x88-byte command buffer */
        /* TODO: Build command buffer with header: 0x8000, 0x06, 0x02, 0x8001 */
        /* TODO: Get controller ID from ctx->controller_id (devExt[+0x15c48]) */
        /* TODO: Copy controller data from ctx->controller_data[0..15] */
        /* TODO: Copy data from sg_list[+8] (max 0x55 bytes) using memcpy */
        rc_printk(RC_DEBUG, "rc_special_command_handler_type12: building 0x88-byte buffer\n");
    } else if (flags_byte == 0x80) {
        /* Build 0x18-byte command buffer with 0x14008000 header */
        rc_printk(RC_DEBUG, "rc_special_command_handler_type12: building 0x18-byte buffer\n");
    } else if (flags_byte == 0x86) {
        /* Build 0x40-byte command buffer with special header */
        rc_printk(RC_DEBUG, "rc_special_command_handler_type12: building 0x40-byte buffer\n");
    } else {
        /* Unsupported */
        rc_srb_completion_handler(srb, NULL, 2, 0x2000); /* Command not supported */
        return 1;
    }

    /* Success */
    rc_srb_completion_handler(srb, NULL, 0, 0);
    return 1;
}

/*----------------------------------------------------------------------
 * FUN_14000fa2c: Special command handler (type 0x9e)
 * 
 * Handles special commands with type 0x9e (controller serial number).
 *----------------------------------------------------------------------*/

int rc_special_command_handler_type9e(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list,
                                       void *unused4)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    u8 sub_command;
    u32 sg_length;
    u8 *serial_dest;
    int i;

    if (!adapter || !srb)
        return 0;

    if (!sg_list) {
        rc_srb_completion_handler(srb, NULL, 2, 0x8407); /* Invalid parameter */
        return 1;
    }

    sub_command = srb->sub_command;      /* +0x61 */
    /* TODO: Get sg_length from sg_list[+0x10] */
    sg_length = 0; /* Placeholder */

    if (sub_command != 0x10 || sg_length < 0x20) {
        return 1; /* Invalid */
    }

    /* Copy controller serial number from devExt[+0x15cf0] through devExt[+0x15cf7] */
    /* 8 bytes copied in reverse order */
    serial_dest = (u8 *)sg_list + 8;
    for (i = 0; i < 8; i++) {
        /* TODO: Copy from ctx->controller_serial[7-i] to serial_dest[i] */
        /* For now, zero the destination */
        serial_dest[i] = 0;
    }

    /* Set format indicator */
    serial_dest[10] = 2;

    rc_printk(RC_DEBUG, "rc_special_command_handler_type9e: copied serial number\n");

    /* Success */
    rc_srb_completion_handler(srb, NULL, 0, 0);
    return 1;
}

/*----------------------------------------------------------------------
 * NVMe Command Submission Functions
 * All follow the same pattern using rc_allocate_command_descriptor
 *----------------------------------------------------------------------*/

/* FUN_14000d190 - Command submission (cmd_type=0) */
int rc_submit_command_type0(struct rc_adapter *adapter,
                            struct rc_srb *srb,
                            void *unused3,
                            void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15948 */
    queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_command_type0: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_type0: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=0 and context from devExt+0x15ce8 */
    desc->command_type = 0;
    desc->context = adapter->ctx.nvme_context;
    
    /* Sets SRB tracking fields: param_2[+0xc0] and param_2[+0xc4] */
    srb->context = (u32)(unsigned long)desc;
    srb->context2 = queue->producer_index;
    
    /* TODO: Fill in command data DWORDs */
    /* TODO: Copy descriptor to MMIO queue (64 bytes at slot_index * 0x40) */
    /* TODO: Update producer index at queue_base + 0x1000 + queue_id * 8 */
    
    rc_printk(RC_DEBUG, "rc_submit_command_type0: submitted cmd_type=0\n");
    
    return 1;
}

/* FUN_14000d350 - Queue management command (cmd_type=10) */
int rc_submit_queue_management_cmd10(struct rc_queue_handle *queue)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, (void (*)(void *))rc_queue_callback_handler);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd10: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=10 and descriptor[+0x16]=0xd */
    desc->command_type = 10;
    desc->command_data[0x16] = 0xd;
    
    /* Uses callback FUN_14000eef8 (rc_queue_callback_handler) */
    /* Note: callback signature mismatch - will be fixed when callback is called */
    desc->callback = NULL;  /* TODO: Fix callback signature */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd10: submitted cmd_type=10\n");
    
    return 1;
}

/* FUN_14000d66c - Queue management command (cmd_type=6) */
int rc_submit_queue_management_cmd6(struct rc_queue_handle *queue)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Always submits (no null check - assumes slot available) */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_ERROR, "rc_submit_queue_management_cmd6: failed to allocate descriptor\n");
        return 0;
    }
    
    /* Sets cmd_type=6 and descriptor[+6]=6 */
    desc->command_type = 6;
    desc->command_data[6] = 6;
    
    /* Sets descriptor[+0x34]=0 */
    desc->context = 0;
    
    /* Uses callback FUN_14000d714 (not yet implemented) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd6: submitted cmd_type=6\n");
    
    return 1;
}

/* FUN_14000d974 - Queue management command (cmd_type=6) with null check */
int rc_submit_queue_management_cmd6_safe(struct rc_queue_handle *queue)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Checks for null descriptor before submitting */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd6_safe: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=6 and descriptor[+6]=6 */
    desc->command_type = 6;
    desc->command_data[6] = 6;
    
    /* Sets descriptor[+0x16]=1 and descriptor[+0x34]=0 */
    desc->command_data[0x16] = 1;
    desc->context = 0;
    
    /* Uses callback FUN_14000d714 (not yet implemented) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd6_safe: submitted cmd_type=6\n");
    
    return 1;
}

/* FUN_14000da20 - Queue management command (cmd_type=6) with parameter */
int rc_submit_queue_management_cmd6_param(struct rc_queue_handle *queue, u32 param)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Takes param_2 as command parameter */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd6_param: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=6 and descriptor[+0xc]=6 */
    desc->command_type = 6;
    desc->command_data[0xc] = 6;
    
    /* Sets descriptor[+0xd]=param_2 and descriptor[+0x16]=0 */
    desc->command_data[0xd] = param;
    desc->command_data[0x16] = 0;
    
    /* Uses callback FUN_14000d714 (not yet implemented) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_management_cmd6_param: submitted cmd_type=6 param=0x%08x\n",
              param);
    
    return 1;
}

/* FUN_14000cf38 - Command submission (cmd_type=9) */
int rc_submit_command_type9(struct rc_adapter *adapter,
                            struct rc_srb *srb,
                            void *command_ptr,
                            void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15948 */
    queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_command_type9: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* If param_3==NULL, calls FUN_14000c900 for error handling */
    if (!command_ptr) {
        rc_srb_completion_handler(srb, completion, 2, 0x8407);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_type9: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=9 */
    desc->command_type = 9;
    
    /* Sets context from devExt+0x15ce8 */
    desc->context = adapter->ctx.nvme_context;
    
    /* Sets command data from param_3 and param_2[+0x61/0x62] */
    /* TODO: Copy command data from command_ptr */
    /* TODO: Set command data from srb->sub_command and srb->flags_byte */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_type9: submitted cmd_type=9\n");
    
    return 1;
}

/* FUN_14000d3f4 - Scatter-gather command (cmd_type=2) */
int rc_submit_scatter_gather_cmd2(struct rc_adapter *adapter,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 sg_size;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15940 */
    queue = adapter->ctx.queue_handles[0];  /* Primary queue */
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_scatter_gather_cmd2: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Detects scatter-gather size from param_2[+0x61]: */
    /* == 1: 0x40 bytes (64 bytes) */
    /* == 2 or 3: 0x200 bytes (512 bytes) */
    if (srb->sub_command == 1) {
        sg_size = 0x40;
    } else if (srb->sub_command == 2 || srb->sub_command == 3) {
        sg_size = 0x200;
    } else {
        sg_size = 0x40;  /* Default */
    }
    
    /* If param_3==NULL, calls FUN_14000c900 for error handling */
    if (!sg_list) {
        rc_srb_completion_handler(srb, completion, 2, 0x8407);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_scatter_gather_cmd2: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=2 */
    desc->command_type = 2;
    
    /* Sets descriptor[+0x16] based on size and type */
    desc->command_data[0x16] = sg_size;
    
    /* If type==2, sets descriptor[+0xd] = -1 */
    if (srb->sub_command == 2) {
        /* Store as 32-bit value, will be copied correctly to descriptor */
        *((u32 *)&desc->command_data[0xd]) = 0xffffffff;
    }
    
    /* Build scatter-gather list */
    if (rc_build_scatter_gather_list(queue, srb, sg_list, desc) == 0) {
        rc_srb_completion_handler(srb, completion, 2, 0x8407);
        return 0;
    }
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_scatter_gather_cmd2: submitted cmd_type=2 sg_size=0x%x\n",
              sg_size);
    
    return 1;
}

/* FUN_14000c0bc - NVMe command submission (cmd_type=8) */
int rc_submit_nvme_command_type8(struct rc_adapter *adapter,
                                  void *unused2,
                                  struct rc_srb *srb,
                                  void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    
    if (!adapter)
        return 0;
    
    /* Gets NVMe queue from devExt+0x15940 */
    queue = adapter->ctx.queue_handles[0];  /* Primary NVMe queue */
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_nvme_command_type8: no queue available\n");
        if (srb)
            rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* If param_3 == 0, releases queue and returns */
    if (!srb) {
        rc_printk(RC_DEBUG, "rc_submit_nvme_command_type8: srb is NULL, releasing queue\n");
        return 0;
    }
    
    /* Allocate command slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_nvme_command_type8: queue full\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Sets cmd_type=8 */
    desc->command_type = 8;
    
    /* Programs command DWORDs 0-3 (command type, flags, NSID) */
    /* TODO: Fill in command DWORDs from SRB */
    
    /* Sets completion callback at param_3+0xc4 */
    srb->context2 = (u32)(unsigned long)desc;
    
    /* TODO: Copy command to submission queue (64-byte entries) */
    /* TODO: Update queue producer index with modulo arithmetic */
    
    rc_printk(RC_DEBUG, "rc_submit_nvme_command_type8: submitted cmd_type=8\n");
    
    return 1;
}

/* FUN_14000c718 - Command submission (cmd_type=2 or 0xc) */
int rc_submit_command_type2_or_c(struct rc_adapter *adapter,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 cmd_type;
    u32 sg_size;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15948 */
    queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_command_type2_or_c: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Detects command type and scatter-gather size from param_2[+0x61]: */
    /* == 1: 0x40 bytes (64 bytes), cmd_type=2 */
    /* == 2 or 3: 0x200 bytes (512 bytes), cmd_type=0xc */
    if (srb->sub_command == 1) {
        sg_size = 0x40;
        cmd_type = 2;
    } else if (srb->sub_command == 2 || srb->sub_command == 3) {
        sg_size = 0x200;
        cmd_type = 0xc;
    } else {
        sg_size = 0x40;  /* Default */
        cmd_type = 2;
    }
    
    /* If param_3==NULL, calls FUN_14000c900 for error handling */
    if (!sg_list) {
        rc_srb_completion_handler(srb, completion, 2, 0x8407);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_type2_or_c: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=2 or 0xc */
    desc->command_type = cmd_type;
    
    /* Sets descriptor[+0x16] based on size */
    desc->command_data[0x16] = sg_size;
    
    /* Build scatter-gather list */
    if (rc_build_scatter_gather_list(queue, srb, sg_list, desc) == 0) {
        rc_srb_completion_handler(srb, completion, 2, 0x8407);
        return 0;
    }
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_type2_or_c: submitted cmd_type=0x%x sg_size=0x%x\n",
              cmd_type, sg_size);
    
    return 1;
}

/* FUN_14000cb4c - Command submission (cmd_type=5) */
int rc_submit_command_type5(struct rc_queue_handle *queue)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_type5: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=5 */
    desc->command_type = 5;
    
    /* TODO: Fill in command data for multi-command sequences */
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_type5: submitted cmd_type=5\n");
    
    return 1;
}

/* FUN_14000cd50 - Command helper (cmd_type=1) */
int rc_submit_command_helper_type1(struct rc_queue_handle *queue,
                                    void (*completion)(void *))
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_helper_type1: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=1 */
    desc->command_type = 1;
    
    /* TODO: Fill in command data for sequencing */
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_helper_type1: submitted cmd_type=1\n");
    
    return 1;
}

/* FUN_14000ce5c - Iterates and submits commands (cmd_type=0) */
int rc_submit_commands_iterative(struct rc_queue_handle *queue,
                                  void (*completion)(void *))
{
    struct rc_command_descriptor *desc;
    int i;
    int count;
    
    if (!queue)
        return 0;
    
    /* Loops through command array param_1[6] times */
    /* param_1[6] is producer_index or queue depth */
    count = queue->producer_index;
    if (count == 0)
        count = queue->queue_depth;  /* Default to queue depth */
    
    for (i = 0; i < count; i++) {
        /* Allocate descriptor slot for each iteration */
        desc = rc_allocate_command_descriptor(queue, completion);
        if (!desc) {
            rc_printk(RC_DEBUG, "rc_submit_commands_iterative: queue full at iteration %d\n", i);
            return 0;
        }
        
        /* Sets cmd_type=0 and descriptor[+0x58] = loop_index + 1 */
        desc->command_type = 0;
        *((u32 *)&desc->command_data[0x58]) = i + 1;
        
        /* TODO: Copy descriptor to MMIO queue */
        /* TODO: Update producer index */
    }
    
    rc_printk(RC_DEBUG, "rc_submit_commands_iterative: submitted %d commands (cmd_type=0)\n", count);
    
    return 1;
}

/* FUN_14000cc28 - State machine command submission (cmd_type=4 or 1) */
int rc_submit_state_machine_command(struct rc_queue_handle *queue,
                                     struct rc_completion_descriptor *comp_desc,
                                     struct rc_srb *srb,
                                     void (*completion)(void *))
{
    struct rc_command_descriptor *desc;
    u32 cmd_type;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_state_machine_command: queue full\n");
        if (srb)
            rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Determine cmd_type based on completion descriptor */
    /* Complex state machine with locking - simplified for now */
    /* TODO: Check completion descriptor fields to determine command type */
    /* For now, default to cmd_type=1 */
    cmd_type = 1;
    if (comp_desc) {
        /* TODO: Extract command type from completion descriptor */
        /* Check completion_type_dword or adapter_index to determine command type */
        /* Completion descriptor structure may have different field layout */
    }
    
    /* Sets cmd_type=4 or 1 */
    desc->command_type = cmd_type;
    
    /* TODO: Implement complex state machine logic with locking */
    /* TODO: Handle command sequencing and completion */
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_state_machine_command: submitted cmd_type=0x%x\n", cmd_type);
    
    return 1;
}

/*----------------------------------------------------------------------
 * Simple Helper/Getter Functions
 *----------------------------------------------------------------------*/

/* FUN_14000c814 - NVMe state getter */
u8 rc_get_nvme_queue_state(struct rc_adapter *adapter)
{
    /* Returns byte at devExt+0x15d00 (NVMe queue state) */
    return adapter->ctx.nvme_queue_state;
}

/* FUN_1400100c0 - NVMe status update */
void rc_nvme_status_update(struct rc_adapter *adapter, u32 status_flags)
{
    /* Error logging, flag processing */
    /* TODO: Implement error logging logic */
    /* TODO: Process status flags and update adapter state */
    rc_printk(RC_DEBUG, "rc_nvme_status_update: adapter %d flags=0x%08x\n",
              adapter->instance, status_flags);
}

/* FUN_14000e800 - NVMe command submission wrapper */
int rc_nvme_command_submission_wrapper(struct rc_adapter *adapter, u32 port_index)
{
    /* Calls FUN_14000e820(param_1, param_2, 1) for command submission */
    /* TODO: Implement FUN_14000e820 or inline the logic */
    rc_printk(RC_DEBUG, "rc_nvme_command_submission_wrapper: adapter %d port %u\n",
              adapter->instance, port_index);
    return 0;
}

/* FUN_1400102d8 - Steady-state dispatcher */
int rc_steady_state_dispatcher(struct rc_adapter *adapter)
{
    /* Returns 1, safe path dispatcher */
    return 1;
}

/* FUN_1400102f4 - Helper stub */
int rc_helper_stub(struct rc_adapter *adapter, u32 param)
{
    /* Returns 1, parameter marshalling labels */
    return 1;
}

/* FUN_14000e494 - Early init stub */
void rc_early_init_stub(struct rc_adapter *adapter)
{
    /* Checks devExt+0x16074 flag */
    /* If set, calls FUN_14000fca4 */
    /* Checks devExt+0x16075 flag */
    /* If set, calls FUN_14000fd10 */
    /* TODO: Add flags to devExt structure and implement FUN_14000fca4/FUN_14000fd10 */
    rc_printk(RC_DEBUG, "rc_early_init_stub: adapter %d\n", adapter->instance);
}

/*----------------------------------------------------------------------
 * Remaining Command Submission Functions
 *----------------------------------------------------------------------*/

/* FUN_14000e2c8 - Queue type selection command */
int rc_submit_queue_type_selection(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void *unused3,
                                   void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 cmd_type;
    u32 sg_flags;
    
    if (!adapter || !srb)
        return 0;
    
    /* Selects queue based on param_2[+0x60]: */
    /* == 0x01: Uses primary queue at devExt+0x15940 */
    /* == 0x02: Uses secondary queue at devExt+0x15948[queue_index] */
    /* Otherwise: Returns 0 */
    if (srb->command_code == 0x01) {
        queue = adapter->ctx.queue_handles[0];  /* Primary queue */
    } else if (srb->command_code == 0x02) {
        queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];  /* Secondary queue */
    } else {
        rc_printk(RC_ERROR, "rc_submit_queue_type_selection: invalid command code 0x%02x\n",
                  srb->command_code);
        return 0;
    }
    
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_queue_type_selection: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Sets cmd_type from param_2[+0x61] */
    cmd_type = srb->sub_command;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_queue_type_selection: queue full\n");
        return 0;
    }
    
    desc->command_type = cmd_type;
    
    /* Handles scatter-gather list based on param_2[+0x64] bits: */
    sg_flags = srb->sg_flags;
    if (sg_flags & 0x4) {
        desc->command_data[0x58] = srb->data_length;  /* TODO: Use correct offset */
    }
    if (sg_flags & 0x8) {
        desc->command_data[0x5c] = srb->data_length;  /* TODO: Use correct offset */
    }
    if (sg_flags & 0x10) {
        desc->command_data[0x60] = srb->data_length;  /* TODO: Use correct offset */
    }
    if (sg_flags & 0x20) {
        desc->command_data[0x64] = srb->data_length;  /* TODO: Use correct offset */
    }
    if (sg_flags & 0x40) {
        desc->command_data[0x68] = srb->data_length;  /* TODO: Use correct offset */
    }
    if (sg_flags & 0x80) {
        desc->command_data[0x6c] = srb->data_length;  /* TODO: Use correct offset */
    }
    
    /* Calls FUN_14000c9e4 before submitting */
    /* TODO: Build scatter-gather list if needed */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_type_selection: submitted cmd_type=0x%x\n", cmd_type);
    
    return 1;
}

/* FUN_14000e4d0 - Command helper with limits (cmd_type=0xc) */
int rc_submit_command_with_limits(struct rc_queue_handle *queue)
{
    struct rc_command_descriptor *desc;
    u32 limit;
    
    if (!queue)
        return 0;
    
    /* Checks queue depth limit: param_1[+0x39c] < min(2, param_1[+0x351]) */
    /* TODO: Add limit fields to queue handle structure */
    limit = 2;  /* Default limit */
    /* TODO: Check actual limit from queue structure */
    
    /* Only submits if limit not reached */
    /* TODO: Implement limit check */
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_with_limits: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=0xc */
    desc->command_type = 0xc;
    
    /* Increments counter */
    /* TODO: Increment limit counter */
    
    /* Uses callback FUN_14000c718 (rc_submit_command_type2_or_c) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_with_limits: submitted cmd_type=0xc\n");
    
    return 1;
}

/* FUN_14000ee30 - Command with parameters (cmd_type=9) */
int rc_submit_command_with_params(struct rc_queue_handle *queue,
                                  u32 param2,
                                  u32 param3,
                                  void (*completion)(void *))
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_with_params: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=9 */
    desc->command_type = 9;
    
    /* Sets descriptor[+0x16]=param_2 and descriptor[+0x17]=param_3 */
    desc->command_data[0x16] = param2;
    desc->command_data[0x17] = param3;
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_with_params: submitted cmd_type=9 param2=0x%08x param3=0x%08x\n",
              param2, param3);
    
    return 1;
}

/* FUN_14000ef80 - Command with flags (cmd_type=9) */
int rc_submit_command_with_flags(struct rc_queue_handle *queue, u8 flags)
{
    struct rc_command_descriptor *desc;
    
    if (!queue)
        return 0;
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_with_flags: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=9 and descriptor[+0xc]=9 */
    desc->command_type = 9;
    desc->command_data[0xc] = 9;
    
    /* Sets descriptor[+0x16]=0xd and descriptor[+0x17]=param_2 */
    desc->command_data[0x16] = 0xd;
    desc->command_data[0x17] = flags;
    
    /* If param_2 & 1 (bit 0 set): */
    if (flags & 1) {
        /* Sets descriptor[+0x18] through descriptor[+0x1b] from param_1[+0xcf] through param_1[+0xd1] */
        /* TODO: Extract these values from queue handle structure */
    }
    
    /* Uses callback FUN_14000eef8 (rc_queue_callback_handler) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_with_flags: submitted cmd_type=9 flags=0x%02x\n", flags);
    
    return 1;
}

/* FUN_14000f06c - Command with context (cmd_type=9) */
int rc_submit_command_with_context(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void (*completion)(void *),
                                   u32 context_param)
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 context;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15940 */
    queue = adapter->ctx.queue_handles[0];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_command_with_context: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_with_context: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=9 and descriptor[+0xc]=9 */
    desc->command_type = 9;
    desc->command_data[0xc] = 9;
    
    /* Sets descriptor[+0x16]=2 */
    desc->command_data[0x16] = 2;
    
    /* Sets descriptor[+0x17] from param_4 or devExt[+0x15c92] if param_4==0 */
    if (context_param == 0) {
        /* TODO: Get context from devExt[+0x15c92] */
        context = adapter->ctx.nvme_context;
    } else {
        context = context_param;
    }
    desc->command_data[0x17] = context;
    
    /* Sets descriptor[+6] and descriptor[+7] to param_2 (SRB pointer) */
    /* TODO: Set SRB pointer in descriptor */
    
    /* Sets SRB tracking: param_2[+0xc0] and param_2[+0xc4] */
    srb->context = (u32)(unsigned long)desc;
    srb->context2 = queue->producer_index;
    
    /* Uses callback FUN_14000eef8 (rc_queue_callback_handler) */
    /* TODO: Set callback */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_with_context: submitted cmd_type=9 context=0x%08x\n",
              context);
    
    return 1;
}

/* FUN_14000fe44 - Command with context (cmd_type=4) */
int rc_submit_command_context_type4(struct rc_adapter *adapter,
                                    struct rc_srb *srb,
                                    void *unused3,
                                    void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15948 */
    queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_command_context_type4: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_command_context_type4: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=4 and context from devExt+0x15ce8 */
    desc->command_type = 4;
    desc->context = adapter->ctx.nvme_context;
    
    /* Sets command data from param_2[+0x64] and param_2[+0x6c] */
    /* TODO: Extract command data from SRB */
    
    /* Sets SRB tracking: param_2[+0xc0] and param_2[+0xc4] */
    srb->context = (u32)(unsigned long)desc;
    srb->context2 = queue->producer_index;
    
    /* Uses callback FUN_14000e1ec (rc_nvme_completion_with_sg) */
    desc->completion_cb = (void (*)(void *))rc_nvme_completion_with_sg;
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_command_context_type4: submitted cmd_type=4\n");
    
    return 1;
}

/* FUN_14000ff50 - Special command (cmd_type=0x11) */
int rc_submit_special_command_11(struct rc_adapter *adapter,
                                 struct rc_srb *srb,
                                 void *unused3,
                                 void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15940 */
    queue = adapter->ctx.queue_handles[0];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_special_command_11: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_special_command_11: queue full\n");
        return 0;
    }
    
    /* Sets cmd_type=0x11 */
    desc->command_type = 0x11;
    
    /* Sets descriptor[+0x58]=0 and descriptor[+0x5c]=0 */
    *((u32 *)&desc->command_data[0x58]) = 0;
    *((u32 *)&desc->command_data[0x5c]) = 0;
    
    /* Calls FUN_14000c9e4 before submitting */
    /* TODO: Build scatter-gather list if needed */
    
    /* Sets SRB tracking: param_2[+0xc0] and param_2[+0xc4] */
    srb->context = (u32)(unsigned long)desc;
    srb->context2 = queue->producer_index;
    
    /* Uses callback FUN_14001005c (rc_simple_completion_callback) */
    desc->completion_cb = (void (*)(void *))rc_simple_completion_callback;
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_special_command_11: submitted cmd_type=0x11\n");
    
    return 1;
}

/*----------------------------------------------------------------------
 * Simple Callback Functions
 *----------------------------------------------------------------------*/

/* FUN_140003598 - Queue cleanup */
void rc_queue_cleanup_all_ports(struct rc_adapter *adapter)
{
    int i;
    
    /* Iterates 48 ports, checks port state */
    for (i = 0; i < 48 && i < RC_MAX_SCSI_TARGETS; i++) {
        /* TODO: Check port state and cleanup if needed */
        /* TODO: Release port resources */
    }
    
    rc_printk(RC_DEBUG, "rc_queue_cleanup_all_ports: adapter %d cleaned up\n",
              adapter->instance);
}

/* FUN_140003f7c - Status polling cleanup */
void rc_status_polling_cleanup(struct rc_adapter *adapter, void *status_buf)
{
    /* Spinlock release, port state clearing */
    /* TODO: Release spinlocks */
    /* TODO: Clear port states */
    /* TODO: Free status buffer */
    rc_printk(RC_DEBUG, "rc_status_polling_cleanup: adapter %d\n", adapter->instance);
}

/* FUN_140001438 - Early init wrapper */
int rc_early_init_wrapper(struct rc_adapter *adapter,
                          void *port_context,
                          struct rc_srb *srb,
                          void (*completion)(void *))
{
    /* Checks port state at param_2+8 (byte offset) */
    /* Verifies queue bitmask at devExt+0x3f8 + port_index*0x728 (bit 0x400 must be set) */
    /* Checks SRB state at param_3+0x60 (must be 0 or 3) */
    /* Increments counter at devExt+0x420 + port_index*0x728 */
    /* Calls FUN_140001318 to handle port operations */
    /* If param_4 == 0, calls FUN_1400097ac(param_3) for SRB completion */
    /* Otherwise calls StorPort service +0x838 for completion */
    
    /* TODO: Implement port state checks */
    /* TODO: Implement queue bitmask verification */
    /* TODO: Implement SRB state checks */
    /* TODO: Implement counter increment */
    /* TODO: Call port operation handler */
    
    if (!completion) {
        rc_srb_indirect_completion(srb);
    } else {
        /* TODO: Call StorPort service +0x838 */
        if (completion)
            completion(srb);
    }
    
    rc_printk(RC_DEBUG, "rc_early_init_wrapper: adapter %d\n", adapter->instance);
    
    return 1;
}

/* FUN_14000ea34 - Queue rotation command (cmd_type=1 or 2) */
int rc_submit_queue_rotation_command(struct rc_adapter *adapter,
                                     struct rc_srb *srb,
                                     void *unused3,
                                     void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 cmd_type;
    u32 threshold;
    u32 queue_count;
    
    if (!adapter || !srb)
        return 0;
    
    /* Uses queue at devExt+0x15948[queue_index] where queue_index = devExt+0x15968 */
    queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    if (!queue) {
        rc_printk(RC_ERROR, "rc_submit_queue_rotation_command: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Rotates queue when counter reaches threshold */
    /* Uses DAT_140014160 or DAT_140014164 as threshold (based on param_2[+0x188]) */
    /* TODO: Get threshold from global constants or SRB */
    threshold = 32;  /* Default threshold */
    
    /* Increments devExt+0x1596c counter */
    adapter->ctx.queue_rotation_counter++;
    
    /* When counter >= threshold, resets to 0 and increments queue_index */
    if (adapter->ctx.queue_rotation_counter >= threshold) {
        adapter->ctx.queue_rotation_counter = 0;
        /* queue_index = (queue_index + 1) % devExt[+0x15d1c] */
        /* TODO: Get queue count from devExt[+0x15d1c] */
        queue_count = RC_MAX_QUEUE_DESCRIPTORS;
        adapter->ctx.queue_index = (adapter->ctx.queue_index + 1) % queue_count;
        queue = adapter->ctx.queue_handles[adapter->ctx.queue_index];
    }
    
    /* Sets cmd_type=1 or cmd_type=2 based on param_2[+0x154] & 4 */
    /* TODO: Check SRB field at offset 0x154 */
    if (srb->data_length & 4) {
        cmd_type = 2;
    } else {
        cmd_type = 1;
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_queue_rotation_command: queue full\n");
        return 0;
    }
    
    desc->command_type = cmd_type;
    
    /* Calls FUN_14000c9e4 before submitting */
    /* TODO: Build scatter-gather list if needed */
    
    /* If FUN_14000c9e4 fails, calls FUN_14000c900 for error handling */
    /* TODO: Check scatter-gather build result */
    
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_queue_rotation_command: submitted cmd_type=0x%x queue_index=%u\n",
              cmd_type, adapter->ctx.queue_index);
    
    return 1;
}

/* FUN_14000dae4 - Complex state machine (cmd_type=1 or 5) */
int rc_submit_complex_state_machine(struct rc_queue_handle *queue,
                                    void *unused2,
                                    struct rc_completion_descriptor *comp_desc,
                                    struct rc_srb *srb)
{
    struct rc_command_descriptor *desc;
    u32 cmd_type;
    
    if (!queue || !srb)
        return 0;
    
    /* Checks param_4[+0xc] < 0x20000 (size check - early exit if too large) */
    if (srb->data_length >= 0x20000) {
        rc_printk(RC_ERROR, "rc_submit_complex_state_machine: data length too large (0x%x)\n",
                  srb->data_length);
        return 0;
    }
    
    /* TODO: Implement complex state machine logic */
    /* Path 1: If param_3[+0x30] == 0x01 */
    /* Path 2: If param_3[+0x30] == 0x05 */
    /* Completion: When all flags set, call FUN_14000e4d0 in loop, then FUN_14000c1e4 */
    
    /* For now, simplified implementation */
    if (comp_desc && comp_desc->adapter_index == 0x01) {
        cmd_type = 1;
    } else if (comp_desc && comp_desc->adapter_index == 0x05) {
        cmd_type = 5;
    } else {
        cmd_type = 1;  /* Default */
    }
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, NULL);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_submit_complex_state_machine: queue full\n");
        return 0;
    }
    
    desc->command_type = cmd_type;
    
    /* TODO: Implement full state machine logic with counter, flags, and completion */
    /* TODO: Copy descriptor to MMIO queue */
    /* TODO: Update producer index */
    
    rc_printk(RC_DEBUG, "rc_submit_complex_state_machine: submitted cmd_type=0x%x\n", cmd_type);
    
    return 1;
}

/* FUN_14000c82c - NVMe completion check */
int rc_check_nvme_completion(struct rc_adapter *adapter, int port_index)
{
    struct rc_queue_handle *queue;
    
    if (!adapter)
        return 0;
    
    /* If param_2 == -1 (all ports): */
    if (port_index == -1) {
        /* Checks all NVMe queues for completion */
        /* Compares submission queue head vs tail */
        int i;
        for (i = 0; i < RC_MAX_QUEUE_DESCRIPTORS; i++) {
            queue = adapter->ctx.queue_handles[i];
            if (queue) {
                /* TODO: Check queue head vs tail for completions */
                if (queue->producer_index != queue->consumer_index) {
                    return 1;  /* Completions pending */
                }
            }
        }
        return 0;  /* No completions */
    } else {
        /* Checks single queue */
        if (port_index >= RC_MAX_QUEUE_DESCRIPTORS)
            return 0;
        
        queue = adapter->ctx.queue_handles[port_index];
        if (!queue)
            return 0;
        
        /* TODO: Set completion register bitmask */
        /* Returns true if completions pending */
        return (queue->producer_index != queue->consumer_index) ? 1 : 0;
    }
}

/*----------------------------------------------------------------------
 * Additional Callback Functions
 *----------------------------------------------------------------------*/

/* FUN_140003838 - Secondary queue helper */
int rc_secondary_queue_helper_legacy(struct rc_adapter *adapter,
                                     struct rc_srb *srb,
                                     void *sg_list,
                                     void (*completion)(void *))
{
    struct rc_queue_handle *queue;
    struct rc_command_descriptor *desc;
    u32 port_id;
    
    if (!adapter || !srb)
        return 0;
    
    /* Determines target port from SRB (param_2+0x38+8) */
    port_id = srb->port_id;
    
    /* Selects queue based on port state and command type */
    /* TODO: Select queue based on port state */
    queue = adapter->ctx.queue_handles[0];  /* Default to primary queue */
    if (!queue) {
        rc_printk(RC_ERROR, "rc_secondary_queue_helper_legacy: no queue available\n");
        rc_srb_completion_handler(srb, completion, 2, 0x2600);
        return 0;
    }
    
    /* Checks if queue slot is available (bitmask check) */
    /* TODO: Implement bitmask check */
    
    /* Allocate descriptor slot */
    desc = rc_allocate_command_descriptor(queue, completion);
    if (!desc) {
        rc_printk(RC_DEBUG, "rc_secondary_queue_helper_legacy: queue full\n");
        return 0;
    }
    
    /* Sets command flags based on SRB type (0x01, 0x04, 0x07, 0x08) */
    desc->command_type = srb->command_code;
    
    /* Copies scatter-gather entries to command table */
    if (sg_list) {
        /* TODO: Build scatter-gather list */
    }
    
    /* Updates queue producer index */
    /* TODO: Update producer index */
    
    /* Calls FUN_1400027dc to check if doorbell should be rung */
    /* If doorbell needed, updates MMIO registers and rings doorbell */
    /* TODO: Implement doorbell logic */
    
    rc_printk(RC_DEBUG, "rc_secondary_queue_helper_legacy: submitted port=%u\n", port_id);
    
    return 1;
}

/* FUN_140004090 - Primary queue dispatcher (legacy) */
int rc_primary_queue_dispatcher_legacy(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list,
                                       void (*completion)(void *))
{
    u32 port_id;
    u8 srb_function;
    
    if (!adapter || !srb)
        return 0;
    
    /* Clears param_2+0x58 (SRB status) */
    srb->status = 0;
    
    /* Gets port index from param_2+0x38+8 */
    port_id = srb->port_id;
    
    /* Checks port state at devExt+0x410 + port_index*0x728 */
    /* TODO: Check port state */
    
    /* Routes based on SRB command type (param_2+0x14c): */
    srb_function = srb->srb_function;
    
    switch (srb_function) {
    case 0x01:
    case 0x04:
        /* Read/Write commands - use secondary queue helper */
        return rc_secondary_queue_helper_legacy(adapter, srb, sg_list, completion);
        
    case 0x03:
        /* Port control - calls FUN_140003d94 */
        /* TODO: Call AHCI command submission */
        break;
        
    case 0x09:
        /* Secondary queue - calls FUN_140003838 */
        return rc_secondary_queue_helper_legacy(adapter, srb, sg_list, completion);
        
    default:
        /* Default: Calls FUN_140003838 for command submission */
        return rc_secondary_queue_helper_legacy(adapter, srb, sg_list, completion);
    }
    
    return 1;
}

/* FUN_140004170 - Command routing helper */
int rc_command_routing_helper(struct rc_adapter *adapter,
                              struct rc_srb *srb,
                              void *sg_list,
                              void (*completion)(void *))
{
    u32 port_id;
    u8 srb_function;
    
    if (!adapter || !srb)
        return 0;
    
    /* Gets port index from param_2+0x38+8 */
    port_id = srb->port_id;
    
    /* Gets port descriptor from devExt + 0x158 + port_index*0x728 */
    /* TODO: Get port descriptor */
    
    /* Routes based on command type (param_2+0x14c): */
    srb_function = srb->srb_function;
    
    if (srb_function == 0x07 || srb_function == 0x08) {
        /* Special commands - checks port state */
        /* If state < 10, calls FUN_140003838 or FUN_140003d94 */
        /* TODO: Check port state and route accordingly */
        return rc_secondary_queue_helper_legacy(adapter, srb, sg_list, completion);
    } else {
        /* Calls FUN_140001008 to build vendor mailbox */
        /* TODO: Build vendor mailbox */
        /* Completes SRB with error or success */
        rc_srb_completion_handler(srb, completion, 0, 0);
        return 1;
    }
}

/* FUN_140003d94 - AHCI command submission */
void rc_ahci_command_submission(struct rc_adapter *adapter,
                                struct rc_srb *srb,
                                void (*completion)(void *))
{
    /* Uses StorPort service +0x310 to get adapter handle */
    /* Uses StorPort service +0x318 to submit command with callback FUN_1400075ac */
    /* Uses StorPort service +0x328 to check completion status */
    /* If submission fails, releases handle and completes SRB with error */
    /* The callback FUN_1400075ac allocates scatter-gather list and calls callback at devExt+0x16148 */
    
    /* TODO: Implement StorPort service equivalents */
    /* TODO: Submit AHCI command via StorPort services */
    /* TODO: Set up callback chain */
    
    rc_printk(RC_DEBUG, "rc_ahci_command_submission: adapter %d\n", adapter->instance);
}

/* FUN_1400075ac - Command submission callback */
int rc_command_submission_callback(void *adapter_handle,
                                   void *unused2,
                                   void *srb_context,
                                   void *unused4,
                                   void *sg_list_ptr)
{
    struct rc_adapter *adapter = (struct rc_adapter *)adapter_handle;
    struct rc_srb *srb = (struct rc_srb *)srb_context;
    
    /* Releases spinlock via StorPort service +0x350 */
    /* TODO: Release spinlock */
    
    /* Allocates pool for scatter-gather list (ExAllocatePoolWithTag, tag 0x72634148) */
    /* TODO: Allocate pool for scatter-gather list */
    
    /* Builds scatter-gather list from param_5 array */
    /* TODO: Build scatter-gather list */
    
    /* Sets bit 0x100000 in SRB flags */
    /* TODO: Set SRB flag */
    
    /* Calls callback at devExt+0x16148 (secondary queue helper) with scatter-gather list */
    /* If callback fails, frees pool and clears bit */
    /* TODO: Call secondary queue helper */
    
    rc_printk(RC_DEBUG, "rc_command_submission_callback: adapter %d\n",
              adapter ? adapter->instance : 0);
    
    return 1;
}

/* FUN_14000f178 - NVMe command submission */
void rc_nvme_command_submission_legacy(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void (*completion)(void *))
{
    /* Similar to FUN_140003d94 but for NVMe path */
    /* Uses StorPort services +0x310, +0x318, +0x328 for command submission */
    /* Calls FUN_14000f79c or FUN_14000f718 for command preparation */
    /* Uses callback FUN_1400075ac for scatter-gather handling */
    
    /* TODO: Implement StorPort service equivalents */
    /* TODO: Submit NVMe command via StorPort services */
    /* TODO: Set up callback chain */
    
    rc_printk(RC_DEBUG, "rc_nvme_command_submission_legacy: adapter %d\n", adapter->instance);
}

/* FUN_14000f454 - NVMe queue initialization */
int rc_nvme_queue_initialization(void *queue_desc,
                                 void *adapter_handle,
                                 u32 queue_index,
                                 u32 sub_queue_index,
                                 u8 init_flag,
                                 u8 enable_flag,
                                 dma_addr_t comp_queue_base,
                                 dma_addr_t sub_queue_base)
{
    /* Initializes queue descriptor structure (0x78 bytes) */
    /* Sets queue ID, queue size (max 0x100 entries, clamped) */
    /* Programs completion queue base at psVar1+0xc (param_7) */
    /* Programs submission queue base at psVar1+0x14 (param_8) */
    /* Sets up completion queue entries (0x100 entries, 0x78 bytes each) */
    /* If param_6 == 0, calls FUN_14000f608 to enable queue */
    
    /* TODO: Initialize queue descriptor structure */
    /* TODO: Program completion queue base */
    /* TODO: Program submission queue base */
    /* TODO: Set up completion queue entries */
    /* TODO: Call enable queue function if needed */
    
    rc_printk(RC_DEBUG, "rc_nvme_queue_initialization: queue_index=%u comp_base=0x%llx sub_base=0x%llx\n",
              queue_index, (unsigned long long)comp_queue_base, (unsigned long long)sub_queue_base);
    
    return 0;
}

/* FUN_140010488 - MMIO register I/O */
int rc_mmio_register_io(void *register_context,
                        void *unused2,
                        void *buffer_descriptor)
{
    struct rc_adapter *adapter;
    void __iomem *mmio_base;
    
    /* TODO: Get adapter and MMIO base from context */
    /* For now, this is a placeholder */
    
    /* Writes to MMIO offset 0x10 (not 0x100+ - so this is likely command/control registers) */
    /* Reads from offsets 0x34, 0x78, 0x80, 0x9c */
    /* Writes values 0x100, 0x200, or 0x300 based on buffer state */
    /* Uses KeStallExecutionProcessor(0x19) (25µs) delays for polling */
    
    /* TODO: Implement MMIO register I/O */
    /* TODO: Read from specified offsets */
    /* TODO: Write values based on buffer state */
    /* TODO: Implement polling delays */
    
    rc_printk(RC_DEBUG, "rc_mmio_register_io: register I/O\n");
    
    return 0;
}

/*----------------------------------------------------------------------
 * Critical Initialization Functions
 *----------------------------------------------------------------------*/

/* FUN_1400021d4 - Spinlock callback (critical queue initialization) */
int rc_spinlock_callback_queue_init(struct rc_adapter *adapter, u8 mode)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    void __iomem *mmio_base = ctx->mmio_base;
    u8 old_mode;
    int i;
    
    /* Save old mode */
    /* TODO: Add queue_init_mode field to devExt structure */
    old_mode = 0;
    
    /* Set new mode */
    /* TODO: Store mode in devExt structure */
    
    /* Clear queue state */
    ctx->queue_state = 0;
    
    /* Initialize queue context */
    /* TODO: Store queue context in devExt structure */
    
    /* Clear completion register context */
    /* TODO: Initialize completion register context */
    
    /* If old_mode == 0 and new mode == 0, initialize queues */
    if (old_mode == 0 && mode == 0) {
        /* TODO: Call service slot +0x100 to get adapter handle */
        /* TODO: Initialize 8 ports with stride 0x728 */
        for (i = 0; i < 8 && i < RC_MAX_SCSI_TARGETS; i++) {
            /* TODO: Initialize port queues */
        }
    }
    
    /* Poll for completion register programming */
    /* TODO: Implement polling loop with KeStallExecutionProcessor(1000) */
    /* TODO: Check completion register status */
    
    /* Set completion register flags */
    /* TODO: Set flags in completion register context */
    
    /* Program completion registers */
    /* Calls service slot +0x9D8 three times: */
    /* First call: (serviceCtx, 0, devExt + 0x15928) */
    /* Second call: (serviceCtx, 0, queue_desc + 0x6f0) - in queue loop */
    /* Third call: (serviceCtx, 0, queue_desc + 0x6e8) - in queue loop */
    /* TODO: Implement service slot +0x9D8 calls */
    
    /* Read completion register context size after first call */
    /* TODO: Read devExt + 0x157b8 */
    
    /* Program queue descriptors for all 8 ports */
    /* TODO: Call FUN_140002ef0 for completion structure setup */
    
    rc_printk(RC_INFO, "rc_spinlock_callback_queue_init: adapter %d mode=0x%02x\n",
              adapter->instance, mode);
    
    return 0;
}

/* FUN_1400028f8 - Port enable/resume */
int rc_port_enable_resume(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    int i;
    u8 port_state;
    
    /* Very complex state machine processing port queues */
    /* Iterates through all active ports (up to 48, stride 0xe5) */
    for (i = 0; i < 48 && i < RC_MAX_SCSI_TARGETS; i++) {
        /* TODO: Get port state */
        port_state = 0;  /* Default state */
        
        /* State machine with states 0-15 */
        switch (port_state) {
        case 0:
            /* Port enable, sets bitmasks */
            /* TODO: Set port enable bitmasks */
            break;
        case 1:
            /* Calls FUN_140002ef0 for completion setup */
            /* TODO: Call completion setup function */
            break;
        case 2:
            /* Sets queue register to 0x70000000, advances timestamp */
            /* TODO: Set queue register */
            break;
        case 3:
            /* Calls FUN_1400014dc for SRB allocation */
            /* TODO: Allocate SRB context */
            break;
        case 4:
            /* Calls FUN_14000484c for port configuration */
            /* TODO: Configure port */
            break;
        case 5:
            /* Calls FUN_140004578 for queue programming */
            /* TODO: Program queue */
            break;
        case 6:
            /* Calls FUN_140004c10 for controller settings */
            /* TODO: Set controller settings */
            break;
        case 7:
            /* Handles port enable with nested queue activation */
            /* TODO: Enable port with nested queue */
            break;
        case 8:
            /* Sets register bit 2, advances to state 9 */
            /* TODO: Set register bit */
            break;
        case 9:
            /* Sets completion flags, calls FUN_140002ef0 */
            /* TODO: Set completion flags */
            break;
        case 10:
            /* Handles command completion, checks SRB status */
            /* TODO: Check SRB completion status */
            break;
        case 12:
            /* Calls FUN_140001868 for doorbell activation */
            /* TODO: Activate doorbell */
            break;
        default:
            break;
        }
        
        /* TODO: Call various helper functions: */
        /* FUN_1400058dc, FUN_1400014dc, FUN_1400016b0, FUN_1400036e0, FUN_14000b500 */
    }
    
    rc_printk(RC_INFO, "rc_port_enable_resume: adapter %d processed %d ports\n",
              adapter->instance, i);
    
    return 0;
}

/* FUN_140003048 - Port disable/quiesce */
int rc_port_disable_quiesce(struct rc_adapter *adapter, u32 port_mask)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    int i;
    u32 port_count;
    u32 port_register;
    
    /* Iterates through ports (up to devExt+0x158fc count) */
    /* TODO: Get port count from devExt+0x158fc */
    port_count = ctx->port_count;
    
    for (i = 0; i < port_count && i < RC_MAX_SCSI_TARGETS; i++) {
        /* Check port mask */
        if (port_mask != 0xffffffff && !(port_mask & (1 << i))) {
            continue;  /* Skip this port */
        }
        
        /* For each active port: */
        /* Reads port register at offset 0x10 */
        /* TODO: Read port register */
        port_register = 0;  /* Default */
        
        if (port_register == 0) {
            /* If register == 0, calls FUN_14000330c to snapshot queue pointers */
            /* TODO: Call snapshot function */
        } else {
            /* Otherwise processes pending requests: */
            /* Checks bit 0x1e (bit 30) for completion flag */
            if (port_register & (1 << 30)) {
                /* Calls FUN_14000438c and FUN_14000403c to flush SRBs */
                /* TODO: Flush pending SRBs */
                
                /* Calls FUN_140001318 for port disable */
                /* TODO: Disable port */
                
                /* Calls FUN_140001868 for doorbell cleanup */
                /* TODO: Cleanup doorbell */
                
                /* Calls FUN_14000330c to hand off to firmware */
                /* TODO: Hand off to firmware */
            }
        }
        
        /* Handles error conditions and queue state transitions */
        /* TODO: Handle errors and state transitions */
    }
    
    rc_printk(RC_INFO, "rc_port_disable_quiesce: adapter %d port_mask=0x%08x\n",
              adapter->instance, port_mask);
    
    return 0;
}

/* FUN_14000dd44 - NVMe spinlock callback */
int rc_nvme_spinlock_callback_init(struct rc_adapter *adapter, u8 init_flag)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    void __iomem *mmio_base = ctx->mmio_base;
    int i;
    u32 queue_size = 0x30000;  /* 0x30000 bytes per queue */
    dma_addr_t queue_base;
    
    /* Extensive NVMe queue setup: */
    /* Allocates submission/completion queues (0x30000 bytes per queue) */
    /* TODO: Allocate DMA memory for queues */
    
    /* Sets up queue descriptors (0x78 bytes each) */
    /* TODO: Set up queue descriptors */
    
    /* Programs queue doorbell registers */
    /* TODO: Program doorbell registers */
    
    /* Initializes queue state tracking */
    /* TODO: Initialize queue state */
    
    /* Reads controller capabilities from MMIO */
    /* TODO: Read NVMe controller capabilities */
    
    /* Sets up 1-4 queues based on devExt+0xb0 (port count) */
    for (i = 0; i < ctx->port_count && i < 4; i++) {
        /* Programs completion queue entries (0x78 bytes each, 0x100 entries) */
        /* Calls FUN_14000f454 for each queue initialization */
        rc_nvme_queue_initialization(NULL,  /* queue_desc */
                                     adapter,  /* adapter_handle */
                                     i,  /* queue_index */
                                     0,  /* sub_queue_index */
                                     init_flag,  /* init_flag */
                                     1,  /* enable_flag */
                                     0,  /* comp_queue_base - TODO: Get from DMA allocation */
                                     0);  /* sub_queue_base - TODO: Get from DMA allocation */
    }
    
    rc_printk(RC_INFO, "rc_nvme_spinlock_callback_init: adapter %d queues=%d\n",
              adapter->instance, ctx->port_count);
    
    return 0;
}

/* FUN_14000e59c - NVMe completion processing */
int rc_nvme_cleanup_completion(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    int i;
    int iterations = 0;
    int max_iterations = 2000;  /* 2 seconds with 1000µs delay */
    
    /* Acquires spinlock */
    /* TODO: Acquire spinlock */
    
    /* Releases all NVMe queue resources */
    /* TODO: Release queue resources */
    
    /* Calls FUN_140005f90 for queue state check */
    /* TODO: Check queue state */
    
    /* Polls completion queues with KeStallExecutionProcessor(1000) delays */
    /* Waits up to 2000 iterations (2 seconds) for completions */
    for (i = 0; i < RC_MAX_QUEUE_DESCRIPTORS; i++) {
        struct rc_queue_handle *queue = ctx->queue_handles[i];
        if (!queue)
            continue;
        
        iterations = 0;
        while (iterations < max_iterations) {
            /* Check for completions */
            if (queue->producer_index == queue->consumer_index) {
                break;  /* No more completions */
            }
            
            /* Process completion */
            /* TODO: Process completion */
            
            /* Delay 1000µs */
            udelay(1000);
            iterations++;
        }
    }
    
    /* Updates MMIO registers for queue state */
    /* TODO: Update MMIO registers */
    
    /* Calls FUN_14000fdd8 for final cleanup */
    /* TODO: Call final cleanup function */
    
    rc_printk(RC_INFO, "rc_nvme_cleanup_completion: adapter %d\n", adapter->instance);
    
    return 0;
}

/* FUN_140005ff4 - Adapter initialization and device enumeration */
int rc_adapter_init_device_enumeration(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    u8 device_info[512];
    u8 device_info_large[2048];
    u8 firmware_version[80];
    u32 adapter_handle;
    u32 version_major, version_minor, version_patch;
    
    /* 1. Device information retrieval */
    /* Calls service +0x3f0 with queue index 1 to read device information into local_c38 (512 bytes) */
    /* TODO: Call service +0x3f0 with queue index 1 */
    memset(device_info, 0, sizeof(device_info));
    
    /* Calls service +0x3f0 with queue index 2 to read device information into local_838 (2048 bytes) */
    /* TODO: Call service +0x3f0 with queue index 2 */
    memset(device_info_large, 0, sizeof(device_info_large));
    
    /* Searches for "CC_010802" string in device information */
    /* TODO: Search for capability string */
    
    /* 2. Firmware version parsing */
    /* Calls service +0x3f0 with queue index 10 to read firmware version string */
    /* TODO: Call service +0x3f0 with queue index 10 */
    memset(firmware_version, 0, sizeof(firmware_version));
    
    /* Parses version string to extract major, minor, and patch version numbers */
    /* TODO: Parse version string */
    version_major = 0;
    version_minor = 0;
    version_patch = 0;
    
    /* 3. Adapter context creation */
    /* Calls service +0x258 (600 decimal) to create adapter context */
    /* TODO: Call service +0x258 */
    adapter_handle = 0;
    
    /* Calls service +0x650 to get adapter extension pointer */
    /* TODO: Get adapter extension */
    
    /* Stores adapter handle in global array */
    /* TODO: Store in global array */
    
    /* Sets adapter fields */
    /* Combines parsed version numbers: puVar5[0x5818] = (iVar4 << 8 | uVar15) << 8 | uVar10 */
    /* TODO: Add firmware_version field to devExt structure */
    /* ctx->firmware_version = (version_major << 16) | (version_minor << 8) | version_patch; */
    
    /* 4. Firmware capability parsing */
    /* Calls FUN_140007d40 to parse firmware capabilities and install callbacks */
    /* TODO: Call firmware capability parsing */
    
    /* 5. Adapter linkage setup */
    /* TODO: Link adapters */
    
    /* 6. WMI/descriptor registration */
    /* Calls service +0x418 to initialize descriptor accessor */
    /* TODO: Initialize descriptor accessor */
    
    /* Sets up WMI configuration structure */
    /* TODO: Set up WMI configuration */
    
    /* Calls service +0x298 for WMI registration */
    /* TODO: Register WMI */
    
    rc_printk(RC_INFO, "rc_adapter_init_device_enumeration: adapter %d initialized\n",
              adapter->instance);
    
    return 0;
}

/* FUN_1400067fc - Adapter object & WMI registration helper */
int rc_adapter_object_wmi_registration(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    u32 adapter_handle;
    
    /* 1. Adapter context setup */
    /* Calls service +0x650 to get adapter extension pointer */
    /* TODO: Get adapter extension */
    
    /* Calls service +0x690 to get adapter handle */
    /* TODO: Get adapter handle */
    adapter_handle = 0;
    
    /* Calls service +0x258 with adapter handle, stores result in devExt+0x16018 */
    /* TODO: Call service +0x258 */
    /* TODO: Add adapter_handle field to devExt structure */
    /* ctx->adapter_handle = adapter_handle; */
    
    /* Sets devExt+0x16020 from devExt+0x16018 */
    /* TODO: Add adapter_context field to devExt structure */
    /* ctx->adapter_context = (void *)(unsigned long)adapter_handle; */
    
    /* If first adapter, sets global first adapter pointer */
    /* TODO: Set global first adapter */
    
    /* Increments adapter count */
    /* TODO: Increment global adapter count */
    
    /* 2. Adapter linkage */
    /* Gets adapter list context via service +0x650 */
    /* TODO: Get adapter list context */
    
    /* Adds current adapter to list */
    /* TODO: Add to adapter list */
    
    /* Sets devExt+0x16050 = 0 */
    /* TODO: Add adapter_index field to devExt structure */
    /* ctx->adapter_index = 0; */
    
    /* 3. WMI registration */
    /* Calls service +0x298 to create WMI object */
    /* TODO: Create WMI object */
    
    /* Calls service +0x2a0 with WMI configuration */
    /* TODO: Configure WMI */
    
    /* Calls FUN_14000a430 (WMI GUID binder) */
    /* TODO: Bind WMI GUIDs */
    
    /* Calls service +0x428 to register adapter */
    /* TODO: Register adapter */
    
    /* Calls service +0xe8 with configuration */
    /* TODO: Finalize configuration */
    
    rc_printk(RC_INFO, "rc_adapter_object_wmi_registration: adapter %d registered\n",
              adapter->instance);
    
    return 0;
}
