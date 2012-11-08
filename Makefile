LINUX ?= /lib/modules/$(shell uname -r)/build

zio-objs := zio-core.o zio-cdev.o zio-sys.o zio-misc.o
zio-objs += buffers/zio-buf-kmalloc.o triggers/zio-trig-user.o

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


.PHONY: tools

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools clean
