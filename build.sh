#!/bin/bash

# AMD RAID Driver Build Script
# Helps with common build issues on different distributions

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
if [ "$EUID" -eq 0 ]; then
    log_warn "Building as root - this is not recommended"
fi

# Get kernel version
KERNEL_VERSION=$(uname -r)
log_info "Building for kernel: $KERNEL_VERSION"

# Check if kernel headers exist
if [ ! -d "/lib/modules/${KERNEL_VERSION}/build" ]; then
    log_error "Kernel headers not found for ${KERNEL_VERSION}"
    log_info "Please install kernel headers:"
    log_info "  Ubuntu/Debian: sudo apt install linux-headers-${KERNEL_VERSION}"
    log_info "  Arch Linux: sudo pacman -S linux-headers"
    log_info "  RHEL/CentOS: sudo yum install kernel-devel"
    log_info "  SUSE: sudo zypper install kernel-devel"
    exit 1
fi

# Check if build tools are available
if ! command -v make >/dev/null 2>&1; then
    log_error "make not found - please install build tools"
    exit 1
fi

if ! command -v gcc >/dev/null 2>&1; then
    log_error "gcc not found - please install build tools"
    exit 1
fi

# Clean previous build
log_info "Cleaning previous build..."
make clean

# Build the driver
log_info "Building AMD RAID driver..."
make

# Check if build was successful
if [ -f "rcraid.ko" ]; then
    log_info "Build successful! Driver module: rcraid.ko"
    
    # Show module info
    log_info "Module information:"
    modinfo rcraid.ko | head -10
    
    # Test module loading (dry run)
    if modprobe -n rcraid 2>/dev/null; then
        log_info "Module can be loaded successfully"
    else
        log_warn "Module loading test failed - check dependencies"
    fi
else
    log_error "Build failed - rcraid.ko not found"
    log_error "Common issues:"
    log_error "  - Missing kernel headers: sudo apt install linux-headers-\$(uname -r)"
    log_error "  - Missing build tools: sudo apt install build-essential"
    log_error "  - Check build output above for specific errors"
    exit 1
fi

log_info "Build completed successfully!"
log_info "To install: sudo make install"
log_info "To install with DKMS: sudo make install-dkms"
