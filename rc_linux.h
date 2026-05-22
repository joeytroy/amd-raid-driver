/****************************************************************************
 * AMD RAID Driver for Linux - Main Header
 * Based on Windows driver architecture (rcbottom, rccfg, rcraid)
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#ifndef _RC_LINUX_H_
#define _RC_LINUX_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
// Try to include SCSI headers with fallback
#if __has_include(<linux/scsi/scsi.h>)
#include <linux/scsi/scsi.h>
#include <linux/scsi/scsi_host.h>
#include <linux/scsi/scsi_device.h>
#define RC_SCSI_AVAILABLE 1
#else
// SCSI headers not available - define minimal fallbacks
#define RC_SCSI_AVAILABLE 0
struct Scsi_Host;
struct scsi_device;
struct scsi_cmnd;
struct scsi_host_template;
#define SCSI_SCAN_INITIAL 1
#define DID_OK 0x00
// Define missing SCSI functions as stubs
static inline int scsi_remove_host(struct Scsi_Host *host) { return 0; }
static inline void scsi_host_put(struct Scsi_Host *host) { }
static inline struct Scsi_Host *scsi_host_alloc(struct scsi_host_template *sht, int privsize) { return NULL; }
static inline int scsi_add_host(struct Scsi_Host *host, struct device *dev) { return 0; }

// gendisk functions are available through linux/blkdev.h
#endif
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/msi.h>
#include <linux/acpi.h>
#include <linux/proc_fs.h>
#include <linux/sysctl.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "rc_pci_ids.h"

// Driver Information
#define RC_DRIVER_NAME               "rcraid"
#define RC_DRIVER_VERSION            "9.3.2.00255"
#define RC_DRIVER_DESCRIPTION        "AMD RAID Controller for Linux"
#define RC_DRIVER_BUILD              "9.3.2-00039"

// Maximum adapters and targets (AMD Official Specifications)
#define RC_MAX_ADAPTERS              11  // Max Controller Count per AMD spec
#define RC_MAX_SCSI_TARGETS          32
#define RC_MAX_SCSI_LUNS             8
#define RC_MAX_DEVICES               14  // Total devices (SATA + NVMe)
#define RC_MAX_NVME_DEVICES          10  // Max NVMe devices per AMD spec
#define RC_MAX_RAID_ARRAYS           8   // Maximum RAID arrays per controller

// RAID Array Types
#define RC_RAID_TYPE_RAID0           0
#define RC_RAID_TYPE_RAID1           1
#define RC_RAID_TYPE_RAID5           5
#define RC_RAID_TYPE_RAID6           6
#define RC_RAID_TYPE_RAID10          10
#define RC_RAID_TYPE_JBOD            15

// RAID Array States
#define RC_RAID_STATE_OFFLINE        0
#define RC_RAID_STATE_ONLINE         1
#define RC_RAID_STATE_DEGRADED       2
#define RC_RAID_STATE_REBUILDING     3
#define RC_RAID_STATE_FAILED         4

// Debug levels
#define RC_DEBUG                     0
#define RC_INFO                      1
#define RC_NOTE                      2
#define RC_WARN                      3
#define RC_ERROR                     4

// Debug macro
#define rc_printk(level, fmt, ...) \
    printk(KERN_INFO RC_DRIVER_NAME ": " fmt, ##__VA_ARGS__)

// Maximum number of PCI BARs we track
#define RC_MAX_BARS            PCI_STD_NUM_BARS

// Maximum number of queue descriptors
#define RC_MAX_QUEUE_DESCRIPTORS 32

// Interrupt delivery mode
enum rc_irq_mode {
    RC_IRQ_MODE_NONE = 0,
    RC_IRQ_MODE_MSI,
    RC_IRQ_MODE_MSIX,
    RC_IRQ_MODE_LEGACY,
};

// Per-BAR bookkeeping (mirrors StorPort resource descriptors)
struct rc_bar {
    resource_size_t phys;   // Physical base address
    resource_size_t len;    // Length of the BAR window
    u32 flags;              // IORESOURCE_* flags
    void __iomem *virt;     // Remapped pointer when mapped
};

// Forward declaration
struct rc_adapter;

// Interrupt/queue state (device extension offsets 0x670–0x6C0)
struct rc_irq_state {
    void *primary_queue;        // devExt+0x6B8
    void **queue_table;         // devExt+0x6C0
    u32 pending;                // devExt+0x678
    void *scratch_head;         // devExt+0x670
    void *scratch_tail;         // devExt+0x698
};

// Callback function pointers (device extension offsets 0x16100–0x16168)
// Windows driver installs different handlers based on controller variant
struct rc_adapter_callbacks {
    void (*queue_dispatcher)(struct rc_adapter *adapter, void *arg);      // +0x16100
    void (*queue_toggle)(struct rc_adapter *adapter, u8 mode, void *ptr); // +0x16108
    void (*spinlock_callback)(struct rc_adapter *adapter);                // +0x16110
    void (*port_disable)(struct rc_adapter *adapter, u32 port_id);        // +0x16120
    void (*port_resume)(struct rc_adapter *adapter, u32 port_id);         // +0x16130
    void (*status_poll)(struct rc_adapter *adapter, void *status_buf);    // +0x16140
    void (*secondary_queue)(struct rc_adapter *adapter, void *arg);       // +0x16148
};

// Work item for deferred queue processing (FUN_14000e960)
// 0x28 bytes (40 bytes), matches Windows structure
struct rc_work_item {
    struct rc_work_item *next;      // Next pointer (linked list)
    struct rc_work_item *prev;      // Previous pointer
    void *srb;                      // SRB pointer (param_2)
    void *param3;                   // Unused parameter (param_3)
    void (*completion)(void *);     // Completion callback (param_4)
    void *completion_arg;           // Completion callback argument
};

// Doorbell/template context (device extension offsets 0x16010–0x1C2DC)
struct rc_doorbell_state {
    void __iomem *template_page;    // devExt+0x16010
    void __iomem *doorbell_page;    // devExt+0x16020
    bool adapter_active;            // devExt+0x16054
    u8 queue_state;                 // devExt+0x16068
    u8 queue_mode;                  // devExt+0x1606C
    u16 variant[4];                 // devExt+0x16056/58/5A/5C
    u8 fast_path_enabled;           // devExt+0x1607C
    u32 capability_word;            // devExt+0x1C2D8
    struct rc_adapter_callbacks callbacks; // +0x16100–0x16168
    
    // Work item queue (devExt+0x15f80–0x15f90) - FUN_14000e960
    spinlock_t work_queue_lock;     // devExt+0x15f80 (spinlock)
    struct rc_work_item *work_queue_head; // devExt+0x15f88 (head pointer)
    struct rc_work_item *work_queue_tail; // devExt+0x15f90 (tail pointer)
};

// Queue handle structure (from FUN_14000c2fc)
// Used for NVMe command slot allocation
struct rc_queue_handle {
    void *queue_base;           // +0x00: Queue handle pointer
    u16 queue_depth;            // +0x02: Maximum number of slots
    u16 next_slot_index;        // +0x04: Where to start searching
    u16 producer_index;         // +0x06: Next slot to produce
    u16 consumer_index;         // +0x08: Next slot to consume
    u16 queue_size;             // +0x0a: Total queue size
    u64 queue_base_addr;        // +0x0c: MMIO queue base address
    u64 descriptor_array_base;  // +0x48: Array of 0x78-byte descriptor structures
    u32 queue_flags;            // +0x398: Queue flags (for FUN_14000eef8)
    void *descriptors[0];       // Descriptor array (variable size)
};

// Controller mode — selects the init/dispatch path.
// Mirrors the Windows AMD driver's split between ahci.c (iVar7==1) and
// nvme.c (iVar7==2). See docs/GHIDRA_FINDINGS_2026.md.
enum rc_ctrl_mode {
    RC_CTRL_MODE_UNKNOWN = 0,
    RC_CTRL_MODE_AHCI    = 1,   // DEV_7905/7916/7917/43BD (CC_0104)
    RC_CTRL_MODE_NVME    = 2,   // DEV_B000 and other CC_0108 devices
    RC_CTRL_MODE_STUB    = 99,  // unknown device, fallback to no-op stubs
};

// NVMe controller state (per NVMe 1.4 spec, populated for RC_CTRL_MODE_NVME).
// All DMA addresses are bus addresses; depths are entry counts (not bytes).
struct rc_nvme_state {
    // Admin queue (queue 0)
    void          *admin_sq;          // virtual addr of admin SQ buffer
    dma_addr_t     admin_sq_dma;      // bus addr (4 KiB aligned)
    void          *admin_cq;          // virtual addr of admin CQ buffer
    dma_addr_t     admin_cq_dma;      // bus addr (4 KiB aligned)
    u16            admin_sq_depth;    // number of admin SQ entries (max 4096)
    u16            admin_cq_depth;    // number of admin CQ entries (max 4096)
    u16            admin_sq_tail;     // next free admin SQ slot
    u16            admin_cq_head;     // next admin CQ slot to read
    u8             admin_cq_phase;    // phase bit, toggles each wrap

    // Capability snapshot (CAP at BAR0 + 0x00)
    u64            cap;
    u32            mqes;              // CAP.MQES + 1 = max queue depth (must be wider than u16: spec max is 65536)
    u8             dstrd;             // CAP.DSTRD doorbell stride
    u8             timeout_500ms;     // CAP.TO * 500 ms = boot timeout

    // Namespace 1 (per Identify NS); populated only after identify_namespace.
    u32            ns1_lba_bytes;     // LBA size in bytes (e.g. 512 or 4096)
    u64            ns1_nsze;          // total LBAs in the namespace

    // I/O queue 1 (single pair, polled). Allocated after admin Identify.
    void          *io_sq;             // 64-byte SQE array
    dma_addr_t     io_sq_dma;
    u16            io_sq_depth;
    u16            io_sq_tail;
    void          *io_cq;             // 16-byte CQE array
    dma_addr_t     io_cq_dma;
    u16            io_cq_depth;
    u16            io_cq_head;
    u8             io_cq_phase;

    // RAIDCore metadata (LBA 0x5000), populated after validation.
    bool           md_valid;
    u64            md_member_uuid;    // offset 0x10 — per-member identity
    u64            md_fld_18;         // offset 0x18 (purpose TBD)
    u64            md_fld_20;         // offset 0x20 (purpose TBD)
    u32            md_stripe_sectors; // offset 0x28 — likely stripe size in sectors
    u32            md_version;        // offset 0x2C — must equal 0x00030000
    u64            md_fld_30;         // offset 0x30 — count? (0x1C observed)
    u64            md_fld_38;         // offset 0x38 — per-member info

    // Per-doorbell pointers (computed once after CAP read)
    void __iomem  *sq_doorbell_base;  // BAR0 + 0x1000
};

// Device context layout (clean-room mirror of the Windows device extension)
struct rc_dev_context {
    struct rc_bar bar[RC_MAX_BARS];
    void __iomem *mmio_base;        // devExt+0x10 (primary BAR window)
    resource_size_t mmio_len;       // devExt+0x18
    resource_size_t mmio_phys;
    u8 bar_type[RC_MAX_BARS];       // devExt+0xB5 (per BAR attributes)
    void *descriptor_table;         // devExt+0x1C2A0
    struct rc_irq_state irq;
    struct rc_doorbell_state doorbell;

    // Hardware code-path selector. Set by rc_parse_firmware_capabilities()
    // based on PCI device ID. Gates AHCI vs NVMe vs stub init paths.
    enum rc_ctrl_mode ctrl_mode;

    // NVMe controller bookkeeping. Only valid when ctrl_mode == RC_CTRL_MODE_NVME.
    struct rc_nvme_state nvme;
    
    // Queue handles (from FUN_14000c2fc and FUN_14000fafc)
    struct rc_queue_handle *queue_handles[RC_MAX_QUEUE_DESCRIPTORS]; // devExt+0x15948
    u32 queue_index;                // devExt+0x15968: Queue index selector
    u32 queue_rotation_counter;     // devExt+0x1596c: Queue rotation counter
    u8 queue_state;                 // devExt+0x15920: Queue state flag (for FUN_140001ba4)
    u8 port_count;                  // devExt+0xb0: Port count (spinlock count)
    
    // NVMe-specific fields (for queue_state == 2)
    u8 nvme_queue_state;            // devExt+0x15d00: NVMe queue state
    u8 nvme_init_flag;              // devExt+0x15d01: NVMe initialization flag
    u32 nvme_completion_state[2];   // devExt+0x15ce0, devExt+0x15ce1
    u32 nvme_context;               // devExt+0x15ce8: NVMe context
    u32 nvme_flags;                 // devExt+0x15cd8: NVMe flags
    u64 controller_serial[8];       // devExt+0x15cf0-0x15cf7: Controller serial number
    u32 controller_id;              // devExt+0x15c48: Controller ID
    u32 controller_data[16];        // devExt+0x15c4c-0x15c88: Controller data
};

// Request tracking for blk-mq completion
#define RC_MAX_PENDING_REQUESTS 256

struct rc_pending_request {
    struct request *rq;
    dma_addr_t dma_addr;
    void *dma_buf;
    u32 cmd_id;
};

// Hardware command/ completion structures must be defined before the queue
// context because the context embeds completion instances for sync commands.
struct rc_hw_command {
    u32 command_id;
    u32 opcode;
    u32 flags;
    u32 channel_id;        // Logical device ID
    u64 lba;
    u32 sector_count;
    u64 data_addr;
    u64 completion_addr;
    u64 generation_number; // For config commands
    u32 reserved[2];
} __packed;

struct rc_hw_completion {
    u32 command_id;
    u32 status;
    u32 error_code;
    u32 bytes_transferred;
    u64 timestamp;
    u32 reserved[3];
} __packed;

// SRB (SCSI Request Block) structure - mirrors Windows driver layout
// Used for command submission and completion tracking
struct rc_srb {
    // Command tracking (offsets 0x00-0x60)
    u32 original_status;        // +0x13c: Original status (saved)
    u32 information;            // +0x140: Information field
    u32 status;                 // +0x100: Current status
    u32 secondary_status;       // +0x10c: Secondary status
    u16 status_byte;            // +0x110: Status byte (0=success, 2=error)
    u16 error_code;             // +0x10e: Error code (byte-swapped)
    u16 command_type;           // +0x112: Command type
    u8 srb_function;            // +0x14c: SRB function code
    u8 command_code;            // +0x60: Command type/code
    u8 sub_command;             // +0x61: Sub-command byte
    u8 flags_byte;              // +0x62: Flags byte
    
    // Scatter-gather and DMA
    void *scatter_gather_list;  // +0x48: Scatter-gather list pointer
    void *sg_handle;            // +0x58: Scatter-gather list handle
    u32 sg_flags;               // +0x64: Scatter-gather flags (bits 2-7)
    
    // Status and context
    u32 status_dword[4];        // +0x84-0x90: Status DWORDs (for FUN_140010184)
    u32 context;                // +0xc0: Context field
    u32 context2;               // +0xc4: Context field 2
    
    // Completion callback
    void (*completion_callback)(void *); // +0x70: Completion callback
    void *completion_arg;       // +0x18: Command descriptor pointer (for callbacks)
    
    // Port/channel information
    u32 port_id;                // +0x38+8: Port index
    
    // Command data
    u64 lba;                    // Logical block address
    u32 sector_count;           // Sector count
    u32 data_length;            // Data transfer length
    u32 reserved[20];           // Reserved fields
} __packed;

// Command descriptor structure (0x78 bytes) - from FUN_14000c2fc
// Used for NVMe command slot allocation
struct rc_command_descriptor {
    u16 slot_index;             // +0x20: Slot index in queue
    u16 queue_index;            // +0x22: Producer index when allocated
    void *srb_ptr;              // +0x18: SRB pointer (for completion)
    void (*callback)(void *);   // +0x28: Completion callback
    u8 command_type;            // +0x30: Command type/opcode
    u16 slot_index_copy;        // +0x32: Duplicate of +0x20
    u32 context;                // +0x34: Command context
    u64 metadata;               // +0x38: Command metadata
    u8 command_data[64];        // +0x40-0x7f: NVMe command DWORDs (64 bytes)
    void (*completion_cb)(void *); // +0x70: SRB completion callback
} __packed;

// Completion descriptor structure (0x68 bytes) - from FUN_140006e3c
struct rc_completion_descriptor {
    u8 completion_type;         // +0x00: Completion type
    u8 reserved1[5];            // Reserved
    void *srb_ptr;              // +0x06: SRB pointer (for type 2)
    u32 status_dword;           // +0x0c: Status DWORD
    u32 reserved2[3];           // Reserved
    u32 queue_index;            // +0x18: Queue index/port number
    u32 reserved3[2];           // Reserved
    u32 queue_type;             // +0x24: Queue type (0x1=primary, 0x2=secondary, 0x3=tertiary)
    u32 queue_depth;            // +0x28: Queue depth/size
    u32 completion_type_dword;  // +0x2c: Completion type (DWORD)
    u32 adapter_index;          // +0x30: Adapter index
    u32 status_flags;           // +0x34: Status flags
    u64 completion_data[4];     // +0x38-0x5f: Completion data (DMA addresses, command data)
    void *callback;             // +0x1c: Completion callback (for type 2)
} __packed;

// Vendor mailbox structure (TRX50 firmware requirement)
// Embedded in AHCI command table at offset 0x10e
// Decoded from rcbottom.sys FUN_140001008
struct rc_vendor_mailbox {
    u16 completion_flags;  /* +0x10e: 0x4400/0x1100/0x1400/0x4703/0x0000 */
    u8  cmd_type;          /* +0x110: 0x02 or 0x00 */
    u8  control_flags;     /* +0x111: command-specific control */
    u8  payload_length;    /* +0x112: 0x08, 0x14, or 0x00 */
    u8  secondary_control; /* +0x113: command-specific flags */
    u32 payload[5];        /* +0x114–0x124: command data */
    u8  reserved[0x2c];    /* +0x128–0x153: unused */
    u32 extended_flags;    /* +0x154: bit 17 = 0x20000 */
} __packed;

