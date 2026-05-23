// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — hardware-layer stubs
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * The NVMe (DEV_B000) code path does its hardware bring-up directly
 * from rc_nvme.c.  The AHCI variants (DEV_7905 / 0x43BD / 0x7916 /
 * 0x7917) are claimed in MODULE_DEVICE_TABLE but not yet
 * implemented; until they are, these stubs satisfy the call sites in
 * rc_bottom.c so the AHCI-mode probe falls through without doing
 * anything destructive.  When the AHCI path is built, replace these
 * with the real implementations.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

#include "rc_linux.h"

int rc_hw_init(struct rc_adapter *adapter)
{
	struct rc_hw_queue_context *hw = &adapter->hw;

	memset(hw, 0, sizeof(*hw));
	hw->owner     = adapter;
	hw->pdev      = adapter->pdev;
	hw->mmio_base = adapter->ctx.mmio_base;

	rc_printk(RC_INFO, "rc_hw_init: adapter %d (stub — NVMe path does its own bring-up in rc_nvme.c)\n",
		  adapter->instance);
	return 0;
}

void rc_hw_cleanup(struct rc_adapter *adapter)
{
	rc_printk(RC_INFO, "rc_hw_cleanup: adapter %d\n", adapter->instance);
}

/*
 * Registered as the MSI handler for every adapter at probe time but
 * not actually used yet — the NVMe path polls completions and
 * doesn't enable controller interrupts.  Returning IRQ_NONE marks
 * any spurious firing as not-ours so the kernel can move on.
 *
 * When interrupt-driven completion lands (see docs/STATUS.md), this
 * grows into the real handler.
 */
irqreturn_t rc_hw_interrupt_handler(int irq, void *dev_id)
{
	(void)irq;
	(void)dev_id;
	return IRQ_NONE;
}

/*
 * Callback installer — was the AHCI fast-path dispatcher hook.  No
 * AHCI fast-path implemented yet; succeeds silently so the firmware
 * dispatcher in rc_firmware.c can fall through.
 */
int rc_install_callbacks(struct rc_adapter *adapter, bool fast_path)
{
	(void)adapter;
	(void)fast_path;
	return 0;
}
