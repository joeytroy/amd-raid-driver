#include "patch_prototypes.h"
/**
 * ==============================================================================
 * CLEANROOM ARCHITECTURE: INDEPENDENT PARITY GENERATION LAYER (RAID 5 / RAID 6)
 * MODULE PATH: joeytroy/amd-raid-driver / patch_parity_math.c
 * ==============================================================================
 *
 * RAID 5 P parity, and the vendor's RAID 6 dual parity (P + Q).
 *
 * RAID 6 Q is NOT GF(2^8) Reed-Solomon.  The vendor computes it as EVENODD/RDP
 * diagonal parity over a 512-byte unit with prime 17 — XOR plus an integer
 * modulus, no field arithmetic at all (rc_raid6_evenodd.cpp;
 * docs/REVERSE_ENGINEERING.md item 8).  That construction lives in
 * raid6_evenodd.h, which the userspace harness compiles too, so the test
 * exercises the code this module ships instead of a private copy of it.  The
 * GF(2^8) exp/log tables this file used to build in a module constructor are
 * gone with it.
 *
 * P = d0 ⊕ d1 ⊕ ... ⊕ d(k-1)   (plain XOR, per word column)
 * Q = EVENODD diagonal parity (see raid6_evenodd.h for the full geometry)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/slab.h>

#include "raid6_evenodd.h"

/* =========================================================================
 * RAID 5 PARITY (P) — simple XOR across all data drives
 * ========================================================================= */
void rc_amd_generate_raid5_parity(uint8_t **data_buffers, uint8_t *parity_buffer,
                                   size_t block_len, int num_drives)
{
    int data_drives = num_drives - 1;
    size_t i;
    int d;

    if (unlikely(data_drives <= 0 || block_len == 0))
        return;

    memset(parity_buffer, 0, block_len);

    for (d = 0; d < data_drives; d++) {
        uint8_t *src = data_buffers[d];
        for (i = 0; i < block_len; i++)
            parity_buffer[i] ^= src[i];
    }
}
EXPORT_SYMBOL_GPL(rc_amd_generate_raid5_parity);

/* =========================================================================
 * RAID 6 DUAL PARITY (P + Q)
 *
 * P = d0 ⊕ d1 ⊕ ... ⊕ d(k-1)                     (XOR, per word column)
 * Q = EVENODD/RDP diagonal parity, prime 17      (raid6_evenodd.h)
 *
 * k = data_drives = num_drives - 2.  The vendor's per-unit geometry — sixteen
 * 32-byte words, seventeen diagonals, the S correction term — is documented in
 * raid6_evenodd.h; this function only adapts the driver's buffer and argument
 * convention to it, and Q covers whole 512-byte units only.
 * ========================================================================= */
void rc_amd_generate_raid6_parity(uint8_t **data_buffers, uint8_t *p_buffer,
                                   uint8_t *q_buffer, size_t block_len, int num_drives)
{
    int data_drives = num_drives - 2;

    if (unlikely(data_drives <= 0 || block_len == 0))
        return;

    /* The S walk assumes at most p-1 = 16 data members; beyond that the
     * vendor's own encoder does not exist, so refuse rather than emit Q that
     * no AMD stack could read.  WARN because reaching here means a caller
     * built an array geometry the driver should have rejected earlier. */
    if (unlikely(data_drives > RC_RAID6_MAX_DATA_MEMBERS)) {
        WARN_ON_ONCE(1);
        memset(p_buffer, 0, block_len);
        memset(q_buffer, 0, block_len);
        return;
    }

    rc_raid6_evenodd_encode((uint8_t *const *)data_buffers, data_drives,
                            p_buffer, q_buffer, block_len);
}
EXPORT_SYMBOL_GPL(rc_amd_generate_raid6_parity);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Independent Cleanroom Parity Architecture Team");
MODULE_DESCRIPTION("RAID 5/6 parity — vendor EVENODD RAID 6 Q (prime 17)");

/* =========================================================================
 * AMD-SPECIFIC ROTATION ROUTING PLANE (RAID 5 / RAID 6)
 * Calculates Left-Asymmetric Distributed Parity offsets across member slots.
 *
 * On-disk encoding: the LEVEL comes from the LogicalDevice DeviceType only
 * (RAID 5 = 0x1BF5, RAID 6 = 0x1BFA; confirmed by the vendor dispatch,
 * which derives parity count as (devtype == 0x1BFA) + 1, and by a BIOS-created
 * 3-disk RAID 5 record).  An earlier revision of this comment claimed FirstCount =
 * N-1 / N-2 and SecondCount = 1 / 2 encoded the level; that cannot be right
 * — FirstCount x SecondCount = Devices on every real record, and neither
 * N-1 x 1 nor N-2 x 2 equals N — so a count-encoded 0x1BF6 record is a
 * RAID0/RAID10 volume, never a parity array (issue #3).
 *   Chunk size ladder identical to RAID 0/10: chunk_index 1/2/3 → 64/128/256 KiB
 *   Layout: DEDICATED parity on the last member, dense per-member packing —
 *   read out of the vendor core (issue #6): RC_RAID5DriveMap returns
 *   (lba >> chunk_pow) %% (Devices - 1), so data never lands on member N-1,
 *   and the RAID 5 flush path counts Devices-1 data children with no stripe
 *   or row term.  This REPLACES the earlier left-symmetric rotation, which no
 *   vendor code performs.
 *
 *   This layout is settled against the vendor implementation, not guessed:
 *   the arithmetic above is AMD's own, and no read, parity-lock or
 *   parity-memory path applies a row term.  Two things are unobserved rather
 *   than unsettled — nobody has read a vendor-written array back through
 *   this driver, and member order comes from the LD element array.  Neither
 *   affects the map arithmetic.  Parity volumes are read-only because there
 *   is no P/Q write path yet, not because the map is in doubt.
 *
 * Everything the driver knows about the parity layout lives in the two map
 * functions below; that is the single place to change if a vendor-written
 * array ever disagrees.
 * ========================================================================= */

