#!/bin/bash
# Remove rcraid DKMS module, udev rule, helper script, and modprobe drop-in.

set -eu

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
PKG_VERSION="$(awk -F'"' '/^PACKAGE_VERSION=/ {print $2}' "$SRC_DIR/dkms.conf")"

[ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 1; }

echo "==> rcraid uninstall-dkms.sh  (package: ${PKG_NAME}-${PKG_VERSION})"

if lsmod | grep -q '^rcraid'; then
    echo "==> unloading rcraid"
    rmmod rcraid || {
        echo "rmmod failed.  Unmount /dev/rcraid0 and try again." >&2
        exit 1
    }
fi

if dkms status -m "$PKG_NAME" -v "$PKG_VERSION" 2>/dev/null | grep -q "$PKG_NAME"; then
    echo "==> dkms remove"
    dkms remove -m "$PKG_NAME" -v "$PKG_VERSION" --all 2>&1 | sed 's/^/    /'
fi

rm -rf "/usr/src/${PKG_NAME}-${PKG_VERSION}"
rm -f  /usr/sbin/rcraid-bind
rm -f  /etc/udev/rules.d/50-rcraid.rules
rm -f  /etc/modprobe.d/rcraid.conf

udevadm control --reload-rules

echo "==> Uninstall complete.  Reboot to fully release drives back to nvme,"
echo "    or run:  for bdf in \$(lspci -d 1022:b000 -D | awk '{print \$1}'); do"
echo "                echo \$bdf > /sys/bus/pci/drivers/pci/drivers_probe"
echo "             done"
