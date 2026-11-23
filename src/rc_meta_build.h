// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — RAIDCore metadata encoder
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Builds the byte images a metadata writer has to put on a member:
 * the 512-byte RC_MetaData block at LBA 0x5000, a config generation
 * (header sector + packed records, starting with the per-disk raw LD
 * and then the array LD), and the config-commit block at LBA 0x5001.
 *
 * The layout mirrors, byte for byte, what scripts/qemu-test/mkmeta.py
 * writes — the generator the QEMU rig assembles arrays from, itself
 * verified against firmware-created arrays.  make test-metabuild builds
 * both encoders and diffs their output, so this file and the rig cannot
 * drift apart silently.
 *
 * Pure on purpose: no allocation, no I/O, no printing, and no includes.
 * An includer must supply u8/u16/u32/u64, memcpy() and memset()
 * (rc_linux.h does, in-kernel).  Writing the images to media, choosing
 * the generation slot and the two-phase commit are deliberately not
 * here: those are policy and belong to the caller.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See docs/RE_METHODOLOGY.md.
 */

#ifndef _RC_META_BUILD_H_
#define _RC_META_BUILD_H_

#define RC_MB_SECTOR		512u

/* Fixed layout locations (RC_MetaData in rc_linux.h). */
#define RC_MB_METADATA_LBA	0x5000u
#define RC_MB_COMMIT_LBA	0x5001u
#define RC_MB_RING_LBA		0x5800u
#define RC_MB_RING_SECTORS	2048u
/* Firmware reservation ahead of user data.  The vendor's own LD elements
 * on the live RAID 0 (2026-11-13) read AllocOffset = 0x100000 sectors
 * (512 MiB) and UserDataOffset = 0x105000 (546 MiB), i.e. a 1 MiB-aligned
 * allocation region plus a 10 MiB gap.  Starting user data at 0x6000, as
 * this encoder did, put every sector inside the area firmware keeps for
 * itself (config ring, relocation, MBR stash). */
#define RC_MB_ALLOC_LBA		0x100000u	/* 512 MiB */
#define RC_MB_USERDATA_LBA	0x105000u	/* 546 MiB */
#define RC_MB_MAGIC		0x65726F4344494152ULL	/* "RAIDCore" */
#define RC_MB_VERSION		0x00030000u
#define RC_MB_FEATURES		0x1Cu	/* observed on firmware arrays */

/* RC_MetaData field offsets (512-byte block). */
#define RC_MB_MD_CHECKSUM	0x00
#define RC_MB_MD_MAGIC		0x08
#define RC_MB_MD_DEVICE_ID	0x10
#define RC_MB_MD_COMMIT_LBA	0x18
#define RC_MB_MD_RING_LBA	0x20
#define RC_MB_MD_RING_SIZE	0x28
#define RC_MB_MD_VERSION	0x2C
#define RC_MB_MD_FEATURES	0x30
#define RC_MB_MD_SPARE_INFO	0x34
#define RC_MB_MD_MBR_CHECKSUM	0x38
#define RC_MB_MD_PAYLOAD	0x1F8u	/* checksummed bytes, from +0x08 */

/* Config generation: header sector, records from +0x200. */
#define RC_MB_GEN_HEADER	0x200u
#define RC_MB_GEN_TS		0x00

/* RC_ConfigCommit field offsets (512-byte block).  Checksum at +0x00
 * covers [0x08, 0x200), the layout vendor firmware writes (corroborated
 * on the X399 fixtures — docs/REVERSE_ENGINEERING.md). */
#define RC_MB_COMMIT_CHECKSUM	0x00
#define RC_MB_COMMIT_GEN_LBA	0x08
#define RC_MB_COMMIT_GEN_LEN	0x0C
#define RC_MB_COMMIT_GEN_TS	0x10
#define RC_MB_COMMIT_GEN_SEQ	0x30

/* RC_LogicalDevice field offsets. */
#define RC_MB_LD_TAG		0x00
#define RC_MB_LD_ELEM_OFFSET	0x04
#define RC_MB_LD_DEVICE_TYPE	0x0C
#define RC_MB_LD_CAPACITY	0x50
#define RC_MB_LD_DEVICES	0x68
#define RC_MB_LD_FIRST_COUNT	0x6C
#define RC_MB_LD_SECOND_COUNT	0x70
#define RC_MB_LD_PACKET_SIZE	0x90
#define RC_MB_LD_CHUNK_SIZE	0xAC
#define RC_MB_LD_CHUNK_INDEX	0x110
#define RC_MB_LD_ELEM_ARRAY	0x130u

