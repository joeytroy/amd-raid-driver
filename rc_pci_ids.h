// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — PCI device-ID table
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * IDs identified through clean-room reverse engineering of AMD's
 * Windows .inf files and .sys binaries.  See docs/RE_METHODOLOGY.md.
 */

#ifndef _RC_PCI_IDS_H_
#define _RC_PCI_IDS_H_

/*
 * Canonical AMD-RAID PCI identifiers from the Windows driver's
 * rcbottom.inf.  AMD ships exactly these five Device IDs.  The much
 * larger CPU/chipset support matrix on AMD's website
 * (X870E/X870/B850/B840/X670E/X670/B650E/B650/A620; Ryzen 7000+;
 * Threadripper 7000/9000; Ryzen AI 300 / AI Max 300) all expose one
 * of these five PCI IDs depending on the controller variant — the
 * IDs themselves are not chipset-specific.
 *
 * Class codes:
 *   CC_0104 = Mass Storage / RAID  → AHCI-style command path
 *   CC_0108 = Mass Storage / NVMe  → NVMe command path
 */
#define RC_PD_VID_AMD                0x1022

/* SATA RAID variants — class code 0x0104, AHCI-style path.
 *
 * NOTE: only RC_PD_DID_PROMONTORY is in the pci_device_id bind table
 * (rc_bottom.c).  Bristol/Summit/X570S have no BAR-mapping case yet, so
 * claiming them just stole the device from other drivers and then failed
 * probe.  The defines stay here as documentation for the future AHCI
 * path. */
#define RC_PD_DID_BRISTOL            0x7905  /* older SATA RAID — NOT claimed (no code path) */
#define RC_PD_DID_PROMONTORY         0x43BD  /* Promontory SATA RAID */
#define RC_PD_DID_SUMMIT             0x7916  /* older SATA RAID — NOT claimed (no code path) */
#define RC_PD_DID_X570S              0x7917  /* X570S-era SATA RAID — NOT claimed (no code path) */

/* NVMe RAID variant — class code 0x0108, NVMe command path.
 * Used by TRX50/WRX90 (Threadripper 7000/9000) and by the X870/B850
 * consumer chipsets' NVMe RAID controllers. */
#define RC_PD_DID_NVME_RAID_BOTTOM   0xB000

/* PCI class codes the driver checks at probe / firmware-dispatch time. */
#define RC_PCI_CLASS_SCSI_ADAPTER    0x010400
#define RC_PCI_CLASS_NVME_CONTROLLER 0x010800

/*
 * Settings extracted from AMD's INF files; not all are wired into the
 * Linux driver yet (most apply only to the AHCI path that's not on
 * the live B000 code path).  Kept here so the values are documented
 * in one place and can be referenced when the AHCI path is built.
 */
#define RC_HIPM_ENABLE               0xFFFFFFFF
#define RC_DIPM_DISABLE              0x00000000
#define RC_HMB_POLICY_DEFAULT        0x00000002

/* MSI: 9.3.2's INF limited to 5 vectors; 9.3.3 raised to 16.  We request
 * RC_NVME_IO_QUEUE_TARGET + 1 = 5 (1 admin + 4 I/O queues, the Windows
 * hard cap) in rc_bottom_setup_interrupts — within both limits, so
 * nothing changes across releases unless the queue target is raised. */
#define RC_MSI_SUPPORTED             1
#define RC_MSI_MESSAGE_LIMIT_9_3_2   5
#define RC_MSI_MESSAGE_LIMIT_9_3_3   16
#define RC_MSI_AFFINITY_POLICY       5

#define RC_NUMBER_OF_REQUESTS        254
#define RC_BUS_TYPE                  0x00000008
#define RC_ENABLE_AN                 0x00000000
#define RC_ENABLE_ZPODD              0x00000000
#define RC_ENABLE_NCQ                0x00000001
#define RC_STORAGE_FEATURES          0x1
#define RC_DRIVER_PARAMETER          "CSMI=Limited;"

#define ENABLE_CLUSTERING            1

#endif /* _RC_PCI_IDS_H_ */
