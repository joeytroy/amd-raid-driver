#include "rc_blk.h"
#include "rc_linux.h"
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/highmem.h>

#define RC_SECTORS_PER_PAGE (PAGE_SIZE >> 9)          /* sectors cached per page */
#define RC_SECTOR_OFFSET_MASK (RC_SECTORS_PER_PAGE - 1)

struct rc_page_bucket {
    struct list_head list;
    sector_t sector_base;
    u8 *data;
};

static struct rc_page_bucket *rc_lookup_bucket(struct rc_raid_array *array,
                                               sector_t sector, bool create)
{
    const sector_t base = sector & ~(sector_t)RC_SECTOR_OFFSET_MASK;
    struct rc_page_bucket *found;
    struct rc_page_bucket *new_bucket;
    unsigned long flags;

    spin_lock_irqsave(&array->page_lock, flags);
    list_for_each_entry(found, &array->page_list, list) {
        if (found->sector_base == base) {
            spin_unlock_irqrestore(&array->page_lock, flags);
            return found;
        }
    }
    spin_unlock_irqrestore(&array->page_lock, flags);

    if (!create)
        return NULL;

    new_bucket = kzalloc(sizeof(*new_bucket), GFP_NOIO);
    if (!new_bucket)
        return ERR_PTR(-ENOMEM);

    new_bucket->data = kzalloc(PAGE_SIZE, GFP_NOIO);
    if (!new_bucket->data) {
        kfree(new_bucket);
        return ERR_PTR(-ENOMEM);
    }
    new_bucket->sector_base = base;

    spin_lock_irqsave(&array->page_lock, flags);
    list_for_each_entry(found, &array->page_list, list) {
        if (found->sector_base == base) {
            spin_unlock_irqrestore(&array->page_lock, flags);
            kfree(new_bucket->data);
            kfree(new_bucket);
            return found;
        }
    }
    list_add(&new_bucket->list, &array->page_list);
    spin_unlock_irqrestore(&array->page_lock, flags);

    return new_bucket;
}

static int rc_transfer_data(struct rc_raid_array *array, sector_t sector,
                            u8 *buf, unsigned int len, bool write)
{
    while (len) {
        unsigned int offset = (sector & RC_SECTOR_OFFSET_MASK) << 9;
        unsigned int chunk = min_t(unsigned int, PAGE_SIZE - offset, len);
        struct rc_page_bucket *bucket;

        bucket = rc_lookup_bucket(array, sector, write);
        if (IS_ERR(bucket))
            return PTR_ERR(bucket);

        if (write) {
            memcpy(bucket->data + offset, buf, chunk);
        } else if (bucket) {
            memcpy(buf, bucket->data + offset, chunk);
        } else {
            memset(buf, 0, chunk);
        }

        buf += chunk;
        sector += chunk >> 9;
        len -= chunk;
    }

    return 0;
}

static int rc_zero_range(struct rc_raid_array *array, sector_t sector,
                         unsigned int len)
{
    while (len) {
        unsigned int offset = (sector & RC_SECTOR_OFFSET_MASK) << 9;
        unsigned int chunk = min_t(unsigned int, PAGE_SIZE - offset, len);
        struct rc_page_bucket *bucket;

        bucket = rc_lookup_bucket(array, sector, false);
        if (bucket)
            memset(bucket->data + offset, 0, chunk);

        sector += chunk >> 9;
        len -= chunk;
    }

    return 0;
}

static blk_status_t rc_status_from_errno(int err)
{
    if (!err)
        return BLK_STS_OK;
    if (err == -ENOMEM)
        return BLK_STS_RESOURCE;
    return BLK_STS_IOERR;
}

static blk_status_t rc_handle_rw(struct rc_raid_array *array, struct request *rq)
{
    struct req_iterator iter;
    struct bio_vec bvec;
    sector_t sector = blk_rq_pos(rq);
    bool write = op_is_write(req_op(rq));
    int ret;

    rq_for_each_segment(bvec, rq, iter) {
        void *kaddr = kmap_local_page(bvec.bv_page);
        u8 *buf = (u8 *)kaddr + bvec.bv_offset;

        ret = rc_transfer_data(array, sector, buf, bvec.bv_len, write);
        kunmap_local(kaddr);
        if (ret)
            return rc_status_from_errno(ret);

        sector += bvec.bv_len >> 9;
    }

    return BLK_STS_OK;
}

