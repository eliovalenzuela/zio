LINUX ?= /lib/modules/$(shell uname -r)/build

zio-core-objs := zio-cdev.o zio-sys.o
obj-m = zio-core.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

obj-m += tools/

# WARNING: the line below doesn't work in-kernel if you compile with O=
EXTRA_CFLAGS += -I$(obj)/include/

EXTRA_CFLAGS += -DDEBUG

all: modules tools

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)

.PHONY: tools

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools clean
