/****************************************************************************
 * AMD RAID Driver for Linux - PCI Device IDs
 * Based on Windows driver specifications
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 ****************************************************************************/

#ifndef _RC_PCI_IDS_H_
#define _RC_PCI_IDS_H_

// AMD Vendor ID
#define RC_PD_VID_AMD                0x1022

// AMD RAID Device IDs (from rcbottom.inf)
#define RC_PD_DID_BRISTOL            0x7905  // AMD Bristol RAID mode
#define RC_PD_DID_PROMONTORY         0x43BD  // AMD Promontory SATA controller  
#define RC_PD_DID_SUMMIT             0x7916  // AMD Summit RAID mode
#define RC_PD_DID_X570S              0x7917  // AMD X570S chipset RAID mode
#define RC_PD_DID_NVME_RAID_BOTTOM   0xB000  // AMD NVMe RAID Bottom Device

// PCI Class Codes (from Windows drivers)
#define RC_PCI_CLASS_SCSI_ADAPTER    0x010400  // SCSIAdapter
#define RC_PCI_CLASS_NVME_CONTROLLER 0x010800  // Non-Volatile memory controller

// Power Management Settings (from rcbottom.inf)
#define RC_HIPM_ENABLE               0xFFFFFFFF
#define RC_DIPM_DISABLE              0x00000000
#define RC_HMB_POLICY_DEFAULT        0x00000002

// MSI Settings (from rcbottom.inf)
#define RC_MSI_SUPPORTED             1
#define RC_MSI_MESSAGE_LIMIT         5
#define RC_MSI_AFFINITY_POLICY       5  // Spread messages across all processors

// RAID Configuration (from rcraid.inf)
#define RC_NUMBER_OF_REQUESTS        254
#define RC_BUS_TYPE                  0x00000008
#define RC_ENABLE_AN                 0x00000000
#define RC_ENABLE_ZPODD              0x00000000
#define RC_ENABLE_NCQ                0x00000001
#define RC_STORAGE_FEATURES          0x1
#define RC_DRIVER_PARAMETER          "CSMI=Limited;"

#endif /* _RC_PCI_IDS_H_ */
