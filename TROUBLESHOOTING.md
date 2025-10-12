# Troubleshooting Guide

This guide helps resolve common build and installation issues with the AMD RAID driver.

## Build Issues

### Error: "No such file or directory" for src/Makefile

**Problem:**
```
/usr/src/linux-headers-6.14.0-27-generic/scripts/Makefile.build:41: /home/kubuntu/amd-raid-driver/src/Makefile: No such file or directory
```

**Solution:**
This error occurs when the kernel build system can't find the source files. The driver doesn't use a separate Makefile in the src/ directory.

1. **Use the build script:**
   ```bash
   ./build.sh
   ```

2. **Or build manually:**
   ```bash
   make clean
   make
   ```

3. **If still failing, check kernel headers:**
   ```bash
   # Ubuntu/Debian
   sudo apt install linux-headers-$(uname -r)
   
   # Arch Linux
   sudo pacman -S linux-headers
   
   # RHEL/CentOS
   sudo yum install kernel-devel
   ```

### Error: "Compiler differs from the one used to build the kernel"

**Problem:**
```
warning: the compiler differs from the one used to build the kernel
The kernel was built by: x86_64-linux-gnu-gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
You are using:           gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
```

**Solution:**
This is usually just a warning and won't prevent the build. However, to fix it:

1. **Install the exact compiler version:**
   ```bash
   # Ubuntu/Debian
   sudo apt install gcc-13
   
   # Make sure it's the default
   sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100
   ```

2. **Or ignore the warning** - the build should still work.

### Error: "No rule to make target"

**Problem:**
```
make[4]: *** No rule to make target 'src/rcblob.x86_64.o', needed by 'rcraid.o'. Stop.
```

**Solution:**
This error occurs when the build system can't find the rcblob binary files. This has been fixed in the latest version.

1. **Update to the latest version** of the driver
2. **Use the build script:**
   ```bash
   ./build.sh
   ```

3. **If still failing, clean and rebuild:**
   ```bash
   make clean
   make
   ```

### Error: "Conflicting types" or "overriding recipe"

**Problem:**
```
src/rc_msg.c:222:1: error: conflicting types for 'rc_vprintf'
Makefile:246: warning: overriding recipe for target 'rcblob.x86_64.o'
```

**Solution:**
These errors occur due to function signature mismatches and duplicate Makefile rules. This has been fixed in the latest version.

1. **Update to the latest version** of the driver
2. **Clean and rebuild:**
   ```bash
   make clean
   make
   ```

3. **If still failing, use the build script:**
   ```bash
   ./build.sh
   ```

### Error: "Unknown type name"

**Problem:**
```
src/rc.h:157:97: error: unknown type name 'rc_thread_buf_t'
```

**Solution:**
This error occurs when a function prototype references a type that hasn't been defined yet. This has been fixed in the latest version.

1. **Update to the latest version** of the driver
2. **Clean and rebuild:**
   ```bash
   make clean
   make
   ```

3. **If still failing, use the build script:**
   ```bash
   ./build.sh
   ```

### Error: "No such file or directory" for src/Makefile

**Problem:**
```
/usr/src/linux-headers-6.14.0-27-generic/scripts/Makefile.build:41: /home/kubuntu/amd-raid-driver/src/Makefile: No such file or directory
```

**Solution:**
This error occurs when the kernel build system can't find the source files. The driver doesn't use a separate Makefile in the src/ directory.

1. **Use the build script:**
   ```bash
   ./build.sh
   ```

2. **Or build manually:**
   ```bash
   make clean
   make
   ```

### Error: "Kernel headers not found"

**Problem:**
```
Kernel headers for 6.14.0-27-generic not found
```

**Solution:**
Install the correct kernel headers for your running kernel:

```bash
# Check your kernel version
uname -r

# Ubuntu/Debian
sudo apt update
sudo apt install linux-headers-$(uname -r)

# Arch Linux
sudo pacman -S linux-headers

# RHEL/CentOS/Fedora
sudo yum install kernel-devel
# or
sudo dnf install kernel-devel

# SUSE/openSUSE
sudo zypper install kernel-devel
```