// Hardware adapter bookkeeping (queue/DMA resources)
struct rc_hw_queue_context {
    struct rc_adapter *owner;
    struct pci_dev *pdev;
    void __iomem *mmio_base;
    resource_size_t mmio_len;
    resource_size_t mmio_phys;
    u32 msi_vector;

    // Command / completion queues
    struct rc_hw_command *cmd_queue;
    dma_addr_t cmd_queue_dma;
    u32 cmd_queue_head;
    u32 cmd_queue_tail;
    u32 cmd_queue_size;

    struct rc_hw_completion *comp_queue;
    dma_addr_t comp_queue_dma;
    u32 comp_queue_head;
    u32 comp_queue_size;

    spinlock_t irq_lock;
    atomic_t irq_count;
    atomic_t cmd_sequence;
    atomic_t completion_count;
    atomic_t sync_completion_count;

    struct dma_pool *dma_pool;

    // Request tracking for blk-mq
    struct rc_pending_request pending_reqs[RC_MAX_PENDING_REQUESTS];
    spinlock_t pending_lock;

    spinlock_t sync_lock;
    struct {
        u32 cmd_id;
        struct rc_hw_completion completion;
        bool active;
        bool completed;
    } sync;
};

// Adapter structure (rcbottom equivalent)
struct rc_adapter {
    struct pci_dev *pdev;
    struct device *dev;
    int instance;
    struct dentry *debugfs_dir;

