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

set -euo pipefail

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

echo "==> [1/8] installing live-session build dependencies"
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
echo "==> [2/8] detecting array members"
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

echo "==> [3/8] building rcraid.ko against live kernel $(uname -r)"
make -C "$SRC_DIR" clean >/dev/null 2>&1 || true
make -C "$SRC_DIR" all 2>&1 | sed 's/^/    /'
[ -f "$SRC_DIR/rcraid.ko" ] || { echo "build failed" >&2; exit 1; }
echo

echo "==> [4/8] loading rcraid into the live session"
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
echo "==> [5/8] releasing any leftover installer mounts on /dev/rcraid0"
# Graphical installers (Calamares/Ubiquity) usually leave the freshly
# installed system mounted — e.g. /dev/rcraid0p2 on /tmp/calamares-root-XXXX
# with /boot/efi and the API filesystems bind-mounted beneath it.  Those
# stale mounts make our own `mount` of the same partitions fail with EBUSY
# and leave the chroot half-populated.  Clear them, deepest mount first, so
# nested submounts unwind cleanly.  A few passes handle mounts freed only
# once their parent is gone.
_released=0
for _pass in 1 2 3; do
    mapfile -t _mps < <(
        for _part in /dev/rcraid0 /dev/rcraid0p*; do
            [ -b "$_part" ] || continue
            findmnt -nro TARGET --source "$_part" 2>/dev/null || true
        done | awk 'NF { print length, $0 }' | sort -rn | cut -d" " -f2-
    )
    [ "${#_mps[@]}" -gt 0 ] || break
    for _mp in "${_mps[@]}"; do
        [ -n "$_mp" ] || continue
        echo "    unmounting $_mp"
        umount -R "$_mp" 2>/dev/null || umount -l "$_mp" 2>/dev/null || true
        _released=1
    done
done
if [ "$_released" = 1 ]; then
    echo "    cleared installer leftovers"
else
    echo "    none found"
fi
echo

echo "==> [6/8] locating the target rootfs on /dev/rcraid0"

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

ESP_DEV=""
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
        if [ "$sub" = /boot/efi ]; then ESP_DEV="$fs_dev"; fi
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
echo "==> [7/8] installing rcraid into the target system"

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
# Bake the source revision into the staged tree — it has no .git, so the
# Makefile reads .rcraid_rev to stamp the module banner and modinfo.
if rev=$(git -C "$SRC_DIR" describe --always --dirty 2>/dev/null); then
    echo "$rev" > "$TARGET_SRC/.rcraid_rev"
elif [ -f "$SRC_DIR/.rcraid_rev" ]; then
    # Live sessions often run from an unpacked tarball (no .git); a
    # pre-baked .rcraid_rev travels with it.
    cp "$SRC_DIR/.rcraid_rev" "$TARGET_SRC/.rcraid_rev"
fi
# packaging/ needs to be reachable for the in-target script.
mkdir -p "$TARGET/tmp/rcraid-pkg"
cp -r "$SRC_DIR/packaging" "$TARGET/tmp/rcraid-pkg/"

# Build the in-target install script.  We do it here rather than copying a
# separate file in so all variables (subsystem_vendor, family, kver) are
# baked at the call site — no parsing inside the chroot.
cat > "$TARGET/tmp/rcraid-in-target.sh" <<EOF
#!/bin/bash
set -euo pipefail

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

# ----------------------------------------------------------------------------
# [8/8] Make the array actually selectable at boot.
#
# The graphical installers' own efibootmgr call is frequently lost: the
# firmware reaches the array through its RAIDXpert2 UEFI driver, whose EFI
# device path doesn't match what the installer computed from the Linux RAID
# node — so a fresh install can leave NO usable boot entry at all (the "only
# Windows shows in BIOS" symptom).  Two independent safety nets, with the ESP
# still mounted at $TARGET/boot/efi from the fstab step above:
#
#   1. \EFI\BOOT\BOOTX64.EFI — the removable-media fallback that every UEFI
#      firmware boots off any visible ESP with no NVRAM entry required.  This
#      is what makes the array boot regardless of the device-path mismatch.
#   2. A labelled efibootmgr NVRAM entry — best-effort, for a named item in
#      the firmware boot menu; skipped cleanly when the live session isn't
#      UEFI-booted or the firmware refuses the write.
# ----------------------------------------------------------------------------
echo "==> [8/8] ensuring a UEFI boot entry for the array"
ESP_MNT="$TARGET/boot/efi"

