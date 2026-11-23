// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — /dev/rc_api SG_IO compatibility surface
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Types shared between the volume registry (rc_nvme.c) and the SG_IO
 * character device (rc_api.c).  The snapshot is a plain copy-out taken
 * under rc_volume_lock: no kernel pointer crosses the boundary, and the
 * packet builder never touches driver state directly.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See docs/RE_METHODOLOGY.md
 * and the "rcadm /dev/rc_api config-packet ABI" section of
 * docs/REVERSE_ENGINEERING.md.
 */

#ifndef _RC_API_H_
#define _RC_API_H_

#include <linux/types.h>

/* Maximum members in a volume (matches RC_VOLUME_MAX_MEMBERS). */
#define RC_API_PD_MAX		8

/*
 * Largest RC_LogicalDevice record the shim re-emits: the fixed 0x200-byte
 * header, one 64-byte RC_LogicalElement_LE per member, and the 16-byte tail
 * the firmware's PacketSize includes.  A 4-member record is 0x310 bytes;
 * 8 members is 0x410.  Keep some slack above that.
 */
#define RC_API_LD_MAX		0x420

/* Config-packet geometry, from docs/REVERSE_ENGINEERING.md. */
#define RC_API_PACKET_BYTES	0x10000u	/* what rcadm reads */
#define RC_API_PACKET_SIZE	0x4000u		/* PacketSize the vendor reports */
#define RC_API_PD_BASE		0x200u		/* first PhysicalDevice record */
#define RC_API_PD_STRIDE	0x100u		/* flag-0 record size */
#define RC_API_DESCRIPTOR_BYTES	1024u		/* READ(10) LBA 0, 2 blocks */

struct rc_api_member {
	u64 device_id;		/* RC_MetaData.DeviceId — matches LD elements */
	u64 capacity_sectors;	/* namespace NSZE (vendor writes the same) */
	u64 free_sectors;	/* usable sectors, 0 while in an array */
	u32 ld_position;	/* index in the LogicalDevice element array */
	u8  online;		/* member state is LIVE */
	u8  _pad[3];
	u16 vid;
	u16 ssvid;
	u16 sub_vid;		/* PCI subsystem vendor id */
	u16 sub_dev;		/* PCI subsystem device id */
	u32 pci_location;	/* bus << 16 | device << 8 | function */
	u8  firmware[8];	/* Identify Controller FR, space-padded */
	u8  model[40];		/* Identify Controller MN, space-padded */
	u8  serial[24];		/* Identify Controller SN (20) + pad */
};

struct rc_api_snapshot {
	u32 nr_members;
	u32 level;		/* normalized RC_LDT_* */
	u32 stripe_sectors;
	u64 capacity_sectors;	/* assembled volume size, 0 if not up */
	u8  have_ld;		/* ld_raw holds the active LD record */
	u8  _pad[3];
	u32 ld_len;
	u8  ld_raw[RC_API_LD_MAX];
	struct rc_api_member member[RC_API_PD_MAX];
};

/* Copy the assembled-volume state out under rc_volume_lock (rc_nvme.c). */
int  rc_api_snapshot_get(struct rc_api_snapshot *out);

/* Persist a DELETE and tear the volume down (rc_nvme.c). */
int  rc_volume_delete_array(void);

/* Stamp a new array on the selected members, in the order given (rc_nvme.c).
 * device_ids == NULL means "every bound member"; requested_id is the
 * logical-device Id rcadm sends as PreferDeviceID and looks up afterwards
 * (0 means "derive one"). */
int  rc_volume_create_array(u32 raid_type, u32 first, u32 second,
			    u32 device_count, u32 size_mb, u64 requested_id,
			    const u64 *device_ids, u32 nr_ids);

/* /dev/rc_api registration (rc_api.c). */
int  rc_api_init(void);
void rc_api_cleanup(void);

#endif /* _RC_API_H_ */
