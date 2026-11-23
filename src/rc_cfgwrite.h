// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — RC_CfgWrite request decoder
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * The /dev/rc_api shim is handed mutation frames by rcadm: a 1 KiB
 * RC_CfgWrite header followed by a command-specific payload.  This
 * header turns such a frame into named fields.  The layout is the one
 * rcadm 9.3.3-00302 actually emits (captured at the SG_IO boundary and
 * cross-checked against the vendor DWARF) — see "Captured mutation
 * frames" in docs/REVERSE_ENGINEERING.md.
 *
 * The decoder is pure: it copies bytes into a struct, never prints or
 * allocates, so the same code is exercisable from userspace.  It has no
 * includes on purpose; an includer must supply u8/u32/u64, memset() and
 * the E* errno macros (rc_linux.h does, in-kernel).
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See docs/RE_METHODOLOGY.md.
 */

#ifndef _RC_CFGWRITE_H_
#define _RC_CFGWRITE_H_

/* RC_ConfigWriteCommand values that carry a payload we know. */
#define RC_CW_CREATE		2u
#define RC_CW_DELETE		3u
#define RC_CW_TRANSFORM		8u

/* Header fields (RC_CfgWrite). */
#define RC_CW_OFF_COMMAND	0x00
#define RC_CW_OFF_CHANNEL	0x04
#define RC_CW_OFF_GENERATION	0x08
#define RC_CW_OFF_SYNC		0x10
#define RC_CW_OFF_SIZE		0x14
#define RC_CW_OFF_MAJOR		0x18
#define RC_CW_OFF_MINOR		0x1C
#define RC_CW_HDR_BYTES		0x20

/* Device identifiers are 16-byte RC_DeviceIdentifier values. */
#define RC_CW_ID_BYTES		16u

/* RC_WpGetCreateInfo payload (payload-relative offsets). */
#define RC_CW_CR_RAID_TYPE	0x00
#define RC_CW_CR_DEVICE_COUNT	0x04
#define RC_CW_CR_SPARE_COUNT	0x08
#define RC_CW_CR_CACHE_TAG	0x0C
#define RC_CW_CR_CACHE_FLAGS	0x10
#define RC_CW_CR_FLAGS		0x14
#define RC_CW_CR_SIZE_MB	0x18
#define RC_CW_CR_PREFER_ID	0x20	/* u64, not meaningful on the
					 * forced path used to capture it */
#define RC_CW_CR_FIRST_COUNT	0x28
#define RC_CW_CR_SECOND_COUNT	0x2C
#define RC_CW_CR_DIST_SPARE	0x30
#define RC_CW_CR_PREFER_READ	0x34
#define RC_CW_CR_TASK_PRIO	0x38
#define RC_CW_CR_PRIVATE	0x3C	/* DevicePrivate[32] */
#define RC_CW_CR_SPARE_LIST	0x5C	/* 4 x RC_DeviceIdentifier */
#define RC_CW_CR_DEVICE_LIST	0x9C	/* 1 x RC_DeviceIdentifier */
#define RC_CW_CR_BYTES		0xAC

/* RC_CW_DeleteLogicalDevice payload. */
#define RC_CW_DL_ALL		0x00
#define RC_CW_DL_DEVICE_COUNT	0x04
#define RC_CW_DL_DEVICE_LIST	0x08
#define RC_CW_DL_BYTES		0x18	/* one identifier; grows by 16 */

/* RC_WpGetTransformInfo payload. */
#define RC_CW_TR_RAID_TYPE	0x00
#define RC_CW_TR_LOGICAL_DEVICE	0x04	/* RC_DeviceIdentifier */
#define RC_CW_TR_DEVICE_COUNT	0x14
#define RC_CW_TR_SPARE_COUNT	0x18
#define RC_CW_TR_FLAGS		0x1C
#define RC_CW_TR_SIZE_MB	0x20
#define RC_CW_TR_FIRST_COUNT	0x28
#define RC_CW_TR_SECOND_COUNT	0x2C
#define RC_CW_TR_DIST_SPARE	0x30
#define RC_CW_TR_PREFER_READ	0x34
#define RC_CW_TR_TASK_PRIO	0x38
#define RC_CW_TR_SPARE_LIST	0x3C	/* 4 x RC_DeviceIdentifier */
#define RC_CW_TR_DEVICE_LIST	0x7C	/* 1 x RC_DeviceIdentifier */
#define RC_CW_TR_BYTES		0x8C

/* Retained device identifiers: the payload sizes above cap them at one
 * (create/transform) or one plus a count (delete), so eight is ample. */
#define RC_CW_MAX_IDS		8u

struct rc_cfgwrite_hdr {
	u32 command;
	u32 channel_id;		/* SG_IO LBA of the frame, not a channel */
	u64 generation;
	u32 sync_command;
	u32 config_write_size;	/* 0 on every captured frame */
	u32 major;
	u32 minor;
};

