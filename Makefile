# AMD RAID Driver for Linux
# Based on Windows driver architecture (rcbottom, rccfg, rcraid)

obj-m += rcraid.o

rcraid-objs := rc_main.o rc_bottom.o rc_config.o rc_raid.o

# Kernel build directory
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Compiler flags
EXTRA_CFLAGS += -Wall -Wextra -Werror
EXTRA_CFLAGS += -DRC_DEBUG_LEVEL=$(RC_DEBUG_LEVEL)

# Build targets
all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

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
