/**
 * ==============================================================================
 * USERSpace TEST HARNESS: RAID 5/6 Parity Math Verification
 * Compiles with: gcc -O2 -std=c11 -Wall -Wextra -o test_parity test_parity_userspace.c
 * ==============================================================================
 *
 * Verifies:
 *   1. RAID 5 P parity = XOR across data drives
 *   2. RAID 6 Q parity = the vendor's EVENODD/RDP construction (prime 17),
 *      against hardcoded vectors that were not produced by this code
 *   3. Diagonal topology: which Q words each data word feeds, including the
 *      S correction term
 *   4. Distributed parity mapping (RAID 5/6) for N=3..8
 *   5. Recovery: any two simultaneous member failures, 2..16 data members,
 *      solved as a GF(2) linear system independent of the encoder's loops
 *
 * The generator itself is NOT copied here.  src/raid6_evenodd.h is included
 * directly, and it is the same file src/patch_parity_math.c compiles, so a
 * change to the driver cannot leave this test measuring stale code (issue #10).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "raid6_evenodd.h"/* =========================================================================
 * PARITY GENERATION — RAID 5 is a copy; RAID 6 comes from raid6_evenodd.h
 * ========================================================================= */
void rc_amd_generate_raid5_parity(uint8_t **data_buffers, uint8_t *parity_buffer,
                                   size_t block_len, int num_drives)
{
    int data_drives = num_drives - 1;
    size_t i;
    int d;

    if (data_drives <= 0 || block_len == 0)
        return;

    memset(parity_buffer, 0, block_len);

    for (d = 0; d < data_drives; d++) {
        uint8_t *src = data_buffers[d];
        for (i = 0; i < block_len; i++)
            parity_buffer[i] ^= src[i];
    }
}

/* Mirrors the driver's exported wrapper exactly: num_drives counts the data
 * members plus P and Q, and the shared encoder does the work. */
static void rc_amd_generate_raid6_parity(uint8_t **data_buffers, uint8_t *p_buffer,
                                         uint8_t *q_buffer, size_t block_len,
                                         int num_drives)
{
    int data_drives = num_drives - 2;

    if (data_drives <= 0 || block_len == 0)
        return;

    assert(data_drives <= RC_RAID6_MAX_DATA_MEMBERS);

    rc_raid6_evenodd_encode((uint8_t *const *)data_buffers, data_drives,
                            p_buffer, q_buffer, block_len);
}

/* =========================================================================
 * DISTRIBUTED PARITY MAPPING — copied from patch_parity_math.c
 * ========================================================================= */

uint64_t rc_amd_map_distributed_raid5(uint64_t sector_lba, uint32_t chunk_sectors,
                                       int *target_member, int *parity_member, int num_drives)
{
    uint64_t stripe_num, row;
    uint32_t stripe_off;
    int data_drives = num_drives - 1;
    int parity_pos, j;

    if (num_drives < 3 || !chunk_sectors) {
        *target_member = 0;
        *parity_member = 0;
        return sector_lba;
    }

    stripe_num  = sector_lba / chunk_sectors;
    stripe_off  = (uint32_t)(sector_lba - stripe_num * chunk_sectors);

    row = stripe_num / (uint64_t)data_drives;
    j   = (int)(stripe_num - row * (uint64_t)data_drives);

    /* Dedicated parity on the last member (vendor, issue #6). */
    parity_pos = num_drives - 1;
    *parity_member = parity_pos;
    *target_member = j;

    return row * chunk_sectors + stripe_off;
}

uint64_t rc_amd_map_distributed_raid6(uint64_t sector_lba, uint32_t chunk_sectors,
                                       int *target_member, int *p_member, int *q_member,
                                       int num_drives)
{
    uint64_t stripe_num, row;
    uint32_t stripe_off;
    int data_drives = num_drives - 2;
    int q_pos, p_pos, j;

    if (num_drives < 4 || !chunk_sectors) {
        *target_member = 0;
        *p_member = 0;
        *q_member = 0;
        return sector_lba;
    }

    stripe_num  = sector_lba / chunk_sectors;
    stripe_off  = (uint32_t)(sector_lba - stripe_num * chunk_sectors);

    row = stripe_num / (uint64_t)data_drives;
    j   = (int)(stripe_num - row * (uint64_t)data_drives);

    /* Dedicated parities on the last two members: P on N-2, Q on N-1. */
    p_pos = num_drives - 2;
    q_pos = num_drives - 1;

    *q_member = q_pos;
    *p_member = p_pos;
    *target_member = j;

    return row * chunk_sectors + stripe_off;
}

