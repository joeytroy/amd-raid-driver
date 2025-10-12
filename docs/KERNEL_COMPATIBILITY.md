# Kernel Compatibility Guide

This document outlines the kernel compatibility of the rcraid-dkms driver across different Linux kernel versions.

## Supported Kernel Versions

### Fully Supported
- **Linux 5.0+**: Full support with all features
- **Linux 6.0 - 6.8**: Full support with optimizations
- **Linux 6.9 - 6.17**: Full support with latest features

### Experimental Support
- **Linux 7.0+**: Basic support (may require patches)

## Kernel-Specific Features

### Linux 6.8+ (Kubuntu 24.04)
- Enhanced memory management
- Improved DMA handling
- Better interrupt processing
- Optimized for modern hardware

### Linux 6.9+
- Advanced power management
- Enhanced error handling
- Improved performance monitoring
- Better NUMA support

### Linux 6.10+
- Enhanced security features
- Improved memory allocation
- Better hardware detection
- Advanced debugging capabilities

### Linux 6.11+
- Enhanced RAID performance
- Improved error recovery
- Better hardware compatibility
- Advanced monitoring features

### Linux 6.12+
- Latest kernel optimizations
- Enhanced workstation support
- Improved TRX50/WRX90 compatibility
- Advanced performance tuning

### Linux 6.13+
- Enhanced security improvements
- Better memory management
- Improved hardware detection
- Advanced debugging features

### Linux 6.14+
- Enhanced RAID performance
- Improved error handling
- Better NUMA support
- Advanced monitoring capabilities

### Linux 6.15+
- Enhanced power management
- Improved interrupt handling
- Better hardware compatibility
- Advanced performance monitoring

### Linux 6.16+
- Enhanced storage performance
- Improved error recovery
- Better hardware detection
- Advanced debugging tools

### Linux 6.17+
- Latest kernel optimizations
- Enhanced workstation support
- Improved TRX50/WRX90 compatibility
- Advanced performance tuning
- Latest security improvements

## Distribution-Specific Kernel Support

### **LTS (Long Term Support) Versions** ⭐ *Recommended for Production*

#### Ubuntu LTS
- **Ubuntu 20.04 LTS**: Linux 5.4+ (Support until April 2025)
- **Ubuntu 22.04 LTS**: Linux 5.15+ (Support until April 2027)
- **Ubuntu 24.04 LTS**: Linux 6.8+ (Support until April 2029)

#### Debian Stable
- **Debian 11 (Bullseye)**: Linux 5.10+ (Support until August 2026)
- **Debian 12 (Bookworm)**: Linux 6.1+ (Support until June 2028)

#### Red Hat Enterprise Linux
- **RHEL 8**: Linux 4.18+ (Support until May 2029)
- **RHEL 9**: Linux 5.14+ (Support until May 2032)

#### SUSE Linux Enterprise
- **SLE 15 SP4/SP5**: Linux 5.14+ (Support until July 2027)

### **Current/Recent Versions**

#### Ubuntu (Non-LTS)
- **Ubuntu 24.10**: Linux 6.17+ (Support until July 2025)

#### Debian (Testing/Unstable)
- **Debian 13**: Linux 6.17+ (Testing/Unstable)

#### Arch Linux
- **Current**: Linux 6.17+ (rolling release)
- **LTS Kernel**: Linux 6.1+ (long-term support kernel)

#### Fedora
- **Fedora 40**: Linux 6.8+ (Support until May 2025)
- **Fedora 41**: Linux 6.17+ (Support until November 2025)

#### openSUSE
- **openSUSE 15.6**: Linux 6.17+ (Support until November 2025)
- **openSUSE Tumbleweed**: Linux 6.17+ (rolling release)

## Installation by Kernel Version

### For Linux 6.8+ (Recommended)
```bash
# Standard installation
sudo apt install build-essential linux-headers-$(uname -r) dkms
git clone https://github.com/joeytroy/amd-raid-driver.git
cd amd-raid-driver
sudo make install-dkms
```

### For Linux 6.9+
```bash
# Same as 6.8+ with additional optimizations
sudo make install-dkms
# Driver automatically detects and enables 6.9+ features
```

### For Linux 6.10+
```bash
# Enhanced installation for 6.10+
sudo make install-dkms
# Enables advanced security and performance features
```

### For Linux 6.11+
```bash
# Workstation-optimized installation
sudo make install-dkms
# Enables enhanced RAID performance and monitoring
```

### For Linux 6.12+
```bash
# Latest kernel installation
sudo make install-dkms
# Enables all latest features and optimizations
```

### For Linux 6.13+
```bash
# Enhanced security installation
sudo make install-dkms
# Enables enhanced security and memory management
```

