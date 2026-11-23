/**
 * ==============================================================================
 * UNIFIED PROTOTYPES FOR ASYNC WORKER, PARITY MATH CORES, AND NESTED GEOMETRY
 * MODULE PATH: /home/chazz/amd-raid-driver/src/patch_prototypes.h
 * ==============================================================================
 */

#ifndef PATCH_PROTOTYPES_H
#define PATCH_PROTOTYPES_H

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/types.h>

/* ------------------------------------------------------------------------------
 * Asynchronous mirror completion engine (patch_async_worker.c)
 *
 * Contract in one paragraph: rc_amd_queue_async_completion() registers a
 * parent bio, the caller dispatches N member bios and reports each one
 * through rc_amd_mirror_complete[_member](), and the parent is ended by the
 * "kamd_async_worker" kthread once the last member has reported.  Every one
 * of those entry points is callable from a hardware completion context, so
 * none of them ever sleeps.  The parent bio is therefore never ended by the
 * caller.
 * ------------------------------------------------------------------------------
 */

struct rc_mirror_context;

/* Engine telemetry, copied out by rc_amd_get_async_stats(). */
struct rc_async_stats {
	atomic64_t	dispatched;	/* parents registered              */
	atomic64_t	completed;	/* parent endio actually issued    */
	atomic64_t	mirror_ok;	/* of those, ended BLK_STS_OK       */
	atomic64_t	mirror_errors;	/* ended with a terminal status    */
	atomic64_t	member_errors;	/* member reports with non-OK     */
	atomic64_t	faults;		/* submit-path allocations failed  */
	atomic64_t	resource_injections;/* BLK_STS_RESOURCE injected      */
	atomic64_t	rejected;	/* refused (unload / bad arguments) */
	atomic64_t	stale_reports;	/* duplicate member reports        */
	atomic64_t	media_faults;	/* any non-OK member status        */
	atomic64_t	drain_forced;	/* contexts failed at unload       */
	atomic64_t	resync_requests;
	atomic64_t	resync_done;
	atomic64_t	resync_failed;
	atomic64_t	resync_cancelled;
};

/* Rebuild state, also visible as an amd_ctl0 status mask. */
enum rc_resync_state {
	RC_RESYNC_IDLE = 0,
	RC_RESYNC_PENDING,
	RC_RESYNC_RUNNING,
	RC_RESYNC_DONE,
	RC_RESYNC_FAILED,
	RC_RESYNC_CANCELLED,
};

/* Snapshot of rebuild progress; filled by rc_amd_get_resync_progress(). */
struct rc_resync_progress {
	u64	start_sector;
	u64	end_sector;
	u64	cursor;
	u64	chunks;
	u64	bytes;
	int	state;		/* enum rc_resync_state */
	int	result;		/* negative errno, or 0 */
	int	target_member;
};

/*
 * Media-layer hook for the rebuild pump.  Called from the resync thread
 * (sleepable context) with @sector already translated through the nested
 * RAID 10 matrix when a geometry is configured.
 *
 * It must ISSUE the copy and return: it must not wait for the member's
 * completion, and it must not sleep on hardware.  In-flight copies are
 * bounded by rc_resync_outstanding_fn instead, which the resync thread
 * polls (with a sleep) so it never outruns the media layer and never
 * spins.
 */
typedef int (*rc_resync_chunk_fn)(void *cookie, int source_member,
				  int target_member, u64 sector,
				  u32 nr_sectors);

/* Number of chunk copies this media layer still has outstanding. */
typedef u32 (*rc_resync_outstanding_fn)(void *cookie);

/* Asynchronous Worker Hooks */
struct rc_mirror_context *rc_amd_queue_async_completion(struct bio *parent_bio,
							 int total_mirrors,
							 int status_flag);
int rc_amd_mirror_complete(struct rc_mirror_context *ctx, blk_status_t status);
int rc_amd_mirror_complete_member(struct rc_mirror_context *ctx, int member, blk_status_t status);
int rc_amd_mirror_abort(struct rc_mirror_context *ctx, blk_status_t status);
int rc_amd_inflight_contexts(void);

/* Rebuild pump + member fault hooks (amd_ctl0 control keys) */
int rc_amd_resync_request(u64 start_sector, u64 end_sector, u32 chunk_sectors,
			  int source_member, int target_member);
void rc_amd_resync_cancel(void);
int rc_amd_get_resync_progress(struct rc_resync_progress *out);
void rc_amd_set_resync_io(rc_resync_chunk_fn chunk_fn,
			  rc_resync_outstanding_fn outstanding_fn,
			  void *cookie, u32 window);
void rc_amd_set_resync_geometry(int num_drives, u32 chunk_sectors);
void rc_amd_resync_kick(void);
int rc_amd_fail_member(int member);
int rc_amd_reinstate_member(int member);
bool rc_amd_member_is_failed(int member);
int rc_amd_failed_member_count(void);

/* Status/telemetry */
u32 rc_amd_get_status(void);
void rc_amd_get_async_stats(struct rc_async_stats *out);

int rc_amd_init_async_subsystem(void);
void rc_amd_exit_async_subsystem(void);

/* RAID 5/6 Parity Engine Hooks */
void rc_amd_generate_raid5_parity(uint8_t **data_buffers, uint8_t *parity_buffer, size_t block_len, int num_drives);
void rc_amd_generate_raid6_parity(uint8_t **data_buffers, uint8_t *p_buffer, uint8_t *q_buffer, size_t block_len, int num_drives);

/* RAID 10 Nested Geometry Matrix Hooks */
u64 rc_amd_map_nested_raid10(u64 sector_lba, u32 chunk_sectors, int *target_member, int num_drives);

void rc_amd_route_io_nested_raid10(u64 *lba, int *mbr, u32 chunk_sectors, int num_drives);
blk_status_t rc_amd_handle_transient_retry(struct bio *bio, int retry_count, int max_retries, int device_status);

/* Live-volume control surface exposed by /dev/amd_ctl0.  These read and
 * mutate rc_nvme.c's assembled-volume state, not the async mirror engine. */
struct AmdHardwarePayload;
int rc_volume_get_status(u32 *mask);
int rc_volume_get_inventory(struct AmdHardwarePayload *out);
int rc_volume_fail_member_slot(int slot);
int rc_volume_request_resync(void);

/* Indexed CREATE for /dev/amd_ctl0.  Indices are the disk_index values
 * rc_volume_get_inventory() publishes; the driver resolves them itself. */
int rc_volume_create_array_indexed(u32 level, const u32 *indices,
				   u32 count, u32 size_mb);
int rc_volume_delete_array(void);

int rc_amd_init_ioctl_bridge(void);
void rc_amd_exit_ioctl_bridge(void);

/* Multi-Format Distributed Parity Layout Geometry Extensions */
u64 rc_amd_map_distributed_raid5(u64 sector_lba, u32 chunk_sectors, int *target_member, int *parity_member, int num_drives);
void rc_amd_route_io_distributed_raid5(u64 *lba, int *mbr, int *parity_mbr, u32 chunk_sectors, int num_drives);

/* RAID 6 Distributed Parity Layout */
u64 rc_amd_map_distributed_raid6(u64 sector_lba, u32 chunk_sectors,
                                  int *target_member, int *p_member, int *q_member, int num_drives);
void rc_amd_route_io_distributed_raid6(u64 *lba, int *mbr, int *p_mbr, int *q_mbr,
                                        u32 chunk_sectors, int num_drives);

#endif /* PATCH_PROTOTYPES_H */