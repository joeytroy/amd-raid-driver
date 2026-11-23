#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# rcraid hardware self-test — assemble the firmware array READ-ONLY, prove the
# stripe mapping against reference reads taken before the controllers move,
# then hand the disks back to nvme.
#
#   sudo ./scripts/hw-test-array.sh            # run the test
#   sudo ./scripts/hw-test-array.sh --dry-run  # checks only, changes nothing
#   sudo ./scripts/hw-test-array.sh --keep     # keep the scratch directory
#
# Safe by construction: the module loads with writes disabled (its default),
# nothing is mounted, and a trap restores the controllers on EVERY exit path —
# success, failure, Ctrl-C, or a refused module load.  It refuses to run while
# Secure Boot enforces signatures (the module is unsigned) or while any member
# disk is mounted.
#
# What it proves:
#   - the firmware metadata parses and the volume assembles at the capacity the
#     LD record names;
#   - logical stripe i reads exactly the bytes that member i holds at its
#     UserDataOffset, for two full stripe rounds (element order and user-data
#     offset both come from the metadata, not from a hardcoded guess);
#   - a read at the end of the volume works;
#   - the read-only volume refuses writes;
#   - after the disks go back to nvme, their metadata checksums are unchanged.

set -uo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
LOG=/var/log/rcraid-hw-test.log
SCRATCH=""
DRY_RUN=0
KEEP=0
TOUCHED=0

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --keep)    KEEP=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }

log() {
    printf '%s\n' "$*" | tee -a "$LOG"
}

fail() {
    log "FAIL: $*"
    exit 1
}

secure_boot_enforcing() {
    local sb

    command -v mokutil >/dev/null 2>&1 || return 1
    # Capture instead of piping into grep -q: with pipefail, grep -q
    # exiting early SIGPIPEs mokutil and the pipeline reports 141.
    sb=$(mokutil --sb-state 2>/dev/null || true)
    case "$sb" in
        *"SecureBoot enabled"*) return 0 ;;
    esac
    return 1
}

# --- preconditions -----------------------------------------------------------

: > "$LOG"
log "rcraid hardware self-test — $(date -Is)"
log "repo: $REPO_DIR"

if secure_boot_enforcing; then
    log "BLOCKED: Secure Boot is enforcing and rcraid.ko is unsigned."
    log "         Disable Secure Boot in firmware setup, boot again, re-run."
    exit 3
fi

BDFS=$(lspci -Dn -d 1022:b000 2>/dev/null | awk '{print $1}')
[ -n "$BDFS" ] || fail "no 1022:b000 controllers present"
log "controllers: $(echo "$BDFS" | tr '\n' ' ')"

declare -a MEMBER_DEV=() MEMBER_BDF=()
for blk in /sys/block/nvme*n1; do
    [ -e "$blk" ] || continue
    name=${blk##*/}
    ctrl=$(basename "$(readlink -f "$blk/device")")
    bdf=$(basename "$(readlink -f "/sys/class/nvme/$ctrl/device")")
    case " $(echo "$BDFS" | tr '\n' ' ') " in
        *" $bdf "*)
            # Partitions count: /proc/mounts lists /dev/nvme0n1p2, not the
            # whole disk, and unbinding the controller would rip it away.
            if grep -Eqs "/dev/${name}(p[0-9]+)?[[:space:]]" /proc/mounts ||
               grep -qs "^/dev/${name}" /proc/swaps ||
               [ -n "$(ls -A "/sys/block/$name/holders" 2>/dev/null)" ]; then
                fail "$name is mounted, swapped on, or held by another block device"
            fi
            MEMBER_DEV+=("$name")
            MEMBER_BDF+=("$bdf")
            ;;
    esac
done
[ "${#MEMBER_DEV[@]}" -ge 2 ] || fail "fewer than 2 NVMe members behind 1022:b000"
log "members: ${MEMBER_DEV[*]}"

KO=$REPO_DIR/rcraid.ko
[ -f "$KO" ] || fail "rcraid.ko missing — run 'make' in $REPO_DIR first"
KO_VERMAGIC=$(modinfo -F vermagic "$KO" 2>/dev/null || true)
case "$KO_VERMAGIC" in
    "$(uname -r) "*) : ;;
    *) fail "rcraid.ko was not built for $(uname -r)" ;;
esac
log "module: $KO ($KO_VERMAGIC)"

