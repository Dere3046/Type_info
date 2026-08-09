obj-m := type_info.o
type_info-objs := lib/port.o lib/slide.o lib/btf.o lib/query.o lib/reg.o lib/lib.o lib/anchor.o lib/dwarf.o src/main.o src/verify.o kallrecon/lib/core.o kallrecon/lib/anchor.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/kallrecon/lib

ifeq ($(TI_PUBLIC_ANCHOR),1)
ccflags-y += -DCONFIG_TI_PUBLIC_ANCHOR
endif

ifeq ($(TI_FEATURE),1)
ccflags-y += -DCONFIG_TI_FEATURE
endif

ifeq ($(TI_DWARF),1)
ccflags-y += -DCONFIG_TI_DWARF
endif

ifeq ($(TI_DWARF_EXPORT),1)
ccflags-y += -DCONFIG_TI_DWARF
ccflags-y += -DCONFIG_TI_DWARF_EXPORT
endif

ifneq ($(TI_REMAP),0)
ccflags-y += -DCONFIG_TI_REMAP
endif

ifeq ($(KDIR),)
$(error KDIR must be set, e.g. "make KDIR=/path/to/kernel-source")
endif
PWD := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
