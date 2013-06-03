/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"


#define ZOBJ_SYSFS_NAME		"name"
#define ZOBJ_SYSFS_ENABLE	"enable"
#define CSET_SYSFS_BUFFER	"current_buffer"
#define CSET_SYSFS_TRIGGER	"current_trigger"

const char zio_zdev_attr_names[_ZIO_DEV_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_GAIN]			= "gain_factor",
	[ZIO_ATTR_OFFSET]		= "offset",
	[ZIO_ATTR_NBITS]		= "resolution-bits",
	[ZIO_ATTR_MAXRATE]		= "max-sample-rate",
	[ZIO_ATTR_VREFTYPE]		= "vref-src",
};
EXPORT_SYMBOL(zio_zdev_attr_names);

const char zio_trig_attr_names[_ZIO_TRG_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_TRIG_REENABLE]	= "re-enable",
	[ZIO_ATTR_TRIG_PRE_SAMP]	= "pre-samples",
	[ZIO_ATTR_TRIG_POST_SAMP]	= "post-samples",
};
EXPORT_SYMBOL(zio_trig_attr_names);

const char zio_zbuf_attr_names[_ZIO_BUF_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_ZBUF_MAXLEN]	= "max-buffer-len",
	[ZIO_ATTR_ZBUF_MAXKB]	= "max-buffer-kb",
};
EXPORT_SYMBOL(zio_zbuf_attr_names);

static struct zio_attribute *__zattr_clone(const struct zio_attribute *src,
		unsigned int n)
{
	struct zio_attribute *dest = NULL;
	unsigned int size;

	if (!src || !n)
		return NULL;
	size = n * sizeof(struct zio_attribute);
	dest = kmalloc(size, GFP_KERNEL);
	if (!dest)
		return NULL;

	dest = memcpy(dest, src, size);

	return dest;
}

static void __zattr_unclone(struct zio_attribute *zattr)
{
	kfree(zattr);
}

int __zattr_set_copy(struct zio_attribute_set *dest,
		     struct zio_attribute_set *src)
{
	if (!dest || !src)
		return -EINVAL;
	dest->n_std_attr = src->n_std_attr;
	dest->n_ext_attr = src->n_ext_attr;
	dest->std_zattr = __zattr_clone(src->std_zattr, dest->n_std_attr);
	dest->ext_zattr = __zattr_clone(src->ext_zattr, dest->n_ext_attr);

	return 0;
}
void __zattr_set_free(struct zio_attribute_set *zattr_set)
{
	if (!zattr_set)
		return;
	__zattr_unclone(zattr_set->ext_zattr);
	__zattr_unclone(zattr_set->std_zattr);
}

