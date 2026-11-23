/*
 * api-read-test.c — QEMU-rig helper for the /dev/rc_api read surface.
 *
 * Drives the same SG_IO sequence rcadm issues and asserts the returned
 * packet field by field, so the ABI in src/rc_api.c has automated
 * coverage without bundling AMD's binary (issue #13, option 1):
 *
 *   1. INQUIRY EVPD page 0xD7 (the bare id string, no page header),
 *   2. plain INQUIRY (vendor/product/rev),
 *   3. READ(10) LBA 0, 2 blocks: RC_GetChannelID()'s 1 KiB descriptor —
 *      the packet with its first word replaced by the channel window, so
 *      the header and PhysicalDevice records sit at their packet offsets,
 *   4. the config-window walk 0xC0 -> 0xD0 -> 0xE0, sticky at 0xE0,
 *   5. READ(10) at LBA 0x5000, 64 KiB: the full packet, every member
 *      record included,
 *   6. the GET_CREATE_INFO mailbox-window write + read pair rcadm uses to
 *      size a CREATE, including that the reply is the create-info struct
 *      and the next read goes back to the config packet,
 *   7. mutation frames (TRANSFORM, and a frame too short to decode) must
 *      come back as CHECK CONDITION with the right ASC, never as success,
 *   8. a zero-length read must be refused rather than answered.
 *
 * The vendor's char driver reports success as status 0x01, not the Linux
 * sg driver's 0x00; rcadm treats 0 as failure.  That quirk is asserted
 * here because a regression in it breaks every rcadm verb silently.
 *
 * Usage: rcapireadtest <device> <expected-members>
 *
 * Exit statuses are distinct so the guest log says which step failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Config-packet geometry and field offsets (src/rc_api.c, rc_api.h). */
#define PKT_BYTES	0x10000u
#define PKT_SIZE	0x4000u
#define PD_BASE		0x200u
#define PD_STRIDE	0x100u

#define HDR_VERSION	0x14
#define HDR_PACKET_SIZE	0x18
#define HDR_PD_OFFSET	0x24
#define HDR_PD_SIZE	0x28
#define HDR_LD_OFFSET	0x2c
#define HDR_LD_SIZE	0x30
#define HDR_CTRL_OFFSET	0x40
#define HDR_CTRL_SIZE	0x44
#define HDR_SEP_OFFSET	0x48
#define HDR_SEP_SIZE	0x4c

#define PD_TAG		0x00
#define PD_DEVICE_NUMBER 0x0c
#define PD_CAPACITY	0x1c
#define PD_DEVICE_TYPE	0x24
#define PD_STATE	0x2c
#define PD_MODEL	0x88
#define PD_SERIAL	0xb0

#define PD_TYPE_DISK		0x1200
#define PD_TYPE_CONTROLLER	0x1201
#define PD_STATE_ONLINE		0x3000
#define PD_TAG_VALUE		0x25bc	/* RC_DST_PHYSICAL_DEVICE */

#define CTRL_STRIDE	0x88
#define CTRL_TAG_VALUE	0x25be	/* RC_DST_CONTROLLER_DEVICE */
#define CTRL_ID		0x04
#define CTRL_HANDLE	0x0c
#define CTRL_VID	0x6c
#define CTRL_DID	0x6e
#define CTRL_NUMBER	0x70
#define CTRL_PCILOC	0x84
#define ASC_INVALID_OPCODE 0x20
#define ASC_INVALID_FIELD  0x24

#define WINDOW_FIRST	0xc0
#define WINDOW_LAST	0xe0

#define OP_INQUIRY	0x12
#define OP_READ_10	0x28
#define OP_WRITE_10	0x2a

static void put_le32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static unsigned int get_le32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int get_le16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long long get_le64(const unsigned char *p)
{
	return (unsigned long long)get_le32(p) |
	       ((unsigned long long)get_le32(p + 4) << 32);
}

/*
 * One SG_IO round trip.  Returns 0 on a completed command, -1 on an ioctl
 * error; the SCSI status/resid/sense are reported through the pointers.
 * Completion is not success: callers check status and sense.
 */
