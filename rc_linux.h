// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — shared definitions and adapter state
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries (rcbottom.sys,
 * rcraid.sys, rccfg.sys) under DMCA §1201(f) interoperability
 * protections. No code is copied from AMD source distributions.
 * See RE_METHODOLOGY.md at the repository root for the full process
 * and legal record.
 */

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

// Source revision, normally injected by the Makefile (git describe, or the
// .rcraid_rev file in DKMS/live-CD staged trees).
#ifndef RC_DRIVER_BUILD_REV
#define RC_DRIVER_BUILD_REV          "unknown"
#endif

// Maximum adapters and targets (AMD Official Specifications)
#define RC_MAX_ADAPTERS              11  // Max Controller Count per AMD spec
#define RC_MAX_NVME_DEVICES          10  // Max NVMe devices per AMD spec

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

// Doorbell/template context — small remnant of the AHCI-era device
// extension.  Only fields that the live NVMe path still touches are
// kept (variant[]/queue_state/queue_mode are zeroed in
// rc_parse_firmware_capabilities; capability_word is reserved for
// future AHCI use).  The historic callback table and work-item queue
// were removed with the AHCI scaffolding drop.
struct rc_doorbell_state {
    void __iomem *template_page;
    void __iomem *doorbell_page;
    bool adapter_active;
    u8  queue_state;
    u8  queue_mode;
    u16 variant[4];
    u8  fast_path_enabled;
    u32 capability_word;
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

/* Target number of I/O queue pairs to request at init via Set Features
 * Number of Queues.  Matches Windows rcbottom.sys's hardcoded cap (see
 * docs/WINDOWS_MULTIQUEUE_FINDINGS.md).  Controller may grant fewer;
 * the granted count lands in nvme->nr_io_queues.  Step 3a creates this
 * many queue pairs (with their own MSI-X vectors); step 3b connects
 * them to blk-mq via per-CPU hctx mapping. */
#define RC_NVME_IO_QUEUE_TARGET		4u

// One NVMe I/O queue pair (SQ + CQ + its own MSI-X vector).  Allocated
// per-queue at controller init, sized using nvme->nr_io_queues granted
// by Set Features.  Doorbell offsets are computed from qid and CAP.DSTRD
// at submit/complete time.
struct rc_nvme_io_queue {
    struct rc_adapter *adapter;   // back-pointer so the per-queue ISR can find peer state
    u16            qid;           // NVMe queue ID (1, 2, ..., nr_io_queues)

    void          *sq;            // 64-byte SQE array
    dma_addr_t     sq_dma;
    u16            sq_depth;
    u16            sq_tail;

    void          *cq;            // 16-byte CQE array
    dma_addr_t     cq_dma;
    u16            cq_depth;
    u16            cq_head;
    u8             cq_phase;

    // Linux IRQ number assigned to this queue's MSI-X vector
    // (pci_irq_vector(pdev, qid)).  -1 if not registered.
    int            irq_vector;

    // Guards sq_tail + cq_head + cq_phase against submitter/ISR race.
    spinlock_t     lock;

    // Used only by the boot-time sync helper (rc_nvme_io_cmd_sync) which
    // sleeps here until the matching CQE wakes it.  Once rc_volume_disk
    // is up the ISR routes CQEs to blk_mq_complete_request instead.
    wait_queue_head_t cq_wait;

    // hctx index this queue is mapped to.  Used by the per-queue ISR
    // to look up the right blk_mq_tags array.  -1 until blk-mq mapping
    // is established (step 3b); during 3a all queues map to hctx 0 and
    // only queue 0 sees traffic.
    int            hctx_idx;
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
    u8             mdts;              // Identify Controller MDTS (0 = unlimited)
    u32            max_transfer_bytes;// derived from MDTS + CAP.MPSMIN (0 if unlimited)
    u8             vwc_present;       // Identify Controller VWC bit0: 1 = controller has a volatile write cache

    // Namespace 1 (per Identify NS); populated only after identify_namespace.
    u32            ns1_lba_bytes;     // LBA size in bytes (e.g. 512 or 4096)
    u64            ns1_nsze;          // total LBAs in the namespace