SUBSYS=$(cat "/sys/bus/pci/devices/${MEMBER_BDF[0]}/subsystem_vendor" 2>/dev/null)
log "subsystem vendor: ${SUBSYS:-unknown}"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/rcraid-hw.XXXXXX") || exit 1

# --- metadata parse (element order, user offsets, capacity) ------------------

parse_meta() {
    # Prints CAPACITY=, CHUNK=, ELEM=, and one DEV=<id>=<block>=<offset>.
    python3 - "$@" <<'PY'
import struct
import sys

MAGIC = 0x65726F4344494152
MD_LBA = 0x5000
TAG_LD = 0x25BD
CHUNK = {0: 128, 1: 128, 2: 256, 3: 512}


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


paths = sys.argv[1:]
first = None
elems = []
for path in paths:
    name = path.rsplit("/", 1)[-1]
    # Every offset below is relative to the metadata LBA, so read from
    # there: 2 MiB covers the commit block and the whole config ring.
    try:
        with open(path, "rb") as f:
            f.seek(MD_LBA * 512)
            data = f.read(2 * 1024 * 1024)
    except OSError as exc:
        sys.exit(f"ERROR={path}: {exc}")
    if len(data) < 2048 or u64(data, 0x08) != MAGIC:
        sys.exit(f"ERROR={path}: no RAIDCore metadata at LBA 0x5000")
    dev_id = u64(data, 0x10)
    gen_lba, gen_len = u32(data, 0x200 + 0x08), u32(data, 0x200 + 0x0C)
    base = (gen_lba - 0x5000) * 512
    if base < 0 or base + gen_len > len(data):
        sys.exit(f"ERROR={path}: active generation @LBA 0x{gen_lba:x} "
                 f"({gen_len} B) lies outside the readable window")
    gen = data[base:base + gen_len]
    vol = None
    for off in range(0x200, max(0x200, len(gen) - 0x114), 4):
        if u32(gen, off) != TAG_LD:
            continue
        devtype, devices = u32(gen, off + 0x0C), u32(gen, off + 0x68)
        first_c, second_c = u32(gen, off + 0x6C), u32(gen, off + 0x70)
        if devtype == 0x1BF9 or devices < 2:
            continue
        if first_c * second_c < devices:
            continue
        vol = off
        break
    if vol is None:
        sys.exit(f"ERROR={path}: no volume LD record in the active generation")
    # This test models RAID 0 only: every logical chunk round-robins
    # across all members.  A mirror or a parity array maps stripes onto
    # columns/redundancy instead, so the per-member compare below would
    # report a false failure.  FirstCount=members, SecondCount=1 is the
    # RAID 0 encoding (rc_ld_level_from in rc_nvme.c).
    first_c = u32(gen, vol + 0x6C)
    second_c = u32(gen, vol + 0x70)
    if first_c != devices or second_c != 1:
        sys.exit(f"ERROR={path}: not a RAID 0 record (FirstCount={first_c} "
                 f"SecondCount={second_c} devices={devices}) — this test "
                 f"checks whole-member striping only")
    cap = u64(gen, vol + 0x50)
    chunk = CHUNK.get(u32(gen, vol + 0x110), 128)
    elem_off = vol + u32(gen, vol + 0x04)
    devices = u32(gen, vol + 0x68)
    order = []
    offsets = {}
    for idx in range(devices):
        pos = elem_off + idx * 64
        if pos + 64 > len(gen):
            break
        ident = u64(gen, pos)
        order.append(ident)
        offsets[ident] = u64(gen, pos + 0x20)
    if first is None:
        first = (cap, chunk, order)
    # Hex, matching ELEM below: the two lists are joined by ID, and a
    # decimal/hex mix would make every element lookup miss.
    print(f"DEV={dev_id:016x}={name}={offsets.get(dev_id, '')}")
print(f"CAPACITY={first[0]}")
print(f"CHUNK={first[1]}")
print("ELEM=" + ",".join(f"{x:016x}" for x in first[2]))
PY
}

META_FILES=()
for name in "${MEMBER_DEV[@]}"; do
    META_FILES+=("/dev/$name")
done
PARSE=$(parse_meta "${META_FILES[@]}") || fail "metadata parse failed: ${PARSE:-no output}"
printf '%s\n' "$PARSE" >> "$LOG"

