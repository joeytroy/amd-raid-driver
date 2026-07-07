# SPDX-License-Identifier: GPL-2.0-only
# AMD-RAID Linux driver

obj-m += rcraid.o

rcraid-objs := \
    rc_main.o \
    rc_bottom.o \
    rc_firmware.o \
    rc_nvme.o \
    rc_hw.o \
    rc_config.o \
    rc_sysfs.o \
    rc_debugfs.o

# Kernel build directory
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Compiler flags
EXTRA_CFLAGS += -Wall -Wextra
EXTRA_CFLAGS += -Wno-error
EXTRA_CFLAGS += -DRC_DEBUG_LEVEL=$(RC_DEBUG_LEVEL)

# Build revision baked into the load banner and modinfo, so a loaded module
# can always be matched to the exact source it was built from (a stale build
# on the test box once cost a debugging round).  Resolution order:
#   1. .rcraid_rev in the source dir — written by install-dkms.sh /
#      install-livecd.sh when they stage sources outside the git tree
#   2. git describe — building straight from a checkout
#   3. "unknown"
# $(src) is set when kbuild re-reads this file as the module makefile;
# it's empty for the outer make invocation, where "." is the source dir.
RCRAID_SRC := $(if $(src),$(src),.)
RCRAID_REV := $(shell cat $(RCRAID_SRC)/.rcraid_rev 2>/dev/null || git -C $(RCRAID_SRC) describe --always --dirty 2>/dev/null || echo unknown)
EXTRA_CFLAGS += -DRC_DRIVER_BUILD_REV=\"$(RCRAID_REV)\"

# Build targets
all:
	@echo "Attempting to build with current kernel..."
	@$(MAKE) -C $(KERNELDIR) M=$(PWD) modules || \
	(echo "Build failed, trying with older kernel..." && \
	 find /usr/src -name "linux-source-*" -type d | head -1 | xargs -I {} sh -c 'if [ -d "{}/build" ]; then echo "Using kernel: {}"; $(MAKE) -C {}/build M=$(PWD) modules; else echo "No suitable kernel found"; exit 1; fi')

clean:
	@echo "Cleaning build files..."
	@rm -f *.o *.ko *.mod.c *.mod *.symvers *.order .*.cmd
	@rm -rf .tmp_versions
	@echo "Clean completed"

# Simple build target that ignores missing files
simple:
	@echo "Building with minimal requirements..."
	@$(MAKE) -C $(KERNELDIR) M=$(PWD) modules EXTRA_CFLAGS="$(EXTRA_CFLAGS) -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers" || \
	(echo "Trying with older kernel..." && \
	 find /usr/src -name "linux-source-*" -type d | head -1 | xargs -I {} sh -c 'if [ -d "{}/build" ]; then echo "Using: {}"; $(MAKE) -C {}/build M=$(PWD) modules EXTRA_CFLAGS="$(EXTRA_CFLAGS) -Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers"; else echo "No kernel found"; exit 1; fi')

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

# Help target
help:
	@echo "AMD RAID Driver for Linux"
	@echo "Targets:"
	@echo "  all     - Build the driver"
	@echo "  clean   - Clean build files"
	@echo "  install - Install the driver"
	@echo "  help    - Show this help"

.PHONY: all clean install help
