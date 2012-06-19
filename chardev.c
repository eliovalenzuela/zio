/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

static DEFINE_MUTEX(zmutex);
static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

static int zio_dev_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	unsigned long *flags ;

	flags = dev_get_drvdata(dev);
	add_uevent_var(env, "DEVMODE=%#o", (*flags & ZIO_DIR ? 0220 : 0440));

	return 0;
}


/*
 * zio_cdev_class: don't use class_create to create class, as it doesn't permit
 * to insert a set of class attributes. This structure is the exact
 * reproduction of what class_create does but with some additional settings.
 */
static struct class zio_cdev_class = {
	.name		= "zio-cdev",
	.owner		= THIS_MODULE,
	.dev_uevent	= zio_dev_uevent,
};

/* Retrieve a channel from one of its minors */
static struct zio_channel *zio_minor_to_chan(int minor)
{
	struct zio_cset *zcset;
	int chindex, found = 0;

	list_for_each_entry(zcset, &zstat->list_cset, list_cset) {
		if (minor >= zcset->minor && minor <= zcset->maxminor ) {
			found = 1;
			break;
		}
	}
	if (!found)
		return NULL;
	chindex = (minor - zcset->minor) / 2;
	return zcset->chan + chindex;
}

static inline int zio_channel_get(struct zio_channel *chan)
{
	return try_module_get(chan->cset->zdev->owner);
	/* bi use count is done in the caller */
}
static inline void zio_channel_put(struct zio_channel *chan)
{
	atomic_dec(&chan->bi->use_count);
	module_put(chan->cset->zdev->owner);
}

static int zio_f_open(struct inode *ino, struct file *f)
{
	struct zio_f_priv *priv = NULL;
	struct zio_channel *chan;
	struct zio_buffer_type *zbuf;
	const struct file_operations *old_fops, *new_fops;
	unsigned long flags;
	int err, minor;

	pr_debug("%s:%i\n", __func__, __LINE__);

	minor = iminor(ino);
	chan = zio_minor_to_chan(minor);

	if (!chan || !chan->bi || !zio_channel_get(chan)) {
		pr_err("%s: no channel or no buffer for minor %i\n",
			__func__, minor);
		return -ENODEV;
	}

	/* Take the cset lock to protect against a cset-wide buffer change */
	spin_lock_irqsave(&chan->cset->lock, flags);
	atomic_inc(&chan->bi->use_count);
	err = (chan->bi->flags & ZIO_STATUS) == ZIO_DISABLED ? -EAGAIN : 0;
	spin_unlock_irqrestore(&chan->cset->lock, flags);
	if (err)
		goto out;

	priv = kzalloc(sizeof(struct zio_f_priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto out;
	}
	priv->chan = chan;

	/* even number is control, odd number is data */
	if (minor & 0x1)
		priv->type = ZIO_CDEV_DATA;
	else
		priv->type = ZIO_CDEV_CTRL;

	/* Change the file operations, locking the status structure */
	mutex_lock(&zmutex);
	old_fops = f->f_op;
	zbuf = chan->cset->zbuf;
	new_fops = fops_get(zbuf->f_op);
	err = 0;
	if (new_fops->open)
		err = new_fops->open(ino, f);
	if (err) {
		fops_put(zbuf->f_op);
		mutex_unlock(&zmutex);
		goto out;
	}
	fops_put(old_fops);
	f->f_op = new_fops;
	mutex_unlock(&zmutex);

	f->private_data = priv;
	return 0;

out:
	kfree(priv);
	zio_channel_put(chan);
	return err;
}

static const struct file_operations zfops = {
	.owner = THIS_MODULE,
	.open = zio_f_open,
};

/* set the base minor for a cset*/
int zio_minorbase_get(struct zio_cset *zcset)
{
	unsigned long i;
	int nminors = zcset->n_chan * 2;

	zio_ffa_reset(zstat->minors); /* always start from zero */
	i = zio_ffa_alloc(zstat->minors, nminors, GFP_KERNEL);
	if (i == ZIO_FFA_NOSPACE)
		return -ENOMEM;
	zcset->minor = i;
	zcset->maxminor = i + nminors - 1;
	return 0;
}
void zio_minorbase_put(struct zio_cset *zcset)
{
	zio_ffa_free_s(zstat->minors, zcset->minor, zcset->n_chan * 2);
}

/*
 * create control and data char devices for a channel. The even minor
 * is for control, the odd one for data.
 */
int zio_create_chan_devices(struct zio_channel *chan)
{
	int err;
	dev_t devt_c, devt_d;


	devt_c = zstat->basedev + chan->cset->minor + chan->index * 2;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_c);
	chan->ctrl_dev = device_create(&zio_cdev_class, &chan->head.dev, devt_c,
			&chan->flags, "%s-%i-%i-ctrl",
			dev_name(&chan->cset->zdev->head.dev),
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->ctrl_dev)) {
		err = PTR_ERR(&chan->ctrl_dev);
		goto out;
	}

	devt_d = devt_c + 1;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_d);
	chan->data_dev = device_create(&zio_cdev_class, &chan->head.dev, devt_d,
			&chan->flags, "%s-%i-%i-data",
			dev_name(&chan->cset->zdev->head.dev),
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->data_dev)) {
		err = PTR_ERR(&chan->data_dev);
		goto out_data;
	}

	return 0;

