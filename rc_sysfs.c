// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — sysfs entries
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Exposes per-adapter statistics under the standard PCI driver
 * sysfs tree.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

#include "rc_linux.h"

/*----------------------------------------------------------------------
 * Adapter attributes
 *----------------------------------------------------------------------*/

static ssize_t adapter_info_show(struct device *dev,
                                  struct device_attribute *attr,
                                  char *buf)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);

    if (!adapter)
        return -ENODEV;

    return sprintf(buf,
                   "Vendor: 0x%04x\n"
                   "Device: 0x%04x\n"
                   "Subsystem Vendor: 0x%04x\n"
                   "Subsystem Device: 0x%04x\n"
                   "Revision: 0x%02x\n"
                   "Instance: %d\n"
                   "IRQ Mode: %s\n"
                   "IRQ Vector: %d\n",
                   adapter->vendor_id,
                   adapter->device_id,
                   adapter->subsystem_vendor,
                   adapter->subsystem_device,
                   adapter->revision,
                   adapter->instance,
                   adapter->irq_mode == RC_IRQ_MODE_MSI ? "MSI" : "Legacy",
                   adapter->irq_vector);
}
static DEVICE_ATTR_RO(adapter_info);

static ssize_t queue_stats_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    struct rc_hw_queue_context *hw;
    ssize_t len = 0;

    if (!adapter)
        return -ENODEV;

    hw = &adapter->hw;

    len += sprintf(buf + len,
                   "Command Queue:\n"
                   "  Size: %u\n"
                   "  Head: %u\n"
                   "  Tail: %u\n"
                   "  DMA Address: 0x%llx\n"
                   "\n"
                   "Completion Queue:\n"
                   "  Size: %u\n"
                   "  Head: %u\n"
                   "  DMA Address: 0x%llx\n"
                   "\n"
                   "Statistics:\n"
                   "  IRQ Count: %d\n"
                   "  Command Sequence: %d\n"
                   "  Completions: %d\n"
                   "  Sync Completions: %d\n",
                   hw->cmd_queue_size,
                   hw->cmd_queue_head,
                   hw->cmd_queue_tail,
                   (unsigned long long)hw->cmd_queue_dma,
                   hw->comp_queue_size,
                   hw->comp_queue_head,
                   (unsigned long long)hw->comp_queue_dma,
                   atomic_read(&hw->irq_count),
                   atomic_read(&hw->cmd_sequence),
                   atomic_read(&hw->completion_count),
                   atomic_read(&hw->sync_completion_count));

    return len;
}
static DEVICE_ATTR_RO(queue_stats);

static ssize_t doorbell_state_show(struct device *dev,
                                    struct device_attribute *attr,
                                    char *buf)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    struct rc_doorbell_state *doorbell;

    if (!adapter)
        return -ENODEV;

    doorbell = &adapter->ctx.doorbell;

    return sprintf(buf,
                   "Adapter Active: %s\n"
                   "Fast Path Enabled: %s\n"
                   "Queue State: 0x%02x\n"
                   "Queue Mode: 0x%02x\n"
                   "Variants: 0x%04x 0x%04x 0x%04x 0x%04x\n",
                   doorbell->adapter_active ? "yes" : "no",
                   doorbell->fast_path_enabled ? "yes" : "no",
                   doorbell->queue_state,
                   doorbell->queue_mode,
                   doorbell->variant[0],
                   doorbell->variant[1],
                   doorbell->variant[2],
                   doorbell->variant[3]);
}
static DEVICE_ATTR_RO(doorbell_state);

static ssize_t bar_mapping_show(struct device *dev,
                                 struct device_attribute *attr,
                                 char *buf)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    struct rc_dev_context *ctx;
    ssize_t len = 0;
    u32 i;

    if (!adapter)
        return -ENODEV;

    ctx = &adapter->ctx;

    len += sprintf(buf + len, "Primary MMIO:\n");
    len += sprintf(buf + len, "  Physical: 0x%llx\n",
                   (unsigned long long)ctx->mmio_phys);
    len += sprintf(buf + len, "  Length: 0x%llx\n",
                   (unsigned long long)ctx->mmio_len);
    len += sprintf(buf + len, "  Virtual: %p\n\n", ctx->mmio_base);

    len += sprintf(buf + len, "BAR Mappings:\n");
    for (i = 0; i < RC_MAX_BARS; i++) {
        if (!ctx->bar[i].len)
            continue;

        len += sprintf(buf + len,
                       "  BAR%u: phys=0x%llx len=0x%llx flags=0x%x virt=%p\n",
                       i,
                       (unsigned long long)ctx->bar[i].phys,
                       (unsigned long long)ctx->bar[i].len,
                       ctx->bar[i].flags,
                       ctx->bar[i].virt);
    }

    return len;
}
static DEVICE_ATTR_RO(bar_mapping);

static ssize_t reset_store(struct device *dev,
                            struct device_attribute *attr,
                            const char *buf, size_t count)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    int ret;

    if (!adapter)
        return -ENODEV;

    if (adapter->ctx.ctrl_mode != RC_CTRL_MODE_NVME)
        return -ENOTSUPP;

    /* Accept "1" or "1\n" only — avoid acting on stray writes. */
    if (!((count == 1 && buf[0] == '1') ||
          (count == 2 && buf[0] == '1' && buf[1] == '\n')))
        return -EINVAL;

    ret = rc_nvme_reset_controller(adapter);
    return ret < 0 ? ret : (ssize_t)count;
}
static DEVICE_ATTR_WO(reset);

static ssize_t pending_requests_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    struct rc_hw_queue_context *hw;
    ssize_t len = 0;
    u32 count = 0;
    u32 i;

    if (!adapter)
        return -ENODEV;

    hw = &adapter->hw;

    // Count pending requests
    for (i = 0; i < RC_MAX_PENDING_REQUESTS; i++) {
        if (hw->pending_reqs[i].rq)
            count++;
    }

    len += sprintf(buf, "Pending Requests: %u / %u\n",
                   count, RC_MAX_PENDING_REQUESTS);

    return len;
}
static DEVICE_ATTR_RO(pending_requests);

static struct attribute *rc_adapter_attrs[] = {
    &dev_attr_adapter_info.attr,
    &dev_attr_queue_stats.attr,
    &dev_attr_doorbell_state.attr,
    &dev_attr_bar_mapping.attr,
    &dev_attr_pending_requests.attr,
    &dev_attr_reset.attr,
    NULL,
};

static const struct attribute_group rc_adapter_attr_group = {
    .name = "rcraid",
    .attrs = rc_adapter_attrs,
};

/*----------------------------------------------------------------------
 * Sysfs initialization & cleanup
 *----------------------------------------------------------------------*/

int rc_sysfs_create(struct rc_adapter *adapter)
{
    int ret;

    ret = sysfs_create_group(&adapter->pdev->dev.kobj,
                             &rc_adapter_attr_group);
    if (ret) {
        rc_printk(RC_ERROR, "rc_sysfs_create: failed to create sysfs group\n");
        return ret;
    }

    rc_printk(RC_INFO, "rc_sysfs_create: created sysfs entries for adapter %d\n",
              adapter->instance);

    return 0;
}

void rc_sysfs_remove(struct rc_adapter *adapter)
{
    sysfs_remove_group(&adapter->pdev->dev.kobj, &rc_adapter_attr_group);
    rc_printk(RC_INFO, "rc_sysfs_remove: removed sysfs entries for adapter %d\n",
              adapter->instance);
}

