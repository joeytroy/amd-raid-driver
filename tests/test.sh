#!/bin/bash

# Test script for rcraid driver
# Tests basic functionality and compatibility

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

# Test functions
test_build() {
    log_info "Testing driver build..."
    cd "$(dirname "$0")/.."
    make clean
    make
    if [ -f "rcraid.ko" ]; then
        log_info "Build test passed"
        return 0
    else
        log_error "Build test failed - rcraid.ko not found"
        return 1
    fi
}

test_module_info() {
    log_info "Testing module information..."
    cd "$(dirname "$0")/.."
    if [ -f "rcraid.ko" ]; then
        modinfo rcraid.ko > /dev/null
        log_info "Module info test passed"
        return 0
    else
        log_error "Module info test failed - rcraid.ko not found"
        return 1
    fi
}

test_module_loading() {
    log_info "Testing module loading (dry run)..."
    if modprobe -n rcraid 2>/dev/null; then
        log_info "Module loading test passed"
        return 0
    else
        log_warn "Module loading test failed - this may be expected if not running as root"
        return 1
    fi
}

test_dkms_config() {
    log_info "Testing DKMS configuration..."
    cd "$(dirname "$0")/.."
    if [ -f "dkms.conf" ]; then
        # Check if dkms.conf has required fields
        if grep -q "PACKAGE_NAME" dkms.conf && grep -q "PACKAGE_VERSION" dkms.conf; then
            log_info "DKMS configuration test passed"
            return 0
        else
            log_error "DKMS configuration test failed - missing required fields"
            return 1
        fi
    else
        log_error "DKMS configuration test failed - dkms.conf not found"
        return 1
    fi
}

test_install_scripts() {
    log_info "Testing installation scripts..."
    cd "$(dirname "$0")/.."
    scripts=("scripts/install_debian" "scripts/install_arch" "scripts/install_generic" "scripts/uninstall_generic")
    for script in "${scripts[@]}"; do
        if [ -f "$script" ] && [ -x "$script" ]; then
            log_info "Script $script is present and executable"
        else
            log_error "Script $script is missing or not executable"
            return 1
        fi
    done
    log_info "Installation scripts test passed"
    return 0
}

test_documentation() {
    log_info "Testing documentation..."
    cd "$(dirname "$0")/.."
    if [ -f "README.md" ]; then
        # Check if README has minimum content
        lines=$(wc -l < README.md)
        if [ $lines -gt 50 ]; then
            log_info "Documentation test passed ($lines lines)"
            return 0
        else
            log_error "Documentation test failed - README too short ($lines lines)"
            return 1
        fi
    else
        log_error "Documentation test failed - README.md not found"
        return 1
    fi
}

# Main test execution
main() {
    log_info "Starting rcraid driver tests..."
    
    tests=(
        "test_build"
        "test_module_info"
        "test_module_loading"
        "test_dkms_config"
        "test_install_scripts"
        "test_documentation"
    )
    
    passed=0
    total=${#tests[@]}
    
    for test in "${tests[@]}"; do
        if $test; then
            ((passed++))
        fi
    done
    
    log_info "Test results: $passed/$total tests passed"
    
    if [ $passed -eq $total ]; then
        log_info "All tests passed!"
        exit 0
    else
        log_error "Some tests failed"
        exit 1
    fi
}

# Run tests
main "$@"
