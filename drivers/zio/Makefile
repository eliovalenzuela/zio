# include parent_common.mk for buildsystem's defines
# It allows you to inherit an environment configuration from larger project
REPO_PARENT ?= ../../../
-include $(REPO_PARENT)/parent_common.mk

LINUX ?= /lib/modules/$(shell uname -r)/build

GIT_VERSION := $(shell git describe --dirty --long --tags)

# Extract major, minor and patch number
ZIO_VERSION := -D__ZIO_MAJOR_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 2 | cut -d '.' -f 1; )
ZIO_VERSION += -D__ZIO_MINOR_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 2 | cut -d '.' -f 2; )
ZIO_VERSION += -D__ZIO_PATCH_VERSION=$(shell echo $(GIT_VERSION) | cut -d '-' -f 3)

export GIT_VERSION
export ZIO_VERSION

all: modules

modules coccicheck modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd) $@