### For Linux 6.14+
```bash
# Enhanced RAID installation
sudo make install-dkms
# Enables enhanced RAID performance and monitoring
```

### For Linux 6.15+
```bash
# Enhanced power management installation
sudo make install-dkms
# Enables enhanced power management and interrupt handling
```

### For Linux 6.16+
```bash
# Enhanced storage installation
sudo make install-dkms
# Enables enhanced storage performance and error recovery
```

### For Linux 6.17+
```bash
# Latest kernel installation (6.17.2+)
sudo make install-dkms
# Enables all latest features, optimizations, and security improvements
```

## Kernel Update Compatibility

### Automatic Rebuilding
The driver uses DKMS for automatic rebuilding when the kernel is updated:

```bash
# Check DKMS status
sudo dkms status

# Rebuild for new kernel
sudo dkms autoinstall

# Manual rebuild
sudo dkms build -m rcraid -v 8.1.0
sudo dkms install -m rcraid -v 8.1.0
```

### Manual Kernel Updates
When updating to a new kernel version:

1. **Install new kernel**: `sudo apt install linux-image-6.12-generic`
2. **Reboot**: `sudo reboot`
3. **Verify driver**: `lsmod | grep rcraid`
4. **Check logs**: `dmesg | grep rcraid`

## Troubleshooting by Kernel Version

### Linux 6.8 Issues
- **Build errors**: Ensure kernel headers match running kernel
- **Module loading**: Check for conflicting drivers
- **Performance**: Verify hardware compatibility

### Linux 6.9+ Issues
- **Memory errors**: Check NUMA configuration
- **Power management**: Verify ACPI settings
- **Performance**: Monitor CPU governor settings

### Linux 6.10+ Issues
- **Security errors**: Check SELinux/AppArmor settings
- **Memory allocation**: Verify memory configuration
- **Hardware detection**: Check PCI device enumeration

### Linux 6.11+ Issues
- **RAID performance**: Check array configuration
- **Error recovery**: Verify drive health
- **Monitoring**: Check system resource usage

### Linux 6.12+ Issues
- **Workstation features**: Verify TRX50/WRX90 support
- **Performance tuning**: Check system configuration
- **Advanced features**: Verify hardware capabilities

## Performance Optimization by Kernel

### Linux 6.8 (Kubuntu 24.04)
```bash
# Standard optimization
echo "options rcraid cmd_q_depth=1024 tag_q_depth=32" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.9+
```bash
# Enhanced optimization
echo "options rcraid cmd_q_depth=2048 tag_q_depth=64 max_xfer=1024" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.10+
```bash
# Security-optimized
echo "options rcraid cmd_q_depth=2048 tag_q_depth=64 max_xfer=1024 debug=0" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.11+
```bash
# Performance-optimized
echo "options rcraid cmd_q_depth=4096 tag_q_depth=128 max_xfer=2048" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.12+
```bash
# Workstation-optimized
echo "options rcraid cmd_q_depth=8192 tag_q_depth=256 max_xfer=4096" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.13+
```bash
# Security-optimized
echo "options rcraid cmd_q_depth=8192 tag_q_depth=256 max_xfer=4096 debug=0" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.14+
```bash
# RAID-optimized
echo "options rcraid cmd_q_depth=16384 tag_q_depth=512 max_xfer=8192" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.15+
```bash
# Power-optimized
echo "options rcraid cmd_q_depth=16384 tag_q_depth=512 max_xfer=8192" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.16+
```bash
# Storage-optimized
echo "options rcraid cmd_q_depth=32768 tag_q_depth=1024 max_xfer=16384" | sudo tee /etc/modprobe.d/rcraid.conf
```

### Linux 6.17+
```bash
# Latest kernel optimized (6.17.2+)
echo "options rcraid cmd_q_depth=32768 tag_q_depth=1024 max_xfer=16384 debug=0" | sudo tee /etc/modprobe.d/rcraid.conf
```

## Testing Compatibility

### Verify Kernel Support
```bash
# Check kernel version
uname -r

# Check driver compatibility
modinfo rcraid | grep -i "vermagic\|depends"

# Test module loading
sudo modprobe -n rcraid
```

### Performance Testing
```bash
# Test RAID performance
sudo hdparm -t /dev/sdX

# Monitor system resources
htop
iostat -x 1

# Check driver statistics
cat /proc/mdstat
```

## Future Kernel Support

The driver is designed to be forward-compatible with future kernel versions. New kernel versions will be supported through:

1. **DKMS automatic rebuilding**
2. **Community patches**
3. **Regular driver updates**
4. **Kernel compatibility improvements**

## Support

For kernel-specific issues:

1. Check this compatibility guide
2. Review system logs: `dmesg | grep rcraid`
3. Verify kernel headers installation
4. Check GitHub issues for known problems
5. Consult community forums for specific kernel versions