/* When touching attributes, we always use the spinlock for the hosting dev */
static spinlock_t *__get_spinlock(struct zio_obj_head *head)
{
	spinlock_t *lock;

	switch (head->zobj_type) {
	case ZIO_DEV:
		lock = &to_zio_dev(&head->dev)->lock;
		break;
	case ZIO_CSET:
		lock = &to_zio_cset(&head->dev)->zdev->lock;
		break;
	case ZIO_CHAN:
		lock = &to_zio_chan(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_TI: /* we might not want to take a lock but... */
		lock = &to_zio_ti(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_BI:
		lock = &to_zio_bi(&head->dev)->cset->zdev->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}


/*
 * used to init and update sysfs attribute value into a control.
 * The bit mask is set also during update to make the code simple, but
 * this does not decrease performance
 */
static inline void __zattr_valcpy(struct zio_ctrl_attr *ctrl,
				  struct zio_attribute *zattr)
{
	if ((zattr->flags & ZIO_ATTR_TYPE) == ZIO_ATTR_TYPE_EXT) {
		ctrl->ext_mask |= (1 << zattr->index);
		ctrl->ext_val[zattr->index] = zattr->value;
	} else {
		if (zattr->index == ZIO_ATTR_INDEX_NONE)
			return;
		ctrl->std_mask |= (1 << zattr->index);
		ctrl->std_val[zattr->index] = zattr->value;
	}
}

void __ctrl_update_nsamples(struct zio_ti *ti)
{
	ti->nsamples = ti->zattr_set.std_zattr[ZIO_ATTR_TRIG_PRE_SAMP].value +
		       ti->zattr_set.std_zattr[ZIO_ATTR_TRIG_POST_SAMP].value;
}
static void __zattr_propagate_value(struct zio_obj_head *head,
			       struct zio_attribute *zattr)
{
	int i, j;
	unsigned long flags, tflags;
	struct zio_ti *ti;
	struct zio_device *zdev;
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_control *ctrl;

	if (!(zattr->flags & ZIO_ATTR_CONTROL))
		return; /* the attribute is not in the control */

	pr_debug("%s\n", __func__);
	switch (head->zobj_type) {
	case ZIO_DEV:
		zdev = to_zio_dev(&head->dev);
		for (i = 0; i < zdev->n_cset; ++i) {
			cset = &zdev->cset[i];
			for (j = 0; j < cset->n_chan; ++j) {
				ctrl = cset->chan[j].current_ctrl;
				__zattr_valcpy(&ctrl->attr_channel, zattr);
			}
		}
		break;
	case ZIO_CSET:
		cset = to_zio_cset(&head->dev);
		for (i = 0; i < cset->n_chan; ++i) {
			ctrl = cset->chan[i].current_ctrl;
			__zattr_valcpy(&ctrl->attr_channel, zattr);
		}
		break;
	case ZIO_CHAN:
		ctrl = to_zio_chan(&head->dev)->current_ctrl;
		__zattr_valcpy(&ctrl->attr_channel, zattr);
		break;
	case ZIO_TI:
		ti = to_zio_ti(&head->dev);
		/*
		 * If trigger params change, we need to abort ongoing I/O.
		 * Disable, temporarily, while we change configuration.
		 */
		tflags = zio_trigger_abort_disable(ti->cset, 1);
		/*
		 * It is disabled, nobody can enable since that is only
		 * possible through sysfs and we hold the config lock.
		 * So pick the I/O lock to prevent I/O operations and proceed.
		 */
		spin_lock_irqsave(&ti->cset->lock, flags);
		__ctrl_update_nsamples(ti);
		/* Update attributes in all "current_ctrl" struct */
		for (i = 0; i < ti->cset->n_chan; ++i) {
			chan = &ti->cset->chan[i];
			ctrl = chan->current_ctrl;
			__zattr_valcpy(&ctrl->attr_trigger, zattr);
		}
		/* If it was enabled, re-enable it */
		if ((tflags & ZIO_STATUS) == ZIO_ENABLED)
			ti->flags = (ti->flags & ~ZIO_STATUS) | ZIO_ENABLED;
		spin_unlock_irqrestore(&ti->cset->lock, flags);
		break;
	default:
		return;
	}
}

void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl)
{
	int i;
	struct zio_ctrl_attr *ctrl_attr_trig = &ctrl->attr_trigger;

	strncpy(ctrl->triggername, ti->cset->trig->head.name, ZIO_OBJ_NAME_LEN);
	ctrl_attr_trig->std_mask = 0;
	ctrl_attr_trig->ext_mask = 0;
	/* Copy trigger value */
	for (i = 0; i < ti->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_trig, &ti->zattr_set.std_zattr[i]);
	for (i = 0; i < ti->zattr_set.n_ext_attr; ++i)
		if (ti->zattr_set.ext_zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_trig,
				       &ti->zattr_set.ext_zattr[i]);
}
static int __zattr_chan_init_ctrl(struct zio_channel *chan, unsigned int start)
{
	struct zio_ctrl_attr *ctrl_attr_chan;
	struct zio_attribute *zattr;
	struct zio_control *ctrl;
	struct zio_device *zdev;
	struct zio_cset *cset;
	int i;

	cset = chan->cset;
	zdev = cset->zdev;
	ctrl = chan->current_ctrl;
	ctrl->addr.dev_id = chan->cset->zdev->dev_id;
	ctrl_attr_chan = &chan->current_ctrl->attr_channel;
	if (!(start + chan->zattr_set.n_ext_attr < 32)) {
		dev_err(&zdev->head.dev, "too many extended attribute");
		return -EINVAL;
	}

	__zattr_trig_init_ctrl(cset->ti, chan->current_ctrl);
	/* Copy standard attributes into the control */
	for (i = 0; i < chan->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &chan->zattr_set.std_zattr[i]);
	for (i = 0; i < cset->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &cset->zattr_set.std_zattr[i]);
	for (i = 0; i < zdev->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &zdev->zattr_set.std_zattr[i]);

	/* Fix and copy attributes within channel */
	zattr = chan->zattr_set.ext_zattr;
	for (i = 0; i < chan->zattr_set.n_ext_attr; ++i) {
		if (zattr[i].flags & ZIO_ATTR_CONTROL) {
			/* Fix channel extended attribute index */
			zattr[i].index = start + i;
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
		} else {
			zattr[i].index = ZIO_ATTR_INDEX_NONE;
		}
	}
	/* Copying attributes from cset */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	/* Copying attributes from zdev */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	return 0;
}
static int __zattr_cset_init_ctrl(struct zio_cset *cset, unsigned int start)
{
	struct zio_attribute *zattr;
	int i, err, start_c = start;

	/* Fix cset extended attribute index */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start_c++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

	for (i = 0; i < cset->n_chan; ++i) {
		err = __zattr_chan_init_ctrl(&cset->chan[i], start_c);
		if (err)
			return err;
	}
	return 0;
}

/*
 * fix the zio attribute index for the extended attribute within device
 * and set the attribute value into the current control of each channel
 */
int __zattr_dev_init_ctrl(struct zio_device *zdev)
{
	struct zio_attribute *zattr;
	int i, err, start = 0;

	pr_debug("%s\n", __func__);
	/* Device level */
	/* Fix device extended attribute index */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

	for (i = 0; i < zdev->n_cset; ++i) {
		err = __zattr_cset_init_ctrl(&zdev->cset[i], start);
		if (err)
			return err;
	}
	return 0;
}

/*
 * enable/disable a zio object
 * must be called while holding the zio_device spinlock
 */
static void __zobj_enable(struct device *dev, unsigned int enable)
{
	unsigned long *zf, flags;
	int i, status;
	struct zio_obj_head *head;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	head = to_zio_head(dev);

	zf = zio_get_from_obj(to_zio_head(dev), flags);
	status = !((*zf) & ZIO_STATUS);
	/* if the status is not changing */
	if (!(enable ^ status))
		return;
	/* change status */
	*zf = (*zf & (~ZIO_STATUS)) | status;
	switch (head->zobj_type) {
	case ZIO_DEV:
		pr_debug("%s: zdev\n", __func__);

		zdev = to_zio_dev(dev);
		/* enable/disable all cset */
		for (i = 0; i < zdev->n_cset; ++i)
			__zobj_enable(&zdev->cset[i].head.dev, enable);
		/* device callback */
		break;
	case ZIO_CSET:
		pr_debug("%s: zcset\n", __func__);

		cset = to_zio_cset(dev);
		/* enable/disable trigger instance */
		__zobj_enable(&cset->ti->head.dev, enable);
		/* enable/disable all channel*/
		for (i = 0; i < cset->n_chan; ++i)
			__zobj_enable(&cset->chan[i].head.dev, enable);
		/* cset callback */
		break;
	case ZIO_CHAN:
		pr_debug("%s: zchan\n", __func__);
		/* channel callback */
		break;
	case ZIO_TI:
		pr_debug("%s: zti\n", __func__);

		ti = to_zio_ti(dev);
		zio_trigger_abort_disable(ti->cset, 0);
		spin_lock_irqsave(&ti->cset->lock, flags);
		/* trigger instance callback */
		if (ti->t_op->change_status)
			ti->t_op->change_status(ti, status);
		spin_unlock_irqrestore(&ti->cset->lock, flags);
		break;
	/* following objects can't be enabled/disabled */
	case ZIO_BUF:
	case ZIO_TRG:
	case ZIO_BI:
		pr_debug("%s: others\n", __func__);
		/* buffer instance callback */
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
	}
}

/* Print the name of a zio object */
static ssize_t zobj_show_name(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_head(dev)->name);
}
/* Print the name of a zio object */
static ssize_t zobj_show_dev_type(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", dev->type->name);
}
/* Print the current trigger name */
static ssize_t zobj_show_cur_trig(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->trig->head.name);
}
/* Change the current trigger */
static ssize_t zobj_store_cur_trig(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	struct zio_cset *cset;
	int err = 0;

	if (strlen(buf) > ZIO_OBJ_NAME_LEN+1)
		return -EINVAL; /* name too long */
	sscanf(buf, "%s\n", buf_tmp);
	cset = to_zio_cset(dev);
	/* change trigger only if is different then current */
	if (strcmp(buf_tmp, cset->trig->head.name))
		err = zio_change_current_trigger(cset, buf_tmp);
	return err ? err : count;
}
/* Print the current buffer name */
static ssize_t zobj_show_cur_zbuf(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->zbuf->head.name);
}
/* Change the current buffer */
static ssize_t zobj_store_cur_zbuf(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	struct zio_cset *cset;
	int err = 0;

	if (strlen(buf) > ZIO_OBJ_NAME_LEN+1)
		return -EINVAL; /* name too long */
	sscanf(buf, "%s\n", buf_tmp);
	cset = to_zio_cset(dev);
	/* change buffer only if is different then current */
	if (strcmp(buf_tmp, cset->trig->head.name))
		err = zio_change_current_buffer(cset, buf_tmp);
	return err ? err : count;
}
/* Print the current enable status */
static ssize_t zobj_show_enable(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long *flags;
	int status;

	flags = zio_get_from_obj(to_zio_head(dev), flags);
	status = !(*flags & ZIO_DISABLED);
	return sprintf(buf, "%d\n", status);
}
/* Change the current enable status */
static ssize_t zobj_store_enable(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	long val;
	int err;
	spinlock_t *lock;

	err = strict_strtol(buf, 0, &val);
	if (err)
		return -EINVAL;

	lock = __get_spinlock(to_zio_head(dev));
	/* change enable status */
	if (unlikely(strcmp(attr->attr.name, ZOBJ_SYSFS_ENABLE) == 0 &&
	    (val == 0 || val == 1))) {
		spin_lock(lock);
		__zobj_enable(dev, val);
		spin_unlock(lock);
		return count;
	}

	return count;
}
/*
 * Zio objects all handle uint32_t values. So the show and store
 * are centralized here, and each device has its own get_info and set_conf
 * which handle binary 32-bit numbers. Both the function are locked to prevent
 * concurrency issue when editing device register.
 */
