# Installation Guide

This guide provides comprehensive installation instructions for the AMD RAID driver (rcraid-dkms) across different Linux distributions.

## Prerequisites

### Required Dependencies

Before installing the driver, ensure you have the following packages installed:

**Essential Dependencies:**
- `build-essential` - Compiler and build tools
- `linux-headers-$(uname -r)` - Kernel headers for current kernel
- `dkms` - Dynamic Kernel Module Support
- `git` - To clone the repository

**Distribution-Specific Commands:**
```bash
# Ubuntu/Debian (including Kubuntu 24.04)
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) dkms git

# Arch Linux
sudo pacman -S base-devel linux-headers dkms git

# RHEL/CentOS/Fedora
sudo yum groupinstall "Development Tools"
sudo yum install kernel-devel dkms git

# SUSE/openSUSE
sudo zypper install -t pattern devel_C_C++
sudo zypper install kernel-devel dkms git
```

## Quick Installation

> **💡 LTS Recommendation**: For production systems, we recommend using **Ubuntu 24.04 LTS** or **Debian 12** for the best long-term support and stability.

### Method 1: DKMS (Recommended)

```bash
# Install dependencies (if not already installed)
sudo apt install build-essential linux-headers-$(uname -r) dkms git

# Clone and install
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

### Method 2: Manual Installation

```bash
# Install dependencies (if not already installed)
sudo apt install build-essential linux-headers-$(uname -r) git

# Clone repository
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver

# Build and install
make
sudo make install
```

## Distribution-Specific Instructions

### Ubuntu/Debian (including Kubuntu 24.04)

```bash
# Install dependencies
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) dkms

# Install driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

### Kubuntu 24.04 Specific Notes

Kubuntu 24.04 uses Linux kernel 6.8, which is fully supported by this driver. The driver has been tested and verified to work with:

- **Kernel**: Linux 6.8.x (with support up to 6.17+)
- **Hardware**: All supported AMD chipsets including newer X670/B650/A620 and workstation chipsets (TRX50, WRX90)
- **Installation**: Use the standard Ubuntu/Debian installation method above
- **Future Updates**: DKMS automatically rebuilds the driver when kernel updates are installed

**Important**: Ensure your BIOS is configured for RAID mode before installing Linux.

### Workstation/Server Systems (TRX50, WRX90)

For high-end workstation and server systems with Threadripper PRO processors:

```bash
# Install dependencies (same as Ubuntu/Debian)
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) dkms

# Install driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

**TRX50/WRX90 Specific Notes:**
- Full support for Threadripper PRO 7000 and 9000 series
- Optimized for high-performance workstation workloads
- Supports multiple RAID arrays and high-speed storage
- Compatible with all major Linux distributions

### Arch Linux

```bash
# Install dependencies
sudo pacman -S base-devel linux-headers dkms

# Install driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

### Red Hat/CentOS/Fedora

```bash
# Install dependencies
sudo yum groupinstall "Development Tools"
sudo yum install kernel-devel dkms

# Install driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

### SUSE/openSUSE

```bash
# Install dependencies
sudo zypper install -t pattern devel_C_C++
sudo zypper install kernel-devel dkms

# Install driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

## BIOS Configuration

Before installing the driver, ensure your BIOS is configured for RAID mode:

1. Boot into BIOS/UEFI
2. Navigate to SATA configuration
3. Change SATA mode from "AHCI" to "RAID"
4. Save and exit BIOS
5. Boot into Linux

## Installation for Fresh Linux Installs

If you're installing Linux on a system with RAID configured, you'll need to modify the live USB to blacklist the AHCI driver:

### Step 1: Prepare Live USB

**Ubuntu/Debian Live USB:**
1. Create Ubuntu/Debian Live USB using the official ISO
2. Mount the USB and edit `/boot/grub/grub.cfg`:
   ```bash
   # Find the line starting with "linux" and add:
   modprobe.blacklist=ahci
   ```
   Example:
   ```
   linux /casper/vmlinuz file=/cdrom/preseed/ubuntu.seed boot=casper quiet splash modprobe.blacklist=ahci
   ```

**Arch Linux ISO:**
1. Boot Arch Linux ISO
2. At the boot prompt, add `modprobe.blacklist=ahci` to kernel parameters:
   ```
   vmlinuz-linux modprobe.blacklist=ahci
   ```

