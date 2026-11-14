#!/usr/bin/env python3
"""Write synthetic AMD RAIDCore metadata onto blank disk images.

Produces what rc_nvme.c's parser validates (see rc_linux.h for the
authoritative layout), mimicking the structures REAL firmware writes —
verified against TRX50 hardware dumps (2026-07-10):

  - per-member RC_MetaData block at LBA 0x5000: magic "RAIDCore",
    version 0x00030000, and the XOR-with-lane-shuffle checksum over
    bytes [0x08..0x1FF] (ported from rcraid.sys FUN_1400014ec, same
    algorithm as rc_raidcore_checksum() in rc_nvme.c);
  - a config-commit block at LBA 0x5001 naming the ACTIVE config
    generation (start LBA + byte length + the generation's timestamp);
  - the config ring at LBA 0x5800 as a journal of config GENERATIONS,
    each one header sector (timestamp at +0x00) followed by packed
    records at +0x200.

Two generations are written, reproducing the failure observed on real
hardware (a deleted array's records persisting in the journal ahead of
the live config):

  - a DECOY generation at the ring head: an LD record for the OPPOSITE
    RAID level with the same member DeviceIDs but the wrong capacity.
    It is NOT referenced by the commit block.  A parser that scans the
    ring from the start and takes the first DeviceID match (the exact
    bug that shipped) assembles this dead config and fails the test's
    capacity/level checks.
  - the ACTIVE generation 16 sectors in, pointed at by the commit
    block, holding a raw single-disk LD (0x1BF9, member 0's own
    DeviceID — must be skipped, not assembled) followed by the real
    array LD record.

RAID level is encoded the way firmware does it: DeviceType 0x1BF6 for
both levels, with FirstCount x SecondCount = stripe width x mirror count
(RAID0: n x 1, RAID1: 1 x 2).  The driver derives the level from the
counts.

The record set matches the order firmware writes — PhysicalDevice
records, the array LD, the raw-disk LD, then ControllerDevice records —
but the values inside those records are this tool's synthetic ones, so
images made here are for driver testing, not a firmware-created array.

Targets may be regular image files or real block devices (/dev/nvmeXn1);
the latter lets the same metadata drive a bare-metal driver test.  Writing
to a block device overwrites LBAs 0x5000..0x6000 (magic, commit block and
config ring) and nothing else — it does NOT create a firmware array, so
RAIDXpert/Windows will not accept the result.

Usage:
  mkmeta.py --level raid0 --chunk-index 3 member0.img member1.img
  mkmeta.py --level raid1 member0.img member1.img
  mkmeta.py --level raid0 --chunk-index 3 /dev/nvme1n1 /dev/nvme2n1

Prints "capacity_sectors=<N>" on success — the expected size of the
assembled volume, for the guest test to verify against.
"""

import argparse
import fcntl
import os
import stat
import struct
import sys

SECTOR = 512
# _IOR(0x12, 114, size_t) — asks a block device how many bytes it holds.
# fstat() reports st_size == 0 for block devices, so os.path.getsize()
# cannot size them.
BLKGETSIZE64 = 0x80081272
RAIDCORE_LBA = 0x5000
CONFIG_COMMIT_LBA = 0x5001
CONFIG_RING_LBA = 0x5800
RAIDCORE_MAGIC = 0x65726F4344494152  # "RAIDCore" LE
RAIDCORE_VERSION = 0x00030000
FEATURES = 0x1C          # value observed on real firmware-created arrays
CONFIG_RING_SIZE = 2048  # RC_MetaData.ConfigRingSize, sectors

DST_LOGICAL_DEVICE = 0x25BD
# Firmware writes 0x1BF6 for RAID0 AND RAID1 (level lives in the counts).
DEVTYPE_VOLUME = 0x1BF6
# Per-disk "raw disk" LD firmware publishes for non-array disks (1x1
# counts, element array = the disk's own DeviceID).  The driver must never
# assemble one of these as a volume.
DEVTYPE_SINGLE = 0x1BF9
LEVELS = ("raid0", "raid1", "raid5", "raid10")

