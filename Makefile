LINUX ?= /lib/modules/$(shell uname -r)/build

zio-y := core.o chardev.o sysfs.o misc.o
zio-y += bus.o objects.o helpers.o
zio-y += buffers/zio-buf-kmalloc.o triggers/zio-trig-user.o

# Waiting for Kconfig...
CONFIG_ZIO_SNIFF_DEV:=y

zio-$(CONFIG_ZIO_SNIFF_DEV) += sniff-dev.o

obj-m = zio.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

obj-m += tools/

# WARNING: the line below doesn't work in-kernel if you compile with O=
EXTRA_CFLAGS += -I$(obj)/include/

all: modules tools

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)

modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) $@


coccicheck:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) coccicheck


.PHONY: tools

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools clean
