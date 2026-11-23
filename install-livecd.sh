#!/bin/bash
# Robust, production-ready verbose rcraid modular live-CD setup engine.
set -euo pipefail
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

[ "$(id -u)" -eq 0 ] || { echo "Run as root: sudo $0" >&2; exit 1; }

# Run the whole install in a private mount namespace.
#
# Mount events propagate, and the live session's root is a shared peer, so
# every mount made below — the target root, then the chroot's /dev, /proc,
# /sys and /run with their children — is copied into each systemd service
# namespace.  Those copies are invisible from here, so `umount -R` cannot
# reach them and the target filesystem stays open: the block device never
# closes, rcraid keeps refcount 1, `rmmod` fails until the next reboot, and
# every later attempt to mount the target reports "already mounted or mount
# point busy".  The install "succeeds" and leaves the session unable to let go
# of the array.
#
# Re-exec instead of setting propagation flags piecemeal: making the target
# mount private after creating it still leaks the target mount itself, because
# that one propagated at creation.  (Measured on Ubuntu 26.10: 3 leaked mounts
# with the old code, 1 with `--make-rslave` applied after the bind, 0 inside
# this namespace.)  Whatever is still mounted when this process tree exits is
# discarded with the namespace.
if [ "${RCRAID_PRIVATE_MNT_NS:-0}" != 1 ]; then
    if command -v unshare >/dev/null 2>&1; then
        export RCRAID_PRIVATE_MNT_NS=1
        exec unshare --mount --propagation private -- /bin/bash "$0" "$@"
    fi
    echo "WARN: unshare(1) not found — mounts may leak into systemd namespaces." >&2
    echo "      The install itself still works, but rcraid may not unload from" >&2
    echo "      this session afterwards (until it reboots)." >&2
fi

. "$SRC_DIR/scripts/lib/secure-boot.sh"

