KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
obj-m           += mundo.o

ccflags-y := -std=gnu99

.PHONY: all clean load unload install

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean

load:
	sudo insmod mundo.ko

unload:
	sudo rmmod mundo

install:
	./install.sh
