#!/bin/bash
# Boot rcraid in a QEMU VM against synthetic virtual-NVMe arrays.
#
# Builds rcraid.ko against the running kernel, creates member images,
# writes RAIDCore metadata with mkmeta.py, packs a busybox initramfs
# whose /init assembles the array and runs a write/readback test, then
# boots the HOST's own kernel in QEMU (so the module always matches).
# Exit 0 iff the guest printed RCRAID-TEST-PASS.
#
# No root needed, with one caveat: Ubuntu ships /boot/vmlinuz-* mode
# 0600.  Either run once with a readable copy:
#     sudo install -m 644 /boot/vmlinuz-$(uname -r) /somewhere/vmlinuz
#     ./run-qemu-raid-test.sh --kernel /somewhere/vmlinuz
# or chmod the one in /boot.  KVM is used when /dev/kvm is accessible,
# otherwise falls back to TCG (slow but works, e.g. in CI).
#
# Usage:
#   run-qemu-raid-test.sh [--kernel <vmlinuz>] [--kver <version>]
#                         [--level raid0|raid1|raid5|raid10]
#                         [--parity-writes] [--create-test]
#                         [--members N] [--size-mib M] [--workdir DIR]
#                         [--writer mkmeta|encoder] [--chunk-index 0-3]
#
# The statically linked busybox packed into the initramfs is found
# automatically; set BUSYBOX=/path/to/busybox to force a specific binary.
#
# --kver builds the module against /lib/modules/<version>/build and boots
# /boot/vmlinuz-<version> (unless --kernel overrides), so the test can run
# against an installed kernel that is NOT the running one — e.g. CI runners
# whose own kernel is older than the driver's minimum: install the HWE
# kernel + headers as packages and point --kver at it, no reboot needed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KVER="$(uname -r)"
KERNEL=""
LEVEL=raid0
MEMBERS=2
SIZE_MIB=1024
WORKDIR=""
WRITER=mkmeta
CHUNK_INDEX=3
FAIL_MEMBER=0
OMIT_MEMBER=0
PARITY_WRITES=0
CREATE_TEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)   KERNEL="$2"; shift 2 ;;
        --kver)     KVER="$2"; shift 2 ;;
        --level)    LEVEL="$2"; shift 2 ;;
        --members)  MEMBERS="$2"; shift 2 ;;
        --size-mib) SIZE_MIB="$2"; shift 2 ;;
        --workdir)  WORKDIR="$2"; shift 2 ;;
        # Which encoder writes the images: mkmeta.py (Python, default) or
        # "encoder" for src/stamp_meta_userspace.c, the driver's own C
        # encoder via rc_meta_build.h.  The latter proves the writer
        # primitive that rxp2adm CREATE will be built on actually assembles.
        --writer)   WRITER="$2"; shift 2 ;;
        --chunk-index) CHUNK_INDEX="$2"; shift 2 ;;
        --fail-member) FAIL_MEMBER=1; shift ;;
        # Degraded BOOT: write metadata for the full member set but attach
        # all-but-the-last drive to the VM; the guest loads with
        # allow_degraded=1 and asserts the volume assembles degraded after
        # the fallback delay.
        --omit-member) OMIT_MEMBER=1; shift ;;
        # Exercise the opt-in RAID 5 full-row write path
        # (parity_writes=1): one complete row must write and read back,
        # a partial row must be refused.
        --parity-writes) PARITY_WRITES=1; shift ;;
        # Delete the synthetic RAID 5 through our own CLI, then create it
        # again through the driver's metadata writer and verify reassembly.
        --create-test) CREATE_TEST=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if { [ "$FAIL_MEMBER" = 1 ] || [ "$OMIT_MEMBER" = 1 ]; } && [ "$LEVEL" != raid1 ]; then
    echo "--fail-member/--omit-member require --level raid1 (RAID0 has no degraded mode)" >&2
    exit 2
fi
if [ "$PARITY_WRITES" = 1 ] && [ "$LEVEL" != raid5 ]; then
    echo "--parity-writes requires --level raid5" >&2
    exit 2
fi
if [ "$CREATE_TEST" = 1 ] && [ "$LEVEL" != raid5 ]; then
    echo "--create-test requires --level raid5" >&2
    exit 2
