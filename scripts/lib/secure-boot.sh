# shellcheck shell=bash
# Shared Secure Boot handling for the rcraid installers.  Sourced (not
# executed) by install-dkms.sh and install-livecd.sh, which need the same
# answers to the same questions: will this kernel refuse our module, which key
# signs it, and how does that key become trusted?
#
# rcraid.ko is out-of-tree, so a Secure-Boot-enforcing kernel rejects it unless
# it is signed with a key in MOK (or db): insmod fails with "Key was rejected
# by service" and dmesg says "Loading of unsigned module is rejected".
#
# Signing and enrolment are split across a reboot on purpose:
#
#   install time    put a key pair on disk, point DKMS at it (so every build,
#                   including kernel-update rebuilds, is signed), and queue a
#                   MokNew request in firmware
#   next boot       shim's MOK manager — which runs before the kernel — asks
#                   for the one-time password and enrolls the certificate, so
#                   the module that boot loads from initramfs is already
#                   signed by a key the kernel trusts
#
# That is one reboot, and it is the reboot the install needs anyway.  The other
# order (enrol, reboot, then install) costs two, and cannot work at all from a
# live USB: the live session's key lives in RAM, not on the target.
#
# The corollary is that a queued enrolment is a *promise* about the next boot.
# Anything that cannot verify the request is actually pending fails: a silent
# miss here is the "installs fine, array missing after reboot" failure that
# this file exists to prevent.
#
# RCRAID_ALLOW_SECURE_BOOT=1 tells the callers "I sign the modules myself";
# RCRAID_MOK_KEY / RCRAID_MOK_CERT override key discovery; RCRAID_MOK_PASSWORD
# sets the enrolment password instead of prompting for one; RCRAID_MOK_ENROLL=1
# asks for a key even with Secure Boot off (the post-boot signing path, see
# mok_setup_signing); RCRAID_EFIVARS_DIR overrides the efivarfs path (for tests
# only).

# ---------------------------------------------------------------------------
# Firmware state
# ---------------------------------------------------------------------------

# The SecureBoot EFI variable is read directly rather than via mokutil: the
# live-CD preflight runs before any packages are installed.  efivarfs layout
# is 4 bytes of attributes followed by the data; for SecureBoot the data is
# one byte, 1 = enforcing.  No efivarfs entry at all means a BIOS/CSM boot,
# where Secure Boot doesn't exist.
#
# The 5 bytes must be read from offset 0: efivarfs does not support seeking,
# so `od -j4` (and `dd skip=4`) fail with "Illegal seek" / "cannot skip past
# end of combined input" on every real efivar, which would make every probe
# take the fail-closed path below and report Secure Boot as enforcing even on
# machines that have it switched off.
#
# A variable that exists but can't be read (firmware EIO quirks happen on
# real boards) — or reads back empty/truncated, which od reports as SUCCESS
# with no output — FAILS CLOSED: assume enforcing, warn, and let the user
# decide via RCRAID_ALLOW_SECURE_BOOT=1.  Silently treating an anomalous
# variable as "not enforcing" would reproduce the exact only-fails-at-next-
# boot behavior this check exists to prevent.
secure_boot_enforcing() {
    local var byte raw
    for var in "${RCRAID_EFIVARS_DIR:-/sys/firmware/efi/efivars}"/SecureBoot-*; do
        [ -e "$var" ] || break
        # Read attributes + data in one go and take the 5th field; see the
        # seek note above.  The read is kept out of a pipeline so its exit
        # status survives: `od | awk` would report awk's status and turn a
        # failed read into an empty value instead of the unreadable warning.
        if ! raw=$(od -An -tu1 -N5 "$var" 2>/dev/null); then
            echo "WARN: SecureBoot EFI variable exists but can't be read — assuming" >&2
            echo "      Secure Boot is ENFORCING (override: RCRAID_ALLOW_SECURE_BOOT=1)" >&2
            return 0
        fi
        byte=$(printf '%s\n' "$raw" | awk '{print $5}')
        byte="${byte//[[:space:]]/}"
        if [ -z "$byte" ]; then
            echo "WARN: SecureBoot EFI variable exists but is empty/truncated — assuming" >&2
            echo "      Secure Boot is ENFORCING (override: RCRAID_ALLOW_SECURE_BOOT=1)" >&2
            return 0
        fi
        [ "$byte" = "1" ]
        return
    done
    return 1
}

