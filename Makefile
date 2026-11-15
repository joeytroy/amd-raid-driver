# SPDX-License-Identifier: GPL-2.0-only
# AMD-RAID Linux driver

obj-m += rcraid.o

rcraid-objs := \
    src/rc_main.o \
    src/rc_bottom.o \
    src/rc_firmware.o \
    src/rc_nvme.o \
    src/rc_hw.o \
    src/rc_config.o \
    src/rc_api.o \
    src/rc_sysfs.o src/patch_async_worker.o src/patch_parity_math.o src/patch_ioctl_bridge.o \
    src/rc_debugfs.o

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
	@rm -rf $(ASYNC_TEST_BIN) $(dir $(ASYNC_TEST_BIN))
	@rm -rf $(CFGWRITE_TEST_BIN) $(dir $(CFGWRITE_TEST_BIN))
	@rm -rf $(METABUILD_TEST_BIN) $(dir $(METABUILD_TEST_BIN))
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

# ------------------------------------------------------------------------------
# Userspace concurrency harness for the async mirror engine.
#
# src/test_async_worker_harness.c #includes the REAL src/patch_async_worker.c
# behind a shim for the kernel primitives, so it runs the engine's actual
# locking and accounting code under real threads and real preemption — which is
# where bugs in this kind of code live, and where a module on a RAID box with no
# spare machine to debug on cannot be observed.  It needs no kernel headers at
# all, so it deliberately does not touch KERNELDIR and is not an rcraid-objs
# member.
#
# The engine #includes <linux/*.h>, which do not exist in userspace.  The target
# materializes empty stand-ins in a scratch include dir: the harness defines
# every primitive itself before including the engine, so the preprocessor only
# needs those paths to resolve.  Generating them beats checking in a fake
# linux/ tree, which would otherwise invite editor indexers and greps into it.
#
# Drain budget is shortened from the engine's 5000 ms default because the
# stuck-member unload test is deliberately slower than any real drain.
# ------------------------------------------------------------------------------
ASYNC_TEST_BIN  ?= $(CURDIR)/.async-test/async_test
# Recursive (=) on purpose: it has to track an overridden ASYNC_TEST_BIN, so the
# sanitizer variants below get their own scratch dir instead of racing each
# other over the same empty stub headers.
ASYNC_SHIM_DIR  = $(dir $(ASYNC_TEST_BIN))shim
ASYNC_TEST_SRC  := src/test_async_worker_harness.c
ASYNC_TEST_DEPS := $(ASYNC_TEST_SRC) src/patch_async_worker.c src/patch_prototypes.h
# Exactly the <linux/*.h> set the engine includes.  A new kernel include in the
# engine without an entry here fails the build loudly, which is the point.
ASYNC_SHIM_HDRS := atomic bio blk_types err kernel kthread list module sched \
                   slab spinlock wait
ASYNC_TEST_CFLAGS ?=

test-async: $(ASYNC_TEST_BIN)
	@$(ASYNC_TEST_BIN)

$(ASYNC_TEST_BIN): $(ASYNC_TEST_DEPS)
	@mkdir -p $(ASYNC_SHIM_DIR)/linux
	@for h in $(ASYNC_SHIM_HDRS); do : > $(ASYNC_SHIM_DIR)/linux/$$h.h; done
	@echo "Building async engine harness (userspace, no kernel headers)..."
	$(CC) -O1 -g -pthread -Wall -Wextra -Wno-unused-parameter \
		-DRC_ASYNC_DRAIN_TIMEOUT_MS=300 $(ASYNC_TEST_CFLAGS) \
		-o $(ASYNC_TEST_BIN) $(ASYNC_TEST_SRC) \
		-I$(ASYNC_SHIM_DIR) -Isrc

# Same build under the sanitizers.  Separate targets because the point is to run
# it once per configuration rather than to build a universal binary:
#   make test-async-asan     memory errors + undefined behaviour + leaks
#   make test-async-tsan     data races (the reason this harness exists)
test-async-asan:
	@$(MAKE) test-async \
		ASYNC_TEST_BIN=$(ASYNC_TEST_BIN)-asan \
		ASYNC_TEST_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

test-async-tsan:
	@$(MAKE) test-async \
		ASYNC_TEST_BIN=$(ASYNC_TEST_BIN)-tsan \
		ASYNC_TEST_CFLAGS="-fsanitize=thread -fno-omit-frame-pointer"