/* RC_LogicalElement_LE (64 bytes each). */
#define RC_MB_LE_BYTES		64u
#define RC_MB_LE_DEVICE_ID	0x00
#define RC_MB_LE_ALLOC_OFFSET	0x10
#define RC_MB_LE_ALLOC_SIZE	0x18
#define RC_MB_LE_USER_OFFSET	0x20
#define RC_MB_LE_USER_SIZE	0x28

/* Record tags. */
#define RC_MB_TAG_PHYSICAL_DEVICE	0x25BCu
#define RC_MB_TAG_LOGICAL_DEVICE	0x25BDu
#define RC_MB_TAG_CONTROLLER_DEVICE	0x25BEu
/* DeviceType the request/LD format uses for a striped/mirrored volume;
 * the level itself is in FirstCount/SecondCount. */
#define RC_MB_DEVTYPE_VOLUME	0x1BF6u
/* Raw-disk LD firmware publishes for a disk that is not an array. */
#define RC_MB_DEVTYPE_SINGLE	0x1BF9u
/* Parity levels carry an explicit DeviceType; the counts do not encode
 * them (RAID 5 is FirstCount 1, SecondCount = members). */
#define RC_MB_DEVTYPE_RAID5	0x1BF5u
#define RC_MB_DEVTYPE_RAID6	0x1BFAu

/* Any LD DeviceType that names an array volume (rather than a raw disk). */
static inline int rc_mb_is_volume_type(u32 t)
{
	return t == RC_MB_DEVTYPE_VOLUME || t == RC_MB_DEVTYPE_RAID5 ||
	       t == RC_MB_DEVTYPE_RAID6;
}

/* Largest this encoder emits per LD record (16 members + element table). */
#define RC_MB_LD_MAX		(RC_MB_LD_ELEM_ARRAY + 16u * RC_MB_LE_BYTES)

static inline void rc_mb_put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static inline void rc_mb_put16(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static inline void rc_mb_put64(u8 *p, u64 v)
{
	rc_mb_put32(p, (u32)v);
	rc_mb_put32(p + 4, (u32)(v >> 32));
}

static inline u32 rc_mb_get32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
	       ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 rc_mb_get64(const u8 *p)
{
	return (u64)rc_mb_get32(p) | ((u64)rc_mb_get32(p + 4) << 32);
}

/*
 * RAIDCore checksum: XOR of the 64-bit words, each first permuted by
 * swapping the 16-bit lanes (acc & 3) and (word & 3), or ((i + 1) & 3)
 * and (i & 3) when those collide.  Not a CRC; order-dependent.
 * Same algorithm as rc_raidcore_checksum() in rc_nvme.c, which is what
 * the parser validates a member block against.
 */
static inline u64 rc_mb_checksum(const void *data, u32 bytes)
{
	const u8 *bp = (const u8 *)data;
	u32 words = bytes >> 3;
	u64 acc = 0;
	u32 i;

	for (i = 0; i < words; i++) {
		u64 w = rc_mb_get64(bp + (u32)i * 8);
		u32 a = (u32)(acc & 3);
		u32 b = (u32)(w & 3);
		u16 lane[4];
		u16 tmp;
		u32 k;

		if (a == b) {
			a = i & 3;
			b = (i + 1) & 3;
		}
		for (k = 0; k < 4; k++)
			lane[k] = (u16)(w >> (k * 16));
		tmp = lane[a];
		lane[a] = lane[b];
		lane[b] = tmp;
		w = 0;
		for (k = 0; k < 4; k++)
			w |= (u64)lane[k] << (k * 16);

		acc ^= w;
	}
	return acc;
}

/*
 * Per-member RC_MetaData block (512 B).  The commit and ring LBAs are
 * the fixed locations above.  MBRChecksum is not derivable from this
 * block: firmware fills it from the member's MBR area, outside the 512
 * bytes here.  A writer must carry the value already on media forward
 * (rc_mb_metadata_block_ex); zeroing it makes the encoder's output
 * differ from the firmware's block at +0x38 and turns off the writer
 * gate in rc_nvme_verify_meta_encoding.  This wrapper is the "no MBR"
 * form for a freshly initialised member.
 */
static inline void rc_mb_metadata_block_ex(u8 *out, u64 device_id,
					   u64 mbr_checksum)
{
	memset(out, 0, RC_MB_SECTOR);
	rc_mb_put64(out + RC_MB_MD_MAGIC, RC_MB_MAGIC);
	rc_mb_put64(out + RC_MB_MD_DEVICE_ID, device_id);
	rc_mb_put64(out + RC_MB_MD_COMMIT_LBA, RC_MB_COMMIT_LBA);
	rc_mb_put64(out + RC_MB_MD_RING_LBA, RC_MB_RING_LBA);
	rc_mb_put32(out + RC_MB_MD_RING_SIZE, RC_MB_RING_SECTORS);
	rc_mb_put32(out + RC_MB_MD_VERSION, RC_MB_VERSION);
	rc_mb_put32(out + RC_MB_MD_FEATURES, RC_MB_FEATURES);
	rc_mb_put32(out + RC_MB_MD_SPARE_INFO, 0);
	rc_mb_put64(out + RC_MB_MD_MBR_CHECKSUM, mbr_checksum);
	rc_mb_put64(out + RC_MB_MD_CHECKSUM,
		    rc_mb_checksum(out + RC_MB_MD_MAGIC, RC_MB_MD_PAYLOAD));
}

static inline void rc_mb_metadata_block(u8 *out, u64 device_id)
{
	rc_mb_metadata_block_ex(out, device_id, 0);
}

/* User-data sectors a member offers after the metadata reserve, rounded
 * down to whole stripes.  0 means the member is too small to use. */
static inline u64 rc_mb_user_sectors(u64 member_sectors, u32 chunk_sectors)
{
	if (!chunk_sectors || member_sectors <= RC_MB_USERDATA_LBA)
		return 0;
	return (member_sectors - RC_MB_USERDATA_LBA) / chunk_sectors *
	       chunk_sectors;
}

/*
 * Array LD record: one element per member, each covering the same user
 * window.  `capacity` is the assembled volume in sectors (user_size for
 * a mirror, user_size * members for a stripe).  Returns the record size
 * in bytes, 0 on bad arguments.
 */
static inline u32 rc_mb_ld_record(u8 *out, u32 cap, u32 device_type,
				  u32 first, u32 second, u32 members,
				  const u64 *device_ids, u64 capacity,
				  u64 user_size, u32 chunk_index)
{
	u32 size = RC_MB_LD_ELEM_ARRAY + members * RC_MB_LE_BYTES;
	u32 i;

	if (!members || members > 16 || size > cap)
		return 0;

	memset(out, 0, size);
	rc_mb_put32(out + RC_MB_LD_TAG, RC_MB_TAG_LOGICAL_DEVICE);
	rc_mb_put32(out + RC_MB_LD_ELEM_OFFSET, RC_MB_LD_ELEM_ARRAY);
	rc_mb_put32(out + RC_MB_LD_DEVICE_TYPE, device_type);
	rc_mb_put64(out + RC_MB_LD_CAPACITY, capacity);
	rc_mb_put32(out + RC_MB_LD_DEVICES, members);
	rc_mb_put32(out + RC_MB_LD_FIRST_COUNT, first);
	rc_mb_put32(out + RC_MB_LD_SECOND_COUNT, second);
	rc_mb_put32(out + RC_MB_LD_PACKET_SIZE, size);
	rc_mb_put32(out + RC_MB_LD_CHUNK_SIZE, 0);	/* 0 = chunk_index */
	rc_mb_put32(out + RC_MB_LD_CHUNK_INDEX, chunk_index);

	for (i = 0; i < members; i++) {
		u32 e = RC_MB_LD_ELEM_ARRAY + i * RC_MB_LE_BYTES;

		rc_mb_put64(out + e + RC_MB_LE_DEVICE_ID, device_ids[i]);
		rc_mb_put64(out + e + RC_MB_LE_ALLOC_OFFSET, RC_MB_ALLOC_LBA);
		rc_mb_put64(out + e + RC_MB_LE_ALLOC_SIZE, user_size);
		rc_mb_put64(out + e + RC_MB_LE_USER_OFFSET, RC_MB_USERDATA_LBA);
		rc_mb_put64(out + e + RC_MB_LE_USER_SIZE, user_size);
	}
	return size;
}

/*
 * The raw-disk LD (0x1BF9) firmware publishes ahead of the array record:
 * 1x1 counts and the disk's own DeviceID, capacity/alloc = the whole
 * disk.  Not a volume; the parser must skip it, and a writer that omits
 * it produces a generation the firmware layout would not.
 */
static inline u32 rc_mb_raw_ld_record(u8 *out, u32 cap, u64 device_id,
				      u64 disk_sectors)
{
	u32 size = RC_MB_LD_ELEM_ARRAY + RC_MB_LE_BYTES;

	if (size > cap)
		return 0;

	memset(out, 0, size);
	rc_mb_put32(out + RC_MB_LD_TAG, RC_MB_TAG_LOGICAL_DEVICE);
	rc_mb_put32(out + RC_MB_LD_ELEM_OFFSET, RC_MB_LD_ELEM_ARRAY);
	rc_mb_put32(out + RC_MB_LD_DEVICE_TYPE, RC_MB_DEVTYPE_SINGLE);
	rc_mb_put64(out + RC_MB_LD_CAPACITY, disk_sectors);
	rc_mb_put32(out + RC_MB_LD_DEVICES, 1);
	rc_mb_put32(out + RC_MB_LD_FIRST_COUNT, 1);
	rc_mb_put32(out + RC_MB_LD_SECOND_COUNT, 1);
	rc_mb_put32(out + RC_MB_LD_PACKET_SIZE, size);
	rc_mb_put32(out + RC_MB_LD_CHUNK_INDEX, 1);
	rc_mb_put64(out + RC_MB_LD_ELEM_ARRAY + RC_MB_LE_DEVICE_ID, device_id);
	rc_mb_put64(out + RC_MB_LD_ELEM_ARRAY + RC_MB_LE_ALLOC_SIZE,
		    disk_sectors);
	rc_mb_put64(out + RC_MB_LD_ELEM_ARRAY + RC_MB_LE_USER_SIZE,
		    disk_sectors);
	return size;
}

/*
 * RC_PhysicalDeviceOnDisk (tag 0x25BC) — 0x80 bytes inside a generation,
 * ahead of the LD records.  Layout read off the vendor's own 4-member
 * generation on 2026-11-13: tag, DeviceID, channel/port, capacity, type
 * and state.  Firmware writes one controller record (type 0x1201) and one
 * record per member (type 0x1200), then a volume record (0x1203) for the
 * array it already knows about.
 */
#define RC_MB_PD_BYTES		0x80u
#define RC_MB_PD_DEVICE_ID	0x04u
#define RC_MB_PD_CHANNEL	0x0Cu
#define RC_MB_PD_PORT		0x0Eu
#define RC_MB_PD_CAPACITY	0x1Cu
#define RC_MB_PD_DEVICE_TYPE	0x24u
#define RC_MB_PD_STATE		0x2Cu
#define RC_MB_PD_TYPE_CONTROLLER 0x1201u
#define RC_MB_PD_TYPE_DISK	0x1200u
#define RC_MB_PD_STATE_ONLINE	0x3000u
/* DeviceID the controller record carries (observed on the live array). */
#define RC_MB_PD_CONTROLLER_ID	0x0000102285900000ULL

/* RC_ControllerDevice (tag 0x25BE) — 0x88 bytes, one per member. */
#define RC_MB_CTRL_BYTES	0x88u
#define RC_MB_CTRL_ID		0x04u
#define RC_MB_CTRL_HANDLE	0x0Cu
#define RC_MB_CTRL_NAME		0x24u
#define RC_MB_CTRL_FEATURE	0x34u
#define RC_MB_CTRL_PORTS	0x68u
#define RC_MB_CTRL_VID		0x6Cu
#define RC_MB_CTRL_DID		0x6Eu
#define RC_MB_CTRL_NUMBER	0x70u
#define RC_MB_CTRL_SUBVID	0x74u
#define RC_MB_CTRL_SUBDID	0x76u
#define RC_MB_CTRL_REGION_ID	0x78u
#define RC_MB_CTRL_ORIGVID	0x80u
#define RC_MB_CTRL_ORIGDID	0x82u
#define RC_MB_CTRL_PCILOC	0x84u
#define RC_MB_CTRL_FEATURE_VALUE 0x01ffu

/*
 * LD fields rcadm renders but the assembler does not need: State at +0x08
 * (a zeroed word renders as UNKNOWN and CREATE is reported failed), Id at
 * +0x2C, ParentCount at +0x74 (1 for a single array) and cache flags at
 * +0x80 (read cache + write-back).
 */
#define RC_MB_LD_STATE		0x08u
#define RC_MB_LD_ID		0x2Cu
#define RC_MB_LD_PARENTCOUNT	0x74u
#define RC_MB_LD_CACHE		0x80u
#define RC_MB_LD_STATE_NORMAL	0x200u
#define RC_MB_LD_CACHE_RW	0xc0000000u

/* One member's identity, as the record set needs it. */
struct rc_mb_member_meta {
	u64 device_id;
	u64 disk_sectors;
	u16 sub_vid;
	u16 sub_dev;
	u32 pci_loc;
};

static inline u32 rc_mb_pd_record(u8 *out, u32 cap, u64 device_id, u32 type,
				  u16 channel, u16 port, u64 capacity)
{
	if (cap < RC_MB_PD_BYTES)
		return 0;
	memset(out, 0, RC_MB_PD_BYTES);
	rc_mb_put32(out, RC_MB_TAG_PHYSICAL_DEVICE);
	rc_mb_put64(out + RC_MB_PD_DEVICE_ID, device_id);
	rc_mb_put16(out + RC_MB_PD_CHANNEL, channel);
	rc_mb_put16(out + RC_MB_PD_PORT, port);
	rc_mb_put64(out + RC_MB_PD_CAPACITY, capacity);
	rc_mb_put32(out + RC_MB_PD_DEVICE_TYPE, type);
	rc_mb_put32(out + RC_MB_PD_STATE, RC_MB_PD_STATE_ONLINE);
	return RC_MB_PD_BYTES;
}

static inline u32 rc_mb_ctrl_record(u8 *out, u32 cap, u64 id, u32 number,
				    u16 sub_vid, u16 sub_dev, u32 pci_loc)
{
	if (cap < RC_MB_CTRL_BYTES)
		return 0;
	memset(out, 0, RC_MB_CTRL_BYTES);
	rc_mb_put32(out, RC_MB_TAG_CONTROLLER_DEVICE);
	rc_mb_put64(out + RC_MB_CTRL_ID, id);
	rc_mb_put32(out + RC_MB_CTRL_HANDLE, 0x10000u | number);
	memcpy(out + RC_MB_CTRL_NAME, "NVMe", 5);
	rc_mb_put16(out + RC_MB_CTRL_FEATURE, RC_MB_CTRL_FEATURE_VALUE);
	rc_mb_put32(out + RC_MB_CTRL_PORTS, 1);
	rc_mb_put16(out + RC_MB_CTRL_VID, 0x1022);
	rc_mb_put16(out + RC_MB_CTRL_DID, 0xb000);
	rc_mb_put32(out + RC_MB_CTRL_NUMBER, number);
	rc_mb_put16(out + RC_MB_CTRL_SUBVID, sub_vid);
	rc_mb_put16(out + RC_MB_CTRL_SUBDID, sub_dev);
	rc_mb_put64(out + RC_MB_CTRL_REGION_ID, id);
	rc_mb_put16(out + RC_MB_CTRL_ORIGVID, 0x1022);
	rc_mb_put16(out + RC_MB_CTRL_ORIGDID, 0xb000);
	rc_mb_put32(out + RC_MB_CTRL_PCILOC, pci_loc);
	return RC_MB_CTRL_BYTES;
}

/*
 * Full record set for one generation, in the order firmware writes it:
 * controller PD, one PD per member, the array LD, the raw-disk LD for
 * member 0, then one ControllerDevice record per member.  The array LD
 * carries the cosmetic fields rcadm renders (Id/State/ParentCount/cache).
 * Returns the byte length, 0 if it does not fit.
 */
static inline u32 rc_mb_records(u8 *out, u32 cap, u32 device_type,
				const struct rc_mb_member_meta *m, u32 members,
				u32 first, u32 second, u64 capacity,
				u64 user_size, u32 chunk_index, u64 volume_id)
{
	u64 ids[16];
	u32 off = 0, n, i, ld_off;

	if (!members || members > 16)
		return 0;
	for (i = 0; i < members; i++)
		ids[i] = m[i].device_id;

	n = rc_mb_pd_record(out, cap, RC_MB_PD_CONTROLLER_ID,
			    RC_MB_PD_TYPE_CONTROLLER, 0xffffu, 0, 0);
	if (!n)
		return 0;
	off += n;
	for (i = 0; i < members; i++) {
		n = rc_mb_pd_record(out + off, cap - off, ids[i],
				    RC_MB_PD_TYPE_DISK, 1, 1,
				    m[i].disk_sectors);
		if (!n)
			return 0;
		off += n;
	}

	ld_off = off;
	n = rc_mb_ld_record(out + off, cap - off, device_type, first,
			    second, members, ids, capacity, user_size,
			    chunk_index);
	if (!n)
		return 0;
	rc_mb_put64(out + ld_off + RC_MB_LD_ID, volume_id);
	rc_mb_put32(out + ld_off + RC_MB_LD_STATE, RC_MB_LD_STATE_NORMAL);
	rc_mb_put32(out + ld_off + RC_MB_LD_PARENTCOUNT, 1);
	rc_mb_put32(out + ld_off + RC_MB_LD_CACHE, RC_MB_LD_CACHE_RW);
	off += n;

	n = rc_mb_raw_ld_record(out + off, cap - off, ids[0],
				m[0].disk_sectors);
	if (!n)
		return 0;
	off += n;

	for (i = 0; i < members; i++) {
		u64 id = ((u64)0x1022 << 32) | ((u64)0xb000 << 16) | i;

		n = rc_mb_ctrl_record(out + off, cap - off, id, i,
				      m[i].sub_vid, m[i].sub_dev,
				      m[i].pci_loc);
		if (!n)
			return 0;
		off += n;
	}
	return off;
}

/*
 * One config generation: a 512-byte header carrying the generation
 * timestamp at +0x00, then `records` at +0x200, padded to a whole
 * number of sectors (the commit block's length must be sector-aligned).
 * Returns the generation size in bytes, 0 if it does not fit or a
 * record would straddle the header boundary.
 */
static inline u32 rc_mb_generation(u8 *out, u32 cap, u64 ts,
				   const u8 *records, u32 records_len)
{
	u32 total, padded;

	if (records_len > cap)
		return 0;
	total = RC_MB_GEN_HEADER + records_len;
	if (total < records_len)		/* u32 overflow */
		return 0;
	padded = (total + RC_MB_SECTOR - 1) & ~(RC_MB_SECTOR - 1);
	if (padded < total || padded > cap)
		return 0;

	memset(out, 0, padded);
	rc_mb_put64(out + RC_MB_GEN_TS, ts);
	if (records_len)
		memcpy(out + RC_MB_GEN_HEADER, records, records_len);
	return padded;
}

/*
 * Config-commit block (512 B): names the byte extent of the active
 * generation and its timestamp.  `seq` is the generation sequence
 * number; the parser does not read it, but firmware orders the journal
 * by it.  +0x00 is the checksum over the rest of the block, not a stamp
 * of its own: the vendor's commit blocks carry no separate timestamp
 * there (X399 fixtures), and firmware validates that word.
 */
static inline void rc_mb_commit_block(u8 *out, u32 gen_lba, u32 gen_len,
				      u64 gen_ts, u32 seq)
{
	memset(out, 0, RC_MB_SECTOR);
	rc_mb_put32(out + RC_MB_COMMIT_GEN_LBA, gen_lba);
	rc_mb_put32(out + RC_MB_COMMIT_GEN_LEN, gen_len);
	rc_mb_put64(out + RC_MB_COMMIT_GEN_TS, gen_ts);
	rc_mb_put32(out + RC_MB_COMMIT_GEN_SEQ, seq);
	rc_mb_put64(out + RC_MB_COMMIT_CHECKSUM,
		    rc_mb_checksum(out + RC_MB_COMMIT_GEN_LBA,
				   RC_MB_SECTOR - RC_MB_COMMIT_GEN_LBA));
}

#endif /* _RC_META_BUILD_H_ */