/* =========================================================================
 * TEST VECTORS & VERIFICATION
 * ========================================================================= */

#define TEST_BLOCK_LEN  4096

/* =========================================================================
 * EVENODD TEST VECTORS — expected bytes are literals, not recomputed
 *
 * Each word is filled with one byte value repeated across all 32 bytes, so one
 * byte identifies a whole word and the unit stays hand-checkable.  The expected
 * parities were derived from the construction in docs/REVERSE_ENGINEERING.md
 * item 8 by independent arithmetic, not by the code below — otherwise the
 * assertion would only prove the encoder agrees with itself, the circularity
 * issue #10 called out.
 * ========================================================================= */

#define PATTERN_WIDE    0   /* byte 16 * member + word + 1 */
#define PATTERN_SPARSE  1   /* byte  7 * member + 3 * word + 1 */

static uint8_t vector_byte(int pattern, int member, int word)
{
    if (pattern == PATTERN_WIDE)
        return (uint8_t)(16 * member + word + 1);
    return (uint8_t)(7 * member + 3 * word + 1);
}

static void check_vector(int pattern, int members, const uint8_t *expect_q,
                         const uint8_t *expect_p)
{
    uint8_t *bufs[RC_RAID6_MAX_DATA_MEMBERS];
    uint8_t p[RC_RAID6_UNIT_BYTES];
    uint8_t q[RC_RAID6_UNIT_BYTES];

    for (int d = 0; d < members; d++) {
        bufs[d] = malloc(RC_RAID6_UNIT_BYTES);
        assert(bufs[d]);
        for (int w = 0; w < RC_RAID6_WORDS; w++)
            memset(bufs[d] + (size_t)w * RC_RAID6_WORD_BYTES,
                   vector_byte(pattern, d, w), RC_RAID6_WORD_BYTES);
    }

    rc_amd_generate_raid6_parity(bufs, p, q, RC_RAID6_UNIT_BYTES, members + 2);

    for (int w = 0; w < RC_RAID6_WORDS; w++) {
        for (int b = 0; b < RC_RAID6_WORD_BYTES; b++) {
            assert(q[(size_t)w * RC_RAID6_WORD_BYTES + b] == expect_q[w]);
            assert(p[(size_t)w * RC_RAID6_WORD_BYTES + b] == expect_p[w]);
        }
    }

    for (int d = 0; d < members; d++)
        free(bufs[d]);
}

static void test_evenodd_vectors(void)
{
    /* 4 data members, member m word w = 16m + w + 1  (S = 0x31) */
    static const uint8_t q4[RC_RAID6_WORDS] = {
        0x3f, 0x62, 0x01, 0x35, 0x31, 0x35, 0x31, 0x3d,
        0x31, 0x3d, 0x31, 0x35, 0x31, 0x35, 0x31, 0x2d,
    };
    static const uint8_t p4[RC_RAID6_WORDS] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
    };
    /* 16 data members — the encoder's maximum — member m word w = 7m + 3w + 1 */
    static const uint8_t q16[RC_RAID6_WORDS] = {
        0x7c, 0x4e, 0x32, 0x4c, 0x38, 0x6a, 0xfe, 0x78,
        0xf4, 0xe6, 0x2a, 0xa4, 0x10, 0xa2, 0x56, 0x71,
    };
    static const uint8_t p16[RC_RAID6_WORDS] = {
        0x40, 0x70, 0x40, 0x30, 0x20, 0x50, 0x40, 0x30,
        0xe0, 0xd0, 0x40, 0x50, 0xc0, 0xf0, 0x80, 0x70,
    };

    printf("Testing RAID 6 Q against recorded vectors...\n");
    check_vector(PATTERN_WIDE, 4, q4, p4);
    check_vector(PATTERN_SPARSE, RC_RAID6_MAX_DATA_MEMBERS, q16, p16);
    printf("  EVENODD vectors: PASS\n");
}

