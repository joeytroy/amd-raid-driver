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

// Global state
struct rc_global_state rc_state = {
    .num_adapters = 0,
    .initialized = 0,
};

// Block major number
int rc_major = 0;

// Module parameters.  rc_debug_level is consumed by the rc_printk macro
// (rc_linux.h): messages below the threshold are suppressed and each level
// maps to the matching KERN_* severity.
int rc_debug_level = RC_NOTE;
module_param_named(debug_level, rc_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level (0=debug, 1=info, 2=note, 3=warn, 4=error)");

// Module information
MODULE_AUTHOR("Joey Troy and contributors");
MODULE_DESCRIPTION(RC_DRIVER_DESCRIPTION);
MODULE_VERSION(RC_DRIVER_VERSION);
MODULE_INFO(build_rev, RC_DRIVER_BUILD_REV);
MODULE_LICENSE("GPL v2");

// Module initialization
static int __init rc_init(void)
{
    int err;
    
    rc_printk(RC_NOTE, "rc_init: AMD RAID Driver version %s (build %s)\n",
              RC_DRIVER_VERSION, RC_DRIVER_BUILD_REV);
    rc_printk(RC_NOTE, "rc_init: Based on Windows driver architecture\n");
    
    // Register block major
    rc_major = register_blkdev(0, "rcraid");
    if (rc_major < 0) {
        rc_printk(RC_ERROR, "rc_init: register_blkdev failed: %d\n", rc_major);
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
        return err;
    }
    
    // Initialize rccfg (configuration layer)
    err = rc_config_init();
    if (err) {
        rc_printk(RC_ERROR, "rc_init: failed to initialize rccfg\n");
        rc_bottom_cleanup();
        unregister_blkdev(rc_major, "rcraid");
        return err;
    }
    
    // Initialize rcraid (RAID layer) - will be called when adapters are found
    // For now, just mark as ready for adapter registration
    
    rc_state.initialized = 1;
    rc_printk(RC_NOTE, "rc_init: AMD RAID Driver initialized successfully\n");
    rc_printk(RC_NOTE, "rc_init: Found %d adapters\n", rc_state.num_adapters);
    
    return 0;
}

// Module cleanup
static void __exit rc_exit(void)
{
    rc_printk(RC_NOTE, "rc_exit: AMD RAID Driver cleanup\n");

    // Tear down the assembled RAID volume + gendisk before unbinding members.
    rc_volume_teardown();

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