# A DELETE tears the volume's gendisk down asynchronously, so `rmmod` issued
# right after the writer returns can lose the race with the block-device
# release and fail "Module rcraid is in use" while /proc/modules already shows
# refcount 0 (seen 2026-11-14 immediately after a lab CREATE ioctl).  Retrying
# beats telling the operator to reboot a live session whose array was just
# created: the metadata is on the disks and the volume only needs re-probing.
unload_rcraid() {
    local i

    for i in 1 2 3 4 5 6 7 8 9 10; do
        rmmod rcraid 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# Secure Boot has to be off for the live session, and that is a property of
# where this script runs rather than an oversight: the live kernel refuses
# unsigned out-of-tree modules, and the only key that could sign this
# session's rcraid.ko would have to be enrolled by rebooting the live session —
# a reboot that throws away the session and the key it just made.  Fail here,
# before anything on disk is touched, instead of at insmod after the user has
# already handed the disks to the installer.
if secure_boot_enforcing; then
    cat >&2 <<'EOF'
ERROR: Secure Boot is enforcing, so this live session cannot load rcraid.ko.

The live kernel rejects unsigned out-of-tree modules ("Key was rejected by
service"), so /dev/rcraid0 never appears and there is nothing to install onto.
No key can fix that from here: enrolling one takes a reboot of the live
session, which is exactly the reboot this installer ends with.

What works:
  1. Switch Secure Boot off in firmware setup.
  2. Run this installer.
  3. Its FIRST boot afterwards enrolls the installed system's own signing key
     in MOK (the installer queues the request; shim's MOK manager asks for the
     password before the kernel starts).  Switch Secure Boot back on once that
     prompt has been answered.

See docs/INSTALL.md, "Secure Boot".
EOF
    exit 1
fi

echo "======================================================================"
echo "🚀 INITIALIZING VERBOSE DRIVER COMPILATION AND INTERFACE HANDOFF"
echo "======================================================================"
echo "📥 Step 1: Gathering live system dependencies..."
sudo apt-get update -qq && sudo apt-get install -y --no-install-recommends -qq build-essential "linux-headers-$(uname -r)" dkms pciutils >/dev/null

echo "🛠️  Step 2: Invoking Makefile tree compilation..."
make -C "$SRC_DIR" clean all

# A re-run must not need a manual teardown.  An earlier run — or an array still
# assembled from it — leaves rcraid loaded with the members bound to rcbottom,
# which hides them from lsblk (so the member scan would see no disks) and makes
# insmod fail with "File exists".  Unstack that here; the RAIDCore metadata on
# the disks is not touched, so the scan can still find it.
# Read /proc/modules directly: `lsmod | grep -q` under `set -o pipefail` is a
# race — grep exits at the first match, lsmod dies of SIGPIPE with status 141,
# and pipefail makes this condition false even though the module is loaded.
# That is exactly how a re-run used to skip the unload and then see no disks.
if grep -q '^rcraid' /proc/modules; then
    echo "♻️  Unloading the rcraid instance left by an earlier run..."
    for link in /sys/bus/pci/drivers/rcbottom/*; do
        [ -e "$link" ] || continue
        bdf=$(basename "$link")
        case "$bdf" in
            [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:*) ;;
            *) continue ;;
        esac
        echo "$bdf" > "/sys/bus/pci/drivers/rcbottom/unbind" 2>/dev/null || true
        if [ -e "/sys/bus/pci/devices/$bdf/driver_override" ]; then
            echo "" > "/sys/bus/pci/devices/$bdf/driver_override" 2>/dev/null || true
        fi
        echo "$bdf" > "/sys/bus/pci/drivers/nvme/bind" 2>/dev/null || true
    done
    if ! unload_rcraid; then
        echo "❌ rcraid is still in use — something has /dev/rcraid0 open or mounted." >&2
        echo "   Unmount/close it and re-run. Nothing on the disks was changed." >&2
        exit 1
    fi
    udevadm settle 2>/dev/null || true
fi

echo "🔍 Step 3: Looking for an array the firmware already created..."
# The array is normally built in the UEFI RAIDXpert2 utility, which writes the
# RAIDCore metadata itself — the same records the driver validates and
# rcraid-bind gates on.  Detect exactly those disks: they become the members,
# and nothing is written to them.  The array has to come from the firmware —
# there is no disk/level menu here and no install-time create path.
RC_MAGIC="52414944436f7265"   # "RAIDCore" at +0x08 of the LBA 0x5000 sector
FW_MEMBERS=""
for disk in $(lsblk -dno NAME,TYPE | awk '$2 == "disk" && $1 ~ /^nvme/ { print $1 }'); do
    magic="$(dd if="/dev/$disk" bs=512 skip=$((0x5000)) count=1 status=none 2>/dev/null \
             | tail -c +9 | head -c 8 | od -An -tx1 -v | tr -d ' \n')"
    if [ "$magic" = "$RC_MAGIC" ]; then
        FW_MEMBERS="$FW_MEMBERS $disk"
    fi
done
FW_MEMBERS="${FW_MEMBERS# }"

# This installer assembles the array the firmware created; it does not build
# one.  The only writer in this tree (mkmeta.py) stamps RAID 0/1 metadata over
# blank disks for the QEMU rig, where no firmware can produce those records —
# that is a lab path, not an install path.  Without a firmware array there is nothing to
# hand the OS installer, so stop here, before a single disk is touched.
if [ -z "$FW_MEMBERS" ]; then
    cat >&2 <<'EOF'
ERROR: no array found — no NVMe disk carries RAIDCore metadata (LBA 0x5000).

Create the array in the UEFI RAIDXpert2 utility first, save and exit, then boot
this live session again and re-run this script.  Nothing on the disks was
changed by this run.
EOF
    exit 1
fi

SELECTED_DRIVES="$FW_MEMBERS"
echo "✅ Array found — members: ${FW_MEMBERS}"
echo "   Its level and geometry come from the RAIDCore metadata on those disks."

clear
echo "======================================================================"
echo "⚙️  EXECUTING CONTROLLER REGISTRATION PIPELINE"
echo "======================================================================"
echo "📊 Layout                          : from the RAIDCore metadata (RAIDXpert2)"
echo "💽 Targeted Disk Components        : ${SELECTED_DRIVES}"
echo "----------------------------------------------------------------------"

echo "📝 Step 1: Using the firmware's RAIDCore records — nothing written to the disks."

MEMBER_BDFS=""
for disk in $SELECTED_DRIVES; do
    if [ -e "/sys/block/$disk/device" ]; then
        # /sys/block/nvme0n1/device resolves to the NVMe controller
        # (.../0000:03:00.0/nvme/nvme0), so its basename is "nvme0" and not
        # the PCI address rcraid binds to.  The address is an ancestor on that
        # path; take the last BDF-shaped component of it.
        bdf=$(printf '%s' "$(readlink -f "/sys/block/$disk/device" 2>/dev/null)" \
              | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' | tail -1 || true)
        if [ -n "$bdf" ] && [ -e "/sys/bus/pci/devices/$bdf" ]; then
            MEMBER_BDFS="$MEMBER_BDFS $bdf"
            echo "    • Found PCIe Component: Slot $disk maps to BDF Address $bdf"
            echo "      └─ Detaching native 'nvme' module link from hardware register tree..."
            [ -e "/sys/bus/pci/devices/$bdf/driver_override" ] && echo rcbottom > "/sys/bus/pci/devices/$bdf/driver_override"
            [ -e "/sys/bus/pci/drivers/nvme/$bdf" ] && echo "$bdf" > /sys/bus/pci/drivers/nvme/unbind 2>/dev/null || true
        fi
    fi
done
MEMBER_BDFS="${MEMBER_BDFS# }"

SUBSYSTEM_VENDOR=""
if [ -n "$MEMBER_BDFS" ]; then
    SUBSYSTEM_VENDOR=$(cat "/sys/bus/pci/devices/$(echo $MEMBER_BDFS | awk '{print $1}')/subsystem_vendor" 2>/dev/null || true)
fi
# safe_subsys_vendor is a probe *filter*, and 0 disables it (rc_bottom.c).
# Never pass a guessed vendor the members do not report: on a board whose
# subsystem vendor is the board maker's (0x1987 on this array box) a guessed
# 0x1022 makes probe skip every member and no volume ever assembles.
if [ -z "$SUBSYSTEM_VENDOR" ]; then
    SUBSYSTEM_VENDOR=0
    echo "    • WARN: the members' subsystem_vendor could not be read; loading rcraid without the probe filter."
fi

echo "📥 Step 3: Injecting compiled out-of-tree module (rcraid.ko) with active developer override flags..."
# A firmware-created array needs no create opt-in.  RAID 5 is the level that
# also needs parity_writes=1 before the volume accepts writes; the re-load below
# adds it when a firmware array assembles read-only.
LOAD_PARAMS=(enable_writes=1 "safe_subsys_vendor=$SUBSYSTEM_VENDOR")
echo "    • Parameters: ${LOAD_PARAMS[*]}"
insmod "$SRC_DIR/rcraid.ko" "${LOAD_PARAMS[@]}"

if [ -n "$MEMBER_BDFS" ]; then
    echo "⚡ Step 4: Forcing hardware bus probes across unchained PCIe ports..."
    for bdf in $MEMBER_BDFS; do
        echo "    • Probing PCIe bus address: $bdf"
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    done
fi

# Wait for the driver to publish the volume node: the members are probed right
# after insmod and the node appears a moment later.
wait_for_volume() {
    local i
    udevadm settle 2>/dev/null || true
    for i in {1..20}; do
        [ -b /dev/rcraid0 ] && return 0
        echo "    • [Attempt $i/20] Scanning device mapping nodes..."
        sleep 0.5
    done
    return 1
}

echo "🔍 Step 5: Waiting for the driver validation matrix to instantiate /dev/rcraid0..."
if ! wait_for_volume; then
    echo "❌ Critical Failure: /dev/rcraid0 did not appear." >&2
    echo "   No member was accepted, or the firmware has no array. Check:" >&2
    echo "     dmesg | grep -i 'rcraid\|rc_volume'" >&2
    echo "     lspci -d 1022:b000 -k      # members should show rcbottom" >&2
    exit 1
fi

echo "----------------------------------------------------------------------"
echo "✅ SUCCESS! Unified block storage device array target node is active."
lsblk /dev/rcraid0

# Say plainly what the driver decided about writes. An OS install needs a
# writable volume, and whether this one is writable depends on the level and
# geometry the driver read out of the firmware's records — not on the level
# anyone meant to create.
# Read it from debugfs, which describes the volume assembled right now: dmesg
# keeps every assembly of this boot, so parsing it can answer with the level of
# an array that has since been replaced.
read_volume_state() {
    RO_STATE="$(cat /sys/block/rcraid0/ro 2>/dev/null || echo unknown)"
    VOL_LEVEL="$(sed -n 's/^level:[[:space:]]*//p' \
                  /sys/kernel/debug/rcraid/volume 2>/dev/null | head -1)"
    if [ -z "$VOL_LEVEL" ]; then
        VOL_LEVEL="$(dmesg 2>/dev/null | grep -o '(level=[A-Za-z0-9]*)' \
                     | tail -1 | tr -d '()' || true)"
        VOL_LEVEL="${VOL_LEVEL#level=}"
    fi
    VOL_LEVEL="${VOL_LEVEL^^}"
    [ -n "$VOL_LEVEL" ] || VOL_LEVEL="unknown"
}
read_volume_state

# A RAID 5 array always assembles with parity writes off — the flag is never in
# LOAD_PARAMS, because a firmware array is not created here.  An install needs
# the volume writable, so re-load once with the opt-in instead of sending the
# operator back to the firmware.  Loading with it writes nothing; the flag only
# permits writes *through* the volume afterwards.
if [ "$RO_STATE" != "0" ] && [ "$VOL_LEVEL" = "RAID5" ]; then
    echo "🔁 RAID 5 assembled read-only (parity writes default off) — re-loading rcraid with parity_writes=1..."
    if ! unload_rcraid; then
        echo "❌ rcraid would not unload, so the volume cannot be re-loaded with parity" >&2
        echo "   writes enabled. Reboot the live session and re-run this script." >&2
        exit 1
    fi
    insmod "$SRC_DIR/rcraid.ko" "${LOAD_PARAMS[@]}" parity_writes=1
    for bdf in $MEMBER_BDFS; do
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    done
    if ! wait_for_volume; then
        echo "❌ The array did not re-assemble after enabling parity writes." >&2
        exit 1
    fi
    read_volume_state
fi

if [ "$RO_STATE" = "0" ]; then
    echo "✅ Volume is WRITABLE (level ${VOL_LEVEL}) — the OS installer can use it."
else
    echo "❌ Volume is READ-ONLY (level ${VOL_LEVEL}, ro=${RO_STATE}) — an OS install"
    echo "   cannot proceed on it."
    case "$VOL_LEVEL" in
        RAID5)
            echo "   RAID 5 writes need parity_writes=1, which this script passes, and the"
            echo "   driver still refuses. The veto it logged is above:"
            dmesg 2>/dev/null | grep -i "READ-ONLY\|UNTRUSTED\|parity" | tail -3 | sed 's/^/     /'
            ;;
        *)
            echo "   The driver refused writes for this level/geometry; dmesg says why:"
            dmesg 2>/dev/null | grep -i "READ-ONLY\|UNTRUSTED" | tail -3 | sed 's/^/     /'
            ;;
    esac
    exit 1
