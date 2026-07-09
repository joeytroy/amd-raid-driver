// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — PCI probe + per-adapter bring-up
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Implements the per-adapter equivalent of AMD's Windows `rcbottom`
 * PnP driver: PCI enable, MSI setup, BAR mapping, NVMe controller
 * bring-up dispatch.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

#include "rc_linux.h"

/*----------------------------------------------------------------------
 * Subsystem-vendor allowlist (safety filter)
 *
 * The PCI device IDs we claim (especially 1022:b000) cover ALL drives
 * the AMD RAID firmware exposes through the chipset — including drives
 * that are NOT part of any RAID volume (e.g. an OS-only NVMe sitting on
 * the same chipset).  Without a filter, a freshly loaded rcraid will
 * happily bind to such an unrelated drive, reset its NVMe controller in
 * rc_nvme_init_controller(CC.EN=0), and brick the running OS.
 *
 * Setting safe_subsys_vendor=<u16> at insmod time restricts probe to
 * devices whose PCI subsystem_vendor matches.  Default 0 means no
 * filter (legacy behaviour).  Recommended on multi-drive boxes:
 *   insmod rcraid.ko safe_subsys_vendor=0xc0a9   (Micron/Crucial)
 *----------------------------------------------------------------------*/
static unsigned int rc_safe_subsys_vendor;
module_param_named(safe_subsys_vendor, rc_safe_subsys_vendor, uint, 0444);
MODULE_PARM_DESC(safe_subsys_vendor,
                 "If non-zero, only bind to PCI devices whose subsystem_vendor matches this u16. "
                 "Use to keep rcraid off the OS drive on chipsets that expose every NVMe as 1022:b000.");

/*----------------------------------------------------------------------
 * PCI identity table.  Mirrors the five entries in AMD's
 * Windows rcbottom.inf (9.3.2/9.3.3): four SATA-RAID device IDs
 * (class 0x0104, AHCI-style path) plus the NVMe RAID Bottom device
 * (class 0x0108, NVMe path).
 *
 * Status of each path:
 *   0xB000  NVMe RAID Bottom    — fully implemented and validated
 *   0x43BD  Promontory SATA     — claimed, AHCI path is stub
 *   0x7905  older SATA RAID     — claimed, AHCI path is stub
 *   0x7916  older SATA RAID     — claimed, AHCI path is stub
 *   0x7917  X570S-era SATA RAID — claimed, AHCI path is stub
 *
 * Binding all five so modalias matching works for any board that
 * AMD's own driver would handle.  When the AHCI path is built out
 * (see docs/STATUS.md), no PCI-table change will be needed.
 *----------------------------------------------------------------------*/
static const struct pci_device_id rc_bottom_pci_tbl[] = {
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_BRISTOL)          },
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_PROMONTORY)       },
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_SUMMIT)           },
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_X570S)            },
    { PCI_DEVICE(RC_PD_VID_AMD, RC_PD_DID_NVME_RAID_BOTTOM) },
    { 0, }
};

MODULE_DEVICE_TABLE(pci, rc_bottom_pci_tbl);

/*----------------------------------------------------------------------
 * Helpers modelling StorPort service slots
 *----------------------------------------------------------------------*/

