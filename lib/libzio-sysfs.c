/*
 * Copyright 2015 CERN
 * Author: Federico Vaga <federico.vaga@cern.ch>
 *
 * GNU GPLv3 or later
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "libzio.h"


/**
 * It counts the occurences of a character in a string
 *
 * @param[in] str string to use
 * @param[in] len length of the string
 * @param[in] ch character to find
 * @return the number of occurrences
 */
static int __str_count_occurrences(char *str, size_t len, char ch)
{
	int i, count = 0;

	for (i = 0; i < len; ++i)
		if (str[i] == ch)
			count++;
	return count;
}


int __uzio_attr_raw_get(struct uzio_attribute *attr,
			 char *buffer, unsigned int n)
{
	int fd, ret;

	fd = open(attr->path, O_RDONLY);
	if (fd < 0)
		return -1;
	ret = read(fd, buffer, n);
	close(fd);
	return ret;
}

int __uzio_attr_raw_set(struct uzio_attribute *attr,
			 char *buffer, unsigned int n)
{
	int fd, ret;

	fd = open(attr->path, O_WRONLY);
	if (fd < 0)
		return -1;
	ret = write(fd, buffer, n);
	close(fd);
	if (ret < 0)
		return -1;
	if (ret == n)
		return 0;
	errno = EIO; /* short write */
	return ret;
}


int uzio_attr_string_get(struct uzio_attribute *attr,
			 char *str, unsigned int n)
{
	int count;
	char *tmp;

	memset(str, 0, n);
        count = __uzio_attr_raw_get(attr, str, n);
	tmp = strchr(str, '\n');
	if (tmp)
		*tmp = '\0';

	return count;
}


int uzio_attr_string_set(struct uzio_attribute *attr,
			 char *str, unsigned int n)
{
	int count;

	count = __uzio_attr_raw_set(attr, str, n);
	if (count == n)
		return count;
	errno = EIO;
	return -1;
}


/**
 * It reads a ZIO attribute value
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_attr_value_get(struct uzio_attribute *attr, uint32_t *val)
{
	char buf[16];
	int ret;

	ret = __uzio_attr_raw_get(attr, buf, 16);
	if (ret < 0)
		return ret;

	ret = sscanf(buf, "%"SCNu32, val);
	if (ret !=1)
		return -1;
	return 0;
}


/**
 * It writes a ZIO attribute value
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_attr_value_set(struct uzio_attribute *attr, uint32_t val)
{
	char buf[16];
	int ret;

	sprintf(buf, "%"PRIu32, val);
	ret = __uzio_attr_raw_set(attr, buf, 16);
	if (ret < 0)
		return ret;
	return 0;
}

/**
 * Generic read/write function for the current control attribute
 * @param[in] chan zio channel that we are interested in
 * @param[in,out] ctrl according to the direction is the source or the destination
 * @param[in] flags used to control the behaviour of the function:
 *           * O_RDONLY to read
 *           * O_WRONLY to write
 * @return 0 on success, -1 otherwise and errno is set appropriately
 */
static int __uzio_ctrl_rw(struct uzio_channel *chan, struct zio_control * ctrl,
			  int flags)
{
	int i, err = 0;

	if (flags == O_RDONLY)
		i = __uzio_attr_raw_get(&chan->current_ctrl,
					(char *)ctrl, __ZIO_CONTROL_SIZE);
	else
		i = __uzio_attr_raw_set(&chan->current_ctrl,
					(char *)ctrl, __ZIO_CONTROL_SIZE);
	/* Check what happen during file I/O */
	switch (i) {
	case -1:
		errno = EIO;
		err = -1;
		break;
	case 0:
		errno = EIO;
		err = -1;
		break;
	default: /* FIXME usefull only for control */
		fprintf(stderr, "File I/O warn: %i bytes (expected %i)\n",
			i, __ZIO_CONTROL_SIZE);
		/* continue anyways */
	case __ZIO_CONTROL_SIZE:
		break; /* ok */
	}
	return err;
}


/**
 * Get the current control of a channel from the binary sysfs attribute
 *
 * @param[in] chan zio channel that we are interested in
 * @param[out] ctrl where store the current control
 * @return 0 on success, -1 otherwise and errno is set appropriately
 */
int uzio_ctrl_get(struct uzio_channel *chan, struct zio_control *ctrl)
{
	return __uzio_ctrl_rw(chan, ctrl, O_RDONLY);
}


/**
 * Get the current control of a channel from the binary sysfs attribute
 *
 * @param[in] chan zio channel that we are interested in
 * @param[in] ctrl zio control to copy into channel
 * @return 0 on success, -1 otherwise and errno is set appropriately
 */
int uzio_ctrl_set(struct uzio_channel *chan, struct zio_control *ctrl)
{
	return __uzio_ctrl_rw(chan, ctrl, O_WRONLY);
}


/**
 * It returns the list of avaiable modules from the correspondent
 * sysfs attribute. The library provieds wrappers for the most common lists:
 * uzio_device_list(), uzio_buffer_list(), uzio_trigger_list().
 *
 * @param[in] target the kind of module that you are looking for
 * @return a list of strings containing the modules' name. Otherwise NULL and
 *         errno is set appropriately
 */
struct uzio_module_list *uzio_module_list(const struct uzio_attribute *a)
{
	char buf[getpagesize()], *tok, *toksrc;
	struct uzio_module_list *list;
	int i, count;

	list = malloc(sizeof(struct uzio_module_list));
	if (!list)
	        goto out;

	count = __uzio_attr_raw_get((struct uzio_attribute *)a,
				    buf, getpagesize());
	if (count <= 0) {
		errno = EUZIONOMODLIST;
	        goto out_read;
	}

        list->len = __str_count_occurrences(buf, count, '\n');
	list->names = malloc(sizeof(char *) * list->len);
	if (!list->names)
		goto out_names;

	toksrc = buf;
	for (i = 0; i < list->len; ++i) {
		tok = strtok(toksrc, "\n");
		toksrc = NULL;
		if (!tok) {
			break;
		}

		list->names[i] = malloc(strlen(tok) + 1);
		if (!list->names[i])
			goto out_name_i;

		strcpy(list->names[i], tok);
	}

	return list;

out_name_i:
	while (--i) {
		free(list->names[i]);
	}
        free(list->names);
out_names:
out_read:
	free(list);
out:
	return NULL;
}


/**
 * Enable or disable a given ZIO instance
 * @param[in] zobj ZIO instance to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_object_enable(struct uzio_object *zobj, unsigned int enable)
{
	return uzio_attr_value_set(&zobj->enable, !!enable);
}



/**
 * Enable or disable a given device instance
 * @param[in] dev device to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_device_enable(struct uzio_device *dev, unsigned int enable)
{
	return uzio_object_enable(&dev->head, enable);
}


/**
 * Enable or disable a given channel-set instance
 * @param[in] cset channel-set to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_cset_enable(struct uzio_cset *cset, unsigned int enable)
{
	return uzio_object_enable(&cset->head, enable);
}


/**
 * Enable or disable a given channel instance
 * @param[in] chan channel to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_channel_enable(struct uzio_channel *chan, unsigned int enable)
{
	return uzio_object_enable(&chan->head, enable);
}
