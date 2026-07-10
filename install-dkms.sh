#!/bin/bash
# Install rcraid via DKMS plus boot-time auto-bind glue.
#
# After this script succeeds, the array survives reboots:
#   - DKMS rebuilds the module against every kernel update.
#   - A udev rule hands matching PCI devices to rcbottom at hot-plug time,
#     so /dev/rcraid0 appears during boot without any manual unbind+insmod.
#   - A modprobe.d drop-in records `enable_writes=1` and the array's
#     `safe_subsys_vendor`, so no extra args are needed at load time.
#
# Requires root.  Run from the repo root.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
PKG_VERSION="$(awk -F'"' '/^PACKAGE_VERSION=/ {print $2}' "$SRC_DIR/dkms.conf")"
DRIVER_VERSION="$(cat "$SRC_DIR/VERSION")"
DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

[ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 1; }

# Tooling preflight — fail fast with an actionable message rather than later
# with a silent "no devices found" or a confusing dkms error.
for tool in lspci dkms udevadm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        case "$tool" in
            lspci)   pkg="pciutils" ;;
            dkms)    pkg="dkms" ;;
            udevadm) pkg="systemd / udev" ;;
        esac
        echo "missing required tool: $tool (install $pkg)" >&2
        exit 1
    fi
done

# Secure Boot preflight — rcraid.ko is unsigned, so an enforcing kernel
# refuses to load it ("Key was rejected by service").  Without this check the
# install itself would look successful and the failure would only surface at
# the next boot, with the array absent.  Detection is shared with
# install-livecd.sh and fails closed on an unreadable variable.
. "$SRC_DIR/scripts/lib/secure-boot.sh"

if [ "${RCRAID_ALLOW_SECURE_BOOT:-0}" != 1 ] && secure_boot_enforcing; then
    cat >&2 <<'EOF'
ERROR: Secure Boot is enabled.

