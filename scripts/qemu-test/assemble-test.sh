#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Exercise scripts/rcassemble.py against synthetic arrays — no hardware,
# no QEMU, no kernel driver.  For each level (raid0, raid1, raid10):
#
#   1. create member image files and stamp them with mkmeta.py;
#   2. write a known pattern into the volume's LBA space by hand (striping
#      / mirroring in this script, independently of rcassemble's math);
#   3. attach the images to loop devices, assemble read-only with
#      rcassemble.py, and verify the pattern reads back through
#      /dev/mapper — proving parser, geometry, and dm-table agree with
#      the on-disk layout;
#   4. also verify --parse-only and --dry-run work unprivileged, that a
#      raid10 assembly succeeds with one leg of each pair missing, and
#      that a corrupted checksum is rejected.
#
# Needs root for losetup/dmsetup (steps 3-4); steps run unprivileged
# where possible.  Exit 0 iff every check passes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MKMETA="$SCRIPT_DIR/mkmeta.py"
RCASSEMBLE="$SCRIPT_DIR/../rcassemble.py"
SIZE_MIB=64
UD_OFF=$((0x6000))            # userdata_offset_sectors, per mkmeta.py
PATTERN_MIB=4

WORKDIR="$(mktemp -d /tmp/rcassemble-test.XXXXXX)"
LOOPS=()
DM_NAME="rcatest"

