#!/bin/bash
# Write-path torture test for /dev/rcraid0 (or a partition on it).
#
# DESTRUCTIVE — erases the target device.  Run it from the live-USB
# environment before installing an OS, or against a dedicated scratch
# partition/array.  It will refuse mounted targets.
#
# Exists because dd/fio with O_DIRECT is NOT sufficient validation of the
# write path: direct I/O arrives in page-aligned, page-sized segments,
# while real filesystem workloads (mkfs, buffered writeback) arrive as
# fragmented 512-byte buffer-head bvecs at unaligned offsets.  The
# July 2026 multi-stripe fan-out bug passed every direct-I/O smoke test
# and then failed mkfs.ext4 during an OS install (see PR #10).
#
# Phases:
#   1. fragmented buffered writes (512B-4K mixed, random order) + fsync,
#      then drop caches and verify with direct reads   <- the mkfs killer
#   2. large sequential direct writes (1 MiB) at a stripe-unaligned
#      offset + direct verify                          <- multi-stripe shape
#   3. mkfs.ext4 + e2fsck -fn                          <- the real workload
#   4. dmesg tripwire: any rcraid "rejected" / "failed SC/SCT" / I/O error
#      line emitted during the run fails the test
#
# Usage:
#   sudo ./test_write_path.sh /dev/rcraid0p3          # scratch partition
#   sudo ./test_write_path.sh --yes /dev/rcraid0      # skip confirmation
#
# Tunables (env): TEST_SIZE (default 1G)  — per-fio-phase data size

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

TEST_SIZE="${TEST_SIZE:-1G}"
RANDSEED=8675309
ASSUME_YES=0
DEV=""

for arg in "$@"; do
    case "$arg" in
        --yes) ASSUME_YES=1 ;;
        -*)    echo "unknown option: $arg" >&2; exit 2 ;;
        *)     DEV="$arg" ;;
    esac
done

if [ -z "$DEV" ]; then
    echo "usage: sudo $0 [--yes] /dev/rcraid0pN" >&2
    exit 2
fi
[ "$(id -u)" -eq 0 ] || { echo "must be root (try: sudo $0 $*)" >&2; exit 1; }
[ -b "$DEV" ] || { echo "$DEV is not a block device" >&2; exit 1; }

command -v fio >/dev/null || {
    echo "fio is required.  Install it first:" >&2
    echo "  apt-get install fio    (Debian/Ubuntu)" >&2
    echo "  dnf install fio        (Fedora)" >&2
    exit 1
}

# Refuse if the target, any child partition, or the parent disk is mounted.
if lsblk -no MOUNTPOINTS "$DEV" 2>/dev/null | grep -q .; then
    echo "$DEV (or a partition on it) is mounted — refusing to run" >&2
    lsblk "$DEV" >&2
    exit 1
fi

DEV_BYTES=$(blockdev --getsize64 "$DEV")
MIN_BYTES=$((3 * 1024 * 1024 * 1024))   # phases need TEST_SIZE + headroom
if [ "$DEV_BYTES" -lt "$MIN_BYTES" ]; then
    echo "$DEV is only $DEV_BYTES bytes; need >= 3 GiB for the default phases" >&2
    exit 1
fi

echo "==============================================================="
echo "  rcraid write-path torture test"
echo "  target: $DEV ($((DEV_BYTES / 1024 / 1024 / 1024)) GiB)"
echo "==============================================================="
echo
echo -e "${RED}THIS WILL DESTROY ALL DATA ON $DEV${NC}"
blkid "$DEV" 2>/dev/null | sed 's/^/  currently: /' || true
echo
if [ "$ASSUME_YES" -ne 1 ]; then
    read -rp "Type DESTROY to continue: " confirm
    [ "$confirm" = "DESTROY" ] || { echo "aborted"; exit 1; }
fi

FAILED=0
fail() { echo -e "${RED}  ✗ FAIL:${NC} $1"; FAILED=1; }
pass() { echo -e "${GREEN}  ✓ PASS:${NC} $1"; }