/* Which Q words a single data word feeds.  Word (member d, word w) sits on
 * diagonal (w + d) mod 17, so it lands in exactly that one Q word — unless
 * that diagonal is 16, the one the encoder cannot read: then the word goes
 * into S instead and moves every Q word.  This is the topology asserted
 * directly rather than by comparing against another encoder run. */
static void test_evenodd_topology(void)
{
    uint8_t *base[RC_RAID6_MAX_DATA_MEMBERS];
    uint8_t *alt[RC_RAID6_MAX_DATA_MEMBERS];
    uint8_t p0[RC_RAID6_UNIT_BYTES], q0[RC_RAID6_UNIT_BYTES];
    uint8_t p1[RC_RAID6_UNIT_BYTES], q1[RC_RAID6_UNIT_BYTES];

    printf("Testing EVENODD diagonal topology...\n");

    for (int members = 2; members <= RC_RAID6_MAX_DATA_MEMBERS; members++) {
        for (int d = 0; d < members; d++) {
            base[d] = malloc(RC_RAID6_UNIT_BYTES);
            alt[d] = malloc(RC_RAID6_UNIT_BYTES);
            assert(base[d] && alt[d]);
            for (int i = 0; i < RC_RAID6_UNIT_BYTES; i++) {
                base[d][i] = (uint8_t)(d * 13 + i);
                alt[d][i] = base[d][i];
            }
        }

        rc_amd_generate_raid6_parity(base, p0, q0, RC_RAID6_UNIT_BYTES,
                                     members + 2);

        for (int d = 0; d < members; d++) {
            for (int w = 0; w < RC_RAID6_WORDS; w++) {
                size_t at = (size_t)w * RC_RAID6_WORD_BYTES;
                int expect_col = (w + d) % RC_RAID6_PRIME;

                alt[d][at] ^= 0xff;    /* one byte of one word */
                rc_amd_generate_raid6_parity(alt, p1, q1, RC_RAID6_UNIT_BYTES,
                                             members + 2);
                alt[d][at] ^= 0xff;

                for (int col = 0; col < RC_RAID6_WORDS; col++) {
                    int changed = memcmp(q0 + (size_t)col * RC_RAID6_WORD_BYTES,
                                         q1 + (size_t)col * RC_RAID6_WORD_BYTES,
                                         RC_RAID6_WORD_BYTES) != 0;

                    if (expect_col == RC_RAID6_PRIME - 1)
                        assert(changed);        /* carried in S: hits all */
                    else
                        assert(changed == (col == expect_col));
                }

                /* P is per word row, so only that row moves. */
                for (int row = 0; row < RC_RAID6_WORDS; row++) {
                    int changed = memcmp(p0 + (size_t)row * RC_RAID6_WORD_BYTES,
                                         p1 + (size_t)row * RC_RAID6_WORD_BYTES,
                                         RC_RAID6_WORD_BYTES) != 0;
                    assert(changed == (row == w));
                }
            }
        }

        for (int d = 0; d < members; d++) {
            free(base[d]);
            free(alt[d]);
        }
    }

    printf("  EVENODD topology: PASS\n");
}