# ---------------------------------------------------------------------------
# Certificates: identity and trust state
# ---------------------------------------------------------------------------

# Print the certificate's SHA-1 as lowercase hex with no separators.  Accepts
# DER or PEM.  Matching on the hash rather than the subject matters: subjects
# are free text, and stale keys sharing a CN are common (Ubuntu's shim key and
# a hand-made one can both say "MOK").
mok_cert_fingerprint() {
    local cert="$1" fp=""
    fp=$(openssl x509 -inform DER -in "$cert" -noout -fingerprint -sha1 2>/dev/null) || fp=""
    if [ -z "$fp" ]; then
        fp=$(openssl x509 -inform PEM -in "$cert" -noout -fingerprint -sha1 2>/dev/null) || fp=""
    fi
    [ -n "$fp" ] || return 1
    printf '%s\n' "${fp#*=}" | tr -d ':' | tr 'A-Z' 'a-z'
}

# $1 = fingerprint, $2 = enrolled|new (the suffix of mokutil's --list-* flag).
# Flattening the listing first makes the comparison independent of mokutil's
# indentation and of where it wraps "SHA1 Fingerprint:" across lines.
mok_list_contains() {
    local fp="$1" list="$2"
    mokutil "--list-$list" 2>/dev/null \
        | tr -d ':' | tr -d ' \t\n' | tr 'A-Z' 'a-z' | grep -q "$fp"
}

# Is this certificate in the firmware's trusted MOK list?
mok_is_enrolled() {
    local fp
    fp=$(mok_cert_fingerprint "$1") || return 1
    [ -n "$fp" ] || return 1
    mok_list_contains "$fp" enrolled
}

# Is an enrolment for this certificate already queued for the next boot?
mok_is_pending() {
    local fp
    fp=$(mok_cert_fingerprint "$1") || return 1
    [ -n "$fp" ] || return 1
    mok_list_contains "$fp" new
}

# ---------------------------------------------------------------------------
# Key pair discovery and creation
# ---------------------------------------------------------------------------

# Default key-pair locations, in preference order: the shim key Debian/Ubuntu
# create, DKMS's own key, then this tree's own convention for systems that
# have neither.
mok_pair_specs() {
    printf '%s %s\n' \
        /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der \
        /var/lib/dkms/mok.key /var/lib/dkms/mok.pub \
        /var/lib/rcraid-mok/rcraid.priv /var/lib/rcraid-mok/rcraid.der
}

# Set MOK_KEY / MOK_CERT to the first pair that exists on disk.  Optional $1 is
# a root prefix, which is how install-livecd.sh looks at the installed system
# instead of the live session.  Returns 1 when nothing is there.
# shellcheck disable=SC2120  # optional root-prefix argument; library callers may pass it
mok_find_pair() {
    local root="${1:-}" key cert
    while read -r key cert; do
        if [ -f "$root$key" ] && [ -f "$root$cert" ]; then
            MOK_KEY="$root$key"
            MOK_CERT="$root$cert"
            return 0
        fi
    done < <(mok_pair_specs)
    return 1
}

# Same, but only pairs the firmware already trusts.  Used first so a machine
# that is already set up never grows a second key and a second MOK prompt.
# shellcheck disable=SC2120  # optional root-prefix argument; library callers may pass it
mok_find_enrolled_pair() {
    local root="${1:-}" key cert
    while read -r key cert; do
        if [ -f "$root$key" ] && [ -f "$root$cert" ] && mok_is_enrolled "$root$cert"; then
            MOK_KEY="$root$key"
            MOK_CERT="$root$cert"
            return 0
        fi
    done < <(mok_pair_specs)
    return 1
}

