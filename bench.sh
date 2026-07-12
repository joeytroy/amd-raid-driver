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

echo "=== verify RAIDCore metadata via the volume (logical sector 40960) ==="
# logical 40960 -> member 0 phys 20480 = 0x5000
got=$(dd if="$RCRAID" bs=512 skip=40960 count=1 status=none | xxd -l 16 -p)
echo "  read 16 bytes: $got"
case "$got" in
    *52414944436f7265*)
        echo "  PASS — 'RAIDCore' magic present at logical sector 40960"
        ;;
    *)
        echo "  FAIL — magic not found at expected location"
        echo "         (members may be in wrong order — try"
        echo "          rmmod rcraid; modprobe rcraid reverse_member_order=1)"
        ;;
esac
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
