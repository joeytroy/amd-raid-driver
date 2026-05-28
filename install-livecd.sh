#!/bin/bash
# rcraid live-CD installer.
#
# Run this from a stock Debian / Ubuntu / Fedora live environment to install
# Linux onto an AMD-RAID array.  Two phases, one script:
#
#   Phase 1 (now):
#     - Detect distro, install build deps in the live session.
#     - Build rcraid.ko against the live kernel.
#     - Rebind array members from `nvme` to `rcbottom` and load the module so
#       /dev/rcraid0 appears.  At this point the distro's normal OS installer
#       (Anaconda, Ubiquity, Calamares, debian-installer) sees the array as
#       an ordinary disk.
#
#   --- you launch the OS installer, install onto /dev/rcraid0pN, then come
#       back here and press Enter.  Do NOT reboot yet. ---
#
#   Phase 2 (after Enter):
#     - Find the freshly-installed rootfs on /dev/rcraid0.
#     - chroot into it, install DKMS + initramfs hook so future kernel
#       updates rebuild rcraid and so the system can boot off the array.
#     - Tell you to reboot.
#
# Mirrors the workflow DesktopECHO's `raidxpert2-install` offers, but using
# our clean-room rcraid stack instead of AMD's proprietary blob.

set -eu

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
PKG_VERSION="$(awk -F'"' '/^PACKAGE_VERSION=/ {print $2}' "$SRC_DIR/dkms.conf")"

[ "$(id -u)" -eq 0 ] || { echo "must be root (try: sudo $0)" >&2; exit 1; }

# Detect the live distro so we know which package manager + header layout to
# use.  We only branch on family — Debian/Ubuntu use apt; Fedora/RHEL use dnf.
. /etc/os-release
case "${ID:-}${ID_LIKE:+ $ID_LIKE}" in
    *debian*|*ubuntu*) FAMILY="debian" ;;
    *fedora*|*rhel*|*centos*) FAMILY="fedora" ;;
    *) echo "unrecognized distro: ID=$ID ID_LIKE=${ID_LIKE:-}" >&2
       echo "supported: Debian/Ubuntu/Mint/Pop OR Fedora/RHEL/CentOS"  >&2
       exit 1 ;;
esac

echo "==============================================================="
echo "  rcraid live-CD installer  (package: ${PKG_NAME}-${PKG_VERSION})"
echo "  detected live distro: $PRETTY_NAME ($FAMILY family)"
echo "==============================================================="
echo

# ----------------------------------------------------------------------------
# Phase 1 — build + load into the live session
# ----------------------------------------------------------------------------

echo "==> [1/6] installing live-session build dependencies"
case "$FAMILY" in
    debian)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y --no-install-recommends \
            build-essential "linux-headers-$(uname -r)" dkms pciutils \
            | sed 's/^/    /'
        ;;
    fedora)
        dnf install -y \
            gcc make "kernel-devel-$(uname -r)" "kernel-headers-$(uname -r)" \
            dkms pciutils \
            | sed 's/^/    /'
        ;;
esac
echo

# Auto-detect array members by subsystem_vendor (the AMD chipset shadows every
# downstream NVMe as 1022:b000, including the live USB stick on some boards,
# so we filter by subsys vendor).  Heuristic: pick the vendor with >= 2
# devices; if there's a tie, ask the user.
echo "==> [2/6] detecting array members"
declare -A vendor_count
declare -A vendor_bdfs
while read -r bdf; do
    [ -n "$bdf" ] || continue
    sv=$(cat "/sys/bus/pci/devices/$bdf/subsystem_vendor")
    vendor_count[$sv]=$((${vendor_count[$sv]:-0} + 1))
    vendor_bdfs[$sv]="${vendor_bdfs[$sv]:-} $bdf"
done < <(lspci -d 1022:b000 -D | awk '{print $1}')

