#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# dracut module for rcraid — bundles everything needed to bring the AMD-RAID
# array up inside the initramfs so the rootfs on /dev/rcraid0pN is mountable
# at pivot_root time.
#
# Components copied into the initramfs:
#   - rcraid.ko             (the kernel module itself)
#   - /usr/sbin/rcraid-bind (helper that does the nvme→rcbottom rebind)
#   - 50-rcraid.rules       (udev rule that invokes the helper)
#   - rcraid.conf           (modprobe.d drop-in with enable_writes + filter)
#
# Numerical prefix 95 puts us after the generic block-device modules but
# before 99base finishes setup — same slot multipath/lvm use.

check() {
    [ -e /etc/udev/rules.d/50-rcraid.rules ] || return 1
    [ -x /usr/sbin/rcraid-bind ]             || return 1
    return 0
}

depends() {
    echo "udev-rules"
    return 0
}

installkernel() {
    instmods rcraid
}

install() {
    inst_simple /usr/sbin/rcraid-bind                /usr/sbin/rcraid-bind
    inst_simple /etc/udev/rules.d/50-rcraid.rules    /etc/udev/rules.d/50-rcraid.rules
    inst_simple /etc/modprobe.d/rcraid.conf          /etc/modprobe.d/rcraid.conf
}