CAPACITY=$(printf '%s\n' "$PARSE" | sed -n 's/^CAPACITY=//p')
CHUNK=$(printf '%s\n' "$PARSE" | sed -n 's/^CHUNK=//p')
ELEM=$(printf '%s\n' "$PARSE" | sed -n 's/^ELEM=//p')
[ -n "$CAPACITY" ] && [ -n "$CHUNK" ] && [ -n "$ELEM" ] \
    || fail "could not read capacity/chunk/element order from metadata"
log "metadata: capacity=$CAPACITY sectors, chunk=$CHUNK sectors, elements=$ELEM"

declare -A ID_BLOCK=()
while IFS='=' read -r _ id block _; do
    [ -n "${id:-}" ] || continue
    ID_BLOCK[$id]=$block
done < <(printf '%s\n' "$PARSE" | grep '^DEV=')

declare -a ORDER=()
IFS=',' read -r -a ELEM_IDS <<< "$ELEM"
for id in "${ELEM_IDS[@]}"; do
    blk=${ID_BLOCK[$id]:-}
    [ -n "$blk" ] || fail "element $id names a disk that is not bound (missing member)"
    ORDER+=("$blk")
done
log "element order: ${ORDER[*]}"

# --- reference reads, taken while nvme still owns the disks ------------------

for blk in "${MEMBER_DEV[@]}"; do
    off=$(printf '%s\n' "$PARSE" | sed -n "s/^DEV=[^=]*=$blk=//p")
    [ -n "$off" ] || fail "no UserDataOffset for $blk"
    dd if="/dev/$blk" of="$SCRATCH/$blk.c0" bs=512 skip="$off" \
        count="$CHUNK" status=none || fail "reference read failed on $blk"
    dd if="/dev/$blk" of="$SCRATCH/$blk.c1" bs=512 \
        skip=$((off + CHUNK)) count="$CHUNK" status=none \
        || fail "second reference read failed on $blk"
done
log "reference reads captured ($(( ${#MEMBER_DEV[@]} * 2 )) x $((CHUNK / 2)) KiB)"

if [ "$DRY_RUN" = 1 ]; then
    log "DRY RUN: all preconditions met; nothing was bound, loaded or written."
    exit 0
fi

# --- bind + load, with unconditional restore --------------------------------

