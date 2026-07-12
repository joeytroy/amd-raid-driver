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
# Compiler flags.  ccflags-y, NOT the old EXTRA_CFLAGS: kbuild dropped
# EXTRA_CFLAGS support (it is absent from scripts/Makefile.lib on 6.x),
# so anything added through it silently never reaches the compiler —
# which is exactly how the build-rev stamp below was broken for a while.
ccflags-y += -Wall -Wextra
ccflags-y += -Wno-error

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
ccflags-y += -DRC_DRIVER_BUILD_REV=\"$(RCRAID_REV)\"

# Driver version = the AMD Windows release this port's behavior matches,
# read from the single-line VERSION file at the repo root.  When a new AMD
# release has been torn down with Ghidra and verified against the port
# (docs/REVERSE_ENGINEERING.md "Version delta"), bump VERSION — no code
# change needed.  Falls back to the rc_linux.h default if the file is
# missing (e.g. a hand-copied source dir).
RCRAID_VERSION := $(firstword $(shell cat $(RCRAID_SRC)/VERSION 2>/dev/null))
ifneq ($(RCRAID_VERSION),)
ccflags-y += -DRC_DRIVER_VERSION=\"$(RCRAID_VERSION)\"
endif

# Build targets.
# No `find /usr/src | xargs sh -c` fallback: it silently picked an arbitrary
# tree (whichever `head -1` produced), broke on paths with spaces, and
# masked the real error from the primary build.  If the headers for the
# running kernel are missing, say so and stop.
all:
	@if [ ! -d "$(KERNELDIR)" ]; then \
		echo "error: kernel build directory '$(KERNELDIR)' not found." >&2; \
		echo "       Install the headers for the running kernel, or set KERNELDIR=/path/to/kernel/build" >&2; \
		exit 1; \
	fi
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	@echo "Cleaning build files..."
	@rm -f *.o *.ko *.mod.c *.mod *.symvers *.order .*.cmd
	@rm -rf .tmp_versions
	@echo "Clean completed"

# Lenient build target: extra warnings suppressed via KCFLAGS (the
# supported way to append flags from outside kbuild; EXTRA_CFLAGS on the
# command line is ignored by modern kernels).
simple:
	@if [ ! -d "$(KERNELDIR)" ]; then \
		echo "error: kernel build directory '$(KERNELDIR)' not found." >&2; \
		echo "       Install the headers for the running kernel, or set KERNELDIR=/path/to/kernel/build" >&2; \
		exit 1; \
	fi
	@echo "Building with minimal requirements..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules KCFLAGS="-Wno-error -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers"

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