    // PCI identity
    u16 vendor_id;
    u16 device_id;
    u16 subsystem_vendor;
    u16 subsystem_device;
    u8 revision;

    // Power-management defaults (from rcbottom.inf)
    u32 enable_hipm;
    u32 enable_dipm;
    u32 hmb_policy;

    // Interrupt wiring
    enum rc_irq_mode irq_mode;
    int irq_vector;

    // Device-extension state mirrors
    struct rc_dev_context ctx;

    // Hardware queue/DMA resources
    struct rc_hw_queue_context hw;

    struct list_head list_node;
};

// Configuration structure (rccfg equivalent)
struct rc_config {
    struct miscdevice misc_dev;
    struct mutex lock;
    int initialized;
};

// RAID Array structure
struct rc_raid_array {
    int array_id;
    int raid_type;
    int state;
    sector_t total_sectors;
    sector_t used_sectors;
    int num_disks;
    int stripe_size;
    char name[32];
    struct gendisk *disk;
    struct request_queue *queue;
    struct blk_mq_tag_set tag_set;
    struct rc_adapter *adapter;
    int initialized;
    
    // Additional fields for blk-mq helper
    u64 size_bytes;          /* set this before calling rc_blk_create_disk */
    int index;               /* 0,1,2… */
    struct list_head page_list; /* in-memory backing store buckets */
    spinlock_t page_lock;
};