fi
echo "======================================================================"

echo "➡️  Open your graphical Kubuntu installer desktop app now."
echo "➡️  Target /dev/rcraid0 under manual partitioning, map root, and let files copy."
echo "======================================================================"

# The final Calamares job (calamares-logs-helper) copies /var/log/casper.log
# into the target.  That file only exists on a casper live boot; launched from
# an installed system the copy fails and Calamares reports "Installation
# Failed" *after* a complete, bootable install.  Give it something to copy.
[ -e /var/log/casper.log ] || sudo touch /var/log/casper.log

read -rp "Press [Enter] ONLY when the graphical OS installer finishes copying file sectors... " _

echo "==> [3/3] Mapping Mount Bridges and Diving Inside the Target OS..."
# Partition nodes appear a moment after the volume node, and every probe below
# iterates /dev/rcraid0p*: without this wait an immediate phase 3 reports "no
# rootfs on the array" while the filesystem is sitting right there.
for _ in {1..20}; do
    ls /dev/rcraid0p* >/dev/null 2>&1 && break
    sleep 0.5
done

for _part in /dev/rcraid0 /dev/rcraid0p*; do
    [ -b "$_part" ] || continue
    # One mount point per line: an empty array expanded as "${mps[@]}" trips
    # `set -u` on some bash builds, and there is usually nothing mounted here
    # anyway, so the loop would abort on the normal path.
    while IFS= read -r mp; do
        [ -n "$mp" ] || continue
        sudo umount -R "$mp" 2>/dev/null || true
    done < <(findmnt -nro TARGET --source "$_part" 2>/dev/null || true)
