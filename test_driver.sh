#!/bin/bash
#
# AMD RAID Driver Test Script
# Basic validation for TRX50 RAID driver
#

set -e

DRIVER_NAME="rcraid"
DRIVER_MODULE="${DRIVER_NAME}.ko"
SYSFS_BASE="/sys/bus/pci/drivers/rcbottom"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================="
echo "AMD RAID Driver Test Script"
echo "=================================="
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root${NC}"
    exit 1
fi

# Test 1: Check if driver is built
echo -e "${YELLOW}[TEST 1]${NC} Checking if driver is built..."
if [ -f "$DRIVER_MODULE" ]; then
    echo -e "${GREEN}✓ Driver module found: $DRIVER_MODULE${NC}"
    ls -lh "$DRIVER_MODULE"
else
    echo -e "${RED}✗ Driver module not found. Run 'make' first.${NC}"
    exit 1
fi
echo

# Test 2: Check for AMD RAID device
echo -e "${YELLOW}[TEST 2]${NC} Checking for AMD RAID hardware (1022:43bd)..."
if lspci -d 1022:43bd | grep -q "AMD"; then
    echo -e "${GREEN}✓ AMD RAID device found:${NC}"
    lspci -d 1022:43bd -vv | head -20
else
    echo -e "${YELLOW}⚠ AMD RAID device (1022:43bd) not found${NC}"
    echo "  This is OK if testing on non-TRX50 hardware"
fi
echo

# Test 3: Load driver
echo -e "${YELLOW}[TEST 3]${NC} Loading driver..."
if lsmod | grep -q "^${DRIVER_NAME}"; then
    echo -e "${YELLOW}⚠ Driver already loaded, unloading first...${NC}"
    rmmod "$DRIVER_NAME" 2>/dev/null || true
    sleep 1
fi

if insmod "$DRIVER_MODULE"; then
    echo -e "${GREEN}✓ Driver loaded successfully${NC}"
else
    echo -e "${RED}✗ Failed to load driver${NC}"
    dmesg | tail -20
    exit 1
fi
sleep 1
echo

# Test 4: Check dmesg for driver messages
echo -e "${YELLOW}[TEST 4]${NC} Checking kernel messages..."
echo "Recent driver messages:"
dmesg | grep -i "rcraid\|rcbottom\|rc_bottom\|rc_hw\|rc_queue" | tail -20 || echo "(no messages found)"
echo

# Test 5: Check sysfs entries
echo -e "${YELLOW}[TEST 5]${NC} Checking sysfs entries..."
if [ -d "$SYSFS_BASE" ]; then
    echo -e "${GREEN}✓ Driver sysfs directory exists${NC}"
    
    # Check for bound devices
    DEVICES=$(find "$SYSFS_BASE" -maxdepth 1 -type l -name "0000:*" 2>/dev/null)
    if [ -n "$DEVICES" ]; then
        echo -e "${GREEN}✓ Found bound device(s):${NC}"
        for dev in $DEVICES; do
            dev_name=$(basename "$dev")
            echo "  - $dev_name"
            
            # Check rcraid sysfs group
            if [ -d "$SYSFS_BASE/$dev_name/rcraid" ]; then
                echo -e "${GREEN}  ✓ rcraid sysfs group exists${NC}"
                
                # Show adapter info
                if [ -f "$SYSFS_BASE/$dev_name/rcraid/adapter_info" ]; then
                    echo "  Adapter Info:"
                    cat "$SYSFS_BASE/$dev_name/rcraid/adapter_info" | sed 's/^/    /'
                fi
                
                # Show queue stats
                if [ -f "$SYSFS_BASE/$dev_name/rcraid/queue_stats" ]; then
                    echo "  Queue Stats:"
                    cat "$SYSFS_BASE/$dev_name/rcraid/queue_stats" | sed 's/^/    /'
                fi
                
                # Show doorbell state
                if [ -f "$SYSFS_BASE/$dev_name/rcraid/doorbell_state" ]; then
                    echo "  Doorbell State:"
                    cat "$SYSFS_BASE/$dev_name/rcraid/doorbell_state" | sed 's/^/    /'
                fi
            else
                echo -e "${YELLOW}  ⚠ rcraid sysfs group not found${NC}"
            fi
        done
    else
        echo -e "${YELLOW}⚠ No bound devices found${NC}"
    fi
else
    echo -e "${YELLOW}⚠ Driver sysfs directory not found${NC}"
fi
echo

# Test 6: Check for block devices
echo -e "${YELLOW}[TEST 6]${NC} Checking for RAID block devices..."
if ls /dev/rcraid* 2>/dev/null; then
    echo -e "${GREEN}✓ RAID block devices found:${NC}"
    ls -l /dev/rcraid*
    
    # Try to get device info
    for dev in /dev/rcraid*; do
        if [ -b "$dev" ]; then
            echo "  Device: $dev"
            blockdev --getsize64 "$dev" 2>/dev/null && \
                echo "    Size: $(blockdev --getsize64 "$dev") bytes" || true
        fi
    done
else
    echo -e "${YELLOW}⚠ No RAID block devices found (/dev/rcraid*)${NC}"
    echo "  This may be normal if no RAID arrays are configured"
fi
echo

# Test 7: Check module info
echo -e "${YELLOW}[TEST 7]${NC} Module information..."
if lsmod | grep -q "^${DRIVER_NAME}"; then
    echo -e "${GREEN}✓ Module loaded:${NC}"
    lsmod | grep "^${DRIVER_NAME}"
    echo
    modinfo "$DRIVER_MODULE" | grep -E "^(filename|version|description|author)" || true
else
    echo -e "${RED}✗ Module not loaded${NC}"
fi
echo

# Test 8: Basic I/O test (if devices exist)
echo -e "${YELLOW}[TEST 8]${NC} Basic I/O test..."
RAID_DEV=$(ls /dev/rcraid* 2>/dev/null | head -1)
if [ -n "$RAID_DEV" ] && [ -b "$RAID_DEV" ]; then
    echo "Testing basic read on $RAID_DEV..."
    if dd if="$RAID_DEV" of=/dev/null bs=4096 count=1 iflag=direct 2>&1 | grep -q "1+0"; then
        echo -e "${GREEN}✓ Basic read successful${NC}"
    else
        echo -e "${YELLOW}⚠ Read test inconclusive${NC}"
    fi
else
    echo -e "${YELLOW}⚠ No block device available for I/O test${NC}"
fi
echo

# Summary
echo "=================================="
echo "Test Summary"
echo "=================================="
echo
if lsmod | grep -q "^${DRIVER_NAME}"; then
    echo -e "${GREEN}✓ Driver loaded and operational${NC}"
    echo
    echo "Next steps:"
    echo "  1. Check dmesg for any errors: dmesg | grep -i rcraid"
    echo "  2. Monitor sysfs: watch -n1 'cat $SYSFS_BASE/*/rcraid/queue_stats'"
    echo "  3. Test I/O if arrays are configured"
    echo "  4. Unload driver: rmmod $DRIVER_NAME"
else
    echo -e "${RED}✗ Driver not loaded${NC}"
    echo "Check dmesg for errors"
fi
echo

# Keep driver loaded unless explicitly unloading
read -p "Unload driver now? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Unloading driver..."
    rmmod "$DRIVER_NAME" 2>/dev/null || true
    echo -e "${GREEN}Driver unloaded${NC}"
else
    echo "Driver remains loaded"
fi