fi
[ -n "$KERNEL" ] || KERNEL="/boot/vmlinuz-$KVER"

[ -d "/lib/modules/$KVER/build" ] || {
    echo "no kernel headers at /lib/modules/$KVER/build" >&2
    exit 2
}

[ -r "$KERNEL" ] || {
    echo "kernel image $KERNEL is not readable." >&2
    echo "  sudo install -m 644 /boot/vmlinuz-\$(uname -r) <dir>/vmlinuz" >&2
    echo "  $0 --kernel <dir>/vmlinuz" >&2
    exit 2
}
command -v qemu-system-x86_64 >/dev/null || {
    echo "qemu-system-x86_64 not found (install qemu-system-x86)" >&2
    exit 2
}
# The initramfs has no libc, so the busybox packed into it must be static.
# `command -v busybox` resolves by PATH order and typically returns the
# DYNAMIC /usr/bin/busybox from busybox-initramfs, which fails the check
# below even though /usr/bin/busybox-static is installed alongside it.  Probe
# the static names as well, and let BUSYBOX pick an explicit binary.
BUSYBOX=""
BUSYBOX_DYNAMIC=""
for cand in "${BUSYBOX:-}" "$(command -v busybox || true)" \
            /usr/bin/busybox-static /bin/busybox-static /usr/bin/busybox; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    if file "$cand" | grep -q "statically linked"; then
        BUSYBOX="$cand"
        break
    fi
    BUSYBOX_DYNAMIC="${BUSYBOX_DYNAMIC:+$BUSYBOX_DYNAMIC }$cand"
done
if [ -z "$BUSYBOX" ]; then
    echo "no statically linked busybox found (install busybox-static)" >&2
    [ -n "$BUSYBOX_DYNAMIC" ] && \
        echo "  rejected, dynamically linked: $BUSYBOX_DYNAMIC" >&2
    exit 2
fi
echo "==> busybox: $BUSYBOX"

if [ -z "$WORKDIR" ]; then
    WORKDIR="$(mktemp -d /tmp/rcraid-qemu.XXXXXX)"
    trap 'rm -rf "$WORKDIR"' EXIT
fi
mkdir -p "$WORKDIR"
echo "==> workdir: $WORKDIR"

# ----------------------------------------------------------------------------
# 1. Build the module against the target kernel (default: the running one).
# ----------------------------------------------------------------------------
echo "==> building rcraid.ko against $KVER"
make -C "/lib/modules/$KVER/build" M="$REPO_DIR" modules \
    > "$WORKDIR/build.log" 2>&1 || {
    tail -n 30 "$WORKDIR/build.log" >&2
    echo "module build failed — see $WORKDIR/build.log" >&2
    exit 1
}

# ----------------------------------------------------------------------------
# 2. Member images + synthetic metadata.
# ----------------------------------------------------------------------------
echo "==> creating $MEMBERS member images (${SIZE_MIB} MiB each), level=$LEVEL"
IMAGES=()
for i in $(seq 0 $((MEMBERS - 1))); do
    img="$WORKDIR/member$i.img"
    rm -f "$img"
    truncate -s "${SIZE_MIB}M" "$img"
    IMAGES+=("$img")
done

case "$WRITER" in
    mkmeta)
        MKMETA_OUT="$(python3 "$SCRIPT_DIR/mkmeta.py" --level "$LEVEL" \
            --chunk-index "$CHUNK_INDEX" "${IMAGES[@]}")" ;;
    encoder)
        make -s -C "$REPO_DIR" stamp-tool \
            STAMP_TOOL_BIN="$WORKDIR/rcstamp" > /dev/null || {
            echo "failed to build the encoder stamping tool" >&2
            exit 1
        }
        MKMETA_OUT="$("$WORKDIR/rcstamp" --level "$LEVEL" \
            --chunk-index "$CHUNK_INDEX" "${IMAGES[@]}")" ;;
    *)
        echo "--writer must be mkmeta or encoder" >&2
        exit 2 ;;
