// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — RAIDCore metadata stamping tool
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Stamps a complete image set (per-member RC_MetaData at LBA 0x5000, the
 * RC_ConfigCommit block at 0x5001, and a config generation in the ring at
 * 0x5800) using the driver's own encoder, src/rc_meta_build.h.  This is the
 * userspace stand-in for the kernel-side writer that rcadm CREATE needs: it
 * proves the C encoder's output — not just mkmeta.py's — assembles.
 *
 *   make stamp-tool
 *   ./.metabuild-test/rcstamp --level raid0 member0.img member1.img
 *   ./.metabuild-test/rcstamp --level raid10 m0.img m1.img m2.img m3.img
 *
 * RAID10 is encoded the way firmware and the driver agree on: FirstCount =
 * columns (members / 2), SecondCount = 2, and members are in column order —
 * slots 2c and 2c+1 are column c's mirror pair (rc_volume_phys_for_member).
 *
 * Prints the same key=value lines mkmeta.py does, so the QEMU rig can drive
 * either generator with --writer encoder.
 *
 * No kernel headers: the encoder is header-only and pure.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>		/* fsync() */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#include "rc_meta_build.h"

#define STAMP_MAX_MEMBERS	16u
#define STAMP_METADATA_LBA	0x5000u	/* commit LBA - 1 */
/* Generation slots: the decoy sits at the ring head, the active
 * generation 16 sectors in — the layout mkmeta.py writes and the rig
 * exercises with a stale-config decoy. */
#define STAMP_DECOY_SECTOR	0u
#define STAMP_ACTIVE_SECTOR	16u
#define STAMP_ACTIVE_TS		0x5243544553540A11ull
#define STAMP_DECOY_TS		0x5243544553540DEAull
#define STAMP_GEN_SEQ		2u

/* Same encoding as mkmeta.py / rc_volume_chunk_sectors_for(). */
static const u32 chunk_sectors_for[4] = { 128, 128, 256, 512 };

static int write_at(FILE *f, u64 lba, const void *buf, size_t len)
{
	if (fseeko(f, (off_t)lba * RC_MB_SECTOR, SEEK_SET)) {
		perror("fseeko");
		return -1;
	}
	if (fwrite(buf, 1, len, f) != len) {
		perror("fwrite");
		return -1;
	}
	return 0;
}

static int file_sectors(const char *path, u64 *out)
{
	struct stat st;

	if (stat(path, &st)) {
		perror(path);
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "%s: only regular image files are supported\n",
			path);
		return -1;
	}
	if (st.st_size <= 0 || st.st_size % RC_MB_SECTOR) {
		fprintf(stderr, "%s: size is not sector-aligned\n", path);
		return -1;
	}
	*out = (u64)st.st_size / RC_MB_SECTOR;
	return 0;
}