static void test_raid5_parity(void)
{
    printf("Testing RAID 5 parity...\n");

    const int num_drives = 5;  /* 4 data + 1 parity */
    const int data_drives = num_drives - 1;

    uint8_t *buffers[4];
    uint8_t parity[TEST_BLOCK_LEN];

    for (int d = 0; d < data_drives; d++) {
        buffers[d] = malloc(TEST_BLOCK_LEN);
        /* Fill with known pattern */
        for (int i = 0; i < TEST_BLOCK_LEN; i++)
            buffers[d][i] = (uint8_t)(d * 73 + i * 17);
    }

    rc_amd_generate_raid5_parity(buffers, parity, TEST_BLOCK_LEN, num_drives);

    /* Verify P = XOR of all data */
    for (int i = 0; i < TEST_BLOCK_LEN; i++) {
        uint8_t expected = 0;
        for (int d = 0; d < data_drives; d++)
            expected ^= buffers[d][i];
        assert(parity[i] == expected);
    }

    /* Test reconstruction: corrupt one drive, rebuild from parity + others */
    uint8_t *corrupted = malloc(TEST_BLOCK_LEN);
    memcpy(corrupted, buffers[2], TEST_BLOCK_LEN);

    /* Zero out drive 2 to simulate failure */
    memset(buffers[2], 0, TEST_BLOCK_LEN);

    /* Rebuild drive 2: parity ⊕ all other drives */
    for (int i = 0; i < TEST_BLOCK_LEN; i++) {
        uint8_t rebuilt = parity[i];
        for (int d = 0; d < data_drives; d++) {
            if (d != 2)
                rebuilt ^= buffers[d][i];
        }
        assert(rebuilt == corrupted[i]);
    }

    for (int d = 0; d < data_drives; d++) free(buffers[d]);
    free(corrupted);

    printf("  RAID 5 parity: PASS\n");
}

static void test_raid6_parity(void)
{
    printf("Testing RAID 6 P parity and single-member rebuild...\n");

    const int num_drives = 6;  /* 4 data + 2 parity (P+Q) */
    const int data_drives = num_drives - 2;

    uint8_t *buffers[4];
    uint8_t p_parity[TEST_BLOCK_LEN];
    uint8_t q_parity[TEST_BLOCK_LEN];

    for (int d = 0; d < data_drives; d++) {
        buffers[d] = malloc(TEST_BLOCK_LEN);
        for (int i = 0; i < TEST_BLOCK_LEN; i++)
            buffers[d][i] = (uint8_t)(d * 97 + i * 31);
    }

    rc_amd_generate_raid6_parity(buffers, p_parity, q_parity, TEST_BLOCK_LEN, num_drives);

    /* P = XOR down each byte column — the one parity the vendor also computes
     * this way (RC_RAID6_Compute_XOR).  Q is pinned by the vectors and the
     * topology test, not by re-deriving the encoder's loops here. */
    for (int i = 0; i < TEST_BLOCK_LEN; i++) {
        uint8_t expected = 0;
        for (int d = 0; d < data_drives; d++)
            expected ^= buffers[d][i];
        assert(p_parity[i] == expected);
    }

    /* Test reconstruction: lose ONE drive, rebuild from P + remaining */
    uint8_t *lost_drive = malloc(TEST_BLOCK_LEN);
    memcpy(lost_drive, buffers[1], TEST_BLOCK_LEN);
    memset(buffers[1], 0, TEST_BLOCK_LEN);

    for (int i = 0; i < TEST_BLOCK_LEN; i++) {
        uint8_t rebuilt = p_parity[i];
        for (int d = 0; d < data_drives; d++) {
            if (d != 1)
                rebuilt ^= buffers[d][i];
        }
        assert(rebuilt == lost_drive[i]);
    }

    memcpy(buffers[1], lost_drive, TEST_BLOCK_LEN);
    free(lost_drive);

    for (int d = 0; d < data_drives; d++) free(buffers[d]);

    /* Degenerate geometry: one data member.  S is empty and every Q word is
     * that member's word at the same index, so Q (and P) must mirror the data
     * exactly — the single-row, dense case. */
    {
        uint8_t *one[1];
        uint8_t p1[RC_RAID6_UNIT_BYTES];
        uint8_t q1[RC_RAID6_UNIT_BYTES];

        one[0] = malloc(RC_RAID6_UNIT_BYTES);
        assert(one[0]);
        for (int i = 0; i < RC_RAID6_UNIT_BYTES; i++)
            one[0][i] = (uint8_t)(i * 7 + 1);

        rc_amd_generate_raid6_parity(one, p1, q1, RC_RAID6_UNIT_BYTES, 3);
        assert(memcmp(one[0], p1, RC_RAID6_UNIT_BYTES) == 0);
        assert(memcmp(one[0], q1, RC_RAID6_UNIT_BYTES) == 0);
        free(one[0]);
    }

    /* All-zero data must give all-zero parity: the construction is linear and
     * carries no constant term (no α^0 offset like the Reed-Solomon version). */
    {
        uint8_t *zero[4];
        uint8_t pz[RC_RAID6_UNIT_BYTES];
        uint8_t qz[RC_RAID6_UNIT_BYTES];

        for (int d = 0; d < 4; d++) {
            zero[d] = calloc(1, RC_RAID6_UNIT_BYTES);
            assert(zero[d]);
        }
        memset(pz, 0xff, sizeof(pz));
        memset(qz, 0xff, sizeof(qz));

        rc_amd_generate_raid6_parity(zero, pz, qz, RC_RAID6_UNIT_BYTES, 6);
        for (int i = 0; i < RC_RAID6_UNIT_BYTES; i++) {
            assert(pz[i] == 0);
            assert(qz[i] == 0);
        }
        for (int d = 0; d < 4; d++) free(zero[d]);
    }

    /* Length that is not a whole 512-byte unit: P is defined everywhere, Q is
     * not, and the tail must come back zeroed rather than holding whatever the
     * caller left in the buffer. */
    {
        uint8_t *bufs[3];
        uint8_t p[700];
        uint8_t q[700];

        for (int d = 0; d < 3; d++) {
            bufs[d] = malloc(sizeof(p));
            assert(bufs[d]);
            for (size_t i = 0; i < sizeof(p); i++)
                bufs[d][i] = (uint8_t)(d * 41 + i * 5);
        }
        memset(q, 0xa5, sizeof(q));

        rc_amd_generate_raid6_parity(bufs, p, q, sizeof(p), 5);

        for (size_t i = 0; i < sizeof(p); i++) {
            uint8_t expected = 0;
            for (int d = 0; d < 3; d++)
                expected ^= bufs[d][i];
            assert(p[i] == expected);
            if (i >= RC_RAID6_UNIT_BYTES)
                assert(q[i] == 0);   /* no vendor Q definition past the unit */
        }
        for (int d = 0; d < 3; d++) free(bufs[d]);
    }

    printf("  RAID 6 P parity: PASS\n");
}

