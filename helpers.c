/*
 * Copyright 2011 CERN
 * Author: Alessandro Rubini <rubini@gnudd.com>
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

/* The trigger is armed and the cset spin lock is taken */
static void __zio_internal_data_done(struct zio_cset *cset)
{
	struct zio_buffer_type *zbuf;
	struct zio_device *zdev;
	struct zio_channel *chan;
	struct zio_block *block;
	struct zio_ti *ti;
	struct zio_bi *bi;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = cset->ti;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	if (unlikely((ti->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)) {
		chan_for_each(chan, cset) {
			bi = chan->bi;
			block = chan->active_block;
			if (block)
				zbuf->b_op->free_block(chan->bi, block);
			/* We may have a new block ready, or not */
			chan->active_block = zbuf->b_op->retr_block(chan->bi);
		}
		return;
	}
	/* DIR_INPUT */
	chan_for_each(chan, cset) {
		bi = chan->bi;
		block = chan->active_block;
		if (!block)
			continue;
		/* Copy the stamp: it is cset-wide so it lives in the trigger */
		chan->current_ctrl->tstamp.secs = ti->tstamp.tv_sec;
		chan->current_ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
		chan->current_ctrl->tstamp.bins = ti->tstamp_extra;
		memcpy(zio_get_ctrl(block), chan->current_ctrl,
		       ZIO_CONTROL_SIZE);

		if (zbuf->b_op->store_block(bi, block)) /* may fail, no prob */
			zbuf->b_op->free_block(bi, block);
	}
}

/*
 * zio_trigger_data_done
 * This is a ZIO helper to invoke the data_done trigger operation when a data
 * transfer is over and we need to complete the operation. The trigger
 * is in "ARMED" state when this is called, and is not any more when
 * the function returns. Please note that  we keep the cset lock
 * for the duration of the whole function, which must be atomic
 */
void zio_trigger_data_done(struct zio_cset *cset)
{
	spin_lock(&cset->lock);

	if (cset->ti->t_op->data_done)
		cset->ti->t_op->data_done(cset);
	else
		__zio_internal_data_done(cset);

	cset->ti->flags &= ~ZIO_TI_ARMED;
	spin_unlock(&cset->lock);
}
EXPORT_SYMBOL(zio_trigger_data_done);

static void __zio_internal_abort_free(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;

	chan_for_each(chan, cset) {
		block = chan->active_block;
		if (block)
			cset->zbuf->b_op->free_block(chan->bi, block);
		chan->active_block = NULL;
	}
}

/*
 * zio_trigger_abort
 * This is a ZIO helper to invoke the abort function. This must be used when
 * something is going wrong during the acquisition or an armed trigger
 * must be modified. If so requested, the trigger is disabled too.
 * The function returns the previous value of the disabled flags.
 */
int zio_trigger_abort_disable(struct zio_cset *cset, int disable)
{
	struct zio_ti *ti = cset->ti;
	int ret;

	/*
	 * If the trigger is running (ZIO_TI_ARMED), then abort it.
	 * Since the whole data_done procedure happens in locked context,
	 * there is no concurrency with an already-completing trigger event.
	 */
	spin_lock(&cset->lock);
	if (ti->flags & ZIO_TI_ARMED) {
		if (ti->t_op->abort)
			ti->t_op->abort(ti);
		else
			__zio_internal_abort_free(cset);
		ti->flags &= (~ZIO_TI_ARMED);
	}
	ret = ti->flags &= ZIO_STATUS;
	if (disable)
		ti->flags |= ZIO_DISABLED;
	spin_unlock(&cset->lock);
	return ret;
}
EXPORT_SYMBOL(zio_trigger_abort_disable);

static void __zio_arm_input_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int datalen;

	cset = ti->cset;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* Allocate the buffer for the incoming sample, in active channels */
	chan_for_each(chan, cset) {
		ctrl = chan->current_ctrl;
		ctrl->seq_num++;

		ctrl->nsamples = ti->nsamples;
		datalen = ctrl->ssize * ti->nsamples;
		block = zbuf->b_op->alloc_block(chan->bi, datalen,
						GFP_ATOMIC);
		/* on error it returns NULL so we are all happy */
		chan->active_block = block;
	}
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		zio_trigger_data_done(cset);
	}
}

static void __zio_arm_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* We are expected to already have a block in active channels */
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		zio_trigger_data_done(cset);
	}
}

/*
 * When a software trigger fires, it should call this function. It
 * used to be called zio_fire_trigger, but actually it only arms the trigger.
 * When hardware is self-timed, the actual trigger fires later.
 */
void zio_arm_trigger(struct zio_ti *ti)
{
	/* If the trigger runs too early, ti->cset is still NULL */
	if (!ti->cset)
		return;

	/* check if trigger is disabled or previous instance is pending */
	spin_lock(&ti->cset->lock);
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED ||
		     (ti->flags & ZIO_TI_ARMED))) {
		spin_unlock(&ti->cset->lock);
		return;
	}
	ti->flags |= ZIO_TI_ARMED;
	spin_unlock(&ti->cset->lock);

	if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		__zio_arm_input_trigger(ti);
	else
		__zio_arm_output_trigger(ti);
}
EXPORT_SYMBOL(zio_arm_trigger);
