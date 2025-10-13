# TRX50 Support Changes

This document summarizes the changes made to add TRX50 platform support to the AMD RAID driver.

## Changes Made

### 1. Added Missing PCI Device IDs

**File: `src/rc_pci_ids.h`**
- Added `RC_PD_DID_BRISTOL` (0x7905) - PCIe AMD Bristol RAID mode
- Added `RC_PD_DID_SUMMIT` (0x7916) - PCIe AMD Summit RAID mode  
- Added `RC_PD_DID_X570S` (0x7917) - AMD X570S chipset RAID mode

### 2. Updated PCI Device Table

**File: `src/rc_init.c`**
- Added entries for Bristol, Summit, and X570S chipsets to `rcraid_id_tbl[]`
- All new devices use the `rc_ahci_version` driver data
- Maintains compatibility with existing Promontory and NVMe devices

### 3. Enhanced Power Management

**File: `src/rc_init.c`**
- Added `RC_HMBAllocationPolicy` variable for Host Memory Buffer allocation
- Added `RCRAID_DEFAULT_HMB` (0x00000002) default value
- Added HMB policy to sysctl table (`/proc/sys/dev/scsi/rcraid/hmb_policy`)
- Added HMB policy to proc filesystem (`/proc/scsi/rcraid/hmb_policy`)

### 4. Enhanced MSI Support

**File: `src/rc_init.c`**
- Updated `rc_ahci_init()` to try MSI-X first, then MSI, then legacy interrupts
- Enhanced `rc_ahci_shutdown()` to properly disable MSI-X/MSI
- Added proper interrupt type tracking (`ismsi` field: 0=legacy, 1=MSI, 2=MSI-X)

### 5. Enhanced Power Management Features

**File: `src/rc_init.c`**
- Added ASPM (Active State Power Management) support in `rc_ahci_start()`
- Added HMB allocation policy application
- Enhanced power management logging

### 6. Updated Driver Description

**File: `src/rc_init.c`**
- Updated module description to include supported chipsets: "AMD TRX50 Hybrid RAID Controller - SCSI + NVMe Support (Bristol/Summit/X570S)"

## Windows Driver Features Implemented

The following features from the Windows drivers have been implemented:

1. **PCI Device Support**: All device IDs from Windows drivers are now supported
2. **HMB Allocation Policy**: Host Memory Buffer allocation policy (default: 2)
3. **Enhanced MSI Support**: MSI-X and MSI interrupt support
4. **Power Management**: ASPM and enhanced power management features
5. **Configuration Interface**: HMB policy accessible via proc filesystem

## Supported Hardware

The driver now supports the following AMD chipsets for TRX50 platforms:

- **Promontory** (0x43BD) - SATA RAID controller
- **Bristol** (0x7905) - PCIe AMD Bristol RAID mode
- **Summit** (0x7916) - PCIe AMD Summit RAID mode
- **X570S** (0x7917) - AMD X570S chipset RAID mode
- **NVMe RAID Bottom** (0xB000) - NVMe RAID controllers

## Configuration

The driver can be configured via:

1. **Module parameters**: Standard Linux module parameters
2. **Proc filesystem**: `/proc/scsi/rcraid/` entries
3. **Sysctl**: `/proc/sys/dev/scsi/rcraid/` entries
4. **Misc device**: `/dev/rc_api` for advanced configuration

## Testing

To test the updated driver:

1. Build the driver: `make`
2. Load the module: `insmod rcraid.ko`
3. Check dmesg for initialization messages
4. Verify device detection: `lspci | grep -i raid`
5. Check proc entries: `ls /proc/scsi/rcraid/`

## Compatibility

- Maintains full backward compatibility with existing hardware
- No changes to existing API or configuration methods
- Enhanced features are optional and don't affect basic functionality
