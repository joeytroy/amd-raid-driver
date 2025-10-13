/****************************************************************************
 * AMD RAID Driver for Linux - rcbottom equivalent
 * Hardware initialization layer based on rcbottom.inf
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"

// PCI device table (from rcbottom.inf)
static const struct pci_device_id rc_bottom_pci_tbl[] = {
    // AMD Bristol RAID mode
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_BRISTOL), .class = RC_PCI_CLASS_SCSI_ADAPTER },
    // AMD Promontory SATA controller
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_PROMONTORY), .class = RC_PCI_CLASS_SCSI_ADAPTER },
    // AMD Summit RAID mode  
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_SUMMIT), .class = RC_PCI_CLASS_SCSI_ADAPTER },
    // AMD X570S chipset RAID mode
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_X570S), .class = RC_PCI_CLASS_SCSI_ADAPTER },
    // AMD NVMe RAID Bottom Device
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_NVME_RAID_BOTTOM), .class = RC_PCI_CLASS_NVME_CONTROLLER },
    { 0, }
};

MODULE_DEVICE_TABLE(pci, rc_bottom_pci_tbl);

// rcbottom initialization (hardware layer)
static int rc_bottom_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct rc_adapter *adapter;
    int err;
    
    rc_printk(RC_NOTE, "rc_bottom_probe: initializing hardware - VID=0x%04x DID=0x%04x\n",
              id->vendor, id->device);
    
    // Allocate adapter structure
    adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
    if (!adapter) {
        rc_printk(RC_ERROR, "rc_bottom_probe: failed to allocate adapter\n");
        return -ENOMEM;
    }
    
    adapter->pdev = pdev;
    adapter->vendor_id = id->vendor;
    adapter->device_id = id->device;
    
    // Enable PCI device
    err = pci_enable_device(pdev);
    if (err) {
        rc_printk(RC_ERROR, "rc_bottom_probe: failed to enable PCI device\n");
        goto err_free_adapter;
    }
    
    rc_printk(RC_NOTE, "rc_bottom_probe: PCI device enabled successfully\n");
    
    // Set DMA mask (64-bit preferred, 32-bit fallback)
    if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(64))) {
        if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(32))) {
            rc_printk(RC_ERROR, "rc_bottom_probe: failed to set DMA mask\n");
            err = -ENODEV;
            goto err_disable_device;
        }
        rc_printk(RC_NOTE, "rc_bottom_probe: using 32-bit DMA\n");
    } else {
        rc_printk(RC_NOTE, "rc_bottom_probe: using 64-bit DMA\n");
    }
    
    // Debug: Check all BARs
    rc_printk(RC_NOTE, "rc_bottom_probe: checking PCI resources:\n");
    for (int bar = 0; bar < 6; bar++) {
        unsigned long start = pci_resource_start(pdev, bar);
        unsigned long len = pci_resource_len(pdev, bar);
        unsigned long flags = pci_resource_flags(pdev, bar);
        rc_printk(RC_NOTE, "  BAR %d: start=0x%lx len=0x%lx flags=0x%lx\n", 
                  bar, start, len, flags);
    }
    
    // Map MMIO space - try different BARs for AMD RAID controllers
    adapter->mmio_base = NULL;
    for (int bar = 0; bar < 6; bar++) {
        if (pci_resource_len(pdev, bar) > 0) {
            rc_printk(RC_NOTE, "rc_bottom_probe: trying to map BAR %d\n", bar);
            adapter->mmio_base = pci_iomap(pdev, bar, 0);
            if (adapter->mmio_base) {
                adapter->mmio_len = pci_resource_len(pdev, bar);
                rc_printk(RC_NOTE, "rc_bottom_probe: mapped MMIO space BAR %d (len=%llu)\n", 
                          bar, (unsigned long long)adapter->mmio_len);
                break;
            } else {
                rc_printk(RC_NOTE, "rc_bottom_probe: failed to map BAR %d\n", bar);
            }
        }
    }
    
    if (!adapter->mmio_base) {
        rc_printk(RC_ERROR, "rc_bottom_probe: failed to map any MMIO space - trying without MMIO\n");
        // Continue without MMIO for now - some RAID controllers might not need it
        adapter->mmio_base = NULL;
        adapter->mmio_len = 0;
        rc_printk(RC_NOTE, "rc_bottom_probe: continuing without MMIO mapping\n");
    }
    
    // Setup MSI/MSI-X (from rcbottom.inf)
    if (pci_alloc_irq_vectors(pdev, 1, RC_MSI_MESSAGE_LIMIT, PCI_IRQ_MSIX | PCI_IRQ_MSI) > 0) {
        adapter->msi_enabled = 1;
        adapter->msi_vectors = pci_irq_vector(pdev, 0);
        rc_printk(RC_NOTE, "rc_bottom_probe: MSI enabled with vector %d\n", 
                  adapter->msi_vectors);
    } else {
        adapter->msi_enabled = 0;
        adapter->irq = pdev->irq;
        rc_printk(RC_NOTE, "rc_bottom_probe: using legacy interrupt %d\n", adapter->irq);
    }
    
    // Set power management parameters (from rcbottom.inf)
    adapter->enable_hipm = RC_HIPM_ENABLE;
    adapter->enable_dipm = RC_DIPM_DISABLE;
    adapter->hmb_policy = RC_HMB_POLICY_DEFAULT;
    
    // Request interrupt
    err = request_irq(adapter->msi_enabled ? adapter->msi_vectors : adapter->irq,
                      rc_interrupt_handler, IRQF_SHARED, RC_DRIVER_NAME, adapter);
    if (err) {
        rc_printk(RC_ERROR, "rc_bottom_probe: failed to request interrupt\n");
        goto err_unmap;
    }
    
    // Add to global adapter list
    mutex_lock(&rc_state.lock);
    if (rc_state.num_adapters < RC_MAX_ADAPTERS) {
        adapter->instance = rc_state.num_adapters;
        rc_state.adapters[rc_state.num_adapters] = *adapter;
        rc_state.num_adapters++;
        rc_printk(RC_NOTE, "rc_bottom_probe: adapter %d initialized successfully\n", 
                  adapter->instance);
    } else {
        rc_printk(RC_ERROR, "rc_bottom_probe: maximum adapters exceeded\n");
        err = -ENODEV;
        mutex_unlock(&rc_state.lock);
        goto err_free_irq;
    }
    mutex_unlock(&rc_state.lock);
    
    pci_set_drvdata(pdev, adapter);
    return 0;
    
err_free_irq:
    free_irq(adapter->msi_enabled ? adapter->msi_vectors : adapter->irq, adapter);
err_unmap:
    pci_iounmap(pdev, adapter->mmio_base);
err_disable_device:
    pci_disable_device(pdev);
err_free_adapter:
    kfree(adapter);
    return err;
}

static void rc_bottom_remove(struct pci_dev *pdev)
{
    struct rc_adapter *adapter = pci_get_drvdata(pdev);
    
    if (adapter) {
        rc_printk(RC_NOTE, "rc_bottom_remove: removing adapter %d\n", adapter->instance);
        
        free_irq(adapter->msi_enabled ? adapter->msi_vectors : adapter->irq, adapter);
        pci_iounmap(pdev, adapter->mmio_base);
        pci_disable_device(pdev);
        kfree(adapter);
    }
}

// PCI driver structure
static struct pci_driver rc_bottom_driver = {
    .name = "rcbottom",
    .id_table = rc_bottom_pci_tbl,
    .probe = rc_bottom_probe,
    .remove = rc_bottom_remove,
};

// rcbottom module initialization
int rc_bottom_init(void)
{
    rc_printk(RC_NOTE, "rc_bottom_init: initializing hardware layer\n");
    
    mutex_init(&rc_state.lock);
    rc_state.num_adapters = 0;
    
    return pci_register_driver(&rc_bottom_driver);
}

void rc_bottom_cleanup(void)
{
    rc_printk(RC_NOTE, "rc_bottom_cleanup: cleaning up hardware layer\n");
    pci_unregister_driver(&rc_bottom_driver);
}