struct rc_cfgwrite_create {
	u32 raid_type;
	u32 device_count;
	u32 spare_count;
	u32 cache_tag_size;
	u32 cache_flags;
	u32 flags;
	u32 size_mb;
	u64 prefer_device_id;
	u32 first_count;
	u32 second_count;
	u32 dist_spare;
	u32 prefer_read_element;
	u32 task_priority;
	u8  private_bytes[32];	/* DevicePrivate[32] — the CLI fills this
				 * in and looks the created LD up by it */
	u32 nr_spare;		/* identifiers present in the payload, not
				 * the requested SpareDeviceCount */
	u32 nr_device;		/* likewise; the 172-byte create form
				 * names only one device */
	u8  spare_id[RC_CW_MAX_IDS][RC_CW_ID_BYTES];
	u8  device_id[RC_CW_MAX_IDS][RC_CW_ID_BYTES];
};

struct rc_cfgwrite_delete {
	u32 all;
	u32 device_count;
	u32 nr_device;
	u8  device_id[RC_CW_MAX_IDS][RC_CW_ID_BYTES];
};

struct rc_cfgwrite_transform {
	u32 raid_type;
	u8  logical_device[RC_CW_ID_BYTES];
	u32 device_count;
	u32 spare_count;
	u32 flags;
	u32 size_mb;
	u32 first_count;
	u32 second_count;
	u32 dist_spare;
	u32 prefer_read_element;
	u32 task_priority;
	u32 nr_spare;
	u32 nr_device;
	u8  spare_id[RC_CW_MAX_IDS][RC_CW_ID_BYTES];
	u8  device_id[RC_CW_MAX_IDS][RC_CW_ID_BYTES];
};

struct rc_cfgwrite {
	struct rc_cfgwrite_hdr hdr;
	u32 payload_bytes;	/* size the command implies, not the frame */
	union {
		struct rc_cfgwrite_create create;
		struct rc_cfgwrite_delete delete;
		struct rc_cfgwrite_transform transform;
	} u;
};

static inline u32 rc_cw_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) |
	       ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 rc_cw_le64(const u8 *p)
{
	return (u64)rc_cw_le32(p) | ((u64)rc_cw_le32(p + 4) << 32);
}

static inline void rc_cw_copy_id(u8 *dst, const u8 *src)
{
	u32 i;

	for (i = 0; i < RC_CW_ID_BYTES; i++)
		dst[i] = src[i];
}

static inline void rc_cw_copy_ids(u8 (*dst)[RC_CW_ID_BYTES], u32 *nr,
			   const u8 *src, u32 count)
{
	u32 i;

	if (count > RC_CW_MAX_IDS)
		count = RC_CW_MAX_IDS;
	for (i = 0; i < count; i++)
		rc_cw_copy_id(dst[i], src + i * RC_CW_ID_BYTES);
	*nr = count;
}

/*
 * Decode a mutation frame.  `len` is how many bytes the caller has of the
 * frame (the 1 KiB request, or less if the SG_IO transfer was short).
 *
 * Returns 0 and fills `out` when the frame is structurally valid;
 * -EINVAL for a short or inconsistent frame; -EOPNOTSUPP when the command
 * is one this decoder has no payload layout for (the header is still
 * filled in, so the caller can log which command it was).
 *
 * A returned 0 says only that the bytes parse.  Whether the requested
 * configuration may be applied is the caller's decision.
 */
