# AMD RAID Driver for Linux
# Based on Windows driver architecture (rcbottom, rccfg, rcraid)

obj-m += rcraid.o

rcraid-objs := rc_main.o rc_bottom.o rc_config.o rc_raid.o rc_blk.o rc_hw.o rc_metadata.o rc_queue.o

# Kernel build directory
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Compiler flags
EXTRA_CFLAGS += -Wall -Wextra
EXTRA_CFLAGS += -Wno-error
EXTRA_CFLAGS += -DRC_DEBUG_LEVEL=$(RC_DEBUG_LEVEL)

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