if [ ${#vendor_count[@]} -eq 0 ]; then
    echo "No PCI 1022:b000 devices found." >&2
    echo "Make sure your BIOS is in RAID mode and the array is created." >&2
    exit 1
fi

candidates=()
for sv in "${!vendor_count[@]}"; do
    if [ "${vendor_count[$sv]}" -ge 2 ]; then
        candidates+=("$sv")
    fi
done

if [ ${#candidates[@]} -eq 0 ]; then
    echo "Found 1022:b000 devices but no vendor has >= 2 (no array detected):" >&2
    for sv in "${!vendor_count[@]}"; do
        echo "  subsystem_vendor $sv → ${vendor_bdfs[$sv]}" >&2
    done
    exit 1
elif [ ${#candidates[@]} -eq 1 ]; then
    SUBSYSTEM_VENDOR="${candidates[0]}"
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
MEMBER_BDFS="${vendor_bdfs[$SUBSYSTEM_VENDOR]# }"
echo "    members: $MEMBER_BDFS"
echo "    subsystem_vendor: $SUBSYSTEM_VENDOR"
echo

echo "==> [3/6] building rcraid.ko against live kernel $(uname -r)"
make -C "$SRC_DIR" clean >/dev/null 2>&1 || true
make -C "$SRC_DIR" all 2>&1 | sed 's/^/    /'
[ -f "$SRC_DIR/rcraid.ko" ] || { echo "build failed" >&2; exit 1; }
echo

echo "==> [4/6] loading rcraid into the live session"
# Inline rebind — same logic as packaging/sbin/rcraid-bind, but that helper
# isn't installed yet.  Pin each member to rcbottom, drop nvme's claim if
# present, kick a re-probe.  rcraid.ko load comes after so it has somewhere
# to bind.
for bdf in $MEMBER_BDFS; do
    if [ -e "/sys/bus/pci/devices/$bdf/driver_override" ]; then
        echo rcbottom > "/sys/bus/pci/devices/$bdf/driver_override"
    fi
    if [ -e "/sys/bus/pci/drivers/nvme/$bdf" ]; then
        echo "$bdf" > /sys/bus/pci/drivers/nvme/unbind 2>/dev/null || true
    fi
done

insmod "$SRC_DIR/rcraid.ko" \
    enable_writes=1 "safe_subsys_vendor=$SUBSYSTEM_VENDOR" 2>&1 \
    | sed 's/^/    /' || {
    echo "insmod failed — see dmesg" >&2
    dmesg | tail -20 >&2
    exit 1
}

# Trigger probe for each member after the module is loaded.
for bdf in $MEMBER_BDFS; do
    echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
done

# Wait briefly for /dev/rcraid0 to appear.  blk-mq add_disk → udev /dev node
# is typically <1 s but be defensive on slow live USBs.
for _ in $(seq 1 20); do
    [ -b /dev/rcraid0 ] && break
    sleep 0.5
done

if [ ! -b /dev/rcraid0 ]; then
    echo "/dev/rcraid0 didn't appear within 10s" >&2
    dmesg | tail -30 >&2
    exit 1
fi

echo "    /dev/rcraid0 is up:"
lsblk /dev/rcraid0 2>/dev/null | sed 's/^/    /'
echo

# ----------------------------------------------------------------------------
# Phase 2 boundary — hand off to the user, then resume after Enter
# ----------------------------------------------------------------------------

cat <<EOF
==============================================================
  /dev/rcraid0 is now visible to your OS installer.

  NEXT STEPS:

    1. Open the distro's installer from the live desktop:
         - Ubuntu:  Click "Install Ubuntu"
         - Debian:  Click "Install"
         - Fedora:  Click "Install to Hard Drive"

    2. When it asks where to install, pick "Custom" or "Manual"
       partitioning and select /dev/rcraid0.  Lay out partitions
       however you like (/, /home, swap, EFI, etc.).

    3. Let the installer finish.  When it offers to reboot,
       DO NOT REBOOT YET.  Close the installer and come back
       to this terminal.

    4. Press Enter here.  Phase 2 will chroot into the new
       install and configure it to boot off the array.
==============================================================
EOF

read -rp "Press Enter once the OS installer is FINISHED (Ctrl-C to abort): " _

# ----------------------------------------------------------------------------
# Phase 2 — find the new install, chroot, configure for boot-from-RAID
# ----------------------------------------------------------------------------

echo
echo "==> [5/6] locating the target rootfs on /dev/rcraid0"

# Probe each partition read-only and find the one that looks like a Linux root
# (has /etc/os-release or /usr/lib/os-release).  Skip swap, EFI, etc.
PROBE_DIR=$(mktemp -d)
TARGET_PART=""
for part in /dev/rcraid0p*; do
    [ -b "$part" ] || continue
    if mount -o ro "$part" "$PROBE_DIR" 2>/dev/null; then
        if [ -e "$PROBE_DIR/etc/os-release" ] || \
           [ -e "$PROBE_DIR/usr/lib/os-release" ]; then
            TARGET_PART="$part"
            # Note target's distro for the chroot install commands.
            if [ -e "$PROBE_DIR/etc/os-release" ]; then
                target_os=$(. "$PROBE_DIR/etc/os-release" && echo "$ID")
            else
                target_os=$(. "$PROBE_DIR/usr/lib/os-release" && echo "$ID")
            fi
            umount "$PROBE_DIR"
            break
        fi
        umount "$PROBE_DIR"
    fi
done
rmdir "$PROBE_DIR"

if [ -z "$TARGET_PART" ]; then
    echo "Couldn't find a Linux rootfs on /dev/rcraid0pN." >&2
    echo "Did the OS installer finish writing to /dev/rcraid0?" >&2
    exit 1
fi

case "$target_os" in
    debian|ubuntu|linuxmint|pop) TARGET_FAMILY="debian" ;;
    fedora|rhel|centos|rocky|almalinux) TARGET_FAMILY="fedora" ;;
    *) echo "target distro '$target_os' not recognized; assuming $FAMILY" >&2
       TARGET_FAMILY="$FAMILY" ;;
esac

echo "    found rootfs: $TARGET_PART ($target_os, $TARGET_FAMILY family)"

# Mount target rootfs + bind /dev /proc /sys + auto-mount /boot and /boot/efi
# from the target's fstab if they're separate partitions.
TARGET=/mnt/rcraid-target
mkdir -p "$TARGET"
mount "$TARGET_PART" "$TARGET"

trap 'umount -R "$TARGET" 2>/dev/null || true' EXIT

for sub in /boot /boot/efi; do
    fs_line=$(awk -v sub="$sub" '$2==sub && $1 !~ /^#/ {print; exit}' "$TARGET/etc/fstab" 2>/dev/null || true)
    [ -n "$fs_line" ] || continue
    fs_spec=$(echo "$fs_line" | awk '{print $1}')
    case "$fs_spec" in
        UUID=*) fs_dev=$(blkid -U "${fs_spec#UUID=}" 2>/dev/null || true) ;;
        PARTUUID=*) fs_dev=$(blkid -t PARTUUID="${fs_spec#PARTUUID=}" -o device 2>/dev/null || true) ;;
        /dev/*) fs_dev="$fs_spec" ;;
        *) fs_dev="" ;;
    esac
    if [ -n "$fs_dev" ] && [ -b "$fs_dev" ]; then
        echo "    mounting $sub from $fs_dev"
        mount "$fs_dev" "$TARGET$sub"
    fi
done

# Recursive bind so submounts (/dev/pts, /sys/fs/cgroup, etc.) come along —
# update-initramfs/dracut and dkms occasionally reach into them.
mount --rbind /dev  "$TARGET/dev"
mount --rbind /proc "$TARGET/proc"
mount --rbind /sys  "$TARGET/sys"
mount --make-rslave "$TARGET/dev"
mount --make-rslave "$TARGET/proc"
mount --make-rslave "$TARGET/sys"
# resolv.conf for apt/dnf in chroot.  The target's /etc/resolv.conf is
# typically a symlink to /run/systemd/resolve/stub-resolv.conf which is dead
# inside the chroot; replace it with the live's actual content for the
# duration of the install.  systemd-resolved will recreate the link on first
# boot of the target.
if [ -e /etc/resolv.conf ]; then
    rm -f "$TARGET/etc/resolv.conf"
    cp -L /etc/resolv.conf "$TARGET/etc/resolv.conf"
fi

echo

# Stage rcraid sources into the target and run the in-target install.
echo "==> [6/6] installing rcraid into the target system"

# Pick the kernel in the target that we'll build for.  Live and target kernels
# can differ (e.g. Ubuntu 24.04 live boots HWE while the installed system uses
# GA).  Use the newest one present in the target's /lib/modules.
TARGET_KVER=$(ls "$TARGET/lib/modules" 2>/dev/null | sort -V | tail -1 || true)
if [ -z "$TARGET_KVER" ]; then
    echo "no kernel found at $TARGET/lib/modules — was the install incomplete?" >&2
    exit 1
fi
echo "    target kernel: $TARGET_KVER"

# Copy sources into the target.  We deliberately copy only what DKMS needs
# (matches install-dkms.sh) plus the packaging/ tree so the chroot can lay
# down the same udev/modprobe/initramfs glue.
TARGET_SRC="$TARGET/usr/src/${PKG_NAME}-${PKG_VERSION}"
rm -rf "$TARGET_SRC"
mkdir -p "$TARGET_SRC"
cp -r \
    "$SRC_DIR"/Makefile \
    "$SRC_DIR"/dkms.conf \
    "$SRC_DIR"/rc_*.c \
    "$SRC_DIR"/rc_*.h \
    "$TARGET_SRC/"
# packaging/ needs to be reachable for the in-target script.
mkdir -p "$TARGET/tmp/rcraid-pkg"
cp -r "$SRC_DIR/packaging" "$TARGET/tmp/rcraid-pkg/"

# Build the in-target install script.  We do it here rather than copying a
# separate file in so all variables (subsystem_vendor, family, kver) are
# baked at the call site — no parsing inside the chroot.
cat > "$TARGET/tmp/rcraid-in-target.sh" <<EOF
#!/bin/bash
set -eu

PKG_NAME="$PKG_NAME"
PKG_VERSION="$PKG_VERSION"
TARGET_KVER="$TARGET_KVER"
SUBSYSTEM_VENDOR="$SUBSYSTEM_VENDOR"
TARGET_FAMILY="$TARGET_FAMILY"
SRC_DIR="/usr/src/\${PKG_NAME}-\${PKG_VERSION}"
PKG_DIR="/tmp/rcraid-pkg/packaging"

echo "    installing build deps in target ($TARGET_FAMILY)"
case "\$TARGET_FAMILY" in
    debian)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y --no-install-recommends \\
            build-essential "linux-headers-\$TARGET_KVER" dkms initramfs-tools
        ;;
    fedora)
        dnf install -y gcc make "kernel-devel-\$TARGET_KVER" \\
            "kernel-headers-\$TARGET_KVER" dkms dracut
        ;;
esac

echo "    dkms add/build/install rcraid (against kernel \$TARGET_KVER)"
dkms add    -m "\$PKG_NAME" -v "\$PKG_VERSION"
dkms build  -m "\$PKG_NAME" -v "\$PKG_VERSION" -k "\$TARGET_KVER"
dkms install -m "\$PKG_NAME" -v "\$PKG_VERSION" -k "\$TARGET_KVER"

echo "    installing udev rule, modprobe drop-in, bind helper"
install -m 0755 "\$PKG_DIR/sbin/rcraid-bind" /usr/sbin/rcraid-bind
sed "s/@SUBSYSTEM_VENDOR@/\$SUBSYSTEM_VENDOR/g" \\
    "\$PKG_DIR/udev/50-rcraid.rules.in" \\
    > /etc/udev/rules.d/50-rcraid.rules
chmod 0644 /etc/udev/rules.d/50-rcraid.rules
sed "s/@SUBSYSTEM_VENDOR@/\$SUBSYSTEM_VENDOR/g" \\
    "\$PKG_DIR/modprobe.d/rcraid.conf.in" \\
    > /etc/modprobe.d/rcraid.conf
chmod 0644 /etc/modprobe.d/rcraid.conf

echo "    installing initramfs hook + regenerating initramfs"
if command -v dracut >/dev/null 2>&1; then
    install -d /usr/lib/dracut/modules.d/95rcraid
    install -m 0755 "\$PKG_DIR/dracut/95rcraid/module-setup.sh" \\
        /usr/lib/dracut/modules.d/95rcraid/module-setup.sh
    dracut -f --kver "\$TARGET_KVER"
elif command -v update-initramfs >/dev/null 2>&1; then
    install -d /etc/initramfs-tools/hooks
    install -m 0755 "\$PKG_DIR/initramfs-tools/hooks/rcraid" \\
        /etc/initramfs-tools/hooks/rcraid
    update-initramfs -u -k "\$TARGET_KVER"
else
    echo "WARN: no initramfs generator in target — boot-from-RAID will fail" >&2
    exit 1
fi
EOF
chmod +x "$TARGET/tmp/rcraid-in-target.sh"

chroot "$TARGET" /tmp/rcraid-in-target.sh 2>&1 | sed 's/^/    /'

# Clean up the staging files inside the target.
rm -rf "$TARGET/tmp/rcraid-pkg" "$TARGET/tmp/rcraid-in-target.sh"

echo

# Best-effort unmount.  trap will catch anything we miss.
umount -R "$TARGET" 2>/dev/null || true
trap - EXIT

cat <<EOF
==============================================================
  Done.

  /dev/rcraid0pN is configured to boot off the AMD-RAID array.
  rcraid is installed via DKMS so it rebuilds on kernel updates.
  An initramfs hook makes the array available before pivot_root.

  You can now reboot:

      sudo reboot

  If anything goes wrong on first boot, hit 'e' in GRUB and add
  rd.driver.blacklist=nvme to the kernel cmdline to prove the
  rcraid path; revert once verified.
==============================================================
EOF