done

PROBE_DIR=$(mktemp -d)
TARGET_PART=""
target_os=""
for part in /dev/rcraid0p*; do
    [ -b "$part" ] || continue
    if mount -o ro "$part" "$PROBE_DIR" 2>/dev/null; then
        if [ -e "$PROBE_DIR/etc/os-release" ]; then
            TARGET_PART="$part"
            target_os=$(awk -F= '$1 == "ID" {gsub(/["\x27]/, "", $2); print $2; exit}' "$PROBE_DIR/etc/os-release")
            umount "$PROBE_DIR"; break
        fi
        umount "$PROBE_DIR"
    fi
done
rmdir "$PROBE_DIR"

[ -z "$TARGET_PART" ] && { echo "❌ Couldn't find a valid Linux rootfs on the array partitions." >&2; exit 1; }

TARGET_MNT="/mnt/rcraid-target"
mkdir -p "$TARGET_MNT"
mount "$TARGET_PART" "$TARGET_MNT"

for sysfs in dev proc sys run; do mount --rbind /$sysfs "$TARGET_MNT/$sysfs" && mount --make-rslave "$TARGET_MNT/$sysfs"; done

ESP_DEV=""
for part in /dev/rcraid0p*; do
    [ -b "$part" ] || continue
    # Probe the GPT entry directly.  Partitions on the array's own gendisk
    # never get ID_PART_ENTRY_TYPE in the udev database, so `blkid -s PARTTYPE`
    # (and `lsblk -o PARTTYPE`) print nothing for a perfectly valid ESP; they
    # do work for ordinary disks such as the live USB, which is what makes
    # this easy to miss.  `blkid -p` re-reads the parent's GPT itself and
    # reports the type in udev-style output, which is lowercase — lowercase
    # the other side to match.
    part_type=$(blkid -p -o udev "$part" 2>/dev/null | awk -F= '$1 == "ID_PART_ENTRY_TYPE" { print $2; exit }' || true)
    if [ "${part_type,,}" = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" ]; then
        ESP_DEV="$part"
        mkdir -p "$TARGET_MNT/boot/efi"
        mount "$ESP_DEV" "$TARGET_MNT/boot/efi" 2>/dev/null || true; break
    fi
done
cp -L /etc/resolv.conf "$TARGET_MNT/etc/resolv.conf" 2>/dev/null || true

# The DKMS package version comes from dkms.conf, so the tree staged in the
# target matches the PACKAGE_VERSION dkms is told on the command line there.
TGT_VER="$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)".*/\1/p' "$SRC_DIR/dkms.conf")"
[ -n "$TGT_VER" ] || { echo "❌ Cannot read PACKAGE_VERSION from dkms.conf." >&2; exit 1; }
TGT_SRC="$TARGET_MNT/usr/src/rcraid-${TGT_VER}"
# Same layout as the repo: build files at the root, sources under src/ (the
# root Makefile's rcraid-objs paths are relative to it).
mkdir -p "$TGT_SRC/src"
cp "$SRC_DIR"/Makefile "$SRC_DIR"/dkms.conf "$TGT_SRC/"
for f in "$SRC_DIR"/src/*.c "$SRC_DIR"/src/*.h; do
    case "${f##*/}" in
        test_*) continue ;;
    esac
    cp "$f" "$TGT_SRC/src/"