# RC_PhysicalDeviceOnDisk (tag 0x25BC) — 0x80 bytes.
PD_TAG = 0x25BC
PD_BYTES = 0x80
PD_DEVICE_ID = 0x04
PD_CHANNEL = 0x0C
PD_PORT = 0x0E
PD_CAPACITY = 0x1C
PD_DEVICE_TYPE = 0x24
PD_STATE = 0x2C
PD_TYPE_CONTROLLER = 0x1201
PD_TYPE_DISK = 0x1200
PD_STATE_ONLINE = 0x3000
PD_CONTROLLER_ID = 0x0000102285900000

# RC_ControllerDevice (tag 0x25BE) — 0x88 bytes.
CTRL_TAG = 0x25BE
CTRL_BYTES = 0x88
CTRL_ID = 0x04
CTRL_HANDLE = 0x0C
CTRL_NAME = 0x24
CTRL_FEATURE = 0x34
CTRL_PORTS = 0x68
CTRL_VID = 0x6C
CTRL_DID = 0x6E
CTRL_NUMBER = 0x70
CTRL_SUBVID = 0x74
CTRL_SUBDID = 0x76
CTRL_REGION_ID = 0x78
CTRL_ORIGVID = 0x80
CTRL_ORIGDID = 0x82
CTRL_PCILOC = 0x84
CTRL_FEATURE_VALUE = 0x01FF
SUB_VENDOR = 0x1987
SUB_DEVICE = 0x5012
PCILOC_BASE = 0x0300

# RC_LogicalDevice field offsets (rc_linux.h RC_LD_*_OFFSET)
LD_ELEMENTOFFSET = 0x04
LD_DEVICETYPE = 0x0C
LD_CAPACITY = 0x50
LD_DEVICES = 0x68
LD_FIRSTCOUNT = 0x6C
LD_SECONDCOUNT = 0x70
LD_PACKETSIZE = 0x90
LD_CHUNKSIZE = 0xAC
LD_CHUNKINDEX = 0x110
LD_STATE = 0x08
LD_ID = 0x2C
LD_PARENTCOUNT = 0x74
LD_CACHE = 0x80
LD_STATE_NORMAL = 0x200
LD_CACHE_RW = 0xC0000000

# RC_LogicalElement_LE (64 bytes each)
LE_BYTES = 64
LE_DEVICEID = 0x00
LE_ALLOC_OFFSET = 0x10
LE_ALLOC_SIZE = 0x18
LE_USERDATA_OFFSET = 0x20
LE_USERDATA_SIZE = 0x28

ELEM_ARRAY_OFFSET = 0x130  # past every fixed LD field we set (max 0x110+4)

# Commit-block field offsets (rc_linux.h RC_COMMIT_*_OFFSET).  The word at
# +0x00 is the checksum over [0x08, 0x200), as vendor firmware writes it —
# corroborated on the X399 fixtures, not a timestamp of its own.
COMMIT_CHECKSUM = 0x00
COMMIT_GEN_LBA = 0x08
COMMIT_GEN_LEN = 0x0C
COMMIT_GEN_TS = 0x10
COMMIT_GEN_SEQ = 0x30

# Generation timestamps: arbitrary nonzero values.  The commit block's
# GEN_TS must equal the u64 at offset 0 of the active generation's header
# sector — the driver validates that linkage.  The decoy generation gets a
# different timestamp so it can never accidentally satisfy the check.
ACTIVE_GEN_TS = 0x5243544553540A11
DECOY_GEN_TS = 0x5243544553540DEA

GEN_HEADER_BYTES = 0x200        # records start 0x200 into a generation
DECOY_GEN_SECTOR = 0            # ring head — where a naive scan looks first
ACTIVE_GEN_SECTOR = 16

# Firmware's reservation ahead of user data: AllocOffset = 0x100000
# sectors (512 MiB, 1 MiB-aligned) and UserDataOffset = 0x105000
# (546 MiB) on the vendor's own LD elements.  Member images must be
# larger than this.
ALLOC_START = 0x100000
USERDATA_START = 0x105000