## Installation Issues

### Error: "Module not found" after installation

**Problem:**
```bash
modprobe: FATAL: Module rcraid not found
```

**Solution:**
1. **Check if module was installed:**
   ```bash
   find /lib/modules/$(uname -r) -name "rcraid.ko"
   ```

2. **Update module dependencies:**
   ```bash
   sudo depmod -a
   ```

3. **Reinstall the driver:**
   ```bash
   sudo make install
   # or
   sudo make install-dkms
   ```

### Error: "Permission denied" during installation

**Problem:**
```
Permission denied
```

**Solution:**
Run installation commands with sudo:

```bash
sudo make install
# or
sudo make install-dkms
```

### Error: "DKMS not found"

**Problem:**
```
DKMS not found, falling back to manual install
```

**Solution:**
Install DKMS for automatic kernel rebuilds:

```bash
# Ubuntu/Debian
sudo apt install dkms

# Arch Linux
sudo pacman -S dkms

# RHEL/CentOS
sudo yum install dkms

# SUSE
sudo zypper install dkms
```

## Runtime Issues

### Error: "Device not found" or "No RAID arrays detected"

**Problem:**
Driver loads but doesn't detect RAID arrays.

**Solution:**
1. **Check BIOS settings:**
   - Ensure SATA mode is set to "RAID" (not AHCI)
   - Enable RAID controller in BIOS

2. **Check for conflicting drivers:**
   ```bash
   lsmod | grep -E "(ahci|sata)"
   ```

3. **Blacklist conflicting drivers:**
   ```bash
   echo "blacklist ahci" | sudo tee /etc/modprobe.d/blacklist-ahci.conf
   echo "blacklist sata_ahci" | sudo tee -a /etc/modprobe.d/blacklist-ahci.conf
   ```

4. **Reboot and try again**

### Error: "System becomes unresponsive" after loading driver

**Problem:**
System hangs or becomes unresponsive when loading the driver.

**Solution:**
1. **Check hardware compatibility:**
   - Ensure your AMD chipset is supported
   - Check if RAID arrays are properly configured in BIOS

2. **Try with different parameters:**
   ```bash
   sudo modprobe rcraid debug=0 cmd_q_depth=512
   ```

3. **Check system logs:**
   ```bash
   dmesg | grep -i rcraid
   journalctl -u systemd-modules-load
   ```

## Performance Issues

### Slow RAID performance

**Problem:**
RAID arrays perform slower than expected.

**Solution:**
1. **Optimize driver parameters:**
   ```bash
   echo "options rcraid cmd_q_depth=2048 tag_q_depth=64 max_xfer=1024" | sudo tee /etc/modprobe.d/rcraid.conf
   ```

2. **Reload the module:**
   ```bash
   sudo modprobe -r rcraid
   sudo modprobe rcraid
   ```

3. **Check RAID array health:**
   ```bash
   cat /proc/mdstat
   ```

## Getting Help

If you're still experiencing issues:

1. **Check system logs:**
   ```bash
   dmesg | grep -i rcraid
   journalctl -u systemd-modules-load
   ```

2. **Verify hardware compatibility:**
   - Check if your AMD chipset is supported
   - Verify BIOS RAID configuration

3. **Create an issue on GitHub** with:
   - Your distribution and kernel version
   - Complete error messages
   - System logs
   - Hardware information

4. **Check the kernel compatibility guide:**
   - See `docs/KERNEL_COMPATIBILITY.md`

## Common Commands

```bash
# Build the driver
./build.sh

# Install with DKMS
sudo make install-dkms

# Check if driver is loaded
lsmod | grep rcraid

# Check module information
modinfo rcraid

# Check RAID arrays
lsblk
cat /proc/mdstat

# Check system logs
dmesg | grep -i rcraid

# Uninstall driver
sudo make uninstall
```
