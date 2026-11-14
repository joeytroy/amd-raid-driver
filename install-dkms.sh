#!/bin/bash
# Install rcraid via DKMS plus boot-time auto-bind glue.
#
# After this script succeeds, the array survives reboots:
#   - DKMS rebuilds the module against every kernel update.
#   - A udev rule hands matching PCI devices to rcbottom at hot-plug time,
#     so /dev/rcraid0 appears during boot without any manual unbind+insmod.
#   - A modprobe.d drop-in records `enable_writes=1` and the array's
#     `safe_subsys_vendor`, so no extra args are needed at load time.
#   - Under Secure Boot the module is signed with a Machine Owner Key, and the
#     enrolment is queued here: the reboot after this install completes the
#     signing, instead of a separate enrolment reboot beforehand.
#
# Requires root.  Run from the repo root.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="rcraid"
# Two versions, and they are not the same thing:
#   PKG_VERSION     the DKMS package version.  Read from dkms.conf so the tree
#                   staged under /usr/src always carries the PACKAGE_VERSION
#                   this script passes to `dkms add/build/install` — dkms
#                   refuses the mismatch if those two ever diverge.
#   DRIVER_VERSION  the AMD release this port matches, from docs/VERSION.  It
#                   is staged as VERSION for the Makefile's banner/modinfo.
PKG_VERSION="$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)".*/\1/p' "$SRC_DIR/dkms.conf")"
if [ -z "$PKG_VERSION" ]; then
    echo "cannot read PACKAGE_VERSION from $SRC_DIR/dkms.conf" >&2
    exit 1
fi
DRIVER_VERSION="$(cat "$SRC_DIR/docs/VERSION" 2>/dev/null || echo unknown)"
DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

[ "$(id -u)" -eq 0 ] || { echo "must be root" >&2; exit 1; }

# Tooling preflight — fail fast with an actionable message rather than later
# with a silent "no devices found" or a confusing dkms error.
for tool in lspci dkms udevadm modprobe; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        case "$tool" in
            lspci)    pkg="pciutils" ;;
            dkms)     pkg="dkms" ;;
            udevadm)  pkg="systemd / udev" ;;
            modprobe) pkg="kmod" ;;
        esac
        echo "missing required tool: $tool (install $pkg)" >&2
        exit 1
    fi
done

# ----------------------------------------------------------------------------
# 0. Vendor-driver preflight.
#
# This tree installs as DKMS package `rcraid` with a module also named
# rcraid, claiming the same 1022:b000 functions as AMD's blob-backed driver.
# The two cannot coexist: step 3 retires every other registered version of
# the package, which deletes the vendor module from every installed kernel
# and its sources from /usr/src.  That is the point of the install — but it
# is also, on a machine booting from the array, the only driver that can
# currently mount the root filesystem, so it must never happen silently.
#
# Read-only probe: `dkms status` and `modinfo` change nothing.  A loaded
# module is recognised as the vendor build by its license, which covers an
# install that DKMS did not register (e.g. a vendor RPM laying one down).
# ----------------------------------------------------------------------------

VENDOR_VERSIONS=()
while read -r ver; do
    [ -n "$ver" ] || continue
    [ "$ver" = "$PKG_VERSION" ] && continue
    VENDOR_VERSIONS+=("$ver")
done < <(dkms status "$PKG_NAME" 2>/dev/null \
             | sed -n "s#^${PKG_NAME}[/,][ ]*\([^,/:]*\)[,/:].*#\1#p" | sort -u)

VENDOR_LOADED=0
if grep -q "^${PKG_NAME} " /proc/modules 2>/dev/null; then
    case "$(modinfo -F license "$PKG_NAME" 2>/dev/null || true)" in
        *[Pp]roprietary*) VENDOR_LOADED=1 ;;
    esac
fi