# chunk_index encoding per rcraid.sys FUN_1400121d0 (see
# rc_volume_chunk_sectors_for in rc_nvme.c): only 2 and 3 are explicit,
# everything else means 128 sectors.
CHUNK_INDEX_SECTORS = {3: 512, 2: 256, 1: 128, 0: 128}


def target_size_bytes(path: str) -> int:
    """Capacity of an image file or block device, in bytes."""
    st = os.stat(path)
    if stat.S_ISBLK(st.st_mode):
        with open(path, "rb", buffering=0) as f:
            buf = fcntl.ioctl(f.fileno(), BLKGETSIZE64, b"\0" * 8)
        return struct.unpack("<Q", buf)[0]
    return st.st_size


def rc_checksum(payload: bytes) -> int:
    """rc_raidcore_checksum(): XOR of 64-bit LE words after swapping two
    16-bit lanes chosen by (acc & 3, word & 3), falling back to
    (i & 3, (i+1) & 3) when they collide."""
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


def build_metadata_block(device_id: int) -> bytes:
    md = bytearray(SECTOR)
    struct.pack_into("<Q", md, 0x08, RAIDCORE_MAGIC)
    struct.pack_into("<Q", md, 0x10, device_id)
    struct.pack_into("<Q", md, 0x18, CONFIG_COMMIT_LBA)
    struct.pack_into("<Q", md, 0x20, CONFIG_RING_LBA)
    struct.pack_into("<I", md, 0x28, CONFIG_RING_SIZE)
    struct.pack_into("<I", md, 0x2C, RAIDCORE_VERSION)
    struct.pack_into("<I", md, 0x30, FEATURES)
    struct.pack_into("<I", md, 0x34, 0)  # SpareInfo
    struct.pack_into("<Q", md, 0x38, 0)  # MBRChecksum (unchecked by driver)
    struct.pack_into("<Q", md, 0x00, rc_checksum(bytes(md[0x08:0x200])))
    return bytes(md)


def devtype_for(level: str) -> int:
    """On-disk DeviceType: 0x1BF6 carries 0/1/10 in the counts, parity
    levels have their own explicit codes (REVERSE_ENGINEERING.md)."""
    return 0x1BF5 if level == "raid5" else DEVTYPE_VOLUME


def counts_for(level: str, members: int):
    """FirstCount (stripe width) x SecondCount (mirror count), as real
    firmware encodes the level.  RAID5 cannot be expressed in the counts
    (the driver identifies it by DeviceType alone), so 1 x N is what the
    vendor core writes and what the parser ignores."""
    if level == "raid0":
        return members, 1
    if level == "raid1":
        return 1, 2
    if level == "raid5":
        return 1, members
    if level == "raid10":
        # 2-way mirrors, so half the members are stripe columns.
        return members // 2, 2
    raise ValueError(level)


def build_raw_disk_ld(device_id: int, disk_sectors: int) -> bytes:
    """The per-disk raw LD (0x1BF9): devices=1, FirstCount=SecondCount=1,
    element array = this disk's own DeviceID, capacity = the raw disk.
    Placed in the committed generation BEFORE the array LD, mimicking real
    firmware — a parser that accepts it assembles a bogus 1-member volume
    and fails the test's capacity/level checks."""
    size = ELEM_ARRAY_OFFSET + LE_BYTES
    ld = bytearray(size)
    struct.pack_into("<I", ld, 0x00, DST_LOGICAL_DEVICE)
    struct.pack_into("<I", ld, LD_ELEMENTOFFSET, ELEM_ARRAY_OFFSET)
    struct.pack_into("<I", ld, LD_DEVICETYPE, DEVTYPE_SINGLE)
    struct.pack_into("<Q", ld, LD_CAPACITY, disk_sectors)
    struct.pack_into("<I", ld, LD_DEVICES, 1)
    struct.pack_into("<I", ld, LD_FIRSTCOUNT, 1)
    struct.pack_into("<I", ld, LD_SECONDCOUNT, 1)
    struct.pack_into("<I", ld, LD_PACKETSIZE, size)
    struct.pack_into("<I", ld, LD_CHUNKINDEX, 1)
    e = ELEM_ARRAY_OFFSET
    struct.pack_into("<Q", ld, e + LE_DEVICEID, device_id)
    struct.pack_into("<Q", ld, e + LE_ALLOC_SIZE, disk_sectors)
    struct.pack_into("<Q", ld, e + LE_USERDATA_SIZE, disk_sectors)
    return bytes(ld)


