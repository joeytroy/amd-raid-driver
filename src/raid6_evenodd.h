/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * ==============================================================================
 * RAID 6 DUAL PARITY — vendor EVENODD / RDP CONSTRUCTION (prime 17)
 * ==============================================================================
 *
 * Shared by the module (src/patch_parity_math.c) and the userspace harness
 * (src/test_parity_userspace.c) so the test proves the code the driver ships
 * rather than a private copy of it.  Nothing here depends on the kernel, so it
 * compiles unchanged on both sides; patch_parity_math.c wraps it in the
 * exported rc_amd_generate_raid6_parity().
 *
 * THIS HEADER IS NOT LICENSED UNDER THE MODULE'S TERMS BY ACCIDENT: the file it
 * is included from is GPL-2.0-only (MODULE_LICENSE("GPL")), and the userspace
 * harness is a test, not a distribution artefact.
 *
 * WHAT THE VENDOR DOES (docs/REVERSE_ENGINEERING.md item 8; settled 2026-11-08
 * from the DWARF-bearing core and cross-checked against rcraid.sys 9.3.3-00291)
 *
 * The encode unit is 512 bytes per member: sixteen 32-byte words.  There are
 * seventeen diagonal positions; the one at index 16 has no word to read, so it
 * is skipped in every column and its contribution is carried separately in a
 * correction term S that is XORed into all sixteen Q words.
 *
 *   word unit:   q[col] = S ^ XOR over members d of member[d][mod(col - d, 17)]
 *                        with member d skipped when mod(col - d, 17) == 16
 *
 *   S        =  XOR of member[d][16 - d] for d = 1 .. members-1
 *                (member 0 is excluded because its index would be 16, the word
 *                that does not exist; walking back one word per member is why
 *                the vendor routine is written for at most 16 data members —
 *                a 17th would step to a negative offset)
 *
 *   P        =  plain XOR down each word column (RC_RAID6_Compute_XOR)
 *
 * There is no Galois arithmetic anywhere in the vendor path: no multiply, no
 * log/exp table, no carry reduction.  Q is XOR plus an integer modulus by 17.
 * Not to be confused with the GF(2^8) Reed-Solomon Q used by Linux md RAID-6 —
 * that is a different code and its bytes would not match firmware-written
 * parity.
 *
 * WHY "16 - d" AND NOT "15 - d": the trace shows the S walk starting at the
 * last word of the first member that has one, which leaves member 0 out
 * entirely.  The alternative reading (member d taking word 15 - d) is
 * singular — two-failure recovery fails for nearly every pair — while 16 - d
 * is full rank for all 120 pairs across 2..16 members.  Decodability settles
 * it; src/test_parity_userspace.c asserts it.
 */
#ifndef RC_RAID6_EVENODD_H
#define RC_RAID6_EVENODD_H

#define RC_RAID6_PRIME             17  /* diagonal positions (p)              */
#define RC_RAID6_WORDS             16  /* words per 512-byte unit (p - 1)     */
#define RC_RAID6_WORD_BYTES        32  /* the vendor XOR unit                 */
#define RC_RAID6_UNIT_BYTES        (RC_RAID6_WORDS * RC_RAID6_WORD_BYTES)
#define RC_RAID6_MAX_DATA_MEMBERS  16  /* S walks back one word per member    */

static inline void rc_raid6_word_zero(uint8_t *w)
{
	int i;

	for (i = 0; i < RC_RAID6_WORD_BYTES; i++)
		w[i] = 0;
}

static inline void rc_raid6_word_copy(uint8_t *dst, const uint8_t *src)
{
	int i;

	for (i = 0; i < RC_RAID6_WORD_BYTES; i++)
		dst[i] = src[i];
}

static inline void rc_raid6_word_xor(uint8_t *dst, const uint8_t *src)
{
	int i;

	for (i = 0; i < RC_RAID6_WORD_BYTES; i++)
		dst[i] ^= src[i];
}

