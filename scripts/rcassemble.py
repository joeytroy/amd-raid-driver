#!/usr/bin/env python3
"""Assemble an AMD RAIDCore array READ-ONLY in userspace via device-mapper.

Parses the on-disk RAIDCore metadata (the same layout rc_nvme.c validates;
rc_linux.h is the authoritative spec, scripts/qemu-test/mkmeta.py the
reference encoder) from each member block device or image file, derives the
array geometry, and either prints it, prints the dm table, or applies it
with dmsetup.

This tool never writes to the members and always creates the dm device
read-only.  It exists for cases the kernel driver deliberately does not
cover:

  - SATA members (the rcraid bottom driver binds NVMe-class PCI functions
    only; SATA members sit behind stock ahci — issue #57's X399 RAID10),
  - data recovery on machines without the driver installed,
  - RAID10, which the driver has no dispatch for.

Validation performed per member (mirroring the driver):
  - RC_MetaData at LBA 0x5000: magic, version, XOR-lane-shuffle checksum;
  - commit block at CommitLBA naming the ACTIVE config generation;
  - generation-header timestamp must match the commit block (this is what
    skips dead/deleted configs earlier in the journal ring);
  - raw single-disk LDs (DeviceType 0x1BF9) are skipped, never assembled;
  - the volume LD (0x1BF6) must be byte-identical across members, every
    member's DeviceID must appear in the element array, and no position
    may be missing.

Levels (from FirstCount x SecondCount): raid0 (n x 1), raid1 (1 x 2),
raid10 (n/2 x 2, elements pair-major: position // SecondCount = stripe
column, position % SecondCount = mirror leg).

Usage:
  rcassemble.py --parse-only  /dev/sdb /dev/sdc /dev/sdd /dev/sde
  rcassemble.py --dry-run     member0.img member1.img
  sudo rcassemble.py --name rcvol /dev/sdb /dev/sdc     # creates /dev/mapper/rcvol (ro)

Member order on the command line does not matter — positions come from the
metadata.  For mirrored levels the FIRST healthy leg listed for a pair is
used; pass only the legs you trust if one is stale.
"""

import argparse
import os
import stat
import struct
import subprocess
import sys

SECTOR = 512
RAIDCORE_LBA = 0x5000
RAIDCORE_MAGIC = 0x65726F4344494152  # "RAIDCore" LE
RAIDCORE_VERSION = 0x00030000

DST_LOGICAL_DEVICE = 0x25BD
DEVTYPE_VOLUME = 0x1BF6
DEVTYPE_SINGLE = 0x1BF9

# RC_LogicalDevice field offsets (rc_linux.h RC_LD_*_OFFSET)
LD_ELEMENTOFFSET = 0x04
LD_DEVICETYPE = 0x0C
LD_CAPACITY = 0x50
LD_DEVICES = 0x68
LD_FIRSTCOUNT = 0x6C
LD_SECONDCOUNT = 0x70
LD_PACKETSIZE = 0x90
LD_CHUNKINDEX = 0x110

# RC_LogicalElement_LE (64 bytes each)
LE_BYTES = 64
LE_DEVICEID = 0x00
LE_USERDATA_OFFSET = 0x20
LE_USERDATA_SIZE = 0x28

# Commit-block field offsets
COMMIT_GEN_LBA = 0x08
COMMIT_GEN_LEN = 0x0C
COMMIT_GEN_TS = 0x10

GEN_HEADER_BYTES = 0x200

# chunk_index encoding (rc_volume_chunk_sectors_for): only 2 and 3 are
# explicit, everything else means 128 sectors.
CHUNK_INDEX_SECTORS = {3: 512, 2: 256}