static ssize_t zattr_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	ssize_t len = 0;


	pr_debug("%s\n", __func__);
	if (!zattr->s_op)
		goto out;
	if (zattr->s_op->info_get) {
		struct zio_obj_head *head = to_zio_head(dev);
		spinlock_t *lock;
		int err = 0;

		lock = __get_spinlock(head);
		spin_lock(lock);
		err = zattr->s_op->info_get(dev, zattr, &zattr->value);
		spin_unlock(lock);
		if (err)
			return err;
	}
out:
	len = sprintf(buf, "%i\n", zattr->value);
	return len;
}

static ssize_t zattr_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	struct zio_obj_head *head = to_zio_head(dev);
	spinlock_t *lock;
	long val;
	int err;

	pr_debug("%s\n", __func__);

	if (strict_strtol(buf, 0, &val))
		return -EINVAL;

	/* device attributes */
	if (!zattr->s_op->conf_set)
		return -EINVAL;

	lock = __get_spinlock(head);
	spin_lock(lock);
	err = zattr->s_op->conf_set(dev, zattr, (uint32_t)val);
	if (err) {
		spin_unlock(lock);
		return err;
	}
	zattr->value = (uint32_t)val;
	__zattr_propagate_value(head, zattr);
	spin_unlock(lock);

	return count;
}