done
if [ -f "$SRC_DIR/docs/VERSION" ]; then
    cp "$SRC_DIR/docs/VERSION" "$TGT_SRC/VERSION"
fi
# Same build-revision stamp install-dkms.sh writes, so a module built here can
# be matched back to the source it came from (the target tree has no .git).
if _rev=$(git -C "$SRC_DIR" describe --always --dirty 2>/dev/null); then
    echo "$_rev" > "$TGT_SRC/.rcraid_rev"
fi
sed -i "s/@PKGVER@/${TGT_VER}/g" "$TGT_SRC/dkms.conf"

# Ask now, while there is someone to ask: Secure Boot is off at this point (see
# the preflight), so this decides whether the installed system gets its own
# signing key.  Enrolling it is what lets Secure Boot be switched back on
# later, and the enrolment completes at the first boot — the same
# signing-after-install ordering install-dkms.sh uses.
RCRAID_TARGET_MOK="${RCRAID_MOK_ENROLL:-}"
if [ -z "$RCRAID_TARGET_MOK" ]; then
    if [ -t 0 ]; then
        read -rp "Enrol the installed system's module-signing key in MOK (needed to re-enable Secure Boot later)? [Y/n] " _ans
        case "$_ans" in
            [nN]*) RCRAID_TARGET_MOK=0 ;;
            *)     RCRAID_TARGET_MOK=1 ;;
        esac
    else
        RCRAID_TARGET_MOK=1
    fi
