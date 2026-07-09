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
    echo "rcraid-test: --- last dmesg lines ---"
    dmesg | tail -n 40
    echo "RCRAID-TEST-FAIL"
    poweroff -f
    # Backstop: if poweroff somehow doesn't halt PID 1, do NOT return to
    # the caller — falling through could reach the PASS marker and report
    # a false pass to the host runner.
    exit 1
}

expected_sectors=""
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        expected_sectors=*) expected_sectors="${arg#expected_sectors=}" ;;
    esac
done
[ -n "$expected_sectors" ] || fail "no expected_sectors= on kernel cmdline"

# Bind every NVMe-class PCI function to rcbottom.  new_id entries die
# with the module, so the reload cycle below calls this again.
bind_nvme_functions() {
    found=0
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/class")" = "0x010802" ] || continue
        v=$(cat "$d/vendor"); dev=$(cat "$d/device")
        # Duplicate IDs EEXIST on the second write; that's fine — actual
        # bind success is verified by the driver-symlink check below.
        echo "${v#0x} ${dev#0x}" > /sys/bus/pci/drivers/rcbottom/new_id 2>/dev/null
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

echo "rcraid-test: reload cycle (metadata must still validate)"
rmmod rcraid || fail "rmmod"
insmod /rcraid.ko enable_writes=1 allow_foreign_nvme=1 || fail "re-insmod"
bind_nvme_functions
i=0
while [ ! -b /dev/rcraid0 ]; do
    i=$((i + 1))
    [ "$i" -le 100 ] || fail "/dev/rcraid0 did not reappear after reload"
    sleep 0.1
done
echo 3 > /proc/sys/vm/drop_caches
got=$(dd if=/dev/rcraid0 bs=1M skip=33 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
[ "$got" = "$want" ] || fail "data mismatch after module reload"

echo "RCRAID-TEST-PASS"
poweroff -f
