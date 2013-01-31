/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_TRIGGER_H__
#define __ZIO_TRIGGER_H__

#include <linux/delay.h>
#include <linux/zio.h>
#include <linux/zio-buffer.h>

#define ZIO_DEFAULT_TRIGGER "user"

struct zio_trigger_type {
	struct zio_obj_head	head;
	struct module		*owner;
	struct list_head	list; /* instances, and list lock */
	spinlock_t		lock;
	unsigned long		flags; /* to be defined */

	const struct zio_sysfs_operations	*s_op;
	const struct zio_trigger_operations	*t_op;

	/* default attributes for instance */
	struct zio_attribute_set		zattr_set;
};
#define to_zio_trig(ptr) container_of(ptr, struct zio_trigger_type, head.dev)

int __must_check zio_register_trig(struct zio_trigger_type *ztrig,
				   const char *name);
void zio_unregister_trig(struct zio_trigger_type *trig);

struct zio_ti {
	struct zio_obj_head	head;
	struct list_head	list;		/* instance list */
	struct zio_cset		*cset;

	unsigned long		flags;		/* input or output, etc */
	int			nsamples;
	spinlock_t		lock;
	/* This is for software stamping */
	struct timespec		tstamp;
	uint64_t tstamp_extra;

	/* Standard and extended attributes for this object */
	struct zio_attribute_set		zattr_set;

	const struct zio_trigger_operations	*t_op;
	uint32_t		pipestamp_arm;
	uint32_t		pipestamp_done;
};

/* first 4bit are reserved for zio object universal flags */
enum zio_ti_flag_mask {
	ZIO_TI_ARMED = 0x10,		/* trigger is armed, device rules */
};

#define to_zio_ti(obj) container_of(obj, struct zio_ti, head.dev)
void zio_arm_trigger(struct zio_ti *ti);

/*
 * When a buffer has a complete block of data, it can send it to the trigger
 * using push_block. The trigger can either accept it (returns 0) or not
 * (returns -EBUSY). This because an output  trigger has only one pending
 * data transfer. When the block is consumed, the trigger may bi->retr_block
 * to get the next one. Buffering is in the buffer, not in the trigger.
 *
 * For input channels, a buffer may call pull_block. The trigger may thus
 * fire input directly and later have a block. In the normal case, the trigger
 * runs by itself and it will call bi->store_block when a new block
 * happens to be ready. In this case the pull_block method here may be null.
 *
 * Input and output in the device is almost always asynchronous, so when
 * the data has been transferred for the cset, the device calls back the
 * trigger. For output, data_done frees the blocks and prepares new
 * blocks if possible; for input, data_done pushes material to the buffers.
 * If the transfer must be interrupted before the invocation of data_done,
 * the abort function must be called. The abort function has two choice: it
 * can invoke data_done and return partial blocks, or free all the active
 * blocks.
 *
 * Then, a trigger instance is configured either by sysfs (and this means
 * the conf_set callback runs and the instance is notified) or by writing
 * a whole control to the control device. In this case the config method
 * is called by the write method.
 *
 * A trigger can be enabled or disabled; each time the trigger status changes,
 * ZIO invokes change_status. The trigger driver must use change_status to
 * start and stop the trigger depending on status value.
 *
 */
struct zio_trigger_operations {
	int			(*push_block)(struct zio_ti *ti,
					      struct zio_channel *chan,
					      struct zio_block *block);
	void			(*pull_block)(struct zio_ti *ti,
					      struct zio_channel *chan);

	int			(*config)(struct zio_ti *ti,
					  struct zio_control *ctrl);

	struct zio_ti *		(*create)(struct zio_trigger_type *trig,
					  struct zio_cset *cset,
					  struct zio_control *ctrl,
					  fmode_t flags);
	void			(*destroy)(struct zio_ti *ti);
	void			(*change_status)(struct zio_ti *ti,
						 unsigned int status);

	/*
	 * Only ZIO core can use the following functions. The user must use
	 * the helper
	 */
	void			(*abort)(struct zio_ti *ti);
	int			(*arm)(struct zio_ti *ti);
	int			(*data_done)(struct zio_cset *cset);
};

int zio_trigger_data_done(struct zio_cset *cset);

/* This, in helpers.c, uses the cset spinlock and may return -EAGAIN */
int __zio_trigger_abort_disable(struct zio_cset *cset, int disable);

/* This can only be called in non-atomic context */
static inline int zio_trigger_abort_disable(struct zio_cset *cset, int disable)
{
	int ret;
	do {
		ret = __zio_trigger_abort_disable(cset, disable);
		if (ret == -EAGAIN)
			msleep(1);
	} while (ret == -EAGAIN);
	return ret;
}

/*
 * This generic_data_done can be used by triggers, as part of their own.
 * If no trigger-specific function is specified, the core calls this one.
 * This can also be called by cset->stop_io to return partial blocks.
 * The function is called while holding the cset spin lock.
 *
 * The function returns 1 if the trigger must be rearmed automatically,
 * otherwise, it returns 0.
 */
static inline int zio_generic_data_done(struct zio_cset *cset)
{
	int self_timed = cset->flags & ZIO_CSET_SELF_TIMED;
	struct zio_buffer_type *zbuf;
	struct zio_channel *chan;
	struct zio_block *block;
	struct zio_control *ctrl;
	struct zio_ti *ti;
	struct zio_bi *bi;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = cset->ti;
	zbuf = cset->zbuf;

	/* Input and output are very similar by now */
	chan_for_each(chan, cset) {
		bi = chan->bi;
		block = chan->active_block;
		ctrl = chan->current_ctrl;

		/* Update the current control: sequence and timestamp */
		ctrl->seq_num++;
		ctrl->tstamp.secs = ti->tstamp.tv_sec;
		ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
		ctrl->tstamp.bins = ti->tstamp_extra;

		/* Copy pipestamp as well (which is nop if unconfigured) */
		__zio_copy_pipestamp(&ctrl->attr_trigger
				     .std_val[ZIO_ATTR_TRIG_ARM_TIME],
				     &ti->pipestamp_arm);
		__zio_copy_pipestamp(&ctrl->attr_trigger
				     .std_val[ZIO_ATTR_TRIG_DONE_TIME],
				     &ti->pipestamp_done);

		if (!block) {
			ctrl->zio_alarms |= ZIO_ALARM_LOST_BLOCK;
			continue;
		}

		if (unlikely((ti->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)) {
				zbuf->b_op->free_block(chan->bi, block);
		} else { /* DIR_INPUT */
			memcpy(zio_get_ctrl(block), ctrl,
			       zio_control_size(chan));
			zio_buffer_store_block(bi, block);
		}
		chan->active_block = NULL;
	}
	if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		return (self_timed ? 1 : 0);

	/* Only for output: prepare the next event if any is ready */
	chan_for_each(chan, cset)
		chan->active_block = zio_buffer_retr_block(chan->bi);

	return (self_timed ? 1 : 0);
}

/**
 * This helper try to push a block to the trigger
 */
static inline int zio_trigger_try_push(struct zio_bi *bi,
				       struct zio_channel *chan,
				       struct zio_block *block)
{
	struct zio_ti *ti = chan->cset->ti;
	int pushed;

	/* chek if trigger is disabled */
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED))
		return 0;

	/* Keep the lock, but mark we are pushing so trigger-user won't retr */
	bi->flags |= ZIO_BI_PUSHING;
	pushed = (ti->t_op->push_block(ti, chan, block) == 0);
	bi->flags &=  ~ZIO_BI_PUSHING;
	return pushed;
}

#endif /* __ZIO_TRIGGER_H__ */