def build_ld_record(level: str, device_ids, capacity: int,
                    user_size: int, chunk_index: int,
                    volume_id: int = 0) -> bytes:
    n = len(device_ids)
    first, second = counts_for(level, n)
    size = ELEM_ARRAY_OFFSET + n * LE_BYTES
    ld = bytearray(size)
    struct.pack_into("<I", ld, 0x00, DST_LOGICAL_DEVICE)
    struct.pack_into("<I", ld, LD_ELEMENTOFFSET, ELEM_ARRAY_OFFSET)
    struct.pack_into("<I", ld, LD_DEVICETYPE, devtype_for(level))
    struct.pack_into("<Q", ld, LD_CAPACITY, capacity)
    struct.pack_into("<I", ld, LD_DEVICES, n)
    struct.pack_into("<I", ld, LD_FIRSTCOUNT, first)
    struct.pack_into("<I", ld, LD_SECONDCOUNT, second)
    struct.pack_into("<I", ld, LD_PACKETSIZE, size)
    struct.pack_into("<I", ld, LD_CHUNKSIZE, 0)    # 0 → chunk_index encoding
    # Real RAID1 records carry a chunk_index too (observed =3 on hardware)
    # even though mirrors don't stripe; the driver must ignore it for RAID1.
    struct.pack_into("<I", ld, LD_CHUNKINDEX, chunk_index)
    # Fields rcadm renders: Id, State, ParentCount, cache flags.
    struct.pack_into("<Q", ld, LD_ID, volume_id)
    struct.pack_into("<I", ld, LD_STATE, LD_STATE_NORMAL)
    struct.pack_into("<I", ld, LD_PARENTCOUNT, 1)
    struct.pack_into("<I", ld, LD_CACHE, LD_CACHE_RW)
    for pos, did in enumerate(device_ids):
        e = ELEM_ARRAY_OFFSET + pos * LE_BYTES
        struct.pack_into("<Q", ld, e + LE_DEVICEID, did)
        struct.pack_into("<Q", ld, e + LE_ALLOC_OFFSET, ALLOC_START)
        struct.pack_into("<Q", ld, e + LE_ALLOC_SIZE, user_size)
        struct.pack_into("<Q", ld, e + LE_USERDATA_OFFSET, USERDATA_START)
        struct.pack_into("<Q", ld, e + LE_USERDATA_SIZE, user_size)
    return bytes(ld)


def build_pd_record(device_id: int, ptype: int, channel: int, port: int,
                    capacity: int) -> bytes:
    pd = bytearray(PD_BYTES)
    struct.pack_into("<I", pd, 0x00, PD_TAG)
    struct.pack_into("<Q", pd, PD_DEVICE_ID, device_id)
    struct.pack_into("<H", pd, PD_CHANNEL, channel)
    struct.pack_into("<H", pd, PD_PORT, port)
    struct.pack_into("<Q", pd, PD_CAPACITY, capacity)
    struct.pack_into("<I", pd, PD_DEVICE_TYPE, ptype)
    struct.pack_into("<I", pd, PD_STATE, PD_STATE_ONLINE)
    return bytes(pd)


def build_ctrl_record(index: int, sub_vid: int = SUB_VENDOR,
                      sub_dev: int = SUB_DEVICE, pci_loc: int = None) -> bytes:
    if pci_loc is None:
        pci_loc = PCILOC_BASE + index
    cid = (0x1022 << 32) | (0xB000 << 16) | index
    c = bytearray(CTRL_BYTES)
    struct.pack_into("<I", c, 0x00, CTRL_TAG)
    struct.pack_into("<Q", c, CTRL_ID, cid)
    struct.pack_into("<I", c, CTRL_HANDLE, 0x10000 | index)
    c[CTRL_NAME:CTRL_NAME + 5] = b"NVMe\x00"
    struct.pack_into("<H", c, CTRL_FEATURE, CTRL_FEATURE_VALUE)
    struct.pack_into("<I", c, CTRL_PORTS, 1)
    struct.pack_into("<H", c, CTRL_VID, 0x1022)
    struct.pack_into("<H", c, CTRL_DID, 0xB000)
    struct.pack_into("<I", c, CTRL_NUMBER, index)
    struct.pack_into("<H", c, CTRL_SUBVID, sub_vid)
    struct.pack_into("<H", c, CTRL_SUBDID, sub_dev)
    struct.pack_into("<Q", c, CTRL_REGION_ID, cid)
    struct.pack_into("<H", c, CTRL_ORIGVID, 0x1022)
    struct.pack_into("<H", c, CTRL_ORIGDID, 0xB000)
    struct.pack_into("<I", c, CTRL_PCILOC, pci_loc)
    return bytes(c)