/*
 * zobj_show_devname
 *
 * The function return the address of the different ZIO object in the device
 * hierarchy.
 *
 * FIXME with interface, this function call an interface operation which returns
 * the address. In this way, pf-zio and cdev can return their own devname
 */
static ssize_t zobj_show_devname(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct zio_obj_head *head = to_zio_head(dev);
	struct zio_channel *chan;
	struct zio_cset *cset;

	switch (head->zobj_type) {
	case ZIO_DEV:
		return sprintf(buf, "%s\n", dev_name(dev));
	case ZIO_CSET:
		cset = to_zio_cset(dev);
		return sprintf(buf, "%s-%i\n",
			       dev_name(&cset->zdev->head.dev), cset->index);
	case ZIO_CHAN:
		chan = to_zio_chan(dev);
		return sprintf(buf, "%s-%i-%i\n",
			       dev_name(&chan->cset->zdev->head.dev),
			       chan->cset->index, chan->index);
	default:
		WARN(1, "ZIO: unknown zio object %i for address\n",
			head->zobj_type);
		return -EINVAL;
	}
	return 0;

}

/*
 * zobj_show_alarm
 * It get alarm status from the current control of the channel and it return
 * the value to sysfs
 */
static ssize_t zio_show_alarm(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct zio_channel *chan = to_zio_chan(dev);
	struct zio_control *ctrl = chan->current_ctrl;

	return sprintf(buf, "%d %d\n", ctrl->zio_alarms, ctrl->drv_alarms);
}