if [ ${#VENDOR_VERSIONS[@]} -eq 0 ] && [ "$VENDOR_LOADED" -eq 0 ]; then
    echo "==> vendor rcraid preflight: nothing to replace"
    echo
else
    echo "==> vendor rcraid preflight: AMD's driver is installed"
    for ver in "${VENDOR_VERSIONS[@]}"; do
        echo "    DKMS package: ${PKG_NAME}/${ver}"
    done
    if [ "$VENDOR_LOADED" = 1 ]; then
        echo "    loaded now:   ${PKG_NAME} (proprietary build, in use)"
    fi
    cat <<EOF

Installing this tree replaces it: the staging step runs
    dkms remove -m ${PKG_NAME} -v <version> --all
for every version listed above, deleting the vendor module from every
installed kernel and its sources from /usr/src.  If the array is serving /,
this install is replacing the only driver that can mount it, and the loaded
copy keeps working only until the next reboot.

Rollback is to reinstall the vendor package (the Lenovo RPM, or
DesktopECHO/rcraid-nvme's .deb/.rpm), then rebuild the initramfs.

EOF
    if [ "${RCRAID_REPLACE_VENDOR:-0}" = 1 ]; then
        echo "    RCRAID_REPLACE_VENDOR=1 — continuing without a prompt"
    elif [ ! -t 0 ]; then
        cat >&2 <<EOF
ERROR: the vendor driver would be replaced, and stdin is not a terminal,
       so confirmation cannot be asked for.  Nothing was changed.

Re-run interactively, or set RCRAID_REPLACE_VENDOR=1 to accept the
replacement explicitly in a scripted install.
EOF
        exit 1
    else
        answer=""
        read -rp "Type 'replace' to remove AMD's driver and continue: " answer || true
        if [ "$answer" != "replace" ]; then
            echo "Aborted — nothing was changed." >&2
            exit 1
        fi
    fi
    echo
fi

# Secure Boot / module signing.
#
# An enforcing kernel refuses an unsigned module ("Key was rejected by
# service"), and the failure surfaces only at the next boot, with the array
# absent — unbootable when / lives on the array.  Signing is therefore set up
# here and *completed by the reboot this install needs anyway*:
#   1. pick a key pair (an enrolled one if present), or create one,
#   2. point DKMS at it, so this build and every kernel-update rebuild is
#      signed,
#   3. queue a MOK enrolment and verify it really is pending.
# shim's MOK manager runs before the kernel on that boot, so the module in the
# initramfs is trusted the first time it is loaded: enrolment and first use
# happen in the same boot, instead of one reboot to enrol plus another to
# install.  Step 3b below then catches a DKMS that silently failed to sign.
# scripts/lib/secure-boot.sh carries the details.
. "$SRC_DIR/scripts/lib/secure-boot.sh"

# Not `func && VAR=1`: under `set -e` a false return would abort the script.
SB_ENFORCING=0
if secure_boot_enforcing; then
    SB_ENFORCING=1
fi

SETUP_RC=0
mok_setup_signing || SETUP_RC=$?

if [ "$SETUP_RC" -ne 0 ]; then
    if [ "${RCRAID_ALLOW_SECURE_BOOT:-0}" = 1 ]; then
        echo "WARN: continuing because RCRAID_ALLOW_SECURE_BOOT=1 — the module is" >&2
        echo "      left for you to sign by hand.  Sign it before rebooting, then" >&2
        echo "      run scripts/verify-boot-safety.sh (INSTALL.md, \"Secure Boot\")." >&2
        MOK_SIGNING=0
        MOK_ENROLL_PENDING=0
        echo
    else
        cat >&2 <<'EOF'
ERROR: the module would be unsigned, and this kernel will refuse to load it.

Secure Boot is enforcing and the signing set-up above did not finish (usually
mokutil or openssl is missing).  Installing now would look successful and
leave the array absent after the next reboot — on a machine that boots from
the array, that boot cannot complete at all.

Fix what is reported above (usually `sudo apt install mokutil openssl`) and
re-run: the installer creates the key, signs with it, and queues the enrolment
that the post-install reboot completes.

If you sign the modules yourself, point the installer at your key:

  sudo RCRAID_MOK_KEY=/path/MOK.priv RCRAID_MOK_CERT=/path/MOK.der \
      ./install-dkms.sh

or set RCRAID_ALLOW_SECURE_BOOT=1 to have the installer skip signing entirely.
EOF
        exit 1
    fi
fi

# Tell DKMS which key to sign with, so kernel-update rebuilds inherit it.
if [ "$MOK_SIGNING" = 1 ]; then
    echo "==> DKMS signing configured ($MOK_KEY)"
    if [ "$MOK_ENROLL_PENDING" = 1 ]; then
        # Printed here as well as at the end: if a later step fails, the
        # enrolment is already queued and the password must not be lost.
        echo "    MOK enrolment queued for the next boot — the MOK screen asks for the password you entered"
    fi
    echo
fi

echo "==> rcraid install-dkms.sh  (driver ${DRIVER_VERSION}, dkms package ${PKG_NAME}-${PKG_VERSION})"
echo

# ----------------------------------------------------------------------------
# 1. Detect array subsystem_vendor.
#
# The AMD chipset reports every downstream NVMe as PCI 1022:b000, including
# the OS drive.  Subsystem vendor used to be the only way to tell members
# apart, but on a single-vendor box (every drive from the same OEM) it is
# identical for all of them and separates nothing — issue #8.  The value is
# still rendered into the modprobe drop-in as a coarse filter, while the
# binding decision that actually matters is made on the RAIDCore metadata in
# rcraid-bind.  Heuristic here: pick the vendor that has >= 2 devices.  If
# there's a tie, list them and let the user pick.
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

# Warn when the filter cannot discriminate: on these boxes safe_subsys_vendor
# matches the drive carrying / just as well as the members, so relying on it
# would be relying on nothing.
ROOT_BDF=""
ROOT_SRC="$(findmnt -no SOURCE / 2>/dev/null || true)"
if [ -n "$ROOT_SRC" ] && [ -b "$ROOT_SRC" ]; then
    while read -r kname ktype; do
        [ "$ktype" = disk ] || continue
        [ -n "$kname" ] || continue
        link=$(readlink -f "/sys/block/$kname/device" 2>/dev/null) || continue
        case "$link" in
            */nvme/*)
                ROOT_BDF="${link%/nvme/*}"
                ROOT_BDF="${ROOT_BDF##*/}"
                break
                ;;
        esac
    done < <(lsblk -nrso KNAME,TYPE "$ROOT_SRC" 2>/dev/null)
fi

if [ -n "$ROOT_BDF" ] && [ -r "/sys/bus/pci/devices/$ROOT_BDF/subsystem_vendor" ] \
   && [ "$(cat "/sys/bus/pci/devices/$ROOT_BDF/subsystem_vendor")" = "$SUBSYSTEM_VENDOR" ]; then
    echo
    echo "NOTE: the drive backing / ($ROOT_BDF) reports the same subsystem_vendor"
    echo "      ($SUBSYSTEM_VENDOR) as the array, so safe_subsys_vendor cannot tell"
    echo "      them apart.  That is normal on a single-vendor box: the udev rule"
    echo "      and rcraid-bind gate on RAIDCore metadata at LBA 0x5000 instead and"
    echo "      leave this drive bound to nvme."
fi
echo

# ----------------------------------------------------------------------------
# 2. Unload nvme and ahci so the new module can claim the controller.
#
# The kernel's nvme driver matches the RAID Bottom by class code (0x010802),
# not only by vendor ID, so it binds every trapped member before rcraid gets
# a chance; ahci does the same for the SATA RAID variants.  Clearing them out
# here lets the freshly installed rcraid probe 1022:b000 itself, and shows
# which of the two the running system can actually live without before the
# reboot makes that the boot-time configuration.
#
# This is a runtime unload, not a blacklist: after the reboot both load again
# and the udev rule still takes members away from nvme one BDF at a time, so
# docs/INSTALL.md's "do not blacklist nvme" remains the boot-time rule.
#
# Non-fatal by design.  A module with a mounted filesystem behind it cannot be
# unloaded (EBUSY), and on a box whose / is a plain NVMe or SATA disk that is
# the correct outcome — report it and carry on, because rcraid-bind can still
# release only the members it verifies at LBA 0x5000.
# ----------------------------------------------------------------------------

UNLOAD_FAILED=()

echo "==> unloading conflicting block drivers (nvme, ahci)"
for mod in nvme ahci; do
    if [ ! -d "/sys/module/$mod" ]; then
        echo "    $mod: not loaded"
        continue
    fi
    if [ ! -e "/sys/module/$mod/refcnt" ]; then
        echo "WARN: $mod is built into this kernel — it cannot be unloaded." >&2
        echo "      If it claims 1022:b000 before rcraid, boot with" >&2
        echo "      modprobe.blacklist=$mod and retry." >&2
        continue
    fi
    if modprobe -r "$mod" 2>&1 | sed 's/^/    /'; then
        echo "    $mod: unloaded"
    else
        UNLOAD_FAILED+=("$mod")
        echo "WARN: $mod could not be unloaded (in use by a mounted filesystem" >&2
        echo "      or an open device).  Continuing; rcraid-bind will release" >&2
        echo "      only the array members it can verify at LBA 0x5000." >&2
    fi
done
echo

# ----------------------------------------------------------------------------
# 3. Stage sources under /usr/src/<pkg>-<ver> and run dkms install.
# ----------------------------------------------------------------------------

echo "==> staging sources at $DKMS_SRC"
rm -rf "$DKMS_SRC"
mkdir -p "$DKMS_SRC/src"
# Stage the tree's own layout, not a flat copy: the root Makefile lists its
# objects as src/*.o, so the sources have to arrive under src/ or kbuild finds
# nothing to build.  test_*.c are userspace harnesses, not part of the module.
cp "$SRC_DIR"/Makefile "$SRC_DIR"/dkms.conf "$DKMS_SRC/"
for f in "$SRC_DIR"/src/*.c "$SRC_DIR"/src/*.h; do
    case "${f##*/}" in
        test_*) continue ;;
    esac
    cp "$f" "$DKMS_SRC/src/"