def build_records(level: str, device_ids, capacity: int, user_size: int,
                  chunk_index: int, disk_sectors: int) -> bytes:
    """Controller PD, one PD per member, the array LD, the raw-disk LD for
    member 0, then one ControllerDevice record per member — the order
    firmware writes (mirrors rc_mb_records in src/rc_meta_build.h)."""
    volume_id = (device_ids[0] ^ (device_ids[1] << 1)) | 1
    out = build_pd_record(PD_CONTROLLER_ID, PD_TYPE_CONTROLLER,
                          0xFFFF, 0, 0)
    for did in device_ids:
        out += build_pd_record(did, PD_TYPE_DISK, 1, 1, disk_sectors)
    out += build_ld_record(level, device_ids, capacity, user_size,
                           chunk_index, volume_id)
    out += build_raw_disk_ld(device_ids[0], disk_sectors)
    for i in range(len(device_ids)):
        out += build_ctrl_record(i)
    return out


def build_generation(ts: int, records: bytes) -> bytes:
    """One config generation: header sector (timestamp at +0x00), records
    at +GEN_HEADER_BYTES, zero-padded to a whole number of sectors (the
    driver requires the committed length to be sector-aligned)."""
    body = bytearray(GEN_HEADER_BYTES) + bytearray(records)
    struct.pack_into("<Q", body, 0x00, ts)
    pad = (-len(body)) % SECTOR
    return bytes(body) + bytes(pad)


