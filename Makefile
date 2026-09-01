# Makefile — neko_drv kernel module
# Target: Android GKI 6.1.x (arm64)
#
# Used by GitHub Actions build.yml:
#   make -C $KERNELDIR/out M=$(pwd) ARCH=arm64 CC=clang LLVM=1 modules

obj-m := neko_drv.o

# Include this directory so neko_shm.h resolves for both kernel and userspace
ccflags-y += -I$(src)

# Strip debug info — reduces .ko size and removes readable strings
ccflags-y += -g0 -fno-asynchronous-unwind-tables -fno-stack-protector

# Suppress common GKI out-of-tree warnings
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-missing-declarations

# ── build target ──────────────────────────────────────────────────────────
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
ARCH      ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CC        ?= clang

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		CC=$(CC) LLVM=1 LLVM_IAS=1 \
		modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) \
		ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		CC=$(CC) LLVM=1 LLVM_IAS=1 \
		clean
	rm -f *.ko.sh
