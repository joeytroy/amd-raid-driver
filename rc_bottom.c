/****************************************************************************
 * AMD RAID Driver for Linux - rcbottom equivalent
 * Hardware initialization layer (TRX50 clean-room port)
 ****************************************************************************/

#include "rc_linux.h"

/*----------------------------------------------------------------------
 * PCI identity table (from Windows rcbottom.inf)
 *----------------------------------------------------------------------*/
static const struct pci_device_id rc_bottom_pci_tbl[] = {
    { PCI_DEVICE(0x1022, 0x43bd) }, /* AMD Promontory SATA RAID bottom device */
    { PCI_DEVICE(0x1022, 0xb000) }, /* AMD NVMe RAID Bottom Device (TRX50) */
    { 0, }
};

MODULE_DEVICE_TABLE(pci, rc_bottom_pci_tbl);

/*----------------------------------------------------------------------
 * Helpers modelling StorPort service slots
 *----------------------------------------------------------------------*/

static void rc_bottom_init_bar_templates(struct rc_adapter *adapter)
{
    /* Windows service slot +0x1B8 writes static doorbell templates.
     * Our port just caches BAR metadata for rcbottom -> rccfg -> rcraid.
     * This is now handled in rc_bottom_map_bars() based on device ID.
     */
}

/*----------------------------------------------------------------------
 * BAR discovery & mapping (FUN_140008f34 analogue)
 *----------------------------------------------------------------------*/

static int rc_bottom_map_bars(struct rc_adapter *adapter)
{
    struct pci_dev *pdev = adapter->pdev;
    struct rc_dev_context *ctx = &adapter->ctx;
    u32 i;

    for (i = 0; i < RC_MAX_BARS; i++) {
        struct rc_bar *bar = &ctx->bar[i];
        resource_size_t start = pci_resource_start(pdev, i);
        resource_size_t len = pci_resource_len(pdev, i);
        unsigned long flags = pci_resource_flags(pdev, i);

        bar->phys = start;
        bar->len = len;
        bar->flags = flags;
        bar->virt = NULL;

        if (!len)
            continue;

        if (!(flags & IORESOURCE_MEM)) {
            rc_printk(RC_INFO,
                      "rc_bottom: BAR%u non-memory flags=0x%lx ignored\n",
                      i, flags);
            continue;
        }

        bar->virt = pci_ioremap_bar(pdev, i);
        if (!bar->virt) {
            rc_printk(RC_ERROR,
                      "rc_bottom: BAR%u map failed (start=0x%llx len=0x%llx)\n",
                      i,
                      (unsigned long long)start,
                      (unsigned long long)len);
            return -ENOMEM;
        }

        rc_printk(RC_INFO,
                  "rc_bottom: BAR%u mapped start=0x%llx len=0x%llx virt=%p\n",
                  i,
                  (unsigned long long)start,
                  (unsigned long long)len,
                  bar->virt);
    }

    /* For device ID 0xb000 (NVMe RAID Bottom), MMIO is in BAR0 */
    /* For device ID 0x43bd (Promontory), MMIO is in BAR5 */
    if (adapter->device_id == 0xb000) {
        if (!ctx->bar[0].virt) {
            rc_printk(RC_ERROR, "rc_bottom: BAR0 missing for NVMe RAID Bottom device\n");
            return -ENODEV;
        }
        ctx->mmio_base = ctx->bar[0].virt;
        ctx->mmio_len = ctx->bar[0].len;
        ctx->mmio_phys = ctx->bar[0].phys;
        rc_printk(RC_INFO, "rc_bottom: Using BAR0 for NVMe RAID Bottom (phys=0x%llx len=0x%llx)\n",
                  (unsigned long long)ctx->mmio_phys, (unsigned long long)ctx->mmio_len);
    } else if (adapter->device_id == 0x43bd) {
    if (!ctx->bar[5].virt) {
            rc_printk(RC_ERROR, "rc_bottom: BAR5 missing for Promontory device\n");
            return -ENODEV;
        }
        ctx->mmio_base = ctx->bar[5].virt;
        ctx->mmio_len = ctx->bar[5].len;
        ctx->mmio_phys = ctx->bar[5].phys;
        rc_printk(RC_INFO, "rc_bottom: Using BAR5 for Promontory device (phys=0x%llx len=0x%llx)\n",
                  (unsigned long long)ctx->mmio_phys, (unsigned long long)ctx->mmio_len);
    } else {
        rc_printk(RC_ERROR, "rc_bottom: Unknown device ID 0x%04x\n", adapter->device_id);
        return -ENODEV;
    }

    return 0;
}