    // I/O queue count granted by the controller in response to
    // Set Features Number of Queues at init.  In step 3a we create
    // this many queue pairs but blk-mq stays single-hctx (all
    // dispatches still go to io_queues[0]).  Step 3b makes blk-mq
    // use them all via per-CPU hctx mapping.
    u16            nr_io_queues;

    // Array of allocated per-queue contexts.  io_queues[i] is qid i+1
    // on the wire.  Sized at RC_NVME_IO_QUEUE_TARGET (the static cap)
    // even when nr_io_queues is smaller; unused slots are NULL.
    struct rc_nvme_io_queue *io_queues[RC_NVME_IO_QUEUE_TARGET];

    // Per-member RAIDCore metadata (LBA 0x5000), populated after validation.
    // Field names mirror struct RC_MetaData in AMD's SDK (rcblob.x86_64).
    bool           md_valid;
    u64            md_device_id;        // RC_MetaData.DeviceId
    u64            md_config_commit_lba;// RC_MetaData.ConfigCommitOffset (= 0x5001 here)
    u64            md_config_ring_lba;  // RC_MetaData.ConfigRingOffset  (= 0x5800 here)
    u32            md_stripe_sectors;   // RC_MetaData.ConfigRingSize — see note in struct rc_raidcore_md
    u32            md_version;          // RC_MetaData.Version
    u32            md_features;         // RC_MetaData.Features
    u32            md_spare_info;       // RC_MetaData.SpareInfo
    u64            md_mbr_checksum;     // RC_MetaData.MBRChecksum

    // Volume-level info parsed from the on-disk RC_LogicalDevice record in
    // this member's config ring (LBA md_config_ring_lba + scan).  Populated
    // after rc_volume_parse_logical_device runs.
    bool           ld_valid;
    u32            ld_device_type;      // RC_LogicalDevice.DeviceType (RC_LDT_*)
    u32            ld_devices;          // RC_LogicalDevice.Devices (member count)
    u32            ld_chunk_sectors;    // RC_LogicalDevice.ChunkSize (0 → use RAID-level default)
    u32            ld_chunk_index;      // RC_LogicalDevice field@0x110 — RAID0 stripe encoding (1→64K, 2→128K, 3→256K, ...)
    u64            ld_capacity_sectors; // RC_LogicalDevice.Capacity (total volume sectors)
    int            ld_my_position;      // index of THIS adapter in LogicalElement array, -1 if not found
    u64            ld_alloc_offset;     // RC_LogicalElement_LE.AllocOffset (sector offset of allocated region)
    u64            ld_alloc_size;       // RC_LogicalElement_LE.AllocSize
    u64            ld_userdata_offset;  // RC_LogicalElement_LE.UserDataOffset (sector offset of user data on member)
    u64            ld_userdata_size;    // RC_LogicalElement_LE.UserDataSize

    // Per-doorbell pointers (computed once after CAP read)
    void __iomem  *sq_doorbell_base;  // BAR0 + 0x1000

    // Admin wait queue, woken from rc_nvme_admin_irq when an admin CQE
    // arrives.  Per-I/O-queue wait queues (used only by the boot-time
    // sync helper) live inside struct rc_nvme_io_queue.
    wait_queue_head_t admin_cq_wait;

    // Once set, no new I/O dispatches to this adapter and the next
    // .timeout callback drains every in-flight request that targeted it.
    // Set from rc_nvme_irq (CSTS.CFS or CSTS=~0) and from rc_volume_timeout
    // (timeout without a controller-reset path means we can't safely recycle
    // this CID).  Read with READ_ONCE in dispatch hot paths.  Never cleared.
    bool              dead;

    // Serializes rc_nvme_admin_cmd so a timeout-issued Abort can't collide
    // with module teardown or with another timeout's admin issue.  Held
    // across SQE write + doorbell + CQE wait + CQ-head advance.
    struct mutex      admin_mutex;

    // Scheduled from rc_volume_timeout when an adapter is flagged dead.
    // Calls rc_nvme_reset_controller and clears `dead` on success.
    // workqueue serialises duplicate schedules: queuing while already
    // queued is a no-op, so multiple timeouts on the same death episode
    // coalesce into one reset attempt.
    struct work_struct auto_reset_work;

