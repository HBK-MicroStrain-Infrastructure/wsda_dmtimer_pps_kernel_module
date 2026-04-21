obj-m += dmtimer-pps.o

SRC := $(shell pwd)

ccflags-y += -I$(KERNEL_SRC)/arch/arm/plat-omap/include

all:
	make -C $(KERNEL_SRC) M=$(SRC) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
	rm -f Module.markers Module.symvers modules.order
	rm -rf .tmp_versions Modules.symvers