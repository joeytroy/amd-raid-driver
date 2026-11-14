#!/bin/bash
# Dedicated Phase 1 AMD RAIDXpert2 Verbose Terminal GUI Component
set -euo pipefail

echo "🔍 [TUI Preflight] Scanning host environment for block-layer rendering dependencies..." >&2
if ! command -v dialog &> /dev/null; then
    echo "📦 [TUI Preflight] 'dialog' binary missing in volatile RAM space. Triggering non-interactive apt-get pool..." >&2
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update -qq && sudo apt-get install -y -qq dialog >/dev/null
    echo "✅ [TUI Preflight] Dependency tracking successfully resolved." >&2
else
    echo "✅ [TUI Preflight] Core 'dialog' dependency already active in system tree." >&2
fi

echo "🔍 [TUI Discovery] Sweeping PCIe topology via lsblk for unassigned NVMe endpoints..." >&2
# `|| true` keeps an empty result from killing the script: under `set -e` a
# failing command substitution aborts here, which used to swallow the abort
# message below and look like the picker had simply exited.
RAW_DISKS=$(lsblk -dno NAME,SIZE | awk '$1 ~ /^nvme/ {print $1 " [" $2 "]" " off"}' || true)

if [ -z "$RAW_DISKS" ]; then
    # Inventing disk names here used to end at mkmeta.py with "not a block
    # device node"; say what is actually wrong instead.
    echo "❌ [TUI Abort] No NVMe disks visible." >&2
    echo "   Check BIOS/NVMe mode (AHCI hides 1022:b000) or create the array in" >&2
    echo "   the UEFI RAIDXpert2 utility first — this menu can create RAID" >&2
    echo "   0/1/10 and (behind parity_create) RAID 5 on blank disks." >&2
    exit 1
fi
echo "📊 [TUI Discovery] Discovered targets available for array indexing:" >&2
echo "$RAW_DISKS" | sed 's/^/    • /' >&2

# Members first: which levels the driver will accept depends on how many there
# are (RAID 1 takes exactly two, RAID 10 an even number of four or more, RAID 5
# at least three), so the level menu can only be filtered once they are known.
TEMP_FILE=$(mktemp)
dialog --title " AMD RAIDXpert2 TUI - Select Member Disks " \
       --checklist "Use [Spacebar] to highlight individual disks (MAXIMUM 8 DRIVES):" 15 65 6 \
       $RAW_DISKS 2> "$TEMP_FILE"

SELECTED_DRIVES=$(cat "$TEMP_FILE" | tr -d '"')
rm -f "$TEMP_FILE"

if [ -z "$SELECTED_DRIVES" ]; then
    echo "❌ [TUI Abort] User canceled layout orchestration pass or zero items selected." >&2
    exit 1
fi

read -r -a DRIVE_ARRAY <<< "$SELECTED_DRIVES"
COUNT=${#DRIVE_ARRAY[@]}
echo "📋 [TUI Filter] Registration pass captured $COUNT targeted components: [ $SELECTED_DRIVES ]" >&2

if [ "$COUNT" -gt 8 ]; then
    dialog --title "⚠️  ARRAY LIMIT EXCEEDED" --msgbox "AMD RAID restricts arrays to a maximum of 8 member disks. You chose $COUNT." 8 65
    echo "❌ [TUI Guard] Aborting setup due to fixed structural limit overrun constraint (Max: 8)." >&2
    exit 1
fi

# The level list is the driver's, not this script's: TUI_LEVELS carries what the
# built module reports it can create.  level_ok() mirrors the member-count rules
# of rc_volume_create_array_indexed() so a level the writer would answer with
# -EINVAL is never offered here.
level_ok() {
    case "$1" in
        0)  [ "$COUNT" -ge 2 ] ;;
        1)  [ "$COUNT" -eq 2 ] ;;
        10) [ "$COUNT" -ge 4 ] && [ $((COUNT % 2)) -eq 0 ] ;;
        5)  [ "$COUNT" -ge 3 ] ;;
        *)  return 1 ;;
    esac
}

level_desc() {
    case "$1" in
        0)  echo "RAID 0 (Stripe - High Performance Optimization)" ;;
        1)  echo "RAID 1 (Mirror - Redundancy Safeguard)" ;;
        10) echo "RAID 10 (Striped Mirrors - Minimum 4 Disks Required)" ;;
        5)  echo "RAID 5 (Distributed Parity - Left Asymmetric Rotation)" ;;
        *)  echo "RAID $1" ;;
    esac
}

