TOP_DIR ?= ..
DRIVER_NAME := zio
VERSION := $(shell git describe --abbrev=0 | grep -o -E "[^v]*")
DIR_NAME := $(DRIVER_NAME)-$(VERSION)
KEEP_TEMP ?= n
BUILD ?= $(abspath build)
BUILD_DKMS := $(BUILD)/dkms
BUILD_DKMSSOURCE := $(BUILD_DKMS)/source
BUILD_DKMSTREE := $(BUILD_DKMS)/tree


SRC := $(TOP_DIR)/Makefile
SRC += $(TOP_DIR)/Makefile

DKMS_OPT := --dkmstree $(BUILD_DKMSTREE) -m $(DRIVER_NAME)/$(VERSION)

all: kernel

kernel: dkms-tar dkms-rpm

dkms-tree:
	@mkdir -p $(BUILD_DKMSSOURCE)
	@mkdir -p $(BUILD_DKMSTREE)

dkms-src: dkms-tree
	$(eval $@_dir := $(BUILD_DKMSSOURCE)/$(DRIVER_NAME)-$(VERSION))
	@mkdir -p $($@_dir)
	@mkdir -p $($@_dir)/buffers
	@mkdir -p $($@_dir)/triggers

	@cp $(TOP_DIR)/distribution/dkms.conf $($@_dir)
	@cp $(TOP_DIR)/distribution/module-symvers-save $($@_dir)
	@$(MAKE) -C $(TOP_DIR)/drivers/zio/ clean
	@cp -a $(TOP_DIR)/include $($@_dir)
	@cp -a $(TOP_DIR)/drivers/zio/* $($@_dir)

	@cp $(TOP_DIR)/LICENSES/GPL-2.0-or-later.txt $($@_dir)/LICENSE
	@sed -r -i -e "s/^GIT_VERSION\s:=\s.*/GIT_VERSION = $(VERSION)/" $($@_dir)/Makefile
	@sed -r -i -e "s/\.\.\/\.\.\/include/include/" $($@_dir)/Kbuild $($@_dir)/devices/Makefile $($@_dir)/buffers/Makefile $($@_dir)/triggers/Makefile

	@sed -r -i -e "s/@PKGNAME@/$(DRIVER_NAME)/g" -e "s/@PKGVER@/$(VERSION)/g" $($@_dir)/dkms.conf


dkms-add: dkms-src
	@dkms add $(DKMS_OPT) --sourcetree $(BUILD_DKMSSOURCE)

dkms-tar: dkms-add
	@dkms mktarball $(DKMS_OPT) --source-only

dkms-rpm: dkms-add
	@dkms mkrpm $(DKMS_OPT) --source-only

clean:
	@rm -rf $(BUILD)

.PHONY: dkmstree dkms-add kernel-dkms-tar