static void test_mapping_functions(void)
{
    printf("Testing distributed parity mapping...\n");

    const uint32_t chunk_sectors = 512;  /* 256 KiB stripe */

    /* Test RAID 5 mapping for various N */
    for (int n = 3; n <= 8; n++) {
        for (uint64_t lba = 0; lba < 10000; lba += 17) {
            int target, parity;
            uint64_t phys = rc_amd_map_distributed_raid5(lba, chunk_sectors, &target, &parity, n);

            assert(target >= 0 && target < n);
            assert(parity >= 0 && parity < n);
            assert(target != parity);
            assert(phys < UINT64_MAX);
        }

        /* Vendor layout: parity is dedicated to the last member, data fills
         * 0..n-2 and never collides with it. */
        int parity_counts[8] = {0};
        for (int stripe = 0; stripe < n * 100; stripe++) {
            int t, p;
            uint64_t s = (uint64_t)stripe * chunk_sectors;
            rc_amd_map_distributed_raid5(s, chunk_sectors, &t, &p, n);
            assert(p == n - 1);
            assert(t >= 0 && t <= n - 2);
            parity_counts[p]++;
        }
        for (int i = 0; i < n - 1; i++)
            assert(parity_counts[i] == 0);
        assert(parity_counts[n - 1] == n * 100);
    }

    /* Test RAID 6 mapping for various N */
    for (int n = 4; n <= 8; n++) {
        for (uint64_t lba = 0; lba < 10000; lba += 23) {
            int target, p_mem, q_mem;
            rc_amd_map_distributed_raid6(lba, chunk_sectors, &target, &p_mem, &q_mem, n);

            assert(target >= 0 && target < n);
            assert(p_mem >= 0 && p_mem < n);
            assert(q_mem >= 0 && q_mem < n);
            assert(target != p_mem);
            assert(target != q_mem);
            assert(p_mem != q_mem);
        }

        /* Vendor layout: P and Q are dedicated to the last two members. */
        int p_counts[8] = {0}, q_counts[8] = {0};
        for (int stripe = 0; stripe < n * 100; stripe++) {
            int t, p, q;
            uint64_t s = (uint64_t)stripe * chunk_sectors;
            rc_amd_map_distributed_raid6(s, chunk_sectors, &t, &p, &q, n);
            assert(p == n - 2);
            assert(q == n - 1);
            assert(t >= 0 && t <= n - 3);
            p_counts[p]++;
            q_counts[q]++;
        }
        assert(p_counts[n - 2] == n * 100);
        assert(q_counts[n - 1] == n * 100);
        for (int i = 0; i < n - 2; i++)
            assert(p_counts[i] == 0 && q_counts[i] == 0);
    }

    printf("  Mapping functions: PASS\n");
}

