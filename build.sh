#!/bin/bash

# AMD RAID Driver Build Script
# Handles kernel header issues and provides fallback options

set -e

echo "AMD RAID Driver Build Script"
echo "============================"

# Building a module needs no privileges — only loading it does.

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

# Clean previous builds (only once we know the kernel dir is valid)
echo "Cleaning previous builds..."
make -C "$KERNELDIR" M="$(pwd)" clean

# Set environment variables.  KCFLAGS is the supported way to append
# compiler flags from the environment; modern kbuild ignores EXTRA_CFLAGS.
export KERNELDIR
export KCFLAGS="-Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers"

echo "Building with kernel directory: $KERNELDIR"
echo "Extra CFLAGS (KCFLAGS): $KCFLAGS"

# Build the module
echo "Building AMD RAID driver..."
if make -C "$KERNELDIR" M="$(pwd)" modules; then
    echo "Build successful!"
    echo "Module files created:"
    ls -la *.ko 2>/dev/null || echo "No .ko files found"
else
    echo "Build failed. Trying alternative approach..."

    # Try with minimal flags
    export KCFLAGS="-Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers -Wno-unused-but-set-variable"
    if make -C "$KERNELDIR" M="$(pwd)" modules; then
        echo "Build successful with minimal flags!"
        ls -la *.ko 2>/dev/null || echo "No .ko files found"
    else
        echo "Build failed completely. Check kernel headers installation."
        exit 1
    fi
fi

echo "Build script completed."