rcraid.ko is unsigned, so the kernel will refuse to load it (insmod:
"Key was rejected by service"; dmesg: "Loading of unsigned module is
rejected").  Installing now would look successful but leave the array
absent after the next reboot.

Disable Secure Boot in BIOS setup and re-run this script — and keep it
off: DKMS rebuilds on every kernel update are just as unsigned.

If you sign the module and enrol a MOK yourself, re-run with
RCRAID_ALLOW_SECURE_BOOT=1.  See INSTALL.md ("Secure Boot") for the
signing procedure and its DKMS caveat.
EOF
    exit 1
fi

echo "==> rcraid install-dkms.sh  (driver ${DRIVER_VERSION}, dkms package ${PKG_NAME}-${PKG_VERSION})"
echo

# ----------------------------------------------------------------------------
# 1. Detect array subsystem_vendor.
#
# The AMD chipset reports every downstream NVMe as PCI 1022:b000, including
# the OS drive.  Subsystem vendor is the only reliable way to tell members
# apart.  Heuristic: pick the vendor that has >= 2 devices.  If there's a
# tie, list them and let the user pick.
# ----------------------------------------------------------------------------

declare -A vendor_count
declare -A vendor_bdfs
while read -r bdf; do
    [ -n "$bdf" ] || continue
    sv=$(cat "/sys/bus/pci/devices/$bdf/subsystem_vendor")
    vendor_count[$sv]=$((${vendor_count[$sv]:-0} + 1))
    vendor_bdfs[$sv]="${vendor_bdfs[$sv]:-} $bdf"
done < <(lspci -d 1022:b000 -D | awk '{print $1}')

if [ ${#vendor_count[@]} -eq 0 ]; then
    cat >&2 <<EOF
No PCI 1022:b000 devices found.  This box doesn't appear to have any
AMD-RAID Bottom controllers.  Aborting.

If the BIOS RAID mode is enabled and you still see no devices, check
that you're not in AHCI mode and that drives are seated.
EOF
    exit 1
fi

candidates=()
for sv in "${!vendor_count[@]}"; do
    if [ "${vendor_count[$sv]}" -ge 2 ]; then
        candidates+=("$sv")
    fi
done

if [ ${#candidates[@]} -eq 0 ]; then
    echo "Found these 1022:b000 devices but no vendor has >= 2 (so no array detected):" >&2
    for sv in "${!vendor_count[@]}"; do
        echo "  subsystem_vendor $sv → ${vendor_bdfs[$sv]}" >&2
    done
    echo >&2
    echo "rcraid needs a multi-drive array.  Aborting." >&2
    exit 1
elif [ ${#candidates[@]} -eq 1 ]; then
    SUBSYSTEM_VENDOR="${candidates[0]}"
    echo "Detected array members: ${vendor_bdfs[$SUBSYSTEM_VENDOR]# }"
    echo "Subsystem vendor: $SUBSYSTEM_VENDOR"
else
    echo "Multiple candidate arrays detected:"
    i=1
    for sv in "${candidates[@]}"; do
        echo "  [$i] subsystem_vendor $sv → ${vendor_bdfs[$sv]# }"
        i=$((i + 1))
    done
    n=${#candidates[@]}
    while :; do
        read -rp "Pick one [1-$n, default 1]: " choice
        choice="${choice:-1}"
        if [[ "$choice" =~ ^[0-9]+$ ]] && [ "$choice" -ge 1 ] && [ "$choice" -le "$n" ]; then
            break
        fi
        echo "  invalid — enter a number between 1 and $n"
    done
    SUBSYSTEM_VENDOR="${candidates[$((choice - 1))]}"
fi
echo

# ----------------------------------------------------------------------------
# 2. Stage sources under /usr/src/<pkg>-<ver> and run dkms install.
# ----------------------------------------------------------------------------

echo "==> staging sources at $DKMS_SRC"
rm -rf "$DKMS_SRC"
mkdir -p "$DKMS_SRC"
cp -r \
    "$SRC_DIR"/Makefile \
    "$SRC_DIR"/dkms.conf \
    "$SRC_DIR"/VERSION \
    "$SRC_DIR"/rc_*.c \
    "$SRC_DIR"/rc_*.h \
    "$DKMS_SRC/"

# Bake the source revision into the staged tree — it has no .git, so the
# Makefile reads .rcraid_rev to stamp the module banner and modinfo.
if rev=$(git -C "$SRC_DIR" describe --always --dirty 2>/dev/null); then
    echo "$rev" > "$DKMS_SRC/.rcraid_rev"
fi

# The dkms package version (from dkms.conf) stays fixed across driver updates
# (the driver version lives in VERSION), so a re-run of this script must not
# assume a clean slate: skip `add` if the package is already registered, and
# force build/install so dkms rebuilds from the freshly staged sources instead
# of silently keeping the previously installed module.
if [ -n "$(dkms status -m "$PKG_NAME" -v "$PKG_VERSION" 2>/dev/null)" ]; then
    echo "==> dkms add (skipped — ${PKG_NAME}/${PKG_VERSION} already registered)"
else
    echo "==> dkms add"
    dkms add -m "$PKG_NAME" -v "$PKG_VERSION" 2>&1 | sed 's/^/    /'
fi

echo "==> dkms build"
dkms build -m "$PKG_NAME" -v "$PKG_VERSION" --force 2>&1 | sed 's/^/    /'

echo "==> dkms install"
dkms install -m "$PKG_NAME" -v "$PKG_VERSION" --force 2>&1 | sed 's/^/    /'
echo

# ----------------------------------------------------------------------------
# 3. Lay down the udev rule, helper, and modprobe drop-in.
# ----------------------------------------------------------------------------

echo "==> installing /usr/sbin/rcraid-bind"
install -m 0755 "$SRC_DIR/packaging/sbin/rcraid-bind" /usr/sbin/rcraid-bind

echo "==> rendering /etc/udev/rules.d/50-rcraid.rules"
sed "s/@SUBSYSTEM_VENDOR@/$SUBSYSTEM_VENDOR/g" \
    "$SRC_DIR/packaging/udev/50-rcraid.rules.in" \
    > /etc/udev/rules.d/50-rcraid.rules
chmod 0644 /etc/udev/rules.d/50-rcraid.rules

echo "==> rendering /etc/modprobe.d/rcraid.conf"
sed "s/@SUBSYSTEM_VENDOR@/$SUBSYSTEM_VENDOR/g" \
    "$SRC_DIR/packaging/modprobe.d/rcraid.conf.in" \
    > /etc/modprobe.d/rcraid.conf
chmod 0644 /etc/modprobe.d/rcraid.conf

echo "==> reloading udev rules"
udevadm control --reload-rules
echo

# ----------------------------------------------------------------------------
# 4. Install initramfs hook so the array can serve /, and regenerate the
#    initramfs for the running kernel.
#
# We support both dracut (Fedora/RHEL/Arch/openSUSE) and initramfs-tools
# (Debian/Ubuntu).  If neither is present, the install still succeeds but
# boot-from-RAID won't work — you can only mount the array post-boot.
# ----------------------------------------------------------------------------

INITRAMFS_GENERATOR=""

if command -v dracut >/dev/null 2>&1; then
    INITRAMFS_GENERATOR="dracut"
    echo "==> installing dracut module 95rcraid"
    install -d /usr/lib/dracut/modules.d/95rcraid
    install -m 0755 \
        "$SRC_DIR/packaging/dracut/95rcraid/module-setup.sh" \
        /usr/lib/dracut/modules.d/95rcraid/module-setup.sh

    echo "==> regenerating initramfs (dracut -f)"
    if ! dracut -f 2>&1 | sed 's/^/    /'; then
        echo "WARN: dracut failed — boot-from-RAID won't work until you re-run it" >&2
    fi
elif command -v update-initramfs >/dev/null 2>&1; then
    INITRAMFS_GENERATOR="initramfs-tools"
    echo "==> installing initramfs-tools hook"
    install -d /etc/initramfs-tools/hooks
    install -m 0755 \
        "$SRC_DIR/packaging/initramfs-tools/hooks/rcraid" \
        /etc/initramfs-tools/hooks/rcraid

    echo "==> regenerating initramfs (update-initramfs -u)"
    if ! update-initramfs -u 2>&1 | sed 's/^/    /'; then
        echo "WARN: update-initramfs failed — boot-from-RAID won't work until you re-run it" >&2
    fi
else
    echo "==> no initramfs generator found (dracut / update-initramfs)"
    echo "    Skipping initramfs hook install.  The array will only be"
    echo "    available AFTER the root filesystem mounts.  This is fine if"
    echo "    your rootfs is NOT on rcraid; if it is, install dracut or"
    echo "    initramfs-tools and re-run this script."
fi
echo

# ----------------------------------------------------------------------------
# 5. Done.  Tell the user what happens next.
# ----------------------------------------------------------------------------

if grep -q '^rcraid ' /proc/modules; then
    cat <<EOF
NOTE: rcraid is currently loaded, so the running module is still the old
build.  The freshly installed module (and the regenerated initramfs) take
effect on the next reboot.

EOF
fi

cat <<EOF
==> Install complete.

What's installed:
  DKMS module    /usr/src/${PKG_NAME}-${PKG_VERSION}/
                 /lib/modules/\$(uname -r)/updates/dkms/rcraid.ko
  Bind helper    /usr/sbin/rcraid-bind
  udev rule      /etc/udev/rules.d/50-rcraid.rules
                 (filters on subsystem_vendor=$SUBSYSTEM_VENDOR)
  modprobe conf  /etc/modprobe.d/rcraid.conf
                 (enable_writes=1, safe_subsys_vendor=$SUBSYSTEM_VENDOR)
  initramfs      ${INITRAMFS_GENERATOR:-<none — boot-from-RAID disabled>}

Next: reboot to test the auto-bind path end-to-end.  After reboot:

  lsblk /dev/rcraid0     # should show your volume + partitions
  lspci -d 1022:b000 -k  # array members should show "rcbottom"

To take effect WITHOUT rebooting (one-shot test):

  for bdf in ${vendor_bdfs[$SUBSYSTEM_VENDOR]# }; do
      /usr/sbin/rcraid-bind \$bdf
  done

To remove: sudo ./uninstall-dkms.sh
EOF
