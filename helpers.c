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
	unsigned long flags;
	int ret;

	/*
	 * If the trigger is running (ZIO_TI_ARMED), then abort it.
	 * Since the whole data_done procedure happens in locked context,
	 * there is no concurrency with an already-completing trigger event.
	 */
	spin_lock_irqsave(&cset->lock, flags);
	if (ti->flags & ZIO_TI_ARMED) {
		if (ti->t_op->abort)
			ti->t_op->abort(ti);
		else if (ti->cset->stop_io)
			ti->cset->stop_io(ti->cset);
		else
			__zio_internal_abort_free(cset);
		ti->flags &= (~ZIO_TI_ARMED);
	}
	ret = ti->flags &= ZIO_STATUS;
	if (disable)
		ti->flags |= ZIO_DISABLED;
	spin_unlock_irqrestore(&cset->lock, flags);
	return ret;
}
EXPORT_SYMBOL(zio_trigger_abort_disable);

static int __zio_arm_input_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int i, datalen;

	cset = ti->cset;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* Allocate the buffer for the incoming sample, in active channels */
	chan_for_each(chan, cset) {
		ctrl = chan->current_ctrl;
		ctrl->nsamples = ti->nsamples;
		datalen = ctrl->ssize * ti->nsamples;
		block = zio_buffer_alloc_block(chan->bi, datalen, GFP_ATOMIC);
		/* If alloc error, it is reported at data_done time */
		chan->active_block = block;
	}
	i = cset->raw_io(cset);
	if (!i) {
		zio_trigger_data_done(cset); /* Succeeded */
	} else if (i != -EAGAIN) {
		/* Error: Free blocks */
		dev_err(&ti->head.dev,
			"raw_io failed (%i), cannot arm trigger\n", i);
		chan_for_each(chan, cset) {
			zio_buffer_free_block(chan->bi, chan->active_block);
		}
	}

	return i;
}

static int __zio_arm_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* We are expected to already have a block in active channels */
	i = cset->raw_io(cset);
	if (!i)
		zio_trigger_data_done(cset); /* Succeeded */
	return i;
}

/*
 * When a software trigger fires, it should call this function. It
 * used to be called zio_fire_trigger, but actually it only arms the trigger.
 * When hardware is self-timed, the actual trigger fires later.
 */
void zio_arm_trigger(struct zio_ti *ti)
{
	unsigned long flags;
	int ret;

	pr_debug("%s:%d\n", __func__, __LINE__);

	__zio_pipestamp(&ti->pipestamp_arm);

	/* check if trigger is disabled or previous instance is pending */
	spin_lock_irqsave(&ti->cset->lock, flags);
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED ||
		     (ti->flags & ZIO_TI_ARMED))) {
		spin_unlock_irqrestore(&ti->cset->lock, flags);
		return;
	}
	ti->flags |= ZIO_TI_ARMED;
	getnstimeofday(&ti->tstamp);
	spin_unlock_irqrestore(&ti->cset->lock, flags);

	if (ti->t_op->arm)
		ret = ti->t_op->arm(ti);
	else if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		ret = __zio_arm_input_trigger(ti);
	else
		ret = __zio_arm_output_trigger(ti);

	/* already succeeded or accepted for later */
	if (!ret || ret == -EAGAIN)
		return;
	/* real error: un-arm */
	spin_lock_irqsave(&ti->cset->lock, flags);
	ti->flags &= ~ZIO_TI_ARMED;
	spin_unlock_irqrestore(&ti->cset->lock, flags);
}
EXPORT_SYMBOL(zio_arm_trigger);

/*
 * zio_trigger_data_done
 * This is a ZIO helper to invoke the data_done trigger operation when a data
 * transfer is over and we need to complete the operation. The trigger
 * is in "ARMED" state when this is called, and is not any more when
 * the function returns. Please note that  we keep the cset lock
 * for the duration of the whole function, which must be atomic.
 *
 * The data_done trigger operation returns an integer [0, 1] which mean if the
 * trigger must be re-armed or not. The rearm value it is returned to the caller
 * to notify if the trigger was rearmed or not.
 */
int zio_trigger_data_done(struct zio_cset *cset)
{
	unsigned long flags;
	int rearm;

	spin_lock_irqsave(&cset->lock, flags);

	__zio_pipestamp(&cset->ti->pipestamp_done);
	if (cset->ti->t_op->data_done)
		rearm = cset->ti->t_op->data_done(cset);
	else
		rearm = zio_generic_data_done(cset);

	cset->ti->flags &= ~ZIO_TI_ARMED;
	spin_unlock_irqrestore(&cset->lock, flags);

	/*
	 * zio_arm_trigger() needs to lock, so it's correct we
	 * released the lock above. No race is expected, because
	 * self-timed devices need to run the transparent trigger. But
	 * if the cset is misconfigured and somebody arm the trigger
	 * in this small window, no harm is done anyways.
	 */
	if (rearm == 1)
		zio_arm_trigger(cset->ti);

	return rearm;
}
EXPORT_SYMBOL(zio_trigger_data_done);
