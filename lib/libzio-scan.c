/*
 * Copyright 2015 CERN
 * Author: Federico Vaga <federico.vaga@cern.ch>
 *
 * GNU GPLv3 or later
 */

#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <glob.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "libzio.h"

static int __uzio_attributes_add(struct uzio_object *zobj)
{
	char pattern[UZIO_MAX_PATH_LEN];
	glob_t gattr;
	unsigned int i;

	snprintf(pattern, UZIO_MAX_PATH_LEN, "%s/standard/*", zobj->sysbase);
	glob(pattern, 0, NULL, &gattr);
	/* Standard Attributes */
	for (i = 0; i < gattr.gl_pathc && i < ZIO_MAX_STD_ATTR; ++i) {
		zobj->std[i].parent = zobj;
		strncpy(zobj->std[i].path, gattr.gl_pathv[i], UZIO_MAX_PATH_LEN);
	}
	globfree(&gattr);


	/* Extended Attributes */
	snprintf(pattern, UZIO_MAX_PATH_LEN, "%s/extended/*", zobj->sysbase);
	glob(pattern, 0, NULL, &gattr);
	for (i = 0; i < gattr.gl_pathc && i < ZIO_MAX_EXT_ATTR; ++i) {
		zobj->ext[i].parent = zobj;
		strncpy(zobj->ext[i].path, gattr.gl_pathv[i], UZIO_MAX_PATH_LEN);
	}
	globfree(&gattr);

	return 0;
}

static int __uzio_object_add(struct uzio_object *zobj)
{
	char tmp[ZIO_OBJ_NAME_FULL_LEN];
	int err = 0;

	snprintf(zobj->enable.path, UZIO_MAX_PATH_LEN,
		 "%s/enable", zobj->sysbase);
	zobj->enable.parent = zobj;

	snprintf(zobj->__name.path, UZIO_MAX_PATH_LEN,
		 "%s/name", zobj->sysbase);
	zobj->__name.parent = zobj;
	err = uzio_attr_string_get(&zobj->__name, zobj->name,
				   ZIO_OBJ_NAME_FULL_LEN);
	if (err < 0)
		return -1;

	snprintf(zobj->__devname.path, UZIO_MAX_PATH_LEN,
		 "%s/devname", zobj->sysbase);
	zobj->__devname.parent = zobj;
	err = uzio_attr_string_get(&zobj->__devname, zobj->devname,
				   ZIO_OBJ_NAME_FULL_LEN);
	if (err < 0)
		return -1;

	snprintf(zobj->__type.path, UZIO_MAX_PATH_LEN,
		 "%s/devtype", zobj->sysbase);
	zobj->__type.parent = zobj;
	err = uzio_attr_string_get(&zobj->__type, tmp, ZIO_OBJ_NAME_FULL_LEN);
	uzio_str_to_enum_type(&zobj->type, tmp, ZIO_OBJ_NAME_FULL_LEN);
	if (err < 0)
		return -1;

	return __uzio_attributes_add(zobj);
}

static int __uzio_device_cset_chan_buf_add(struct uzio_channel *chan)
{
	snprintf(chan->buffer.head.sysbase, UZIO_MAX_PATH_LEN,
		 "%s/buffer", chan->head.sysbase);

	chan->buffer.flush.parent = &chan->buffer.head;
	snprintf(chan->buffer.flush.path, UZIO_MAX_PATH_LEN,
		 "%s/flush", chan->buffer.head.sysbase);

	return __uzio_attributes_add(&chan->buffer.head);
}


static void __uzio_device_cset_chan_buf_del(struct uzio_channel *chan)
{
	return;
}