static int sg(int fd, const unsigned char *cdb, int cdb_len, void *buf,
	      unsigned int len, unsigned char *status, int *resid,
	      unsigned char *sense)
{
	sg_io_hdr_t hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.interface_id = 'S';
	hdr.cmdp = (unsigned char *)cdb;
	hdr.cmd_len = cdb_len;
	hdr.dxferp = buf;
	hdr.dxfer_len = len;
	if (cdb[0] == OP_WRITE_10)
		hdr.dxfer_direction = SG_DXFER_TO_DEV;
	else
		hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	hdr.sbp = sense;
	hdr.mx_sb_len = 18;
	hdr.timeout = 5000;

	if (ioctl(fd, SG_IO, &hdr) < 0) {
		perror("SG_IO");
		return -1;
	}
	*status = hdr.status;
	*resid = hdr.resid;
	return 0;
}

static void cdb_read10(unsigned char *cdb, unsigned long long lba,
		       unsigned int blocks)
{
	memset(cdb, 0, 10);
	cdb[0] = OP_READ_10;
	cdb[2] = (unsigned char)(lba >> 24);
	cdb[3] = (unsigned char)(lba >> 16);
	cdb[4] = (unsigned char)(lba >> 8);
	cdb[5] = (unsigned char)lba;
	cdb[7] = (unsigned char)(blocks >> 8);
	cdb[8] = (unsigned char)blocks;
}

static void cdb_write10(unsigned char *cdb, unsigned long long lba,
			unsigned int blocks)
{
	cdb_read10(cdb, lba, blocks);
	cdb[0] = OP_WRITE_10;
}

/* A plain success: vendor status convention 0x01, nothing left over. */
static int ok(const char *what, unsigned char status, int resid)
{
	if (status != 0x01) {
		fprintf(stderr, "%s: status 0x%02x, expected 0x01 (the "
			"vendor's success convention)\n", what, status);
		return -1;
	}
	if (resid) {
		fprintf(stderr, "%s: resid %d, expected 0\n", what, resid);
		return -1;
	}
	return 0;
}

/* CHECK CONDITION + fixed-format sense with the expected ASC. */
static int refused(const char *what, unsigned char status, int resid,
		   unsigned int dxfer_len, const unsigned char *sense,
		   unsigned char want_asc)
{
	if (status != 0x02) {
		fprintf(stderr, "%s: status 0x%02x, expected CHECK CONDITION "
			"(0x02); a mutation frame must never report success\n",
			what, status);
		return -1;
	}
	if (sense[0] != 0x70 || sense[2] != 0x05) {
		fprintf(stderr, "%s: sense 0x%02x/0x%02x, expected fixed "
			"format ILLEGAL REQUEST (0x70/0x05)\n", what,
			sense[0], sense[2]);
		return -1;
	}
	if (sense[12] != want_asc) {
		fprintf(stderr, "%s: ASC 0x%02x, expected 0x%02x\n", what,
			sense[12], want_asc);
		return -1;
	}
	if (resid != (int)dxfer_len) {
		fprintf(stderr, "%s: resid %d, expected %u (nothing "
			"transferred)\n", what, resid, dxfer_len);
		return -1;
	}
	return 0;
}