# If the target fstab didn't hand us the ESP, find it by GPT type GUID and
# mount it ourselves (the trap's `umount -R $TARGET` releases it either way).
if [ -z "${ESP_DEV:-}" ]; then
    for _p in /dev/rcraid0p*; do
        [ -b "$_p" ] || continue
        if [ "$(blkid -o value -s PARTTYPE "$_p" 2>/dev/null || true)" = \
             "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" ]; then
            ESP_DEV="$_p"
            break
        fi
    done
    if [ -n "${ESP_DEV:-}" ] && ! mountpoint -q "$ESP_MNT" 2>/dev/null; then
        mkdir -p "$ESP_MNT" 2>/dev/null || true
        mount "$ESP_DEV" "$ESP_MNT" 2>/dev/null || true
    fi
fi

if [ -z "${ESP_DEV:-}" ] || [ ! -d "$ESP_MNT/EFI" ]; then
    echo "    no ESP with an EFI/ directory found — skipping."
    echo "    If the box won't boot, confirm the installer created an EFI"
    echo "    System Partition on /dev/rcraid0 and populated \\EFI."
else
    # 1) Removable-media fallback loader.  Discover the VENDOR loader, and
    #    prefer the TARGET distro's own dir (EFI/$target_os) first.  This
    #    matters on a shared ESP that already carries another OS's loader: the
    #    NVRAM entry below is labelled for $target_os and its -l path comes from
    #    the same boot_src, so if discovery picked, say, EFI/ubuntu while we're
    #    installing Fedora, the "rcraid (fedora)" entry would actually boot
    #    Ubuntu's shim.  EFI/ubuntu stays as a fallback (Mint et al. ship their
    #    loader there), then the generic glob.  Skip our own EFI/BOOT fallback
    #    copy and any EFI/BOOT.rcraid-{orig,bak} backup dir: those sort before
    #    real vendor dirs under the EFI/*/ glob and would otherwise be picked.
    boot_src=""
    for cand in \
        "$ESP_MNT/EFI/$target_os/shimx64.efi" \
        "$ESP_MNT/EFI/$target_os/grubx64.efi" \
        "$ESP_MNT/EFI/ubuntu/shimx64.efi" \
        "$ESP_MNT/EFI/ubuntu/grubx64.efi" \
        "$ESP_MNT"/EFI/*/shimx64.efi \
        "$ESP_MNT"/EFI/*/grubx64.efi; do
        case "$cand" in
            *"/EFI/BOOT/"*|*"/BOOT.rcraid-"*) continue ;;
        esac
        [ -f "$cand" ] && { boot_src="$cand"; break; }
    done
    if [ -n "$boot_src" ]; then
        vendor_dir=$(dirname "$boot_src")
        BOOT_DIR="$ESP_MNT/EFI/BOOT"
        cur="$BOOT_DIR/BOOTX64.EFI"
        sentinel="$BOOT_DIR/.rcraid-managed"
        mkdir -p "$BOOT_DIR" 2>/dev/null || true
        # \EFI\BOOT\BOOTX64.EFI is the firmware-wide removable-media fallback
        # and may already belong to another OS on a shared ESP (e.g. Windows'
        # own fallback loader on a dual-boot box).  Preserve any loader we did
        # not write before overwriting it, so it can be restored.
        #
        # Deciding "did we write the current file?" cannot key on the sentinel
        # alone: another OS's installer/repair can overwrite BOOTX64.EFI on a
        # shared ESP without touching our sentinel, so a later run would
        # silently clobber that new foreign loader.  Instead record the hash of
        # what we install (in the sentinel) and treat the current file as
        # foreign whenever it does NOT match that hash (or the sentinel is
        # absent).  This also keeps the reinstall / multi-distro-on-the-array
        # workflow correct: an unchanged loader we wrote last time still
        # matches, so we don't re-preserve our own shim as "the original".
        # Everything below is best-effort: the fallback loader is belt-and-
        # suspenders (boot-from-RAID also works via DKMS + initramfs), so a
        # transient I/O error here — plausible against a degraded/rebuilding
        # array member — must WARN and press on, never abort the installer via
        # set -e before the NVRAM step and the final summary.
        # The sentinel binds the installed loader to BOTH its content hash and
        # the vendor dir it came from ("<hash> <vendor>").  The vendor half
        # matters for the multi-distro-on-the-array workflow: install Ubuntu,
        # then Fedora on the same ESP.  BOOTX64.EFI still holds Ubuntu's shim
        # (unchanged, so the hash alone still matches), but we're now installing
        # Fedora's — a hash-only check would call it "ours/unchanged" and
        # overwrite Ubuntu's fallback with no preservation.  Treating a vendor
        # mismatch as displaced routes it through the backup path instead.
        our_vendor=$(basename "$vendor_dir")
        # Two distinct cases for the loader currently occupying the slot:
        #   foreign=1        — a loader we did NOT write (a real other-OS loader,
        #                      or one an external agent wrote over ours).  Nowhere
        #                      else to recover it from, so preserve before we
        #                      overwrite.
        #   displaced_ours=1 — OUR OWN loader from a previous run for a DIFFERENT
        #                      distro (the multi-distro switch): the hash still
        #                      matches the sentinel, only the vendor differs.
        #                      That distro still boots from its own EFI/<vendor>
        #                      loader and NVRAM entry, so it's recoverable — WARN
        #                      but do NOT back it up.  Backing every switch up is
        #                      what let the .rcraid-bak dirs accumulate unbounded.
        foreign=0
        displaced_ours=0
        if [ -f "$cur" ]; then
            # If we can't hash the current loader, treat it as foreign so we
            # err on the side of preserving it.
            if cur_hash=$(sha256sum "$cur" 2>/dev/null | awk '{print $1}') && \
               [ -n "$cur_hash" ]; then
                if [ -f "$sentinel" ]; then
                    read -r rec_hash rec_vendor < "$sentinel" 2>/dev/null || \
                        { rec_hash=""; rec_vendor=""; }
                    if [ "$cur_hash" != "$rec_hash" ]; then
                        foreign=1          # changed out from under us
                    elif [ "$rec_vendor" != "$our_vendor" ]; then
                        displaced_ours=1   # our own loader, switching distros
                    fi
                elif ! cmp -s "$cur" "$boot_src"; then
                    # No sentinel yet (first run): treat the existing loader as
                    # foreign only if it is NOT already byte-identical to the
                    # one we're about to install — the OS installer commonly
                    # drops its own removable-fallback copy of the same shim
                    # here, and that isn't worth "preserving" as an original.
                    foreign=1
                fi
            else
                echo "    WARN: couldn't hash existing BOOTX64.EFI — treating as foreign" >&2
                cur_hash=""
                foreign=1
            fi
        fi
        # If the current fallback is foreign, a successful backup is a hard
        # precondition for overwriting the slot: if we can't preserve it, leave
        # everything in place and rely on the NVRAM entry below rather than
        # silently clobbering a loader we couldn't save.
        may_write=1
        if [ "$foreign" = 1 ]; then
            may_write=0
            # Preserve the ENTIRE existing EFI/BOOT chain (shim + sibling
            # grubx64.efi/mmx64.efi, which the vendor copy below would clobber)
            # before overwriting.  Name the backup by the loader's content hash
            # so re-displacing the SAME chain on a later run is an idempotent
            # no-op instead of piling up .1/.2 duplicates.  When the hash is
            # unavailable we can't dedup blind, so fall back to a unique name
            # and err toward preserving.
            if [ -n "$cur_hash" ]; then
                dest="$ESP_MNT/EFI/BOOT.rcraid-bak.${cur_hash:0:12}"
            else
                dest="$ESP_MNT/EFI/BOOT.rcraid-bak.unknown"
                _n=0
                while [ -e "$dest" ]; do
                    _n=$((_n + 1)); dest="$ESP_MNT/EFI/BOOT.rcraid-bak.unknown.$_n"
                done
            fi
            if [ -e "$dest" ]; then
                # A COMPLETE copy already exists — the atomic rename below only
                # publishes $dest after a full cp, so its mere presence means a
                # good backup.  Nothing to do.
                may_write=1
            else
                # Copy to a temp path, then atomically rename into place.  An
                # interrupted cp (power loss, I/O error on a degraded member)
                # then leaves only the temp — never a partial copy under $dest
                # that a later run would mistake for a complete backup and skip,
                # silently losing the displaced loader.
                _tmp="$ESP_MNT/EFI/BOOT.rcraid-partial"
                rm -rf "$_tmp" 2>/dev/null || true
                if cp -r "$BOOT_DIR" "$_tmp" 2>/dev/null && \
                   mv "$_tmp" "$dest" 2>/dev/null; then
                    may_write=1
                    echo "    preserved existing EFI/BOOT loader chain -> $(basename "$dest")"
                else
                    rm -rf "$_tmp" 2>/dev/null || true
                    echo "    WARN: couldn't back up existing EFI/BOOT — leaving it in place" >&2
                    echo "    and NOT overwriting BOOTX64.EFI; relying on the NVRAM entry." >&2
                fi
            fi
        elif [ "$displaced_ours" = 1 ]; then
            # Our own prior fallback for a different distro — recoverable, so we
            # overwrite without a backup (that is what keeps backups bounded),
            # but say so rather than clobber silently.
            echo "    note: replacing this array's own ${rec_vendor:-previous} fallback"
            echo "    loader with ${our_vendor} — the former still boots via its own"
            echo "    EFI/${rec_vendor:-<vendor>} loader and NVRAM entry."
        fi
        if [ "$may_write" = 1 ]; then
            # Copy the whole loader chain (shim + grub + MOK manager) so shim,
            # once running as BOOTX64.EFI, still finds grubx64.efi beside it.
            # Ubuntu's grub has an embedded prefix pointing at \EFI\ubuntu, so
            # it reads its real config from there regardless of where it ran.
            cp -f "$vendor_dir"/*.efi "$BOOT_DIR/" 2>/dev/null || true
            if cp -f "$boot_src" "$cur" 2>/dev/null; then
                # Record "<hash> <vendor>" so a future run can tell our own
                # loader from a foreign overwrite AND detect a distro switch.
                printf '%s %s\n' \
                    "$(sha256sum "$cur" 2>/dev/null | awk '{print $1}')" "$our_vendor" \
                    > "$sentinel" 2>/dev/null || true
                echo "    fallback loader installed: EFI/BOOT/BOOTX64.EFI ($(basename "$boot_src"))"
            else
                echo "    WARN: couldn't install EFI/BOOT/BOOTX64.EFI fallback loader" >&2
            fi
        fi
    else
        echo "    WARN: no shim/grub .efi under $ESP_MNT/EFI — install may be incomplete" >&2
    fi

    # 2) Best-effort labelled NVRAM entry.
    if [ -d /sys/firmware/efi/efivars ] && command -v efibootmgr >/dev/null 2>&1; then
        # Derive BOTH the disk and the partition number from the ESP device
        # itself.  $ESP_DEV comes from the target's fstab /boot/efi entry, which
        # is not guaranteed to live on /dev/rcraid0 — so hardcoding -d
        # /dev/rcraid0 could point efibootmgr at the wrong disk.  lsblk
        # PKNAME/PARTN handle any naming scheme; fall back to parsing the
        # rcraid `pN` suffix if those columns are unavailable (older lsblk).
        esp_disk="" esp_partnum=""
        if pk=$(lsblk -nro PKNAME "$ESP_DEV" 2>/dev/null | awk 'NF{print; exit}'); then
            [ -n "$pk" ] && esp_disk="/dev/$pk"
        fi
        if pn=$(lsblk -nro PARTN "$ESP_DEV" 2>/dev/null | awk 'NF{print; exit}'); then
            esp_partnum="$pn"
        fi
        if [ -z "$esp_disk" ] || [ -z "$esp_partnum" ]; then
            case "$ESP_DEV" in
                *p[0-9]*) esp_disk="${ESP_DEV%p*}"; esp_partnum="${ESP_DEV##*p}" ;;
            esac
        fi
        [[ "$esp_partnum" =~ ^[0-9]+$ ]] || esp_partnum=""

        # Point the NVRAM entry at the loader we actually resolved above, so
        # it's correct for ubuntu/fedora/debian vendor dirs alike, converting
        # the ESP-relative path to the backslash form efibootmgr expects.
        # Fall back to the removable-media loader if nothing specific matched.
        if [ -n "$boot_src" ]; then
            loader="${boot_src#"$ESP_MNT"}"   # e.g. /EFI/ubuntu/shimx64.efi
            loader="${loader//\//\\}"          # -> \EFI\ubuntu\shimx64.efi
        else
            loader='\EFI\BOOT\BOOTX64.EFI'
        fi
        # Label the entry after the loader we ACTUALLY resolved, so the label
        # and the -l path (and the BOOTX64.EFI fallback) can never disagree by
        # construction — both come from $boot_src.  boot_src discovery already
        # prefers EFI/$target_os, so this is normally the target distro; if it
        # had to fall back to another vendor dir, the label honestly reflects
        # what will boot.  With no vendor loader at all, fall back to the
        # target distro name for the generic BOOTX64.EFI entry.
        if [ -n "$boot_src" ]; then
            nvram_label="rcraid ($(basename "$(dirname "$boot_src")"))"
        else
            nvram_label="rcraid (${target_os:-linux})"
        fi
        if [ -n "$esp_disk" ] && [ -b "$esp_disk" ] && [ -n "$esp_partnum" ]; then
            # Drop only OUR prior entries for THIS distro so re-running doesn't
            # pile up duplicates.  Compare the label as a LITERAL string (not a
            # regex): matching the exact label avoids deleting a sibling
            # distro's "rcraid (<other>)" entry, and a literal compare avoids an
            # os-release ID with regex metacharacters (e.g. a '.') widening it.
            # efibootmgr (no -v) prints "BootXXXX* <label>"; split off the tag
            # and compare the remainder.
            while read -r _tag _rest; do
                case "$_tag" in
                    Boot[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]*) ;;
                    *) continue ;;
                esac
                [ "$_rest" = "$nvram_label" ] || continue
                _bn="${_tag#Boot}"; _bn="${_bn%\*}"
                efibootmgr -b "$_bn" -B >/dev/null 2>&1 || true
            done < <(efibootmgr 2>/dev/null)
            if efibootmgr -c -d "$esp_disk" -p "$esp_partnum" \
                 -L "$nvram_label" -l "$loader" >/dev/null 2>&1; then
                echo "    NVRAM entry created: '$nvram_label' -> $esp_disk p$esp_partnum $loader"
            else
                echo "    efibootmgr wouldn't write an NVRAM entry (device path not"
                echo "    resolvable) — the BOOTX64.EFI fallback above will still boot."
            fi
        else
            echo "    couldn't resolve the ESP disk/partition from $ESP_DEV — skipping"
            echo "    the NVRAM entry; the BOOTX64.EFI fallback above will still boot."
        fi
    else
        echo "    live session not UEFI-booted (or efibootmgr absent) — skipping the"
        echo "    NVRAM entry; the EFI/BOOT/BOOTX64.EFI fallback will boot the array."
    fi
fi

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
  A UEFI boot entry + \EFI\BOOT\BOOTX64.EFI fallback were installed
  so the firmware can find the array even if the installer's own
  boot entry didn't stick.

  You can now reboot:

      sudo reboot

  If anything goes wrong on first boot, hit 'e' in GRUB and add
  rd.driver.blacklist=nvme to the kernel cmdline to prove the
  rcraid path; revert once verified.
==============================================================
EOF
