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
#   sudo dkms install -m rcraid -v <ver> --force
# then regenerate that kernel's initramfs:
#   sudo update-initramfs -u -k <kver>     # Debian/Ubuntu (initramfs-tools)
#   sudo dracut -f --kver <kver>           # Fedora/RHEL/Arch (dracut)).
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

# Detect the initramfs toolchain.  Debian/Ubuntu use initramfs-tools
# (image /boot/initrd.img-<K>, listed with lsinitramfs); Fedora/RHEL/Arch/
# openSUSE use dracut (image /boot/initramfs-<K>.img, listed with lsinitrd).
# Fail LOUDLY when neither lister exists — silently swallowing the
# command-not-found used to make every kernel falsely FAIL on dracut boxes.
if command -v lsinitramfs >/dev/null 2>&1; then
    INITRD_LISTER="lsinitramfs"
    FIX_HINT="sudo update-initramfs -u -k"
elif command -v lsinitrd >/dev/null 2>&1; then
    INITRD_LISTER="lsinitrd"
    FIX_HINT="sudo dracut -f --kver"
else
    echo "ERROR: neither lsinitramfs (initramfs-tools) nor lsinitrd (dracut)" >&2
    echo "       is available, so initramfs contents CANNOT be verified." >&2
    echo "       Install the tool matching your initramfs generator and" >&2
    echo "       re-run.  Refusing to report PASS without checking." >&2
    exit 1
fi

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
    # Try both naming schemes so a mixed /boot doesn't confuse us.
    initrd=""
    for cand in "/boot/initrd.img-$K" "/boot/initramfs-$K.img"; do
        if [ -e "$cand" ]; then initrd="$cand"; break; fi
    done
    if [ -z "$initrd" ]; then
        echo "   [FAIL] no initramfs (initrd.img-$K / initramfs-$K.img) — kernel cannot mount / via rcraid"
        ok=0
    elif "$INITRD_LISTER" "$initrd" 2>/dev/null | grep -q 'rcraid\.ko'; then
        echo "   [ok]   present in initramfs ($initrd)"
    else
        echo "   [FAIL] rcraid MISSING from initramfs — this kernel will NOT boot"
        echo "          fix: $FIX_HINT $K"
        ok=0
    fi

    # 5. Informational: is the on-disk module signed (matters under Secure Boot)?
    if [ -n "$ko" ]; then
        # modinfo needs a .ko-suffixed path to recognise the file as a module.
        # Decompress by suffix (distros use zstd/xz/gzip, or none).
        signer=""
        if tmpd="$(mktemp -d 2>/dev/null)"; then
            tmp="$tmpd/rcraid.ko"
            case "$ko" in
                *.zst) zstd -dc "$ko" 2>/dev/null > "$tmp" ;;
                *.xz)  xz   -dc "$ko" 2>/dev/null > "$tmp" ;;
                *.gz)  gzip -dc "$ko" 2>/dev/null > "$tmp" ;;
                *)     cp "$ko" "$tmp" 2>/dev/null ;;
            esac
            signer="$(modinfo "$tmp" 2>/dev/null | sed -n 's/^signer:[[:space:]]*//p')"
            rm -rf "$tmpd"
        fi
        if [ -n "$signer" ]; then
            echo "   [info] signed by: $signer"
        else
            echo "   [info] unsigned or signer unreadable (fine unless Secure Boot is enforced)"
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
