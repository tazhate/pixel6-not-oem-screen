# Makefile for ftm5_kprobe kernel module
#
# Build:
#   make                 # uses default kernel headers path
#   make KDIR=/path/to/kernel  # custom kernel tree
#
# Cross-compile for Pixel 6 (ARM64):
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#     KDIR=/home/taz/repos/ftm5-patch/kernel-headers/common

obj-m := ftm5_kprobe.o

# Default kernel headers (android14-6.1 for Pixel 6)
KDIR ?= /home/taz/repos/ftm5-patch/kernel-headers/common
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := clang-18
LD := ld.lld-18
AR := llvm-ar-18
NM := llvm-nm-18
STRIP := llvm-strip-18
OBJCOPY := llvm-objcopy-18
OBJDUMP := llvm-objdump-18

PWD := $(shell pwd)

MAKE_ARGS := ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	CC=$(CC) LD=$(LD) AR=$(AR) NM=$(NM) \
	STRIP=$(STRIP) OBJCOPY=$(OBJCOPY) OBJDUMP=$(OBJDUMP)

all:
	$(MAKE) $(MAKE_ARGS) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) $(MAKE_ARGS) -C $(KDIR) M=$(PWD) clean

# Deploy to device
push:
	adb push ftm5_kprobe.ko /data/local/tmp/

# Load module on device
load:
	adb shell "su -c 'insmod /data/local/tmp/ftm5_kprobe.ko'"

# Force-load (bypass vermagic check)
force-load:
	adb shell "su -c 'insmod -f /data/local/tmp/ftm5_kprobe.ko'"

# Unload module
unload:
	adb shell "su -c 'rmmod ftm5_kprobe'"

# Check dmesg for module output
check:
	adb shell "su -c 'dmesg'" | grep ftm5_kprobe

# Check kprobe status
status:
	adb shell "su -c 'cat /sys/kernel/debug/kprobes/list'" | grep fts_

# Full deploy cycle
deploy: all push force-load check

.PHONY: all clean push load force-load unload check status deploy
