# Makefile — neko_drv advanced kernel module
# Build for Android GKI 6.1.x (arm64)
#
# Usage (manual):
#   make KERNELDIR=/path/to/kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang
#
# In GitHub Actions: all vars set automatically by build.yml

obj-m := neko_drv.o

# Include path for neko_shm.h (same directory as driver)
ccflags-y += -I$(src)

# Optimization: strip debug info, enable O2 (smaller + faster binary)
ccflags-y += -g0 -O2 -fno-asynchronous-unwind-tables

# Suppress noisy warnings
ccflags-y += -Wno-unused-variable -Wno-missing-declarations

# GKI 6.1 specific: allow VMA iterator API
ccflags-y += -DCONFIG_MMU=1

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
ARCH      ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CC            ?= clang

all:
	$(MAKE) -C $(KERNELDIR)        \
		M=$(CURDIR)                 \
		ARCH=$(ARCH)                \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		CC=$(CC)                    \
		LLVM=1 LLVM_IAS=1          \
		modules

clean:
	$(MAKE) -C $(KERNELDIR)        \
		M=$(CURDIR)                 \
		ARCH=$(ARCH)                \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		CC=$(CC)                    \
		LLVM=1 LLVM_IAS=1          \
		clean
	rm -f *.ko.sh