fi
if [ "$RCRAID_TARGET_MOK" != 1 ]; then
    RCRAID_TARGET_MOK=0
fi

export RCRAID_TARGET_MOK
mkdir -p "$TARGET_MNT/tmp/pkg"
cp -r "$SRC_DIR/packaging" "$TARGET_MNT/tmp/pkg/"
cp -r "$SRC_DIR/scripts/lib" "$TARGET_MNT/tmp/pkg/"

# The chroot steps live in a script rather than a `bash -c` one-liner so they
# can source the shared signing library and fail loudly: `set -e` inside means
# a failed apt/dkms/initramfs step stops the install instead of being hidden
# behind the exit status of whatever ran last.
cat > "$TARGET_MNT/tmp/pkg/rcraid-target.sh" <<'EOS'
#!/bin/bash
set -euo pipefail

VER="$1"
SUBSYS="$2"
VOL_LEVEL_ARG="${3:-}"
. /tmp/pkg/lib/secure-boot.sh

export DEBIAN_FRONTEND=noninteractive
echo "==> installing build dependencies for the installed kernel(s)"
apt-get update -qq
KVER="$(ls /lib/modules | sort -V | tail -1)"
apt-get install -y -qq build-essential "linux-headers-$KVER" dkms \
    initramfs-tools pciutils efibootmgr mokutil >/dev/null

# --- module signing --------------------------------------------------------
# Configured before the build so the module DKMS installs is signed, with the
# enrolment queued for this system's first boot (shim's MOK manager runs
# before that kernel, so it is trusted the first time it is loaded).
MOK_SIGNING=0
MOK_ENROLL_PENDING=0
MOK_KEY=""
MOK_CERT=""
if [ "${RCRAID_TARGET_MOK:-0}" = 1 ]; then
    if mok_find_enrolled_pair; then
        echo "    • signing key already enrolled: $MOK_KEY"
    elif mok_find_pair || mok_create_pair; then
        echo "    • signing key for this system: $MOK_KEY"
    else
        echo "Could not obtain a module signing key in the target system." >&2
        exit 1
    fi
    if ! mok_write_dkms_conf; then
        echo "DKMS in the target cannot be pointed at $MOK_CERT." >&2
        exit 1
    fi
    MOK_SIGNING=1
fi

# --- build, install, and wire the boot path --------------------------------
# One dkms install per installed kernel: -k all also walks kernels whose
# headers are absent and dies on them, and the live kernel is not one of these.
echo "==> building rcraid $VER with DKMS"
dkms add -m rcraid -v "$VER" --quiet 2>/dev/null || true
for k in /lib/modules/*; do
    [ -e "$k/build" ] || continue
    dkms install -m rcraid -v "$VER" -k "${k##*/}" --force
done

install -m 0755 /tmp/pkg/packaging/sbin/rcraid-bind /usr/sbin/rcraid-bind
sed "s/@SUBSYSTEM_VENDOR@/$SUBSYS/g" \
    /tmp/pkg/packaging/udev/50-rcraid.rules.in > /etc/udev/rules.d/50-rcraid.rules
sed "s/@SUBSYSTEM_VENDOR@/$SUBSYS/g" \
    /tmp/pkg/packaging/modprobe.d/rcraid.conf.in > /etc/modprobe.d/rcraid.conf
# A RAID 5/6 volume is forced read-only unless parity_writes=1 is set at load
# time (rc_volume_create_disk: enable_writes alone is not enough), and root
# cannot mount read-write on a read-only volume — the install would not boot.
# Persist the opt-in with the other options, before the initramfs is rebuilt,
# so the copy that modprobe reads from the initrd carries it too.
case "$VOL_LEVEL_ARG" in
    RAID5|RAID6)
        sed -i 's/^\(options[[:space:]]\+rcraid[[:space:]]\+\)/\1parity_writes=1 /' \
            /etc/modprobe.d/rcraid.conf
        ;;