# ------------------------------------------------------------------------------
# Userspace harness for the RC_CfgWrite decoder (src/rc_cfgwrite.h).
#
# The decoder is header-only and dependency-free precisely so the wire layout
# captured off rcadm can be checked without kernel headers or hardware: an
# offset error here becomes an offset error in an array operation later.
# ------------------------------------------------------------------------------
CFGWRITE_TEST_BIN ?= $(CURDIR)/.cfgwrite-test/cfgwrite_test
CFGWRITE_TEST_SRC := src/test_cfgwrite_userspace.c
CFGWRITE_TEST_DEPS := $(CFGWRITE_TEST_SRC) src/rc_cfgwrite.h

test-cfgwrite: $(CFGWRITE_TEST_BIN)
	@$(CFGWRITE_TEST_BIN)

$(CFGWRITE_TEST_BIN): $(CFGWRITE_TEST_DEPS)
	@mkdir -p $(dir $(CFGWRITE_TEST_BIN))
	@echo "Building RC_CfgWrite decoder harness (userspace, no kernel headers)..."
	$(CC) -O1 -g -Wall -Wextra -Werror -o $(CFGWRITE_TEST_BIN) \
		$(CFGWRITE_TEST_SRC) -Isrc

# ------------------------------------------------------------------------------
# Metadata encoder parity harness.
#
# Emits the images src/rc_meta_build.h builds and diffs them against
# scripts/qemu-test/mkmeta.py, byte for byte.  A metadata writer that disagrees
# with the generator the QEMU rig validates would produce arrays the driver
# cannot read back, so this is the gate before any write path is wired up.
# ------------------------------------------------------------------------------
METABUILD_TEST_BIN ?= $(CURDIR)/.metabuild-test/metabuild_test
METABUILD_TEST_OUT  = $(dir $(METABUILD_TEST_BIN))out
METABUILD_TEST_SRC := src/test_metabuild_userspace.c
METABUILD_TEST_DEPS := $(METABUILD_TEST_SRC) src/rc_meta_build.h \
                       scripts/qemu-test/mkmeta.py \
                       scripts/qemu-test/check-metabuild.py

# Userspace stamping tool: writes a whole image set (metadata + commit +
# generation) from the driver's own encoder, so the QEMU rig can boot an
# array the C encoder produced rather than mkmeta.py's Python output.
STAMP_TOOL_BIN ?= $(CURDIR)/.metabuild-test/rcstamp

stamp-tool: $(STAMP_TOOL_BIN)

$(STAMP_TOOL_BIN): src/stamp_meta_userspace.c src/rc_meta_build.h
	@mkdir -p $(dir $(STAMP_TOOL_BIN))
	@echo "Building RAIDCore metadata stamping tool (userspace, no kernel headers)..."
	$(CC) -O2 -Wall -Wextra -Werror -o $(STAMP_TOOL_BIN) \
		src/stamp_meta_userspace.c -Isrc

test-metabuild: $(METABUILD_TEST_BIN)
	@rm -rf $(METABUILD_TEST_OUT)
	@mkdir -p $(METABUILD_TEST_OUT)
	@$(METABUILD_TEST_BIN) $(METABUILD_TEST_OUT)
	@python3 scripts/qemu-test/check-metabuild.py $(METABUILD_TEST_OUT)

$(METABUILD_TEST_BIN): $(METABUILD_TEST_DEPS)
	@mkdir -p $(dir $(METABUILD_TEST_BIN))
	@echo "Building RAIDCore metadata encoder harness (userspace, no kernel headers)..."
	$(CC) -O1 -g -Wall -Wextra -Werror -o $(METABUILD_TEST_BIN) \
		$(METABUILD_TEST_SRC) -Isrc

# Help target
help:
	@echo "AMD RAID Driver for Linux"
	@echo "Targets:"
	@echo "  all            - Build the driver"
	@echo "  clean          - Clean build files"
	@echo "  install        - Install the driver"
	@echo "  test-async      - Build and run the async engine harness (userspace)"
	@echo "  test-async-asan - Same, under AddressSanitizer + UBSan + LeakSanitizer"
	@echo "  test-async-tsan - Same, under ThreadSanitizer"
	@echo "  test-cfgwrite   - Build and run the RC_CfgWrite decoder harness (userspace)"
	@echo "  test-metabuild  - Diff the metadata encoder against mkmeta.py (userspace)"
	@echo "  stamp-tool      - Build the image-set stamping tool (userspace)"
	@echo "  help            - Show this help"

.PHONY: all clean install help test-async test-async-asan test-async-tsan \
	test-cfgwrite test-metabuild stamp-tool
