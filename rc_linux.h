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

// Interrupt delivery mode
enum rc_irq_mode {
    RC_IRQ_MODE_NONE = 0,
    RC_IRQ_MODE_MSI,
    RC_IRQ_MODE_LEGACY,
};

// Per-BAR bookkeeping (mirrors StorPort resource descriptors)
struct rc_bar {
    resource_size_t phys;   // Physical base address
    resource_size_t len;    // Length of the BAR window
    u32 flags;              // IORESOURCE_* flags
    void __iomem *virt;     // Remapped pointer when mapped
};

// Interrupt/queue state (device extension offsets 0x670–0x6C0)
struct rc_irq_state {
    void *primary_queue;        // devExt+0x6B8
    void **queue_table;         // devExt+0x6C0
    u32 pending;                // devExt+0x678
    void *scratch_head;         // devExt+0x670
    void *scratch_tail;         // devExt+0x698
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
};

// Request tracking for blk-mq completion
#define RC_MAX_PENDING_REQUESTS 256

struct rc_pending_request {
    struct request *rq;
    dma_addr_t dma_addr;
    void *dma_buf;
    u32 cmd_id;
};

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
    u32 comp_queue_tail;
    u32 comp_queue_size;

    spinlock_t irq_lock;
    atomic_t irq_count;
    atomic_t cmd_sequence;

    struct dma_pool *dma_pool;

    // Request tracking for blk-mq
    struct rc_pending_request pending_reqs[RC_MAX_PENDING_REQUESTS];
    spinlock_t pending_lock;
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
#define RC_REG_COMMAND_QUEUE		0x000
#define RC_REG_COMPLETION_QUEUE	0x100
#define RC_REG_DOORBELL		0x200
#define RC_REG_STATUS			0x300
#define RC_REG_CONTROL			0x400
#define RC_REG_INTERRUPT_STATUS	0x500
#define RC_REG_INTERRUPT_MASK	0x600

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

// Status codes
#define RC_STATUS_SUCCESS		0x00
#define RC_STATUS_ERROR			0x01
#define RC_STATUS_BUSY			0x02
#define RC_STATUS_INVALID_CMD		0x03
#define RC_STATUS_INVALID_PARAM	0x04
#define RC_STATUS_DEVICE_ERROR		0x05

// Command queue structures
struct rc_hw_command {
	u32 command_id;
	u32 opcode;
	u32 flags;
	u32 channel_id;		// Logical device ID
	u64 lba;
	u32 sector_count;
	u64 data_addr;
	u64 completion_addr;
	u64 generation_number;	// For config commands
	u32 reserved[2];
} __packed;

// Completion queue structures  
struct rc_hw_completion {
	u32 command_id;
	u32 status;
	u32 error_code;
	u32 bytes_transferred;
	u64 timestamp;
	u32 reserved[3];
} __packed;

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

// Hardware adapter structure
struct rc_hw_adapter {
	void __iomem *mmio_base;
	u32 msi_vector;
	struct pci_dev *pdev;
	
	// Command/Completion queues
	struct rc_hw_command *cmd_queue;
	dma_addr_t cmd_queue_dma;
	u32 cmd_queue_head;
	u32 cmd_queue_tail;
	u32 cmd_queue_size;
	
	struct rc_hw_completion *comp_queue;
	dma_addr_t comp_queue_dma;
	u32 comp_queue_head;
	u32 comp_queue_tail;
	u32 comp_queue_size;
	
	// Interrupt handling
	spinlock_t irq_lock;
	atomic_t irq_count;
	
	// DMA operations
	struct dma_pool *dma_pool;
};

// Block major number (defined in rc_main.c)
extern int rc_major;

// Hardware functions
int rc_hw_init(struct rc_adapter *adapter);
void rc_hw_cleanup(struct rc_adapter *adapter);
int rc_hw_submit_command(struct rc_hw_queue_context *hw, struct rc_hw_command *cmd);
int rc_hw_process_completions(struct rc_hw_queue_context *hw);
irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id);
int rc_hw_submit_request(struct rc_adapter *adapter, struct request *rq,
                         u32 opcode, sector_t lba, u32 sector_count,
                         dma_addr_t dma_addr, void *dma_buf);

// Queue management functions (StorPort service slot equivalents)
int rc_queue_init(struct rc_adapter *adapter);
void rc_queue_cleanup(struct rc_adapter *adapter);
int rc_activate_doorbells(struct rc_adapter *adapter);

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