// RAID structure (rcraid equivalent)
struct rc_raid {
    struct Scsi_Host *host;
    struct rc_adapter *adapter;
    struct rc_raid_array arrays[RC_MAX_RAID_ARRAYS];
    int num_arrays;
    int initialized;
    int scsi_host_created;
};

// Global state
struct rc_global_state {
    struct rc_adapter *adapters[RC_MAX_ADAPTERS];
    struct rc_config config;
    struct rc_raid raid;
    int num_adapters;
    int initialized;
    struct mutex lock;
    struct list_head adapter_list;
};

extern struct rc_global_state rc_state;

// Function prototypes
int rc_bottom_init(void);
void rc_bottom_cleanup(void);
int rc_config_init(void);
void rc_config_cleanup(void);
int rc_raid_init(void);
void rc_raid_cleanup(void);

// PCI driver functions
int rc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void rc_pci_remove(struct pci_dev *pdev);

// SCSI functions
#if RC_SCSI_AVAILABLE
int rc_scsi_probe(struct scsi_device *sdev);
int rc_scsi_remove(struct scsi_device *sdev);
int rc_scsi_queuecommand(struct Scsi_Host *host, struct scsi_cmnd *scmd);
#else
int rc_scsi_probe(void *sdev);
int rc_scsi_remove(void *sdev);
int rc_scsi_queuecommand(void *host, void *scmd);
#endif

// Block device functions
int rc_raid_array_init(struct rc_raid_array *array);
void rc_raid_array_cleanup(struct rc_raid_array *array);
// Forward declaration removed - function is static
int rc_raid_array_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg);

// Real hardware register definitions (from TRX50 testing)
#define RC_MMIO_BASE_ADDR		0x81a80000
#define RC_MMIO_SIZE			1024
#define RC_MSI_VECTOR			244
#define RC_PCI_VID			0x1022
#define RC_PCI_DID			0x43bd

// Hardware register offsets (BAR 5 mapping)
#define RC_REG_STATUS			0x000
#define RC_REG_CONTROL			0x004
#define RC_REG_INTERRUPT_STATUS	0x008
#define RC_REG_INTERRUPT_MASK	0x00C
#define RC_REG_DOORBELL		0x010
#define RC_REG_CMD_Q_BASE_LO		0x020
#define RC_REG_CMD_Q_BASE_HI		0x024
#define RC_REG_CMD_Q_SIZE		0x028
#define RC_REG_COMP_Q_BASE_LO		0x030
#define RC_REG_COMP_Q_BASE_HI		0x034
#define RC_REG_COMP_Q_SIZE		0x038

