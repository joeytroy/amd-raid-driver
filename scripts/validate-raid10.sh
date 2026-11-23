#!/bin/bash
#
# validate-raid10.sh — one-command RAID 10 certification.
#
# RUN THIS ON THE ARRAY BOX (the machine with the 1022:B000 controller and a
# firmware-created RAID 10 array), NOT on a development laptop.  It runs the
# non-reboot checks from docs/VALIDATION_RAID10.md and prints a single verdict.
# Paste the whole output when asking for sign-off.
#
#   sudo ./scripts/validate-raid10.sh --full --yes
#
# Options:
#   (no --full)  read-only: trust gate, level/geometry, cache-dropped reads.
#   --full       also run the forced fail -> degraded -> rebuild cycle.
#   --no-wipe    with --full, skip test_write_path.sh so the array's data is
#                NOT overwritten.  Failing then re-admitting one mirror copy is
#                data-safe (it is what RAID is for); quiesce the array first.
#                Use this to certify an array you cannot erase.
#   --yes        skip the confirmation prompt.
#   --dev PATH   device to validate (default /dev/rcraid0).
#   --timeout N  rebuild wait, seconds (default: auto from device size).
#
# Exit: 0 = every executed check passed; 1 = a check failed; 2 = precondition.

set -u

DEV=/dev/rcraid0
FULL=0
NO_WIPE=0
ASSUME_YES=0
REBUILD_TIMEOUT=0   # 0 = auto-size from the device
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --full)    FULL=1; shift ;;
        --no-wipe) NO_WIPE=1; shift ;;
        --yes)     ASSUME_YES=1; shift ;;
        --dev)     DEV="$2"; shift 2 ;;
        --timeout) REBUILD_TIMEOUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
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
    echo "      Load the driver / assemble the array first: sudo ./test_driver.sh"
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
[ "$bytes" -gt 0 ] || bytes=$((512 * 2000 * 1000))
dev_mib=$((bytes / 1048576))

if [ "$REBUILD_TIMEOUT" -eq 0 ]; then
    # A full member rebuild copies the whole member.  Assume a conservative
    # 300 MB/s floor (slow QLC over PCIe 3.0 can be slower) plus 10 min slack,
    # with a 15 min minimum so small test arrays still fail fast.
    REBUILD_TIMEOUT=$((bytes / (300 * 1024 * 1024) + 600))
    [ "$REBUILD_TIMEOUT" -lt 900 ] && REBUILD_TIMEOUT=900
fi

echo "RAID 10 certification — $(date -Is)"
echo "device=$DEV  full=$FULL  no-wipe=$NO_WIPE  rebuild-timeout=${REBUILD_TIMEOUT}s"

if [ "$FULL" -eq 1 ] && [ "$ASSUME_YES" -eq 0 ]; then
    if [ "$NO_WIPE" -eq 1 ]; then
        read -r -p "This will fail and rebuild a mirror member of $DEV (data preserved). Type 'yes' to continue: " a
    else
        read -r -p "This will DESTROY data on $DEV. Type 'yes' to continue: " a
    fi
    [ "$a" = yes ] || { echo "aborted"; exit 2; }
fi

# ---------------------------------------------------------------- 0. trust
hdr "0. Trust gate"
if dmesg | grep -qi UNTRUSTED; then
    bad "kernel vetoed the geometry (see 'dmesg | grep -i UNTRUSTED')"
else
    ok "no UNTRUSTED geometry veto"
fi

# ---------------------------------------------------------------- 1. geometry
hdr "1. Level and geometry"
level="$(awk -F': ' '/^level:/{print $2}' "$DBG" 2>/dev/null)"
n="$(awk -F': ' '/^members:/{print $2}' "$DBG" 2>/dev/null)"
[ -z "$level" ] && level="unknown"
[ -z "$n" ] && n=0

if [ "$level" = raid10 ]; then
    ok "level=$level"
else
    bad "level=$level (expected raid10)"
fi

if [ "$n" -ge 4 ] && [ $((n % 2)) -eq 0 ]; then
    ok "members=$n (mirror columns=$((n / 2)))"
else
    bad "members=$n (RAID10 needs an even count >= 4)"
fi

echo "  ---- debugfs /sys/kernel/debug/rcraid/volume ----"
sed 's/^/  /' "$DBG" 2>/dev/null || note "debugfs not mounted?"

# ---------------------------------------------------------------- 1b. reads
hdr "1b. Cache-dropped direct reads"
sync
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
for frac in 0 50 100; do
    off=$((dev_mib * frac / 100))
    if [ "$off" -gt 16 ]; then off=$((off - 16)); else off=0; fi
    if dd if="$DEV" of=/dev/null bs=1M count=16 skip="$off" iflag=direct status=none; then
        ok "16 MiB O_DIRECT read @${frac}% (skip=${off}MiB)"
    else
        bad "O_DIRECT read of $DEV @${frac}% failed"
    fi
