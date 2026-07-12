#!/bin/bash
# Remove rcraid DKMS module, udev rule, helper script, modprobe drop-in,
# apt kernel-hold, and initramfs hooks.
#
# Refuses to run when the system is running FROM the array (root/boot on
# /dev/rcraid*): removing the module and regenerating the initramfs would
# make the next boot unable to mount root.  Override with --force only
# from a rescue environment where that is what you actually want.

set -euo pipefail

PKG_NAME="rcraid"

[ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 1; }

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        *) echo "usage: $0 [--force]" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Root-on-array guard (H-6).  If any critical mount is served by the array,
# uninstalling would brick the next boot.  Checked whether or not the module
# is currently loaded — a rescue boot has the module unloaded but the
# installed system's initramfs still needs rcraid.
# ---------------------------------------------------------------------------
if [ "$FORCE" -ne 1 ]; then
    for mp in / /boot /boot/efi /usr /var /home; do
        src="$(findmnt -no SOURCE "$mp" 2>/dev/null || true)"
        case "$src" in
            /dev/rcraid*)
                cat >&2 <<EOF
ERROR: $mp is mounted from $src — this system is running FROM the
rcraid array.  Uninstalling would remove the driver from the initramfs
and make the next boot unable to mount $mp.

Refusing to continue.  If you really mean it (e.g. you are migrating
off the array from a rescue boot), re-run with:  $0 --force
EOF
                exit 1
                ;;
        esac
    done
fi

echo "==> rcraid uninstall-dkms.sh"

if grep -q '^rcraid ' /proc/modules; then
    echo "==> unloading rcraid"
    rmmod rcraid || {
        echo "rmmod failed.  Unmount /dev/rcraid0 and try again." >&2
        exit 1
    }
fi

# Remove EVERY registered rcraid version (the package version tracks the
# driver version, so several may have accumulated across upgrades).
while read -r ver; do
    [ -n "$ver" ] || continue
    echo "==> dkms remove ${PKG_NAME}/${ver}"
    dkms remove -m "$PKG_NAME" -v "$ver" --all 2>&1 | sed 's/^/    /'
    rm -rf "/usr/src/${PKG_NAME}-${ver}"
done < <(dkms status "$PKG_NAME" 2>/dev/null \
             | sed -n "s#^${PKG_NAME}[/,][ ]*\([^,/:]*\)[,/:].*#\1#p" | sort -u)

rm -f  /usr/sbin/rcraid-bind
rm -f  /etc/udev/rules.d/50-rcraid.rules
rm -f  /etc/modprobe.d/rcraid.conf
rm -f  /etc/apt/apt.conf.d/52-rcraid-kernel-hold

# Pull the initramfs hook back out and regenerate so the next boot doesn't
# try to bring up an array we no longer have a driver for.
if [ -d /usr/lib/dracut/modules.d/95rcraid ]; then
    echo "==> removing dracut module 95rcraid"
    rm -rf /usr/lib/dracut/modules.d/95rcraid
    if command -v dracut >/dev/null 2>&1; then
        echo "==> regenerating initramfs (dracut -f)"
        dracut -f 2>&1 | sed 's/^/    /' || true
    fi
fi

if [ -e /etc/initramfs-tools/hooks/rcraid ]; then
    echo "==> removing initramfs-tools hook"
    rm -f /etc/initramfs-tools/hooks/rcraid
    if command -v update-initramfs >/dev/null 2>&1; then
        echo "==> regenerating initramfs (update-initramfs -u)"
        update-initramfs -u 2>&1 | sed 's/^/    /' || true
    fi
fi

udevadm control --reload-rules

echo "==> Uninstall complete.  Reboot to fully release drives back to nvme,"
echo "    or run:  for bdf in \$(lspci -d 1022:b000 -D | awk '{print \$1}'); do"
echo "                echo > /sys/bus/pci/devices/\$bdf/driver_override"
echo "                echo \$bdf > /sys/bus/pci/drivers_probe"
echo "             done"