esac
install -m 0755 /tmp/pkg/packaging/initramfs-tools/hooks/rcraid \
    /etc/initramfs-tools/hooks/rcraid
# Hold kernel packages out of unattended-upgrades: a kernel update that
# silently broke the DKMS build is an unbootable system when / lives here.
install -m 0644 /tmp/pkg/packaging/apt.conf.d/52-rcraid-kernel-hold \
    /etc/apt/apt.conf.d/52-rcraid-kernel-hold
update-initramfs -u -k all

# Prove the installed copies are signed: when DKMS cannot read its key it warns
# and carries on, and under Secure Boot that only shows up at the next boot, as
# a missing array.
UNSIGNED=0
for ko in /lib/modules/*/updates/dkms/rcraid.ko*; do
    [ -e "$ko" ] || continue
    signer="$(modinfo -F signer "$ko" 2>/dev/null || true)"
    if [ -n "$signer" ]; then
        echo "    • ${ko%/updates/dkms/*}: signed by $signer"
    else
        echo "    • ${ko%/updates/dkms/*}: UNSIGNED" >&2
        UNSIGNED=1
    fi
done
if [ "$UNSIGNED" = 1 ] && [ "$MOK_SIGNING" = 1 ]; then
    echo "ERROR: rcraid was built unsigned even though a signing key was configured." >&2
    exit 1
fi

# Queue the enrolment last: if anything above failed, the user is not left with
# a MOK prompt at the next boot whose password they never saw.
if [ "$MOK_SIGNING" = 1 ] && ! mok_is_enrolled "$MOK_CERT"; then
    MOK_ENROLL_PENDING=1
    if ! mok_queue_enrollment "$MOK_CERT"; then
        echo "Could not queue the MOK enrolment for the target system." >&2
        exit 1
    fi
    echo "    • MOK enrolment queued — the MOK screen asks for the password you entered"
fi

{
    echo "MOK_SIGNING=$MOK_SIGNING"
    echo "MOK_ENROLL_PENDING=$MOK_ENROLL_PENDING"
    echo "MOK_CERT=$MOK_CERT"
} > /tmp/pkg/mok-state
EOS

chroot "$TARGET_MNT" /bin/bash /tmp/pkg/rcraid-target.sh \
    "$TGT_VER" "$SUBSYSTEM_VENDOR" "$VOL_LEVEL"

# Carry the signing outcome back out of the chroot for the closing message.
mok_state_reset
MOK_STATE="$TARGET_MNT/tmp/pkg/mok-state"
mok_load_state "$MOK_STATE"
rm -f "$MOK_STATE"

if [ -n "$ESP_DEV" ] && [ -d "$TARGET_MNT/boot/efi/EFI" ]; then
    ESP_DIR="$TARGET_MNT/boot/efi"
    ESP_OS_DIR="$ESP_DIR/EFI/$target_os"
    mkdir -p "$ESP_DIR/EFI/BOOT"
    # The removable-media path, independent of the install's own directory
    # name: firmware falls back to it when the NVRAM entry is lost or absent.
    # It needs the *stub* grub.cfg as well as shim+grub.  grub derives its
    # prefix from the image it was loaded from, so without EFI/BOOT/grub.cfg
    # there is no config in that prefix and grub falls back to scanning the
    # other ESPs — which on this box is the OS disk (2026-11-14: the array's
    # EFI/BOOT held shim and grubx64.efi but no grub.cfg, so 'rcraid Array
    # Boot' could chain to /dev/sda1's stub and boot /dev/sda2).
    for boot_file in shimx64.efi grubx64.efi grub.cfg; do
        if [ ! -e "$ESP_OS_DIR/$boot_file" ]; then
            echo "❌ $ESP_OS_DIR/$boot_file is missing.  The firmware entry points at" >&2
            echo "   EFI/BOOT/BOOTX64.EFI; without that stub grub finds no" >&2
            echo "   config there and scans the other ESPs, which can boot the" >&2
            echo "   wrong disk.  Re-run grub-install on the array first." >&2
            exit 1
        fi
    done
    if ! { cp "$ESP_OS_DIR/shimx64.efi" "$ESP_DIR/EFI/BOOT/BOOTX64.EFI" &&
           cp "$ESP_OS_DIR/grubx64.efi" "$ESP_DIR/EFI/BOOT/grubx64.efi" &&
           cp "$ESP_OS_DIR/grub.cfg"   "$ESP_DIR/EFI/BOOT/grub.cfg"; }; then
        echo "❌ Could not populate $ESP_DIR/EFI/BOOT." >&2
        exit 1
    fi
    # The fallback has to resolve the array's root, not another ESP's stub.
    if ! cmp -s "$ESP_OS_DIR/grub.cfg" "$ESP_DIR/EFI/BOOT/grub.cfg"; then
        echo "❌ EFI/BOOT/grub.cfg does not match $ESP_OS_DIR/grub.cfg." >&2
        exit 1
    fi
    # grub-install writes the bootloader to the array's ESP, but the firmware
    # only boots what its own NVRAM menu lists.  That entry is ours to create,
    # and it has to be *verified*: `efibootmgr -c ... || true` was silent when
    # it failed, which is exactly how an install ends with a complete ESP and
    # no array in the boot menu (2026-11-14: shim+grub present on rcraid0p1,
    # no Boot#### referencing its PARTUUID 69818327-...).
    if ! efibootmgr -c -d "${ESP_DEV%p*}" -p "${ESP_DEV##*p}" \
            -L "rcraid Array Boot" -l '\EFI\BOOT\BOOTX64.EFI' >/dev/null; then
        echo "❌ efibootmgr could not add the array to the firmware boot menu." >&2
        echo "   The ESP is written; add the entry by hand:" >&2
        echo "     sudo efibootmgr -c -d ${ESP_DEV%p*} -p ${ESP_DEV##*p} -L 'rcraid Array Boot' -l '\EFI\BOOT\BOOTX64.EFI'" >&2
        exit 1
    fi
    if ! efibootmgr -v | grep -q "rcraid Array Boot"; then
        echo "❌ efibootmgr reported success but no 'rcraid Array Boot' entry is" >&2
        echo "   present. Do not pull any other disk until this is fixed." >&2
        exit 1
    fi
    echo "    • firmware boot entry: 'rcraid Array Boot' -> ${ESP_DEV}"
else
    # Silent here meant an install that looked complete and had no array in the
    # firmware menu: the entry is the only thing that makes the firmware boot
    # the array, so a missing ESP has to stop the install, not be skipped.
    echo "❌ No EFI system partition to put the boot entry on." >&2
    echo "   ESP found on /dev/rcraid0p*: '${ESP_DEV:-none}'" >&2
    echo "   EFI directory under $TARGET_MNT/boot/efi: $([ -d "$TARGET_MNT/boot/efi/EFI" ] && echo present || echo missing)" >&2
    echo "   Looked for PARTTYPE c12a7328-f81f-11d2-ba4b-00a0c93ec93b on the" >&2
    echo "   array's partitions. The OS is installed; only the firmware menu" >&2
    echo "   entry is missing. Add it once an ESP exists:" >&2
    echo "     sudo efibootmgr -c -d /dev/rcraid0 -p <esp-partnum> -L 'rcraid Array Boot' -l '\\EFI\\BOOT\\BOOTX64.EFI'" >&2
    exit 1
fi

umount -R "$TARGET_MNT" 2>/dev/null || true
# The private mount namespace discards anything still mounted when this
# process exits, but a tree that survived its own teardown is exactly the
# state that used to leave rcraid un-unloadable, so say it out loud.
if mountpoint -q "$TARGET_MNT"; then
    echo "⚠️  $TARGET_MNT is still mounted; it will be discarded with the private mount namespace." >&2
fi

mok_enrollment_instructions
if [ "$MOK_SIGNING" = 1 ] && [ "$MOK_ENROLL_PENDING" = 1 ]; then
    echo "After that boot, with the enrolment answered, Secure Boot can be switched"
    echo "back on: the driver and every future DKMS rebuild are signed with it."
    echo
fi

echo -e "\n🎉 DONE! The fresh bare-metal PC is 100% prepped. Reboot whenever you choose to launch your array."