restore() {
    if [ "$TOUCHED" = 0 ]; then
        if [ "$KEEP" = 1 ]; then
            log "scratch kept at $SCRATCH"
        else
            rm -rf "$SCRATCH"
        fi
        return 0
    fi
    log "== restore =="
    if [ -d /sys/module/rcraid ]; then
        rmmod rcraid 2>>"$LOG" || log "WARN: rmmod rcraid failed"
    fi
    for bdf in "${MEMBER_BDF[@]}"; do
        if [ -e "/sys/bus/pci/devices/$bdf/driver_override" ]; then
            echo "" > "/sys/bus/pci/devices/$bdf/driver_override" 2>/dev/null
        fi
        if [ -d /sys/bus/pci/drivers/rcbottom ] &&
           [ "$(basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver")")" = rcbottom ]; then
            echo "$bdf" > /sys/bus/pci/drivers/rcbottom/unbind 2>>"$LOG"
        fi
    done
    if ! grep -qs "1022 b000" /sys/bus/pci/drivers/nvme/new_id; then
        echo "1022 b000" > /sys/bus/pci/drivers/nvme/new_id 2>>"$LOG"
    fi
    for bdf in "${MEMBER_BDF[@]}"; do
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>>"$LOG"
    done
    sleep 2
    local blk changed=0
    for blk in "${MEMBER_DEV[@]}"; do
        if [ -b "/dev/$blk" ]; then
            log "  /dev/$blk back ($(cat "/sys/block/$blk/size") sectors)"
            dd if="/dev/$blk" of="$SCRATCH/$blk.after" bs=512 \
                skip=20480 count=2 status=none 2>/dev/null
        else
            log "  WARN: /dev/$blk did not come back"
            changed=1
        fi
    done
    if python3 "$REPO_DIR/scripts/qemu-test/verify-checksums.py" \
            "$SCRATCH"/*.after >> "$LOG" 2>&1; then
        log "  firmware metadata unchanged (all checksum blocks match)"
    else
        log "  WARN: metadata checksum verification failed — inspect $LOG"
        changed=1
    fi
    if [ "$KEEP" = 1 ]; then
        log "  scratch kept at $SCRATCH"
    else
        rm -rf "$SCRATCH"
    fi
    return "$changed"
}
on_exit() {
    local rc=$?

    # A failed restore is a failed run even if every read passed: it means
    # the metadata no longer verifies against what the firmware wrote.
    if ! restore; then
        log "RESULT: metadata verification after restore failed"
        [ "$rc" -eq 0 ] && rc=1
    fi
    exit "$rc"
}
trap on_exit EXIT

TOUCHED=1
DMESG_MARK=$(dmesg | wc -l)
log "== loading rcraid (writes disabled) =="
insmod "$KO" "safe_subsys_vendor=$SUBSYS" 2>&1 | tee -a "$LOG"
[ -d /sys/module/rcraid ] || fail "module load refused (see $LOG)"

for bdf in "${MEMBER_BDF[@]}"; do
    log "  binding $bdf -> rcbottom"
    echo rcbottom > "/sys/bus/pci/devices/$bdf/driver_override" || fail "driver_override on $bdf"
    echo "$bdf" > /sys/bus/pci/drivers/nvme/unbind 2>>"$LOG"
    echo "$bdf" > /sys/bus/pci/drivers_probe 2>>"$LOG"
done

for _ in $(seq 1 40); do
    [ -b /dev/rcraid0 ] && break
    sleep 0.25
done
[ -b /dev/rcraid0 ] || fail "/dev/rcraid0 never appeared"

FAIL=0
SIZE=$(cat /sys/block/rcraid0/size)
RO=$(cat /sys/block/rcraid0/ro)
log "== volume =="
log "  size:  $SIZE sectors (expected $CAPACITY)"
log "  ro:    $RO"
log "  state: $(cat /sys/block/rcraid0/rcraid/state 2>/dev/null || echo n/a)"
log "  members:"; sed 's/^/    /' /sys/block/rcraid0/rcraid/members 2>/dev/null >> "$LOG"
lsblk /dev/rcraid0 >> "$LOG" 2>&1
[ "$SIZE" = "$CAPACITY" ] || { log "  size MISMATCH  FAIL"; FAIL=1; }

log "== stripe mapping (two rounds) =="
n=${#ORDER[@]}
for i in $(seq 0 $((n - 1))); do
    lba=$((i * CHUNK))
    dd if=/dev/rcraid0 of="$SCRATCH/vol.$i" bs=512 skip="$lba" \
        count="$CHUNK" status=none || { log "  chunk $i read failed  FAIL"; FAIL=1; continue; }
    if cmp -s "$SCRATCH/vol.$i" "$SCRATCH/${ORDER[$i]}.c0"; then
        log "  chunk $i (LBA $lba): matches ${ORDER[$i]}  PASS"
    else
        log "  chunk $i (LBA $lba): MISMATCH vs ${ORDER[$i]}  FAIL"; FAIL=1
    fi
    lba2=$((n * CHUNK + i * CHUNK))
    dd if=/dev/rcraid0 of="$SCRATCH/volB.$i" bs=512 skip="$lba2" \
        count="$CHUNK" status=none || { log "  chunk $((n + i)) read failed  FAIL"; FAIL=1; continue; }
    if cmp -s "$SCRATCH/volB.$i" "$SCRATCH/${ORDER[$i]}.c1"; then
        log "  chunk $((n + i)) (LBA $lba2): matches ${ORDER[$i]} +$((CHUNK / 2)) KiB  PASS"
    else
        log "  chunk $((n + i)) (LBA $lba2): MISMATCH  FAIL"; FAIL=1
    fi
done

if dd if=/dev/rcraid0 of=/dev/null bs=1M count=4 skip=$((SIZE / 2048 - 4)) status=none; then
    log "  tail read (last 4 MiB): PASS"
else
    log "  tail read (last 4 MiB): FAIL"; FAIL=1
fi

if [ "$RO" = 1 ]; then
    if dd if=/dev/zero of=/dev/rcraid0 bs=4096 count=1 conv=fsync status=none 2>"$SCRATCH/w.err"; then
        log "  write to read-only volume SUCCEEDED  FAIL"; FAIL=1
    else
        log "  write correctly refused: $(tr '\n' ' ' < "$SCRATCH/w.err")"
    fi
else
    log "  volume is read-write (ro=0) — skipping the write test"
fi

log "== driver messages =="
dmesg | tail -n +"$DMESG_MARK" | grep -iE 'rcraid|rcbottom|rc_volume' \
    | tail -30 >> "$LOG"
log "RESULT: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL)"
exit "$FAIL"
