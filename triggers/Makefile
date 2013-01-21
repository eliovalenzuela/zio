LINUX ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -I$(obj)/../include/

# zio-trig-user.o is now part of zio-core
obj-m = zio-trig-timer.o
#obj-m += zio-trig-irq.o
#ifdef CONFIG_HIGH_RES_TIMERS
#obj-m += zio-trig-hrt.o
#endif
