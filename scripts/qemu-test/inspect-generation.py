#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Walk the committed config generation in a raw RAIDCore member dump.

A dump starts at LBA 0x5000, the layout
`PlaidPiper/x399-raidcore-fixtures` ships and what

    dd if=/dev/nvmeXn1 of=member.meta.bin bs=512 skip=20480 count=4096

produces: sector 0 is `RC_MetaData`, sector 1 `RC_ConfigCommit`.  The commit
names the active generation; this walks it record by record.

    0x25BC  PhysicalDevice    128 B, controller/version record first
    0x25BD  LogicalDevice     variable, u32 size at +0x90
    0x25BE  ControllerDevice  0x88 B
    0x25BF  SEPDevice         0x40 B

The generation has no checksum of its own: +0x00 is its timestamp, which the
commit block's +0x10 must repeat.  Vendor-written bytes were first read this
way on 2026-11-13 (`docs/REVERSE_ENGINEERING.md`, "On-disk generation layout"),
and that map is what a metadata writer has to reproduce.

Usage:
    scripts/qemu-test/inspect-generation.py <dump> [<dump> ...]

Exit status: 0 when every dump is structurally sound and both checksums match,
1 otherwise.

Original work; the reference data is third-party and stays out of this tree.
See docs/RE_METHODOLOGY.md.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkmeta import rc_checksum  # noqa: E402  (path set up just above)

SECTOR = 512
BASE = 0x5000
TAG_PD, TAG_LD, TAG_CTRL, TAG_SEP = 0x25BC, 0x25BD, 0x25BE, 0x25BF
STRIDE_PD, STRIDE_CTRL, STRIDE_SEP = 0x80, 0x88, 0x40
CHECKSUM_AT, PAYLOAD_AT, PAYLOAD_END = 0x00, 0x08, 0x200


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def check_block(label, block):
    stored = u64(block, CHECKSUM_AT)
    calc = rc_checksum(block[PAYLOAD_AT:PAYLOAD_END])
    ok = stored == calc
    print(f"    {label:9} stored 0x{stored:016x} computed 0x{calc:016x}"
          f"  {'MATCH' if ok else 'MISMATCH'}")
    return ok


def show_pd(tag_off, rec):
    print(f"      PD   dev=0x{u64(rec, 0x04):016x} type=0x{u32(rec, 0x24):04x}"
          f" cap={u64(rec, 0x1C)} state=0x{u32(rec, 0x2C):x}"
          f" devices={u32(rec, 0x18)} features=0x{u32(rec, 0x54):x}"
          f" port={u32(rec, 0x58)} prev=0x{u32(rec, 0x14):x}")


def show_ld(tag_off, rec):
    elems = u32(rec, 0x04)
    count = u32(rec, 0x68)
    print(f"      LD   devtype=0x{u32(rec, 0x0C):04x}"
          f" cap={u64(rec, 0x50)} devices={count}"
          f" {u32(rec, 0x6C)}x{u32(rec, 0x70)}"
          f" pktsize=0x{u32(rec, 0x90):x} chunk_idx={u32(rec, 0x110)}")
    for i in range(count):
        off = elems + i * 64
        if off + 64 > len(rec):
            print(f"        element {i} runs past the record")
            break
        print(f"        element {i}: dev=0x{u64(rec, off):016x}"
              f" alloc_off={u64(rec, off + 0x10)}"
              f" alloc_sz={u64(rec, off + 0x18)}"
              f" user_off={u64(rec, off + 0x20)}"
              f" user_sz={u64(rec, off + 0x28)}")


def show_ctrl(rec):
    name = rec[0x24:0x34].split(b"\0")[0].decode("latin1", "replace")
    print(f"      CTRL id=0x{u64(rec, 0x04):016x} name={name!r}"
          f" feature=0x{u16(rec, 0x34):04x} ports={u32(rec, 0x68)}"
          f" {u16(rec, 0x6C):04x}:{u16(rec, 0x6E):04x}"
          f" num={u32(rec, 0x70)}"
          f" sub={u16(rec, 0x74):04x}:{u16(rec, 0x76):04x}"
          f" pci=0x{u32(rec, 0x84):06x}")


def walk_generation(gen):
    """Decode every record; returns False on an unknown tag or bad size."""
    off = 0x200
    ok = True
    while off + 4 <= len(gen):
        tag = u32(gen, off)
        if tag == 0:
            break
        if tag == TAG_PD:
            if off + STRIDE_PD > len(gen):
                print(f"      PD at +0x{off:x} runs past the generation")
                return False
            show_pd(off, gen[off:off + STRIDE_PD])
            off += STRIDE_PD
        elif tag == TAG_LD:
            size = u32(gen, off + 0x90)
            if size < 0x130 or off + size > len(gen):
                print(f"      LD at +0x{off:x} has unusable size 0x{size:x}")
                return False
            show_ld(off, gen[off:off + size])
            off += size
        elif tag == TAG_CTRL:
            if off + STRIDE_CTRL > len(gen):
                print(f"      CTRL at +0x{off:x} runs past the generation")
                return False
            show_ctrl(gen[off:off + STRIDE_CTRL])
            off += STRIDE_CTRL
        elif tag == TAG_SEP:
            if off + STRIDE_SEP > len(gen):
                print(f"      SEP at +0x{off:x} runs past the generation")
                return False
            print(f"      SEP  id=0x{u64(gen, off + 0x04):016x}")
            off += STRIDE_SEP
        else:
            print(f"      unknown tag 0x{tag:04x} at +0x{off:x}")
            ok = False
            break

    if off + 4 <= len(gen):
        tail = gen[off:]
        if any(tail):
            print(f"      non-zero bytes after the last record "
                  f"(+0x{off:x}..+0x{len(gen):x})")
            ok = False
        else:
            print(f"      padding: {len(tail)} zero bytes after +0x{off:x}")
    return ok


def inspect(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 2 * SECTOR:
        print(f"{path}: too short ({len(data)} bytes, need 2 sectors)")
        return False

    md, commit = data[:SECTOR], data[SECTOR:2 * SECTOR]
    magic = u64(md, 0x08)
    device_id = u64(md, 0x10)
    gen_lba, gen_len = struct.unpack_from("<II", commit, 0x08)
    gen_ts = u64(commit, 0x10)

    print(f"{path}: device_id 0x{device_id:016x} magic 0x{magic:016x}")
    off = (gen_lba - BASE) * SECTOR
    if magic != 0x65726F4344494152 or off < 0 or off + gen_len > len(data):
        print(f"    not a usable dump (magic 0x{magic:016x}, generation "
              f"@LBA 0x{gen_lba:x} len {gen_len} B)")
        return False

    ok = check_block("metadata", md)
    ok &= check_block("commit", commit)

    gen = data[off:off + gen_len]
    linked = u64(gen, 0x00) == gen_ts
    print(f"    generation @LBA 0x{gen_lba:x} len {gen_len} B "
          f"ts 0x{gen_ts:016x} (commit link "
          f"{'OK' if linked else 'MISMATCH'})")
    ok &= linked
    ok &= walk_generation(gen)
    return bool(ok)


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    ok = True
    for path in argv[1:]:
        ok &= inspect(path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
