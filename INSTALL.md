# AMD RAID Driver Installation Guide

## 1. Setup Live USB and GRUB Configuration

### Create Live USB
1. Download Kubuntu 24.04 LTS ISO
2. Burn ISO to USB using Rufus on Windows only

### Modify GRUB Configuration

**For SATA drives (RAID mode):**
```bash
# Edit USB:/boot/grub/grub.cfg - append AHCI blacklist to the END:
# FROM: linux /casper/vmlinuz --- quiet splash
# TO:   linux /casper/vmlinuz --- quiet splash modprobe.blacklist=ahci,sata_ahci

# FROM: linux /casper/vmlinuz nomodeset --- quiet splash  
# TO:   linux /casper/vmlinuz nomodeset --- quiet splash modprobe.blacklist=ahci,sata_ahci
```

**For NVMe drives (RAID mode):**
```bash
# Edit USB:/boot/grub/grub.cfg - append AHCI blacklist to the END:
# FROM: linux /casper/vmlinuz --- quiet splash
# TO:   linux /casper/vmlinuz --- quiet splash modprobe.blacklist=ahci,sata_ahci

# FROM: linux /casper/vmlinuz nomodeset --- quiet splash  
# TO:   linux /casper/vmlinuz nomodeset --- quiet splash modprobe.blacklist=ahci,sata_ahci
```

**For Mixed SATA + NVMe (RAID mode):**
```bash
# Edit USB:/boot/grub/grub.cfg - append AHCI blacklist to the END:
# FROM: linux /casper/vmlinuz --- quiet splash
# TO:   linux /casper/vmlinuz --- quiet splash modprobe.blacklist=ahci,sata_ahci

# FROM: linux /casper/vmlinuz nomodeset --- quiet splash  
# TO:   linux /casper/vmlinuz nomodeset --- quiet splash modprobe.blacklist=ahci,sata_ahci
```

**Edit USB:/boot/grub/loopback.cfg:**
```bash
# For SATA drives - append AHCI blacklist to the END:
# FROM: linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci,sata_ahci

# For NVMe drives - append AHCI blacklist to the END:
# FROM: linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci,sata_ahci

# FROM: linux /casper/vmlinuz nomodeset iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz nomodeset iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci,sata_ahci
```

**Important:** 
- **SATA drives only**: Use `modprobe.blacklist=ahci,sata_ahci` (both AHCI drivers)
- **NVMe drives only**: Use `modprobe.blacklist=ahci,sata_ahci` (both AHCI drivers, DO NOT blacklist nvme)
- **Mixed SATA + NVMe**: Use `modprobe.blacklist=ahci,sata_ahci` (both AHCI drivers, DO NOT blacklist nvme)
- **"Append"** means add to the END of the line, not the beginning!
- **TRX50 Note**: The AMD RAID driver needs the NVMe driver loaded to work with NVMe drives
- **AMD Official Note**: AMD's official guide uses `modprobe.blacklist=ahci,nvme` but this driver needs NVMe support

## 2. BIOS Configuration

1. **Boot into BIOS/UEFI** (F2, F12, or Del during boot)
2. **Find SATA Configuration** (Advanced or Storage section)
3. **Change SATA Mode** from "AHCI" to "RAID"
4. **Configure RAID arrays** (RAID 0, RAID 1, etc.)
   - **SATA drives only**: Create RAID with SATA drives
   - **NVMe drives only**: Create RAID with NVMe drives  
   - **Mixed configuration**: Create RAID with both SATA and NVMe drives
5. **Save and exit BIOS**

## 3. Install Driver and Verify RAID

### Boot from Live USB
1. Boot from your modified USB
2. Open terminal

### Install Driver
```bash
# Update and install dependencies
sudo apt-get update
sudo apt install -y build-essential linux-headers-$(uname -r) git flex bison libssl-dev libelf-dev dwarves

# Clone and build driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo cp /sys/kernel/btf/vmlinux /usr/lib/modules/`uname -r`/build/

# Build driver
sudo make clean
sudo make simple

# Create AMD-compatible driver structure
sudo mkdir -p /dd
sudo cp rcraid.ko /dd/
sudo cp rcraid.ko /dd/rcraid_generic.ko  # Generic version for compatibility

# Install driver
sudo cp rcraid.ko /lib/modules/`uname -r`/kernel/drivers/scsi
sudo depmod -a
sudo modprobe rcraid

# Verify RAID arrays are detected
lsblk
```