/* NVMe controller registers (BAR0), per NVMe 1.4 spec.
 * Used when ctx->ctrl_mode == RC_CTRL_MODE_NVME (DEV_B000).
 */
#define RC_NVME_REG_CAP			0x00	/* 64-bit Controller Capabilities */
#define RC_NVME_REG_VS			0x08	/* Version */
#define RC_NVME_REG_INTMS		0x0c	/* Interrupt Mask Set */
#define RC_NVME_REG_INTMC		0x10	/* Interrupt Mask Clear */
#define RC_NVME_REG_CC			0x14	/* Controller Configuration */
#define RC_NVME_REG_CSTS		0x1c	/* Controller Status */
#define RC_NVME_REG_NSSR		0x20	/* NVM Subsystem Reset */
#define RC_NVME_REG_AQA			0x24	/* Admin Queue Attributes */
#define RC_NVME_REG_ASQ			0x28	/* 64-bit Admin SQ base */
#define RC_NVME_REG_ACQ			0x30	/* 64-bit Admin CQ base */
#define RC_NVME_REG_DBL_BASE		0x1000	/* Doorbell registers begin here */

/* CAP field accessors (64-bit register) */
#define RC_NVME_CAP_MQES(cap)		((u32)((cap) & 0xffff))		/* Max queue entries supported - 1 (0's based, so +1 needs u32) */
#define RC_NVME_CAP_TO(cap)		((u8)(((cap) >> 24) & 0xff))	/* Timeout (500 ms units) */
#define RC_NVME_CAP_DSTRD(cap)		((u8)(((cap) >> 32) & 0xf))	/* Doorbell stride */
#define RC_NVME_CAP_CSS(cap)		((u8)(((cap) >> 37) & 0xff))	/* Command sets supported */

/* CC fields (32-bit) */
#define RC_NVME_CC_EN			BIT(0)
#define RC_NVME_CC_CSS_NVM		(0u << 4)
#define RC_NVME_CC_MPS_4K		(0u << 7)	/* memory page size = 2^(12+MPS) */
#define RC_NVME_CC_AMS_RR		(0u << 11)
#define RC_NVME_CC_SHN_NONE		(0u << 14)
#define RC_NVME_CC_IOSQES_64		(6u << 16)	/* log2(64) */
#define RC_NVME_CC_IOCQES_16		(4u << 20)	/* log2(16) */

/* CSTS fields */
#define RC_NVME_CSTS_RDY		BIT(0)
#define RC_NVME_CSTS_CFS		BIT(1)		/* controller fatal status */

/* AQA helpers (each depth is encoded as count-1, 12 bits each) */
#define RC_NVME_AQA(sq_depth, cq_depth)	\
	((u32)(((sq_depth) - 1) & 0xfff) | ((u32)(((cq_depth) - 1) & 0xfff) << 16))

/* Admin queue: 64-byte SQ entries, 16-byte CQ entries. Linux NVMe driver uses
 * 32 entries for the admin queue; we follow the same convention. */
#define RC_NVME_ADMIN_QUEUE_DEPTH	32
#define RC_NVME_SQ_ENTRY_SIZE		64
#define RC_NVME_CQ_ENTRY_SIZE		16

/* Admin command opcodes (NVMe 1.4 §5) — only what we currently submit. */
#define RC_NVME_ADMIN_OP_DELETE_IO_SQ	0x00
#define RC_NVME_ADMIN_OP_CREATE_IO_SQ	0x01
#define RC_NVME_ADMIN_OP_DELETE_IO_CQ	0x04
#define RC_NVME_ADMIN_OP_CREATE_IO_CQ	0x05
#define RC_NVME_ADMIN_OP_IDENTIFY	0x06

/* NVM I/O opcodes */
#define RC_NVME_NVM_OP_READ		0x02

/* I/O queue depth; clamped against CAP.MQES. 64 is plenty for what we
 * currently submit (one READ at a time) and well under MQES (=65536). */
#define RC_NVME_IO_QUEUE_DEPTH		64
#define RC_NVME_IO_QID			1u

/* AMD RAIDCore metadata block (one LBA per member at LBA 0x5000). The
 * layout was recovered from rcraid.sys (RC_CheckMetaData / RC_ReadMetaData).
 * Bytes [0x08..0x1FF] are covered by a 64-bit checksum stored at [0x00]. */
#define RC_RAIDCORE_LBA			0x5000ULL
#define RC_RAIDCORE_BYTES		512u
#define RC_RAIDCORE_PAYLOAD_BYTES	0x1F8u	/* 504 = 0x200 - 8 */
#define RC_RAIDCORE_MAGIC		0x65726F4344494152ULL	/* "RAIDCore" LE */
#define RC_RAIDCORE_VERSION		0x00030000u

struct rc_raidcore_md {
	__le64	checksum;	/* 0x00 — XOR-with-shuffle over [0x08..0x1FF] */
	__le64	magic;		/* 0x08 = "RAIDCore" */
	__le64	member_uuid;	/* 0x10 — per-member identifier */
	__le64	fld_18;		/* 0x18 (purpose TBD) */
	__le64	fld_20;		/* 0x20 (purpose TBD) */
	__le32	stripe_sectors;	/* 0x28 — likely stripe size in sectors */
	__le32	version;	/* 0x2C — must be 0x00030000 */
	__le64	fld_30;		/* 0x30 — count? (0x1C observed) */
	__le64	fld_38;		/* 0x38 — per-member info */
	u8	reserved[RC_RAIDCORE_BYTES - 0x40];
} __packed;

/* Identify CNS values (NVMe 1.4 §5.15.1) */
#define RC_NVME_IDENTIFY_CNS_NS		0x00	/* Identify Namespace (requires NSID) */
#define RC_NVME_IDENTIFY_CNS_CTRL	0x01

/* Identify response is always 4 KiB. */
#define RC_NVME_IDENTIFY_BYTES		4096