static void rc_bottom_unmap_bars(struct rc_adapter *adapter)
{
    struct rc_dev_context *ctx = &adapter->ctx;
    u32 i;

    for (i = 0; i < RC_MAX_BARS; i++) {
        if (ctx->bar[i].virt) {
            pci_iounmap(adapter->pdev, ctx->bar[i].virt);
            ctx->bar[i].virt = NULL;
        }
    }
}

/*----------------------------------------------------------------------
 * PCI probe / remove
 *----------------------------------------------------------------------*/

static int rc_bottom_enable_device(struct rc_adapter *adapter)
{
    struct pci_dev *pdev = adapter->pdev;
    int ret;

    ret = pci_enable_device_mem(pdev);
    if (ret) {
        rc_printk(RC_ERROR, "rc_bottom: pci_enable_device_mem failed (%d)\n", ret);
        return ret;
    }

    pci_set_master(pdev);

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) {
        rc_printk(RC_WARN, "rc_bottom: 64-bit DMA unsupported, falling back\n");
        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
        if (ret) {
            rc_printk(RC_ERROR, "rc_bottom: 32-bit DMA unsupported\n");
            return ret;
        }
    }

    return 0;
}

static void rc_bottom_disable_device(struct rc_adapter *adapter)
{
    pci_disable_device(adapter->pdev);
}

static int rc_bottom_setup_interrupts(struct rc_adapter *adapter)
{
    struct pci_dev *pdev = adapter->pdev;
    int ret;

    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    if (ret > 0) {
        adapter->irq_mode = RC_IRQ_MODE_MSI;
        adapter->irq_vector = pci_irq_vector(pdev, 0);
        rc_printk(RC_INFO, "rc_bottom: MSI vector %d assigned\n", adapter->irq_vector);
    } else {
        adapter->irq_mode = RC_IRQ_MODE_LEGACY;
        adapter->irq_vector = pdev->irq;
        rc_printk(RC_WARN, "rc_bottom: falling back to legacy IRQ %d\n", adapter->irq_vector);
    }

    return 0;
}

static void rc_bottom_release_interrupts(struct rc_adapter *adapter)
{
    struct pci_dev *pdev = adapter->pdev;

    if (adapter->irq_mode == RC_IRQ_MODE_MSI)
        pci_free_irq_vectors(pdev);
}

static int rc_bottom_request_irq(struct rc_adapter *adapter)
{
    int ret;

    ret = request_irq(adapter->irq_vector,
                      rc_hw_interrupt_handler,
                      adapter->irq_mode == RC_IRQ_MODE_LEGACY ? IRQF_SHARED : 0,
                      "rcraid-bottom",
                      adapter);
    if (ret) {
        rc_printk(RC_ERROR, "rc_bottom: request_irq failed (%d)\n", ret);
        return ret;
    }

    return 0;
}

static void rc_bottom_free_irq(struct rc_adapter *adapter)
{
    if (adapter->irq_vector >= 0)
        free_irq(adapter->irq_vector, adapter);
}

static struct rc_adapter *rc_bottom_alloc_adapter(struct pci_dev *pdev)
{
    struct rc_adapter *adapter;

    adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
    if (!adapter)
        return NULL;

    adapter->pdev = pdev;
    adapter->dev = &pdev->dev;
    adapter->vendor_id = pdev->vendor;
    adapter->device_id = pdev->device;
    adapter->subsystem_vendor = pdev->subsystem_vendor;
    adapter->subsystem_device = pdev->subsystem_device;
    adapter->revision = pdev->revision;
    adapter->enable_hipm = RC_HIPM_ENABLE;
    adapter->enable_dipm = RC_DIPM_DISABLE;
    adapter->hmb_policy = RC_HMB_POLICY_DEFAULT;
    adapter->irq_mode = RC_IRQ_MODE_NONE;
    adapter->irq_vector = -1;
    INIT_LIST_HEAD(&adapter->list_node);

    adapter->hw.owner = adapter;
    adapter->hw.pdev = pdev;

    return adapter;
}

static void rc_bottom_attach_adapter(struct rc_adapter *adapter)
{
    mutex_lock(&rc_state.lock);
    if (rc_state.num_adapters < RC_MAX_ADAPTERS) {
        rc_state.adapters[rc_state.num_adapters] = adapter;
        adapter->instance = rc_state.num_adapters;
        rc_state.num_adapters++;
        list_add_tail(&adapter->list_node, &rc_state.adapter_list);
    }
    mutex_unlock(&rc_state.lock);
}

static void rc_bottom_detach_adapter(struct rc_adapter *adapter)
{
    mutex_lock(&rc_state.lock);
    if (adapter->instance >= 0 &&
        adapter->instance < rc_state.num_adapters &&
        rc_state.adapters[adapter->instance] == adapter) {
        rc_state.adapters[adapter->instance] = NULL;
    }
    list_del_init(&adapter->list_node);
    mutex_unlock(&rc_state.lock);
}