### Step 2: Install Linux

1. Boot from the modified live USB with RAID mode enabled in BIOS
2. Install Linux normally (the installer should detect your RAID arrays)
3. Complete the installation process

### Step 3: Install AMD RAID Driver

After Linux installation, install the AMD RAID driver:

```bash
# Install dependencies
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) dkms git

# Install the driver
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms

# Reboot to load the driver
sudo reboot
```

### Step 4: Verify Installation

After reboot, verify the driver is working:

```bash
# Check if driver is loaded
lsmod | grep rcraid

# Check RAID arrays
lsblk

# Check system logs
dmesg | grep -i raid
```

## Usage

### Loading the Driver

```bash
# Load the driver
sudo modprobe rcraid

# Check if loaded
lsmod | grep rcraid

# View driver information
modinfo rcraid
```

### Checking RAID Arrays

```bash
# List block devices
lsblk

# Check RAID status
cat /proc/mdstat

# View system logs
dmesg | grep -i raid
```

## Performance Optimization

### Standard Optimization

```bash
# Create optimized configuration
echo "options rcraid cmd_q_depth=2048 tag_q_depth=64 max_xfer=1024" | sudo tee /etc/modprobe.d/rcraid.conf

# Reload the module
sudo modprobe -r rcraid
sudo modprobe rcraid
```

### Workstation Optimization

For high-end workstations with TRX50/WRX90:

```bash
# Workstation-optimized settings
echo "options rcraid cmd_q_depth=8192 tag_q_depth=256 max_xfer=4096" | sudo tee /etc/modprobe.d/rcraid.conf

# Reload the module
sudo modprobe -r rcraid
sudo modprobe rcraid
```

## Troubleshooting

### Common Issues

1. **Driver won't load**: Ensure BIOS is set to RAID mode
2. **Module not found**: Check if kernel headers are installed
3. **Build errors**: Verify you have build tools installed
4. **RAID arrays not detected**: Check BIOS settings and cable connections

### Dependency Issues

**Missing build-essential:**
```bash
# Ubuntu/Debian
sudo apt install build-essential

# Arch Linux
sudo pacman -S base-devel

# RHEL/CentOS/Fedora
sudo yum groupinstall "Development Tools"

# SUSE/openSUSE
sudo zypper install -t pattern devel_C_C++
```

**Missing kernel headers:**
```bash
# Ubuntu/Debian
sudo apt install linux-headers-$(uname -r)

# Arch Linux
sudo pacman -S linux-headers

# RHEL/CentOS/Fedora
sudo yum install kernel-devel

# SUSE/openSUSE
sudo zypper install kernel-devel
```

**Missing DKMS:**
```bash
# Ubuntu/Debian
sudo apt install dkms

# Arch Linux
sudo pacman -S dkms

# RHEL/CentOS/Fedora
sudo yum install dkms

# SUSE/openSUSE
sudo zypper install dkms
```

**Missing Git:**
```bash
# Ubuntu/Debian
sudo apt install git

# Arch Linux
sudo pacman -S git

# RHEL/CentOS/Fedora
sudo yum install git

# SUSE/openSUSE
sudo zypper install git
```

### Debug Information

```bash
# Check driver status
dmesg | grep rcraid

# View module parameters
cat /sys/module/rcraid/parameters/*

# Check for conflicting modules
lsmod | grep -E "(ahci|sata)"
```

### Getting Help

- Check the [Issues](https://github.com/joeytroy/amd-raid-driver/issues) page
- Review system logs: `journalctl -u systemd-modules-load`
- Verify hardware compatibility

## Uninstallation

```bash
# Remove via DKMS
sudo dkms remove rcraid/8.1.0 --all

# Or manual removal
sudo make uninstall
```

## Development

### Building from Source

```bash
# Clone repository
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver

# Build for current kernel
make

# Build for specific kernel
make KVERS=5.15.0-generic

# Clean build artifacts
make clean
```

### Testing

```bash
# Test build
make test

# Run basic functionality tests
sudo make test-basic
```

## Additional Documentation

- **[Kernel Compatibility Guide](docs/KERNEL_COMPATIBILITY.md)**: Detailed kernel version compatibility information