/**
 * rc_amd_map_distributed_raid5 - Map logical LBA to (target_member, parity_member, phys_lba)
 * @sector_lba:      logical sector address
 * @chunk_sectors:   stripe size in 512-byte sectors
 * @target_member:   out: data drive index (0..N-2)
 * @parity_member:   out: parity drive index for this stripe
 * @num_drives:      total drives in array (N)
 *
 * Vendor algorithm (issue #6):
 *   - A ROW is one chunk on every member: data_drives data chunks plus the
 *     parity chunk, which is ALWAYS the last member (N-1).  No rotation.
 *   - The logical stripe number selects the data chunk within its row: the
 *     data members are 0..N-2 in ascending order.
 *   - The chunk sits at row @row on its member, data or parity, so each
 *     member's user-data region covers the whole row space and the volume
 *     capacity is (N-1) x per-member user size — the capacity the LD
 *     record reports.
 *
 * Returns: physical sector on target_member
 */
u64 rc_amd_map_distributed_raid5(u64 sector_lba, u32 chunk_sectors,
                                  int *target_member, int *parity_member, int num_drives)
{
    u64 stripe_num, row;
    u32 stripe_off;
    int data_drives = num_drives - 1;
    int j;

    if (unlikely(num_drives < 3 || !chunk_sectors)) {
        *target_member = 0;
        *parity_member = 0;
        return sector_lba;
    }

    stripe_num  = div_u64(sector_lba, chunk_sectors);
    stripe_off  = (u32)(sector_lba - stripe_num * chunk_sectors);

    row = div_u64(stripe_num, (u64)data_drives);
    j   = (int)(stripe_num - row * (u64)data_drives);

    /* Parity is dedicated to the last member (vendor: no rotation). */
    *parity_member = num_drives - 1;
    *target_member = j;

    /* Dense packing: the chunk sits at @row on its member */
    return row * chunk_sectors + stripe_off;
}
EXPORT_SYMBOL_GPL(rc_amd_map_distributed_raid5);

/**
 * rc_amd_map_distributed_raid6 - Map logical LBA to (target_member, p_member, q_member, phys_lba)
 * @sector_lba:      logical sector address
 * @chunk_sectors:   stripe size in 512-byte sectors
 * @target_member:   out: data drive index (0..N-3)
 * @p_member:        out: P parity drive index for this stripe
 * @q_member:        out: Q parity drive index for this stripe
 * @num_drives:      total drives in array (N)
 *
 * Vendor algorithm (issue #6), same row model as RAID 5:
 *   - Data occupies members 0..N-3; the two parity chunks are dedicated to
 *     the last two members: P on N-2, Q on N-1.  No rotation.
 *   - The logical stripe number selects the data chunk within its row.
 *   - The chunk sits at row @row on its member, so volume capacity is
 *     (N-2) x per-member user size.
 *
 * The blob proves the DATA range (RC_RAID6DriveMap: %% (Devices - 2)) and
 * shows the last member accessed explicitly as a parity drive; which of the
 * two trailing members holds P versus Q is assumed here in ascending order.
 * The Q *algorithm* is settled (issue #3) and implemented: the vendor uses
 * EVENODD/RDP diagonal parity with prime 17, not GF(2^8) Reed-Solomon — see
 * rc_amd_generate_raid6_parity() and raid6_evenodd.h.  Placement is
 * unaffected, since a healthy array never reads parity; RAID 6 remains
 * read-only either way, because there is no P/Q write path yet.
 *
 * Returns: physical sector on target_member
 */
u64 rc_amd_map_distributed_raid6(u64 sector_lba, u32 chunk_sectors,
                                  int *target_member, int *p_member, int *q_member,
                                  int num_drives)
{
    u64 stripe_num, row;
    u32 stripe_off;
    int data_drives = num_drives - 2;
    int j;

    if (unlikely(num_drives < 4 || !chunk_sectors)) {
        *target_member = 0;
        *p_member = 0;
        *q_member = 0;
        return sector_lba;
    }

    stripe_num  = div_u64(sector_lba, chunk_sectors);
    stripe_off  = (u32)(sector_lba - stripe_num * chunk_sectors);

    row = div_u64(stripe_num, (u64)data_drives);
    j   = (int)(stripe_num - row * (u64)data_drives);

    /* Parities are dedicated to the last two members (vendor: no rotation). */
    *p_member = num_drives - 2;
    *q_member = num_drives - 1;
    *target_member = j;

    /* Dense packing: the chunk sits at @row on its member */
    return row * chunk_sectors + stripe_off;
}
EXPORT_SYMBOL_GPL(rc_amd_map_distributed_raid6);

/* Thin routing wrappers for callers that use the old single-output API */
void rc_amd_route_io_distributed_raid5(u64 *lba, int *mbr, int *parity_mbr,
                                        u32 chunk_sectors, int num_drives)
{
    *lba = rc_amd_map_distributed_raid5(*lba, chunk_sectors, mbr, parity_mbr, num_drives);
}
EXPORT_SYMBOL_GPL(rc_amd_route_io_distributed_raid5);

void rc_amd_route_io_distributed_raid6(u64 *lba, int *mbr, int *p_mbr, int *q_mbr,
                                        u32 chunk_sectors, int num_drives)
{
    *lba = rc_amd_map_distributed_raid6(*lba, chunk_sectors, mbr, p_mbr, q_mbr, num_drives);
}
EXPORT_SYMBOL_GPL(rc_amd_route_io_distributed_raid6);