/* =========================================================================
 * TWO-MEMBER RECOVERY — solved, not re-encoded
 *
 * Recomputing Q and comparing would only prove the encoder agrees with itself.
 * Here the unknowns are the 32 words of the two failed members (2 x 16), the
 * equations are P (one per word row) and Q (one per diagonal column, S terms
 * included), and each byte column is recovered by Gaussian elimination over
 * GF(2).  A construction that cannot survive two failures shows up as a
 * singular or inconsistent system instead of passing quietly.
 * ====================================================================== */

#define RDP_UNKNOWNS (2 * RC_RAID6_WORDS)

/* Independent of raid6_evenodd.h on purpose: if the shared modulus were wrong,
 * the encoder and this check would otherwise be wrong together. */
static int rdp_mod(int v, int m)
{
    int r = v % m;

    return r < 0 ? r + m : r;
}

/* Coefficient mask for the Q equation of column @col when members @a and @b
 * are the unknowns.  Bit i of the low half is word i of member a, bit 16 + i is
 * word i of member b.  A member contributes its diagonal word, and — because S
 * is XORed into every Q column — its word 16 - member as well. */
static uint32_t rdp_q_mask(int members, int a, int b, int col)
{
    uint32_t mask = 0;

    for (int i = 0; i < members; i++) {
        int side, m;

        if (i == a)
            side = 0;
        else if (i == b)
            side = 1;
        else
            continue;

        m = rdp_mod(col - i, RC_RAID6_PRIME);
        if (m != RC_RAID6_PRIME - 1)
            mask ^= 1u << (side * RC_RAID6_WORDS + m);
        if (i >= 1)     /* word 16 - i feeds S, hence every column */
            mask ^= 1u << (side * RC_RAID6_WORDS + (16 - i));
    }

    return mask;
}

/* Gauss-Jordan over GF(2): one bit lane per call, @coeff masks over @nvars
 * unknowns.  Returns 0 when the system is singular or inconsistent. */
static int gf2_solve(uint32_t *coeff, uint8_t *rhs, int nrows, int nvars,
                     uint8_t *sol)
{
    int pivot_col[RDP_UNKNOWNS];
    int pivots = 0;

    for (int col = 0; col < nvars; col++) {
        int sel = -1;

        for (int k = pivots; k < nrows; k++) {
            if ((coeff[k] >> col) & 1u) {
                sel = k;
                break;
            }
        }
        if (sel < 0)
            continue;

        if (sel != pivots) {
            uint32_t tc = coeff[pivots];
            uint8_t tr = rhs[pivots];

            coeff[pivots] = coeff[sel];
            rhs[pivots] = rhs[sel];
            coeff[sel] = tc;
            rhs[sel] = tr;
        }
        for (int k = 0; k < nrows; k++) {
            if (k != pivots && ((coeff[k] >> col) & 1u)) {
                coeff[k] ^= coeff[pivots];
                rhs[k] ^= rhs[pivots];
            }
        }
        pivot_col[pivots++] = col;
    }

    for (int k = 0; k < nrows; k++) {
        if (coeff[k] == 0 && rhs[k] != 0)
            return 0;           /* inconsistent */
    }
    if (pivots != nvars)
        return 0;               /* not enough independent equations */

    for (int k = 0; k < nvars; k++)
        sol[pivot_col[k]] = rhs[k];
    return 1;
}