static int check_header(const char *what, const unsigned char *pkt,
			unsigned int members)
{
	unsigned int pd_off = get_le32(pkt + HDR_PD_OFFSET);
	unsigned int pd_size = get_le32(pkt + HDR_PD_SIZE);
	unsigned int ld_off = get_le32(pkt + HDR_LD_OFFSET);
	unsigned int ld_size = get_le32(pkt + HDR_LD_SIZE);
	unsigned int ctrl_off = get_le32(pkt + HDR_CTRL_OFFSET);
	unsigned int ctrl_size = get_le32(pkt + HDR_CTRL_SIZE);
	unsigned int sep_off = get_le32(pkt + HDR_SEP_OFFSET);

	if (get_le32(pkt + HDR_VERSION) != 1) {
		fprintf(stderr, "%s: header version %u, expected 1\n", what,
			get_le32(pkt + HDR_VERSION));
		return -1;
	}
	if (get_le32(pkt + HDR_PACKET_SIZE) != PKT_SIZE) {
		fprintf(stderr, "%s: PacketSize 0x%x, expected 0x%x\n", what,
			get_le32(pkt + HDR_PACKET_SIZE), PKT_SIZE);
		return -1;
	}
	if (pd_off != PD_BASE || pd_size != (members + 1) * PD_STRIDE) {
		fprintf(stderr, "%s: PD region offset 0x%x size 0x%x, "
			"expected 0x%x/0x%x for %u member(s)\n", what, pd_off,
			pd_size, PD_BASE, (members + 1) * PD_STRIDE, members);
		return -1;
	}
	if (!ld_off || ld_off + ld_size > PKT_BYTES) {
		fprintf(stderr, "%s: LD region 0x%x+0x%x outside the packet\n",
			what, ld_off, ld_size);
		return -1;
	}
	/*
	 * The LD record is re-emitted verbatim, so its length is whatever the
	 * on-disk metadata says — not a formula.  Assert only that it is a
	 * plausible record sitting directly after the PhysicalDevice block.
	 */
	if (ld_off != pd_off + pd_size) {
		fprintf(stderr, "%s: LD region starts at 0x%x, expected 0x%x "
			"(directly after the PD block)\n", what, ld_off,
			pd_off + pd_size);
		return -1;
	}
	if (ld_size < 0x100 || ld_size > 0x420 || (ld_size & 3)) {
		fprintf(stderr, "%s: LD size 0x%x is not a plausible record "
			"(0x100..0x420, word aligned)\n", what, ld_size);
		return -1;
	}
	if (ctrl_off != ld_off + ld_size) {
		fprintf(stderr, "%s: ControllerDevice block at 0x%x, expected "
			"0x%x (directly after the LD record)\n", what, ctrl_off,
			ld_off + ld_size);
		return -1;
	}
	if (ctrl_size != members * CTRL_STRIDE ||
	    ctrl_off + ctrl_size > PKT_BYTES) {
		fprintf(stderr, "%s: controller block 0x%x+0x%x, expected "
			"%u record(s) of 0x%x inside the packet\n", what,
			ctrl_off, ctrl_size, members, CTRL_STRIDE);
		return -1;
	}
	if (sep_off != ctrl_off + ctrl_size || get_le32(pkt + HDR_SEP_SIZE)) {
		fprintf(stderr, "%s: separator region 0x%x+0x%x, expected "
			"0x%x+0 outside the controller block\n", what, sep_off,
			get_le32(pkt + HDR_SEP_SIZE), ctrl_off + ctrl_size);
		return -1;
	}
	if (get_le64(pkt + 0x34) == 0) {
		fprintf(stderr, "%s: header generation is zero\n", what);
		return -1;
	}
	return 0;
}

/* Controller record: tag, type, DeviceNumber 0xffff, non-empty model. */
static int check_controller(const char *what, const unsigned char *pd)
{
	unsigned int i;

	if (get_le32(pd + PD_TAG) != PD_TAG_VALUE ||
	    get_le32(pd + PD_DEVICE_TYPE) != PD_TYPE_CONTROLLER) {
		fprintf(stderr, "%s: controller record tag/type 0x%x/0x%x\n",
			what, get_le32(pd + PD_TAG),
			get_le32(pd + PD_DEVICE_TYPE));
		return -1;
	}
	if (get_le32(pd + PD_DEVICE_NUMBER) != 0xffffu) {
		fprintf(stderr, "%s: controller DeviceNumber 0x%x, expected "
			"0xffff\n", what, get_le32(pd + PD_DEVICE_NUMBER));
		return -1;
	}
	for (i = 0; i < 32; i++)
		if (pd[PD_MODEL + i])
			return 0;
	fprintf(stderr, "%s: controller model string is empty\n", what);
	return -1;
}

/* ControllerDevice record: the list rcadm shows as controllers. */
static int check_ctrl_record(const char *what, const unsigned char *c,
			     unsigned int idx)
{
	unsigned long long id = ((unsigned long long)0x1022 << 32) |
				((unsigned long long)0xb000 << 16) | idx;

	if (get_le32(c) != CTRL_TAG_VALUE) {
		fprintf(stderr, "%s: controller record %u tag 0x%x\n", what, idx,
			get_le32(c));
		return -1;
	}
	if (get_le64(c + CTRL_ID) != id) {
		fprintf(stderr, "%s: controller record %u Id 0x%llx, expected "
			"0x%llx\n", what, idx, get_le64(c + CTRL_ID), id);
		return -1;
	}
	if (get_le32(c + CTRL_HANDLE) != 0x10000u + idx) {
		fprintf(stderr, "%s: controller record %u handle 0x%x, expected "
			"0x%x\n", what, idx, get_le32(c + CTRL_HANDLE),
			0x10000u + idx);
		return -1;
	}
	if (get_le16(c + CTRL_VID) != 0x1022 ||
	    get_le16(c + CTRL_DID) != 0xb000) {
		fprintf(stderr, "%s: controller record %u VID/DID %04x/%04x, "
			"expected 1022/b000\n", what, idx,
			get_le16(c + CTRL_VID), get_le16(c + CTRL_DID));
		return -1;
	}
	if (get_le32(c + CTRL_NUMBER) != idx) {
		fprintf(stderr, "%s: controller record %u number %u\n", what, idx,
			get_le32(c + CTRL_NUMBER));
		return -1;
	}
	if (!get_le32(c + CTRL_PCILOC)) {
		fprintf(stderr, "%s: controller record %u PCI location is "
			"zero\n", what, idx);
		return -1;
	}
	return 0;
}