done

# ---------------------------------------------------------------- 2. writes
if [ "$FULL" -eq 1 ] && [ "$NO_WIPE" -eq 0 ]; then
    hdr "2. Write-path torture (test_write_path.sh)"
    if "$SCRIPT_DIR/../test_write_path.sh" --yes "$DEV"; then
        ok "test_write_path.sh (fragmented writes, mkfs, dmesg tripwire)"
    else
        bad "test_write_path.sh"
    fi
elif [ "$FULL" -eq 1 ]; then
    hdr "2. Write-path torture"
    note "skipped (--no-wipe): array data left intact"
fi

# ---------------------------------------------------------------- 3. rebuild
if [ "$FULL" -eq 1 ]; then
    hdr "3. Fail -> degraded -> rebuild"

    st="$(cat "$SYS/state")"
    ok_before=0
    [ "$st" = optimal ] && ok_before=1

    if [ "$ok_before" -ne 1 ]; then
        bad "state=$st (need 'optimal' to start the failover cycle)"
    else
        slot=0
        partner=$((slot ^ 1))
        bdf="$(awk -v s="$slot" '$1==s{print $2}' "$SYS/members")"
        pbdf="$(awk -v s="$partner" '$1==s{print $2}' "$SYS/members")"
        note "pair under test: member $slot ($bdf) / member $partner ($pbdf)"

        echo "$slot" > "$SYS/fail_member"
        sleep 2

        st="$(cat "$SYS/state")"
        if [ "$st" = degraded ]; then
            ok "state=degraded after failing member $slot"
        else
            bad "state=$st after failing member $slot (expected degraded)"
        fi

        if dd if="$DEV" of=/dev/null bs=1M count=8 iflag=direct status=none; then
            ok "reads still served by the survivor while degraded"
        else
            bad "reads failed while degraded"
        fi

        note "re-admitting member $slot via $bdf reset"
        echo 1 > "/sys/bus/pci/devices/$bdf/rcraid/reset" \
            || bad "reset write failed for $bdf"

        saw_resync=0
        deadline=$((SECONDS + REBUILD_TIMEOUT))
        st=""
        while [ "$SECONDS" -lt "$deadline" ]; do
            st="$(cat "$SYS/state")"
            rp="$(cat "$SYS/resync_progress")"
            [ "$rp" != none ] && saw_resync=1
            note "state=$st  resync=$rp"
            if [ "$st" = optimal ] && [ "$saw_resync" -eq 1 ]; then
                break
            fi
            if [ "$st" = optimal ] && [ "$saw_resync" -eq 0 ] \
               && [ "$SECONDS" -gt $((deadline - REBUILD_TIMEOUT + 30)) ]; then
                break
            fi
            sleep 5
        done

        if [ "$st" = optimal ] && [ "$saw_resync" -eq 1 ]; then
            ok "member rebuilt; volume back to optimal"
        elif [ "$st" = optimal ]; then
            bad "volume optimal but no resync was ever observed (did it rebuild?)"
        else
            bad "volume did not return to optimal within ${REBUILD_TIMEOUT}s (state=$st)"
        fi
    fi
fi

# ---------------------------------------------------------------- manual
hdr "Still manual (cannot be scripted)"
cat <<'EOF'
  - Whole-pair fatal boundary: fail BOTH copies of one pair; confirm
    state=failed and new I/O fast-fails; reset both to recover.
  - Boot-degraded assembly: with allow_degraded=1, remove one copy of a pair,
    reboot; confirm degraded assembly and rebuild on return.  Remove both
    copies of a pair and confirm assembly REFUSES (no /dev/rcraid0).
  - Install Kubuntu onto the array, reboot, then: scripts/verify-boot-safety.sh
EOF

# ---------------------------------------------------------------- verdict
hdr "VERDICT"
for line in "${SUMMARY[@]}"; do echo "  $line"; done
echo
if [ "$FAILED" -ne 0 ]; then
    echo "RESULT: FAILED — see the FAIL line(s) above; paste this output for triage."
    exit 1
fi
if [ "$FULL" -eq 1 ]; then
    if [ "$NO_WIPE" -eq 1 ]; then
        echo "RESULT: RAID10 VALIDATED (automated checks; --no-wipe skipped write torture)."
    else
        echo "RESULT: RAID10 VALIDATED (automated checks)."
    fi
    echo "        Complete the manual steps, then paste this output to promote"
    echo "        the level to 'stable'."
else
    echo "RESULT: read-only checks passed. Re-run with --full --yes to complete"
    echo "        certification (destructive)."
fi
exit 0
