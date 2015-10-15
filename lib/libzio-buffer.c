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

const struct uzio_attribute available_buffers = {
	.parent = NULL,
	.path = UZIO_SYS_DIR"/available_buffers",
};


/**
 * It returns the list of available buffers.
 * @return the list of buffer modules. The structure must be free using
 *         the function uzio_module_list_free()
 */
struct uzio_module_list *uzio_buffer_list(void)
{
	return uzio_module_list(&available_buffers);
}


/**
 * It change buffer type for a given channel set
 * @param[in] cset channel set where act
 * @param[in] name buffer name to use
 * @param[in] n buffer name string lenght
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_buffer_change(struct uzio_cset *cset, char *name, unsigned int n)
{
	int ret;

	ret = uzio_attr_string_set(&cset->current_buffer, name, n);
	return ret < 0 ? -1 : 0;
}

/**
 * Enable or disable a given buffer instance
 * @param[in] buf buffer to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_buffer_enable(struct uzio_buffer *buf, unsigned int enable)
{
	return uzio_object_enable(&buf->head, enable);
}


/**
 * Flush a buffer channel
 * @param[in] chan channel to flush
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_buffer_flush(struct uzio_channel *chan)
{
	return uzio_attr_value_set(&chan->buffer.flush, 1);
}


/**
 * Flush all buffers form a given channel-set
 * @param[in] cset channel-set to flush
 */
int uzio_buffer_flush_cset(struct uzio_cset *cset)
{
	int i, err, last_errno = 0;

	for (i = 0; i < cset->n_chan; ++i) {
		err = uzio_buffer_flush(&cset->chan[i]);
		if (err)
			last_errno = errno;
	}

	if (last_errno) {
		errno = last_errno;
		return -1;
	}
	return 0;
}
