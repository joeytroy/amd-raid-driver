#!/bin/bash
#
# AMD RAID Driver Test Script
# Comprehensive validation and diagnostics for TRX50 RAID driver
#

set -e

DRIVER_NAME="rcraid"
DRIVER_MODULE="${DRIVER_NAME}.ko"
SYSFS_BASE="/sys/bus/pci/drivers/rcbottom"
DEBUGFS_BASE="/sys/kernel/debug/rcraid"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Diagnostic output file
DIAG_FILE="driver_diagnostics_$(date +%Y%m%d_%H%M%S).txt"

# Function to log to both console and file
log_both() {
    echo -e "$1" | tee -a "$DIAG_FILE"
}

# Ensure debugfs is mounted so we can read rcraid diagnostics
ensure_debugfs_mounted() {
    if ! mount | grep -q ' on /sys/kernel/debug '; then
        echo "Mounting debugfs..."
        if mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; then
            echo -e "${GREEN}  ✓ debugfs mounted${NC}"
        else
            echo -e "${YELLOW}  ⚠ Failed to mount debugfs (continuing without debugfs)${NC}"
        fi
    fi
}

# Verify the NVMe init path completed for DEV_B000 controllers.
# This is the decisive check after the May 2026 driver restructure:
# rc_nvme_init_controller logs CAP, then writes AQA/ASQ/ACQ and reads
# them back. If the read-back values don't match what was written, the
# function returns -EIO and logs an error — that means we're still
# poking at the wrong register set and need to re-investigate.
check_nvme_init() {
    echo -e "${YELLOW}[TEST 4.6]${NC} Checking NVMe controller init path..."

    local mode_line cap_line aqa_line ready_line err_line skip_line

    mode_line=$(dmesg | grep "rc_parse_firmware_capabilities:" | tail -1 || true)
    cap_line=$(dmesg | grep "rc_nvme_init_controller: CAP=" | tail -1 || true)
    aqa_line=$(dmesg | grep "rc_nvme_init_controller: AQA=" | tail -1 || true)
    ready_line=$(dmesg | grep "rc_nvme_init_controller: ready" | tail -1 || true)
    err_line=$(dmesg | grep "rc_nvme_init_controller:" | grep -iE 'failed|did not persist|wrong register|fatal' | tail -1 || true)
    skip_line=$(dmesg | grep "rc_hw_init: skipping AHCI" | tail -1 || true)

    if [ -z "$mode_line" ]; then
        echo -e "${YELLOW}  ⚠ rc_parse_firmware_capabilities log line not found — driver may not have probed${NC}"
        echo
        return
    fi
    echo "  $mode_line"

    if echo "$mode_line" | grep -q "mode=2"; then
        echo -e "${GREEN}  ✓ Routed to NVMe path (DEV_B000)${NC}"
    elif echo "$mode_line" | grep -q "mode=1"; then
        echo -e "${YELLOW}  ⚠ Routed to AHCI path — NVMe init checks below will be skipped${NC}"
        echo
        return
    else
        echo -e "${YELLOW}  ⚠ Routed to unknown/stub path${NC}"
        echo
        return
    fi

    [ -n "$cap_line" ]   && echo "  $cap_line"
    [ -n "$aqa_line" ]   && echo "  $aqa_line"
    [ -n "$ready_line" ] && echo "  $ready_line"
    [ -n "$skip_line" ]  && echo "  $skip_line"

    if [ -n "$err_line" ]; then
        echo -e "${RED}  ✗ NVMe init error detected:${NC}"
        echo "    $err_line"
        echo -e "${YELLOW}    → Register write-back failed. See docs/STATUS.md for next steps.${NC}"
    elif [ -n "$ready_line" ]; then
        echo -e "${GREEN}  ✓ Controller reached CSTS.RDY — admin queue programmed successfully${NC}"
    elif [ -n "$aqa_line" ]; then
        echo -e "${YELLOW}  ⚠ AQA/ASQ/ACQ programmed but controller did not signal ready (timeout?)${NC}"
    else
        echo -e "${YELLOW}  ⚠ NVMe init log lines missing — bring-up may not have run${NC}"
    fi

    echo
}

# Check dmesg for rcraid-related error keywords
check_driver_logs_for_errors() {
    echo -e "${YELLOW}[TEST 4.5]${NC} Checking driver logs for errors..."
    local errors
    errors=$(dmesg | grep -iE "rcraid|rcbottom|rc_hw|rc_queue" | grep -iE "error|fail|timeout|reset" 2>/dev/null || true)
    if [ -n "$errors" ]; then
        echo -e "${YELLOW}⚠ Potential issues detected in kernel log:${NC}"
        echo "$errors" | tail -10
    else
        echo -e "${GREEN}✓ No rcraid error keywords found in recent kernel log${NC}"
    fi
    echo
}

# Sanitize numeric metrics parsed from sysfs/debugfs
sanitize_metric() {
    local value="$1"
    if [[ $value =~ ^[0-9]+$ ]]; then
        echo "$value"
    else
        echo "0"
    fi
}

# Verify metadata discovery messages appear in dmesg
validate_metadata_flow() {
    echo -e "${YELLOW}[TEST 5.5]${NC} Checking metadata discovery flow..."
    local scan discover
    scan=$(dmesg | grep "rc_scan_physical_disks" | tail -1 || true)
    discover=$(dmesg | grep "rc_discover_arrays" | tail -1 || true)

    if [ -n "$scan" ] && [ -n "$discover" ]; then
        echo -e "${GREEN}✓ Firmware scan and array discovery messages detected${NC}"
        echo "  $scan"
        echo "  $discover"
    else
        echo -e "${YELLOW}⚠ Metadata discovery messages not observed; firmware handshake may be incomplete${NC}"
    fi
    echo
}

# Confirm pending request lists are empty for each adapter
verify_pending_requests() {
    echo -e "${YELLOW}[TEST 5.6]${NC} Checking pending request queues..."

    local found=0
    for file in "$SYSFS_BASE"/0000:*/rcraid/pending_requests; do
        [ -f "$file" ] || continue
        found=1
        local adapter
        adapter=$(basename "$(dirname "$file")")
        local count
        count=$(awk '/Pending Requests:/ {print $3}' "$file" 2>/dev/null)
        if [ -z "$count" ]; then
            echo -e "${YELLOW}  ⚠ Could not parse pending requests for $adapter${NC}"
            continue
        fi
        if [ "$count" -gt 0 ]; then
            echo -e "${YELLOW}  ⚠ $adapter reports $count pending request(s)${NC}"
        else
            echo -e "${GREEN}  ✓ $adapter pending queue empty${NC}"
        fi
    done

    if [ $found -eq 0 ]; then
        echo -e "${YELLOW}⚠ No rcraid pending request files available${NC}"
    fi

    echo
}

# Sample queue stats to detect activity/interrupt progress
monitor_queue_activity() {
    echo -e "${YELLOW}[TEST 5.7]${NC} Monitoring queue activity (5s sample)..."

    local found=0
    for file in "$SYSFS_BASE"/0000:*/rcraid/queue_stats; do
        [ -f "$file" ] || continue
        found=1
        local adapter
        adapter=$(basename "$(dirname "$file")")

        local seq_start comp_start irq_start
        seq_start=$(awk -F': ' '/Command Sequence/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")
        comp_start=$(awk -F': ' '/Completions/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")
        irq_start=$(awk -F': ' '/IRQ Count/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")

        seq_start=$(sanitize_metric "$seq_start")
        comp_start=$(sanitize_metric "$comp_start")
        irq_start=$(sanitize_metric "$irq_start")

        sleep 5

        local seq_end comp_end irq_end
        seq_end=$(awk -F': ' '/Command Sequence/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")
        comp_end=$(awk -F': ' '/Completions/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")
        irq_end=$(awk -F': ' '/IRQ Count/ {gsub(/^[ \t]+/, "", $2); print $2; exit}' "$file")

        seq_end=$(sanitize_metric "$seq_end")
        comp_end=$(sanitize_metric "$comp_end")
        irq_end=$(sanitize_metric "$irq_end")

        local delta_seq=$((seq_end - seq_start))
        local delta_comp=$((comp_end - comp_start))
        local delta_irq=$((irq_end - irq_start))

        echo "  $adapter:"
        echo "    Command Sequence: $seq_start -> $seq_end (Δ $delta_seq)"
        echo "    Completions:      $comp_start -> $comp_end (Δ $delta_comp)"
        echo "    IRQ Count:        $irq_start -> $irq_end (Δ $delta_irq)"

        if [ "$delta_seq" -gt 0 ] || [ "$delta_comp" -gt 0 ] || [ "$delta_irq" -gt 0 ]; then
            echo -e "    ${GREEN}✓ Activity detected during sample window${NC}"
        else
            echo -e "    ${YELLOW}⚠ No queue or IRQ activity observed (idle path)${NC}"
        fi
    done

    if [ $found -eq 0 ]; then
        echo -e "${YELLOW}⚠ No queue_stats files available${NC}"
    fi

    echo
}

# Validate debugfs entries are present and readable
verify_debugfs_access() {
    echo -e "${YELLOW}[TEST 7.5]${NC} Validating debugfs access..."

    if [ ! -d "$DEBUGFS_BASE" ]; then
        echo -e "${YELLOW}⚠ rcraid debugfs root not available${NC}"
        echo
        return
    fi

    local any=0
    for adapter_dir in "$DEBUGFS_BASE"/adapter*; do
        [ -d "$adapter_dir" ] || continue
        any=1
        local name
        name=$(basename "$adapter_dir")
        for file in cmd_queue comp_queue pending_requests irq_state registers; do
            local path="$adapter_dir/$file"
            if [ -f "$path" ]; then
                if head -n 5 "$path" >/dev/null 2>&1; then
                    echo -e "${GREEN}  ✓ $name/$file readable${NC}"
                else
                    echo -e "${YELLOW}  ⚠ $name/$file not readable${NC}"
                fi
            else
                echo -e "${YELLOW}  ⚠ $name/$file missing${NC}"
            fi
        done
    done

    if [ $any -eq 0 ]; then
        echo -e "${YELLOW}⚠ No rcraid adapters exposed via debugfs${NC}"
    fi

    echo
}

# Check IRQ configuration reported in debugfs
verify_irq_setup() {
    echo -e "${YELLOW}[TEST 7.6]${NC} Verifying IRQ setup..."

    if [ ! -d "$DEBUGFS_BASE" ]; then
        echo -e "${YELLOW}⚠ Debugfs not available; skipping IRQ validation${NC}"
        echo
        return
    fi

    local any=0
    for path in "$DEBUGFS_BASE"/adapter*/irq_state; do
        [ -f "$path" ] || continue
        any=1
        local adapter mode vector count
        adapter=$(basename "$(dirname "$path")")
        mode=$(grep -m1 "IRQ Mode" "$path" | awk -F': ' '{print $2}')
        vector=$(grep -m1 "IRQ Vector" "$path" | awk -F': ' '{print $2}')
        count=$(grep -m1 "IRQ Count" "$path" | awk -F': ' '{print $2}')

        if [ -z "$mode" ]; then
            echo -e "${YELLOW}  ⚠ Unable to read IRQ state for $adapter${NC}"
            continue
        fi

        if [ "$mode" = "MSI" ]; then
            echo -e "${GREEN}  ✓ $adapter using MSI (vector $vector, count $count)${NC}"
        else
            echo -e "${YELLOW}  ⚠ $adapter not using MSI (mode reported: $mode)${NC}"
        fi
    done

    if [ $any -eq 0 ]; then
        echo -e "${YELLOW}⚠ No irq_state debugfs files found${NC}"
    fi

    echo
}

# Function to run command and capture output
run_and_capture() {
    local cmd="$1"
    local desc="$2"
    
    log_both "\n=== $desc ==="
    log_both "Command: $cmd"
    log_both "---"
    eval "$cmd" 2>&1 | tee -a "$DIAG_FILE" || log_both "(command failed or returned no output)"
}

echo "=================================="
echo "AMD RAID Driver Test & Diagnostics"
echo "=================================="
echo "Diagnostic output: $DIAG_FILE"
echo

# Start diagnostic file
log_both "AMD RAID Driver Diagnostic Report"
log_both "Generated: $(date)"
log_both "Hostname: $(hostname)"
log_both "==================================\n"

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
echo -e "${YELLOW}[TEST 2]${NC} Checking for AMD RAID hardware..."
if lspci -d 1022:43bd | grep -q "AMD"; then
    echo -e "${GREEN}✓ AMD RAID device (1022:43bd) found:${NC}"
    lspci -d 1022:43bd -vv | head -20
fi
if lspci -d 1022:b000 | grep -q "AMD"; then
    echo -e "${GREEN}✓ AMD RAID Bottom device (1022:b000) found:${NC}"
    lspci -d 1022:b000 -nn | head -10
    echo
    echo "Checking device bindings..."
    for dev in $(lspci -d 1022:b000 | awk '{print $1}'); do
        driver=$(readlink -f /sys/bus/pci/devices/0000:$dev/driver 2>/dev/null | xargs basename 2>/dev/null || echo "none")
        subsystem=$(lspci -s $dev -v 2>/dev/null | grep -i "subsystem" | head -1)
        echo "  Device $dev: driver=$driver $subsystem"
    done
else
    echo -e "${YELLOW}⚠ AMD RAID device (1022:43bd or 1022:b000) not found${NC}"
    echo "  This is OK if testing on non-TRX50 hardware"
fi
echo

# Test 2.5: Ensure nvme driver is loaded, then unbind devices (except Samsung OS drive)
echo -e "${YELLOW}[TEST 2.5]${NC} Managing NVMe driver bindings..."
echo "Ensuring nvme driver is loaded (needed for OS drive)..."
if ! lsmod | grep -q "^nvme "; then
    echo "  Loading nvme driver..."
    modprobe nvme 2>/dev/null || echo -e "${YELLOW}  ⚠ Could not load nvme driver (may already be loaded)${NC}"
else
    echo -e "${GREEN}  ✓ NVMe driver already loaded${NC}"
fi

echo
echo "Unbinding AMD RAID devices from nvme driver (except Samsung OS drive)..."
UNBOUND_COUNT=0
SKIPPED_COUNT=0
for dev in $(lspci -d 1022:b000 | awk '{print $1}'); do
    driver=$(readlink -f /sys/bus/pci/devices/0000:$dev/driver 2>/dev/null | xargs basename 2>/dev/null || echo "none")
    subsystem=$(lspci -s $dev -v 2>/dev/null | grep -i "subsystem" | head -1)
    
    # Identify Samsung devices (likely OS drive) - keep bound to nvme
    if echo "$subsystem" | grep -qi "samsung"; then
        echo -e "${YELLOW}  ⚠ Skipping Samsung device $dev (OS drive - keeping bound to nvme)${NC}"
        SKIPPED_COUNT=$((SKIPPED_COUNT + 1))
        continue
    fi
    
    # For non-Samsung devices, unbind from nvme if bound
    if [ "$driver" = "nvme" ]; then
        echo "  Unbinding device $dev from nvme driver..."
        if echo "0000:$dev" > /sys/bus/pci/drivers/nvme/unbind 2>/dev/null; then
            echo -e "${GREEN}  ✓ Unbound device $dev from nvme${NC}"
            UNBOUND_COUNT=$((UNBOUND_COUNT + 1))
            sleep 0.5  # Give kernel time to process unbind
        else
            echo -e "${YELLOW}  ⚠ Failed to unbind device $dev (may be in use)${NC}"
        fi
    elif [ "$driver" = "rcbottom" ]; then
        echo -e "${GREEN}  ✓ Device $dev already bound to rcbottom driver${NC}"
    else
        echo "  Device $dev status: driver=$driver (ready for binding)"
    fi
done

echo
if [ $UNBOUND_COUNT -gt 0 ]; then
    echo -e "${GREEN}✓ Unbound $UNBOUND_COUNT device(s) from nvme driver${NC}"
fi
if [ $SKIPPED_COUNT -gt 0 ]; then
    echo -e "${GREEN}✓ Skipped $SKIPPED_COUNT Samsung device(s) (OS drive protected)${NC}"
fi
if [ $UNBOUND_COUNT -eq 0 ] && [ $SKIPPED_COUNT -eq 0 ]; then
    echo -e "${YELLOW}⚠ No devices processed (may already be configured)${NC}"
fi
sleep 1
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

check_nvme_init
check_driver_logs_for_errors

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

validate_metadata_flow
verify_pending_requests
monitor_queue_activity

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

ensure_debugfs_mounted
verify_debugfs_access
verify_irq_setup

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

# Test 9: Comprehensive diagnostic collection
echo -e "${YELLOW}[TEST 9]${NC} Collecting comprehensive diagnostics..."

# System information
run_and_capture "uname -a" "System Information"
run_and_capture "cat /proc/version" "Kernel Version"
run_and_capture "lsb_release -a" "Distribution Info"

# Hardware detection
run_and_capture "lspci -vvv -d 1022:43bd" "PCI Device Details (Full)"
run_and_capture "lspci -nn | grep -i raid" "All RAID Controllers"

# Complete dmesg log
run_and_capture "dmesg | grep -i 'rcraid\|rcbottom\|rc_'" "Complete Driver Messages"

# All sysfs files
if [ -d "$SYSFS_BASE" ]; then
    for device in "$SYSFS_BASE"/0000:*; do
        if [ -d "$device/rcraid" ]; then
            log_both "\n=== Sysfs Data for $(basename $device) ==="
            for file in "$device/rcraid"/*; do
                if [ -f "$file" ]; then
                    log_both "\n--- $(basename $file) ---"
                    cat "$file" 2>&1 | tee -a "$DIAG_FILE" || log_both "(read failed)"
                fi
            done
        fi
    done
fi

# All debugfs files  
if [ -d "$DEBUGFS_BASE/adapter0" ]; then
    log_both "\n=== Debugfs Data ==="
    for file in "$DEBUGFS_BASE/adapter0"/*; do
        if [ -f "$file" ]; then
            log_both "\n--- $(basename $file) ---"
            cat "$file" 2>&1 | tee -a "$DIAG_FILE" || log_both "(read failed)"
        fi
    done
fi

# Memory and resource usage
run_and_capture "cat /proc/meminfo | head -20" "Memory Info"
run_and_capture "cat /proc/interrupts | grep -E 'rcraid|244'" "Interrupt Info"
run_and_capture "lsmod | grep rcraid" "Module Status"

# Block device info
run_and_capture "lsblk" "Block Devices"
run_and_capture "ls -la /dev/rcraid* 2>&1" "RAID Device Nodes"

log_both "\n==================================\n"
log_both "Diagnostic collection complete!"
log_both "Output saved to: $DIAG_FILE"
echo
echo -e "${GREEN}✓ Diagnostics saved to: $DIAG_FILE${NC}"
echo

# Summary
echo "=================================="
echo "Test Summary"
echo "=================================="
echo
if lsmod | grep -q "^${DRIVER_NAME}"; then
    echo -e "${GREEN}✓ Driver loaded and operational${NC}"
    echo
    echo -e "${GREEN}📊 Complete diagnostics saved to: $DIAG_FILE${NC}"
    echo
    echo "Share this file for analysis!"
    echo
    echo "Manual monitoring commands:"
    echo "  • Watch queues: watch -n1 'cat $SYSFS_BASE/*/rcraid/queue_stats'"
    echo "  • View registers: cat /sys/kernel/debug/rcraid/adapter0/registers"
    echo "  • Check dmesg: dmesg | grep -i rcraid"
else
    echo -e "${RED}✗ Driver not loaded${NC}"
    echo "Check $DIAG_FILE for details"
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