static void test_raid6_two_failure_recovery(void)
{
    printf("Testing RAID 6 two-member recovery, 2..%d data members...\n",
           RC_RAID6_MAX_DATA_MEMBERS);

    for (int members = 2; members <= RC_RAID6_MAX_DATA_MEMBERS; members++) {
        uint8_t *bufs[RC_RAID6_MAX_DATA_MEMBERS];
        uint8_t p[RC_RAID6_UNIT_BYTES];
        uint8_t q[RC_RAID6_UNIT_BYTES];
        uint8_t s_known[RC_RAID6_WORD_BYTES];

        srand(1000 + members);
        for (int d = 0; d < members; d++) {
            bufs[d] = malloc(RC_RAID6_UNIT_BYTES);
            assert(bufs[d]);
            for (int i = 0; i < RC_RAID6_UNIT_BYTES; i++)
                bufs[d][i] = (uint8_t)rand();
        }
        rc_amd_generate_raid6_parity(bufs, p, q, RC_RAID6_UNIT_BYTES,
                                     members + 2);

        for (int a = 0; a < members; a++) {
            for (int b = a + 1; b < members; b++) {
                uint32_t coeff[2 * RC_RAID6_WORDS];
                uint8_t rhs[2 * RC_RAID6_WORDS];
                uint8_t sol[RDP_UNKNOWNS];

                /* S over the survivors, word 16 - i of each. */
                memset(s_known, 0, sizeof(s_known));
                for (int i = 1; i < members; i++) {
                    if (i == a || i == b)
                        continue;
                    for (int t = 0; t < RC_RAID6_WORD_BYTES; t++)
                        s_known[t] ^= bufs[i][(size_t)(16 - i) *
                                              RC_RAID6_WORD_BYTES + t];
                }

                /* Byte lanes of a word are independent, so solve each. */
                for (int t = 0; t < RC_RAID6_WORD_BYTES; t++) {
                    for (int w = 0; w < RC_RAID6_WORDS; w++) {
                        uint8_t known = p[(size_t)w * RC_RAID6_WORD_BYTES + t];

                        for (int i = 0; i < members; i++) {
                            if (i == a || i == b)
                                continue;
                            known ^= bufs[i][(size_t)w * RC_RAID6_WORD_BYTES + t];
                        }
                        coeff[w] = (1u << w) | (1u << (RC_RAID6_WORDS + w));
                        rhs[w] = known;
                    }

                    for (int col = 0; col < RC_RAID6_WORDS; col++) {
                        uint8_t known =
                            q[(size_t)col * RC_RAID6_WORD_BYTES + t] ^ s_known[t];

                        for (int i = 0; i < members; i++) {
                            int m;

                            if (i == a || i == b)
                                continue;
                            m = rdp_mod(col - i, RC_RAID6_PRIME);
                            if (m == RC_RAID6_PRIME - 1)
                                continue;
                            known ^= bufs[i][(size_t)m * RC_RAID6_WORD_BYTES + t];
                        }
                        coeff[RC_RAID6_WORDS + col] =
                            rdp_q_mask(members, a, b, col);
                        rhs[RC_RAID6_WORDS + col] = known;
                    }

                    assert(gf2_solve(coeff, rhs, 2 * RC_RAID6_WORDS,
                                     RDP_UNKNOWNS, sol));

                    for (int w = 0; w < RC_RAID6_WORDS; w++) {
                        assert(sol[w] ==
                               bufs[a][(size_t)w * RC_RAID6_WORD_BYTES + t]);
                        assert(sol[RC_RAID6_WORDS + w] ==
                               bufs[b][(size_t)w * RC_RAID6_WORD_BYTES + t]);
                    }
                }
            }
        }

        for (int d = 0; d < members; d++)
            free(bufs[d]);
    }

    printf("  RAID 6 two-member recovery: PASS\n");
}

/* A distributed layout must be one-to-one: no two logical sectors may map to
 * the same (member, physical LBA), and the P/Q/data roles of a stripe must be
 * distinct members. */