/* Field offsets within the Identify Controller response (NVMe 1.4 §5.15.2.2).
 * The full structure is 4 KiB; we only access the fields we log. */
#define RC_NVME_ID_CTRL_VID		0	/* u16 */
#define RC_NVME_ID_CTRL_SSVID		2	/* u16 */
#define RC_NVME_ID_CTRL_SN		4	/* 20 bytes ASCII, space-padded */
#define RC_NVME_ID_CTRL_MN		24	/* 40 bytes ASCII, space-padded */
#define RC_NVME_ID_CTRL_FR		64	/* 8 bytes ASCII, space-padded */
#define RC_NVME_ID_CTRL_NN		516	/* u32, number of namespaces */

/* Field offsets within the Identify Namespace response (NVMe 1.4 §5.15.2.1). */
#define RC_NVME_ID_NS_NSZE		0	/* u64, namespace size in LBAs */
#define RC_NVME_ID_NS_NCAP		8	/* u64, namespace capacity in LBAs */
#define RC_NVME_ID_NS_NUSE		16	/* u64, used LBAs */
#define RC_NVME_ID_NS_FLBAS		26	/* u8, [3:0] = active LBA Format index */
#define RC_NVME_ID_NS_LBAF		128	/* base of LBAF table; entry i at +4*i */
/* LBAF entry: u32 where [16:23] is LBADS (log2 of LBA size in bytes). */

/* Admin command timeout — generous enough for cold controllers but bounded
 * so a misbehaving device doesn't hang module init forever. */
#define RC_NVME_ADMIN_TIMEOUT_MS	2000

/* NVMe Submission Queue Entry (NVMe 1.4 figure 105). Exactly 64 bytes. */
struct rc_nvme_sqe {
	u8	opc;
	u8	fuse_psdt;	/* [1:0]=FUSE, [7:6]=PSDT (0=PRP) */
	__le16	cid;
	__le32	nsid;
	__le64	rsvd2;
	__le64	mptr;
	__le64	prp1;
	__le64	prp2;
	__le32	cdw10;
	__le32	cdw11;
	__le32	cdw12;
	__le32	cdw13;
	__le32	cdw14;
	__le32	cdw15;
} __packed;

/* NVMe Completion Queue Entry (NVMe 1.4 figure 78). Exactly 16 bytes.
 * status bit 0 is the phase tag; bits [15:1] are SC/SCT/M/DNR. */
struct rc_nvme_cqe {
	__le32	result;
	__le32	rsvd;
	__le16	sq_head;
	__le16	sq_id;
	__le16	cid;
	__le16	status;
} __packed;

/* Per-port register block (AHCI compatible) */
#define RC_PORT_REG_BASE(idx)		(0x100 + ((idx) * 0x80))
#define RC_PORT_CLB			0x00
#define RC_PORT_CLBU			0x04
#define RC_PORT_FB			0x08
#define RC_PORT_FBU			0x0C
#define RC_PORT_IS			0x10
#define RC_PORT_CMD			0x18
#define RC_PORT_TFD			0x20
#define RC_PORT_SIG			0x24
#define RC_PORT_SERR			0x30
#define RC_PORT_SACT			0x34
#define RC_PORT_CI			0x38
#define RC_PORT_CMD_ST		BIT(0)
#define RC_PORT_CMD_FRE	BIT(4)
#define RC_PORT_CMD_FR		BIT(14)
#define RC_PORT_CMD_CR		BIT(15)

#define RC_CONTROL_RUN			0x80000000U

// Real AMD RAID command structures (based on Windows driver analysis)
#define RC_CMD_READ_DATA		0x01
#define RC_CMD_WRITE_DATA		0x02
#define RC_CMD_CONFIG_READ		0x03
#define RC_CMD_CONFIG_WRITE		0x04
#define RC_CMD_METADATA_READ		0x05
#define RC_CMD_METADATA_WRITE		0x06
#define RC_CMD_SCAN_DISKS		0x07
#define RC_CMD_RESCAN			0x08

// Command flags
#define RC_CMD_FLAG_SYNC		0x01
#define RC_CMD_FLAG_URGENT		0x02
#define RC_CMD_FLAG_NO_RETRY		0x04
#define RC_CMD_FLAG_DATA_IN		0x100
#define RC_CMD_FLAG_DATA_OUT		0x200

// Status codes
#define RC_STATUS_SUCCESS		0x00
#define RC_STATUS_ERROR			0x01
#define RC_STATUS_BUSY			0x02
#define RC_STATUS_INVALID_CMD		0x03
#define RC_STATUS_INVALID_PARAM	0x04
#define RC_STATUS_DEVICE_ERROR		0x05

// RAID metadata structures (based on Windows driver analysis)
struct rc_raid_metadata {
	u32 signature;		// RAID signature
	u32 version;		// Metadata version
	u32 array_id;		// Array identifier
	u32 raid_level;		// RAID level (0, 1, 5, 6, etc.)
	u32 num_disks;		// Number of member disks
	u64 array_size;		// Total array size in sectors
	u64 stripe_size;	// Stripe size in sectors
	u32 generation;		// Generation number
	u32 checksum;		// Metadata checksum
	u8 reserved[64];	// Reserved for future use
} __packed;

// Block major number (defined in rc_main.c)
extern int rc_major;

/* Called from rc_exit to tear down the assembled-RAID gendisk and free
 * its per-member DMA buffers (defined in rc_nvme.c). */
void rc_volume_teardown(void);

// Hardware functions
int rc_hw_init(struct rc_adapter *adapter);
void rc_hw_cleanup(struct rc_adapter *adapter);
int rc_hw_submit_command(struct rc_hw_queue_context *hw, struct rc_hw_command *cmd);
int rc_hw_process_completions(struct rc_hw_queue_context *hw);
int rc_hw_submit_sync_command(struct rc_hw_queue_context *hw,
                             struct rc_hw_command *cmd,
                             struct rc_hw_completion *completion,
                             unsigned int timeout_ms);
