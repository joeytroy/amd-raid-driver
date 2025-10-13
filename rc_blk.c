#include "rc_blk.h"
#include "rc_linux.h"
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/bio.h>
#include <linux/highmem.h>

static blk_status_t rc_queue_rq(struct blk_mq_hw_ctx *hctx,
                                const struct blk_mq_queue_data *bd)
{
    struct request *req = bd->rq;
    struct bio *bio;
    struct bio_vec bvec;
    struct bvec_iter iter;
    void *kaddr;
    
    blk_mq_start_request(req);
    
    /* Process each bio in the request */
    __rq_for_each_bio(bio, req) {
        bio_for_each_segment(bvec, bio, iter) {
            kaddr = kmap_atomic(bvec.bv_page);
            
            if (bio_data_dir(bio) == READ) {
                /* For reads, return zeros */
                memset(kaddr + bvec.bv_offset, 0, bvec.bv_len);
            }
            /* For writes, just ignore the data */
            
            kunmap_atomic(kaddr);
        }
    }
    
    blk_mq_end_request(req, BLK_STS_OK);
    return BLK_STS_OK;
}

static const struct blk_mq_ops rc_mq_ops = {
    .queue_rq = rc_queue_rq,
};

static const struct block_device_operations rc_bdev_ops = {
    .owner = THIS_MODULE,
};

int rc_blk_create_disk(struct rc_raid_array *a, int major)
{
    int ret;
    sector_t sectors;

    if (!a || !a->size_bytes) {
        pr_err("rcraid: invalid array/size\n");
        return -EINVAL;
    }

    memset(&a->tag_set, 0, sizeof(a->tag_set));
    a->tag_set.ops          = &rc_mq_ops;
    a->tag_set.nr_hw_queues = 1;
    a->tag_set.queue_depth  = 128;
    a->tag_set.numa_node    = NUMA_NO_NODE;
    a->tag_set.flags        = 0; /* Remove BLK_MQ_F_SHOULD_MERGE for compatibility */

    ret = blk_mq_alloc_tag_set(&a->tag_set);
    if (ret)
        return ret;

    a->disk = blk_mq_alloc_disk(&a->tag_set, NULL, a);
    if (IS_ERR(a->disk)) {
        ret = PTR_ERR(a->disk);
        a->disk = NULL;
        blk_mq_free_tag_set(&a->tag_set);
        return ret;
    }

    a->disk->major        = major;
    a->disk->first_minor  = a->index;   /* one minor per array for now */
    a->disk->minors       = 1;
    a->disk->fops         = &rc_bdev_ops;
    a->disk->private_data = a;

    snprintf(a->disk->disk_name, DISK_NAME_LEN, "rcraid%d", a->index);

    /* queue tunables BEFORE add_disk() - skip for now */

    sectors = div_u64(a->size_bytes, 512);
    set_capacity(a->disk, sectors);

    ret = add_disk(a->disk);
    if (ret) {
        pr_err("rcraid: add_disk(%s) failed: %d\n", a->disk->disk_name, ret);
        put_disk(a->disk);
        a->disk = NULL;
        blk_mq_free_tag_set(&a->tag_set);
        return ret;
    }

    pr_info("rcraid: added %s size=%llu MiB\n",
            a->disk->disk_name, div_u64(a->size_bytes, 1024*1024));
    return 0;
}

void rc_blk_destroy_disk(struct rc_raid_array *a)
{
    if (!a || !a->disk)
        return;

    del_gendisk(a->disk);
    put_disk(a->disk);
    blk_mq_free_tag_set(&a->tag_set);
}
