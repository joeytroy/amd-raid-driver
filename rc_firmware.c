/****************************************************************************
 * AMD RAID Driver for Linux - Firmware Capability Parsing
 *
 * This file picks the hardware code path (AHCI vs NVMe vs stub) based on the
 * PCI device ID. It mirrors the Windows driver's FUN_140007d40, with one big
 * simplification: the Windows function does its selection via wcsncmp on the
 * PnP HW-ID string + a class-code byte from the StorPort service +0x3f0; on
 * Linux the same information is just sitting in pdev->device and pdev->class,
 * so there is no need to recreate the WDF class-bind dance (service +0x418).
 *
 * See docs/GHIDRA_FINDINGS_2026.md for the full reasoning. The previous
 * version of this file was a stub that always defaulted to AHCI mode, which
 * is why DEV_B000 never came up.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#include "rc_linux.h"
#include "rc_pci_ids.h"

static enum rc_ctrl_mode rc_classify_device(struct pci_dev *pdev)
{
	/* Windows fast-path: explicit per-device matches all go to ahci.c. */
	switch (pdev->device) {
	case RC_PD_DID_BRISTOL:
	case RC_PD_DID_PROMONTORY:
	case RC_PD_DID_SUMMIT:
	case RC_PD_DID_X570S:
		return RC_CTRL_MODE_AHCI;
	}

	/* Anything else with PCI class "NVM Express controller" (0x010802) goes
	 * to nvme.c on Windows. That's how DEV_B000 (CC_0108) is picked up. */
	if ((pdev->class >> 8) == 0x0108)
		return RC_CTRL_MODE_NVME;

	/* Unknown vendor-specific RAID class → stub callbacks (controller will
	 * load but issue no I/O). Matches Windows iVar7=99 path. */
	return RC_CTRL_MODE_STUB;
}

int rc_parse_firmware_capabilities(struct rc_adapter *adapter)
{
	struct rc_doorbell_state *doorbell = &adapter->ctx.doorbell;
	enum rc_ctrl_mode mode;
	int ret;

	mode = rc_classify_device(adapter->pdev);
	adapter->ctx.ctrl_mode = mode;

	rc_printk(RC_NOTE,
		  "rc_parse_firmware_capabilities: PCI %04x:%04x class=0x%06x → mode=%d (%s)\n",
		  adapter->pdev->vendor, adapter->pdev->device,
		  adapter->pdev->class, mode,
		  mode == RC_CTRL_MODE_AHCI ? "AHCI" :
		  mode == RC_CTRL_MODE_NVME ? "NVMe" :
		  mode == RC_CTRL_MODE_STUB ? "stub" : "unknown");

	/* Variant fields are only meaningful on the AHCI fast path where the
	 * controller reports per-device firmware caps. NVMe controllers carry
	 * their own capability registers (CAP/VS); we read those in
	 * rc_nvme_init_controller(). For the stub path nothing matters. */
	doorbell->variant[0] = 0;
	doorbell->variant[1] = 0;
	doorbell->variant[2] = 0;
	doorbell->variant[3] = 0;
	doorbell->queue_state = 0;
	doorbell->queue_mode = 0;
	doorbell->capability_word = 0;

	switch (mode) {
	case RC_CTRL_MODE_NVME:
		/* Bring the controller through reset → admin queue → enable. */
		ret = rc_nvme_init_controller(adapter);
		if (ret) {
			rc_printk(RC_ERROR,
				  "rc_parse_firmware_capabilities: NVMe init failed (%d)\n",
				  ret);
			return ret;
		}
		/* No AHCI-style callback table; the NVMe path dispatches through
		 * its own helpers. Mark queue_mode so the rest of the driver
		 * knows not to ring AHCI doorbells. */
		doorbell->queue_mode = 0x01;
		return 0;

	case RC_CTRL_MODE_AHCI:
		ret = rc_install_callbacks(adapter, false);
		if (ret)
			rc_printk(RC_WARN,
				  "rc_parse_firmware_capabilities: AHCI callback install failed (%d)\n",
				  ret);
		return ret;

	case RC_CTRL_MODE_STUB:
	default:
		rc_printk(RC_WARN,
			  "rc_parse_firmware_capabilities: device not recognised, using stub callbacks\n");
		return rc_install_callbacks(adapter, false);
	}
}
