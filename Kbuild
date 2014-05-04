
zio-y := core.o chardev.o sysfs.o misc.o
zio-y += bus.o objects.o helpers.o dma.o config.o
zio-y += buffers/zio-buf-kmalloc.o triggers/zio-trig-user.o

# Waiting for Kconfig...
CONFIG_ZIO_SNIFF_DEV:=y

zio-$(CONFIG_ZIO_SNIFF_DEV) += sniff-dev.o

obj-m = zio.o
obj-m += drivers/
obj-m += buffers/
obj-m += triggers/

# src is defined byt the kernel Makefile, but we want to use it also in our
# local Makefile (tools, lib)

# add versions of supermodule. It is useful when ZIO is included as sub-module
# of a bigger project that we want to track
ifdef CONFIG_SUPER_REPO
ifdef CONFIG_SUPER_REPO_VERSION
SUBMODULE_VERSIONS += MODULE_INFO(version_$(CONFIG_SUPER_REPO),\"$(CONFIG_SUPER_REPO_VERSION)\");
endif
endif

ccflags-y += -DADDITIONAL_VERSIONS="$(SUBMODULE_VERSIONS)"

# WARNING: the line below doesn't work in-kernel if you compile with O=
ccflags-y += -I$(src)/include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-y += $(ZIO_VERSION)
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG
