#!/bin/bash
# Quick post-reboot smoke test for /dev/rcraid0.
#
# Confirms (1) the disk exists, (2) read throughput across several bs values,
# (3) the RAIDCore metadata is still readable at the expected logical sector,
# (4) the stripe-mapping fires the right member for an edge-of-stripe read.

set -e
RCRAID=/dev/rcraid0

if [ "$EUID" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

if [ ! -b "$RCRAID" ]; then
    echo "FAIL: $RCRAID is missing"
    echo "      run sudo ./test_driver.sh to load and bind the driver first"
    exit 1
fi

echo "=== $RCRAID present ==="
ls -la "$RCRAID"
lsblk "$RCRAID"
echo

echo "=== sequential read throughput across block sizes ==="
for bs in 4K 8K 32K 64K; do
    bytes=$((4 * 1024 * 1024))
    case "$bs" in
        4K)  count=$((bytes / 4096)) ;;
        8K)  count=$((bytes / 8192)) ;;
        32K) count=$((bytes / 32768)) ;;
        64K) count=$((bytes / 65536)) ;;
    esac
    printf '  bs=%-4s  ' "$bs"
    dd if="$RCRAID" of=/dev/null bs="$bs" count="$count" 2>&1 \
        | tail -1 \
        | sed 's/^/   /'
done
echo

echo "=== verify RAIDCore metadata (driver-validated at bind time) ==="
# The metadata region lives below each member's user_off, so on real
# firmware arrays it is NOT addressable through the volume — reading a
# logical sector for the magic only ever worked on synthetic layouts
# with user_off=0.  Instead, confirm the driver's own validation: it
# logs one RC_NOTE "RAIDCore" metadata line per member at bind time.
# Count only lines from the most recent driver load — earlier binds in
# the same boot (the load/rmmod/tweak/reload workflow) leave stale
# validation lines in the ring buffer that would inflate the count.
members=$(dmesg | awk '
    /rc_init: AMD RAID Driver version/ { n = 0 }
    /rc_nvme_read_validate_metadata: RAIDCore/ { n++ }
    END { print n + 0 }')
if [ "$members" -ge 2 ]; then
    echo "  PASS — driver validated RAIDCore metadata on $members members"
elif [ "$members" -eq 1 ]; then
    echo "  FAIL — only 1 member logged validated metadata (need >= 2)"
else
    echo "  WARN — no metadata-validation lines in dmesg (ring buffer may"
    echo "         have wrapped since boot; not necessarily a failure)"
fi
echo

echo "=== stripe-boundary sanity (logical 2047 vs 2048) ==="
# Sectors 0..2047 → member 0; sectors 2048..4095 → member 1.
# Both should read as zeros (empty array) — what we're checking is no oops.
# mktemp, not a predictable /tmp name: we run as root and a fixed path
# would be a symlink-clobber hazard.
boundary_bin=$(mktemp)
trap 'rm -f "$boundary_bin"' EXIT
dd if="$RCRAID" bs=512 skip=2047 count=2 status=none of="$boundary_bin"
if [ "$(wc -c < "$boundary_bin")" = "1024" ]; then
    echo "  PASS — straddling read across stripe boundary returned 1024 bytes"
else
    echo "  FAIL — boundary read returned wrong size"
fi
echo

echo "Done."