/*
 * zio_store_alarm
 * It get from the user the alarm to clear, then it clears the alarm in the
 * curren control of the channel
 */
static ssize_t zio_store_alarm(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct zio_channel *chan = to_zio_chan(dev);
	struct zio_control *ctrl = chan->current_ctrl;
	unsigned int v1, v2;

	switch(sscanf(buf, "%i %i", &v1, &v2)) {
	case 2:
		ctrl->drv_alarms &= (~v2);
	case 1:
		ctrl->zio_alarms &= (~v1);
		break;
	default:
		return -EINVAL;
	}
	return count;
}

#if ZIO_HAS_BINARY_CONTROL
/*
 * zobj_read_cur_ctrl
 * it returns the current control to userspace through binary sysfs file
 */
ssize_t zobj_read_cur_ctrl(struct file *file,struct kobject *kobj,
			   struct bin_attribute *bin_attr,
			   char *buf, loff_t off, size_t count)
{
	struct zio_channel *chan;

	if (off >= bin_attr->size)
		return 0;

	/* This file must be read entirely */
	if (off != 0)
		return -ESPIPE; /* Illegal seek */
	if (count < bin_attr->size)
		return -EINVAL;
	if (count > bin_attr->size)
		count = bin_attr->size;

	chan = to_zio_chan(container_of(kobj, struct device, kobj));
	memcpy(buf, chan->current_ctrl, count); /* FIXME: lock */

	return count;
}

static ssize_t zobj_show_address(struct file *file,struct kobject *kobj,
				 struct bin_attribute *bin_attr,
				 char *buf, loff_t off, size_t count)
{
	struct zio_channel *chan;

	if (off >= bin_attr->size)
		return 0;

	/* This file must be read entirely */
	if (off != 0)
		return -ESPIPE; /* Illegal seek */
	if (count < bin_attr->size)
		return -EINVAL;
	if (count > bin_attr->size)
		count = bin_attr->size;

	chan = to_zio_chan(container_of(kobj, struct device, kobj));
	memcpy(buf, &chan->current_ctrl->addr, count); /* FIXME: lock */

	return count;

}

struct bin_attribute zio_bin_attr[] = {
	[ZIO_BIN_CTRL] = {
		.attr = { .name = "current-control", .mode = ZIO_RO_PERM, },
		.size = __ZIO_CONTROL_SIZE, /* changed at runtime for TLV */
		.read = zobj_read_cur_ctrl,
	},
	[ZIO_BIN_ADDR] = {
		.attr = { .name = "address", .mode = ZIO_RO_PERM, },
		.size = sizeof(struct zio_addr),
		.read = zobj_show_address,
	}
};

#endif /* ZIO_HAS_BINARY_CONTROL */

/* This is only for internal use (DAN == default attribute name) */
enum zio_default_attribute_numeration {
	ZIO_DAN_NAME = 0,	/* name */
	ZIO_DAN_ENAB,	/* enable */
	ZIO_DAN_CTRI,	/* current_trigger */
	ZIO_DAN_CBUF,	/* current_buffer */
	ZIO_DAN_DNAM,	/* devname */
	ZIO_DAN_TYPE,	/* devtype */
	ZIO_DAN_FLUS,	/* flush */
	ZIO_DAN_ALAR,	/* alarms */
};

/* default zio attributes */
static struct device_attribute zio_default_attributes[] = {
	[ZIO_DAN_NAME] = __ATTR(name, ZIO_RO_PERM,
				zobj_show_name, NULL),
	[ZIO_DAN_ENAB] = __ATTR(enable, ZIO_RW_PERM,
				zobj_show_enable, zobj_store_enable),
	[ZIO_DAN_CTRI] = __ATTR(current_trigger, ZIO_RW_PERM,
				zobj_show_cur_trig, zobj_store_cur_trig),
	[ZIO_DAN_CBUF] = __ATTR(current_buffer, ZIO_RW_PERM,
				zobj_show_cur_zbuf, zobj_store_cur_zbuf),
	[ZIO_DAN_DNAM] = __ATTR(devname, ZIO_RO_PERM,
				zobj_show_devname, NULL),
	[ZIO_DAN_TYPE] = __ATTR(devtype, ZIO_RO_PERM,
				zobj_show_dev_type, NULL),
	[ZIO_DAN_ALAR] = __ATTR(alarms, ZIO_RW_PERM,
				zio_show_alarm, zio_store_alarm),
	__ATTR_NULL,
};
/* default attributes for most of the zio objects */
static struct attribute *def_obj_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_NAME].attr,
	&zio_default_attributes[ZIO_DAN_ENAB].attr,
	&zio_default_attributes[ZIO_DAN_TYPE].attr,
	NULL,
};
/* default attributes for hierachy components (dev/cset/chan) */
static struct attribute *def_hie_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_DNAM].attr,
	NULL,
};
/* default attributes for channel set */
static struct attribute *def_cset_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_CTRI].attr,
	&zio_default_attributes[ZIO_DAN_CBUF].attr,
	NULL,
};
/* default attributes for channel */
static struct attribute *def_chan_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_ALAR].attr,
	NULL,
};
/* default attributes for buffer instance */
static struct attribute *def_bi_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_NAME].attr,
	NULL,
};

