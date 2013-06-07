LINUX ?= /lib/modules/$(shell uname -r)/build

GIT_VERSION = $(shell cd $(src); git describe --dirty --long --tags)

EXTRA_CFLAGS += -I$(obj)/../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"


# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