def build_commit_block(gen_lba: int, gen_bytes: int, gen_ts: int) -> bytes:
    c = bytearray(SECTOR)
    struct.pack_into("<I", c, COMMIT_GEN_LBA, gen_lba)
    struct.pack_into("<I", c, COMMIT_GEN_LEN, gen_bytes)
    struct.pack_into("<Q", c, COMMIT_GEN_TS, gen_ts)
    struct.pack_into("<I", c, COMMIT_GEN_SEQ, 2)         # decoy was gen 1
    struct.pack_into("<Q", c, COMMIT_CHECKSUM,
                     rc_checksum(bytes(c[0x08:0x200])))
    return bytes(c)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", choices=LEVELS, default="raid0")
    ap.add_argument("--chunk-index", type=int, default=3,
                    help="RAID0 stripe encoding: 3=256KiB, 2=128KiB, 0/1=64KiB"
                         " (default 3, matching the hardware dev box)")
    ap.add_argument("images", nargs="+", help="member image files (in position order)")
    args = ap.parse_args()

    if args.level == "raid1" and len(args.images) != 2:
        sys.exit("mkmeta: raid1 wants exactly 2 members")
    if args.level == "raid5" and len(args.images) < 3:
        sys.exit("mkmeta: raid5 wants at least 3 members")
    if args.level == "raid10" and (len(args.images) < 4 or
                                   len(args.images) % 2):
        sys.exit("mkmeta: raid10 wants an even number of members, >= 4")
    if len(args.images) < 2:
        sys.exit("mkmeta: need at least 2 member images")

    if not 0 <= args.chunk_index <= 3:
        # Masking to &3 would lay the image out for one chunk size while
        # the driver (rc_volume_chunk_sectors_for) treats on-disk values
        # > 3 as "not understood" and falls back to 64 KiB —
        # self-inconsistent metadata.  Refuse instead.
        sys.exit("mkmeta: --chunk-index must be 0-3")
    chunk_sectors = CHUNK_INDEX_SECTORS[args.chunk_index]

    sizes = []
    for img in args.images:
        sz = target_size_bytes(img)
        if sz % SECTOR:
            sys.exit(f"mkmeta: {img} size is not sector-aligned")
        sizes.append(sz // SECTOR)

    min_sectors = min(sizes)
    if min_sectors <= USERDATA_START + chunk_sectors:
        sys.exit(f"mkmeta: images too small — need > "
                 f"{(USERDATA_START + chunk_sectors) * SECTOR} bytes")

    # Per-member user-data region: everything past the metadata reserve,
    # rounded down to a whole number of stripes.
    user_size = (min_sectors - USERDATA_START) // chunk_sectors * chunk_sectors
    if args.level == "raid0":
        capacity = user_size * len(args.images)
    elif args.level == "raid5":
        # One member's worth is parity; the driver's capacity cross-check
        # derives data_cols = devices - 1 for parity levels.
        capacity = user_size * (len(args.images) - 1)
    elif args.level == "raid10":
        # Every column is a 2-way mirror, so only the columns add capacity
        # (the driver cross-checks capacity == user_size x FirstCount).
        capacity = user_size * (len(args.images) // 2)
    else:  # raid1
        capacity = user_size

    device_ids = [0x5243544553540000 + i for i in range(len(args.images))]

    # Active generation: the real config, named by the commit block.  A raw
    # single-disk LD for member 0 goes FIRST, like real firmware layouts —
    # its element array is member 0's own DeviceID, so a parser that treats
    # raw-disk records as volumes matches it before the array LD and
    # assembles a bogus 1-member volume (wrong capacity and level → the
    # guest checks fail).
    active_records = build_records(args.level, device_ids, capacity,
                                   user_size, args.chunk_index, min_sectors)
    active_gen = build_generation(ACTIVE_GEN_TS, active_records)

    # Decoy generation: a dead config for the OPPOSITE level with the same
    # DeviceIDs and a capacity that can't match the active one.  Sits at
    # the ring head, unreferenced by the commit block — exactly how a
    # deleted array's journal entries shadowed the live RAID1 on real
    # hardware.  A first-match parser assembles this and fails the test.
    decoy_level = "raid1" if args.level in ("raid0", "raid10", "raid5") \
        else "raid0"
    decoy_capacity = (user_size if decoy_level == "raid1"
                      else user_size * len(args.images))
    decoy_ld = build_ld_record(decoy_level, device_ids, decoy_capacity,
                               user_size, args.chunk_index)
    decoy_gen = build_generation(DECOY_GEN_TS, decoy_ld)
    assert len(decoy_gen) <= (ACTIVE_GEN_SECTOR - DECOY_GEN_SECTOR) * SECTOR

    commit = build_commit_block(CONFIG_RING_LBA + ACTIVE_GEN_SECTOR,
                                len(active_gen), ACTIVE_GEN_TS)

    for pos, img in enumerate(args.images):
        is_block = stat.S_ISBLK(os.stat(img).st_mode)
        with open(img, "r+b", buffering=0 if is_block else -1) as f:
            f.seek(RAIDCORE_LBA * SECTOR)
            f.write(build_metadata_block(device_ids[pos]))
            f.seek(CONFIG_COMMIT_LBA * SECTOR)
            f.write(commit)
            f.seek((CONFIG_RING_LBA + DECOY_GEN_SECTOR) * SECTOR)
            f.write(decoy_gen)
            f.seek((CONFIG_RING_LBA + ACTIVE_GEN_SECTOR) * SECTOR)
            f.write(active_gen)
            if is_block:
                # The next reader is rcraid's own NVMe queues, and the
                # controller is about to be reset when we unbind it from
                # nvme — put the metadata on media first.
                os.fsync(f.fileno())

    print(f"level={args.level} members={len(args.images)} "
          f"chunk_sectors={chunk_sectors} user_size={user_size} "
          f"decoy_level={decoy_level}")
    print(f"capacity_sectors={capacity}")
    print(f"userdata_offset_sectors={USERDATA_START}")


if __name__ == "__main__":
    main()
