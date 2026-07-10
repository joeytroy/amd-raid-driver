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
#                         [--level raid0|raid1]
#                         [--members N] [--size-mib M] [--workdir DIR]
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
SIZE_MIB=256
WORKDIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel)   KERNEL="$2"; shift 2 ;;
        --kver)     KVER="$2"; shift 2 ;;
        --level)    LEVEL="$2"; shift 2 ;;
        --members)  MEMBERS="$2"; shift 2 ;;
        --size-mib) SIZE_MIB="$2"; shift 2 ;;
        --workdir)  WORKDIR="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
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
BUSYBOX="$(command -v busybox)" || { echo "busybox not found" >&2; exit 2; }
file "$BUSYBOX" | grep -q "statically linked" || {
    echo "$BUSYBOX is not static (install busybox-static)" >&2
    exit 2
}

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

MKMETA_OUT="$(python3 "$SCRIPT_DIR/mkmeta.py" --level "$LEVEL" "${IMAGES[@]}")"
echo "$MKMETA_OUT" | sed 's/^/    /'
EXPECTED_SECTORS="$(echo "$MKMETA_OUT" | awk -F= '/^capacity_sectors=/ {print $2}')"
[ -n "$EXPECTED_SECTORS" ] || { echo "mkmeta produced no capacity" >&2; exit 1; }

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
MKE2FS="$(command -v mke2fs || true)"
if [ -n "$MKE2FS" ]; then
    add_binary "$MKE2FS"
    echo "==> bundled $MKE2FS for the sub-page buffered-write test"
else
    echo "==> mke2fs not found on host — guest will skip the sub-page write test"
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

DRIVE_ARGS=()
for i in $(seq 0 $((MEMBERS - 1))); do
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
    -append "console=ttyS0 rdinit=/init expected_sectors=$EXPECTED_SECTORS expected_level=$LEVEL panic=-1" \
    "${DRIVE_ARGS[@]}" \
    -nographic -no-reboot \
    > "$CONSOLE_LOG" 2>&1 || true   # qemu exit code isn't the verdict

# ----------------------------------------------------------------------------
# 5. Verdict comes from the guest's marker line.
# ----------------------------------------------------------------------------
if grep -q "RCRAID-TEST-PASS" "$CONSOLE_LOG"; then
    # RAID1: the guest's readback proves the volume serves data, but not
    # that every mirror got it (round-robin reads could probabilistically
    # hide a lame member).  Prove it from outside: after the VM exits,
    # every member's user-data region must be byte-identical (only the
    # metadata region legitimately differs, by per-member device_id).
    if [ "$LEVEL" = raid1 ]; then
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
