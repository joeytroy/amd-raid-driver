#!/bin/bash
# Install rcraid via DKMS plus boot-time auto-bind glue.
#
# After this script succeeds, the array survives reboots:
#   - DKMS rebuilds the module against every kernel update.
#   - A udev rule hands matching PCI devices to rcbottom at hot-plug time,
#     so /dev/rcraid0 appears during boot without any manual unbind+insmod.
#   - A modprobe.d drop-in records `enable_writes=1` and the member filter
#     (`safe_subsys_vendor`, or `allow_foreign_nvme` on X399), so no extra
#     args are needed at load time.
#
# Requires root.  Run from the repo root.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
# The dkms package version tracks the driver version (VERSION file); the
# repo dkms.conf carries a @PKGVER@ placeholder we substitute at staging
# time, so each driver release installs into its own dkms tree.
DRIVER_VERSION="$(cat "$SRC_DIR/VERSION")"
PKG_VERSION="$DRIVER_VERSION"
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
# 1. Detect array members.
#
# 1022:b000 platforms are told apart by subsystem_vendor; on X399 the members
# keep native NVMe IDs and are found by their RAIDCore metadata instead.  See
# scripts/lib/members.sh.
# ----------------------------------------------------------------------------

. "$SRC_DIR/scripts/lib/members.sh"
detect_members
echo "Detected array members ($BIND_MODE): $MEMBER_BDFS"
[ -n "$SUBSYSTEM_VENDOR" ] && echo "Subsystem vendor: $SUBSYSTEM_VENDOR"
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

# Substitute the package version into the staged dkms.conf.
sed -i "s/@PKGVER@/$PKG_VERSION/g" "$DKMS_SRC/dkms.conf"

# Bake the source revision into the staged tree — it has no .git, so the
# Makefile reads .rcraid_rev to stamp the module banner and modinfo.
if rev=$(git -C "$SRC_DIR" describe --always --dirty 2>/dev/null); then
    echo "$rev" > "$DKMS_SRC/.rcraid_rev"
fi

# Upgrade path: retire any OTHER registered rcraid version first, so two
# driver releases never coexist in the dkms tree (the old module would
# shadow or race the new one in updates/dkms).
while read -r oldver; do
    [ -n "$oldver" ] || continue
    [ "$oldver" = "$PKG_VERSION" ] && continue
    echo "==> dkms remove (retiring old version ${PKG_NAME}/${oldver})"
    dkms remove -m "$PKG_NAME" -v "$oldver" --all 2>&1 | sed 's/^/    /'
    rm -rf "/usr/src/${PKG_NAME}-${oldver}"
done < <(dkms status "$PKG_NAME" 2>/dev/null \
             | sed -n "s#^${PKG_NAME}[/,][ ]*\([^,/:]*\)[,/:].*#\1#p" | sort -u)

# A re-run of this script for the SAME version must not assume a clean
# slate: skip `add` if the package is already registered, and force
# build/install so dkms rebuilds from the freshly staged sources instead
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

echo "==> installing /etc/udev/rules.d/61-rcraid-storage.rules"
install -m 0644 "$SRC_DIR/packaging/udev/61-rcraid-storage.rules" \
    /etc/udev/rules.d/61-rcraid-storage.rules

echo "==> rendering /etc/udev/rules.d/50-rcraid.rules + /etc/modprobe.d/rcraid.conf"
render_member_config "" "$SRC_DIR/packaging"

echo "==> reloading udev rules"
udevadm control --reload-rules
echo

# On apt-based systems, keep kernel packages out of unattended-upgrades so
# a kernel update (and the DKMS rebuild + initramfs regeneration it needs)
# is always deliberate — an unattended one that broke the rcraid build
# would leave the new kernel unbootable with nobody watching.
if [ -d /etc/apt/apt.conf.d ]; then
    echo "==> installing /etc/apt/apt.conf.d/52-rcraid-kernel-hold"
    install -m 0644 "$SRC_DIR/packaging/apt.conf.d/52-rcraid-kernel-hold" \
        /etc/apt/apt.conf.d/52-rcraid-kernel-hold
    echo "    (kernel packages excluded from unattended-upgrades; after a"
    echo "     deliberate kernel update run scripts/verify-boot-safety.sh)"
    echo
fi

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
        echo "ERROR: dracut FAILED — the initramfs does NOT contain rcraid." >&2
        echo "       Do NOT reboot until 'dracut -f' succeeds (a full /boot" >&2
        echo "       is the usual cause), then re-run this script or verify" >&2
        echo "       with scripts/verify-boot-safety.sh." >&2
        exit 1
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
        echo "ERROR: update-initramfs FAILED — the initramfs does NOT contain" >&2
        echo "       rcraid.  Do NOT reboot until 'update-initramfs -u'" >&2
        echo "       succeeds (a full /boot is the usual cause), then re-run" >&2
        echo "       this script or verify with scripts/verify-boot-safety.sh." >&2
        exit 1
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
                 /etc/udev/rules.d/61-rcraid-storage.rules
                 ($([ "$BIND_MODE" = b000 ] && echo "filters on subsystem_vendor=$SUBSYSTEM_VENDOR" || echo "X399: members $MEMBER_BDFS, RAIDCore-verified at bind"))
  modprobe conf  /etc/modprobe.d/rcraid.conf
                 (enable_writes=1, $(member_filter_option))
  initramfs      ${INITRAMFS_GENERATOR:-<none — boot-from-RAID disabled>}
$([ -e /etc/apt/apt.conf.d/52-rcraid-kernel-hold ] && echo '  apt hold       /etc/apt/apt.conf.d/52-rcraid-kernel-hold
                 (kernel packages held back from unattended-upgrades)')

After any kernel update, run scripts/verify-boot-safety.sh BEFORE
rebooting — it confirms rcraid is built and present in every installed
kernel's initramfs.

Next: reboot to test the auto-bind path end-to-end.  After reboot:

  lsblk /dev/rcraid0     # should show your volume + partitions
  lspci -k -s <member>   # array members should show "rcbottom"
                         # (members: $MEMBER_BDFS)

To take effect WITHOUT rebooting (one-shot test):

  for bdf in $MEMBER_BDFS; do
      /usr/sbin/rcraid-bind \$bdf
  done

To remove: sudo ./uninstall-dkms.sh
EOF
