/**
 * ==============================================================================
 * UPSTREAM PATCH VECTOR: HIGHPOINT-STYLE IOCTL MANAGEMENT CONTROLLER LAYER
 * SOURCE ARCHITECTURE TARGET: kubuntugamer/amd-raid-driver / src/patch_ioctl_bridge.c
 * ==============================================================================
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "amd_raid_uapi.h"
#include "patch_prototypes.h"

#define DEVICE_NODE_NAME "amd_ctl0"

static bool amd_ctl_registered;

/**
 * Core IOCTL execution router.
 *
 * Status and inventory are read-only diagnostics and may be used without
 * privileges.  Commands that can reshape the array -- failing a member or
 * starting a rebuild -- are gated on CAP_SYS_ADMIN.  This is deliberately
 * namespace-unaware: these actions change state shared by the whole system.
 */
static long amd_raid_ioctl_multiplexer(struct file *file, unsigned int cmd,
				       unsigned long arg)
{
	switch (cmd) {
	case AMD_IOCTL_GET_HARDWARE_INVENTORY: {
		struct AmdHardwarePayload *payload;
		int ret;

		payload = kzalloc(sizeof(*payload), GFP_KERNEL);
		if (!payload)
			return -ENOMEM;

		ret = rc_volume_get_inventory(payload);
		if (!ret &&
		    copy_to_user((void __user *)arg, payload, sizeof(*payload)))
			ret = -EFAULT;
		kfree(payload);

		if (ret)
			pr_err("rcraid_ctl: inventory request failed: %d\n", ret);
		return ret;
	}

	case AMD_IOCTL_GET_STATUS: {
		u32 status_mask;
		int ret;

		ret = rc_volume_get_status(&status_mask);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &status_mask,
				 sizeof(status_mask)))
			return -EFAULT;

		pr_info("rcraid_ctl: live status mask 0x%08x\n", status_mask);
		return 0;
	}

	case AMD_IOCTL_START_RESYNC: {
		struct amd_resync_range range;
		int ret;

		if (!capable(CAP_SYS_ADMIN)) {
			pr_warn_ratelimited("rcraid_ctl: rejected resync from unprivileged process %s (pid %d)\n",
					    current->comm, current->pid);
			return -EPERM;
		}
		if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
			return -EFAULT;

		/*
		 * The live rebuild scheduler rebuilds a parked member from its
		 * survivor; the request's sector range is informational.  RAID
		 * 5/6 and RAID 0 have no live rebuild path and are refused by
		 * rc_volume_request_resync().
		 */
		ret = rc_volume_request_resync();
		if (ret) {
			pr_err("rcraid_ctl: live resync request rejected: %d\n",
			       ret);
			return ret;
		}
		pr_info("rcraid_ctl: live resync queued (range %u..%u ignored; scheduler picks a parked member)\n",
			range.start_sector, range.end_sector);
		return 0;
	}

	case AMD_IOCTL_FAIL_MEMBER: {
		u32 target_drive_index;
		int ret;

		if (!capable(CAP_SYS_ADMIN)) {
			pr_warn_ratelimited("rcraid_ctl: rejected member eviction from unprivileged process %s (pid %d)\n",
					    current->comm, current->pid);
			return -EPERM;
		}
		if (copy_from_user(&target_drive_index, (void __user *)arg,
				   sizeof(target_drive_index)))
			return -EFAULT;

		pr_alert("rcraid_ctl: user-space command forcing eviction of member index %u\n",
			 target_drive_index);
		ret = rc_volume_fail_member_slot((int)target_drive_index);
		if (ret)
			pr_err("rcraid_ctl: member index %u rejected: %d\n",
			       target_drive_index, ret);
		return ret;
	}

	case AMD_IOCTL_CREATE_ARRAY: {
		struct amd_array_create req;
		int ret;

		if (!capable(CAP_SYS_ADMIN)) {
			pr_warn_ratelimited("rcraid_ctl: rejected array create from unprivileged process %s (pid %d)\n",
					    current->comm, current->pid);
			return -EPERM;
		}
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		if (req.member_count > AMD_ARRAY_MAX_MEMBERS)
			return -EINVAL;

		ret = rc_volume_create_array_indexed(req.level,
						     req.member_indices,
						     req.member_count,
						     req.size_mb);
		if (ret)
			pr_err("rcraid_ctl: create level %u over %u member(s) failed: %d\n",
			       req.level, req.member_count, ret);
		return ret;
	}

	case AMD_IOCTL_DELETE_ARRAY: {
		struct amd_array_delete req;
		int ret;

		if (!capable(CAP_SYS_ADMIN)) {
			pr_warn_ratelimited("rcraid_ctl: rejected array delete from unprivileged process %s (pid %d)\n",
					    current->comm, current->pid);
			return -EPERM;
		}
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		if (req.array_id != 0)
			return -ENOENT;

		ret = rc_volume_delete_array();
		if (ret)
			pr_err("rcraid_ctl: delete array %u failed: %d\n",
			       req.array_id, ret);
		return ret;
	}

	default:
		return -EINVAL;
	}
}

static const struct file_operations amd_ctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = amd_raid_ioctl_multiplexer,
	.compat_ioctl = amd_raid_ioctl_multiplexer,
};

static struct miscdevice amd_ctl_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NODE_NAME,
	.fops = &amd_ctl_fops,
	.mode = 0644,
};

int rc_amd_init_ioctl_bridge(void)
{
	int ret;

	ret = misc_register(&amd_ctl_misc);
	if (ret) {
		pr_err("rcraid_ctl: failed to register /dev/%s: %d\n",
		       DEVICE_NODE_NAME, ret);
		return ret;
	}
	amd_ctl_registered = true;
	pr_info("rcraid_ctl: /dev/%s registered (minor %d)\n",
		DEVICE_NODE_NAME, amd_ctl_misc.minor);
	return 0;
}

void rc_amd_exit_ioctl_bridge(void)
{
	if (!amd_ctl_registered)
		return;
	misc_deregister(&amd_ctl_misc);
	amd_ctl_registered = false;
	pr_info("rcraid_ctl: /dev/%s unregistered\n", DEVICE_NODE_NAME);
}
