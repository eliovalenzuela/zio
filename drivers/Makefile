LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include

EXTRA_CFLAGS += -DDEBUG

obj-m = zio-zero.o
obj-m += zio-gpio.o
ifdef CONFIG_SPI
obj-m += zio-ad788x.o
endif