# Why a driver-supported level does not fit the ticked members.  Kept next to
# level_ok() because both answer the same question for a different audience.
level_why() {
    case "$1" in
        0)  echo "needs at least 2 members" ;;
        1)  echo "needs exactly 2 members" ;;
        10) echo "needs an even count of 4 or more" ;;
        5)  echo "needs at least 3 members" ;;
        *)  echo "not supported by this driver" ;;
    esac
}

MENU_ITEMS=()
VALID_LEVELS=""
SHOWN_LEVELS=""
if [ "${TUI_HAS_EXISTING_ARRAY:-0}" = "1" ]; then
    MENU_ITEMS+=("keep" "Use the existing array as it is (no re-create)")
fi
# shellcheck disable=SC2086  # TUI_LEVELS is a space-separated list on purpose
for _lvl in ${TUI_LEVELS:-}; do
    if level_ok "$_lvl"; then
        MENU_ITEMS+=("$_lvl" "$(level_desc "$_lvl")")
        VALID_LEVELS="$VALID_LEVELS $_lvl"
        SHOWN_LEVELS="${SHOWN_LEVELS:+$SHOWN_LEVELS, }RAID $_lvl"
    else
        # Listed anyway, with the reason: a level that is simply absent reads
        # as a missing feature and sends the operator hunting for a bug.
        MENU_ITEMS+=("$_lvl" "RAID $_lvl - not with ${COUNT} members: $(level_why "$_lvl")")
    fi
done
if [ -z "$SHOWN_LEVELS" ]; then
    dialog --title "⚠️  NO CREATABLE LEVEL" --msgbox "The driver reports no RAID level it can create over $COUNT member(s)." 8 70
    echo "❌ [TUI Abort] Every level the driver reports needs a different member count." >&2
    exit 1
fi

# A level the driver cannot create at all is shown too, for the same reason.
if ! printf '%s\n' ${TUI_LEVELS:-} | grep -x 6 >/dev/null; then
    MENU_ITEMS+=("6" "RAID 6 - NOT SUPPORTED: driver has no Q write/parity path")
fi

# The menu states the probe's answer instead of leaving it implicit: what the
# built driver reports it can create, and which of those fit this member count.
MENU_TEXT="Driver reports it can create: ${TUI_LEVELS:-none}
Supported with ${COUNT} member(s): ${SHOWN_LEVELS}
Choose the array to install onto:"

while :; do
    RAID_LEVEL=$(dialog --title " AMD RAIDXpert2 TUI - Select RAID Level " \
                        --menu "$MENU_TEXT" 19 80 9 \
                        "${MENU_ITEMS[@]}" 3>&1 1>&2 2>&3)
    [ "$RAID_LEVEL" = "keep" ] && break
    case " $VALID_LEVELS " in
        *" $RAID_LEVEL "*) break ;;
    esac
    if [ "$RAID_LEVEL" = "6" ]; then
        dialog --title " RAID 6 — not supported " --msgbox \
            "RAID 6 cannot be created from Linux.\n\nThe driver recognises the 0x1BFA record and can assemble such a volume, but it has no Q write, parity-initialisation or rebuild path, so the volume comes up read-only and cannot host an install.\n\nCreate it in the UEFI RAIDXpert2 utility if the board offers it; this installer will not write that metadata." \
            16 76
    else
        dialog --title " RAID ${RAID_LEVEL} — wrong member count " --msgbox \
            "The driver will not create RAID ${RAID_LEVEL} over ${COUNT} member(s):\n$(level_why "$RAID_LEVEL").\n\nGo back and tick the disks that level needs:\n  RAID 0  - 2 or more\n  RAID 1  - exactly 2\n  RAID 10 - an even count of 4 or more\n  RAID 5  - 3 or more" \
            14 70
    fi
    echo "⚠️  RAID ${RAID_LEVEL} chosen but not creatable with ${COUNT} member(s) — back to the menu." >&2
done

echo "⚙️  [TUI Commit] Selected Matrix Level: RAID ${RAID_LEVEL}" >&2
echo "⚙️  [TUI Commit] Packaging environment metrics to persistent exchange node: ${TUI_ENV_FILE}" >&2

# Dump verified properties directly to the parent environment file descriptors
echo "RAID_LEVEL=\"${RAID_LEVEL}\"" > "${TUI_ENV_FILE}"
echo "SELECTED_DRIVES=\"${SELECTED_DRIVES}\"" >> "${TUI_ENV_FILE}"