cleanup() {
    dmsetup remove "$DM_NAME" 2>/dev/null || true
    for l in "${LOOPS[@]:-}"; do losetup -d "$l" 2>/dev/null || true; done
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

[ "$(id -u)" = 0 ] || fail "must run as root (losetup/dmsetup)"

make_images() { # count -> $IMAGES
    IMAGES=()
    local i
    for i in $(seq 0 $(( $1 - 1 ))); do
        local img="$WORKDIR/m$i.img"
        rm -f "$img"
        truncate -s $((SIZE_MIB * 1024 * 1024)) "$img"
        IMAGES+=("$img")
    done
}

attach_loops() { # $IMAGES -> $LOOPS
    for l in "${LOOPS[@]:-}"; do losetup -d "$l" 2>/dev/null || true; done
    LOOPS=()
    local img
    for img in "${IMAGES[@]}"; do
        LOOPS+=("$(losetup -f --show "$img")")
    done
}

# Write $PATTERN_MIB MiB of deterministic data into the VOLUME LBA space,
# distributing it to member images per the level's layout — independent
# reimplementation of the mapping, so agreement with rcassemble is a real
# cross-check, not self-confirmation.
write_pattern() { # level chunk_sectors user_size_sectors
    local level=$1 chunk=$2 user_size=$3
    dd if=/dev/urandom of="$WORKDIR/pattern" \
       bs=1M count=$PATTERN_MIB status=none
    local nbytes=$((PATTERN_MIB * 1024 * 1024))
    local chunk_bytes=$((chunk * 512))
    local voff=0
    while [ "$voff" -lt "$nbytes" ]; do
        local vchunk=$((voff / chunk_bytes))
        local inchunk=$((voff % chunk_bytes))
        local targets=() col
        case "$level" in
            raid0)
                col=$((vchunk % ${#IMAGES[@]}))
                targets=("${IMAGES[$col]}")
                ;;
            raid1)
                targets=("${IMAGES[@]}")
                ;;
            raid10)
                local pairs=$(( ${#IMAGES[@]} / 2 ))
                col=$((vchunk % pairs))
                # pair-major element order: pair p = images 2p, 2p+1
                targets=("${IMAGES[$((col * 2))]}" "${IMAGES[$((col * 2 + 1))]}")
                ;;
        esac
        local mchunk=$vchunk
        case "$level" in
            raid0)  mchunk=$((vchunk / ${#IMAGES[@]})) ;;
            raid10) mchunk=$((vchunk / (${#IMAGES[@]} / 2) )) ;;
        esac
        local mbyte=$((UD_OFF * 512 + mchunk * chunk_bytes + inchunk))
        local n=$((chunk_bytes - inchunk))
        [ $((voff + n)) -gt "$nbytes" ] && n=$((nbytes - voff))
        local t
        for t in "${targets[@]}"; do
            dd if="$WORKDIR/pattern" of="$t" bs=1 skip="$voff" \
               seek="$mbyte" count="$n" conv=notrunc status=none
        done
        voff=$((voff + n))
    done
}

run_level() { # level member_count [extra mkmeta args...]
    local level=$1 count=$2
    shift 2
    echo "== $level ($count members)${*:+ [$*]}"
    make_images "$count"
    local out
    out="$(python3 "$MKMETA" --level "$level" "$@" "${IMAGES[@]}")"
    local chunk user_size cap
    chunk="$(sed -n 's/.*chunk_sectors=\([0-9]*\).*/\1/p' <<<"$out" | head -1)"
    user_size="$(sed -n 's/.*user_size=\([0-9]*\).*/\1/p' <<<"$out" | head -1)"
    cap="$(sed -n 's/^capacity_sectors=\([0-9]*\)$/\1/p' <<<"$out")"
    [ -n "$chunk" ] && [ -n "$cap" ] || fail "$level: mkmeta output unparsed"

    write_pattern "$level" "$chunk" "$user_size"

    # Unprivileged-path checks against the image files directly.
    python3 "$RCASSEMBLE" --parse-only "${IMAGES[@]}" \
        | grep -q "level=$level" || fail "$level: --parse-only wrong level"
    python3 "$RCASSEMBLE" --dry-run "${IMAGES[@]}" \
        | grep -q "dmsetup create" || fail "$level: --dry-run no table"

    attach_loops
    dmsetup remove "$DM_NAME" 2>/dev/null || true
    python3 "$RCASSEMBLE" --name "$DM_NAME" "${LOOPS[@]}" \
        || fail "$level: assembly failed"

    local got_cap
    got_cap="$(blockdev --getsz "/dev/mapper/$DM_NAME")"
    [ "$got_cap" = "$cap" ] || \
        fail "$level: capacity $got_cap != expected $cap"

    local want got
    want="$(md5sum "$WORKDIR/pattern" | cut -d' ' -f1)"
    got="$(dd if="/dev/mapper/$DM_NAME" bs=1M count=$PATTERN_MIB \
              status=none | md5sum | cut -d' ' -f1)"
    [ "$got" = "$want" ] || fail "$level: pattern readback mismatch"

    # Read-only must be enforced.
    if dd if=/dev/zero of="/dev/mapper/$DM_NAME" bs=512 count=1 \
          conv=notrunc status=none 2>/dev/null; then
        fail "$level: write to read-only dm device succeeded"
    fi

    dmsetup remove --retry "$DM_NAME"
    echo "   $level ok (capacity=$cap readback ok, read-only enforced)"
}

run_level raid0 2
# BIOS-native style: raw ChunkSize (384 sectors — representable by NO
# chunk_index) plus a deliberately misleading chunk_index=3 (512).  The
# raw field must take precedence; getting this wrong garbles readback.
run_level raid0 2 --raw-chunk-sectors 384 --chunk-index 3
run_level raid1 2
run_level raid10 4

# Degraded raid10: drop one leg of each pair — must still assemble and
# read back correctly from the surviving legs.
echo "== raid10 degraded (one leg per pair)"
python3 "$RCASSEMBLE" --name "$DM_NAME" "${LOOPS[0]}" "${LOOPS[2]}" \
    || fail "raid10 degraded: assembly failed"
want="$(md5sum "$WORKDIR/pattern" | cut -d' ' -f1)"
got="$(dd if="/dev/mapper/$DM_NAME" bs=1M count=$PATTERN_MIB \
          status=none | md5sum | cut -d' ' -f1)"
[ "$got" = "$want" ] || fail "raid10 degraded: readback mismatch"
dmsetup remove --retry "$DM_NAME"
echo "   raid10 degraded ok"

# A raid10 missing BOTH legs of a pair must be refused.
if python3 "$RCASSEMBLE" --dry-run "${IMAGES[0]}" "${IMAGES[1]}" \
      >/dev/null 2>&1; then
    fail "raid10 with a whole pair missing was not refused"
fi
echo "   raid10 whole-pair-missing correctly refused"

# Corrupt the metadata checksum on one member: parse must fail.
printf '\x00\x11\x22\x33' | dd of="${IMAGES[0]}" bs=1 \
    seek=$((0x5000 * 512)) conv=notrunc status=none
if python3 "$RCASSEMBLE" --parse-only "${IMAGES[0]}" >/dev/null 2>&1; then
    fail "corrupted checksum was not rejected"
fi
echo "   corrupted-checksum rejection ok"

echo "RCASSEMBLE-TEST-PASS"