## 4. Install Kubuntu

1. **Run Kubuntu installer** - it should now detect your RAID arrays
2. **Complete installation** normally
3. **DO NOT RESTART YET**

## 5. Post-Installation Setup

**Before rebooting, run these commands:**

```bash
# Configure GRUB to blacklist AHCI
sudo chroot /target sed -i.bak -e '/^GRUB_CMDLINE_LINUX_DEFAULT/ s/ *modprobe.blacklist=ahci// ; /^GRUB_CMDLINE_LINUX_DEFAULT/ s/"$/ modprobe.blacklist=ahci"/' /etc/default/grub
sudo chroot /target sed -i.bak -e '/\/boot\/vmlin/ s/ *modprobe.blacklist=ahci// ; /\/boot\/vmlin/ s/$/ modprobe.blacklist=ahci/' /boot/grub/grub.cfg

# Copy driver to installed system
sudo cp rcraid.ko /target/lib/modules/`uname -r`/kernel/drivers/scsi/rcraid.ko
sudo chroot /target depmod -a `uname -r`

# Update initramfs
sudo chroot /target cp -ap /boot/initrd.img-`uname -r` /boot/initrd.img-`uname -r`.bak
sudo chroot /target mkinitramfs -o /boot/initrd.img-`uname -r` `uname -r`

# Reboot
sudo reboot
```

## 6. Update Driver After Kernel Updates

When you update your kernel, reinstall the driver:

```bash
# Install new kernel first
sudo apt update && sudo apt upgrade

# Install build dependencies for new kernel
sudo apt install -y build-essential linux-headers-{new-kernel-version} flex bison libssl-dev libelf-dev dwarves

# Rebuild driver for new kernel
cd ~/amd-raid-driver
sudo make clean
sudo make simple

# Install driver
sudo cp rcraid.ko /lib/modules/{new-kernel-version}/kernel/drivers/scsi/rcraid.ko
sudo depmod -a {new-kernel-version}

# Update initramfs
sudo cp -ap /boot/initrd.img-{new-kernel-version} /boot/initrd.img-{new-kernel-version}.bak
sudo mkinitramfs -o /boot/initrd.img-{new-kernel-version} {new-kernel-version}

# Reboot
sudo reboot
```

## Troubleshooting

### Verify Driver is Working

**Check if driver is loaded:**
```bash
lsmod | grep rcraid
```
**Expected output:**
```
rcraid               5025792  0
```

**Check hardware detection:**
```bash
sudo dmesg | grep -i rcraid
```
**Expected output (TRX50/WRX80 platforms on Kubuntu 24.04 LTS):**
```
[  145.856658] rcraid: rc_init: AMD RAID Driver version 9.3.2.00255
[  145.856758] rcraid: rc_init: Based on Windows driver architecture
[  145.856793] rcraid: rc_bottom_init: initializing hardware layer
[  145.857350] rcraid: rc_bottom_probe: initializing hardware - VID=0x1022 DID=0x43bd
[  145.857400] rcraid: rc_bottom_probe: using 64-bit DMA
[  145.857450] rcraid: rc_bottom_probe: MSI enabled with 5 vectors
[  145.857500] rcraid: rc_bottom_probe: adapter 0 initialized successfully
[  145.857550] rcraid: rc_config_init: initializing configuration layer
[  145.857600] rcraid: rc_config_init: configuration layer initialized
[  145.857650] rcraid: rc_raid_init: initializing RAID layer
[  145.857700] rcraid: rc_raid_init: RAID layer initialized successfully
[  145.857750] rcraid: rc_init: AMD RAID Driver initialized successfully
[  145.857800] rcraid: rc_init: Found 1 adapters
[  146.899682] scsi host1: AMD-RAID
[  146.900915] scsi 1:0:24:0: Processor         AMD-RAID Configuration    V1.2 PQ: 0 ANSI: 5
```

