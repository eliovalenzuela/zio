/* Alessandro Rubini, Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_BUFFER_H__
#define __ZIO_BUFFER_H__

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/zio.h>
#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linux/zio-user.h>

#define ZIO_DEFAULT_BUFFER "kmalloc" /* For devices with no own buffer type */

/*
 * The following structure defines a buffer type, with methods.
 * An instance is created for each channel using it
 */
struct zio_buffer_operations;
struct zio_buffer_type {
	struct zio_obj_head	head;
	struct module		*owner;
	struct list_head	list; /* instances, and list lock */
	spinlock_t		lock;
	unsigned long		flags;

	const struct zio_sysfs_operations	*s_op;
	const struct zio_buffer_operations	*b_op;
	/*
	 * file operations (read/write etc) are buffer-specific too, but
	 * you are strongly suggested to use zio_generic_file_operations. 
	 * If the field is NULL, you'll get ENODEV when opening the cdev.
	 */
	const struct file_operations		*f_op;
	const struct vm_operations_struct	*v_op;

	/* default attributes for instance */
	struct zio_attribute_set		zattr_set;
};
#define to_zio_buf(ptr) container_of(ptr, struct zio_buffer_type, head.dev)

/* buffer_type->flags */
#define ZIO_BFLAG_ALLOC_FOPS	0x00000001 /* set by zio-core */

extern const struct file_operations zio_generic_file_operations;

int __must_check zio_register_buf(struct zio_buffer_type *zbuf,
				  const char *name);
void zio_unregister_buf(struct zio_buffer_type *zbuf);

/* We have our own kmem_cache (a.k.a. slab) for control structures */
int zio_slab_init(void);
void zio_slab_exit(void);
struct zio_control *zio_alloc_control(gfp_t gfp);
void zio_free_control(struct zio_control *ctrl);


struct zio_bi {
	struct zio_obj_head	head;
	struct list_head	list;		/* instance list */
	struct zio_channel	*chan;
	struct zio_cset		*cset;		/* short for chan->cset */

	/* Those using generic_read need this information */
	unsigned long flags;			/* input or output, etc */
	wait_queue_head_t q;			/* for reading or writing */
	spinlock_t		lock;

	/* Standard and extended attributes for this object */
	struct zio_attribute_set		zattr_set;

	const struct zio_buffer_operations	*b_op;
	const struct file_operations		*f_op;
	const struct vm_operations_struct	*v_op;
};
#define to_zio_bi(obj) container_of(obj, struct zio_bi, head.dev)

/* The block is the basic data item being transferred */
struct zio_block {
	unsigned long		ctrl_flags;
	void			*data;
	size_t			datalen;
	size_t			uoff;
};

/*
 * We must know whether the ctrl block has been filled/read or not: "cdone"
 * No "set_ctrl" or "clr_cdone" are needed, as cdone starts 0 and is only set
 */
#define zio_get_ctrl(block) ((struct zio_control *)((block)->ctrl_flags & ~1))
#define zio_set_ctrl(block, ctrl) ((block)->ctrl_flags = (unsigned long)(ctrl))
#define zio_is_cdone(block)  ((block)->ctrl_flags & 1)
#define zio_set_cdone(block)  ((block)->ctrl_flags |= 1)


/*
 * Each buffer implementation must provide the following methods, because
 * internal management of individual data instances is left to each of them.
 *
 * "store" is for input and "retr" for output (called by low-level driver).
 * After store, the block is ready for user space and freed internally;
 * after retr, it's the low level driver that must cal the free method.
 * The "alloc" method is called on trigger setup (activate), because the
 * data storage must be available when data transfer really happens (thus,
 * a DMA-only device will have its own buffer as the preferred one).
 * The buffer may use its own alloc for blocks created at write(2) time.
 *
 * Note that each buffer type will need more information, so the block
 * is usually inside a custom structure, reached by container_of().
 * Thus, all blocks for a buffer type must be allocated and freed using
 * the methods of that specific buffer type.
 */
struct zio_buffer_operations {
	struct zio_block *	(*alloc_block)(struct zio_bi *bi,
					       struct zio_control *ctrl,
					       size_t datalen, gfp_t gfp);
	void			(*free_block)(struct zio_bi *bi,
					      struct zio_block *block);

	int			(*store_block)(struct zio_bi *bi,
					       struct zio_block *block);
	struct zio_block *	(*retr_block) (struct zio_bi *bi);

	struct zio_bi *		(*create)(struct zio_buffer_type *zbuf,
					  struct zio_channel *chan);
	void			(*destroy)(struct zio_bi *bi);
};

/*
 * This is the structure we place in f->private_data at open time.
 * Note that the buffer_create function is called by zio-core.
 */
enum zio_cdev_type {
	ZIO_CDEV_CTRL,
	ZIO_CDEV_DATA,
};
struct zio_f_priv {
	struct zio_channel *chan; /* where current block and buffer live */
	enum zio_cdev_type type;
};

#endif /* __ZIO_BUFFER_H__ */