# Generate a self-signed key pair and set MOK_KEY / MOK_CERT.  Optional $1 is
# a root prefix (live USB target).  The certificate is DER because mokutil
# imports DER only; the 10-year validity keeps kernel-update rebuilds working
# without a re-enrolment every few years, as DKMS's own key does.
# shellcheck disable=SC2120  # optional root-prefix argument; library callers may pass it
mok_create_pair() {
    local root="${1:-}" dir key cert update_policy
    update_policy="$root/usr/sbin/update-secureboot-policy"
    if [ -e "$update_policy" ] || [ -d "$root/var/lib/shim-signed" ]; then
        dir="$root/var/lib/shim-signed/mok"
        key="$dir/MOK.priv"
        cert="$dir/MOK.der"
    elif [ -d "$root/var/lib/dkms" ]; then
        dir="$root/var/lib/dkms"
        key="$dir/mok.key"
        cert="$dir/mok.pub"
    else
        dir="$root/var/lib/rcraid-mok"
        key="$dir/rcraid.priv"
        cert="$dir/rcraid.der"
    fi
    command -v openssl >/dev/null 2>&1 || return 1
    mkdir -p "$dir" || return 1
    if ! openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
            -subj "/CN=rcraid module signing/" \
            -addext "extendedKeyUsage=codeSigning" \
            -keyout "$key" -outform DER -out "$cert" >/dev/null 2>&1; then
        rm -f "$key" "$cert"
        return 1
    fi
    chmod 0600 "$key"
    chmod 0644 "$cert"
    MOK_KEY="$key"
    MOK_CERT="$cert"
}

# ---------------------------------------------------------------------------
# Wiring DKMS and queueing the enrolment
# ---------------------------------------------------------------------------

# Point DKMS at MOK_KEY / MOK_CERT so every build — including the automatic
# rebuild a kernel update triggers — is signed.  Optional $1 is a root prefix.
# Returns 1 when that DKMS has no framework.conf.d support, i.e. when the drop-
# in would be ignored and the distribution default key used instead.
# shellcheck disable=SC2120  # optional root-prefix argument; library callers may pass it
mok_write_dkms_conf() {
    local root="${1:-}" conf key cert
    root="${root%/}"
    conf="$root/etc/dkms/framework.conf.d/rcraid-signing.conf"
    key="$MOK_KEY"
    cert="$MOK_CERT"
    if [ -n "$root" ]; then
        # The drop-in is read by the *target's* DKMS, so it must name the
        # target's paths, not the installer's view of them.
        key="/${key#"$root"/}"
        cert="/${cert#"$root"/}"
    fi
    if ! grep -q 'framework.conf.d' "$root/usr/sbin/dkms" 2>/dev/null; then
        echo "WARN: $root/usr/sbin/dkms does not read /etc/dkms/framework.conf.d;" >&2
        echo "      signing would rely on the distribution default key." >&2
        return 1
    fi
    mkdir -p "$root/etc/dkms/framework.conf.d" || return 1
    cat > "$conf" <<EOF
# Written by rcraid install-dkms.sh / install-livecd.sh.  DKMS sources this when
# signing, so every rebuild — including automatic ones on kernel updates — is
# signed with the key the firmware trusts.
mok_signing_key="$key"
mok_certificate="$cert"
EOF
    chmod 0644 "$conf"
}