static int check_bijection(int n, int is_raid6)
{
    const uint32_t chunk_sectors = 8;
    const uint64_t lbas = 4096;
    uint8_t *seen = calloc((size_t)n * lbas, 1);
    int ok = 1;

    assert(seen);
    for (uint64_t lba = 0; lba < lbas && ok; lba++) {
        int t, p, q;
        uint64_t phys;
        if (is_raid6) {
            phys = rc_amd_map_distributed_raid6(lba, chunk_sectors, &t, &p, &q, n);
            if (t == p || t == q || p == q)
                ok = 0;
        } else {
            phys = rc_amd_map_distributed_raid5(lba, chunk_sectors, &t, &p, n);
            if (t == p)
                ok = 0;
        }
        if (phys >= lbas || seen[(size_t)t * lbas + phys]++)
            ok = 0;
        if (!ok)
            printf("  RAID%d n=%d: lba %lu collides or clashes\n",
                   is_raid6 ? 6 : 5, n, (unsigned long)lba);
    }
    free(seen);
    return ok;
}

/* Dense packing: every row must place exactly one chunk per member, and a
 * member's chunks must occupy rows 0..rows-1 with no holes and no overlaps.
 * That is what makes per-member user data equal the volume capacity divided
 * by the data column count — if a member stored its chunks at their logical
 * stripe numbers instead, it would need room for the whole logical space
 * and a read would address past the user-data region. */
static int check_packing(int n, int is_raid6)
{
    const uint32_t chunk_sectors = 8;
    const int data_cols = is_raid6 ? n - 2 : n - 1;
    const int rows = n * 100;
    const uint64_t stripes = (uint64_t)data_cols * rows;
    uint8_t *seen = calloc((size_t)n * rows, 1);
    int own[8] = {0};
    int ok = 1;

    assert(seen);
    for (uint64_t s = 0; s < stripes && ok; s++) {
        int t, p, q;
        uint64_t phys = is_raid6
            ? rc_amd_map_distributed_raid6(s * chunk_sectors, chunk_sectors,
                                           &t, &p, &q, n)
            : rc_amd_map_distributed_raid5(s * chunk_sectors, chunk_sectors,
                                           &t, &p, n);
        uint64_t row = phys / chunk_sectors;

        if (phys % chunk_sectors || row >= (uint64_t)rows)
            ok = 0;
        else if (seen[(size_t)t * rows + row]++)
            ok = 0;
        else
            own[t]++;
    }

    /* Dedicated parity (vendor layout, issue #6): every row hands one data
     * chunk to each of the data_cols leading members, and the trailing
     * member(s) hold parity only.  With rotating parity every member would
     * instead see data_cols x 100 chunks. */
    for (int m = 0; m < n && ok; m++) {
        int expected = (m < data_cols) ? rows : 0;

        if (own[m] != expected)
            ok = 0;
    }

    if (!ok)
        printf("  RAID%d n=%d: packing is not dense/one-per-row\n",
               is_raid6 ? 6 : 5, n);
    free(seen);
    return ok;
}

static void test_mapping_bijection(void)
{
    printf("Testing distributed mapping is one-to-one...\n");
    for (int n = 3; n <= 8; n++)
        assert(check_bijection(n, 0));
    for (int n = 4; n <= 8; n++)
        assert(check_bijection(n, 1));
    printf("  Mapping bijection: PASS\n");

    printf("Testing distributed mapping packs densely...\n");
    for (int n = 3; n <= 8; n++)
        assert(check_packing(n, 0));
    for (int n = 4; n <= 8; n++)
        assert(check_packing(n, 1));
    printf("  Mapping packing: PASS\n");
}

int main(void)
{
    printf("========================================\n");
    printf("RAID 5/6 Parity Math Test Harness\n");
    printf("========================================\n\n");

    test_raid5_parity();
    test_evenodd_vectors();
    test_evenodd_topology();
    test_raid6_parity();
    test_raid6_two_failure_recovery();
    test_mapping_functions();
    test_mapping_bijection();

    printf("\n========================================\n");
    printf("ALL TESTS PASSED ✓\n");
    printf("========================================\n");

    return 0;
}