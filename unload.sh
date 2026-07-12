#!/bin/bash
#
# AMD RAID Driver Unload Script
# Safely unloads the driver and optionally cleans build artifacts
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================="
echo "AMD RAID Driver Unload Script"
echo "=================================="
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    exit 1
fi

# Refuse to unload while the volume is serving mounted filesystems —
# don't rely on rmmod's EBUSY as the only line of defense.
MOUNTED=$(findmnt -nro SOURCE,TARGET 2>/dev/null | awk '$1 ~ /^\/dev\/rcraid/' || true)
if [ -n "$MOUNTED" ]; then
    echo -e "${RED}✗ rcraid volume(s) are still mounted:${NC}"
    echo "$MOUNTED" | sed 's/^/    /'
    echo "Unmount them first, then re-run this script."
    exit 1
fi

# Check if module is loaded
if lsmod | grep -q "^rcraid"; then
    echo -e "${YELLOW}Unloading rcraid module...${NC}"

    # Try to unload
    if rmmod rcraid 2>/dev/null; then
        echo -e "${GREEN}✓ Module unloaded successfully${NC}"
    else
        echo -e "${RED}✗ Failed to unload module${NC}"
        echo "Module may be in use. Checking..."
        lsmod | grep rcraid
        exit 1
    fi
else
    echo -e "${GREEN}✓ Module not loaded${NC}"
fi

# Verify unload
if lsmod | grep -q "^rcraid"; then
    echo -e "${RED}✗ Module still loaded!${NC}"
    exit 1
else
    echo -e "${GREEN}✓ Module completely unloaded${NC}"
fi

echo
echo "Module unloaded successfully!"
echo "Run ./build.sh to rebuild with latest changes."