# dmesg tripwire: stamp a marker now, grep everything after it at the end.
MARKER="rcraid-write-path-test-$$-$(date +%s)"
echo "$MARKER: BEGIN" > /dev/kmsg

# ----------------------------------------------------------------------------
echo
echo "==> [1/4] fragmented buffered writes (the mkfs killer)"
# psync + buffered + bssplit down to 512 B reproduces buffer-head writeback:
# many tiny bvecs, unaligned everywhere, exactly what overflowed the
# multi-stripe sg arrays.  Fixed randseed makes the workload reproducible so
# the verify pass can regenerate it.  Verify reads use O_DIRECT after a
# cache drop, so data is checked on-media, not in the page cache.
FRAG_ARGS=(--name=frag --filename="$DEV" --ioengine=psync
           --rw=randwrite --bssplit=512/40:1k/30:2k/20:4k/10
           --size="$TEST_SIZE" --randseed="$RANDSEED" --randrepeat=0
           --verify=crc32c --end_fsync=1 --group_reporting --minimal)
if fio "${FRAG_ARGS[@]}" --do_verify=0 > /dev/null; then
    sync
    echo 3 > /proc/sys/vm/drop_caches
    if fio "${FRAG_ARGS[@]}" --verify_only --direct=1 > /dev/null; then
        pass "fragmented buffered write + on-media verify ($TEST_SIZE)"
    else
        fail "verify mismatch after fragmented buffered writes (data corruption)"
    fi
else
    fail "fragmented buffered write phase returned I/O errors"
fi

# ----------------------------------------------------------------------------
echo
echo "==> [2/4] large direct writes at a stripe-unaligned offset"
# 1 MiB requests starting 512 B past a stripe boundary: every request spans
# stripes.  Today blk-mq splits these at chunk_sectors; when the multi-stripe
# fan-out returns, this phase exercises it directly.
SEQ_ARGS=(--name=bigdirect --filename="$DEV" --ioengine=psync
          --rw=write --bs=1M --direct=1 --offset=512
          --size="$TEST_SIZE" --verify=crc32c --group_reporting --minimal)
if fio "${SEQ_ARGS[@]}" --do_verify=0 > /dev/null; then
    if fio "${SEQ_ARGS[@]}" --verify_only > /dev/null; then
        pass "unaligned 1 MiB direct write + verify ($TEST_SIZE)"
    else
        fail "verify mismatch after unaligned direct writes (data corruption)"
    fi
else
    fail "unaligned direct write phase returned I/O errors"
fi

# ----------------------------------------------------------------------------
echo
echo "==> [3/4] mkfs.ext4 + fsck (the workload that originally failed)"
if mkfs.ext4 -qF "$DEV"; then
    if e2fsck -fn "$DEV" > /dev/null 2>&1; then
        pass "mkfs.ext4 completed and filesystem checks clean"
    else
        fail "e2fsck found errors in the freshly created filesystem"
    fi
else
    fail "mkfs.ext4 returned an error"
fi

# ----------------------------------------------------------------------------
echo
echo "==> [4/4] dmesg tripwire"
echo "$MARKER: END" > /dev/kmsg
KMSG_ERRORS=$(dmesg | sed -n "/$MARKER: BEGIN/,/$MARKER: END/p" \
    | grep -iE "rcraid.*(rejected|failed SC/SCT)|I/O error, dev rcraid" || true)
if [ -z "$KMSG_ERRORS" ]; then
    pass "no rcraid errors in dmesg during the run"
else
    fail "rcraid errors appeared in dmesg during the run:"
    echo "$KMSG_ERRORS" | sed 's/^/    /'
fi

# ----------------------------------------------------------------------------
echo
if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}ALL WRITE-PATH TESTS PASSED${NC} — $DEV left with an empty ext4"
    exit 0
else
    echo -e "${RED}WRITE-PATH TESTS FAILED${NC} — see above and dmesg"
    exit 1
fi
