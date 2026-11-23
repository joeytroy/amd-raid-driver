// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD-RAID Linux driver — module init + global state
 *
 * Copyright (C) 2025-2026 Joey Troy and contributors.
 *
 * Original work, independently authored from clean-room reverse
 * engineering of the AMD-RAID Windows driver binaries under DMCA
 * §1201(f) interoperability protections.  See docs/RE_METHODOLOGY.md.
 */

#include "rc_linux.h"
#include "patch_prototypes.h"
#include <linux/seq_file.h>

// Global state
struct rc_global_state rc_state = {
    .num_adapters = 0,
    .initialized = 0,
};

// Block major number
int rc_major = 0;

// Module parameters
int rc_debug_level = RC_NOTE;
module_param_named(debug_level, rc_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level (0=debug, 1=info, 2=note, 3=warn, 4=error)");

// Module information
MODULE_AUTHOR("Joey Troy and contributors");
MODULE_DESCRIPTION(RC_DRIVER_DESCRIPTION);
MODULE_VERSION(RC_DRIVER_VERSION);
MODULE_INFO(build_rev, RC_DRIVER_BUILD_REV);
MODULE_LICENSE("GPL v2");

/*
 * rcadm reads /proc/rcraid/version to report and compare the driver
 * version; the vendor driver provides it.  Without it rcadm omits the
 * "rcraid:" line from <VERSIONS> and always warns about a version
 * mismatch.  Report the real module version here, not the rcadm build.
 */
static struct proc_dir_entry *rc_proc_dir;

static int rc_proc_version_show(struct seq_file *m, void *v)
{
    seq_printf(m, "rcraid %s\n", RC_DRIVER_VERSION);
    return 0;
}

static void rc_proc_init(void)
{
    rc_proc_dir = proc_mkdir("rcraid", NULL);
    if (!rc_proc_dir) {
        rc_printk(RC_WARN, "rc_init: could not create /proc/rcraid\n");
        return;
    }

    if (!proc_create_single("version", 0444, rc_proc_dir,
                            rc_proc_version_show)) {
        rc_printk(RC_WARN,
                  "rc_init: could not create /proc/rcraid/version\n");
        proc_remove(rc_proc_dir);
        rc_proc_dir = NULL;
    }
}

static void rc_proc_cleanup(void)
{
    if (rc_proc_dir) {
        proc_remove(rc_proc_dir);
        rc_proc_dir = NULL;
    }
}

// Module initialization
static int __init rc_init(void)
{
    int err;

    rc_printk(RC_NOTE, "rc_init: AMD RAID Driver version %s (build %s)\n",
              RC_DRIVER_VERSION, RC_DRIVER_BUILD_REV);
    rc_printk(RC_NOTE, "rc_init: Based on Windows driver architecture\n");

    /* Fire up the out-of-tree asynchronous execution kthread worker engine pools */
    err = rc_amd_init_async_subsystem();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize out-of-tree async subsystem (%d)\n", err);
        return err;
    }

    // Register block major
    rc_major = register_blkdev(0, "rcraid");
    if (rc_major < 0) {
        rc_printk(RC_ERROR, "rc_init: register_blkdev failed: %d\n", rc_major);
        rc_amd_exit_async_subsystem();
        return rc_major;
    }
    rc_printk(RC_NOTE, "rc_init: registered block major %d\n", rc_major);

    // Initialize debugfs
    err = rc_debugfs_init();
    if (err) {
        rc_printk(RC_WARN, "rc_init: debugfs initialization failed (non-fatal)\n");
    }

    // Initialize rcbottom (hardware layer)
    err = rc_bottom_init();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize rcbottom\n");
        rc_debugfs_cleanup();
        unregister_blkdev(rc_major, "rcraid");
        rc_amd_exit_async_subsystem();
        return err;
    }

    // Initialize rccfg (configuration layer)
    err = rc_config_init();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize rccfg\n");
        rc_bottom_cleanup();
        rc_debugfs_cleanup();
        unregister_blkdev(rc_major, "rcraid");
        rc_amd_exit_async_subsystem();
        return err;
    }

    /* Register the live control plane.  This creates /dev/amd_ctl0 and
     * routes its commands at the assembled volume, not the idle mirror
     * engine. */
    err = rc_amd_init_ioctl_bridge();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize /dev/amd_ctl0 (%d)\n",
                  err);
        rc_config_cleanup();
        rc_bottom_cleanup();
        rc_debugfs_cleanup();
        unregister_blkdev(rc_major, "rcraid");
        rc_amd_exit_async_subsystem();
        return err;
    }

    /* /dev/rc_api: the vendor rcadm CLI's SG_IO config-packet surface.
     * Read-only view of the assembled volume; see src/rc_api.c. */
    err = rc_api_init();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize /dev/rc_api (%d)\n",
                  err);
        rc_amd_exit_ioctl_bridge();
        rc_config_cleanup();
        rc_bottom_cleanup();
        rc_debugfs_cleanup();
        unregister_blkdev(rc_major, "rcraid");
        rc_amd_exit_async_subsystem();
        return err;
    }

    rc_proc_init();
    rc_state.initialized = 1;
    rc_printk(RC_NOTE, "rc_init: AMD RAID Driver initialized successfully\n");
    rc_printk(RC_NOTE, "rc_init: Found %d adapters\n", rc_state.num_adapters);

    return 0;
}

// Module cleanup
static void __exit rc_exit(void)
{
    rc_printk(RC_NOTE, "rc_exit: AMD RAID Driver cleanup\n");

    rc_proc_cleanup();

    /* Remove the control nodes first so no new ioctl can touch a volume
     * that is about to be dismantled. */
    rc_api_cleanup();
    rc_amd_exit_ioctl_bridge();

    // Tear down the assembled RAID volume + gendisk before unbinding members.
    rc_volume_teardown();

    /* Cleanly flush and stop the background execution thread pools */
    rc_amd_exit_async_subsystem();

    // Cleanup in reverse order
    rc_config_cleanup();
    rc_bottom_cleanup();
    rc_debugfs_cleanup();

    // Unregister block major
    if (rc_major > 0) {
        unregister_blkdev(rc_major, "rcraid");
        rc_printk(RC_NOTE, "rc_exit: unregistered block major %d\n", rc_major);
    }

    rc_state.initialized = 0;
    rc_printk(RC_NOTE, "rc_exit: AMD RAID Driver unloaded\n");
}

module_init(rc_init);
module_exit(rc_exit);
