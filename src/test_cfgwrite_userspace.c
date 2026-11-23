// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — RC_CfgWrite decoder harness
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Builds mutation frames from the layout captured off rcadm 9.3.3-00302
 * (docs/REVERSE_ENGINEERING.md, "Captured mutation frames") and checks
 * that src/rc_cfgwrite.h reads every field back, rejects short/lying
 * frames, and leaves unknown commands to the caller.  Needs no kernel
 * headers: the decoder is header-only and dependency-free by design.
 *
 *   make test-cfgwrite
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#include "rc_cfgwrite.h"

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void put64(u8 *p, u64 v)
{
	put32(p, (u32)v);
	put32(p + 4, (u32)(v >> 32));
}

static void put_header(u8 *f, u32 cmd, u32 chan, u64 gen, u32 sync)
{
	memset(f, 0, 1536);
	put32(f + RC_CW_OFF_COMMAND, cmd);
	put32(f + RC_CW_OFF_CHANNEL, chan);
	put64(f + RC_CW_OFF_GENERATION, gen);
	put32(f + RC_CW_OFF_SYNC, sync);
	put32(f + RC_CW_OFF_MAJOR, 2);
	put32(f + RC_CW_OFF_MINOR, 1);
}

static void fill_id(u8 *p, u8 seed)
{
	u32 i;

	for (i = 0; i < RC_CW_ID_BYTES; i++)
		p[i] = (u8)(seed + i);
}

/* CREATE RAID 0, four members, -T -ss size. */
static void test_create(void)
{
	u8 f[1536];
	struct rc_cfgwrite r;
	u8 *p;
	int rc;

	put_header(f, RC_CW_CREATE, 0xdb0, 13, 0);
	p = f + RC_CW_HDR_BYTES;
	put32(p + RC_CW_CR_RAID_TYPE, 0x1BF6);
	put32(p + RC_CW_CR_DEVICE_COUNT, 4);
	put32(p + RC_CW_CR_SPARE_COUNT, 0);
	put32(p + RC_CW_CR_CACHE_TAG, 3);
	put32(p + RC_CW_CR_CACHE_FLAGS, 0xC0000000u);
	put32(p + RC_CW_CR_SIZE_MB, 15259608);
	put64(p + RC_CW_CR_PREFER_ID, 0x1122334455667788ull);
	put32(p + RC_CW_CR_FIRST_COUNT, 4);
	put32(p + RC_CW_CR_SECOND_COUNT, 1);
	fill_id(p + RC_CW_CR_DEVICE_LIST, 0x40);

	rc = rc_cfgwrite_decode(f, sizeof(f), &r);
	assert(rc == 0);
	assert(r.hdr.command == RC_CW_CREATE);
	assert(r.hdr.channel_id == 0xdb0);
	assert(r.hdr.generation == 13);
	assert(r.hdr.sync_command == 0);
	assert(r.payload_bytes == RC_CW_CR_BYTES);
	assert(r.u.create.raid_type == 0x1BF6);
	assert(r.u.create.device_count == 4);
	assert(r.u.create.cache_tag_size == 3);
	assert(r.u.create.cache_flags == 0xC0000000u);
	assert(r.u.create.size_mb == 15259608);
	assert(r.u.create.prefer_device_id == 0x1122334455667788ull);
	assert(r.u.create.first_count == 4);
	assert(r.u.create.second_count == 1);
	assert(r.u.create.nr_spare == 0);
	assert(r.u.create.nr_device == 1);
	assert(r.u.create.device_id[0][0] == 0x40);
	assert(r.u.create.device_id[0][15] == 0x4f);
	printf("create : type=0x%04x dev=%u %ux%u size=%u MiB id=%02x..%02x\n",
	       r.u.create.raid_type, r.u.create.device_count,
	       r.u.create.first_count, r.u.create.second_count,
	       r.u.create.size_mb, r.u.create.device_id[0][0],
	       r.u.create.device_id[0][15]);
}

