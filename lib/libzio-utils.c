/*
 * Copyright 2015 CERN
 * Author: Federico Vaga <federico.vaga@cern.ch>
 *
 * GNU GPLv3 or later
 */

#include <stdlib.h>
#include <string.h>

#include "libzio.h"

static const char *uzio_errors[] = {
  "No ZIO device available",
  "Incompatible ZIO version",
  "Module list is not available",
  "Control block is not correct",
  "Invalid char-device",
  "Invalid device",
  "Invalid channel set",
  "Invalid channel",
  "Wrong I/O direction while using char-device",
  0,
};


void uzio_str_to_enum_type(enum zio_object_type *type, char *str,
			   unsigned int n)
{
	if (strncmp(str, zdev_device_type_name, n) == 0)
	        *type = ZIO_DEV;
	else if (strncmp(str, cset_device_type_name, n) == 0)
		*type = ZIO_CSET;
	else if (strncmp(str, chan_device_type_name, n) == 0)
		*type = ZIO_CHAN;
	else if (strncmp(str, ti_device_type_name, n) == 0)
		*type = ZIO_TI;
	else if (strncmp(str, bi_device_type_name, n) == 0)
		*type = ZIO_BI;
	else
		*type = ZIO_NONE;
}

void uzio_enum_to_str_type(char *str, enum zio_object_type type,
			   unsigned int n)
{
	switch (type) {
	case ZIO_DEV:
		strncpy(str, zdev_device_type_name, n);
		break;
	case ZIO_CSET:
		strncpy(str, cset_device_type_name, n);
		break;
	case ZIO_CHAN:
		strncpy(str, chan_device_type_name, n);
		break;
	case ZIO_TI:
		strncpy(str, ti_device_type_name, n);
		break;
	case ZIO_BI:
		strncpy(str, bi_device_type_name, n);
		break;
	default:
		strncpy(str, "N/A", n);
		break;
	}
}

/**
 * It retrieves the description of a given error code
 * @param[in] err error code
 * @return  a pointer to a string that describes the error code passed
 */
char *uzio_strerror(unsigned int err)
{
	if (err >= EUZIONODEV && err < __EUZIO_LAST_ERROR)
		return (char *)uzio_errors[err - EUZIONODEV];
	else
		return strerror(err);
}


/**
 * It releases the resources used by the uzio_module_list
 *
 * @param[in] list list to free
 */
void uzio_module_list_free(struct uzio_module_list *list)
{
	int i;

	if (!list)
		return;

	for (i = 0; i < list->len; ++i)
		free(list->names[i]);
	free(list->names);
	free(list);
}
