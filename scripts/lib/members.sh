# shellcheck shell=bash
# Shared array-member detection and config rendering for the rcraid
# installers.  Sourced (not executed) by install-dkms.sh and
# install-livecd.sh.
#
# Two platform families present array members differently:
#
#   b000  — TRX40/TRX50/WRX90, X870/X670, B850/B650, …  The chipset shadows
#           EVERY downstream NVMe (OS drive included) as PCI 1022:b000, so
#           members are told apart by subsystem_vendor: the vendor with >= 2
#           devices is the array.
#
#   x399  — 1st/2nd-gen Threadripper.  AMD's package binds NVMe by class code
#           (PCI\CC_010802); members keep their NATIVE vendor/device IDs and
#           may mix vendors, so neither the PCI ID nor subsystem_vendor says
#           anything.  Members are identified by what is on them: the
#           RAIDCore metadata block at LBA 0x5000, read while the stock nvme
#           driver still owns the drive.  Drives without it are never
#           touched — the OS disk is safe by construction.
#
# detect_members sets:
#   BIND_MODE         b000 | x399
#   MEMBER_BDFS       space-separated PCI addresses of the members
#   SUBSYSTEM_VENDOR  (b000 only) the array's subsystem_vendor
#
# render_member_config <root> then writes <root>/etc/udev/rules.d/50-rcraid.rules
# and <root>/etc/modprobe.d/rcraid.conf for that mode.

RCRAID_RAIDCORE_LBA=20480	# 0x5000, in the device's logical blocks

# raidcore_member <block-device>: true when the namespace carries the
# RAIDCore metadata block (magic "RAIDCore" at byte 8 of LBA 0x5000).
# Same check as packaging/sbin/rcraid-bind.
raidcore_member() {
    local lbs
    lbs=$(cat "/sys/class/block/$(basename "$1")/queue/logical_block_size" 2>/dev/null) || return 1
    [ "$(dd if="$1" bs="$lbs" skip="$RCRAID_RAIDCORE_LBA" count=1 2>/dev/null \
         | dd bs=1 skip=8 count=8 2>/dev/null | tr -d '\000')" = RAIDCore ]
}

detect_b000_members() {
    local bdf sv i n choice
    local -A vendor_count=() vendor_bdfs=()
    local candidates=()

    while read -r bdf; do
        [ -n "$bdf" ] || continue
        sv=$(cat "/sys/bus/pci/devices/$bdf/subsystem_vendor")
        vendor_count[$sv]=$((${vendor_count[$sv]:-0} + 1))
        vendor_bdfs[$sv]="${vendor_bdfs[$sv]:-} $bdf"
    done < <(lspci -d 1022:b000 -D | awk '{print $1}')

    [ ${#vendor_count[@]} -gt 0 ] || return 1

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
        echo "rcraid needs a multi-drive array.  Aborting." >&2
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

    BIND_MODE=b000
    MEMBER_BDFS="${vendor_bdfs[$SUBSYSTEM_VENDOR]# }"
}

# Needs the members still bound to nvme (their namespaces visible).  Members
# already handed to rcbottom (an installer re-run in the same session) are
# picked up from their driver_override instead.
detect_x399_members() {
    local ns dev bdf found=""

    for ns in /sys/class/block/nvme*n*; do
        [ -e "$ns" ] || continue
        case "$(basename "$ns")" in *p*) continue ;; esac
        bdf=$(basename "$(readlink -f "$ns/device/device")")
        [ "$(cat "/sys/bus/pci/devices/$bdf/device")" = 0xb000 ] && continue
        dev="/dev/$(basename "$ns")"
        if raidcore_member "$dev"; then
            case " $found " in *" $bdf "*) ;; *) found="$found $bdf" ;; esac
        fi
    done
    for dev in /sys/bus/pci/devices/*; do
        [ "$(cat "$dev/driver_override" 2>/dev/null)" = rcbottom ] || continue
        bdf=$(basename "$dev")
        case " $found " in *" $bdf "*) ;; *) found="$found $bdf" ;; esac
    done

    [ -n "$found" ] || return 1
    # shellcheck disable=SC2086  # word-split + sort the BDF list
    MEMBER_BDFS="$(printf '%s\n' $found | sort | tr '\n' ' ')"
    MEMBER_BDFS="${MEMBER_BDFS% }"
    BIND_MODE=x399
    SUBSYSTEM_VENDOR=""
}

detect_members() {
    if detect_b000_members; then
        return 0
    fi
    if detect_x399_members; then
        return 0
    fi
    cat >&2 <<'EOF'
No array members found: no PCI 1022:b000 devices, and no NVMe drive
carries AMD RAIDCore metadata.

Make sure the BIOS is in RAID mode (NVMe RAID enabled) and that an array
has been created in RAIDXpert2.
EOF
    exit 1
}

# Module option that keeps rcraid off drives that aren't members.
member_filter_option() {
    case "$BIND_MODE" in
        b000) echo "safe_subsys_vendor=$SUBSYSTEM_VENDOR" ;;
        x399) echo "allow_foreign_nvme=1" ;;
    esac
}

render_member_config() {
    local root="$1" pkg="$2" bdf
    local rules="$root/etc/udev/rules.d/50-rcraid.rules"
    local conf="$root/etc/modprobe.d/rcraid.conf"

    mkdir -p "$(dirname "$rules")" "$(dirname "$conf")"
    case "$BIND_MODE" in
        b000)
            sed "s/@SUBSYSTEM_VENDOR@/$SUBSYSTEM_VENDOR/g" \
                "$pkg/udev/50-rcraid.rules.in" > "$rules"
            ;;
        x399)
            {
                sed '/@MEMBER_RULES@/,$d' "$pkg/udev/50-rcraid-x399.rules.in"
                for bdf in $MEMBER_BDFS; do
                    printf 'ACTION=="add", SUBSYSTEM=="block", ENV{DEVTYPE}=="disk", KERNEL=="nvme*n*", KERNELS=="%s", RUN+="/usr/sbin/rcraid-bind --disk $devnode"\n' "$bdf"
                done
            } > "$rules"
            ;;
    esac
    sed "s/@MEMBER_FILTER@/$(member_filter_option)/g" \
        "$pkg/modprobe.d/rcraid.conf.in" > "$conf"
    chmod 0644 "$rules" "$conf"
}
