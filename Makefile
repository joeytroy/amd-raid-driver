#
# Copyright © 2006-2008 Ciprico Inc. All rights reserved.
# Copyright © 2008-2013 Dot Hill Systems Corp. All rights reserved.
# Copyright © 2024 AMD RAID Driver DKMS Package
#
# Use of this software is subject to the terms and conditions of the written
# software license agreement between you and DHS (the "License"),
# including, without limitation, the following (as further elaborated in the
# License):  (i) THIS SOFTWARE IS PROVIDED "AS IS", AND DHS DISCLAIMS
# ANY AND ALL WARRANTIES OF ANY KIND, WHETHER EXPRESS, IMPLIED, STATUTORY,
# BY CONDUCT, OR OTHERWISE; (ii) this software may be used only in connection
# with the integrated circuit product and storage software with which it was
# designed to be used; (iii) this source code is the confidential information
# of DHS and may not be disclosed to any third party; and (iv) you may not
# make any modification or take any action that would cause this software,
# or any other Dot Hill software, to fall under any GPL license or any other
# open source license.
#

# Build configuration
RC_HOST=$(shell /bin/hostname)
RC_USER=$(shell whoami)
RC_DATE=$(shell /bin/date)
RC_BUILD_DATE=$(shell /bin/date +'%b %d %Y')
PLATFORM=$(shell uname -m)

# Detect Linux distribution
DISTRO := $(shell if [ -f /etc/os-release ]; then . /etc/os-release && if [ "$$ID" = "kubuntu" ]; then echo "ubuntu"; else echo $$ID; fi; elif [ -f /etc/redhat-release ]; then echo "rhel"; elif [ -f /etc/SuSE-release ]; then echo "suse"; else echo "unknown"; fi)
DISTRO_VERSION := $(shell if [ -f /etc/os-release ]; then . /etc/os-release && echo $$VERSION_ID; elif [ -f /etc/redhat-release ]; then grep -oE '[0-9]+' /etc/redhat-release | head -1; elif [ -f /etc/SuSE-release ]; then awk '/^VERSION/ { print $$3 }' /etc/SuSE-release; else echo "unknown"; fi)

# Kernel version detection
KERNEL_VERSION := $(shell uname -r)
KERNEL_MAJOR := $(shell echo $(KERNEL_VERSION) | cut -d. -f1)
KERNEL_MINOR := $(shell echo $(KERNEL_VERSION) | cut -d. -f2)

# Base configuration
EXTRA_CFLAGS += -D__LINUX__
EXTRA_CFLAGS += -DRC_AHCI_SUPPORT -DRC_AMD_AHCI -DRC_AHCI_AUTOSENSE
EXTRA_CFLAGS += -DRC_RAW_SPTD
EXTRA_CFLAGS += -DRC_RAW_PASSTHROUGH
EXTRA_CFLAGS += -DRC_LSI1068
EXTRA_CFLAGS += -DRC_MPT2
EXTRA_CFLAGS += -DRC_DETECT_AND_BLOCK_PROMISE_RAID
EXTRA_CFLAGS += -DRC_DRIVER_BUILD_DATE='"${RC_BUILD_DATE}"'

# Kernel version specific flags
ifeq ($(shell test $(KERNEL_MAJOR) -ge 7; echo $$?),0)
    EXTRA_CFLAGS += -DKERNEL_7_PLUS
endif

ifeq ($(shell test $(KERNEL_MAJOR) -ge 6; echo $$?),0)
    EXTRA_CFLAGS += -DKERNEL_6_PLUS
    # Kernel 6.8+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 8; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_8_PLUS
    endif
    # Kernel 6.9+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 9; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_9_PLUS
    endif
    # Kernel 6.10+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 10; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_10_PLUS
    endif
    # Kernel 6.11+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 11; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_11_PLUS
    endif
    # Kernel 6.12+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 12; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_12_PLUS
    endif
    # Kernel 6.13+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 13; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_13_PLUS
    endif
    # Kernel 6.14+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 14; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_14_PLUS
    endif
    # Kernel 6.15+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 15; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_15_PLUS
    endif
    # Kernel 6.16+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 16; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_16_PLUS
    endif
    # Kernel 6.17+ specific optimizations
    ifeq ($(shell test $(KERNEL_MAJOR) -eq 6 -a $(shell echo $(KERNEL_VERSION) | cut -d. -f2) -ge 17; echo $$?),0)
        EXTRA_CFLAGS += -DKERNEL_6_17_PLUS
    endif
endif

ifeq ($(shell test $(KERNEL_MAJOR) -ge 5; echo $$?),0)
    EXTRA_CFLAGS += -DKERNEL_5_PLUS
endif

# Distribution specific flags
ifeq ($(DISTRO),ubuntu)
    EXTRA_CFLAGS += -DUBUNTU_BUILD
endif

ifeq ($(DISTRO),debian)
    EXTRA_CFLAGS += -DDEBIAN_BUILD
endif

ifeq ($(DISTRO),rhel)
    EXTRA_CFLAGS += -DRHEL_BUILD
endif

