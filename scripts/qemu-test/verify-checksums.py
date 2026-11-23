#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify RAIDCore checksums against raw member dumps.

A dump starts at LBA 0x5000 of a member (the layout
`PlaidPiper/x399-raidcore-fixtures` ships) and must hold at least two
sectors: sector 0 is the `RC_MetaData` block, sector 1 the
`RC_ConfigCommit` block.  Both store their checksum in the first 8 bytes
and cover `[0x08, 0x200)`.

This checks that the algorithm the driver and `mkmeta.py` share
(`rc_raidcore_checksum()` in `src/rc_nvme.c`) reproduces values the
**vendor's firmware wrote**.  A metadata writer is only trustworthy once
that holds, because the QEMU rig has no firmware to disagree with it.

Usage:
    scripts/qemu-test/verify-checksums.py <dump> [<dump> ...]

Exit status is 0 only when every block on every dump matches.

Original work; the fixture data is third-party and stays out of this tree.
See docs/RE_METHODOLOGY.md.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkmeta import rc_checksum  # noqa: E402  (path set up just above)

SECTOR = 512
MD_MAGIC = 0x65726F4344494152  # "RAIDCore"
MD_VERSION = 0x00030000
CHECKSUM_AT = 0x00
PAYLOAD_AT = 0x08
PAYLOAD_END = 0x200


def check_block(label, block):
    """Compare a block's stored checksum with the recomputed one."""
    stored = struct.unpack_from("<Q", block, CHECKSUM_AT)[0]
    calc = rc_checksum(block[PAYLOAD_AT:PAYLOAD_END])
    ok = stored == calc
    print(f"    {label:9} stored 0x{stored:016x} computed 0x{calc:016x}"
          f"  {'MATCH' if ok else 'MISMATCH'}")
    return ok


def check_dump(path):
    """Verify both checksummed blocks in one dump; True when both match."""
    with open(path, "rb") as f:
        data = f.read(2 * SECTOR)
    if len(data) < 2 * SECTOR:
        print(f"{path}: too short ({len(data)} bytes, need {2 * SECTOR})",
              file=sys.stderr)
        return False

    md, commit = data[:SECTOR], data[SECTOR:2 * SECTOR]
    magic = struct.unpack_from("<Q", md, 0x08)[0]
    version = struct.unpack_from("<I", md, 0x2C)[0]
    device_id = struct.unpack_from("<Q", md, 0x10)[0]
    gen_lba, gen_len = struct.unpack_from("<II", commit, 0x08)
    gen_ts = struct.unpack_from("<Q", commit, 0x10)[0]

    print(f"{path}: magic {magic:#018x}, version {version:#010x}, "
          f"device_id {device_id:#018x}")
    print(f"    commit    active generation @LBA 0x{gen_lba:x} "
          f"len {gen_len} B, ts 0x{gen_ts:016x}")

    ok = True
    if magic != MD_MAGIC or version != MD_VERSION:
        print("    header    unexpected magic/version (not a RAIDCore "
              "0x00030000 block)")
        ok = False
    ok &= check_block("metadata", md)
    ok &= check_block("commit", commit)
    return ok


def main(argv):
    if len(argv) < 2:
        print("verify RAIDCore metadata + commit checksums on raw member dumps",
              file=sys.stderr)
        print(f"usage: {argv[0]} <dump> [<dump> ...]", file=sys.stderr)
        return 2

    failures = sum(1 for path in argv[1:] if not check_dump(path))
    print(f"\n{len(argv) - 1 - failures}/{len(argv) - 1} dump(s) verified")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
