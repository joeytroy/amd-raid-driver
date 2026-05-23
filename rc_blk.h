// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — blk-mq scaffolding header
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * See rc_blk.c for the (legacy) implementation notes.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See RE_METHODOLOGY.md.
 */

#pragma once
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/version.h>

struct rc_raid_array; /* forward-declare your array struct */

int rc_blk_create_disk(struct rc_raid_array *a, int major);
void rc_blk_destroy_disk(struct rc_raid_array *a);
