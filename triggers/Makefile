LINUX ?= /lib/modules/$(shell uname -r)/build

GIT_VERSION = $(shell cd $(src); git describe --dirty --long --tags)

EXTRA_CFLAGS += -I$(obj)/../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"

# zio-trig-user.o is now part of zio-core
obj-m = zio-trig-timer.o
#obj-m += zio-trig-irq.o
#ifdef CONFIG_HIGH_RES_TIMERS
#obj-m += zio-trig-hrt.o
#endif