def rc_checksum(payload: bytes) -> int:
    """rc_raidcore_checksum(): XOR of 64-bit LE words after swapping two
    16-bit lanes chosen by (acc & 3, word & 3), falling back to
    (i & 3, (i+1) & 3) when they collide.  Must match mkmeta.py."""
    acc = 0
    for i in range(len(payload) // 8):
        w = int.from_bytes(payload[i * 8:(i + 1) * 8], "little")
        lane_a = acc & 3
        lane_b = w & 3
        if lane_a == lane_b:
            lane_a = i & 3
            lane_b = (i + 1) & 3
        lanes = [(w >> (k * 16)) & 0xFFFF for k in range(4)]
        lanes[lane_a], lanes[lane_b] = lanes[lane_b], lanes[lane_a]
        w = sum(lanes[k] << (k * 16) for k in range(4))
        acc ^= w
    return acc


class Member:
    def __init__(self, path, device_id, ld_blob, elements):
        self.path = path
        self.device_id = device_id
        self.ld_blob = ld_blob          # raw volume-LD record bytes
        self.elements = elements        # [(device_id, ud_offset, ud_size)]
        self.position = None            # filled in during cross-check


def read_at(f, lba, nbytes):
    f.seek(lba * SECTOR)
    data = f.read(nbytes)
    if len(data) != nbytes:
        raise ValueError(f"short read at LBA {lba:#x}")
    return data


def parse_member(path):
    """Validate one member's metadata chain and return a Member with the
    committed volume-LD record.  Raises ValueError with a specific message
    on any validation failure."""
    with open(path, "rb") as f:
        md = read_at(f, RAIDCORE_LBA, SECTOR)
        magic = struct.unpack_from("<Q", md, 0x08)[0]
        if magic != RAIDCORE_MAGIC:
            raise ValueError("no RAIDCore magic at LBA 0x5000")
        version = struct.unpack_from("<I", md, 0x2C)[0]
        if version != RAIDCORE_VERSION:
            raise ValueError(f"unsupported metadata version {version:#x}")
        want_csum = struct.unpack_from("<Q", md, 0x00)[0]
        if rc_checksum(md[0x08:0x200]) != want_csum:
            raise ValueError("RC_MetaData checksum mismatch")
        device_id = struct.unpack_from("<Q", md, 0x10)[0]
        commit_lba = struct.unpack_from("<Q", md, 0x18)[0]
        ring_lba = struct.unpack_from("<Q", md, 0x20)[0]
        ring_size = struct.unpack_from("<I", md, 0x28)[0]

        commit = read_at(f, commit_lba, SECTOR)
        gen_lba = struct.unpack_from("<I", commit, COMMIT_GEN_LBA)[0]
        gen_len = struct.unpack_from("<I", commit, COMMIT_GEN_LEN)[0]
        gen_ts = struct.unpack_from("<Q", commit, COMMIT_GEN_TS)[0]
        if gen_len == 0 or gen_len % SECTOR:
            raise ValueError(f"committed generation length {gen_len} "
                             "is zero or not sector-aligned")
        if not ring_lba <= gen_lba < ring_lba + ring_size:
            raise ValueError(f"committed generation LBA {gen_lba:#x} "
                             "outside the config ring")

        gen = read_at(f, gen_lba, gen_len)
        # The header timestamp must match the commit block — this is the
        # linkage that skips dead generations left in the journal by
        # deleted arrays (the decoy-generation pitfall).
        hdr_ts = struct.unpack_from("<Q", gen, 0x00)[0]
        if hdr_ts != gen_ts:
            raise ValueError(
                f"generation timestamp {hdr_ts:#x} does not match commit "
                f"block {gen_ts:#x} (dead/torn generation?)")

    ld_blob = None
    off = GEN_HEADER_BYTES
    while off + 4 <= len(gen):
        dst = struct.unpack_from("<I", gen, off)[0]
        if dst != DST_LOGICAL_DEVICE:
            break
        pkt = struct.unpack_from("<I", gen, off + LD_PACKETSIZE)[0]
        if pkt < LD_CHUNKINDEX + 4 or off + pkt > len(gen):
            raise ValueError(f"LD record at +{off:#x} has bad "
                             f"PacketSize {pkt}")
        devtype = struct.unpack_from("<I", gen, off + LD_DEVICETYPE)[0]
        if devtype == DEVTYPE_VOLUME:
            if ld_blob is not None:
                raise ValueError("multiple volume LD records in the "
                                 "committed generation")
            ld_blob = bytes(gen[off:off + pkt])
        elif devtype != DEVTYPE_SINGLE:
            raise ValueError(f"unknown LD DeviceType {devtype:#x}")
        # DEVTYPE_SINGLE (raw disk record): skipped, never assembled.
        off += pkt
    if ld_blob is None:
        raise ValueError("no volume LD (0x1BF6) in committed generation")

    elem_off = struct.unpack_from("<I", ld_blob, LD_ELEMENTOFFSET)[0]
    devices = struct.unpack_from("<I", ld_blob, LD_DEVICES)[0]
    if elem_off + devices * LE_BYTES > len(ld_blob):
        raise ValueError("element array overruns LD record")
    elements = []
    for i in range(devices):
        e = elem_off + i * LE_BYTES
        elements.append((
            struct.unpack_from("<Q", ld_blob, e + LE_DEVICEID)[0],
            struct.unpack_from("<Q", ld_blob, e + LE_USERDATA_OFFSET)[0],
            struct.unpack_from("<Q", ld_blob, e + LE_USERDATA_SIZE)[0],
        ))
    return Member(path, device_id, ld_blob, elements)


def derive_geometry(members):
    """Cross-check members and derive (level, capacity, chunk_sectors,
    positions) where positions maps element index -> Member or None."""
    ref = members[0]
    for m in members[1:]:
        if m.ld_blob != ref.ld_blob:
            raise ValueError(f"{m.path}: committed volume LD differs from "
                             f"{ref.path} — mixed generations?")

    ld = ref.ld_blob
    devices = struct.unpack_from("<I", ld, LD_DEVICES)[0]
    first = struct.unpack_from("<I", ld, LD_FIRSTCOUNT)[0]
    second = struct.unpack_from("<I", ld, LD_SECONDCOUNT)[0]
    capacity = struct.unpack_from("<Q", ld, LD_CAPACITY)[0]
    chunk_index = struct.unpack_from("<I", ld, LD_CHUNKINDEX)[0]
    chunk_sectors = CHUNK_INDEX_SECTORS.get(chunk_index, 128)

    if first * second != devices:
        raise ValueError(f"FirstCount {first} x SecondCount {second} != "
                         f"devices {devices}")
    if second == 1:
        level = "raid0"
    elif second == 2 and first == 1:
        level = "raid1"
    elif second == 2:
        level = "raid10"
    else:
        raise ValueError(f"unsupported geometry {first}x{second}")

    positions = [None] * devices
    for m in members:
        for i, (did, _, _) in enumerate(ref.elements):
            if did == m.device_id:
                if positions[i] is not None:
                    raise ValueError(f"{m.path}: duplicate DeviceID "
                                     f"{did:#x} at position {i}")
                positions[i] = m
                m.position = i
                break
        else:
            raise ValueError(f"{m.path}: DeviceID {m.device_id:#x} not in "
                             "the volume's element array — wrong array?")
    return level, capacity, chunk_sectors, positions


def pick_legs(level, second, positions, elements):
    """For mirrored levels, pick one present leg per stripe column.
    Returns [(Member, ud_offset)] per column, or raises if a column has
    no present leg."""
    columns = []
    ncols = len(positions) // second
    for col in range(ncols):
        leg = None
        for j in range(second):
            i = col * second + j   # pair-major element order
            if positions[i] is not None:
                leg = (positions[i], elements[i][1])
                break
        if leg is None:
            raise ValueError(f"stripe column {col}: no member present "
                             "(need at least one leg per mirror pair)")
        columns.append(leg)
    return columns


def dm_table(level, capacity, chunk_sectors, positions, elements):
    if level == "raid0":
        missing = [i for i, m in enumerate(positions) if m is None]
        if missing:
            raise ValueError(f"raid0 needs every member; missing "
                             f"positions {missing}")
        devs = [(positions[i], elements[i][1])
                for i in range(len(positions))]
        legs = " ".join(f"{m.path} {off}" for m, off in devs)
        return (f"0 {capacity} striped {len(devs)} {chunk_sectors} {legs}",
                [m for m, _ in devs])
    if level == "raid1":
        (m, off), = pick_legs(level, 2, positions, elements)
        return f"0 {capacity} linear {m.path} {off}", [m]
    # raid10: stripe across one leg per pair
    cols = pick_legs(level, 2, positions, elements)
    legs = " ".join(f"{m.path} {off}" for m, off in cols)
    return (f"0 {capacity} striped {len(cols)} {chunk_sectors} {legs}",
            [m for m, _ in cols])


def check_not_in_use(path):
    st = os.stat(path)
    if not stat.S_ISBLK(st.st_mode):
        return  # image file — loop-mount checks don't apply
    name = os.path.basename(os.path.realpath(path))
    holders = f"/sys/class/block/{name}/holders"
    if os.path.isdir(holders) and os.listdir(holders):
        raise ValueError(f"{path} is held by {os.listdir(holders)} — "
                         "refusing to assemble over an in-use member")
    with open("/proc/mounts") as f:
        if any(line.split()[0] == path for line in f):
            raise ValueError(f"{path} is mounted — refusing")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[2:]))
    ap.add_argument("--parse-only", action="store_true",
                    help="print derived geometry per member and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the dmsetup table instead of applying it")
    ap.add_argument("--name", default="rcvol",
                    help="dm device name (default rcvol)")
    ap.add_argument("members", nargs="+",
                    help="member block devices or image files")
    args = ap.parse_args()

    members = []
    for path in args.members:
        try:
            m = parse_member(path)
        except (OSError, ValueError) as e:
            sys.exit(f"rcassemble: {path}: {e}")
        members.append(m)

    try:
        level, capacity, chunk_sectors, positions = derive_geometry(members)
    except ValueError as e:
        sys.exit(f"rcassemble: {e}")

    ref = members[0]
    print(f"level={level} devices={len(ref.elements)} "
          f"capacity_sectors={capacity} chunk_sectors={chunk_sectors}")
    for i, (did, off, size) in enumerate(ref.elements):
        m = positions[i]
        print(f"position {i}: device_id={did:#x} userdata_offset={off} "
              f"userdata_size={size} -> {m.path if m else 'ABSENT'}")
    if args.parse_only:
        return

    try:
        table, used = dm_table(level, capacity, chunk_sectors,
                               positions, ref.elements)
    except ValueError as e:
        sys.exit(f"rcassemble: {e}")

    if args.dry_run:
        print(f"dmsetup create {args.name} --readonly --table '{table}'")
        return

    for m in used:
        try:
            check_not_in_use(m.path)
        except ValueError as e:
            sys.exit(f"rcassemble: {e}")
    try:
        subprocess.run(["dmsetup", "create", args.name, "--readonly",
                        "--table", table], check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"rcassemble: dmsetup failed: {e}")
    print(f"created /dev/mapper/{args.name} (read-only)")


if __name__ == "__main__":
    main()