static void rc_bottom_init_bar_templates(struct rc_adapter *adapter)
{
    /* Windows service slot +0x188 writes static doorbell templates.
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
    /* Any other NVMe-class function also uses BAR0 — the NVMe spec puts
     * the controller registers there (MLBAR/MUBAR), and rc_classify_device
     * already routes class 0x0108 to the NVMe path.  This is what lets the
     * QEMU test rig bind virtual NVMe controllers (1b36:0010) at runtime
     * via new_id — see scripts/qemu-test/. */
    if (adapter->device_id == 0xb000 ||
        (adapter->pdev->class >> 8) == 0x0108) {
        if (!ctx->bar[0].virt) {
            rc_printk(RC_ERROR, "rc_bottom: BAR0 missing for NVMe-class device\n");
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
    int desired = RC_NVME_IO_QUEUE_TARGET + 1;  /* 1 admin + N I/O */
    /* pre_vectors=1 = vector 0 (admin) doesn't get auto-affinity.
     * The remaining N I/O vectors get spread across CPUs by the
     * kernel's irq spreading algorithm — completions then naturally
     * land on the same CPU set that dispatched them, which
     * blk_mq_map_hw_queues mirrors when picking which hctx to use
     * per CPU. */
    struct irq_affinity affd = { .pre_vectors = 1 };
    int ret;

    /* Prefer MSI-X with N+1 vectors so we can pin one vector per
     * NVMe I/O queue, with per-vector CPU affinity (step 3c).  Fall
     * back to MSI/INTx with 1 vector if MSI-X is unavailable; the
     * I/O path then shares vector 0 (single-queue effective even if
     * multiple queues are created).  adapter->irq_vector always
     * holds vector 0 — used for the admin handler. */
    ret = pci_alloc_irq_vectors_affinity(pdev, 1, desired,
                                         PCI_IRQ_MSIX | PCI_IRQ_MSI |
                                         PCI_IRQ_INTX | PCI_IRQ_AFFINITY,
                                         &affd);
    if (ret < 1) {
        rc_printk(RC_ERROR,
                  "rc_bottom: pci_alloc_irq_vectors_affinity failed (%d)\n",
                  ret);
        return ret < 0 ? ret : -ENODEV;
    }

    /* Remember how many vectors we actually got: the NVMe layer caps its
     * I/O queue count at nr_irq_vectors - 1 so it never binds a CQ to a
     * vector that doesn't exist (which would fail queue creation and
     * roll back to zero I/O queues instead of degrading). */
    adapter->nr_irq_vectors = ret;

    if (pdev->msix_enabled) {
        adapter->irq_mode = RC_IRQ_MODE_MSIX;
        adapter->irq_vector = pci_irq_vector(pdev, 0);
        rc_printk(RC_INFO, "rc_bottom: MSI-X enabled with %d vectors (admin=%d)\n",
                  ret, adapter->irq_vector);
    } else if (pdev->msi_enabled) {
        adapter->irq_mode = RC_IRQ_MODE_MSI;
        adapter->irq_vector = pci_irq_vector(pdev, 0);
        rc_printk(RC_INFO, "rc_bottom: MSI with %d vector(s) (no MSI-X%s)\n",
                  ret, ret == 1 ? " — I/O will share vector 0" : "");
    } else {
        adapter->irq_mode = RC_IRQ_MODE_LEGACY;
        adapter->irq_vector = pci_irq_vector(pdev, 0);
        rc_printk(RC_WARN, "rc_bottom: falling back to legacy IRQ %d (I/O will share it)\n",
                  adapter->irq_vector);
    }
    return 0;
}

static void rc_bottom_release_interrupts(struct rc_adapter *adapter)
{
    struct pci_dev *pdev = adapter->pdev;

    /* pci_free_irq_vectors handles all of MSI-X, MSI, and INTx that
     * pci_alloc_irq_vectors set up.  No-op on RC_IRQ_MODE_NONE. */
    if (adapter->irq_mode != RC_IRQ_MODE_NONE)
        pci_free_irq_vectors(pdev);
}

static int rc_bottom_request_irq(struct rc_adapter *adapter)
{
    int ret;

    /* IRQF_SHARED is only needed for legacy INTx (multiple devices on one
     * line).  MSI / MSI-X are dedicated per-vector — sharing flag would
     * fail the kernel's strict mode check. */
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
    adapter->nr_irq_vectors = 0;
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

    if (rc_safe_subsys_vendor &&
        pdev->subsystem_vendor != (u16)rc_safe_subsys_vendor) {
        rc_printk(RC_NOTE,
                  "rc_bottom: skipping %s — subsystem_vendor 0x%04x != safe_subsys_vendor 0x%04x\n",
                  pci_name(pdev),
                  pdev->subsystem_vendor,
                  (u16)rc_safe_subsys_vendor);
        return -ENODEV;
    }

    /* The PCI core has already matched against rc_bottom_pci_tbl[]; no
     * additional vendor/device gating needed here.  ID dispatch into
     * NVMe vs AHCI vs stub paths happens in rc_parse_firmware_capabilities. */

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

    /* Classify the PCI device (sets adapter->ctx.ctrl_mode) and, for
     * NVMe controllers, run the controller boot sequence (rc_nvme.c).
     * For AHCI variants the hardware bring-up will land in rc_hw_init
     * once the AHCI path is implemented; for now rc_hw_init is a stub. */
    ret = rc_parse_firmware_capabilities(adapter);
    if (ret) {
        rc_printk(RC_WARN, "rc_bottom: firmware capability parsing failed (%d)\n", ret);
        /* Continue — non-NVMe paths are stubs and won't do harm. */
    }

    ret = rc_hw_init(adapter);
    if (ret)
        goto err_free_irq;

    rc_bottom_attach_adapter(adapter);
    pci_set_drvdata(pdev, adapter);

    ret = rc_sysfs_create(adapter);
    if (ret)
        goto err_detach;

    ret = rc_debugfs_create_adapter(adapter);
    if (ret)
        rc_printk(RC_WARN, "rc_bottom: debugfs creation failed (non-fatal)\n");

    rc_printk(RC_NOTE, "rc_bottom: adapter %d ready\n", adapter->instance);
    return 0;

err_detach:
    rc_bottom_detach_adapter(adapter);
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
    rc_bottom_free_irq(adapter);
    rc_hw_cleanup(adapter);
    rc_bottom_release_interrupts(adapter);
    rc_bottom_unmap_bars(adapter);
    rc_bottom_disable_device(adapter);
    kfree(adapter);
}

/*----------------------------------------------------------------------
 * Power management (S3 suspend-to-RAM and S4 hibernate).
 *
 * For every member the PCI core calls our .suspend with the system going
 * down and .resume on the way back up.  We delegate to the NVMe layer to
 * disable / re-enable the controller; the PCI core handles config-space
 * save/restore and the D-state transition around us.
 *
 * The SIMPLE_DEV_PM_OPS macro maps both .suspend and .freeze (S4 snapshot
 * phase) to our suspend handler, and .resume / .thaw / .restore to our
 * resume handler — so the same pair covers both S3 and S4.
 *
 * AHCI / stub members are no-op (NVMe is the only path with real I/O).
 *----------------------------------------------------------------------*/
/* Why the NVMe-only gate: the AHCI variants (0x43BD, 0x7905, 0x7916, 0x7917)
 * are claimed by this driver but their I/O path is a stub today.  They have
 * an MMIO BAR but it points at AHCI registers, not NVMe ones.  Running
 * rc_nvme_pm_* would write to RC_NVME_REG_INTMS / RC_NVME_REG_CC offsets
 * inside the AHCI register window — scribbling random bytes into HBA state.
 * Returning 0 here is the right no-op until those code paths exist. */
static int rc_bottom_pm_suspend(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);

    if (!adapter || adapter->ctx.ctrl_mode != RC_CTRL_MODE_NVME)
        return 0;

    return rc_nvme_pm_suspend_adapter(adapter);
}

static int rc_bottom_pm_resume(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct rc_adapter *adapter = pci_get_drvdata(pdev);

    if (!adapter || adapter->ctx.ctrl_mode != RC_CTRL_MODE_NVME)
        return 0;

    return rc_nvme_pm_resume_adapter(adapter);
}

static SIMPLE_DEV_PM_OPS(rc_bottom_pm_ops,
                          rc_bottom_pm_suspend,
                          rc_bottom_pm_resume);

static struct pci_driver rc_bottom_driver = {
    .name = "rcbottom",
    .id_table = rc_bottom_pci_tbl,
    .probe = rc_bottom_probe,
    .remove = rc_bottom_remove,
    .driver.pm = &rc_bottom_pm_ops,
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
