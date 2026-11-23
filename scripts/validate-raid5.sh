#!/bin/bash
#
# validate-raid5.sh — read-only RAID 5 certification checks.
#
# RUN THIS ON THE ARRAY BOX (the machine with the 1022:B000 controller and a
# RAIDXpert2-created RAID 5 array), NOT on a development laptop.  RAID 5/6 is
# assembled READ-ONLY: there is no parity write path and no rebuild.  The
# on-disk placement is settled against the vendor core (issue #6) but has not
# been corroborated on vendor-written data.  This script therefore never
# writes to the array — it verifies that writes are REFUSED.
#
#   sudo ./scripts/validate-raid5.sh
#   sudo ./scripts/validate-raid5.sh --full-read    # sequential-read the
#                                                   # whole volume (many GB —
#                                                   # can take hours)
#   sudo ./scripts/validate-raid5.sh --dev /dev/rcraid0
#
# Exit: 0 = every check passed; 1 = a check failed; 2 = precondition.

set -u
set -o pipefail

DEV=/dev/rcraid0
FULL_READ=0
SAMPLE_MIB=16

while [ $# -gt 0 ]; do
    case "$1" in
        --dev)       DEV="$2"; shift 2 ;;
        --full-read) FULL_READ=1; shift ;;
        --sample)    SAMPLE_MIB="$2"; shift 2 ;;
        -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

FAILED=0
declare -a SUMMARY

ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$*"; SUMMARY+=("PASS  $*"); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; SUMMARY+=("FAIL  $*"); FAILED=1; }
note() { printf '  ....  %s\n' "$*"; }
hdr()  { printf '\n== %s ==\n' "$*"; }

# ---------------------------------------------------------------- preflight
if [ ! -b "$DEV" ]; then
    echo "FAIL: $DEV is not a block device."
    echo "      Assemble the array first (load rcraid; the members must be"
    echo "      bound to rcbottom, not nvme)."
    exit 2
fi

BASENAME="$(basename "$DEV")"
SYS="/sys/block/$BASENAME/rcraid"
DBG="/sys/kernel/debug/rcraid/volume"

if [ ! -d "$SYS" ]; then
    echo "FAIL: $SYS not found — is the rcraid volume assembled?"
    exit 2
fi

bytes="$(blockdev --getsize64 "$DEV" 2>/dev/null || echo 0)"
if [ "$bytes" -le 0 ]; then
    echo "FAIL: cannot read the size of $DEV"
    exit 2
fi
sectors=$((bytes / 512))
dev_mib=$((bytes / 1048576))

if [ "$dev_mib" -le $((SAMPLE_MIB * 2)) ]; then
    echo "FAIL: $DEV is too small (${dev_mib} MiB) for the sampled reads."
    exit 2
fi

# Parse the driver's one-time LD record line.  It carries the raw on-disk
# geometry the checks below compare against, and is emitted for every member.
ldline="$(dmesg | grep 'rc_volume_parse_logical_device:' | tail -1 || true)"
ld_level=""; ld_devtype=""; ld_devices=""; ld_capacity=""; ld_usersz=""; ld_cidx=""
if [ -n "$ldline" ]; then
    [[ "$ldline" =~ level=([A-Z0-9]+) ]]                 && ld_level="${BASH_REMATCH[1]}"
    [[ "$ldline" =~ devtype=0x([0-9a-fA-F]+) ]]          && ld_devtype="${BASH_REMATCH[1]}"
    [[ "$ldline" =~ devices=([0-9]+) ]]                  && ld_devices="${BASH_REMATCH[1]}"
    [[ "$ldline" =~ capacity=([0-9]+) ]]                 && ld_capacity="${BASH_REMATCH[1]}"
    [[ "$ldline" =~ user_sz=([0-9]+) ]]                  && ld_usersz="${BASH_REMATCH[1]}"
    [[ "$ldline" =~ chunk_idx=([0-9]+) ]]                && ld_cidx="${BASH_REMATCH[1]}"
fi

echo "RAID 5 validation — $(date -Is)"
echo "device=$DEV  full-read=$FULL_READ  sample=${SAMPLE_MIB}MiB  size=${dev_mib}MiB"

# ---------------------------------------------------------------- 0. trust
hdr "0. Trust gate"
all_untrusted="$(dmesg | grep -i 'UNTRUSTED' || true)"
unexpected=""
if [ -n "$all_untrusted" ]; then
    unexpected="$(printf '%s\n' "$all_untrusted" | grep -v 'RAID 5/6 is assembled READ-ONLY' || true)"
