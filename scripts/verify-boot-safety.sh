#!/bin/bash
#
# verify-boot-safety.sh — confirm rcraid is present and bootable for every
# installed kernel.
#
# rcraid backs the root filesystem, so if a kernel update leaves the module
# unbuilt or absent from that kernel's initramfs, the machine will not boot
# into it. Run this AFTER any kernel update (and it's cheap enough to run any
# time) — BEFORE you reboot into a freshly-installed kernel. Any kernel that
# fails here must not be booted until fixed (rebuild with:
#   sudo dkms install -m rcraid -v <ver> --force && sudo update-initramfs -u -k <kver>).
#
# Exit status: 0 = every installed kernel is safe; 1 = at least one is not.

set -u
PKG=rcraid

# initrd images and dkms state are root-only; re-exec under sudo.
if [ "$EUID" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

RUNNING="$(uname -r)"
FAIL=0
CHECKED=0

# Prefer the dkms-signed source version; fall back to whatever dkms knows.
PKG_VER="$(dkms status "$PKG" 2>/dev/null | sed -n 's#^'"$PKG"'/\([^,]*\),.*#\1#p' | head -1)"

echo "== rcraid boot-safety check =="
echo "   running kernel: $RUNNING"
echo "   dkms version:   ${PKG_VER:-<none registered>}"
echo

for img in /boot/vmlinuz-*; do
    [ -e "$img" ] || continue
    K="${img#/boot/vmlinuz-}"
    CHECKED=$((CHECKED + 1))
    tag=""
    [ "$K" = "$RUNNING" ] && tag=" (running)"
    printf 'kernel %s%s\n' "$K" "$tag"

    ok=1

    # 1. Kernel headers present (needed so a future rebuild can succeed).
    if [ -e "/lib/modules/$K/build" ]; then
        echo "   [ok]   headers present"
    else
        echo "   [WARN] headers missing — dkms cannot (re)build for this kernel"
    fi

    # 2. DKMS reports the module installed for this kernel.
    if dkms status "$PKG" 2>/dev/null | grep -q ", $K,.*installed"; then
        echo "   [ok]   dkms: installed"
    else
        echo "   [FAIL] dkms: NOT installed for this kernel"
        ok=0
    fi

    # 3. The .ko actually exists on disk for this kernel.
    ko="$(ls /lib/modules/"$K"/updates/dkms/rcraid.ko* 2>/dev/null | head -1)"
    if [ -n "$ko" ]; then
        echo "   [ok]   module on disk: ${ko##*/}"
    else
        echo "   [FAIL] module NOT in /lib/modules/$K/updates/dkms/"
        ok=0
    fi

    # 4. THE decisive check: the module is inside THIS kernel's initramfs.
    initrd="/boot/initrd.img-$K"
    if [ ! -e "$initrd" ]; then
        echo "   [FAIL] no initramfs ($initrd) — kernel cannot mount / via rcraid"
        ok=0
    elif lsinitramfs "$initrd" 2>/dev/null | grep -q 'rcraid\.ko'; then
        echo "   [ok]   present in initramfs"
    else
        echo "   [FAIL] rcraid MISSING from initramfs — this kernel will NOT boot"
        echo "          fix: sudo update-initramfs -u -k $K"
        ok=0
    fi

    # 5. Informational: is the on-disk module signed (matters under Secure Boot)?
    if [ -n "$ko" ]; then
        # modinfo needs a .ko-suffixed path to recognise the file as a module.
        tmpd="$(mktemp -d)"; tmp="$tmpd/rcraid.ko"
        case "$ko" in
            *.zst) zstd -dc "$ko" 2>/dev/null > "$tmp" ;;
            *)     cp "$ko" "$tmp" ;;
        esac
        signer="$(modinfo "$tmp" 2>/dev/null | sed -n 's/^signer:[[:space:]]*//p')"
        rm -rf "$tmpd"
        if [ -n "$signer" ]; then
            echo "   [info] signed by: $signer"
        else
            echo "   [info] unsigned (fine unless Secure Boot is enforced)"
        fi
    fi

    [ "$ok" -eq 0 ] && FAIL=1
    echo
done

if [ "$CHECKED" -eq 0 ]; then
    echo "No installed kernels found under /boot — nothing to check."
    exit 1
fi

if [ "$FAIL" -eq 0 ]; then
    echo "RESULT: PASS — rcraid is present in every installed kernel's initramfs."
    exit 0
else
    echo "RESULT: FAIL — at least one kernel would not boot. Do NOT reboot into a"
    echo "         FAILed kernel until its initramfs contains rcraid."
    exit 1
fi
