#pragma once
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/genhd.h>
#include <linux/version.h>

struct rc_array; /* forward-declare your array struct */

int rc_blk_create_disk(struct rc_array *a, int major);
void rc_blk_destroy_disk(struct rc_array *a);