/* Member record: tag, type, handle, capacity, model and serial. */
static int check_member(const char *what, const unsigned char *pd,
			unsigned int idx)
{
	unsigned int i, model = 0, serial = 0;

	if (get_le32(pd + PD_TAG) != PD_TAG_VALUE ||
	    get_le32(pd + PD_DEVICE_TYPE) != PD_TYPE_DISK) {
		fprintf(stderr, "%s: member %u tag/type 0x%x/0x%x\n", what,
			idx, get_le32(pd + PD_TAG),
			get_le32(pd + PD_DEVICE_TYPE));
		return -1;
	}
	if (get_le32(pd + PD_DEVICE_NUMBER) != 0x10000u + idx) {
		fprintf(stderr, "%s: member %u DeviceNumber 0x%x, expected "
			"0x%x\n", what, idx, get_le32(pd + PD_DEVICE_NUMBER),
			0x10000u + idx);
		return -1;
	}
	if (!get_le64(pd + PD_CAPACITY)) {
		fprintf(stderr, "%s: member %u capacity is zero\n", what, idx);
		return -1;
	}
	if (get_le32(pd + PD_STATE) != PD_STATE_ONLINE) {
		fprintf(stderr, "%s: member %u state 0x%x, expected ONLINE "
			"0x%x\n", what, idx, get_le32(pd + PD_STATE),
			PD_STATE_ONLINE);
		return -1;
	}
	for (i = 0; i < 40; i++)
		if (pd[PD_MODEL + i])
			model = 1;
	for (i = 0; i < 24; i++)
		if (pd[PD_SERIAL + i])
			serial = 1;
	if (!model || !serial) {
		fprintf(stderr, "%s: member %u %s string is empty\n", what, idx,
			model ? "serial" : "model");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char cdb[16], sense[18], status, *buf;
	unsigned long long member_cap;
	unsigned int members, i;
	int fd, resid, rc;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <device> <expected-members>\n",
			argv[0]);
		return 2;
	}
	members = (unsigned int)strtoul(argv[2], NULL, 0);
	if (!members || members > 8) {
		fprintf(stderr, "expected-members must be 1..8\n");
		return 2;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 3;
	}
	buf = malloc(PKT_BYTES);
	if (!buf) {
		fprintf(stderr, "malloc failed\n");
		close(fd);
		return 3;
	}

	/* 1. INQUIRY EVPD 0xD7: the bare id string, not a page. */
	memset(buf, 0, 64);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = OP_INQUIRY;
	cdb[1] = 1;
	cdb[2] = 0xd7;
	cdb[4] = 64;
	rc = sg(fd, cdb, 6, buf, 64, &status, &resid, sense);
	if (rc)
		return 10;
	if (status != 0x01 || resid != 64 - 20) {
		fprintf(stderr, "EVPD 0xD7: status 0x%02x resid %d, expected "
			"0x01/44\n", status, resid);
		return 10;
	}
	if (memcmp(buf, "AMD-RAID Controller", 19) || buf[19]) {
		fprintf(stderr, "EVPD 0xD7: id string '%.20s', expected "
			"'AMD-RAID Controller' NUL-terminated\n", buf);
		return 10;
	}

	/* 2. Plain INQUIRY: what the CLI shows as the device. */
	memset(buf, 0, 64);
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = OP_INQUIRY;
	cdb[4] = 36;
	rc = sg(fd, cdb, 6, buf, 36, &status, &resid, sense);
	if (rc)
		return 11;
	if (ok("INQUIRY", status, resid))
		return 11;
	if (buf[0] != 0x00 || buf[2] != 0x05 || buf[3] != 0x02 ||
	    buf[4] != 31) {
		fprintf(stderr, "INQUIRY: type/SPC/format/adlen %u/%u/%u/%u, "
			"expected 0/5/2/31\n", buf[0], buf[2], buf[3], buf[4]);
		return 11;
	}
	if (memcmp(buf + 8, "AMD     ", 8) ||
	    memcmp(buf + 16, "RAID Controller ", 16) ||
	    memcmp(buf + 32, "0001", 4)) {
		fprintf(stderr, "INQUIRY: vendor/product/rev '%.8s'/'%.16s'/"
			"'%.4s'\n", buf + 8, buf + 16, buf + 32);
		return 11;
	}

	/*
	 * 3. The 1 KiB descriptor at LBA 0.  The driver builds the packet and
	 * then overwrites its first word with the channel, so the packet base
	 * is buf — not buf + 4 — and only 1024 of its bytes are present.
	 */
	memset(buf, 0, 1024);
	cdb_read10(cdb, 0, 2);
	rc = sg(fd, cdb, 10, buf, 1024, &status, &resid, sense);
	if (rc)
		return 12;
	if (ok("descriptor read", status, resid))
		return 12;
	if (get_le32(buf) != WINDOW_FIRST) {
		fprintf(stderr, "descriptor: channel window 0x%x, expected "
			"0x%x (the CLI reads the config window from here)\n",
			get_le32(buf), WINDOW_FIRST);
		return 12;
	}
	if (check_header("descriptor", buf, members))
		return 12;
	if (check_controller("descriptor", buf + PD_BASE))
		return 12;
	/*
	 * The 1 KiB window holds the header, the controller slot and exactly
	 * one member record; the rest live past its end and are checked via
	 * the full packet below.
	 */
	if (check_member("descriptor", buf + PD_BASE + PD_STRIDE, 0))
		return 12;
	member_cap = get_le64(buf + PD_BASE + PD_STRIDE + PD_CAPACITY);

	/* 4. The window walk: 0xD0 then 0xE0, sticky at 0xE0. */
	for (i = 0; i < 3; i++) {
		unsigned int want = i < 2 ? WINDOW_FIRST + 0x10 * (i + 1) :
					   WINDOW_LAST;

		memset(buf, 0, 1024);
		cdb_read10(cdb, 0, 2);
		rc = sg(fd, cdb, 10, buf, 1024, &status, &resid, sense);
		if (rc)
			return 13;
		if (ok("window walk", status, resid))
			return 13;
		if (get_le32(buf) != want) {
			fprintf(stderr, "window walk %u: 0x%x, expected "
				"0x%x\n", i, get_le32(buf), want);
			return 13;
		}
	}

	/* 5. Full 64 KiB packet at a nonzero LBA. */
	memset(buf, 0, PKT_BYTES);
	cdb_read10(cdb, 0x5000, 128);
	rc = sg(fd, cdb, 10, buf, PKT_BYTES, &status, &resid, sense);
	if (rc)
		return 14;
	if (ok("64 KiB packet", status, resid))
		return 14;
	if (check_header("64 KiB packet", buf, members))
		return 14;
	if (check_controller("64 KiB packet", buf + PD_BASE))
		return 14;
	for (i = 0; i < members; i++) {
		unsigned int ctrl_off = get_le32(buf + HDR_CTRL_OFFSET);

		if (check_member("64 KiB packet", buf + PD_BASE +
				 (i + 1) * PD_STRIDE, i))
			return 14;
		if (check_ctrl_record("64 KiB packet",
				      buf + ctrl_off + i * CTRL_STRIDE, i))
			return 14;
	}

	/*
	 * 6. GET_CREATE_INFO.  rcadm writes an 8 KiB window request naming the
	 * members it was given, then reads the reply struct back from a
	 * nonzero LBA — the one read that is not the config packet.  Members
	 * are named by handle (0x10000 | slot), which the packet above just
	 * reported, so no DeviceID is needed.
	 */
	memset(buf, 0, 8192);
	put_le32(buf, 1);		/* mailbox command: config */
	put_le32(buf + 4, WINDOW_FIRST);
	put_le32(buf + 0x20, 0x100d);	/* RC_WpGetCreateInfo */
	put_le32(buf + 0x24, 0x1bf6);	/* RAID 0, geometry below */
	put_le32(buf + 0x28, members);	/* DeviceCount */
	put_le32(buf + 0x4c, members);	/* FirstCount */
	put_le32(buf + 0x50, 1);	/* SecondCount */
	for (i = 0; i < members; i++)
		put_le32(buf + 0xc0 + i * 16 + 8, 0x10000u | i);
	cdb_write10(cdb, 0xc0, 16);	/* 8 KiB request */
	rc = sg(fd, cdb, 10, buf, 8192, &status, &resid, sense);
	if (rc)
		return 15;
	/*
	 * Only the 0x180 request bytes are consumed; the driver reports the
	 * rest as resid, and rcadm ignores it for this verb.
	 */
	if (status != 0x01 || resid != 8192 - 0x180) {
		fprintf(stderr, "get_create_info request: status 0x%02x resid "
			"%d, expected 0x01/%d\n", status, resid, 8192 - 0x180);
		return 15;
	}

	memset(buf, 0, 8192);
	cdb_read10(cdb, 0x6000, 16);
	rc = sg(fd, cdb, 10, buf, 8192, &status, &resid, sense);
	if (rc)
		return 15;
	if (ok("get_create_info reply", status, resid))
		return 15;
	if (get_le32(buf) != 0x1bf6 || get_le32(buf + 4) != members ||
	    get_le32(buf + 0x14) != 1) {
		fprintf(stderr, "get_create_info reply: type 0x%x count %u "
			"flags %u, expected 0x1bf6/%u/1 (the request echoed "
			"back)\n", get_le32(buf), get_le32(buf + 4),
			get_le32(buf + 0x14), members);
		return 15;
	}
	/*
	 * Size is level-dependent: 0 for a level the API will not size (RAID 5
	 * reports "not licensed", exactly as rcadm does), otherwise no more
	 * than the members' raw aggregate.
	 */
	if (get_le64(buf + 0x18) >
	    (unsigned long long)members * (member_cap / 2048)) {
		fprintf(stderr, "get_create_info reply: %llu MiB available "
			"exceeds %u member(s) of %llu MiB\n",
			get_le64(buf + 0x18), members, member_cap / 2048);
		return 15;
	}
	/* The reply is one-shot: the next read is the config packet again. */
	memset(buf, 0, PKT_BYTES);
	cdb_read10(cdb, 0x6000, 128);
	rc = sg(fd, cdb, 10, buf, PKT_BYTES, &status, &resid, sense);
	if (rc)
		return 15;
	if (ok("read after create-info reply", status, resid))
		return 15;
	if (get_le32(buf + HDR_PACKET_SIZE) != PKT_SIZE) {
		fprintf(stderr, "read after create-info reply: PacketSize "
			"0x%x, expected the config packet (0x%x) — the pending "
			"reply was not consumed\n",
			get_le32(buf + HDR_PACKET_SIZE), PKT_SIZE);
		return 15;
	}

	/*
	 * 7. A TRANSFORM frame.  It must decode and then be refused — a
	 * success reply for a change nothing persisted is the one lie this
	 * surface must not tell.
	 */
	memset(buf, 0, 1536);
	buf[0] = 8;			/* RC_CW_TRANSFORM */
	memset(sense, 0, sizeof(sense));
	cdb_write10(cdb, 0x8000, 3);
	rc = sg(fd, cdb, 10, buf, 1536, &status, &resid, sense);
	if (rc)
		return 16;
	if (refused("TRANSFORM", status, resid, 1536, sense,
		    ASC_INVALID_OPCODE))
		return 16;

	/* A frame too short to even hold the command header. */
	memset(buf, 0, 1536);
	memset(sense, 0, sizeof(sense));
	cdb_write10(cdb, 0x8000, 1);
	rc = sg(fd, cdb, 10, buf, 4, &status, &resid, sense);
	if (rc)
		return 17;
	if (refused("short mutation frame", status, resid, 4, sense,
		    ASC_INVALID_FIELD))
		return 17;

	/* 8. Zero-length read: refused, not answered with a packet. */
	memset(sense, 0, sizeof(sense));
	cdb_read10(cdb, 0, 2);
	rc = sg(fd, cdb, 10, buf, 0, &status, &resid, sense);
	if (rc)
		return 18;
	if (refused("zero-length read", status, resid, 0, sense,
		    ASC_INVALID_FIELD))
		return 18;

	printf("api-read-test: INQUIRY 0xD7, descriptor + window walk, "
	       "64 KiB packet (%u member(s)), GET_CREATE_INFO, mutation "
	       "refusal verified\n", members);
	free(buf);
	close(fd);
	return 0;
}
