/*
 * parity-write-test.c — QEMU-rig helper for the RAID 5 write path.
 *
 * dd cannot drive this test: busybox's O_DIRECT buffer is not page-aligned,
 * so blk-mq's virt_boundary_mask splits the write into page-sized requests.
 * Those are valid partial-row writes now, but each one exercises RMW and the
 * split makes the expected result awkward; the helper uses an aligned buffer
 * so the driver sees exactly the requests this test intends.
 *
 * Usage: rcparitytest <device> <row-bytes>
 *
 *   1. write one complete row (P generated from the request) and read it back,
 *   2. overwrite 4 KiB inside the row (read-modify-write) and check the whole
 *      row, including bytes outside the write, is exactly right,
 *   3. overwrite the last 4 KiB of the row (same, at a chunk boundary),
 *   4. write 64 KiB across the row boundary (two rows of RMW),
 *   5. two forked processes doing buffered writes + fsync to disjoint rows
 *      (the contention case — see the comment at the step).
 *
 * Exit statuses are distinct so the guest log says which step failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONTEND_PROCS  2
#define CONTEND_ROUNDS 8

static void fill(unsigned char *p, size_t n, unsigned int seed)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(i * 31u + seed);
}

/*
 * Buffered writer for step 5: pwrite + fsync over disjoint rows, seeded per
 * process so the parent can predict the final content.  Buffered on purpose —
 * O_DIRECT would bypass the page cache that produced the field stall.
 */
static int contend_child(const char *dev, unsigned long row, int id)
{
	unsigned long off = (unsigned long)(2 + id * 2) * row;
	unsigned char *b;
	int fd, i;

	alarm(120);
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open (buffered)");
		return 30;
	}
	if (posix_memalign((void **)&b, 4096, row)) {
		fprintf(stderr, "posix_memalign failed\n");
		close(fd);
		return 30;
	}
	for (i = 0; i < CONTEND_ROUNDS; i++) {
		fill(b, row, 300 + id * 100 + i);
		if (pwrite(fd, b, row, off) != (ssize_t)row) {
			perror("concurrent buffered pwrite");
			free(b);
			close(fd);
			return 31;
		}
		if (fsync(fd)) {
			perror("concurrent fsync");
			free(b);
			close(fd);
			return 32;
		}
	}
	free(b);
	close(fd);
	return 0;
}