int rc_hw_poll_completions(struct rc_hw_queue_context *hw);
irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id);
int rc_hw_submit_request(struct rc_adapter *adapter, struct request *rq,
                         u32 opcode, sector_t lba, u32 sector_count,
                         dma_addr_t dma_addr, void *dma_buf);
int rc_install_callbacks(struct rc_adapter *adapter, bool fast_path);
int rc_parse_firmware_capabilities(struct rc_adapter *adapter);

// NVMe controller path (rc_nvme.c). Used when ctx->ctrl_mode == RC_CTRL_MODE_NVME.
int rc_nvme_init_controller(struct rc_adapter *adapter);
void rc_nvme_cleanup_controller(struct rc_adapter *adapter);

// Queue management functions (StorPort service slot equivalents)
int rc_queue_init(struct rc_adapter *adapter);
void rc_queue_cleanup(struct rc_adapter *adapter);
int rc_activate_doorbells(struct rc_adapter *adapter);
int rc_queue_issue_sync(struct rc_adapter *adapter,
                       struct rc_hw_command *cmd,
                       bool data_in, bool data_out,
                       unsigned int timeout_ms,
                       struct rc_hw_completion *completion);

// Queue full handler (FUN_14000e960) - deferred work item queue
void rc_queue_full_handler(struct rc_adapter *adapter, void *srb,
                           void *param3, void (*completion)(void *),
                           void *completion_arg);
int rc_process_deferred_work_items(struct rc_adapter *adapter);
void rc_cleanup_work_item_queue(struct rc_adapter *adapter);

// Queue initialization functions (FUN_140008a48, FUN_140007978, FUN_1400093c4)
int rc_init_queue_bar(struct rc_adapter *adapter);
int rc_program_global_completion_registers(struct rc_adapter *adapter);
int rc_queue_state_table_lookup(struct rc_adapter *adapter);

// Simple callback functions (from FUN_140007d40)
u8 rc_get_queue_state(struct rc_adapter *adapter);
int rc_check_queue_activity(struct rc_adapter *adapter);
void rc_toggle_queue_mode(struct rc_adapter *adapter, u8 mode);
void rc_noop_helper(struct rc_adapter *adapter);

// SRB completion and error handling (FUN_14000c900)
void rc_srb_completion_handler(struct rc_srb *srb,
                                void (*completion_callback)(void *),
                                u8 status_byte, u16 status_word);
void rc_srb_indirect_completion(struct rc_srb *srb);

// Completion callback functions (rcraid.sys)
void rc_nvme_completion_with_sg(void *queue_handle,
                                 void *completion_queue_desc,
                                 struct rc_command_descriptor *cmd_desc,
                                 struct rc_completion_descriptor *comp_desc);
void rc_simple_completion_callback(void *unused1, void *unused2,
                                    struct rc_command_descriptor *cmd_desc,
                                    struct rc_completion_descriptor *comp_desc);
void rc_completion_with_status_update(void *unused1, void *unused2,
                                       struct rc_command_descriptor *cmd_desc,
                                       struct rc_completion_descriptor *comp_desc);

// Queue callback handler (FUN_14000eef8)
int rc_queue_callback_handler(void *queue_handle, void *unused,
                               struct rc_completion_descriptor *comp_desc,
                               void *completion_queue_entry);

// Command routing functions (rcraid.sys)
int rc_primary_queue_dispatcher(struct rc_adapter *adapter,
                                 struct rc_srb *srb,
                                 void *unused3, void *unused4);
int rc_secondary_queue_dispatcher(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void *unused3, void *unused4);
int rc_command_router(struct rc_adapter *adapter,
                       struct rc_srb *srb,
                       void *unused3, void *unused4);
int rc_command_router_srb06(struct rc_adapter *adapter,
                             struct rc_srb *srb,
                             void *unused3, void *completion_callback);

// Completion processing (FUN_140006e3c)
int rc_process_completions_all_adapters(struct rc_adapter *adapter);

// Multi-adapter support (FUN_14000a564, FUN_14000a72c)
int rc_multi_adapter_disconnect(struct rc_adapter *adapter);
int rc_multi_adapter_connect(struct rc_adapter *adapter, int adapter_index);

// Descriptor lookup and command configuration (FUN_14000c2fc, FUN_14000c9e4, FUN_14000c1e4)
struct rc_command_descriptor *rc_allocate_command_descriptor(
    struct rc_queue_handle *queue_handle,
    void (*completion_callback)(void *));
int rc_build_scatter_gather_list(struct rc_queue_handle *queue_handle,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  struct rc_command_descriptor *cmd_desc);
int rc_configure_command_after_state_machine(struct rc_queue_handle *queue_handle,
                                              struct rc_adapter *adapter);

// NVMe command submission functions (from TECHNICAL_REFERENCE.md)
int rc_submit_command_type0(struct rc_adapter *adapter,
                            struct rc_srb *srb,
                            void *unused3,
                            void (*completion)(void *));
int rc_submit_queue_management_cmd10(struct rc_queue_handle *queue);
int rc_submit_queue_management_cmd6(struct rc_queue_handle *queue);
int rc_submit_queue_management_cmd6_safe(struct rc_queue_handle *queue);
int rc_submit_queue_management_cmd6_param(struct rc_queue_handle *queue, u32 param);
int rc_submit_command_type9(struct rc_adapter *adapter,
                            struct rc_srb *srb,
                            void *command_ptr,
                            void (*completion)(void *));
int rc_submit_scatter_gather_cmd2(struct rc_adapter *adapter,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  void (*completion)(void *));
int rc_submit_nvme_command_type8(struct rc_adapter *adapter,
                                  void *unused2,
                                  struct rc_srb *srb,
                                  void (*completion)(void *));
int rc_submit_command_type2_or_c(struct rc_adapter *adapter,
                                  struct rc_srb *srb,
                                  void *sg_list,
                                  void (*completion)(void *));