esac
echo "$MKMETA_OUT" | sed 's/^/    /'
EXPECTED_SECTORS="$(echo "$MKMETA_OUT" | awk -F= '/^capacity_sectors=/ {print $2}')"
[ -n "$EXPECTED_SECTORS" ] || { echo "mkmeta produced no capacity" >&2; exit 1; }

# RAID0/RAID10: hand the guest the geometry for the split-iovec test, the
# only way to force a non-first SG entry at an in-page offset (dd's layout
# is always page-aligned and cannot reach that hole, issue #20).
# PRP_COLS is the stripe column count: every member for RAID0, half of
# them for RAID10.
PRP_CHUNK_BYTES=0
PRP_COLS=0
if [ "$LEVEL" = raid0 ] || [ "$LEVEL" = raid10 ]; then
    PRP_CHUNK_SECTORS="$(printf '%s\n' "$MKMETA_OUT" | grep -o 'chunk_sectors=[0-9]*' | cut -d= -f2 | head -n1)"
    [ -n "$PRP_CHUNK_SECTORS" ] || { echo "$LEVEL: no chunk_sectors from the writer" >&2; exit 1; }
    PRP_CHUNK_BYTES=$(( PRP_CHUNK_SECTORS * 512 ))
    if [ "$LEVEL" = raid10 ]; then
        PRP_COLS=$(( MEMBERS / 2 ))
    else
        PRP_COLS=$MEMBERS
    fi
    # The trap only exists while the whole request stays one request:
    # blk-mq splits at max_hw_sectors (1 MiB) and max_segments (128).
    if [ $(( PRP_CHUNK_BYTES * (PRP_COLS + 1) )) -gt 1048576 ] || \
       [ $(( PRP_CHUNK_BYTES / 4096 * (PRP_COLS + 1) )) -gt 128 ]; then
        echo "==> split-iovec test skipped: ${PRP_CHUNK_BYTES}B x $((PRP_COLS + 1)) stripes exceeds one request"
        PRP_CHUNK_BYTES=0
        PRP_COLS=0
    fi
fi

# A full RAID 5 row is chunk x data members.  Pass it to the guest so the
# parity-write scenario writes exactly one row: anything shorter is a
# partial row and must be refused by the driver.
PARITY_ROW_KIB=0
if [ "$PARITY_WRITES" = 1 ]; then
    CHUNK_SECTORS="$(printf '%s\n' "$MKMETA_OUT" | grep -o 'chunk_sectors=[0-9]*' | cut -d= -f2 | head -n1)"
    [ -n "$CHUNK_SECTORS" ] || { echo "parity-writes: no chunk_sectors from the writer" >&2; exit 1; }
    PARITY_ROW_KIB=$(( CHUNK_SECTORS * (MEMBERS - 1) / 2 ))
    if [ "$PARITY_ROW_KIB" -le 0 ] || [ "$PARITY_ROW_KIB" -gt 1024 ]; then
        echo "parity-writes: a row is ${PARITY_ROW_KIB} KiB; the driver writes at most 1024 KiB" >&2
        exit 2
    fi
fi

# ----------------------------------------------------------------------------
# 3. Busybox initramfs: /init + the module.
# ----------------------------------------------------------------------------
echo "==> packing initramfs"
INITRD_DIR="$WORKDIR/initramfs"
rm -rf "$INITRD_DIR"
mkdir -p "$INITRD_DIR/bin" "$INITRD_DIR/sbin" "$INITRD_DIR/proc" \
         "$INITRD_DIR/sys" "$INITRD_DIR/dev"
cp "$BUSYBOX" "$INITRD_DIR/bin/busybox"
ln -s busybox "$INITRD_DIR/bin/sh"    # /init's shebang needs a shell to exist
cp "$REPO_DIR/rcraid.ko" "$INITRD_DIR/rcraid.ko"