static blk_status_t rc_handle_write_zeroes(struct rc_raid_array *array,
                                           struct request *rq)
{
    int ret = rc_zero_range(array, blk_rq_pos(rq), blk_rq_bytes(rq));

    return rc_status_from_errno(ret);
}

static blk_status_t rc_handle_discard(struct rc_raid_array *array,
                                      struct request *rq)
{
    /* Treat discard the same as zeroing for now. */
    int ret = rc_zero_range(array, blk_rq_pos(rq), blk_rq_bytes(rq));

    return rc_status_from_errno(ret);
}

static blk_status_t rc_queue_rq(struct blk_mq_hw_ctx *hctx,
                                const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct rc_raid_array *array = rq->q ? rq->q->queuedata : NULL;
    blk_status_t status = BLK_STS_OK;

    blk_mq_start_request(rq);

    if (!array) {
        rc_printk(RC_WARN, "rc_queue_rq: missing array context for op=%u\n", req_op(rq));
        blk_mq_end_request(rq, BLK_STS_IOERR);
        return BLK_STS_OK;
    }

    switch (req_op(rq)) {
    case REQ_OP_READ:
    case REQ_OP_WRITE:
        status = rc_handle_rw(array, rq);
        break;
    case REQ_OP_FLUSH:
        status = BLK_STS_OK;
        break;
    case REQ_OP_WRITE_ZEROES:
        status = rc_handle_write_zeroes(array, rq);
        break;
    case REQ_OP_DISCARD:
        status = rc_handle_discard(array, rq);
        break;
    default:
        status = BLK_STS_NOTSUPP;
        break;
    }

    blk_mq_end_request(rq, status);
    if (status != BLK_STS_OK && status != BLK_STS_NOTSUPP)
        rc_printk(RC_ERROR, "rc_queue_rq: op=%u status=%d sector=%llu len=%u\n",
                  req_op(rq), status, (unsigned long long)blk_rq_pos(rq),
                  blk_rq_bytes(rq));
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    a->disk = blk_mq_alloc_disk(&a->tag_set, a);
#else
    a->disk = blk_mq_alloc_disk(&a->tag_set, NULL, a);
#endif
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
    a->disk->queue->queuedata = a;
    a->queue = a->disk->queue;

    snprintf(a->disk->disk_name, DISK_NAME_LEN, "rcraid%d", a->index);

    /* queue tunables BEFORE add_disk() - skip for now */

    sectors = div_u64(a->size_bytes, 512);
    set_capacity(a->disk, sectors);

    ret = add_disk(a->disk);
    if (ret) {
        pr_err("rcraid: add_disk(%s) failed: %d\n", a->disk->disk_name, ret);
        put_disk(a->disk);
        a->disk = NULL;
        a->queue = NULL;
        blk_mq_free_tag_set(&a->tag_set);
        return ret;
    }

    pr_info("rcraid: added %s size=%llu MiB\n",
            a->disk->disk_name, div_u64(a->size_bytes, 1024*1024));
    return 0;
}

void rc_blk_destroy_disk(struct rc_raid_array *a)
{
    unsigned long flags;

    if (!a || !a->disk)
        return;

    del_gendisk(a->disk);
    put_disk(a->disk);
    blk_mq_free_tag_set(&a->tag_set);
    a->disk = NULL;
    a->queue = NULL;

    spin_lock_irqsave(&a->page_lock, flags);
    while (!list_empty(&a->page_list)) {
        struct rc_page_bucket *bucket;

        bucket = list_first_entry(&a->page_list, struct rc_page_bucket, list);
        list_del(&bucket->list);
        spin_unlock_irqrestore(&a->page_lock, flags);

        kfree(bucket->data);
        kfree(bucket);

        spin_lock_irqsave(&a->page_lock, flags);
    }
    spin_unlock_irqrestore(&a->page_lock, flags);
    INIT_LIST_HEAD(&a->page_list);
}