static int __uzio_device_cset_chan_add(struct uzio_cset *cset)
{
	struct uzio_channel *chan;
	char pattern[UZIO_MAX_PATH_LEN], path[UZIO_MAX_PATH_LEN];
	glob_t gchan;
	int err = 0, i, dir;

	snprintf(pattern, UZIO_MAX_PATH_LEN, "%s/chan*", cset->head.sysbase);
	glob(pattern, GLOB_ONLYDIR, NULL, &gchan);

	/* Create Channels */
	cset->n_chan = gchan.gl_pathc;
	cset->chan = malloc(sizeof(struct uzio_channel) * cset->n_chan);
	if (!cset->chan) {
		err = -1;
		goto out_alloc;
	}
	memset(cset->chan, 0, sizeof(struct uzio_channel) * cset->n_chan);
	for (i = 0; i < cset->n_chan; ++i) {
		chan = &cset->chan[i];
		strncpy(chan->head.sysbase, gchan.gl_pathv[i],
			UZIO_MAX_PATH_LEN);
		chan->head.parent = &cset->head;

		chan->current_ctrl.parent = &chan->head;
		snprintf(chan->current_ctrl.path, UZIO_MAX_PATH_LEN,
			 "%s/current_control", chan->head.sysbase);

		chan->alarms.parent = &chan->head;
		snprintf(chan->alarms.path, UZIO_MAX_PATH_LEN,
			 "%s/alarms", chan->head.sysbase);
		err = __uzio_object_add(&chan->head);
		if (err)
			goto out_err;
		if (chan->head.type != ZIO_CHAN) {
			errno = EUZIOICHAN;
			goto out_err;
		}

		dir = cset->flags & UZIO_CSET_FLAG_DIRECTION ?
			O_WRONLY : O_RDONLY;
		snprintf(path, UZIO_MAX_PATH_LEN, "/dev/zio/%s-data",
			 chan->head.devname);

		err = __uzio_device_cset_chan_buf_add(chan);
		if (err)
		        goto out_err;

		chan->fd_data = open(path, dir);
		if (chan->fd_data < 0)
			goto out_fd_data;
		snprintf(path, UZIO_MAX_PATH_LEN, "/dev/zio/%s-ctrl",
			 chan->head.devname);

		chan->fd_ctrl = open(path, dir);
		if (chan->fd_ctrl < 0)
			goto out_fd_ctrl;
	}
out_alloc:
	globfree(&gchan);

	return err;

out_fd_ctrl:
	close(chan[i].fd_data);
out_fd_data:
out_err:
	while(--i >= 0) {
		close(cset->chan[i].fd_ctrl);
		close(cset->chan[i].fd_data);
	}
	return -1;
}


static void __uzio_device_cset_chan_del(struct uzio_cset *cset)
{
	int i;

	for (i = 0; i < cset->n_chan; ++i) {
		close(cset->chan[i].fd_ctrl);
		close(cset->chan[i].fd_data);
		__uzio_device_cset_chan_buf_del(&cset->chan[i]);
	}
	free(cset->chan);
}

static int __uzio_device_cset_trig_add(struct uzio_cset *cset)
{
	snprintf(cset->trigger.head.sysbase, UZIO_MAX_PATH_LEN,
		 "%s/trigger", cset->head.sysbase);
	return __uzio_attributes_add(&cset->trigger.head);
}

static void __uzio_device_cset_trig_del(struct uzio_cset *cset)
{
	return;
}

#define STR_DIR 8
static int __uzio_device_cset_add(struct uzio_device *dev)
{
	struct uzio_cset *cset;
	char pattern[UZIO_MAX_PATH_LEN], dir[STR_DIR];
	glob_t gcset;
	int err, i;

	snprintf(pattern, UZIO_MAX_PATH_LEN, "%s/cset*", dev->head.sysbase);
	glob(pattern, GLOB_ONLYDIR, NULL, &gcset);

	/* Create Channel sets */
	dev->n_cset = gcset.gl_pathc;
	dev->cset = malloc(sizeof(struct uzio_cset) * dev->n_cset);
	if (!dev->cset)
		goto out_alloc;
	memset(dev->cset, 0, sizeof(struct uzio_cset) * dev->n_cset);
	for (i = 0; i < dev->n_cset; ++i) {
		cset = &dev->cset[i];
		strncpy(cset->head.sysbase, gcset.gl_pathv[i],
			UZIO_MAX_PATH_LEN);
		cset->head.parent = &dev->head;
		cset->current_trigger.parent = &cset->head;

		snprintf(cset->current_trigger.path, UZIO_MAX_PATH_LEN,
			 "%s/current_trigger", cset->head.sysbase);
		cset->current_buffer.parent = &cset->head;
		snprintf(cset->current_buffer.path, UZIO_MAX_PATH_LEN,
			 "%s/current_buffer", cset->head.sysbase);
		cset->direction.parent = &cset->head;
		snprintf(cset->direction.path, UZIO_MAX_PATH_LEN,
			 "%s/direction", cset->head.sysbase);

		cset->flags = 0;
		uzio_attr_string_get(&cset->direction, dir, STR_DIR);
		if (strncmp(dir, "output", STR_DIR) == 0)
			cset->flags |= UZIO_CSET_FLAG_DIRECTION_OUT;

		err = __uzio_object_add(&cset->head);
		if (err)
			goto out_obj;

		/* Look for channels */
		err = __uzio_device_cset_chan_add(cset);
		if (err)
			goto out_chan;
		if (cset->head.type != ZIO_CSET) {
			errno = EUZIOICSET;
		        goto out_val;
		}

		err = __uzio_device_cset_trig_add(cset);
		if (err)
		        goto out_trig;
	}
	globfree(&gcset);

	return 0;

out_trig:
out_val:
	i++; /* So we will remove the last channels in the while loop */
out_obj:
out_chan:
	while (--i >= 0)
		__uzio_device_cset_chan_del(&dev->cset[i]);
out_alloc:
	globfree(&gcset);
	return -1;
}