    // Latched true after an auto-reset attempt fails.  Suppresses further
    // automatic attempts until either a manual sysfs reset succeeds (the
    // reset path clears this) or the module is reloaded.  Without this
    // latch a genuinely-fried controller would thrash 30 s per timeout
    // forever.
    bool              auto_reset_disabled;
};

// Device context layout (clean-room mirror of the Windows device extension)
struct rc_dev_context {
    struct rc_bar bar[RC_MAX_BARS];
    void __iomem *mmio_base;        // primary BAR window
    resource_size_t mmio_len;
    resource_size_t mmio_phys;
    u8 bar_type[RC_MAX_BARS];
    struct rc_irq_state irq;
    struct rc_doorbell_state doorbell;

    // PCI-class-based code-path selector set by
    // rc_parse_firmware_capabilities().  Gates NVMe vs (future) AHCI vs
    // stub init paths.
    enum rc_ctrl_mode ctrl_mode;

    // NVMe controller bookkeeping; valid only when ctrl_mode == NVMe.
    struct rc_nvme_state nvme;
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

// Hardware adapter bookkeeping (queue/DMA resources).  The cmd/comp
// queue fields and rc_hw_command/rc_hw_completion definitions are
// kept because rc_debugfs.c still seq_prints them; the queues are
// never allocated on the live NVMe path so they always read empty.
// When the AHCI path is built (or debugfs is reworked), these fields
// will either be populated or removed.
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

// Global state
struct rc_global_state {
    struct rc_adapter *adapters[RC_MAX_ADAPTERS];
    struct rc_config config;
    int num_adapters;
    int initialized;
    struct mutex lock;
    struct list_head adapter_list;
};

extern struct rc_global_state rc_state;

// Module-level lifecycle
int  rc_bottom_init(void);
void rc_bottom_cleanup(void);
int  rc_config_init(void);
void rc_config_cleanup(void);

/* Historical AHCI register offsets are kept here for the future SATA
 * RAID path; the live NVMe code uses RC_NVME_REG_* below instead. */
#define RC_REG_STATUS			0x000
#define RC_REG_CONTROL			0x004
#define RC_REG_INTERRUPT_STATUS	0x008
#define RC_REG_INTERRUPT_MASK	0x00C
#define RC_REG_DOORBELL		0x010

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
#define RC_NVME_ADMIN_OP_ABORT		0x08
#define RC_NVME_ADMIN_OP_SET_FEATURES	0x09

/* Feature IDs for Set Features (CDW10[7:0]). */
#define RC_NVME_FID_VOLATILE_WRITE_CACHE 0x06	/* CDW11 bit0 = WCE */
#define RC_NVME_FID_NUMBER_OF_QUEUES	0x07

/* NVM I/O opcodes (NVMe 1.4 §6) */
#define RC_NVME_NVM_OP_FLUSH		0x00
#define RC_NVME_NVM_OP_WRITE		0x01
#define RC_NVME_NVM_OP_READ		0x02
#define RC_NVME_NVM_OP_DSM		0x09	/* Dataset Management */

/* DSM CDW11 attribute bits (NVMe 1.4 §6.4.1) */
#define RC_NVME_DSM_AD			(1u << 2)	/* AD: Deallocate */

/* DSM range list entry (16 B per range) */
struct rc_nvme_dsm_range {
	__le32 context_attrs;	/* 0 — vendor-defined; leave zero */
	__le32 nlb;		/* length in logical blocks (0's based? no — 1's based per spec) */
	__le64 slba;		/* starting LBA */
} __packed;

/* Per-queue SQ/CQ depth.  Matches Windows rcbottom.sys's per-queue
 * size of 256 (see docs/WINDOWS_MULTIQUEUE_FINDINGS.md).  Clamped
 * against CAP.MQES at queue-create time.  With nr_hw_queues=4 this
 * gives 1024 total outstanding NVMe commands per controller. */
#define RC_NVME_IO_QUEUE_DEPTH		256
#define RC_NVME_IO_QID			1u

/* AMD RAIDCore per-member metadata block (one LBA per member at LBA 0x5000).
 * Layout matches struct RC_MetaData from AMD's open Linux SDK
 * (drivers/reference/amd-sdk-9.3.0 / rcblob.x86_64 — has DWARF info).
 * RC_CheckMetaData validates: magic at +0x08 == "RAIDCore", version at
 * +0x2C == 0x00030000, and the checksum at +0x00 covers bytes [0x08..0x1FF].
 *
 * The legacy field name `stripe_sectors` is kept for the +0x28 field for
 * source compatibility — historically the driver used this value as the
 * stripe size and it happens to be 2048 on the dev box, which also happens
 * to be the correct stripe size in sectors.  The SDK calls this field
 * `ConfigRingSize` (size of the config ring at ConfigRingOffset).  The
 * real per-volume stripe lives in RC_LogicalDevice.ChunkSize on disk,
 * which we don't yet parse — see docs/OPEN_QUESTIONS.md. */
#define RC_RAIDCORE_LBA			0x5000ULL
#define RC_RAIDCORE_BYTES		512u
#define RC_RAIDCORE_PAYLOAD_BYTES	0x1F8u	/* 504 = 0x200 - 8 */
#define RC_RAIDCORE_MAGIC		0x65726F4344494152ULL	/* "RAIDCore" LE */
#define RC_RAIDCORE_VERSION		0x00030000u

struct rc_raidcore_md {
	__le64	checksum;		/* 0x00 — XOR-with-shuffle over [0x08..0x1FF] */
	__le64	magic;			/* 0x08 — "RAIDCore" (RC_MetaData.RCIdent) */
	__le64	device_id;		/* 0x10 — RC_MetaData.DeviceId (was member_uuid) */
	__le64	config_commit_lba;	/* 0x18 — RC_MetaData.ConfigCommitOffset (LBA of config packet) */
	__le64	config_ring_lba;	/* 0x20 — RC_MetaData.ConfigRingOffset  (LBA of config ring)   */
	__le32	stripe_sectors;		/* 0x28 — RC_MetaData.ConfigRingSize; see comment above   */
	__le32	version;		/* 0x2C — RC_MetaData.Version (must be 0x00030000) */
	__le32	features;		/* 0x30 — RC_MetaData.Features (0x1C observed) */
	__le32	spare_info;		/* 0x34 — RC_MetaData.SpareInfo */
	__le64	mbr_checksum;		/* 0x38 — RC_MetaData.MBRChecksum (differs per member) */
	u8	reserved[RC_RAIDCORE_BYTES - 0x40];
} __packed;

/* On-disk record-type tags (first u32 of each record in the config ring).
 * From RC_DeviceStructType enum in rcblob.x86_64 DWARF. */
#define RC_DST_PHYSICAL_DEVICE		0x25BCu
#define RC_DST_LOGICAL_DEVICE		0x25BDu
#define RC_DST_CONTROLLER_DEVICE	0x25BEu
#define RC_DST_SEP_DEVICE		0x25BFu

/* On-disk LogicalDevice DeviceType values.  The DWARF enum
 * RC_LogicalDeviceTypes only publishes the "exotic" types starting at 7157;
 * the basic RAID levels use an undocumented range that the writer dispatches
 * on directly (see RC_CreateRaidArray in rcblob).  RAID0 = 0x1BF6 confirmed
 * by reading the live array on the dev box.  Other values listed here are
 * inferred from the dispatch table and are best-effort. */
#define RC_LDT_RAID0			0x1BF6u
#define RC_LDT_RAID1			0x1BF7u
#define RC_LDT_RAID5			0x1BFAu
#define RC_LDT_RAID10			0x1BFBu

/* On-disk RC_LogicalDevice record — describes one RAID volume.  Layout
 * matches `struct RC_LogicalDevice` from rcblob.x86_64 (pahole-confirmed,
 * writer-confirmed via RC_BuildConfigMetadataFromMemory).  The struct is
 * `__packed__` in the original — fields are byte-packed without alignment.
 *
 * Lives in the config ring (LBA = RC_MetaData.config_ring_lba + N), tagged
 * by the leading u32 == RC_DST_LOGICAL_DEVICE (0x25BD).  ChunkSize at +0xAC
 * is observed to be 0 for RAID0 — the firmware uses a hardcoded default of
 * 2048 sectors (1 MiB) for that RAID level.  See docs/OPEN_QUESTIONS.md.
 *
 * We declare only the fields the driver consumes; the trailing reserved
 * area is implicit. */
#define RC_LD_DEVICES_OFFSET		0x68u	/* u32 — member count */
#define RC_LD_FIRSTCOUNT_OFFSET		0x6Cu	/* u32 */
#define RC_LD_SECONDCOUNT_OFFSET	0x70u	/* u32 */
#define RC_LD_CHUNKSIZE_OFFSET		0xACu	/* u32 — sectors, 0 for RAID0 */
#define RC_LD_CHUNKINDEX_OFFSET		0x110u	/* u32 — RAID0 stripe index, see
						 * rc_volume_chunk_sectors_for() */
#define RC_LD_ELEMENTOFFSET_OFFSET	0x04u	/* u32 — bytes from LD start to element array */
#define RC_LD_DEVICETYPE_OFFSET		0x0Cu	/* u32 — RC_LDT_* */
#define RC_LD_CAPACITY_OFFSET		0x50u	/* u64 — total volume sectors */
#define RC_LD_PACKETSIZE_OFFSET		0x90u	/* u32 — LD record size including element array */

/* On-disk RC_LogicalElement_LE — 64 bytes, one per member of a logical
 * device.  Element index is the per-member position in the stripe layout. */
#define RC_LE_BYTES			64u
#define RC_LE_DEVICEID_OFFSET		0x00u	/* u64 — matches RC_MetaData.DeviceId */
#define RC_LE_ALLOC_OFFSET_OFFSET	0x10u	/* u64 — sector offset of allocated region */
#define RC_LE_ALLOC_SIZE_OFFSET		0x18u	/* u64 — sector count of allocated region */
#define RC_LE_USERDATA_OFFSET_OFFSET	0x20u	/* u64 — sector offset of user-data region */
#define RC_LE_USERDATA_SIZE_OFFSET	0x28u	/* u64 — sector count of user-data region */

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
#define RC_NVME_ID_CTRL_MDTS		77	/* u8, max xfer = (2^MDTS) * CAP.MPSMIN pages */
#define RC_NVME_ID_CTRL_NN		516	/* u32, number of namespaces */
#define RC_NVME_ID_CTRL_VWC		525	/* u8, bit0 = volatile write cache present */

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

// Block major number (defined in rc_main.c)
extern int rc_major;

/* Called from rc_exit to tear down the assembled-RAID gendisk and free
 * its per-member DMA buffers (defined in rc_nvme.c). */
void rc_volume_teardown(void);

/* PCI-ID-based code-path dispatcher (rc_firmware.c). */
int rc_parse_firmware_capabilities(struct rc_adapter *adapter);

/* Hardware-layer stubs (rc_hw.c).  See file header for status. */
int          rc_hw_init(struct rc_adapter *adapter);
void         rc_hw_cleanup(struct rc_adapter *adapter);
irqreturn_t  rc_hw_interrupt_handler(int irq, void *dev_id);
int          rc_install_callbacks(struct rc_adapter *adapter, bool fast_path);

/* NVMe controller bring-up + I/O path (rc_nvme.c). */
int          rc_nvme_init_controller(struct rc_adapter *adapter);
void         rc_nvme_cleanup_controller(struct rc_adapter *adapter);
int          rc_nvme_reset_controller(struct rc_adapter *adapter);
int          rc_nvme_pm_suspend_adapter(struct rc_adapter *adapter);
int          rc_nvme_pm_resume_adapter(struct rc_adapter *adapter);
irqreturn_t  rc_nvme_irq(struct rc_adapter *adapter);
irqreturn_t  rc_nvme_admin_irq(int irq, void *dev_id);
irqreturn_t  rc_nvme_io_queue_irq(int irq, void *dev_id);

/* Sysfs / debugfs interfaces. */
int  rc_sysfs_create(struct rc_adapter *adapter);
void rc_sysfs_remove(struct rc_adapter *adapter);
int  rc_debugfs_init(void);
void rc_debugfs_cleanup(void);
int  rc_debugfs_create_adapter(struct rc_adapter *adapter);
void rc_debugfs_remove_adapter(struct rc_adapter *adapter);

#endif /* _RC_LINUX_H_ */
