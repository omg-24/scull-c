# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the SCULL C kernel module (out-of-tree Kbuild)
#
# Usage:
#   make              — build scull.ko against the running kernel
#   make clean        — remove all build artefacts
#   make load         — insert the module (requires sudo / root)
#   make unload       — remove the module (requires sudo / root)
#   make test_prog    — compile the userspace test program
#   make test         — load + run userspace tests (requires root)
#
# The KDIR variable points to the kernel build directory.
# It defaults to the headers for the currently running kernel.
# Override on the command line if cross-compiling:
#   make KDIR=/path/to/linux-headers-<version>/build

KDIR ?= /lib/modules/$(shell uname -r)/build

# ── Kbuild object list ────────────────────────────────────────────────────────
# This line is read by the Kbuild system (not by GNU make directly).
# It tells Kbuild to compile scull.c → scull.o → scull.ko.
obj-m += scull.o

# ── Module name (used by load/unload targets) ─────────────────────────────────
MODULE_NAME := scull

# ── Default target ────────────────────────────────────────────────────────────
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_scull test_scull.o

# ── Load module ───────────────────────────────────────────────────────────────
# Creates /dev/scull via udev after insmod.
# If udev is slow, a brief sleep before testing may be needed.
load: all
	sudo insmod $(MODULE_NAME).ko
	@echo "Module loaded. Check: sudo dmesg | tail -20"
	@echo "Device node: ls -la /dev/scull"

# ── Unload module ─────────────────────────────────────────────────────────────
unload:
	sudo rmmod $(MODULE_NAME) || true
	@echo "Module unloaded. Check: sudo dmesg | tail -10"

# ── Compile userspace test program ───────────────────────────────────────────
test_prog: test_scull.c scull.h
	gcc -Wall -Wextra -o test_scull test_scull.c
	@echo "Userspace test compiled: ./test_scull"

# ── Run full test suite (requires root to load module) ───────────────────────
test: load test_prog
	sudo ./test_scull
	$(MAKE) unload

# ── Install (optional, requires root) ─────────────────────────────────────────
# Copies the .ko into the kernel's extra modules directory and runs depmod.
# Run 'sudo modprobe scull' after this.  Remove with 'sudo modprobe -r scull'.
INSTALL_MOD_PATH ?= /lib/modules/$(shell uname -r)/extra
install: all
	sudo mkdir -p $(INSTALL_MOD_PATH)
	sudo cp scull.ko $(INSTALL_MOD_PATH)/
	sudo depmod -a
	@echo "Installed to $(INSTALL_MOD_PATH)/scull.ko"
	@echo "Load with: sudo modprobe scull"

.PHONY: all clean load unload test_prog test install