# Queue a MokNew request for $1 (a DER certificate).  The password the MOK
# manager will ask for at the next boot is the operator's: typed here, or set
# in RCRAID_MOK_PASSWORD, then written to mokutil's stdin (which it reads
# instead of the terminal when stdin is not a tty).  Nothing is generated —
# the value has to be typed again at the MOK screen, on a text console, so it
# has to be something a person can remember.  It is typed in the clear: this
# runs on the operator's own console, and a value they cannot see is a value
# they will mistype at the MOK screen anyway.
# The password is never returned to the caller or written to a state file;
# when a request for the same certificate is already queued it belongs to
# whoever queued it.
mok_queue_enrollment() {
    local cert="$1" pw="${RCRAID_MOK_PASSWORD:-}"
    if ! command -v mokutil >/dev/null 2>&1; then
        echo "ERROR: mokutil is not installed, so no MOK enrolment can be queued." >&2
        echo "       Install it (package: mokutil) and re-run." >&2
        return 1
    fi
    if mok_is_pending "$cert"; then
        return 0
    fi
    if [ -z "$pw" ]; then
        if [ ! -t 0 ]; then
            echo "ERROR: a MOK enrolment password is needed, but stdin is not a" >&2
            echo "       terminal to ask for one.  Set RCRAID_MOK_PASSWORD and" >&2
            echo "       re-run, or run this installer interactively." >&2
            return 1
        fi
        read -rp "MOK password (typed again at the MOK screen): " pw || {
            echo >&2
            echo "ERROR: no MOK password entered." >&2
            return 1
        }
        if [ "${#pw}" -lt 8 ]; then
            echo "ERROR: the MOK password must be at least 8 characters — that is" >&2
            echo "       the minimum MOK manager accepts." >&2
            return 1
        fi
    fi
    if ! printf '%s\n%s\n' "$pw" "$pw" | mokutil --import "$cert" >/dev/null 2>&1; then
        echo "ERROR: mokutil --import $cert failed." >&2
        return 1
    fi
    if ! mok_is_pending "$cert"; then
        echo "ERROR: mokutil --import $cert reported success but no request is" >&2
        echo "       pending — the key would not be enrolled at the next boot." >&2
        return 1
    fi
    # MokManager waits ~10 s by default and then *clears* the pending request
    # and boots on.  On a machine that boots from the array that is a failed
    # boot with a password nobody got to use, so ask it to wait for a human
    # instead.  Best-effort: mokutil older than 0.5 has no --timeout.
    mokutil --timeout -1 >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# What the installers tell the user
# ---------------------------------------------------------------------------

# One line for an install summary, after mok_setup_signing.
mok_signing_summary() {
    if [ "$MOK_SIGNING" = 1 ]; then
        printf 'DKMS signs with %s on every rebuild' "$MOK_KEY"
        if [ "$MOK_ENROLL_PENDING" = 1 ]; then
            printf ' (enrolment completed at the next boot)'
        fi
    else
        printf 'none — module is unsigned (Secure Boot must stay off)'
    fi
}

# Start from "nothing configured".  Called by the installers before
# mok_load_state / mok_setup_signing so `set -u` never sees an unset MOK_*.
mok_state_reset() {
    MOK_SIGNING=0
    MOK_ENROLL_PENDING=0
    MOK_CERT=""
}

# Adopt the state a target-system install wrote (install-livecd.sh's chroot
# step records it in /tmp/pkg/mok-state), so the closing message can describe
# what the first boot will ask for.  Silent and harmless when the file is
# absent (no signing was requested).
mok_load_state() {
    local f="$1"
    [ -f "$f" ] || return 0
    MOK_SIGNING="$(sed -n 's/^MOK_SIGNING=//p' "$f")"
    MOK_ENROLL_PENDING="$(sed -n 's/^MOK_ENROLL_PENDING=//p' "$f")"
    MOK_CERT="$(sed -n 's/^MOK_CERT=//p' "$f")"
    return 0
}

# The block both installers end with when a MOK enrolment is queued: what the
# reboot will ask for, why skipping it loses the array, and how to recover.
# Silent when nothing is pending.
mok_enrollment_instructions() {
    [ "$MOK_ENROLL_PENDING" = 1 ] || return 0
    cat <<EOF

!! SIGNING FINISHES AT THE NEXT BOOT — MOK ENROLMENT

rcraid is signed with ${MOK_CERT}, and that certificate is queued for
enrolment.  Signing is completed by the reboot that follows this install:
shim's MOK manager runs BEFORE the kernel, so the module in the initramfs is
already signed with a key that boot trusts.

  password: the one you chose when this request was queued.

At the blue "MOK management" screen: Enroll MOK -> Continue -> Yes -> type the
password -> Reboot.  (MOK manager reboots by itself when it is done; that is
the same boot, not another install step.  Its ~10 s default timeout is
disabled, so the screen waits for you instead of dropping the request.)

If you skip the prompt, the key stays untrusted: with Secure Boot enabled the
kernel then rejects rcraid.ko and the array is missing — and when / lives on
the array, that boot cannot complete at all.  Recovery is that same screen on
the following boot, or:

  sudo mokutil --import ${MOK_CERT}

then reboot whenever you choose to — the request waits until you do.

This requires the boot chain to go through shim (the default on Ubuntu/Debian
and Fedora).  A kernel booted by firmware directly through its own EFI stub
never runs MOK manager — there Secure Boot has to stay off.
EOF
}

# ---------------------------------------------------------------------------
# The whole local flow, for install-dkms.sh
# ---------------------------------------------------------------------------

# Sets, for the caller:
#   MOK_SIGNING         1 when DKMS is configured to sign with MOK_KEY/MOK_CERT
#   MOK_ENROLL_PENDING  1 when the next boot must complete a MOK enrolment
#   MOK_KEY / MOK_CERT  the pair in use
#
# Returns non-zero only when the module would end up unsigned on a Secure-Boot-
# enforcing kernel — the caller must abort then, because that is a machine
# whose next boot finds no array (and cannot boot at all if / is on it).
# With Secure Boot off this stays best-effort: a key already on disk is used if
# the firmware trusts it, nothing is created, and nothing is queued — no
# installer should add a MOK prompt to a machine that never asked for one.
# RCRAID_MOK_ENROLL=1 is that machine asking: create the key, sign with it and
# queue the enrolment even though nothing is enforcing yet, so Secure Boot can
# be switched on afterwards.
mok_setup_signing() {
    local enforcing=0 requested=0

    mok_state_reset
    MOK_KEY="${RCRAID_MOK_KEY:-}"
    MOK_CERT="${RCRAID_MOK_CERT:-}"

    if secure_boot_enforcing; then
        enforcing=1
    fi
    # RCRAID_MOK_ENROLL=1 asks for signing even while the firmware is not
    # enforcing.  That is the post-boot path: the machine was installed with
    # Secure Boot off, and this run has to create the key, sign with it and
    # queue the enrolment that lets Secure Boot be switched on later.
    if [ "${RCRAID_MOK_ENROLL:-0}" = 1 ]; then
        requested=1
    fi

    if [ -n "$MOK_KEY" ] || [ -n "$MOK_CERT" ]; then
        if [ -z "$MOK_KEY" ] || [ -z "$MOK_CERT" ] \
           || [ ! -f "$MOK_KEY" ] || [ ! -f "$MOK_CERT" ]; then
            echo "ERROR: RCRAID_MOK_KEY and RCRAID_MOK_CERT must both be set and" >&2
            echo "       name readable files." >&2
            return 1
        fi
    elif ! mok_find_enrolled_pair && ! mok_find_pair; then
        if [ "$enforcing" = 0 ] && [ "$requested" = 0 ]; then
            return 0
        fi
        if ! mok_create_pair; then
            echo "ERROR: could not create a module signing key pair (openssl missing?)." >&2
            return 1
        fi
        echo "==> generated module signing key: $MOK_KEY"
    fi

    if [ "$enforcing" = 0 ] && [ "$requested" = 0 ] && ! mok_is_enrolled "$MOK_CERT"; then
        echo "WARN: found a signing key at $MOK_CERT but it is not enrolled in MOK." >&2
        echo "      Modules signed with it will still be rejected under Secure Boot." >&2
        echo "      Enrol it: sudo mokutil --import $MOK_CERT   (set a one-time" >&2
        echo "      password, reboot, complete the MOK manager prompt), or re-run" >&2
        echo "      this installer with RCRAID_MOK_ENROLL=1 to do all three steps." >&2
        return 0
    fi

    if ! mok_write_dkms_conf; then
        if [ "$enforcing" = 1 ] || [ "$requested" = 1 ]; then
            echo "ERROR: DKMS cannot be pointed at $MOK_CERT, so the module this" >&2
            echo "       install builds would be signed by the wrong key." >&2
            return 1
        fi
        return 0
    fi
    MOK_SIGNING=1

    if ! mok_is_enrolled "$MOK_CERT"; then
        MOK_ENROLL_PENDING=1
        if ! mok_queue_enrollment "$MOK_CERT"; then
            echo "ERROR: a MOK enrolment could not be queued, so nothing would make" >&2
            echo "       the firmware trust the module this install signs." >&2
            return 1
        fi
    fi
    return 0
}