fi
if [ -n "$unexpected" ]; then
    bad "unexpected geometry veto(s) — see 'dmesg | grep -i UNTRUSTED'"
    printf '%s\n' "$unexpected" | sed 's/^/        /'
else
    ok "no unexpected geometry veto"
    # `grep -c` consumes all input; `grep -q` exits on the first match, which
    # SIGPIPEs the writer and — under `set -o pipefail` — turns a present match
    # into exit 141.  That silently made this check always report "not found".
    if printf '%s\n' "$all_untrusted" | grep -c 'RAID 5/6 is assembled READ-ONLY' >/dev/null; then
        note "read-only veto present and expected: parity write path not implemented"
    fi
fi

# ---------------------------------------------------------------- 1. geometry
hdr "1. Level and geometry"

level="$(awk -F': ' '/^level:/{print $2}' "$DBG" 2>/dev/null || true)"
members="$(awk -F': ' '/^members:/{print $2}' "$DBG" 2>/dev/null || true)"
state="$(cat "$SYS/state" 2>/dev/null || true)"
live="$(cat "$SYS/members" 2>/dev/null | awk '$3=="live"' | wc -l || echo 0)"
[ -z "$level" ]   && level="unknown"
[ -z "$members" ] && members=0

if [ "$level" = raid5 ]; then
    ok "level=raid5"
else
    bad "level=$level (expected raid5)"
fi

if [ "$members" -ge 3 ]; then
    ok "members=$members (RAID5 needs >= 3)"
else
    bad "members=$members (RAID5 needs >= 3)"
fi

if [ "$state" = optimal ] && [ "$live" -eq "$members" ] && [ "$members" -gt 0 ]; then
    ok "state=optimal, all $members members live"
else
    bad "state=$state, live=$live/$members (read-only RAID5 cannot serve degraded)"
fi

if [ -n "$ldline" ]; then
    if [ "$ld_level" = RAID5 ] && [ "$ld_devtype" = 1bf5 ]; then
        ok "on-disk LD: devtype=0x1bf5 (RAID5), level=$ld_level, devices=$ld_devices"
    else
        bad "on-disk LD: devtype=0x$ld_devtype level=$ld_level (expected 0x1bf5 / RAID5)"
    fi

    if [ -n "$ld_devices" ] && [ "$ld_devices" = "$members" ]; then
        ok "LD devices ($ld_devices) matches assembled member count"
    else
        bad "LD devices ($ld_devices) != assembled member count ($members)"
    fi

    if [ -n "$ld_capacity" ] && [ -n "$ld_usersz" ] && [ -n "$ld_devices" ]; then
        expected=$((ld_usersz * (ld_devices - 1)))
        if [ "$expected" -eq "$ld_capacity" ]; then
            ok "capacity = (N-1) x per-member user size ($ld_capacity sectors)"
        else
            bad "capacity $ld_capacity != (N-1) x user_sz $ld_usersz = $expected"
        fi
        if [ "$ld_capacity" -eq "$sectors" ]; then
            ok "block-device size matches the LD capacity ($sectors sectors, $dev_mib MiB)"
        else
            bad "block-device size $sectors sectors != LD capacity $ld_capacity"
        fi
    else
        note "LD capacity/user-size fields unavailable — size cross-check skipped"
    fi

    if [ -n "$ld_cidx" ] && [ "$ld_cidx" -ge 1 ] && [ "$ld_cidx" -le 3 ]; then
        ok "chunk_idx=$ld_cidx (known stripe encoding)"
    else
        note "chunk_idx=$ld_cidx — resolved stripe not one of the documented 1..3 encodings"
    fi
else
    note "no 'rc_volume_parse_logical_device:' line in dmesg — geometry parsed from debugfs only"
fi

echo "  ---- $DBG ----"
sed 's/^/  /' "$DBG" 2>/dev/null || note "debugfs not mounted?"
echo "  ---- $SYS/members ----"
sed 's/^/  /' "$SYS/members" 2>/dev/null || true

# ---------------------------------------------------------------- 2. read-only
hdr "2. Read-only enforcement (writes must be refused)"

ro="$(blockdev --getro "$DEV" 2>/dev/null || echo '?')"
if [ "$ro" = 1 ]; then
    ok "block device is read-only (blockdev --getro=1)"
