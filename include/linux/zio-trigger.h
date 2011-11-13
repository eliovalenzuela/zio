/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_TRIGGER_H__
#define __ZIO_TRIGGER_H__

#include <linux/zio.h>
#include <linux/zio-buffer.h>

#define ZIO_DEFAULT_TRIGGER "timer" /* FIXME: app-request */

struct zio_trigger_type {
	struct zio_obj_head	head;
	struct module		*owner;
	struct list_head	list; /* instances, and list lock */
	struct spinlock		lock;
	unsigned long		flags; /* to be defined */

	/* file_operations because the trigger may override the buffer */
	const struct zio_sys_operations *s_op;
	const struct zio_trigger_operations	*t_op;
	const struct file_operations		*f_op;

	/* FIXME: how "own" devices are listed (here or elsewhere?) */
	struct zio_device	*zdev_owner;
	unsigned int		n_zdev_owner;
};
#define to_zio_trig(_kobj) container_of(_kobj, struct zio_trigger, head.kobj)

int __must_check zio_register_trig(struct zio_trigger_type *ztrig,
				   const char *name);
void zio_unregister_trig(struct zio_trigger_type *trig);

struct zio_ti {
	struct zio_obj_head	head;
	struct list_head	list;		/* instance list */
	struct zio_cset		*cset;

	struct zio_control	*current_ctrl;	/* the active one */
	struct timespec tstamp;
	uint64_t tstamp_extra;

	/* Standard and extended attributes for this object */
	struct zio_attribute_set	zattr_set;

	const struct zio_trigger_operations	*t_op;
	const struct file_operations		*f_op;

};
#define to_zio_ti(_kobj) container_of(_kobj, struct zio_ti, head.kobj)
int zio_fire_trigger(struct zio_ti *ti);

/*
 * When a buffer has a complete block of data, it can send it to the trigger
 * using push_block. The trigger can either accept it (returns 0) or not
 * (returns -EBUSY). This because an output  trigger has only one pending
 * data transfer. When the block is consumed, the trigger may bi->retr_block
 * to get the next one. Buffering is in the buffer, not in the trigger.
 *
 * For input channels, a buffer may call pull_block. The trigger may thus
 * fire input directly and return a block. In the normal case, the trigger
 * runs by itself and it will call bi->store_block when a new block
 * happens to be ready. In this case the pull_block method here may be null.
 *
 * Then, a trigger instance is configured either by sysfs (and this means
 * the conf_set callback runs and the instance is notified) or by writing
 * a whole control to the control device. In this case the config method
 * is called by the write method.
 */
struct zio_trigger_operations {
	int			(*push_block)(struct zio_ti *ti,
					      struct zio_channel *chan,
					      struct zio_block *block);
	struct zio_block *	(*pull_block)(struct zio_ti *ti,
					      struct zio_channel *chan);

	int			(*config)(struct zio_ti *ti,
					  struct zio_control *ctrl);

	struct zio_ti *		(*create)(struct zio_trigger_type *trig,
					  struct zio_cset *cset,
					  struct zio_control *ctrl,
					  fmode_t flags);
	void			(*destroy)(struct zio_ti *ti);
};

#endif /* __ZIO_TRIGGER_H__ */