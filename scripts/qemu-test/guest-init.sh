#!/bin/sh
# /init for the rcraid QEMU test VM (busybox initramfs).
#
# Loads rcraid.ko with allow_foreign_nvme=1, binds every NVMe-class PCI
# function to rcbottom via sysfs new_id (QEMU's virtual NVMe is 1b36:0010
# — not in the module's ID table; the opt-in lets the driver accept
# non-B000 NVMe-class functions), waits for /dev/rcraid0, verifies its
# size against the
# expected_sectors= kernel arg, then round-trips data through the volume
# with the page cache dropped between write and read.
#
# Prints exactly one of RCRAID-TEST-PASS / RCRAID-TEST-FAIL on the serial
# console; the host runner greps for it.  Always powers off.

export PATH=/bin:/sbin
/bin/busybox --install -s /bin

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null

fail() {
    echo "rcraid-test: FAILURE DETAIL: $1"
    echo "rcraid-test: --- /proc/interrupts (MSI-X vector counts) ---"
    cat /proc/interrupts
    echo "rcraid-test: --- last dmesg lines ---"
    dmesg | tail -n 150
    echo "RCRAID-TEST-FAIL"
    poweroff -f
    # Backstop: if poweroff somehow doesn't halt PID 1, do NOT return to
    # the caller — falling through could reach the PASS marker and report
    # a false pass to the host runner.
    exit 1
}

# Timeline marker: lands in dmesg between the driver's own log lines, so
# failures can be anchored to the test step that triggered them.
mark() {
    echo "rcraid-test: MARK $1" > /dev/kmsg
}

expected_sectors=""
expected_level=""
fail_member=0
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        expected_sectors=*) expected_sectors="${arg#expected_sectors=}" ;;
        expected_level=*)   expected_level="${arg#expected_level=}" ;;
        fail_member=*)      fail_member="${arg#fail_member=}" ;;
    esac
done
[ -n "$expected_sectors" ] || fail "no expected_sectors= on kernel cmdline"

# Bind every NVMe-class PCI function to rcbottom, the same way the real
# installers do it: pin with driver_override, unbind whatever driver holds
# the device, and kick a re-probe.  new_id alone is NOT enough — kernels
# with the in-tree nvme driver built in (e.g. Ubuntu's azure flavor on CI
# runners) bind the virtual disks during boot, and new_id can't steal an
# already-bound device.  driver_override outlives a module reload, but the
# devices sit unbound after rmmod — the reload cycle below calls this again
# for the re-probe kick.
bind_nvme_functions() {
    found=0
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/class")" = "0x010802" ] || continue
        bdf="${d##*/}"
        # Already ours?  Do NOT unbind+rebind: with degraded mode in the
        # driver, unbinding a live member is a hot-remove (the volume drops
        # to degraded and the member can't rejoin without resync) — the
        # old teardown-and-reassemble behavior this dance relied on is gone.
        case "$(readlink "$d/driver" 2>/dev/null)" in
            */rcbottom) found=$((found + 1)); continue ;;
        esac
        echo rcbottom > "$d/driver_override"
        if [ -e "$d/driver" ]; then
            echo "$bdf" > "$d/driver/unbind" 2>/dev/null
        fi
        echo "$bdf" > /sys/bus/pci/drivers_probe 2>/dev/null
        found=$((found + 1))
    done
    [ "$found" -ge 2 ] || fail "found $found NVMe functions, need >= 2"

    # new_id probes synchronously, so the driver symlink tells us whether
    # each function really bound (a failed probe or rejected new_id write
    # would otherwise surface only as a vague rcraid0 timeout later).
    bound=0
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/class")" = "0x010802" ] || continue
        case "$(readlink "$d/driver" 2>/dev/null)" in
            */rcbottom) bound=$((bound + 1)) ;;
            *) echo "rcraid-test: WARNING: $(basename "$d") did not bind to rcbottom" ;;
        esac
    done
    [ "$bound" -ge 2 ] || fail "only $bound of $found NVMe functions bound to rcbottom"
}

echo "rcraid-test: loading rcraid.ko"
insmod /rcraid.ko enable_writes=1 allow_foreign_nvme=1 || fail "insmod rcraid.ko"

echo "rcraid-test: binding NVMe-class PCI functions to rcbottom"
bind_nvme_functions

echo "rcraid-test: waiting for /dev/rcraid0"
i=0
while [ ! -b /dev/rcraid0 ]; do
    i=$((i + 1))
    [ "$i" -le 100 ] || fail "/dev/rcraid0 did not appear within 10s"
    sleep 0.1
done

size=$(cat /sys/block/rcraid0/size)
echo "rcraid-test: /dev/rcraid0 up, $size sectors (expected $expected_sectors)"
[ "$size" = "$expected_sectors" ] || fail "capacity mismatch: $size != $expected_sectors"

