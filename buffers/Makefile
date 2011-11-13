LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/
EXTRA_CFLAGS += -DDEBUG

# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
