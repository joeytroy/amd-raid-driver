# AMD RAID Driver Installation Guide

## 1. Setup Live USB and GRUB Configuration

### Create Live USB
1. Download Ubuntu 24.04 LTS ISO
2. Burn ISO to USB using Rufus, Balena Etcher, or `dd`

### Modify GRUB Configuration
**Edit USB:/boot/grub/grub.cfg:**
```bash
# Find these lines and add modprobe.blacklist=ahci:
linux /casper/vmlinuz file=/cdrom/preseed/ubuntu.seed boot=casper quiet splash modprobe.blacklist=ahci
linux /casper/vmlinuz nomodeset file=/cdrom/preseed/ubuntu.seed boot=casper quiet splash modprobe.blacklist=ahci
```

**Edit USB:/boot/grub/loopback.cfg:**
```bash
# Find these lines and add modprobe.blacklist=ahci:
linux /casper/vmlinuz iso-scan/filename=${iso_path} quiet splash modprobe.blacklist=ahci
linux /casper/vmlinuz nomodeset iso-scan/filename=${iso_path} quiet splash modprobe.blacklist=ahci
```

## 2. BIOS Configuration

1. **Boot into BIOS/UEFI** (F2, F12, or Del during boot)
2. **Find SATA Configuration** (Advanced or Storage section)
3. **Change SATA Mode** from "AHCI" to "RAID"
4. **Configure RAID arrays** (RAID 0, RAID 1, etc.)
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

**If RAID arrays not detected:**
```bash
# Check if driver loaded
lsmod | grep rcraid

# Check hardware detection
sudo dmesg | grep -i rcraid
lspci | grep -i raid

# Rescan devices
echo "- - -" | sudo tee /sys/class/scsi_host/host*/scan
```

**If system won't boot:**
- Check BIOS is set to RAID mode
- Verify GRUB has `modprobe.blacklist=ahci` parameter
- Check that driver is in initramfs