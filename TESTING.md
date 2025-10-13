# AMD RAID Driver Testing Guide

This guide will help you test the AMD RAID driver on your TRX50 system and gather the information needed for further development.

## 🎯 **Testing Objectives**

1. **Validate Driver Loading** - Ensure the driver loads without errors
2. **Hardware Detection** - Confirm AMD RAID controller is detected
3. **Basic Functionality** - Test in-memory I/O operations
4. **Gather Hardware Info** - Collect data needed for real hardware implementation

## 📋 **Pre-Testing Checklist**

- [ ] Boot from LiveCD (Ubuntu/Kubuntu recommended)
- [ ] Ensure internet connection for package installation
- [ ] Have the driver source code available
- [ ] Note down your current kernel version

## 🚀 **Step-by-Step Testing Process**

### **Step 1: System Information Gathering**

```bash
# Get basic system info
uname -a
cat /proc/version
lsb_release -a

# Check kernel modules
lsmod | head -20
```

**📝 Record this information:**
- Kernel version: `_________________`
- Distribution: `_________________`
- Architecture: `_________________`

### **Step 2: Hardware Detection**

```bash
# Look for AMD RAID controllers
echo "=== AMD RAID Controllers ==="
lspci | grep -i amd
lspci | grep -i raid
lspci | grep -i "1022:"

# Check specific device IDs we support
echo "=== Supported Device IDs ==="
lspci -nn | grep -E "(1022:7905|1022:43BD|1022:7916|1022:7917|1022:B000)"

# Check if any are already claimed by other drivers
echo "=== Driver Binding Status ==="
lspci -k | grep -A2 -B2 -E "(1022:43BD|1022:7905|1022:7916|1022:7917|1022:B000)"

# Check PCI configuration (concise)
echo "=== PCI Configuration ==="
lspci -nn | grep -E "(1022:43BD|1022:7905|1022:7916|1022:7917|1022:B000)"
```

**📝 Record this information:**
- AMD devices found: `_________________`
- Device IDs detected: `_________________`
- Current driver binding: `_________________`
- PCI configuration details: `_________________`

### **Step 3: Driver Build and Installation**

```bash
# Navigate to driver directory
cd /path/to/amd-raid-driver

# Install build dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

# Build the driver
make clean
make

# Check build result
ls -la *.ko
```

**📝 Record this information:**
- Build success/failure: `_________________`
- Any build errors: `_________________`
- Generated .ko file: `_________________`

### **Step 4: Driver Loading**

```bash
# Load the driver
sudo insmod rcraid.ko

# Check loading status
echo "=== Module Loading Status ==="
lsmod | grep rcraid
sudo dmesg | tail -20

# Check for driver messages
echo "=== Driver Messages ==="
sudo dmesg | grep -i rcraid
sudo dmesg | grep -i "adapter.*initialized"
sudo dmesg | grep -i "found.*raid.*array"
```

**📝 Record this information:**
- Module loaded successfully: `_________________`
- Driver messages: `_________________`
- Any error messages: `_________________`

### **Step 5: Hardware Detection by Driver**

```bash
# Check if driver detected hardware
echo "=== Hardware Detection ==="
sudo dmesg | grep -i "rc_bottom_probe"
sudo dmesg | grep -i "initializing hardware"
sudo dmesg | grep -i "VID.*DID"

# Check for block device creation
echo "=== Block Device Creation ==="
ls -la /dev/rcraid*
lsblk | grep rcraid
```

**📝 Record this information:**
- Hardware detected by driver: `_________________`
- Vendor/Device IDs found: `_________________`
- Block devices created: `_________________`

### **Step 6: Basic I/O Testing**