else
    bad "block device is NOT read-only (blockdev --getro=$ro)"
fi

if dd if=/dev/zero of="$DEV" bs=4096 count=1 oflag=direct conv=notrunc \
      status=none 2>/dev/null; then
    bad "a write to $DEV SUCCEEDED — the read-only contract is broken"
else
    ok "write attempt was refused (expected)"
fi

if dmesg | grep -c 'RAID 5/6 is assembled READ-ONLY' >/dev/null; then
    ok "driver logged the RAID5 read-only veto"
else
    note "read-only veto line not found in dmesg (veto may come from enable_writes=0)"
fi

# ---------------------------------------------------------------- 3. reads
hdr "3. Cache-dropped direct reads and repeatability"

hash16() {
    dd if="$DEV" bs=1M count="$SAMPLE_MIB" skip="$1" iflag=direct status=none \
        | sha256sum | awk '{print $1}'
}

drop_caches() {
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

read_ok=1
last_mib=$((dev_mib - SAMPLE_MIB))
for frac in 0 25 50 75 100; do
    if [ "$frac" -eq 100 ]; then
        off=$last_mib
    else
        off=$((dev_mib * frac / 100))
    fi
    drop_caches
    h1="$(hash16 "$off")" || { bad "O_DIRECT read @${frac}% (skip=${off}MiB) failed"; read_ok=0; continue; }
    drop_caches
    h2="$(hash16 "$off")" || { bad "repeat O_DIRECT read @${frac}% failed"; read_ok=0; continue; }
    if [ "$h1" = "$h2" ]; then
        ok "${SAMPLE_MIB} MiB read @${frac}% (skip=${off}MiB) is stable across a cache drop"
    else
        bad "read @${frac}% returned different data after a cache drop"
        read_ok=0
    fi
done

# Freshly initialized arrays read as zeros; say so, because a zero-only pass
# exercises the mapping but proves nothing about parity placement.
zero_bytes="$(dd if="$DEV" bs=1M count=4 iflag=direct status=none \
              | tr -d '\0' | wc -c || true)"
if [ "$zero_bytes" -eq 0 ]; then
    note "first 4 MiB are all zero — the array looks freshly initialized;"
    note "this pass proves the read path, not the parity placement (issue #6)"
else
    note "first 4 MiB contain data ($zero_bytes non-NUL bytes) — a layout check"
    note "against a known checksum is meaningful here (see VALIDATION_RAID5.md)"
fi

if [ "$FULL_READ" -eq 1 ] && [ "$read_ok" -eq 1 ]; then
    hdr "3b. Full sequential O_DIRECT read (this can take a long time)"
    if dd if="$DEV" of=/dev/null bs=4M iflag=direct status=progress; then
        ok "whole volume read without an I/O error"
    else
        bad "full-volume read failed"
    fi
elif [ "$FULL_READ" -eq 1 ]; then
    note "full read skipped because a sampled read failed"
fi

# ---------------------------------------------------------------- 4. tripwire
hdr "4. Kernel tripwire"
newerr="$(dmesg | grep -iE 'rcraid|rc_volume' \
          | grep -iE 'I/O error|timeout|reset|dead' | tail -5 || true)"
if [ -z "$newerr" ]; then
    ok "no rcraid I/O error / timeout / reset messages"
else
    bad "rcraid error messages present:"
    printf '%s\n' "$newerr" | sed 's/^/        /'
fi

# ---------------------------------------------------------------- manual
hdr "Still manual / not implemented"
cat <<'EOF'
  - Parity PLACEMENT (issue #6): write a known pattern + checksum through the
    vendor path (RAIDXpert2/Windows driver, or a pre-existing filesystem),
    boot Linux, and read it back through $DEV.  Only that confirms the
    vendor's dedicated-parity map against real firmware.
  - Parity write path, member rebuild, and degraded reads are NOT implemented:
    fail_member on a RAID5 member takes the volume to 'failed'; there is no
    reconstruction path to recover it.  Do not run the RAID10 rebuild steps.
  - Boot-from-array RAID5 is out of scope until writes exist.
EOF

# ---------------------------------------------------------------- verdict
hdr "VERDICT"
for line in "${SUMMARY[@]}"; do echo "  $line"; done
echo
if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAILED — see the FAIL line(s) above; paste this output for triage."
    exit 1
fi
echo "RESULT: read-only RAID5 checks passed."
echo "        Layout confirmation and any promotion require the manual steps above."
exit 0