ifeq ($(DISTRO),suse)
    EXTRA_CFLAGS += -DSUSE_BUILD
endif

ifeq ($(DISTRO),arch)
    EXTRA_CFLAGS += -DARCH_BUILD
endif

ifeq ($(DISTRO),fedora)
    EXTRA_CFLAGS += -DFEDORA_BUILD
endif

# Build against an installed kernel object tree.
# Either set the kernel version here or pass in the version.
# Defaults to building for the currently running system.
# examples:  make KVERS=2.6.20-1.2933.fc6
#            make KVERS=2.6.18-1.25-smp

ifndef KVERS
	KVERS=$(shell uname -r)
endif

# either set path to the kernel build tree here or pass in the directory
ifndef KDIR
	KDIR    := /lib/modules/$(KVERS)/build
endif

ifdef KBUILD_SRC
	ifneq ($(shell grep --quiet "irq_handler_t" $(srctree)/include/linux/interrupt.h && echo yes), yes)
		EXTRA_CFLAGS += -DNO_IRQ_HANDLER_T
	endif
endif

ifneq ($(RUN_DEPMOD),)
	DEPMOD := /sbin/depmod -a
else
	DEPMOD := true
endif

PWD    := $(shell pwd)
SRC_DIR := $(PWD)/src
SCRIPTS_DIR := $(PWD)/scripts
TESTS_DIR := $(PWD)/tests

.PHONY: install

all: module

module:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(RM) -f *.o *.ko vers.c .*.cmd .*.d
	$(RM) -f rcraid.mod.c Module.symvers Modules.symvers
	$(RM) -rf .tmp_versions Module.markers modules.order
	$(RM) -f src/*.o src/.*.cmd src/.*.d

test:
	@echo "Running rcraid driver tests..."
	@$(TESTS_DIR)/test.sh

test-basic: module
	@echo "Running basic functionality tests..."
	@if [ -f rcraid.ko ]; then \
		echo "✓ Driver module built successfully"; \
		modinfo rcraid.ko > /dev/null && echo "✓ Module info valid"; \
		modprobe -n rcraid > /dev/null 2>&1 && echo "✓ Module can be loaded" || echo "⚠ Module loading test skipped (requires root)"; \
	else \
		echo "✗ Driver module not found - run 'make' first"; \
		exit 1; \
	fi

install: module
	@echo "Installing rcraid driver for $(DISTRO) $(DISTRO_VERSION) on kernel $(KVERS)"
	@if [ -e /etc/redhat-release ]; then                            \
	    echo "Performing Red Hat/CentOS/Fedora install";            \
	    (sh ./install_rh $(KVERS));                                 \
	elif [ -e /etc/SuSE-release ]; then                             \
	    echo "Performing SUSE install";                             \
	    (sh ./install_suse $(KVERS));                               \
	elif [ -e /etc/debian_version ]; then                           \
	    echo "Performing Debian/Ubuntu install";                    \
	    (sh $(SCRIPTS_DIR)/install_debian $(KVERS));                \
	elif [ -e /etc/arch-release ]; then                             \
	    echo "Performing Arch Linux install";                       \
	    (sh $(SCRIPTS_DIR)/install_arch $(KVERS));                  \
	else                                                            \
	    echo "Performing generic Linux install";                    \
	    (sh $(SCRIPTS_DIR)/install_generic $(KVERS));               \
	fi

install-dkms: module
	@echo "Installing via DKMS for $(DISTRO) $(DISTRO_VERSION)"
	@if command -v dkms >/dev/null 2>&1; then                       \
	    dkms add -m rcraid -v 8.1.0 --force;                       \
	    dkms build -m rcraid -v 8.1.0;                             \
	    dkms install -m rcraid -v 8.1.0 --force;                   \
	else                                                            \
	    echo "DKMS not found, falling back to manual install";     \
	    $(MAKE) install;                                            \
	fi

uninstall:
	@echo "Uninstalling rcraid driver"
	@if command -v dkms >/dev/null 2>&1; then                       \
	    dkms remove -m rcraid -v 8.1.0 --all;                      \
	else                                                            \
	    (sh $(SCRIPTS_DIR)/uninstall_generic $(KVERS));              \
	fi

obj-m := rcraid.o

rcraid-objs := src/rc_init.o \
               src/rc_msg.o \
               src/rc_mem_ops.o \
               src/rc_event.o \
               src/rc_config.o \
               src/rcblob.${PLATFORM}.o \
	       vers.o

.PHONY:	$(obj)/vers.c
$(obj)/vers.c:
	@echo "char *rc_ident = \"built on $(RC_HOST) by $(RC_USER) on $(RC_DATE)\";" > $@

# hack to avoid warning about missing .rcblob.cmd file when modpost tries to
# find all the sources
.PHONY: $(obj)/rcblob.${PLATFORM}.o
$(obj)/rcblob.${PLATFORM}.o:
	ln -sf `basename $@ .o` $@
	@( echo "cmd_$@ := true"; echo "dep_$@ := \\"; echo "	$@ \\"; echo "" ) > $(obj)/.`basename $@`.cmd