static int rc_bottom_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct rc_adapter *adapter;
    int ret;

    rc_printk(RC_NOTE,
              "rc_bottom: probe VID=0x%04x DID=0x%04x SUB=0x%04x/0x%04x\n",
              pdev->vendor,
              pdev->device,
              pdev->subsystem_vendor,
              pdev->subsystem_device);

    if (pdev->vendor != RC_PCI_VID) {
        rc_printk(RC_ERROR, "rc_bottom: unexpected vendor ID 0x%04x\n", pdev->vendor);
        return -ENODEV;
    }
    
    if (pdev->device != RC_PCI_DID && pdev->device != 0xb000) {
        rc_printk(RC_ERROR, "rc_bottom: unexpected device ID 0x%04x\n", pdev->device);
        return -ENODEV;
    }

    adapter = rc_bottom_alloc_adapter(pdev);
    if (!adapter)
        return -ENOMEM;

    ret = rc_bottom_enable_device(adapter);
    if (ret)
        goto err_free_adapter;

    ret = rc_bottom_map_bars(adapter);
    if (ret)
        goto err_disable_device;

    rc_bottom_init_bar_templates(adapter);

    ret = rc_bottom_setup_interrupts(adapter);
    if (ret)
        goto err_unmap_bars;

    ret = rc_bottom_request_irq(adapter);
    if (ret)
        goto err_release_vectors;

    ret = rc_hw_init(adapter);
    if (ret)
        goto err_free_irq;

    /* Parse firmware capabilities to detect controller variant */
    ret = rc_parse_firmware_capabilities(adapter);
    if (ret) {
        rc_printk(RC_WARN, "rc_bottom: firmware capability parsing failed (non-fatal)\n");
        /* Continue anyway - will use safe dispatcher defaults */
    }

    ret = rc_queue_init(adapter);
    if (ret)
        goto err_hw_cleanup;

    ret = rc_activate_doorbells(adapter);
    if (ret)
        goto err_queue_cleanup;

    rc_bottom_attach_adapter(adapter);
    pci_set_drvdata(pdev, adapter);

    ret = rc_sysfs_create(adapter);
    if (ret)
        goto err_detach;

    ret = rc_debugfs_create_adapter(adapter);
    if (ret)
        rc_printk(RC_WARN, "rc_bottom: debugfs creation failed (non-fatal)\n");

	rc_printk(RC_NOTE, "rc_bottom: adapter %d ready\n", adapter->instance);

	// Initialize RAID layer on first adapter - DMA allocation is now fixed
	if (adapter->instance == 0) {
		ret = rc_raid_init();
		if (ret) {
			rc_printk(RC_ERROR, "rc_bottom: RAID layer initialization failed: %d\n", ret);
			// Non-fatal - adapter still usable for diagnostics
		}
	}

	return 0;

err_detach:
    rc_bottom_detach_adapter(adapter);

err_queue_cleanup:
    rc_queue_cleanup(adapter);
err_hw_cleanup:
    rc_hw_cleanup(adapter);
err_free_irq:
    rc_bottom_free_irq(adapter);
err_release_vectors:
    rc_bottom_release_interrupts(adapter);
err_unmap_bars:
    rc_bottom_unmap_bars(adapter);
err_disable_device:
    rc_bottom_disable_device(adapter);
err_free_adapter:
    kfree(adapter);
    return ret;
}

static void rc_bottom_remove(struct pci_dev *pdev)
{
    struct rc_adapter *adapter = pci_get_drvdata(pdev);

    if (!adapter)
        return;

    rc_printk(RC_NOTE, "rc_bottom: remove adapter %d\n", adapter->instance);

    rc_debugfs_remove_adapter(adapter);
    rc_sysfs_remove(adapter);
    rc_bottom_detach_adapter(adapter);
    rc_queue_cleanup(adapter);
    rc_bottom_free_irq(adapter);
    rc_hw_cleanup(adapter);
    rc_bottom_release_interrupts(adapter);
    rc_bottom_unmap_bars(adapter);
    rc_bottom_disable_device(adapter);
    kfree(adapter);
}

static struct pci_driver rc_bottom_driver = {
    .name = "rcbottom",
    .id_table = rc_bottom_pci_tbl,
    .probe = rc_bottom_probe,
    .remove = rc_bottom_remove,
};

int rc_bottom_init(void)
{
    mutex_init(&rc_state.lock);
    INIT_LIST_HEAD(&rc_state.adapter_list);
    rc_state.num_adapters = 0;
    rc_state.initialized = 1;
    return pci_register_driver(&rc_bottom_driver);
}

void rc_bottom_cleanup(void)
{
    pci_unregister_driver(&rc_bottom_driver);
}
