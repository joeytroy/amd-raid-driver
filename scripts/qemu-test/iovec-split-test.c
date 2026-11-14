/*
 * iovec-split-test.c — QEMU-rig helper for the PRP-expressibility hole
 * (issue #20).
 *
 * A RAID 0/10 fan-out hands ONE member several fragments of the same
 * request.  If a later fragment starts at an in-page offset, that
 * member's scatterlist carries a non-first entry PRPs cannot encode
 * (PRP1 is the only entry allowed an offset), and the driver must copy
 * the request through a bounce buffer instead of submitting it.
 *
 * Two things are needed to build that layout, and dd produces neither:
 *
 *   1. the request must START 2 KiB into a page, so every chunk
 *      boundary inside it falls 2 KiB into a page as well — that is the
 *      XFS log-recovery shape (a 256 KiB write starting 58 sectors into
 *      a chunk), and
 *   2. the request must span at least cols+1 chunks, so the column that
 *      owns chunk 0 owns a later chunk too and its fragment is the
 *      member's SECOND sg entry.
 *
 * blk-mq's virt_boundary splitter turns any LATER mid-page bvec into its
 * own request, which is why a plain multi-iovec pwritev does NOT reach
 * the hole (verified: the split shows up as a separate request with a
 * mid-page first bvec, which PRP1 expresses fine).  Starting the whole
 * request mid-page keeps it a single request.
 *
 * Usage: rciovecsplit <device> <chunk-bytes> <cols>
 *   chunk-bytes  one column's stripe (the LD chunk size)
 *   cols         column count; stripes are indexed modulo cols
 *
 * The write/read covers cols+1 whole stripes from a row-aligned LBA, so
 * chunk 0 and chunk `cols` land on the same column.  A plain,
 * page-aligned read of the same range is compared first, so a wrong
 * bounce layout on the write cannot hide behind a matching read.
 *
 * Exit statuses: 1 usage/allocation, 2 open/write, 3 fsync,
 * 4 split-layout read, 5 plain read, 6 readback mismatch.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define START_OFF	2048		/* 2 KiB into the page */
#define START_BYTES	(8u << 20)
/* cols+1 stripes: chunk 0 and chunk `cols` land on the same member. */

static void *xalloc(size_t n)
{
	void *p = NULL;

	if (posix_memalign(&p, 4096, n))
		return NULL;
	return p;
}

static void fill(unsigned char *p, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(i * 7u + seed);
}

int main(int argc, char **argv)
{
	unsigned long long chunk, cols, row, start;
	size_t total;
	unsigned char *wbuf, *rbuf, *ref, *w, *r;
	struct iovec wiov, riov;
	ssize_t n;
	int fd, ret;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <device> <chunk-bytes> <cols>\n",
			argv[0]);
		return 1;
	}
	chunk = strtoull(argv[2], NULL, 0);
	cols = strtoull(argv[3], NULL, 0);
	if (!chunk || (chunk & 4095) || !cols) {
		fprintf(stderr,
			"chunk-bytes must be a non-zero multiple of 4096, cols >= 1\n");
		return 1;
	}
	/* Row-aligned start: stripe 0 of the request is column 0. */
	row = chunk * cols;
	start = (START_BYTES / row) * row;
	total = (size_t)(chunk * (cols + 1u));

	wbuf = xalloc(total + 4096);
	rbuf = xalloc(total + 4096);
	ref = xalloc(total);
	if (!wbuf || !rbuf || !ref) {
		fprintf(stderr, "allocation failed\n");
		return 1;
	}
	w = wbuf + START_OFF;
	r = rbuf + START_OFF;
	memset(w, 0x5a, total);
	fill(w, total, 1);

	wiov.iov_base = w;
	wiov.iov_len = total;
	riov.iov_base = r;
	riov.iov_len = total;

	fd = open(argv[1], O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return 2;
	}
	n = pwritev(fd, &wiov, 1, (off_t)start);
	if (n != (ssize_t)total) {
		fprintf(stderr, "pwritev: %zd of %zu bytes (%s)\n", n, total,
			errno ? strerror(errno) : "short");
		return 2;
	}
	if (fsync(fd)) {
		perror("fsync");
		return 3;
	}

	/* A plain, page-aligned read first: it isolates a wrong WRITE
	 * (bounce layout) from a wrong READ copy-back. */
	n = pread(fd, ref, total, (off_t)start);
	if (n != (ssize_t)total) {
		fprintf(stderr, "pread: %zd of %zu bytes (%s)\n", n, total,
			errno ? strerror(errno) : "short");
		return 5;
	}

	ret = 0;
	if (memcmp(ref, w, total)) {
		size_t i = 0;

		while (i < total && ref[i] == w[i])
			i++;
		fprintf(stderr,
			"plain readback mismatch (WRITE bounce): first diff at %zu of %zu\n",
			i, total);
		ret = 6;
	}

	n = preadv(fd, &riov, 1, (off_t)start);
	if (n != (ssize_t)total) {
		fprintf(stderr, "preadv: %zd of %zu bytes (%s)\n", n, total,
			errno ? strerror(errno) : "short");
		return 4;
	}
	if (memcmp(r, w, total)) {
		size_t i = 0;

		while (i < total && r[i] == w[i])
			i++;
		fprintf(stderr,
			"mid-page readback mismatch (READ copy-back): first diff at %zu of %zu\n",
			i, total);
		ret = 6;
	}
	close(fd);
	if (!ret)
		printf("iovec-split: %llu-byte request at sector %llu from page offset %d, chunk %llu, cols %llu — OK\n",
		       (unsigned long long)total, start, START_OFF, chunk, cols);
	return ret;
}