static void __uzio_device_cset_del(struct uzio_device *dev)
{
	int i;

	for (i = 0; i < dev->n_cset; ++i) {
		__uzio_device_cset_chan_del(&dev->cset[i]);
		__uzio_device_cset_trig_del(&dev->cset[i]);
	}
	free(dev->cset);
}


struct uzio_device *__uzio_device_open(char *name)
{
	struct uzio_device *dev;
	struct stat st;
	int err;
	ssize_t r;

	dev = malloc(sizeof(struct uzio_device));
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(struct uzio_device));

	strncpy(dev->head.sysbase, name, UZIO_MAX_PATH_LEN);

	err = stat(dev->head.sysbase, &st);
	if (err < 0) {
		errno = EUZIONODEV;
		goto out_stat;
	}

	if (S_ISLNK(st.st_mode)) {
		/* If it is a link, resolve it */
		r = readlink(dev->head.sysbase, dev->head.sysbase,
			     UZIO_MAX_PATH_LEN);
		printf("%s:%d %s\n", __func__, __LINE__, dev->head.sysbase);
		if (r < 0)
			goto out_stat;
	}

	err = stat(dev->head.sysbase, &st);
	if (err < 0) {
		errno = EUZIONODEV;
		goto out_stat;
	}
	if (!S_ISDIR(st.st_mode)) {
		errno = EUZIONODEV;
		goto out_stat;
	}

	err = __uzio_object_add(&dev->head);
	if (err)
		goto out_obj;
	if (dev->head.type != ZIO_DEV) {
		errno = EUZIOIDEV;
		goto out_val;
	}

	err = __uzio_attributes_add(&dev->head);
	if (err)
		goto out_attr;

	err = __uzio_device_cset_add(dev);
	if (err)
		goto out_dev;

	return dev;

out_dev:
out_attr:
out_val:
out_obj:
out_stat:
	free(dev);
	return NULL;
}

struct uzio_device *uzio_device_open(char *name, uint32_t dev_id)
{
	char path[UZIO_MAX_PATH_LEN];

	snprintf(path, UZIO_MAX_PATH_LEN, "%s/%s-0x%04x",
		 UZIO_SYS_DIR_DEV, name, dev_id);

	return __uzio_device_open(path);
}

struct uzio_device *uzio_device_open_by_name(char *name)
{
	char path[UZIO_MAX_PATH_LEN];

	snprintf(path, UZIO_MAX_PATH_LEN, "%s/%s",
		 UZIO_SYS_DIR_DEV, name);

	return __uzio_device_open(path);
}

void uzio_device_close(struct uzio_device *dev)
{
	__uzio_device_cset_del(dev);
	free(dev);
}


/**
 * It returns a list of available devices
 */
struct uzio_module_list *uzio_device_list(void)
{
	char pattern[UZIO_MAX_PATH_LEN], *name;
	struct uzio_module_list *list;
	glob_t devices;
	int i, d;

	list = malloc(sizeof(struct uzio_module_list));
	if (!list)
	        goto out;

	snprintf(pattern, UZIO_MAX_PATH_LEN, "%s/*", UZIO_SYS_DIR_DEV);
	glob(pattern, GLOB_ONLYDIR, NULL, &devices);

	/* Count real devices */
	list->len = 0;
	for (i = 0; i < devices.gl_pathc; ++i) {
		name = basename(devices.gl_pathv[i]);
		if (!strncmp("hw-", name, 3))
			continue; /* Not a real device */
		list->len++;
	}

	list->names = malloc(sizeof(char *) * list->len);
	if (!list->names)
		goto out_names;

	for (i = 0, d = 0; i < devices.gl_pathc && d < list->len; ++i) {
		name = basename(devices.gl_pathv[i]);
		if (!strncmp("hw-", name, 3))
			continue; /* Not a real device */

		list->names[d] = strdup(name);
		if (!list->names[d])
			goto out_name_i;
		d++;
	}

	globfree(&devices);

	return list;

out_name_i:
	while (--i) {
		free(list->names[i]);
	}
        free(list->names);
out_names:
	globfree(&devices);
	free(list);
out:
	return NULL;
}