/* This is only for internal use (DAG = default attribute group) */
enum zio_default_attribute_group_enumeration {
	ZIO_DAG_ALL = 0,	/* Group valid for any ZIO object*/
	ZIO_DAG_CSET,	/* Only for cset */
	ZIO_DAG_CHAN,	/* Only for channel */
	ZIO_DAG_BI,	/* Only for buffer instance */
	ZIO_DAG_HIE,	/* Only within device hierarchy (dev, cset, chan) */
};

/* default zio groups */
static struct attribute_group zio_groups[] = {
	[ZIO_DAG_ALL] = {	/* group for all zio object*/
		.attrs = def_obj_attrs_ptr,
	},
	[ZIO_DAG_CSET] = {	/* cset only group */
		.attrs = def_cset_attrs_ptr,
	},
	[ZIO_DAG_CHAN] = {
		.attrs = def_chan_attrs_ptr,
	},
	[ZIO_DAG_BI] = {	/* bi only group */
		.attrs = def_bi_attrs_ptr,
	},
	[ZIO_DAG_HIE] = {	/* group for all device hierarchy objects */
		.attrs = def_hie_attrs_ptr,
	},
};
/* default groups for whole-device */
static struct attribute_group *def_zdev_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for channel set */
static struct attribute_group *def_cset_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_CSET],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for channel */
static struct attribute_group *def_chan_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_CHAN],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for trigger instance */
static struct attribute_group *def_ti_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	NULL,
 };
/* default groups for buffer instance */
static struct attribute_group *def_bi_groups_ptr[] = {
	&zio_groups[ZIO_DAG_BI],
	NULL,
};

/* Device types */
void zio_device_release(struct device *dev)
{
	pr_debug("RELEASE %s\n", dev_name(dev));
	return;
}
void __zio_generic_device_release(struct device *dev)
{
	pr_debug("RELEASE %s\n", dev_name(dev));
	return;
}
struct device_type zdevhw_device_type = {
	.name = zdevhw_device_type_name,
	.release = __zio_generic_device_release,
};
struct device_type zdev_device_type = {
	.name = zdev_device_type_name,
	.release = zio_device_release,
	.groups = def_zdev_groups_ptr,
};
struct device_type cset_device_type = {
	.name = cset_device_type_name,
	.release = zio_device_release,
	.groups = def_cset_groups_ptr,
};
struct device_type chan_device_type = {
	.name = chan_device_type_name,
	.release = zio_device_release,
	.groups = def_chan_groups_ptr,
};
struct device_type ti_device_type = {
	.name = ti_device_type_name,
	.release = zio_device_release,
	.groups = def_ti_groups_ptr,
};
struct device_type bi_device_type = {
	.name = bi_device_type_name,
	.release = zio_device_release,
	.groups = def_bi_groups_ptr,
};

int __check_dev_zattr(struct zio_attribute_set *parent,
		      struct zio_attribute_set *this)
{
	int i, j;