# The volume must have assembled at the requested RAID level — the metadata
# images carry a DECOY generation for the OPPOSITE level (same DeviceIDs),
# so a parser that ignores the commit block matches the decoy and shows the
# wrong level here (and the wrong capacity above).  The parse log prints
# "(level=RAID1) ... (match)" for the record it accepted.
if [ -n "$expected_level" ]; then
    want_level=$(echo "$expected_level" | tr 'a-z' 'A-Z')
    if ! dmesg | grep "rc_volume_parse_logical_device" | grep "(match)" \
            | grep -q "level=$want_level"; then
        fail "volume did not assemble as $want_level (decoy generation matched?)"
    fi
    echo "rcraid-test: assembled level verified: $want_level"
fi

# Round-trip test: 4 MiB of /dev/urandom at three offsets — volume start,
# an unaligned mid-volume position (crosses stripe boundaries), and the
# last 4 MiB.  Read back after dropping caches so the data really comes
# from the driver, not the page cache.
dd if=/dev/urandom of=/pattern bs=1M count=4 2>/dev/null
want=$(md5sum /pattern | cut -d' ' -f1)
last_mib=$((size / 2048 - 4))     # sectors → MiB, minus the 4 we write
for seek_mib in 0 33 "$last_mib"; do
    echo "rcraid-test: write+readback at ${seek_mib} MiB"
    dd if=/pattern of=/dev/rcraid0 bs=1M seek="$seek_mib" conv=fsync 2>/dev/null \
        || fail "write at ${seek_mib} MiB"
    echo 3 > /proc/sys/vm/drop_caches
    got=$(dd if=/dev/rcraid0 bs=1M skip="$seek_mib" count=4 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$got" = "$want" ] || fail "readback mismatch at ${seek_mib} MiB"
done

# The three writes above must not have bled into each other or into the
# metadata: re-read region 2 once more, then confirm the module still
# validates its own metadata on a reload cycle.
echo 3 > /proc/sys/vm/drop_caches
got=$(dd if=/dev/rcraid0 bs=1M skip=33 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
[ "$got" = "$want" ] || fail "region at 33 MiB corrupted by later writes"

# Three cycles, not one: the reload path is where the sync-metadata-read
# vs ISR CQ race lived (the volume disk exists while the re-probed members
# run their sync reads), and a single cycle reproduced it only sometimes.
for cycle in 1 2 3; do
    echo "rcraid-test: reload cycle $cycle (metadata must still validate)"
    mark "reload cycle $cycle"
    rmmod rcraid || fail "rmmod (cycle $cycle)"
    insmod /rcraid.ko enable_writes=1 allow_foreign_nvme=1 \
        || fail "re-insmod (cycle $cycle)"
    bind_nvme_functions
    i=0
    while [ ! -b /dev/rcraid0 ]; do
        i=$((i + 1))
        [ "$i" -le 100 ] || fail "/dev/rcraid0 did not reappear after reload (cycle $cycle)"
        sleep 0.1
    done
    echo 3 > /proc/sys/vm/drop_caches
    got=$(dd if=/dev/rcraid0 bs=1M skip=33 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$got" = "$want" ] || fail "data mismatch after module reload (cycle $cycle)"
    # The reload must have come back via the commit-block path on every
    # member — a legacy-fallback assembly here means the sync metadata
    # reads raced the ISR (or the commit block failed to parse).
    if dmesg | grep -q "falling back to legacy BDF ordering"; then
        fail "a member fell back to legacy assembly during reload (cycle $cycle)"
    fi
done

# Discard an 8 MiB region, then write+readback into it — exercises the DSM
# path (RAID0 multi-member fan-out / RAID1 mirror fan-out) and proves the
# volume still serves I/O afterward.  Post-discard content is undefined per
# spec, so only the post-discard WRITE is content-checked.
echo "rcraid-test: discard + rewrite"
grep -E "rcraid|nvme" /proc/interrupts
mark "blkdiscard start"
blkdiscard -o 0 -l 8388608 /dev/rcraid0 || fail "blkdiscard"
mark "blkdiscard done, writing"
grep -E "rcraid|nvme" /proc/interrupts
dd if=/pattern of=/dev/rcraid0 bs=1M conv=fsync 2>/dev/null \
    || fail "write after discard"
mark "post-discard write done"
echo 3 > /proc/sys/vm/drop_caches
got=$(dd if=/dev/rcraid0 bs=1M count=4 2>/dev/null | md5sum | cut -d' ' -f1)
[ "$got" = "$want" ] || fail "readback mismatch after discard"

# Sub-page buffered-write test: real mke2fs metadata writeback goes
# through buffer heads smaller than a page, producing bios whose bvecs
# the driver must MERGE before building PRPs (non-first segments at
# in-page offsets are unexpressible — the controller rejects them with
# SC 0x13 "PRP Offset Invalid").  This is the exact failure that killed
# the OS install on real hardware while every dd-style test passed.
# -b 1024 forces 1 KiB blocks for the worst-case pattern.
MKE2FS=""
for p in /usr/sbin/mke2fs /sbin/mke2fs; do
    [ -x "$p" ] && MKE2FS="$p" && break
done
if [ -n "$MKE2FS" ]; then
    echo "rcraid-test: mke2fs sub-page buffered-write test"
    mark "mke2fs start"
    errs_before=$(dmesg | grep -c "I/O error")
    "$MKE2FS" -q -b 1024 /dev/rcraid0 || fail "mke2fs on /dev/rcraid0"
    sync
    errs_after=$(dmesg | grep -c "I/O error")
    [ "$errs_before" = "$errs_after" ] \
        || fail "I/O errors during mke2fs (PRP/segment-merge regression?)"
    echo 3 > /proc/sys/vm/drop_caches
    # ext2 superblock magic 0xEF53 lives at byte offset 1080.
    magic=$(dd if=/dev/rcraid0 bs=1 skip=1080 count=2 2>/dev/null \
            | od -An -tx1 | tr -d ' \n')
    [ "$magic" = "53ef" ] \
        || fail "ext2 superblock magic wrong after mke2fs (got '$magic')"
    echo "rcraid-test: mke2fs OK, superblock verified on the volume"
else
    echo "rcraid-test: mke2fs not bundled — skipping sub-page write test"
fi

# ---------------------------------------------------------------------------
# RAID1 degraded-mode scenario (fail_member=1): kill one mirror member via
# sysfs unbind — the same rc_bottom_remove → rc_volume_remove_member path a
# surprise hot-unplug takes — and prove the volume keeps serving:
#   1. data written BEFORE the failure is still readable (from the survivor),
#   2. writes AFTER the failure succeed (degraded write to the survivor only),
#   3. debugfs reports the volume degraded with the member absent.
# The host runner skips its mirror-identity check in this mode — post-failure
# writes legitimately reach only the survivor.
# ---------------------------------------------------------------------------
if [ "$fail_member" = "1" ] && [ "$expected_level" = "raid1" ]; then
    echo "rcraid-test: DEGRADED SCENARIO: pre-failure write"
    mark "degraded scenario start"
    dd if=/dev/urandom of=/pattern2 bs=1M count=4 2>/dev/null
    want2=$(md5sum /pattern2 | cut -d' ' -f1)
    dd if=/pattern2 of=/dev/rcraid0 bs=1M seek=50 conv=fsync 2>/dev/null \
        || fail "pre-failure write at 50 MiB"

    # Unbind the LAST rcbottom-bound NVMe function (RAID1 is symmetric —
    # either member works).
    victim=""
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/class")" = "0x010802" ] || continue
        case "$(readlink "$d/driver" 2>/dev/null)" in
            */rcbottom) victim="${d##*/}" ;;
        esac
    done
    [ -n "$victim" ] || fail "no rcbottom-bound member found to unbind"
    echo "rcraid-test: unbinding member $victim (simulated hot-unplug)"
    mark "unbinding $victim"
    echo "$victim" > "/sys/bus/pci/drivers/rcbottom/unbind" \
        || fail "unbind of $victim"

    [ -b /dev/rcraid0 ] \
        || fail "/dev/rcraid0 disappeared after single-member failure"

    echo 3 > /proc/sys/vm/drop_caches
    got2=$(dd if=/dev/rcraid0 bs=1M skip=50 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$got2" = "$want2" ] \
        || fail "pre-failure data unreadable from survivor (degraded read broken)"
    echo "rcraid-test: degraded READ ok (pre-failure data served by survivor)"

    mark "degraded write"
    dd if=/pattern2 of=/dev/rcraid0 bs=1M seek=60 conv=fsync 2>/dev/null \
        || fail "degraded write at 60 MiB failed"
    echo 3 > /proc/sys/vm/drop_caches
    got2=$(dd if=/dev/rcraid0 bs=1M skip=60 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$got2" = "$want2" ] || fail "degraded write readback mismatch"
    echo "rcraid-test: degraded WRITE ok"

    mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
    if [ -r /sys/kernel/debug/rcraid/volume ]; then
        echo "rcraid-test: --- debugfs volume state ---"
        sed 's/^/rcraid-test:   /' /sys/kernel/debug/rcraid/volume
        grep -q "state: degraded" /sys/kernel/debug/rcraid/volume \
            || fail "debugfs does not report state: degraded"
        grep -q "absent" /sys/kernel/debug/rcraid/volume \
            || fail "debugfs does not show the removed member as absent"
    else
        fail "debugfs rcraid/volume not readable"
    fi
    echo "rcraid-test: degraded scenario complete"
fi

echo "RCRAID-TEST-PASS"
poweroff -f
