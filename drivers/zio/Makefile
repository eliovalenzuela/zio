# include parent_common.mk for buildsystem's defines
# It allows you to inherit an environment configuration from larger project
REPO_PARENT ?= ../../../
-include $(REPO_PARENT)/parent_common.mk

KVERSION ?= $(shell uname -r)
LINUX ?= /lib/modules/$(KVERSION)/build

GIT_VERSION := $(shell git describe --dirty --long --tags | grep -o -E "[^v]*")

# Extract major, minor and patch number
ZIO_VERSION := -D__ZIO_MAJOR_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 1 | cut -d '.' -f 1; )
ZIO_VERSION += -D__ZIO_MINOR_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 1 | cut -d '.' -f 2; )
ZIO_VERSION += -D__ZIO_PATCH_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 1 | cut -d '.' -f 3; )

export GIT_VERSION
export ZIO_VERSION

CONFIG_ZIO_DEVICES ?= m
CONFIG_ZIO_BUFFERS ?= m
CONFIG_ZIO_TRIGGERS ?= m

export CONFIG_ZIO_DEVICES
export CONFIG_ZIO_BUFFERS
export CONFIG_ZIO_TRIGGERS

all: modules

modules coccicheck modules_install clean:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) $@