static inline int rc_cfgwrite_decode(const u8 *req, u32 len, struct rc_cfgwrite *out)
{
	const u8 *p;
	u32 expect;

	memset(out, 0, sizeof(*out));
	if (len < RC_CW_HDR_BYTES)
		return -EINVAL;

	out->hdr.command = rc_cw_le32(req + RC_CW_OFF_COMMAND);
	out->hdr.channel_id = rc_cw_le32(req + RC_CW_OFF_CHANNEL);
	out->hdr.generation = rc_cw_le64(req + RC_CW_OFF_GENERATION);
	out->hdr.sync_command = rc_cw_le32(req + RC_CW_OFF_SYNC);
	out->hdr.config_write_size = rc_cw_le32(req + RC_CW_OFF_SIZE);
	out->hdr.major = rc_cw_le32(req + RC_CW_OFF_MAJOR);
	out->hdr.minor = rc_cw_le32(req + RC_CW_OFF_MINOR);

	p = req + RC_CW_HDR_BYTES;

	switch (out->hdr.command) {
	case RC_CW_CREATE: {
		struct rc_cfgwrite_create *c = &out->u.create;

		expect = RC_CW_CR_BYTES;
		if (len < RC_CW_HDR_BYTES + expect)
			return -EINVAL;

		c->raid_type = rc_cw_le32(p + RC_CW_CR_RAID_TYPE);
		c->device_count = rc_cw_le32(p + RC_CW_CR_DEVICE_COUNT);
		c->spare_count = rc_cw_le32(p + RC_CW_CR_SPARE_COUNT);
		c->cache_tag_size = rc_cw_le32(p + RC_CW_CR_CACHE_TAG);
		c->cache_flags = rc_cw_le32(p + RC_CW_CR_CACHE_FLAGS);
		c->flags = rc_cw_le32(p + RC_CW_CR_FLAGS);
		c->size_mb = rc_cw_le32(p + RC_CW_CR_SIZE_MB);
		c->prefer_device_id = rc_cw_le64(p + RC_CW_CR_PREFER_ID);
		c->first_count = rc_cw_le32(p + RC_CW_CR_FIRST_COUNT);
		c->second_count = rc_cw_le32(p + RC_CW_CR_SECOND_COUNT);
		c->dist_spare = rc_cw_le32(p + RC_CW_CR_DIST_SPARE);
		c->prefer_read_element =
			rc_cw_le32(p + RC_CW_CR_PREFER_READ);
		c->task_priority = rc_cw_le32(p + RC_CW_CR_TASK_PRIO);
		{
			u32 i;

			for (i = 0; i < sizeof(c->private_bytes); i++)
				c->private_bytes[i] = p[RC_CW_CR_PRIVATE + i];
		}
		rc_cw_copy_ids(c->spare_id, &c->nr_spare,
			       p + RC_CW_CR_SPARE_LIST,
			       c->spare_count < 4 ? c->spare_count : 4);
		rc_cw_copy_ids(c->device_id, &c->nr_device,
			       p + RC_CW_CR_DEVICE_LIST, 1);
		break;
	}
	case RC_CW_DELETE: {
		struct rc_cfgwrite_delete *d = &out->u.delete;

		if (len < RC_CW_HDR_BYTES + RC_CW_DL_BYTES)
			return -EINVAL;

		d->all = rc_cw_le32(p + RC_CW_DL_ALL);
		d->device_count = rc_cw_le32(p + RC_CW_DL_DEVICE_COUNT);
		if (d->device_count > RC_CW_MAX_IDS)
			return -EINVAL;
		expect = RC_CW_DL_DEVICE_LIST +
			 d->device_count * RC_CW_ID_BYTES;
		if (len < RC_CW_HDR_BYTES + expect)
			return -EINVAL;
		rc_cw_copy_ids(d->device_id, &d->nr_device,
			       p + RC_CW_DL_DEVICE_LIST, d->device_count);
		break;
	}
	case RC_CW_TRANSFORM: {
		struct rc_cfgwrite_transform *t = &out->u.transform;

		expect = RC_CW_TR_BYTES;
		if (len < RC_CW_HDR_BYTES + expect)
			return -EINVAL;

		t->raid_type = rc_cw_le32(p + RC_CW_TR_RAID_TYPE);
		rc_cw_copy_id(t->logical_device, p + RC_CW_TR_LOGICAL_DEVICE);
		t->device_count = rc_cw_le32(p + RC_CW_TR_DEVICE_COUNT);
		t->spare_count = rc_cw_le32(p + RC_CW_TR_SPARE_COUNT);
		t->flags = rc_cw_le32(p + RC_CW_TR_FLAGS);
		t->size_mb = rc_cw_le32(p + RC_CW_TR_SIZE_MB);
		t->first_count = rc_cw_le32(p + RC_CW_TR_FIRST_COUNT);
		t->second_count = rc_cw_le32(p + RC_CW_TR_SECOND_COUNT);
		t->dist_spare = rc_cw_le32(p + RC_CW_TR_DIST_SPARE);
		t->prefer_read_element =
			rc_cw_le32(p + RC_CW_TR_PREFER_READ);
		t->task_priority = rc_cw_le32(p + RC_CW_TR_TASK_PRIO);
		rc_cw_copy_ids(t->spare_id, &t->nr_spare,
			       p + RC_CW_TR_SPARE_LIST,
			       t->spare_count < 4 ? t->spare_count : 4);
		rc_cw_copy_ids(t->device_id, &t->nr_device,
			       p + RC_CW_TR_DEVICE_LIST, 1);
		break;
	}
	default:
		return -EOPNOTSUPP;
	}

	/*
	 * ConfigWriteSizeInBytes is zero on every captured mutation, so the
	 * frame length cannot be taken as the payload length.  When a caller
	 * does fill it in, it must agree with the command's own size.
	 */
	if (out->hdr.config_write_size &&
	    out->hdr.config_write_size != expect)
		return -EINVAL;

	out->payload_bytes = expect;
	return 0;
}

#endif /* _RC_CFGWRITE_H_ */
