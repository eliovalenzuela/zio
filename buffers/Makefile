LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/

# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
obj-m += zio-buf-sock.o

zio-buf-sock-objs = sock-syscall.o sock-buffer.o sock-nic.o
