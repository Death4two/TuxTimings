obj-m := aod_voltages.o

KVER  ?= $(shell uname -r)
KDIR  ?= /lib/modules/$(KVER)/build
PWD   := $(shell pwd)

# Auto-detect if the running kernel was built with Clang.
# CachyOS and some other distros use clang; mainline Arch/Debian use gcc.
KCONFIG := /lib/modules/$(KVER)/build/.config
IS_CLANG := $(shell grep -c "^CONFIG_CC_IS_CLANG=y" $(KCONFIG) 2>/dev/null || echo 0)

ifeq ($(IS_CLANG),1)
    LLVM_FLAG := LLVM=1
else
    LLVM_FLAG :=
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules $(LLVM_FLAG)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean $(LLVM_FLAG)
