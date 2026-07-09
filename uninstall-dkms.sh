#!/bin/bash
# Remove rcraid DKMS module, udev rule, helper script, and modprobe drop-in.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
PKG_VERSION="$(awk -F'"' '/^PACKAGE_VERSION=/ {print $2}' "$SRC_DIR/dkms.conf")"

[ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 1; }

echo "==> rcraid uninstall-dkms.sh  (package: ${PKG_NAME}-${PKG_VERSION})"

if grep -q '^rcraid ' /proc/modules; then
    echo "==> unloading rcraid"
    rmmod rcraid || {
        echo "rmmod failed.  Unmount /dev/rcraid0 and try again." >&2
        exit 1
    }
fi

if [ -n "$(dkms status -m "$PKG_NAME" -v "$PKG_VERSION" 2>/dev/null)" ]; then
    echo "==> dkms remove"
    dkms remove -m "$PKG_NAME" -v "$PKG_VERSION" --all 2>&1 | sed 's/^/    /'
fi

rm -rf "/usr/src/${PKG_NAME}-${PKG_VERSION}"
rm -f  /usr/sbin/rcraid-bind
rm -f  /etc/udev/rules.d/50-rcraid.rules
rm -f  /etc/modprobe.d/rcraid.conf

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