done

# docs/VERSION is the AMD release this port matches; the Makefile reads it as
# VERSION from the source dir and bakes it into the banner and modinfo.
if [ -f "$SRC_DIR/docs/VERSION" ]; then
    cp "$SRC_DIR/docs/VERSION" "$DKMS_SRC/VERSION"
fi

# Substitute the package version if the repo file still carries the
# placeholder; with a literal PACKAGE_VERSION this is a no-op.
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

# `dkms build` / `dkms install` default to the RUNNING kernel only.  rcraid
# backs /, so every kernel the bootloader can pick needs its own module:
# installing for the running one alone leaves an older entry without the
# driver, and that boot stops at a missing root filesystem.  Do both once per
# installed kernel, and name the kernel in any failure so it is obvious which
# entry must not be rebooted into.
KERNELS=()
KERNELS_NO_HEADERS=()
for d in /lib/modules/*; do
    [ -d "$d" ] || continue
    kver="${d##*/}"
    if [ -e "$d/build" ]; then
        KERNELS+=("$kver")
    else
        KERNELS_NO_HEADERS+=("$kver")
    fi
done

if [ ${#KERNELS[@]} -eq 0 ]; then
    echo "ERROR: no kernel with headers under /lib/modules — cannot build" >&2
    echo "       rcraid.  Install linux-headers for the running kernel." >&2
    exit 1
fi

if [ ${#KERNELS_NO_HEADERS[@]} -gt 0 ]; then
    echo "WARN: no headers for ${KERNELS_NO_HEADERS[*]} — rcraid cannot be built" >&2
    echo "      for those kernels, so no module is installed for them.  Install" >&2
    echo "      their headers and re-run this script before booting them." >&2
    echo
fi

echo "==> dkms build/install for: ${KERNELS[*]}"
for kver in "${KERNELS[@]}"; do
    echo "==> dkms build -k $kver"
    if ! dkms build -m "$PKG_NAME" -v "$PKG_VERSION" -k "$kver" --force 2>&1 \
            | sed 's/^/    /'; then
        echo "ERROR: dkms build FAILED for $kver — do not reboot into it." >&2
        exit 1
    fi

    echo "==> dkms install -k $kver"
    if ! dkms install -m "$PKG_NAME" -v "$PKG_VERSION" -k "$kver" --force 2>&1 \
            | sed 's/^/    /'; then
        echo "ERROR: dkms install FAILED for $kver — do not reboot into it." >&2
        exit 1
    fi
done
echo

# ----------------------------------------------------------------------------
# 3b. Prove what DKMS installed is actually signed.
#
# DKMS signs during build, but a missing/unreadable key makes it print a
# warning and carry on with an unsigned module.  Under Secure Boot that is
# only visible at the next boot, as a missing array — so check here instead.
# ----------------------------------------------------------------------------

if [ "$MOK_SIGNING" = 1 ] || [ "$SB_ENFORCING" = 1 ]; then
    echo "==> verifying module signatures"
    UNSIGNED=0
    for ko in /lib/modules/*/updates/dkms/${PKG_NAME}.ko*; do
        [ -e "$ko" ] || continue
        # <ko> is .../<kver>/updates/dkms/<name>.ko[.zst] — climb three levels.
        KVER="$(basename "$(dirname "$(dirname "$(dirname "$ko")")")")"
        SIGNER="$(modinfo -F signer "$ko" 2>/dev/null || true)"
        if [ -n "$SIGNER" ]; then
            echo "    $KVER: signed by $SIGNER"
        else
            echo "    $KVER: UNSIGNED ($ko)" >&2
            UNSIGNED=1
        fi
    done

    if [ "$UNSIGNED" = 1 ]; then
        # RCRAID_ALLOW_SECURE_BOOT=1 means "I sign these myself": warn rather
        # than undo the install the user explicitly asked for.
        if [ "$SB_ENFORCING" = 1 ] && [ "${RCRAID_ALLOW_SECURE_BOOT:-0}" != 1 ]; then
            echo >&2
            echo "ERROR: at least one installed ${PKG_NAME} module is unsigned while" >&2
            echo "       Secure Boot is enforcing — the kernel will reject it and the" >&2
            echo "       array will be missing at the next boot." >&2
            echo "       Re-sign by hand (INSTALL.md, \"Secure Boot\") then re-run" >&2
            echo "       scripts/verify-boot-safety.sh.  Do not reboot yet." >&2
            exit 1
        fi
        echo "WARN: unsigned module(s) installed." >&2
        if [ "$SB_ENFORCING" = 1 ]; then
            echo "      Secure Boot is enforcing and RCRAID_ALLOW_SECURE_BOOT=1 was" >&2
            echo "      given, so signing is left to you: sign each module listed" >&2
            echo "      above before rebooting (INSTALL.md, \"Secure Boot\"), then" >&2
            echo "      run scripts/verify-boot-safety.sh." >&2
        else
            echo "      Harmless while Secure Boot is off, fatal the moment it is" >&2
            echo "      switched back on." >&2
        fi
    fi
    echo
fi

# ----------------------------------------------------------------------------
# 4. Lay down the udev rule, helper, and modprobe drop-in.
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
# 5. Persist AMD's documented boot-line module blacklist.
#
# AMD-RAIDXpert2 User Guide Rev 3.11 (June 2020), "Boot Options": add
# `modprobe.blacklist=ahci,nvme` to the Linux boot string.  The RAID Bottom
# presents as class 0x010802, which the kernel nvme driver matches by class,
# and the SATA variants look like AHCI controllers — both autoload during
# coldplug, before rcraid's probe can claim the functions.  The udev rule can
# take a verified member back afterwards; with this, nothing has to.
#
# It goes on the kernel command line rather than in modprobe.d because it must
# be in force before udev coldplug, initramfs included.  A module that step 2
# could not unload is backing a live device, so it is never blacklisted here.
# ----------------------------------------------------------------------------

echo "==> boot-line module blacklist (modprobe.blacklist=ahci,nvme)"

cmdline_blacklist="$(tr ' ' '\n' < /proc/cmdline \
                     | sed -n 's/^modprobe\.blacklist=//p' | tr ',' '\n')"
BLACKLIST_MISSING=()
for mod in ahci nvme; do
    printf '%s\n' "$cmdline_blacklist" | grep -qx "$mod" \
        || BLACKLIST_MISSING+=("$mod")
done

if [ ${#BLACKLIST_MISSING[@]} -eq 0 ]; then
    echo "    already active on the running kernel"
else
    APPLY=()
    for mod in "${BLACKLIST_MISSING[@]}"; do
        in_use=0
        for failed in "${UNLOAD_FAILED[@]}"; do
            if [ "$failed" = "$mod" ]; then
                in_use=1
            fi
        done
        if [ "$in_use" = 1 ]; then
            echo "WARN: not blacklisting $mod — it is in use by a live device," >&2
            echo "      and removing it from the boot line would break that mount." >&2
        else
            APPLY+=("$mod")
        fi
    done

    if [ ${#APPLY[@]} -eq 0 ]; then
        echo "    nothing safe to add"
    elif [ ! -f /etc/default/grub ]; then
        echo "WARN: no /etc/default/grub — add this to your boot loader entries:" >&2
        echo "      modprobe.blacklist=$(IFS=,; echo "${APPLY[*]}")" >&2
    else
        GRUB_FILE=/etc/default/grub
        # Update whichever cmdline variable already carries a blacklist;
        # otherwise the default one.  Both quote styles are in the wild.
        target="GRUB_CMDLINE_LINUX_DEFAULT"
        if grep -qE '^GRUB_CMDLINE_LINUX(_DEFAULT)?=.*modprobe\.blacklist=' "$GRUB_FILE"; then
            target="$(sed -n \
                's/^\(GRUB_CMDLINE_LINUX\(_DEFAULT\)\?\)=.*modprobe\.blacklist=.*/\1/p' \
                "$GRUB_FILE" | head -1)"
        fi
        # grep exits 1 when the variable is absent (fresh minimal grub file),
        # and grep -v exits 1 when the value is nothing but the blacklist —
        # either would abort the script under pipefail before the edit.
        line="$(grep -E "^${target}=" "$GRUB_FILE" | head -1 || true)"
        val="$(printf '%s\n' "$line" | sed -E 's/^[A-Z_]+=//' | tr -d "\"'")"
        rest="$(printf '%s\n' "$val" | tr ' ' '\n' \
                | sed -n '/^modprobe\.blacklist=/!p' \
                | sed '/^$/d' | paste -sd' ' -)"
        list="$(printf '%s\n' "$val" | tr ' ' '\n' \
                | sed -n 's/^modprobe\.blacklist=//p' | tr ',' '\n'
                printf '%s\n' "${APPLY[@]}")"
        list="$(printf '%s\n' "$list" | sed '/^$/d' | sort -u | paste -sd, -)"
        new="${rest:+$rest }modprobe.blacklist=$list"

        [ -e "$GRUB_FILE.rcraid.bak" ] || cp -a "$GRUB_FILE" "$GRUB_FILE.rcraid.bak"
        tmp="$(mktemp)"
        awk -v t="$target" -v v="$new" '
            $0 ~ "^" t "=" { print t "=\"" v "\""; seen = 1; next }
            { print }
            END { if (!seen) print t "=\"" v "\"" }
        ' "$GRUB_FILE" > "$tmp"
        cat "$tmp" > "$GRUB_FILE"
        rm -f "$tmp"
        echo "    $target=\"$new\"  (backup: $GRUB_FILE.rcraid.bak)"

        if command -v update-grub >/dev/null 2>&1; then
            if ! update-grub 2>&1 | sed 's/^/    /'; then
                echo "ERROR: update-grub failed — the running kernel keeps its" >&2
                echo "       cmdline, but the next boot may lack the blacklist." >&2
                exit 1
            fi
        elif command -v grub2-mkconfig >/dev/null 2>&1 && [ -d /boot/grub2 ]; then
            if ! grub2-mkconfig -o /boot/grub2/grub.cfg 2>&1 | sed 's/^/    /'; then
                echo "ERROR: grub2-mkconfig failed — see above." >&2
                exit 1
            fi
        elif command -v grub-mkconfig >/dev/null 2>&1 && [ -d /boot/grub ]; then
            if ! grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | sed 's/^/    /'; then
                echo "ERROR: grub-mkconfig failed — see above." >&2
                exit 1
            fi
        else
            echo "WARN: no Grub configuration tool found — run your boot loader's" >&2
            echo "      generator by hand so the new command line is picked up." >&2
        fi
    fi
fi
echo

# ----------------------------------------------------------------------------
# 6. Install initramfs hook so the array can serve /, and regenerate the
#    initramfs for every installed kernel — not just the running one.
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

    echo "==> regenerating initramfs (dracut -f --kver <kver>)"
    for kver in "${KERNELS[@]}"; do
        echo "    $kver"
        if ! dracut -f --kver "$kver" 2>&1 | sed 's/^/      /'; then
            echo "ERROR: dracut FAILED for $kver — that initramfs does NOT" >&2
            echo "       contain rcraid.  Do NOT reboot into it until" >&2
            echo "       'dracut -f --kver $kver' succeeds (a full /boot is" >&2
            echo "       the usual cause), then re-run this script or verify" >&2
            echo "       with scripts/verify-boot-safety.sh." >&2
            exit 1
        fi
    done
elif command -v update-initramfs >/dev/null 2>&1; then
    INITRAMFS_GENERATOR="initramfs-tools"
    echo "==> installing initramfs-tools hook"
    install -d /etc/initramfs-tools/hooks
    install -m 0755 \
        "$SRC_DIR/packaging/initramfs-tools/hooks/rcraid" \
        /etc/initramfs-tools/hooks/rcraid

    echo "==> regenerating initramfs (update-initramfs -u -k <kver>)"
    for kver in "${KERNELS[@]}"; do
        echo "    $kver"
        if ! update-initramfs -u -k "$kver" 2>&1 | sed 's/^/      /'; then
            echo "ERROR: update-initramfs FAILED for $kver — that initramfs" >&2
            echo "       does NOT contain rcraid.  Do NOT reboot into it until" >&2
            echo "       'update-initramfs -u -k $kver' succeeds (a full /boot" >&2
            echo "       is the usual cause), then re-run this script or verify" >&2
            echo "       with scripts/verify-boot-safety.sh." >&2
            exit 1
        fi
    done
else
    echo "==> no initramfs generator found (dracut / update-initramfs)"
    echo "    Skipping initramfs hook install.  The array will only be"
    echo "    available AFTER the root filesystem mounts.  This is fine if"
    echo "    your rootfs is NOT on rcraid; if it is, install dracut or"
    echo "    initramfs-tools and re-run this script."
fi
echo

# ----------------------------------------------------------------------------
# 7. Done.  Tell the user what happens next.
# ----------------------------------------------------------------------------

if grep -q '^rcraid ' /proc/modules; then
    cat <<EOF
NOTE: rcraid is currently loaded, so the running module is still the old
build.  The freshly installed module (and the regenerated initramfs) take
effect on the next reboot.

EOF
fi

SIGNING_SUMMARY="$(mok_signing_summary)"

cat <<EOF
==> Install complete.

What's installed:
  DKMS module    /usr/src/${PKG_NAME}-${PKG_VERSION}/
                 /lib/modules/<kver>/updates/dkms/rcraid.ko, for:
                 ${KERNELS[*]}
  Signing        ${SIGNING_SUMMARY}
$([ ${#KERNELS_NO_HEADERS[@]} -gt 0 ] && echo "  WARNING        no driver built for: ${KERNELS_NO_HEADERS[*]}")
  Bind helper    /usr/sbin/rcraid-bind
  udev rule      /etc/udev/rules.d/50-rcraid.rules
                 (binds only devices carrying RAIDCore metadata)
  modprobe conf  /etc/modprobe.d/rcraid.conf
                 (enable_writes=1, safe_subsys_vendor=$SUBSYSTEM_VENDOR)
  initramfs      ${INITRAMFS_GENERATOR:-<none — boot-from-RAID disabled>}
$([ -e /etc/apt/apt.conf.d/52-rcraid-kernel-hold ] && echo '  apt hold       /etc/apt/apt.conf.d/52-rcraid-kernel-hold
                 (kernel packages held back from unattended-upgrades)')

After any kernel update, run scripts/verify-boot-safety.sh BEFORE
rebooting — it confirms rcraid is built and present in every installed
kernel's initramfs.
EOF

if [ "$MOK_ENROLL_PENDING" = 1 ]; then
    MOK_FP="$(mok_cert_fingerprint "$MOK_CERT" 2>/dev/null || true)"
    mok_enrollment_instructions
    cat <<EOF

Next: reboot, answer the MOK prompt above, then check:

  mokutil --list-enrolled | grep -i ${MOK_FP:0:8}   # key is now trusted
  lsblk /dev/rcraid0                                 # volume + partitions
  lspci -d 1022:b000 -k                              # members show "rcbottom"
EOF
else
    cat <<EOF

Next: reboot to test the auto-bind path end-to-end.  After reboot:

  lsblk /dev/rcraid0     # should show your volume + partitions
  lspci -d 1022:b000 -k  # array members should show "rcbottom"
EOF
fi

if [ "$MOK_ENROLL_PENDING" = 1 ]; then
    cat <<'EOF'

Note: the one-shot load below is rejected until the MOK prompt is answered
("Key was rejected by service") — the key is not trusted yet.
EOF
fi

cat <<EOF

To take effect WITHOUT rebooting (one-shot test):

  for bdf in ${vendor_bdfs[$SUBSYSTEM_VENDOR]# }; do
      /usr/sbin/rcraid-bind \$bdf
  done

To remove: sudo ./uninstall-dkms.sh
EOF