/**
 * rc_raid6_abs_mod17 - RC_RAID6_AbsoluteModulus: a real integer modulus
 *
 * The vendor's sign fixup, reproduced: C's % truncates toward zero, so a
 * negative difference (col < row) comes back negative and has p added once.
 * Callers pass values in -(p-1) .. (p-1), where one add is always enough.
 */
static inline int rc_raid6_abs_mod17(int v)
{
	int r = v % RC_RAID6_PRIME;

	return r < 0 ? r + RC_RAID6_PRIME : r;
}

/**
 * rc_raid6_evenodd_q_unit - Q for one 512-byte unit
 * @data:          data members, each pointing at its own 512-byte unit
 * @data_members:  number of data members (1 .. RC_RAID6_MAX_DATA_MEMBERS)
 * @q:             512-byte Q output
 *
 * Mirrors RC_RAID6_ComputeQ: build S with RC_RAID6_ComputeS, then run the
 * per-column diagonal XOR seeded with S.  Each Q word is fully written.
 */
static inline void rc_raid6_evenodd_q_unit(uint8_t *const *data, int data_members,
					   uint8_t *q)
{
	uint8_t s[RC_RAID6_WORD_BYTES];
	int col, d;

	rc_raid6_word_zero(s);
	for (d = 1; d < data_members; d++)
		rc_raid6_word_xor(s, data[d] + (size_t)(16 - d) * RC_RAID6_WORD_BYTES);

	for (col = 0; col < RC_RAID6_WORDS; col++) {
		uint8_t *out = q + (size_t)col * RC_RAID6_WORD_BYTES;

		rc_raid6_word_copy(out, s);
		for (d = 0; d < data_members; d++) {
			int m = rc_raid6_abs_mod17(col - d);

			if (m == RC_RAID6_PRIME - 1)
				continue;	/* that diagonal is carried in S */
			rc_raid6_word_xor(out, data[d] + (size_t)m * RC_RAID6_WORD_BYTES);
		}
	}
}

/**
 * rc_raid6_evenodd_encode - P and Q across @len bytes of a member
 * @data:          data members, each @len bytes
 * @data_members:  number of data members (1 .. RC_RAID6_MAX_DATA_MEMBERS)
 * @p:             @len-byte P output
 * @q:             @len-byte Q output
 * @len:           byte length; Q covers whole 512-byte units only
 *
 * P is defined for any length.  Q is defined only on the vendor's 512-byte
 * units, so bytes past the last whole unit are written as zero rather than
 * left holding whatever the caller had there — a write path that hands over a
 * partial unit must know it is producing a Q that still has to be fixed up,
 * and zeros make that visible instead of looking like computed parity.
 */
static inline void rc_raid6_evenodd_encode(uint8_t *const *data, int data_members,
					   uint8_t *p, uint8_t *q, size_t len)
{
	uint8_t *unit[RC_RAID6_MAX_DATA_MEMBERS];
	size_t off, i;
	int d;

	if (data_members <= 0 || !len)
		return;
	if (data_members > RC_RAID6_MAX_DATA_MEMBERS)
		return;		/* S cannot be built; caller must reject this */

	for (i = 0; i < len; i++) {
		uint8_t acc = 0;

		for (d = 0; d < data_members; d++)
			acc ^= data[d][i];
		p[i] = acc;
	}

	for (off = 0; off + RC_RAID6_UNIT_BYTES <= len; off += RC_RAID6_UNIT_BYTES) {
		for (d = 0; d < data_members; d++)
			unit[d] = data[d] + off;
		rc_raid6_evenodd_q_unit(unit, data_members, q + off);
	}

	for (i = (len / RC_RAID6_UNIT_BYTES) * RC_RAID6_UNIT_BYTES; i < len; i++)
		q[i] = 0;
}

#endif /* RC_RAID6_EVENODD_H */