/* DELETE the array's LD, exactly as rcadm sent it. */
static void test_delete(void)
{
	u8 f[1536];
	static const u8 ld[16] = {
		0x4e, 0x1f, 0x33, 0x06, 0x5b, 0x47, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	struct rc_cfgwrite r;
	u8 *p;
	int rc;

	put_header(f, RC_CW_DELETE, 0xfd0, 13, 1);
	p = f + RC_CW_HDR_BYTES;
	put32(p + RC_CW_DL_ALL, 0);
	put32(p + RC_CW_DL_DEVICE_COUNT, 1);
	memcpy(p + RC_CW_DL_DEVICE_LIST, ld, sizeof(ld));

	rc = rc_cfgwrite_decode(f, sizeof(f), &r);
	assert(rc == 0);
	assert(r.hdr.sync_command == 1);
	assert(r.payload_bytes == RC_CW_DL_BYTES);
	assert(r.u.delete.all == 0);
	assert(r.u.delete.device_count == 1);
	assert(r.u.delete.nr_device == 1);
	assert(memcmp(r.u.delete.device_id[0], ld, sizeof(ld)) == 0);
	printf("delete : all=%u dev=%u ld=%02x%02x%02x%02x%02x%02x%02x%02x\n",
	       r.u.delete.all, r.u.delete.device_count,
	       r.u.delete.device_id[0][0], r.u.delete.device_id[0][1],
	       r.u.delete.device_id[0][2], r.u.delete.device_id[0][3],
	       r.u.delete.device_id[0][4], r.u.delete.device_id[0][5],
	       r.u.delete.device_id[0][6], r.u.delete.device_id[0][7]);
}

/* TRANSFORM to RAID 5 over three members. */
static void test_transform(void)
{
	u8 f[1536];
	struct rc_cfgwrite r;
	u8 *p;
	int rc;

	put_header(f, RC_CW_TRANSFORM, 0x1080, 13, 0);
	p = f + RC_CW_HDR_BYTES;
	put32(p + RC_CW_TR_RAID_TYPE, 0x1BF5);
	fill_id(p + RC_CW_TR_LOGICAL_DEVICE, 0x10);
	put32(p + RC_CW_TR_DEVICE_COUNT, 3);
	put32(p + RC_CW_TR_SPARE_COUNT, 0);
	put32(p + RC_CW_TR_SIZE_MB, 15259608);
	put32(p + RC_CW_TR_FIRST_COUNT, 1);
	put32(p + RC_CW_TR_SECOND_COUNT, 3);
	fill_id(p + RC_CW_TR_DEVICE_LIST, 0x80);

	rc = rc_cfgwrite_decode(f, sizeof(f), &r);
	assert(rc == 0);
	assert(r.payload_bytes == RC_CW_TR_BYTES);
	assert(r.u.transform.raid_type == 0x1BF5);
	assert(r.u.transform.device_count == 3);
	assert(r.u.transform.first_count == 1);
	assert(r.u.transform.second_count == 3);
	assert(r.u.transform.size_mb == 15259608);
	assert(r.u.transform.logical_device[0] == 0x10);
	assert(r.u.transform.nr_spare == 0);
	assert(r.u.transform.nr_device == 1);
	assert(r.u.transform.device_id[0][0] == 0x80);
	printf("xform  : type=0x%04x dev=%u %ux%u size=%u MiB ld=%02x\n",
	       r.u.transform.raid_type, r.u.transform.device_count,
	       r.u.transform.first_count, r.u.transform.second_count,
	       r.u.transform.size_mb, r.u.transform.logical_device[0]);
}

static void test_rejections(void)
{
	u8 f[1536];
	struct rc_cfgwrite r;

	/* Frame shorter than the header. */
	put_header(f, RC_CW_CREATE, 0xdb0, 13, 0);
	assert(rc_cfgwrite_decode(f, RC_CW_HDR_BYTES - 1, &r) == -EINVAL);

	/* Frame shorter than the command's payload. */
	assert(rc_cfgwrite_decode(f, RC_CW_HDR_BYTES + RC_CW_CR_BYTES - 1,
				  &r) == -EINVAL);
	assert(rc_cfgwrite_decode(f, RC_CW_HDR_BYTES + RC_CW_CR_BYTES,
				  &r) == 0);

	/*
	 * ConfigWriteSizeInBytes is zero on the wire; when a caller fills
	 * it in it must agree with the command, not with the frame length.
	 */
	put32(f + RC_CW_OFF_SIZE, 100);
	assert(rc_cfgwrite_decode(f, sizeof(f), &r) == -EINVAL);
	put32(f + RC_CW_OFF_SIZE, RC_CW_CR_BYTES);
	assert(rc_cfgwrite_decode(f, sizeof(f), &r) == 0);

	/* Commands with no captured layout: header still decoded. */
	put_header(f, 34, 0x1080, 13, 0);
	assert(rc_cfgwrite_decode(f, sizeof(f), &r) == -EOPNOTSUPP);
	assert(r.hdr.command == 34);
	assert(r.hdr.channel_id == 0x1080);

	/* DELETE sizes itself from DeviceCount. */
	put_header(f, RC_CW_DELETE, 0xfd0, 13, 1);
	put32(f + RC_CW_HDR_BYTES + RC_CW_DL_DEVICE_COUNT, 2);
	assert(rc_cfgwrite_decode(f, sizeof(f), &r) == 0);
	assert(r.u.delete.nr_device == 2);
	assert(r.payload_bytes == RC_CW_DL_DEVICE_LIST + 2 * RC_CW_ID_BYTES);

	/* ... and a frame that cannot hold the ids it claims is refused. */
	assert(rc_cfgwrite_decode(f, RC_CW_HDR_BYTES + RC_CW_DL_BYTES,
				  &r) == -EINVAL);

	/* An LD count beyond the retained table is refused, not truncated. */
	put32(f + RC_CW_HDR_BYTES + RC_CW_DL_DEVICE_COUNT, RC_CW_MAX_IDS + 1);
	assert(rc_cfgwrite_decode(f, sizeof(f), &r) == -EINVAL);

	printf("reject : short/lying frames refused\n");
}

int main(void)
{
	test_create();
	test_delete();
	test_transform();
	test_rejections();
	printf("RC_CfgWrite decoder: ok\n");
	return 0;
}