	pr_debug("%s %d\n", __func__, this->n_std_attr);
	/* verify standard attribute */
	for (i = 0; i < this->n_std_attr; ++i) {
		if (this->std_zattr[i].index == ZIO_ATTR_INDEX_NONE)
			continue; /* next attribute */
		for (j = 0; j < parent->n_std_attr; ++j) {
			/*
			 * a standard attribute must be unique from a child
			 * to the root. This allow to create a consistent
			 * vector of value in control structure
			 */
			if (this->std_zattr[i].index ==
						parent->std_zattr[j].index) {
				pr_err("ZIO: attribute conflict for %s\n",
				       this->std_zattr[i].attr.attr.name);
				return -EINVAL;
			}
		}
	}
	return 0;
}
static int __check_attr(struct attribute *attr,
			const struct zio_sysfs_operations *s_op)
{
	/* check name*/
	if (!attr->name)
		return -EINVAL;

	/* check mode */
	if ((attr->mode & S_IWUGO) == S_IWUGO && !s_op->conf_set) {
		pr_err("ZIO: %s: attribute %s has write permission but "
			"no write function\n", __func__, attr->name);
		return -ENOSYS;
	}
	return 0;
}

static struct attribute_group *__allocate_group(int n_attr)
{
	struct attribute_group *group;

	group = kzalloc(sizeof(struct attribute_group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);
	group->attrs = kzalloc(sizeof(struct attribute) * n_attr, GFP_KERNEL);
	if (!group->attrs)
		return ERR_PTR(-ENOMEM);
	return group;
}

/* create a set of zio attributes: the standard one and the extended one */
int zattr_set_create(struct zio_obj_head *head,
		     const struct zio_sysfs_operations *s_op)
{
	int i, err, a_count, g_count = 0, g = 0;
	struct attribute_group **groups;
	struct zio_attribute_set *zattr_set;
	struct attribute *attr;

	pr_debug("%s\n", __func__);
	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return -EINVAL; /* message already printed */

	if (zattr_set->std_zattr && zattr_set->n_std_attr)
		++g_count;	/* There are standard attributes */
	else
		zattr_set->n_std_attr = 0;
	if (zattr_set->ext_zattr && zattr_set->n_ext_attr)
		++g_count;	/* There are extended attributes */
	else
		zattr_set->n_ext_attr = 0;

	if (!g_count)
		goto out;

	/* Allocate needed groups. dev->groups is null ended */
	groups = kzalloc(sizeof(struct attribute_group *) * (g_count + 1),
			 GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	/* Allocate standard attribute group */
	if (!zattr_set->std_zattr || !zattr_set->n_std_attr)
		goto ext;
	groups[g] = __allocate_group(zattr_set->n_std_attr);
	if (IS_ERR(groups[g]))
		return PTR_ERR(groups[g]);
	for (i = 0, a_count = 0; i < zattr_set->n_std_attr; ++i) {
		attr = &zattr_set->std_zattr[i].attr.attr;
		err = __check_attr(attr, s_op);
		switch (err) {
		case 0:
			/* valid attribute */
			groups[g]->attrs[a_count++] = attr;
			zattr_set->std_zattr[i].attr.show = zattr_show;
			zattr_set->std_zattr[i].attr.store = zattr_store;
			zattr_set->std_zattr[i].s_op = s_op;
			zattr_set->std_zattr[i].index = i;
			break;
		case -EINVAL: /* unused std attribute */
			zattr_set->std_zattr[i].index = ZIO_ATTR_INDEX_NONE;
			break;
		default:
			return err;
		}
	}
	++g;
ext:
	/* Allocate extended attribute group */
	if (!zattr_set->ext_zattr || !zattr_set->n_ext_attr)
		goto out_assign;
	groups[g] = __allocate_group(zattr_set->n_ext_attr);
	if (IS_ERR(groups[g]))
		return PTR_ERR(groups[g]);
	for (i = 0, a_count = 0; i < zattr_set->n_ext_attr; ++i) {
		attr = &zattr_set->ext_zattr[i].attr.attr;
		err = __check_attr(attr, s_op);
		if (err)
			return err;
		/* valid attribute */
		groups[g]->attrs[a_count++] = attr;
		zattr_set->ext_zattr[i].attr.show = zattr_show;
		zattr_set->ext_zattr[i].attr.store = zattr_store;
		zattr_set->ext_zattr[i].s_op = s_op;
		zattr_set->ext_zattr[i].index = i;
		zattr_set->ext_zattr[i].flags |= ZIO_ATTR_TYPE_EXT;
	}
	++g;

out_assign:
	groups[g] = NULL;
	head->dev.groups = groups;
out:
	return 0;
}
/* Remove an existent set of attributes */
void zattr_set_remove(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;
	int i;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return;
	if (!head->dev.groups)
		return;
	for (i = 0; head->dev.groups[i]; ++i) {
		kfree(head->dev.groups[i]->attrs);
		kfree(head->dev.groups[i]);
	}
}
