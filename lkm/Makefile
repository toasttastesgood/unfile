# Makefile for ext4_timestomp LKM + userspace ctl tool
#
# Usage:
#   make            — build kernel module + userspace client
#   make modules    — build kernel module only
#   make ctl        — build userspace client only
#   make clean      — remove build artifacts
#
# The build auto-detects ext4_inode_info field offsets via BTF (pahole) so
# crtime patching works without hard-coding struct layout.

KVER    ?= $(shell uname -r)
KDIR    ?= /lib/modules/$(KVER)/build
PWD     := $(shell pwd)

# --------------------------------------------------------------------------
# BTF / pahole offset detection
# --------------------------------------------------------------------------
# We need two offsets from struct ext4_inode_info:
#   EXT4_CRTIME_OFFSET     — byte offset of i_crtime (struct timespec64)
#   EXT4_VFS_INODE_OFFSET  — byte offset of vfs_inode (struct inode)
#
# Strategy 1: pahole + /sys/kernel/btf/vmlinux (preferred, most accurate)
# Strategy 2: pahole + vmlinux binary (if available)
# Strategy 3: pahole + kernel headers (compile a probe struct)
# Strategy 4: hardcoded defaults for common kernel versions (fallback)
#
# The detected values are passed as CFLAGS to the module build.

BTF_FILE    := /sys/kernel/btf/vmlinux
VMLINUX     := /boot/vmlinux-$(KVER)
PAHOLE      := $(shell which pahole 2>/dev/null)

# Attempt BTF-based detection
define get_offset_btf
$(shell $(PAHOLE) --btf_base=$(1) -C ext4_inode_info 2>/dev/null | \
	awk '/$(2)[[:space:]]*;/{gsub(/.*\//,"",$$0); match($$0,/\/\* *([0-9]+)/,a); print a[1]}')
endef

# Attempt pahole from kernel headers (generate minimal probe)
define get_offset_hdr
$(shell \
	TMPDIR=$$(mktemp -d) && \
	cat > $$TMPDIR/probe.c << 'EOF' \
#include <linux/fs.h> \
#include "$(KDIR)/include/linux/ext4_fs.h" \
int main(){} \
EOF \
	pahole $$TMPDIR/probe.c 2>/dev/null | \
	awk '/$(2)[[:space:]]*;/{match($$0,/\/\* *([0-9]+)/,a); print a[1]}' && \
	rm -rf $$TMPDIR)
endef

# Try to get offsets
ifeq ($(PAHOLE),)
  $(info [ext4ts] pahole not found — using fallback offsets)
  CRTIME_OFFSET    :=
  VFS_INODE_OFFSET :=
else ifeq ($(wildcard $(BTF_FILE)),$(BTF_FILE))
  CRTIME_OFFSET    := $(call get_offset_btf,$(BTF_FILE),i_crtime)
  VFS_INODE_OFFSET := $(call get_offset_btf,$(BTF_FILE),vfs_inode)
  $(info [ext4ts] BTF: i_crtime=$(CRTIME_OFFSET) vfs_inode=$(VFS_INODE_OFFSET))
else ifeq ($(wildcard $(VMLINUX)),$(VMLINUX))
  CRTIME_OFFSET    := $(call get_offset_btf,$(VMLINUX),i_crtime)
  VFS_INODE_OFFSET := $(call get_offset_btf,$(VMLINUX),vfs_inode)
  $(info [ext4ts] vmlinux BTF: i_crtime=$(CRTIME_OFFSET) vfs_inode=$(VFS_INODE_OFFSET))
else
  $(info [ext4ts] No BTF source found — crtime patching will be disabled)
  CRTIME_OFFSET    :=
  VFS_INODE_OFFSET :=
endif

# Build the -D flags only when offsets were found
OFFSET_FLAGS :=
ifneq ($(CRTIME_OFFSET),)
ifneq ($(VFS_INODE_OFFSET),)
  OFFSET_FLAGS := -DEXT4_CRTIME_OFFSET=$(CRTIME_OFFSET) \
                  -DEXT4_VFS_INODE_OFFSET=$(VFS_INODE_OFFSET)
endif
endif

# --------------------------------------------------------------------------
# Kernel module build
# --------------------------------------------------------------------------
obj-m := ext4_timestomp.o

ccflags-y := $(OFFSET_FLAGS)

.PHONY: all modules ctl clean load unload

all: modules ctl

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) \
		EXTRA_CFLAGS="$(OFFSET_FLAGS)" \
		modules

# --------------------------------------------------------------------------
# Userspace control tool
# --------------------------------------------------------------------------
ctl: ext4ts_ctl

ext4ts_ctl: ext4ts_ctl.c ext4ts.h
	$(CC) $(CFLAGS) -O2 -Wall -Wextra -o $@ $<

# --------------------------------------------------------------------------
# Convenience targets
# --------------------------------------------------------------------------
load: modules
	sudo insmod ext4_timestomp.ko

unload:
	sudo rmmod ext4_timestomp || true

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ext4ts_ctl