# Bundle mke2fs (+ the shared libraries it needs) when the host has it.
# Real mkfs is the only faithful generator of sub-page buffer-head
# writeback — the bvec pattern that PRPs can't express unless the driver
# merges contiguous fragments.  dd-style tests are always page-aligned and
# CANNOT catch that class (it shipped: mke2fs failed every RAID1 mirror
# write with SC 0x13 on real hardware while the whole rig battery passed).
add_binary() {
    install -D "$1" "$INITRD_DIR$1"
    ldd "$1" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if ($i ~ /^\//) print $i}' \
        | sort -u | while read -r lib; do
        install -D "$lib" "$INITRD_DIR$lib" 2>/dev/null || true
    done
}
# Probe the system paths first: the guest only looks in /usr/sbin and /sbin,
# so a `mke2fs` that wins on PATH from somewhere else (an Android SDK ships
# one) gets bundled at its own absolute path and the guest silently skips
# the sub-page test — the one that catches unmerged PRP segments.
MKE2FS=""
for cand in /usr/sbin/mke2fs /sbin/mke2fs "$(command -v mke2fs || true)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    MKE2FS="$cand"
    break
done
if [ -n "$MKE2FS" ]; then
    add_binary "$MKE2FS"
    echo "==> bundled $MKE2FS for the sub-page buffered-write test"
else
    echo "==> mke2fs not found on host — guest will skip the sub-page write test"
fi
# Aligned O_DIRECT helper for the RAID 5 full-row write test.  dd's
# buffer is not page-aligned, so blk-mq's virt_boundary turns the write
# into page-sized requests, which are (correctly) refused as partial rows.
if [ "$PARITY_WRITES" = 1 ]; then
    gcc -static -O2 -Wall -Wextra -Werror -o "$WORKDIR/rcparitytest" \
        "$SCRIPT_DIR/parity-write-test.c" || {
        echo "failed to build the RAID 5 parity-write helper" >&2
        exit 1
    }
    install -m 0755 "$WORKDIR/rcparitytest" "$INITRD_DIR/rcparitytest"
    echo "==> bundled rcparitytest for the RAID 5 full-row write test"
fi
# SG_IO replay harness for /dev/rc_api (issue #13): drives the sequence
# rcadm issues and asserts the packets field by field, so the read-half ABI
# is covered without bundling AMD's binary.  Small and always bundled.
gcc -static -O2 -Wall -Wextra -Werror -o "$WORKDIR/rcapireadtest" \
    "$SCRIPT_DIR/api-read-test.c" || {
    echo "failed to build the /dev/rc_api replay harness" >&2
    exit 1
}
install -m 0755 "$WORKDIR/rcapireadtest" "$INITRD_DIR/rcapireadtest"
# Split-iovec helper (issue #20): a pwritev whose third iovec starts 2 KiB
# into a page, laid out so one RAID10 column gets the mid-page fragment.
# Nothing else in the battery builds a non-page-aligned bvec after PRP1.
gcc -static -O2 -Wall -Wextra -Werror -o "$WORKDIR/rciovecsplit" \
    "$SCRIPT_DIR/iovec-split-test.c" || {
    echo "failed to build the split-iovec helper" >&2
    exit 1
}
install -m 0755 "$WORKDIR/rciovecsplit" "$INITRD_DIR/rciovecsplit"
if [ "$CREATE_TEST" = 1 ]; then
    gcc -static -O2 -Wall -Wextra -Werror -o "$WORKDIR/rxp2adm" \
        "$REPO_DIR/src/rxp2adm.c" -I"$REPO_DIR/src" || {
        echo "failed to build the RAID 5 create/delete CLI" >&2
        exit 1
    }
    install -m 0755 "$WORKDIR/rxp2adm" "$INITRD_DIR/rxp2adm"
    echo "==> bundled rxp2adm for the RAID 5 create/delete test"
fi
install -m 0755 "$SCRIPT_DIR/guest-init.sh" "$INITRD_DIR/init"
(cd "$INITRD_DIR" && find . | cpio -o -H newc --quiet | gzip -1) \
    > "$WORKDIR/initramfs.gz"

# ----------------------------------------------------------------------------
# 4. Boot.  Each member is its own NVMe controller (mirrors real topology:
#    one PCI function per drive).
# ----------------------------------------------------------------------------
ACCEL_ARGS=(-accel tcg)
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_ARGS=(-enable-kvm -cpu host)
else
    echo "==> /dev/kvm not accessible — using TCG (slow)"
fi

ATTACH=$MEMBERS
if [ "$OMIT_MEMBER" = 1 ]; then
    ATTACH=$((MEMBERS - 1))
    echo "==> degraded boot: attaching only $ATTACH of $MEMBERS members"
fi
DRIVE_ARGS=()
for i in $(seq 0 $((ATTACH - 1))); do
    DRIVE_ARGS+=(
        -drive "file=${IMAGES[$i]},if=none,format=raw,id=d$i"
        -device "nvme,drive=d$i,serial=RCTEST$i"
    )
done

CONSOLE_LOG="$WORKDIR/console.log"
echo "==> booting VM (console → $CONSOLE_LOG)"
timeout --foreground "${RCRAID_QEMU_TIMEOUT:-300}" qemu-system-x86_64 \
    -M q35 -m 2048 -smp 4 "${ACCEL_ARGS[@]}" \
    -kernel "$KERNEL" \
    -initrd "$WORKDIR/initramfs.gz" \
    -append "console=ttyS0 rdinit=/init expected_sectors=$EXPECTED_SECTORS expected_level=$LEVEL fail_member=$FAIL_MEMBER degraded_boot=$OMIT_MEMBER parity_write_test=$PARITY_WRITES parity_row_kib=$PARITY_ROW_KIB create_test=$CREATE_TEST api_members=$MEMBERS prp_chunk_bytes=$PRP_CHUNK_BYTES prp_cols=$PRP_COLS panic=-1" \
    "${DRIVE_ARGS[@]}" \
    -nographic -no-reboot \
    > "$CONSOLE_LOG" 2>&1 || true   # qemu exit code isn't the verdict

# ----------------------------------------------------------------------------
# 5. Verdict comes from the guest's marker line.
# ----------------------------------------------------------------------------
# The guest's marker can be split mid-line by an interleaved kernel printk
# on the shared serial console (e.g. "RCRAID-TE[  2.9] reboot: Power down\n
# ST-PASS").  Normalize before matching: strip "[timestamp] ..." kernel
# interjections to end-of-line, then drop newlines so a split marker
# re-joins.  FAIL is matched the same way and wins over PASS.
CONSOLE_FLAT="$(sed 's/\[ *[0-9][0-9]*\.[0-9]*\].*$//' "$CONSOLE_LOG" | tr -d '\r\n')"
if printf '%s' "$CONSOLE_FLAT" | grep -q "RCRAID-TEST-FAIL"; then
    verdict_pass=0
elif printf '%s' "$CONSOLE_FLAT" | grep -q "RCRAID-TEST-PASS"; then
    verdict_pass=1
else
    verdict_pass=0
fi
if [ "$verdict_pass" = 1 ]; then
    # RAID1: the guest's readback proves the volume serves data, but not
    # that every mirror got it (round-robin reads could probabilistically
    # hide a lame member).  Prove it from outside: after the VM exits,
    # every member's user-data region must be byte-identical (only the
    # metadata region legitimately differs, by per-member device_id).
    if [ "$LEVEL" = raid1 ] && [ "$FAIL_MEMBER" != 1 ] && [ "$OMIT_MEMBER" != 1 ]; then
        # (Skipped with --fail-member: post-failure writes legitimately
        # reach only the survivor, so the mirrors are EXPECTED to diverge.)
        UD_OFF="$(echo "$MKMETA_OUT" | awk -F= '/^userdata_offset_sectors=/ {print $2}')"
        UD_BYTES=$((UD_OFF * 512))
        for i in $(seq 1 $((MEMBERS - 1))); do
            if ! cmp --ignore-initial="$UD_BYTES" \
                     "${IMAGES[0]}" "${IMAGES[$i]}"; then
                echo "==> FAIL — mirror divergence: member0 vs member$i user data differ"
                trap - EXIT
                echo "==> artifacts kept in $WORKDIR"
                exit 1
            fi
        done
        echo "==> mirror check: all $MEMBERS member user-data regions identical"
    fi
    echo "==> PASS"
    grep "rcraid-test:" "$CONSOLE_LOG" | sed 's/^/    /'
    exit 0
fi

echo "==> FAIL — guest console tail:"
tail -n 200 "$CONSOLE_LOG" | sed 's/^/    /'
# Keep the workdir for debugging on failure.
trap - EXIT
echo "==> artifacts kept in $WORKDIR"
exit 1
