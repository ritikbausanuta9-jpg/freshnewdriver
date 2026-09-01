# Makefile — neko_drv kernel module
# Build for Android GKI 6.1.x (arm64)
#
# Usage:
#   make KERNELDIR=/path/to/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang
#
# In GitHub Actions these are set automatically by the workflow.

obj-m := neko_drv.o

# Extra include so both driver and userspace share neko_shm.h from this dir
ccflags-y += -I$(src)

# Suppress modpost "missing MODULE_DEVICE_TABLE" warning
ccflags-y += -Wno-missing-declarations

# Strip debug info for production (smaller, less strings visible in strings(1))
ccflags-y += -g0 -fno-asynchronous-unwind-tables

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
ARCH      ?= arm64

# Clang cross-compile (same toolchain Google uses for GKI)
CROSS_COMPILE ?= aarch64-linux-gnu-
CC            ?= clang

# --------------------------------------------------------------------------
all:
	$(MAKE) -C $(KERNELDIR)                                              \
		M=$(CURDIR)                                                       \
		ARCH=$(ARCH)                                                      \
		CROSS_COMPILE=$(CROSS_COMPILE)                                    \
		CC=$(CC)                                                          \
		LLVM=1 LLVM_IAS=1                                                \
		modules

clean:
	$(MAKE) -C $(KERNELDIR)                                              \
		M=$(CURDIR)                                                       \
		ARCH=$(ARCH)                                                      \
		CROSS_COMPILE=$(CROSS_COMPILE)                                    \
		CC=$(CC)                                                          \
		LLVM=1 LLVM_IAS=1                                                \
		clean
	rm -f *.ko.sh