```bash
# Test basic I/O operations
echo "=== Basic I/O Testing ==="

# Check if device exists
if [ -b /dev/rcraid0 ]; then
    echo "Device /dev/rcraid0 exists"
    
    # Get device info
    sudo fdisk -l /dev/rcraid0
    sudo blockdev --getsize64 /dev/rcraid0
    sudo blockdev --getss /dev/rcraid0
    
    # Test read/write
    echo "Testing write operation..."
    echo "Hello RAID!" | sudo dd of=/dev/rcraid0 bs=512 count=1
    
    echo "Testing read operation..."
    sudo dd if=/dev/rcraid0 bs=512 count=1 2>/dev/null | hexdump -C
    
    echo "I/O test completed"
else
    echo "Device /dev/rcraid0 not found"
fi
```

**📝 Record this information:**
- Device size: `_________________`
- Sector size: `_________________`
- I/O operations successful: `_________________`

### **Step 7: Advanced Testing (Optional)**

```bash
# Test with filesystem operations
echo "=== Filesystem Testing ==="
if [ -b /dev/rcraid0 ]; then
    # Create filesystem
    sudo mkfs.ext4 -F /dev/rcraid0
    
    # Mount and test
    sudo mkdir -p /mnt/test
    sudo mount /dev/rcraid0 /mnt/test
    
    # Test file operations
    echo "Test file content" | sudo tee /mnt/test/test.txt
    sudo cat /mnt/test/test.txt
    
    # Check mount
    df -h | grep rcraid
    mount | grep rcraid
    
    # Cleanup
    sudo umount /mnt/test
    echo "Filesystem test completed"
fi
```

### **Step 8: Debug Information Collection**

```bash
# Collect comprehensive debug info
echo "=== Debug Information ==="

# Module information
modinfo rcraid.ko

# Driver status
cat /proc/modules | grep rcraid

# PCI device details (concise)
lspci -nn -s $(lspci | grep -E "1022:43BD" | head -1 | cut -d' ' -f1)

# Interrupt information
cat /proc/interrupts | grep -i rcraid

# Kernel logs
sudo dmesg | grep -i rcraid > rcraid_dmesg.log
echo "Driver logs saved to rcraid_dmesg.log"
```

### **Step 9: Cleanup**

```bash
# Unload the driver
sudo rmmod rcraid

# Verify cleanup
lsmod | grep rcraid
sudo dmesg | tail -10
```

## 📊 **Information to Report Back**

Please provide the following information:

### **System Information**
- [ ] Kernel version and distribution
- [ ] Architecture (x86_64, etc.)

### **Hardware Detection**
- [ ] AMD RAID controllers found by `lspci`
- [ ] Specific device IDs detected
- [ ] Current driver binding status
- [ ] PCI configuration details

### **Driver Behavior**
- [ ] Build success/failure and any errors
- [ ] Module loading success/failure
- [ ] Driver messages from `dmesg`
- [ ] Block device creation status
- [ ] I/O operation results

### **Debug Files**
- [ ] `rcraid_dmesg.log` file
- [ ] Any error messages or warnings
- [ ] PCI configuration output

## 🔍 **What We're Looking For**

**Success Indicators:**
- Driver loads without errors
- AMD hardware is detected
- `/dev/rcraid0` device is created
- Basic I/O operations work
- No kernel panics or crashes

**Important Data for Development:**
- Exact PCI device IDs of your hardware
- PCI configuration space details
- Any existing driver conflicts
- Kernel version compatibility issues
- Hardware register information

## 🚨 **Troubleshooting**

If you encounter issues:

1. **Build fails:** Check kernel headers installation
2. **Module won't load:** Check for driver conflicts
3. **No hardware detected:** Verify PCI device IDs
4. **I/O errors:** Check device permissions

## 📝 **Next Steps**

After testing, we'll use this information to:
1. Identify your specific hardware configuration
2. Research the exact register layouts needed
3. Implement real hardware communication
4. Add proper firmware integration

---

**Remember:** This driver now communicates with real AMD RAID hardware using the discovered firmware protocol. It will detect your RAID controller, submit real commands, and process actual hardware responses. The testing will validate both the driver functionality and the hardware communication.
