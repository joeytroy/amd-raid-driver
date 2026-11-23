// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — RAIDCore metadata encoder harness
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Emits the images src/rc_meta_build.h builds for two synthetic arrays
 * (RAID 0 over four members, RAID 1 over two) so
 * scripts/qemu-test/check-metabuild.py can diff them byte for byte
 * against scripts/qemu-test/mkmeta.py, the generator the QEMU rig boots
 * arrays from.  Any layout drift between the two encoders fails the
 * build.
 *
 *   make test-metabuild
 *
 * No kernel headers: the encoder is header-only and pure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#include "rc_meta_build.h"

/* Must match the constants in check-metabuild.py. */
#define TEST_DISK_SECTORS	0x200000ull	/* 1 GiB members */
#define TEST_SUB_VENDOR		0x1987u
#define TEST_SUB_DEVICE		0x5012u
#define TEST_PCILOC_BASE	0x0300u
#define TEST_GEN_TS		0x5243544553540A11ull
#define TEST_GEN_SEQ		2u
#define TEST_CHUNK_INDEX	3u		/* 256 KiB stripes */
#define TEST_CHUNK_SECTORS	512u
#define TEST_ID_BASE		0x5243544553540000ull

static int write_file(const char *dir, const char *name, const void *buf,
		      size_t len)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return -1;
	}
	if (fwrite(buf, 1, len, f) != len) {
		perror(path);
		fclose(f);
		return -1;
	}
	return fclose(f) ? -1 : 0;
}

static int emit_array(const char *dir, const char *tag, u32 members,
		      u32 first, u32 second)
{
	struct rc_mb_member_meta metas[16];
	u64 ids[16], volume_id;
	u64 user, capacity;
	u8 *records, *gen, *commit, *md;
	u32 rec_len, gen_len, i;
	int ret = 0;

	for (i = 0; i < members; i++) {
		ids[i] = TEST_ID_BASE + i;
		metas[i].device_id = ids[i];
		metas[i].disk_sectors = TEST_DISK_SECTORS;
		metas[i].sub_vid = TEST_SUB_VENDOR;
		metas[i].sub_dev = TEST_SUB_DEVICE;
		metas[i].pci_loc = TEST_PCILOC_BASE + i;
	}

	user = rc_mb_user_sectors(TEST_DISK_SECTORS, TEST_CHUNK_SECTORS);
	capacity = (first == 1 && second == 2) ? user : user * members;
	if (!user) {
		fprintf(stderr, "member too small\n");
		return -1;
	}

	records = calloc(1, 4096);
	gen = calloc(1, RC_MB_SECTOR * 64);
	commit = calloc(1, RC_MB_SECTOR);
	md = calloc(1, RC_MB_SECTOR);
	if (!records || !gen || !commit || !md) {
		ret = -1;
		goto out;
	}

	volume_id = (ids[0] ^ (ids[1] << 1)) | 1ULL;
	rec_len = rc_mb_records(records, 4096, RC_MB_DEVTYPE_VOLUME, metas,
				members, first, second, capacity, user,
				TEST_CHUNK_INDEX, volume_id);
	if (!rec_len) {
		fprintf(stderr, "record build failed\n");
		ret = -1;
		goto out;
	}

	gen_len = rc_mb_generation(gen, RC_MB_SECTOR * 64, TEST_GEN_TS,
				   records, rec_len);
	if (!gen_len) {
		fprintf(stderr, "generation build failed\n");
		ret = -1;
		goto out;
	}

	rc_mb_commit_block(commit, RC_MB_RING_LBA + 16, gen_len, TEST_GEN_TS,
			   TEST_GEN_SEQ);

	for (i = 0; i < members; i++) {
		char name[64];

		rc_mb_metadata_block(md, ids[i]);
		snprintf(name, sizeof(name), "md_%s_%u.bin", tag, i);
		if (write_file(dir, name, md, RC_MB_SECTOR)) {
			ret = -1;
			goto out;
		}
	}

	{
		char name[64];

		snprintf(name, sizeof(name), "gen_%s.bin", tag);
		if (write_file(dir, name, gen, gen_len)) {
			ret = -1;
			goto out;
		}
		snprintf(name, sizeof(name), "commit_%s.bin", tag);
		if (write_file(dir, name, commit, RC_MB_SECTOR)) {
			ret = -1;
			goto out;
		}
	}

	printf("%s: members=%u %ux%u user_sectors=%llu capacity=%llu "
	       "gen_bytes=%u\n", tag, members, first, second,
	       (unsigned long long)user, (unsigned long long)capacity,
	       gen_len);

out:
	free(records);
	free(gen);
	free(commit);
	free(md);
	return ret;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
		return 2;
	}
	if (emit_array(argv[1], "raid0", 4, 4, 1))
		return 1;
	if (emit_array(argv[1], "raid1", 2, 1, 2))
		return 1;
	return 0;
}