out_data:
	device_destroy(&zio_cdev_class, chan->ctrl_dev->devt);
out:
	return err;
}

void zio_destroy_chan_devices(struct zio_channel *chan)
{
	pr_debug("%s\n", __func__);
	device_destroy(&zio_cdev_class, chan->data_dev->devt);
	device_destroy(&zio_cdev_class, chan->ctrl_dev->devt);
}

int zio_register_cdev()
{
	int err;

	err = class_register(&zio_cdev_class);
	if (err) {
		pr_err("%s: unable to register class\n", __func__);
		goto out;
	}
	/* alloc to zio the maximum number of minors usable in ZIO */
	zstat->minors = zio_ffa_create(0, ZIO_NR_MINORS);
	err = alloc_chrdev_region(&zstat->basedev, 0, ZIO_NR_MINORS, "zio");
	if (err) {
		pr_err("%s: unable to allocate region for %i minors\n",
		       __func__, ZIO_NR_MINORS);
		goto out;
	}

	cdev_init(&zstat->chrdev, &zfops);
	zstat->chrdev.owner = THIS_MODULE;
	err = cdev_add(&zstat->chrdev, zstat->basedev, ZIO_NR_MINORS);
	if (err)
		goto out_cdev;
	INIT_LIST_HEAD(&zstat->list_cset);
	return 0;
out_cdev:
	unregister_chrdev_region(zstat->basedev, ZIO_NR_MINORS);
out:
	class_unregister(&zio_cdev_class);
	zio_ffa_destroy(zstat->minors);

	return err;
}
void zio_unregister_cdev()
{
	cdev_del(&zstat->chrdev);
	unregister_chrdev_region(zstat->basedev, ZIO_NR_MINORS);
	class_unregister(&zio_cdev_class);
	zio_ffa_destroy(zstat->minors);
}


/*
 * Helper functions to check whether read and write would block. The
 * return value is a poll(2) mask, so the poll method just calls them.
 * Both functions return with a user block if read/write can happen.
 */

/* Read is quite straightforward, as blocks reach us already filled */
static int zio_read_mask(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	const int can_read =  POLLIN | POLLRDNORM;

	if (!chan->user_block)
		chan->user_block = bi->b_op->retr_block(bi);
	if (!chan->user_block)
		return 0;

	/* We have a block. But it can be a zero-size block (no data) */
	if (likely(priv->type == ZIO_CDEV_DATA))
		return chan->user_block->datalen ? can_read : 0;

	/* Control: if not yet done, we can read */
	if (!zio_is_cdone(chan->user_block))
		return can_read;

	/* We want to re-read control. Get a new block */
	bi->b_op->free_block(bi, chan->user_block);
	chan->user_block = bi->b_op->retr_block(bi);
	return chan->user_block ? can_read : 0;
}

/* Write is more tricky: this sub-helper asks datasize to the trigger */
static struct zio_block *__zio_write_allocblock(struct zio_bi *bi)
{
	struct zio_cset *cset = bi->chan->cset;
	size_t datalen;

	datalen = cset->ssize * cset->ti->nsamples;
	return bi->b_op->alloc_block(bi, datalen, GFP_KERNEL);
}

static int zio_write_mask(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block = chan->user_block;
	const int can_write = POLLOUT | POLLWRNORM;

	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		/* A control can be replaced before store */
		if (block)
			bi->b_op->free_block(bi, block);
		chan->user_block = __zio_write_allocblock(bi);
		return chan->user_block ? can_write : 0;
	}

	/* We want to write data. If datasize is zero, we can't */
	if (!chan->cset->ssize)
		return 0;

	/* If we have no block, retrieve one */
	if (!block)
		chan->user_block = __zio_write_allocblock(bi);

	/* If there is a previous block, we are sure it is not full */
	return chan->user_block ? can_write : 0;
}

