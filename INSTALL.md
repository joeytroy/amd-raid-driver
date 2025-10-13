# AMD RAID Driver Installation Guide

## 1. Setup Live USB and GRUB Configuration

### Create Live USB
1. Download Ubuntu 24.04 LTS ISO
2. Burn ISO to USB using Rufus, Balena Etcher, or `dd`

### Modify GRUB Configuration

**For SATA drives (AHCI):**
```bash
# Edit USB:/boot/grub/grub.cfg - append modprobe.blacklist=ahci to the END:
# FROM: linux /casper/vmlinuz --- quiet splash
# TO:   linux /casper/vmlinuz --- quiet splash modprobe.blacklist=ahci

# FROM: linux /casper/vmlinuz nomodeset --- quiet splash  
# TO:   linux /casper/vmlinuz nomodeset --- quiet splash modprobe.blacklist=ahci
```

**For NVMe drives:**
```bash
# Edit USB:/boot/grub/grub.cfg - append BOTH blacklist parameters to the END:
# FROM: linux /casper/vmlinuz --- quiet splash
# TO:   linux /casper/vmlinuz --- quiet splash modprobe.blacklist=ahci modprobe.blacklist=nvme

# FROM: linux /casper/vmlinuz nomodeset --- quiet splash  
# TO:   linux /casper/vmlinuz nomodeset --- quiet splash modprobe.blacklist=ahci modprobe.blacklist=nvme
```

**Edit USB:/boot/grub/loopback.cfg:**
```bash
# For SATA drives - append modprobe.blacklist=ahci to the END:
# FROM: linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci

# For NVMe drives - append BOTH blacklist parameters to the END:
# FROM: linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci modprobe.blacklist=nvme

# FROM: linux /casper/vmlinuz nomodeset iso-scan/filename=${iso_path} --- quiet splash
# TO:   linux /casper/vmlinuz nomodeset iso-scan/filename=${iso_path} --- quiet splash modprobe.blacklist=ahci modprobe.blacklist=nvme
```

**Important:** 
- **SATA drives only**: Use `modprobe.blacklist=ahci` only
- **NVMe drives only**: Use `modprobe.blacklist=ahci modprobe.blacklist=nvme` (both)
- **Mixed SATA + NVMe**: Use `modprobe.blacklist=ahci modprobe.blacklist=nvme` (both)
- **"Append"** means add to the END of the line, not the beginning!

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
sudo apt install build-essential dwarves git

# Clone and build driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo cp /sys/kernel/btf/vmlinux /usr/lib/modules/`uname -r`/build/
sudo make clean
sudo make

# Install driver
sudo cp rcraid.ko /lib/modules/`uname -r`/kernel/drivers/scsi
sudo depmod -a
sudo modprobe rcraid

# Verify RAID arrays are detected
lsblk
```

## 4. Install Ubuntu

1. **Run Ubuntu installer** - it should now detect your RAID arrays
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

# Rebuild driver for new kernel
cd ~/amd-raid-driver
sudo make clean
sudo make KDIR=/lib/modules/{new-kernel-version}/build/

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
**Expected output (TRX50 platforms):**
```
[  145.856658] <5>AMD, Inc. rcraid raid driver version 8.1.0 build_number 8.1.0-00039 built Oct 12 2025
[  145.856758] <5>rcraid_probe_one: vendor = 0x1022 device 0x43bd
[  145.856793] <5>rcraid_probe_one: Total adapters matched 1
[  145.857350] <5>rcraid: card 0: AMD, Inc. AHCI
[  145.857400] <5>rc_ahci_init: MSI-X enabled
[  145.857450] <5>rc_ahci_start: ASPM enabled
[  145.857500] <5>rc_ahci_start: HMB allocation policy set to 0x2
[  146.899682] scsi host1: AMD, Inc. AMD-RAID
[  146.900915] scsi 1:0:24:0: Processor         AMD-RAID Configuration    V1.2 PQ: 0 ANSI: 5
```

**Check RAID controller detection:**
```bash
lspci | grep -i raid
```
**Expected output (TRX50 platforms):**
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

### Verify RAID Arrays are Detected

**Check SCSI hosts:**
```bash
ls -la /sys/class/scsi_host/
```
**Expected output:**
```
host1 -> ../../devices/pci0000:40/0000:40:03.6/0000:46:00.0/0000:47:0d.0/0000:4a:00.0/host1/scsi_host/host1
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

# Try rescanning
echo "- - -" | sudo tee /sys/class/scsi_host/host*/scan

# Check again
lsblk
```

### TRX50-Specific Features

**Check HMB allocation policy:**
```bash
cat /proc/scsi/rcraid/hmb_policy
```
**Expected output:**
```
2
```

**Check power management settings:**
```bash
# Check all power management settings
ls /proc/scsi/rcraid/
cat /proc/scsi/rcraid/dipm
cat /proc/scsi/rcraid/hipm
cat /proc/scsi/rcraid/an
cat /proc/scsi/rcraid/ncq
cat /proc/scsi/rcraid/zpodd
```

**Configure HMB allocation policy:**
```bash
# Set HMB policy (requires root)
echo 2 | sudo tee /proc/scsi/rcraid/hmb_policy

# Or via sysctl
echo 2 | sudo tee /proc/sys/dev/scsi/rcraid/hmb_policy
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
  - SATA: `modprobe.blacklist=ahci`
  - NVMe: `modprobe.blacklist=ahci` (NVMe driver must load for RAID)

**TRX50-specific issues:**
- Verify MSI/MSI-X is working: `dmesg | grep -i msi`
- Check ASPM is enabled: `dmesg | grep -i aspm`
- Verify HMB policy is set: `cat /proc/scsi/rcraid/hmb_policy`

**System won't boot:**
- Check BIOS is set to RAID mode
- Verify GRUB has correct blacklist parameters
- Check that driver is in initramfs