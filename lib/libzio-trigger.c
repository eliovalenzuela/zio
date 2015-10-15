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

const struct uzio_attribute available_triggers = {
	.parent = NULL,
	.path = UZIO_SYS_DIR"/available_triggers",
};


/**
 * It returns the list of available buffers.
 * @return the list of trigger modules. The structure must be free using
 *         the function uzio_module_list_free()
 */
struct uzio_module_list *uzio_trigger_list(void)
{
	return uzio_module_list(&available_triggers);
}


/**
 * It change trigger type for a given channel set
 * @param[in] cset channel set where act
 * @param[in] name trigger name to use
 * @param[in] n trigger name string lenght
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_trigger_change(struct uzio_cset *cset, char *name, unsigned int n)
{
	int ret;

	ret = uzio_attr_string_set(&cset->current_trigger, name, n);
	return ret < 0 ? -1 : 0;
}


/**
 * Enable or disable a given trigger instance
 * @param[in] trig trigger to enable/disable
 * @param[in] enable enable status (0 means disable, other values mean enable)
 * @return 0 on success, -1 on error and errno is appropriately set
 */
int uzio_trigger_enable(struct uzio_trigger *trig, unsigned int enable)
{
	return uzio_object_enable(&trig->head, enable);
}