/*
 * The following "generic" read and write (and poll and so on) should
 * work for most buffer types, and are exported for use in their
 * buffer operations.
 */
static ssize_t zio_generic_read(struct file *f, char __user *ubuf,
			 size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;

	pr_debug("%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)
		return -EINVAL;

	if (!zio_read_mask(priv)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(bi->q, zio_read_mask(priv));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	block = chan->user_block;

	/* So, it's readable. Handle control first */
	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		if (count < zio_control_size(chan))
			return -EINVAL;
		count = zio_control_size(chan);
		if (copy_to_user(ubuf, zio_get_ctrl(block), count))
			return -EFAULT;
		zio_set_cdone(block);
		*offp += count;
		return count;
	}

	/* Data file, and some data is there */
	if (count > block->datalen - block->uoff)
		count = block->datalen - block->uoff;
	if (copy_to_user(ubuf, block->data + block->uoff, count))
		return -EFAULT;
	*offp += count;
	block->uoff += count;
	if (block->uoff == block->datalen) {
		chan->user_block = NULL;
		bi->b_op->free_block(bi, block);
	}
	return count;
}

static ssize_t zio_generic_write(struct file *f, const char __user *ubuf,
			  size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;

	pr_debug("%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT)
		return -EINVAL;

	if (!zio_write_mask(priv)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(bi->q, zio_write_mask(priv));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	block = chan->user_block;

	/* File is writeable, handle control first */
	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		if (count < zio_control_size(chan))
			return -EINVAL;
		count = zio_control_size(chan);
		if (copy_from_user(zio_get_ctrl(block), ubuf, count))
			return -EFAULT;
		zio_set_cdone(block);
		*offp += count;

		if (!chan->cset->ssize) { /* data-less: store it now */
			bi->b_op->store_block(bi, block); /* can't fail */
			chan->user_block = NULL;
		}
		return count;
	}

	if (count > block->datalen - block->uoff)
		count =  block->datalen - block->uoff;
	if (copy_from_user(block->data + block->uoff, ubuf, count))
		return -EFAULT;
	*offp += count;
	block->uoff += count;
	if (block->uoff == block->datalen) {
		bi->b_op->store_block(bi, block); /* can't fail */
		chan->user_block = NULL;
	}
	return count;
}

static int zio_generic_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	const struct vm_operations_struct *v_op = bi->v_op;

	if (!v_op)
		return -ENODEV; /* according to man page */
	/* The buffer instance may usecount, so notifiy it and allow
	   it to fail */
	vma->vm_ops = v_op;
	if (v_op->open)
		v_op->open(vma); /* returns void */
	return 0;
}

static unsigned int zio_generic_poll(struct file *f,
				     struct poll_table_struct *w)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;

	poll_wait(f, &bi->q, w);
	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)
		return zio_write_mask(priv);
	else
		return zio_read_mask(priv);
}

static int zio_generic_release(struct inode *inode, struct file *f)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_block *block = chan->user_block;

	if (chan->user_block)
		chan->bi->b_op->free_block(chan->bi, block);
	chan->user_block = NULL;

	zio_channel_put(chan);
	/* priv is allocated by zio_f_open, must be freed */
	kfree(priv);
	return 0;
}

const struct file_operations zio_generic_file_operations = {
	/* no owner: this template is copied over */
	.read =		zio_generic_read,
	.write =	zio_generic_write,
	.poll =		zio_generic_poll,
	.mmap =		zio_generic_mmap,
	.release =	zio_generic_release,
};
/* Export, so buffers can use it or internal function */
EXPORT_SYMBOL(zio_generic_file_operations);

/* Currently, this is a "all or nothing" choice */
int zio_init_buffer_fops(struct zio_buffer_type *zbuf)
{
	struct file_operations *ops;

	/* Current fops may be NULL (buffer for in-kernel data handling */
	if (!zbuf->f_op)
		return 0;
	if (zbuf->f_op != &zio_generic_file_operations)
		return 0;

	/* If it's the generic ones, we must clone to fit the owner field */
	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;
	zbuf->flags |= ZIO_BUF_FLAG_ALLOC_FOPS;
	*ops = zio_generic_file_operations;
	ops->owner = zbuf->owner;
	zbuf->f_op = ops;
	return 0;
}

int zio_fini_buffer_fops(struct zio_buffer_type *zbuf)
{
	if (!(zbuf->flags & ZIO_BUF_FLAG_ALLOC_FOPS))
		return 0;
	zbuf->flags &= ~ZIO_BUF_FLAG_ALLOC_FOPS;
	kfree(zbuf->f_op);
	zbuf->f_op = &zio_generic_file_operations;
	return 0;
}