int rc_submit_command_type5(struct rc_queue_handle *queue);
int rc_submit_command_helper_type1(struct rc_queue_handle *queue,
                                    void (*completion)(void *));
int rc_submit_commands_iterative(struct rc_queue_handle *queue,
                                  void (*completion)(void *));
int rc_submit_state_machine_command(struct rc_queue_handle *queue,
                                     struct rc_completion_descriptor *comp_desc,
                                     struct rc_srb *srb,
                                     void (*completion)(void *));

// Simple helper/getter functions
u8 rc_get_nvme_queue_state(struct rc_adapter *adapter);
void rc_nvme_status_update(struct rc_adapter *adapter, u32 status_flags);
int rc_nvme_command_submission_wrapper(struct rc_adapter *adapter, u32 port_index);
int rc_steady_state_dispatcher(struct rc_adapter *adapter);
int rc_helper_stub(struct rc_adapter *adapter, u32 param);
void rc_early_init_stub(struct rc_adapter *adapter);

// Remaining command submission functions
int rc_submit_queue_type_selection(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void *unused3,
                                   void (*completion)(void *));
int rc_submit_command_with_limits(struct rc_queue_handle *queue);
int rc_submit_command_with_params(struct rc_queue_handle *queue,
                                  u32 param2,
                                  u32 param3,
                                  void (*completion)(void *));
int rc_submit_command_with_flags(struct rc_queue_handle *queue, u8 flags);
int rc_submit_command_with_context(struct rc_adapter *adapter,
                                   struct rc_srb *srb,
                                   void (*completion)(void *),
                                   u32 context_param);
int rc_submit_command_context_type4(struct rc_adapter *adapter,
                                    struct rc_srb *srb,
                                    void *unused3,
                                    void (*completion)(void *));
int rc_submit_special_command_11(struct rc_adapter *adapter,
                                 struct rc_srb *srb,
                                 void *unused3,
                                 void (*completion)(void *));

// Simple callback functions
void rc_queue_cleanup_all_ports(struct rc_adapter *adapter);
void rc_status_polling_cleanup(struct rc_adapter *adapter, void *status_buf);
int rc_early_init_wrapper(struct rc_adapter *adapter,
                          void *port_context,
                          struct rc_srb *srb,
                          void (*completion)(void *));

// Additional callback and command submission functions
int rc_submit_queue_rotation_command(struct rc_adapter *adapter,
                                     struct rc_srb *srb,
                                     void *unused3,
                                     void (*completion)(void *));
int rc_submit_complex_state_machine(struct rc_queue_handle *queue,
                                    void *unused2,
                                    struct rc_completion_descriptor *comp_desc,
                                    struct rc_srb *srb);
int rc_check_nvme_completion(struct rc_adapter *adapter, int port_index);
int rc_secondary_queue_helper_legacy(struct rc_adapter *adapter,
                                     struct rc_srb *srb,
                                     void *sg_list,
                                     void (*completion)(void *));
int rc_primary_queue_dispatcher_legacy(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list,
                                       void (*completion)(void *));
int rc_command_routing_helper(struct rc_adapter *adapter,
                              struct rc_srb *srb,
                              void *sg_list,
                              void (*completion)(void *));
void rc_ahci_command_submission(struct rc_adapter *adapter,
                                struct rc_srb *srb,
                                void (*completion)(void *));
int rc_command_submission_callback(void *adapter_handle,
                                   void *unused2,
                                   void *srb_context,
                                   void *unused4,
                                   void *sg_list_ptr);
void rc_nvme_command_submission_legacy(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void (*completion)(void *));
int rc_nvme_queue_initialization(void *queue_desc,
                                 void *adapter_handle,
                                 u32 queue_index,
                                 u32 sub_queue_index,
                                 u8 init_flag,
                                 u8 enable_flag,
                                 dma_addr_t comp_queue_base,
                                 dma_addr_t sub_queue_base);
int rc_mmio_register_io(void *register_context,
                        void *unused2,
                        void *buffer_descriptor);

// Critical initialization functions
int rc_spinlock_callback_queue_init(struct rc_adapter *adapter, u8 mode);
int rc_port_enable_resume(struct rc_adapter *adapter);
int rc_port_disable_quiesce(struct rc_adapter *adapter, u32 port_mask);
int rc_nvme_spinlock_callback_init(struct rc_adapter *adapter, u8 init_flag);
int rc_nvme_cleanup_completion(struct rc_adapter *adapter);
int rc_adapter_init_device_enumeration(struct rc_adapter *adapter);
int rc_adapter_object_wmi_registration(struct rc_adapter *adapter);

// Special command handlers (FUN_14000f838, FUN_14000fa2c)
int rc_special_command_handler_type12(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list, void *unused4);
int rc_special_command_handler_type9e(struct rc_adapter *adapter,
                                       struct rc_srb *srb,
                                       void *sg_list, void *unused4);

// Sysfs interface
int rc_sysfs_create(struct rc_adapter *adapter);
void rc_sysfs_remove(struct rc_adapter *adapter);

// Debugfs interface
int rc_debugfs_init(void);
void rc_debugfs_cleanup(void);
int rc_debugfs_create_adapter(struct rc_adapter *adapter);
void rc_debugfs_remove_adapter(struct rc_adapter *adapter);

// Metadata discovery functions
int rc_discover_arrays(struct rc_adapter *adapter);
int rc_read_array_metadata(struct rc_adapter *adapter, int array_id, 
			   struct rc_raid_metadata *metadata);
int rc_scan_physical_disks(struct rc_adapter *adapter);

// RAID management functions
int rc_raid_scan_arrays(struct rc_adapter *adapter);
int rc_raid_get_array_info(struct rc_raid_array *array);
int rc_raid_create_array(struct rc_adapter *adapter, int raid_type, int num_disks);
int rc_raid_delete_array(struct rc_raid_array *array);

// Interrupt handlers
irqreturn_t rc_interrupt_handler(int irq, void *dev_id);

#endif /* _RC_LINUX_H_ */
