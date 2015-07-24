/*
 * Copyright 2015 CERN
 * Author: Federico Vaga <federico.vaga@cern.ch>
 *
 * GNU GPLv3 or later
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <libzio.h>

/**
 * It prints on a file stream the control
 * @param[in] stream file where print the control
 * @param[in] type type of attribute to print
 * @param[in] ctrl the control to be printed
 */
void zio_control_print_to_file_attr(FILE *stream,
				    enum zio_control_attr_type type,
				    struct zio_control *ctrl)
{
	uint32_t *val, mask = 0;
	char *name[] = {"device-std", "device-ext",
			"trigger-std", "trigger-ext"};
	int nattr, i;

	switch (type) {
	case ZIO_CTRL_ATTR_DEV_STD:
		mask = ctrl->attr_channel.std_mask;
		val = ctrl->attr_channel.std_val;
		nattr = ZIO_MAX_STD_ATTR;
		break;
	case ZIO_CTRL_ATTR_DEV_EXT:
		mask = ctrl->attr_channel.ext_mask;
		val = ctrl->attr_channel.ext_val;
		nattr = ZIO_MAX_EXT_ATTR;
		break;
	case ZIO_CTRL_ATTR_TRG_STD:
		mask = ctrl->attr_trigger.std_mask;
		val = ctrl->attr_trigger.std_val;
		nattr = ZIO_MAX_STD_ATTR;
		break;
	case ZIO_CTRL_ATTR_TRG_EXT:
		mask = ctrl->attr_trigger.ext_mask;
		val = ctrl->attr_trigger.ext_val;
		nattr = ZIO_MAX_EXT_ATTR;
		break;
	default:
		return;
	}

	printf("Ctrl: %s-mask: 0x%04x\n", name[type], mask);
	for (i = 0; i < nattr; ++i) {
		if (!(mask & (1 << i)))
			continue;
		printf ("Ctrl: %s-%-2i  0x%08x %9i\n",
			name[type], i, val[i], val[i]);
	}
}


/**
 * It prints on a file stream the control in its standard ASCII format which
 * is easly parsable
 * @param[in] stream file where print the control
 * @param[in] ctrl the control to be printed
 */
void zio_control_print_to_file_basic(FILE *stream, struct zio_control *ctrl)
{
	fprintf(stream, "Ctrl: version %i.%i, trigger %.16s, dev %.16s-%04x, "
	       "cset %i, chan %i\n",
	       ctrl->major_version, ctrl->minor_version,
	       ctrl->triggername, ctrl->addr.devname, ctrl->addr.dev_id,
	       ctrl->addr.cset, ctrl->addr.chan);
	fprintf(stream, "Ctrl: alarms 0x%02x 0x%02x\n",
	       ctrl->zio_alarms, ctrl->drv_alarms);
	fprintf(stream, "Ctrl: seq %i, n %i, size %i, bits %i, flags %08x (%s)\n",
	       ctrl->seq_num,
	       ctrl->nsamples,
	       ctrl->ssize,
	       ctrl->nbits,
	       ctrl->flags,
	       ctrl->flags & ZIO_CONTROL_LITTLE_ENDIAN
	       ? "little-endian" :
	       ctrl->flags & ZIO_CONTROL_BIG_ENDIAN
	       ? "big-endian" : "unknown-endian");
	fprintf(stream, "Ctrl: stamp %lli.%09lli (%lli)\n",
	       (long long)ctrl->tstamp.secs,
	       (long long)ctrl->tstamp.ticks,
	       (long long)ctrl->tstamp.bins);
	fprintf(stream, "Ctrl: mem_offset %08x\n", ctrl->mem_offset);
}