static int check(int fd, unsigned long off, unsigned long len,
		 const unsigned char *want, const char *what)
{
	unsigned char *got;
	ssize_t n;

	if (posix_memalign((void **)&got, 4096, len))
		return 20;
	n = pread(fd, got, len, off);
	if (n != (ssize_t)len) {
		if (n < 0)
			perror(what);
		else
			fprintf(stderr, "%s short read: %zd of %lu\n",
				what, n, len);
		free(got);
		return 21;
	}
	if (memcmp(got, want, len)) {
		fprintf(stderr, "%s mismatch\n", what);
		free(got);
		return 22;
	}
	free(got);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev;
	unsigned long row;
	unsigned char *buf = NULL, *want = NULL, *io = NULL;
	int fd, rc;
	ssize_t n;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <device> <row-bytes>\n", argv[0]);
		return 2;
	}
	dev = argv[1];
	row = strtoul(argv[2], NULL, 0);
	if (!row || (row & 4095) || row < 4 * 4096) {
		fprintf(stderr, "row must be a multiple of 4096 and >= 16 KiB\n");
		return 2;
	}

	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return 3;
	}
	if (posix_memalign((void **)&buf, 4096, row) ||
	    posix_memalign((void **)&want, 4096, row) ||
	    posix_memalign((void **)&io, 4096, 64 * 1024)) {
		fprintf(stderr, "posix_memalign failed\n");
		return 3;
	}

	/* 1. One complete row: every data chunk is present in the request, so
	 * the driver generates P directly. */
	fill(buf, row, 7);
	n = pwrite(fd, buf, row, 0);
	if (n != (ssize_t)row) {
		if (n < 0)
			perror("full-row pwrite");
		else
			fprintf(stderr, "full-row pwrite short: %zd\n", n);
		return 10;
	}
	memcpy(want, buf, row);
	rc = check(fd, 0, row, want, "full-row readback");
	if (rc)
		return rc;

	/* 2. 4 KiB in the middle of the row: read-modify-write. */
	fill(io, 4096, 99);
	n = pwrite(fd, io, 4096, 4096);
	if (n != 4096) {
		if (n < 0)
			perror("4 KiB RMW pwrite");
		return 11;
	}
	memcpy(want + 4096, io, 4096);
	rc = check(fd, 0, row, want, "4 KiB RMW readback");
	if (rc)
		return rc;

	/* 3. Last 4 KiB of the row: the chunk boundary case. */
	fill(io, 4096, 123);
	n = pwrite(fd, io, 4096, row - 4096);
	if (n != 4096) {
		if (n < 0)
			perror("boundary RMW pwrite");
		return 12;
	}
	memcpy(want + row - 4096, io, 4096);
	rc = check(fd, 0, row, want, "boundary RMW readback");
	if (rc)
		return rc;

	/* 4. 64 KiB straddling the row boundary: two rows of RMW in one
	 * request (the driver loops per row). */
	fill(io, 64 * 1024, 200);
	n = pwrite(fd, io, 64 * 1024, row - 32 * 1024);
	if (n != 64 * 1024) {
		if (n < 0)
			perror("cross-row RMW pwrite");
		return 13;
	}
	rc = check(fd, row - 32 * 1024, 64 * 1024, io,
		   "cross-row RMW readback");
	if (rc)
		return rc;

	/* 5. Concurrent buffered writers.
	 *
	 * The 2026-11-13 field stall was two page-cache writers whose writeback
	 * reached the parity path at the same moment: dispatch took the parity
	 * lock with down_trylock() and returned BLK_STS_DEV_RESOURCE, which is a
	 * promise that the driver will rerun the queue — nothing did, so the
	 * queue stopped and the writers' fsync() waited indefinitely.  Steps 1-4
	 * are one thread and O_DIRECT, so they cannot contend; this step uses the
	 * same buffered writeback + fsync shape as the field failure.
	 */
	{
		pid_t kids[CONTEND_PROCS];
		int i, bad = 0, status;

		for (i = 0; i < CONTEND_PROCS; i++) {
			kids[i] = fork();
			if (kids[i] < 0) {
				perror("fork");
				return 14;
			}
			if (!kids[i])
				_exit(contend_child(dev, row, i));
		}

		/* A wedged queue must surface as a FAIL, not as a hung rig. */
		alarm(180);
		for (i = 0; i < CONTEND_PROCS; i++) {
			if (waitpid(kids[i], &status, 0) < 0) {
				perror("waitpid");
				return 15;
			}
			if (!WIFEXITED(status) || WEXITSTATUS(status))
				bad = 1;
		}
		alarm(0);
		if (bad) {
			fprintf(stderr, "concurrent buffered writers: stalled or I/O "
				"error under contention\n");
			return 16;
		}

		/* Both regions must hold their last round's data. */
		for (i = 0; i < CONTEND_PROCS; i++) {
			unsigned long off = (unsigned long)(2 + i * 2) * row;

			fill(buf, row, 300 + i * 100 + (CONTEND_ROUNDS - 1));
			rc = check(fd, off, row, buf,
				   "concurrent writer readback");
			if (rc)
				return rc;
		}
		printf("parity-write-test: concurrent buffered writers (2 procs, "
		       "%d rounds each) verified\n", CONTEND_ROUNDS);
	}

	printf("parity-write-test: row=%lu: full-row write, in-row RMW, "
	       "boundary RMW, cross-row RMW and multi-writer contention all "
	       "verified\n", row);
	return 0;
}