int main(int argc, char **argv)
{
	const char *level = "raid0";
	u32 chunk_index = 3, chunk_sectors;
	u64 ids[STAMP_MAX_MEMBERS];
	u64 *sectors = NULL, min_sectors, user, capacity, decoy_capacity;
	u8 *records = NULL, *gen = NULL, *decoy = NULL, *commit = NULL;
	u8 *md = NULL;
	u32 raw_len, ld_len, gen_len, decoy_len;
	const char *images[STAMP_MAX_MEMBERS];
	const char *decoy_level;
	u32 nr = 0, first, second, decoy_first, decoy_second, i;
	int argi, ret = 1;

	for (argi = 1; argi < argc; argi++) {
		if (!strcmp(argv[argi], "--level") && argi + 1 < argc) {
			level = argv[++argi];
		} else if (!strcmp(argv[argi], "--chunk-index") &&
			   argi + 1 < argc) {
			chunk_index = (u32)strtoul(argv[++argi], NULL, 0);
		} else if (argv[argi][0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argv[argi]);
			return 2;
		} else if (nr < STAMP_MAX_MEMBERS) {
			images[nr++] = argv[argi];
		} else {
			fprintf(stderr, "too many members (max %u)\n",
				STAMP_MAX_MEMBERS);
			return 2;
		}
	}

	if (strcmp(level, "raid0") && strcmp(level, "raid1") &&
	    strcmp(level, "raid10")) {
		fprintf(stderr, "level %s: the driver has no dispatch for it\n",
			level);
		return 2;
	}
	if (nr < 2) {
		fprintf(stderr, "need at least 2 member images\n");
		return 2;
	}
	if (!strcmp(level, "raid1") && nr != 2) {
		fprintf(stderr, "raid1 wants exactly 2 members\n");
		return 2;
	}
	/* A 2-member raid10 record is the (1,2) RAID1 encoding, so 10
	 * starts at two columns.  Odd counts cannot form whole pairs. */
	if (!strcmp(level, "raid10") && (nr < 4 || (nr & 1))) {
		fprintf(stderr, "raid10 wants an even number of members, at least 4\n");
		return 2;
	}
	if (chunk_index > 3) {
		fprintf(stderr, "--chunk-index must be 0-3\n");
		return 2;
	}
	chunk_sectors = chunk_sectors_for[chunk_index];

	sectors = calloc(nr, sizeof(*sectors));
	records = calloc(2, RC_MB_LD_MAX);
	gen = calloc(64, RC_MB_SECTOR);
	decoy = calloc(16, RC_MB_SECTOR);
	commit = calloc(1, RC_MB_SECTOR);
	md = calloc(1, RC_MB_SECTOR);
	if (!sectors || !records || !gen || !decoy || !commit || !md) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}

	min_sectors = ~0ull;
	for (i = 0; i < nr; i++) {
		if (file_sectors(images[i], &sectors[i]))
			goto out;
		if (sectors[i] < min_sectors)
			min_sectors = sectors[i];
		ids[i] = 0x5243544553540000ull + i;	/* deterministic */
	}

	user = rc_mb_user_sectors(min_sectors, chunk_sectors);
	if (!user) {
		fprintf(stderr, "images too small for a member metadata reserve\n");
		goto out;
	}
	if (!strcmp(level, "raid0")) {
		/* Striped across every member: capacity adds up. */
		first = nr;
		second = 1;
		capacity = user * nr;
		decoy_level = "raid1";
		decoy_first = 1;
		decoy_second = 2;
		decoy_capacity = user;
	} else if (!strcmp(level, "raid1")) {
		/* Mirrored: capacity is one member's. */
		first = 1;
		second = 2;
		capacity = user;
		decoy_level = "raid0";
		decoy_first = nr;
		decoy_second = 1;
		decoy_capacity = user * nr;
	} else {
		/* RAID10: FirstCount columns x SecondCount mirror copies.
		 * Members are positional: 2c and 2c+1 hold column c, so
		 * capacity is the columns' worth, not every member. */
		first = nr / 2;
		second = 2;
		capacity = user * (nr / 2);
		decoy_level = "raid0";
		decoy_first = nr;
		decoy_second = 1;
		decoy_capacity = user * nr;
	}

	/* Active generation: the raw-disk LD first (firmware does), then the
	 * array LD the driver must assemble. */
	raw_len = rc_mb_raw_ld_record(records, RC_MB_LD_MAX, ids[0],
				      min_sectors);
	ld_len = rc_mb_ld_record(records + raw_len, RC_MB_LD_MAX,
				 RC_MB_DEVTYPE_VOLUME, first, second, nr,
				 ids, capacity, user, chunk_index);
	if (!raw_len || !ld_len) {
		fprintf(stderr, "LD record build failed\n");
		goto out;
	}
	gen_len = rc_mb_generation(gen, 64 * RC_MB_SECTOR, STAMP_ACTIVE_TS,
				   records, raw_len + ld_len);
	if (!gen_len) {
		fprintf(stderr, "active generation build failed\n");
		goto out;
	}

	/* Decoy at the ring head: a dead config for the opposite level with
	 * the same device IDs, so a first-match parser assembles the wrong
	 * volume.  Not referenced by the commit block. */
	ld_len = rc_mb_ld_record(records, RC_MB_LD_MAX, RC_MB_DEVTYPE_VOLUME,
				 decoy_first, decoy_second, nr,
				 ids, decoy_capacity, user, chunk_index);
	if (!ld_len) {
		fprintf(stderr, "decoy record build failed\n");
		goto out;
	}
	decoy_len = rc_mb_generation(decoy, 16 * RC_MB_SECTOR,
				     STAMP_DECOY_TS, records, ld_len);
	if (!decoy_len || decoy_len > STAMP_ACTIVE_SECTOR * RC_MB_SECTOR) {
		fprintf(stderr, "decoy generation does not fit its slot\n");
		goto out;
	}

	rc_mb_commit_block(commit, RC_MB_RING_LBA + STAMP_ACTIVE_SECTOR,
			   gen_len, STAMP_ACTIVE_TS, STAMP_GEN_SEQ);

	for (i = 0; i < nr; i++) {
		FILE *f = fopen(images[i], "r+b");

		if (!f) {
			perror(images[i]);
			goto out;
		}
		rc_mb_metadata_block(md, ids[i]);
		if (write_at(f, STAMP_METADATA_LBA, md, RC_MB_SECTOR) ||
		    write_at(f, RC_MB_COMMIT_LBA, commit, RC_MB_SECTOR) ||
		    write_at(f, RC_MB_RING_LBA + STAMP_DECOY_SECTOR,
			     decoy, decoy_len) ||
		    write_at(f, RC_MB_RING_LBA + STAMP_ACTIVE_SECTOR,
			     gen, gen_len) || fflush(f) || fsync(fileno(f))) {
			fclose(f);
			goto out;
		}
		if (fclose(f)) {
			perror(images[i]);
			goto out;
		}
	}

	printf("level=%s members=%u chunk_sectors=%u user_size=%llu "
	       "decoy_level=%s\n", level, nr, chunk_sectors,
	       (unsigned long long)user, decoy_level);
	printf("capacity_sectors=%llu\n", (unsigned long long)capacity);
	printf("userdata_offset_sectors=%u\n", RC_MB_USERDATA_LBA);
	ret = 0;
out:
	free(sectors);
	free(records);
	free(gen);
	free(decoy);
	free(commit);
	free(md);
	return ret;
}
