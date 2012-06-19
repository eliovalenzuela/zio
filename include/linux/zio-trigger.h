/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_TRIGGER_H__
#define __ZIO_TRIGGER_H__

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-compat-24.h>

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
	spinlock_t		lock;
	/* This is for software stamping */
	struct timespec		tstamp;
	uint64_t tstamp_extra;

	/* Standard and extended attributes for this object */
	struct zio_attribute_set		zattr_set;

	const struct zio_trigger_operations	*t_op;
};

/* first 4bit are reserved for zio object universal flags */
enum zti_flag_mask {
	ZTI_BUSY = 0x10,	/* trigger fire and transfer occurs */
	ZTI_COMPLETING = 0x20	/* trigger is clompleting transfert */
};

#define to_zio_ti(obj) container_of(obj, struct zio_ti, head.dev)
void zio_fire_trigger(struct zio_ti *ti);

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

	void			(*data_done)(struct zio_cset *cset);

	int			(*config)(struct zio_ti *ti,
					  struct zio_control *ctrl);

	struct zio_ti *		(*create)(struct zio_trigger_type *trig,
					  struct zio_cset *cset,
					  struct zio_control *ctrl,
					  fmode_t flags);
	void			(*destroy)(struct zio_ti *ti);
	void			(*change_status)(struct zio_ti *ti,
						 unsigned int status);
	void			(*abort)(struct zio_cset *cset);
};

void zio_trigger_abort(struct zio_cset *cset);

#endif /* __ZIO_TRIGGER_H__ */