**Check RAID controller detection:**
```bash
lspci | grep -i raid
```
**Expected output (TRX50/WRX80 platforms on Kubuntu 24.04 LTS):**
```
# Promontory SATA controller
4a:00.0 RAID bus controller: Advanced Micro Devices, Inc. [AMD] Device 43bd (rev 01)

# Bristol RAID mode (if present)
4b:00.0 RAID bus controller: Advanced Micro Devices, Inc. [AMD] Device 7905 (rev 01)

# Summit RAID mode (if present)  
4c:00.0 RAID bus controller: Advanced Micro Devices, Inc. [AMD] Device 7916 (rev 01)

# X570S chipset RAID mode (if present)
4d:00.0 RAID bus controller: Advanced Micro Devices, Inc. [AMD] Device 7917 (rev 01)

# NVMe RAID Bottom Device (if present)
4e:00.0 Non-Volatile memory controller: Advanced Micro Devices, Inc. [AMD] Device b000 (rev 01)
```

**AMD Official Specifications:**
- **Supported Processors**: 3rd Gen AMD Ryzen™ Threadripper Processors
- **Supported Chipsets**: TRX40/WRX80
- **Max Devices**: 14 total (SATA + NVMe)
- **Max NVMe Devices**: 10
- **Max Controllers**: 11

### Verify RAID Arrays are Detected

**Check SCSI hosts:**
```bash
ls -la /sys/class/scsi_host/
```
**Expected output:**
```
host1 -> ../../devices/pci0000:40/0000:40:03.6/0000:46:00.0/0000:47:0d.0/0000:4a:00.0/host1/scsi_host/host1
```

**Check configuration device:**
```bash
ls -la /dev/rcfg
```
**Expected output:**
```
crw-rw-rw- 1 root root 10, 60 Dec 12 10:30 /dev/rcfg
```

**Check for RAID arrays:**
```bash
lsblk
```
**Expected output (with working RAID):**
```
sdb    8:16   0   3.6T  0 disk
└─sdb1 8:17   0   3.6T  0 part
```

**If RAID arrays not showing:**
```bash
# Check SCSI devices
ls -la /sys/class/scsi_device/

# Check configuration device
ls -la /dev/rcfg

# Try rescanning
echo "- - -" | sudo tee /sys/class/scsi_host/host*/scan

# Check again
lsblk
```

### Driver-Specific Features

**Check driver parameters:**
```bash
# Check all driver parameters
cat /sys/module/rcraid/parameters/*

# Check debug level
cat /sys/module/rcraid/parameters/debug_level
```

**Check configuration device:**
```bash
# Read configuration device info
cat /dev/rcfg

# Check device permissions
ls -la /dev/rcfg
```

**Check MSI status:**
```bash
# Check MSI configuration
lspci -vvv | grep -i msi

# Check interrupt handling
cat /proc/interrupts | grep rcraid
```

### Common Issues

**Driver not loading:**
```bash
# Load manually
sudo modprobe rcraid

# Check for errors
sudo dmesg | tail -20
```

**RAID arrays not detected:**
- Check BIOS is set to RAID mode (not AHCI)
- Verify RAID arrays are configured in BIOS
- Check GRUB has correct blacklist parameters:
  - SATA: `modprobe.blacklist=ahci,sata_ahci`
  - NVMe: `modprobe.blacklist=ahci,sata_ahci` (DO NOT blacklist nvme - driver needs it)
- Verify SCSI host is created: `ls -la /sys/class/scsi_host/`
- Check configuration device: `ls -la /dev/rcfg`

**Driver-specific issues:**
- Verify MSI/MSI-X is working: `dmesg | grep -i msi`
- Check driver parameters: `cat /sys/module/rcraid/parameters/*`
- Verify configuration device: `ls -la /dev/rcfg`
- Check interrupt handling: `cat /proc/interrupts | grep rcraid`

**System won't boot:**
- Check BIOS is set to RAID mode
- Verify GRUB has correct blacklist parameters
- Check that driver is in initramfs

**Build fails:**
```bash
# Try the simple build target:
sudo make clean
sudo make simple

# If still failing, try with older kernel:
sudo apt install -y linux-source-6.8.0-* linux-headers-6.8.0-*
sudo make clean
sudo make KDIR=/usr/src/linux-source-6.8.0-*/build/
```