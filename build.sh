#!/bin/bash

# AMD RAID Driver Build Script
# Handles kernel header issues and provides fallback options

echo "AMD RAID Driver Build Script"
echo "============================"

# Check if we're running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Clean previous builds
echo "Cleaning previous builds..."
make clean

# Try to find a working kernel build directory
KERNELDIR=""
for dir in /lib/modules/$(uname -r)/build /usr/src/linux-headers-$(uname -r) /usr/src/linux-$(uname -r); do
    if [ -d "$dir" ] && [ -f "$dir/Makefile" ]; then
        KERNELDIR="$dir"
        echo "Found kernel build directory: $KERNELDIR"
        break
    fi
done

if [ -z "$KERNELDIR" ]; then
    echo "No suitable kernel build directory found"
    echo "Please install kernel headers:"
    echo "  sudo apt-get install linux-headers-$(uname -r)"
    exit 1
fi

# Set environment variables
export KERNELDIR
export EXTRA_CFLAGS="-Wall -Wextra -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers"

echo "Building with kernel directory: $KERNELDIR"
echo "Extra CFLAGS: $EXTRA_CFLAGS"

# Build the module
echo "Building AMD RAID driver..."
make -C "$KERNELDIR" M="$(pwd)" modules

if [ $? -eq 0 ]; then
    echo "Build successful!"
    echo "Module files created:"
    ls -la *.ko 2>/dev/null || echo "No .ko files found"
else
    echo "Build failed. Trying alternative approach..."
    
    # Try with minimal flags
    export EXTRA_CFLAGS="-Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers -Wno-unused-but-set-variable"
    make -C "$KERNELDIR" M="$(pwd)" modules
    
    if [ $? -eq 0 ]; then
        echo "Build successful with minimal flags!"
        ls -la *.ko 2>/dev/null || echo "No .ko files found"
    else
        echo "Build failed completely. Check kernel headers installation."
        exit 1
    fi
fi

echo "Build script completed."
