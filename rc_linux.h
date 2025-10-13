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
#if __has_include(<linux/scsi/scsi.h>)
#include <linux/scsi/scsi.h>
#include <linux/scsi/scsi_host.h>
#include <linux/scsi/scsi_device.h>
#define RC_SCSI_AVAILABLE 1
#else
#define RC_SCSI_AVAILABLE 0
#endif
#include <linux/blkdev.h>
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

// SCSI fallbacks if headers not available
#if !RC_SCSI_AVAILABLE
#define Scsi_Host                    void
#define scsi_device                  void
#define scsi_cmnd                    void
#define scsi_host_template           void
#define SCSI_SCAN_INITIAL            1
#endif

// Debug levels
#define RC_DEBUG                     0
#define RC_INFO                      1
#define RC_NOTE                      2
#define RC_WARN                      3
#define RC_ERROR                     4

// Debug macro
#define rc_printk(level, fmt, ...) \
    printk(KERN_INFO RC_DRIVER_NAME ": " fmt, ##__VA_ARGS__)

// Adapter structure (rcbottom equivalent)
struct rc_adapter {
    struct pci_dev *pdev;
    int instance;
    int irq;
    void __iomem *mmio_base;
    resource_size_t mmio_len;
    
    // MSI settings (from rcbottom.inf)
    int msi_enabled;
    int msi_vectors;
    
    // Power management (from rcbottom.inf)
    u32 enable_hipm;
    u32 enable_dipm;
    u32 hmb_policy;
    
    // Hardware info
    u16 vendor_id;
    u16 device_id;
    u8 revision;
    
    struct list_head list;
};

// Configuration structure (rccfg equivalent)
struct rc_config {
    struct miscdevice misc_dev;
    struct mutex lock;
    int initialized;
};

// RAID structure (rcraid equivalent)
struct rc_raid {
#if RC_SCSI_AVAILABLE
    struct Scsi_Host *host;
#endif
    struct rc_adapter *adapter;
    int initialized;
    int scsi_host_created;
};

// Global state
struct rc_global_state {
    struct rc_adapter adapters[RC_MAX_ADAPTERS];
    struct rc_config config;
    struct rc_raid raid;
    int num_adapters;
    int initialized;
    struct mutex lock;
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

// Interrupt handlers
irqreturn_t rc_interrupt_handler(int irq, void *dev_id);

#endif /* _RC_LINUX_H_ */
