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

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

/*
 * Top-level ZIO objects has a unique name.
 * You can find a particular object by searching its name.
 */
static inline struct zio_object_list_item *__find_by_name(
			struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *cur;

	if (!name)
		return NULL;
	list_for_each_entry(cur, &zobj_list->list, list) {
		pr_debug("%s:%d %s=%s\n", __func__, __LINE__, cur->name, name);
		if (strcmp(cur->name, name) == 0)
			return cur; /* object found */
	}
	return NULL;
}

static inline struct zio_object_list_item *__zio_object_get(
	struct zio_cset *cset, struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *list_item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/* search for default trigger */
	list_item = __find_by_name(zobj_list, name);
	if (!list_item)
		return NULL;
	/* If different owner, try to increment its use count */
	if (cset->zdev->owner != list_item->owner
	    && !try_module_get(list_item->owner))
		return NULL;

	return list_item;
}
static struct zio_buffer_type *zio_buffer_get(struct zio_cset *cset,
					      char *name)
{
	struct zio_object_list_item *list_item;

	if (!name)
		return ERR_PTR(-EINVAL);
	if (unlikely(strlen(name) > ZIO_OBJ_NAME_LEN))
		return ERR_PTR(-EINVAL); /* name too long */

	list_item = __zio_object_get(cset, &zstat->all_buffer_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_buffer_type, head);
}
static void zio_buffer_put(struct zio_buffer_type *zbuf, struct module *dev_owner)
{
	if (zbuf->owner != dev_owner)
		module_put(zbuf->owner);
}
static struct zio_trigger_type *zio_trigger_get(struct zio_cset *cset,
						char *name)
{
	struct zio_object_list_item *list_item;

	if (!name)
		return ERR_PTR(-EINVAL);
	if (unlikely(strlen(name) > ZIO_OBJ_NAME_LEN))
		return ERR_PTR(-EINVAL); /* name too long */

	list_item = __zio_object_get(cset, &zstat->all_trigger_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_trigger_type, head);
}
static void zio_trigger_put(struct zio_trigger_type *trig, struct module *dev_owner)
{
	if (trig->owner != dev_owner)
		module_put(trig->owner);
}

/* create and initialize a new buffer instance */
static struct zio_bi *__bi_create_and_init(struct zio_buffer_type *zbuf,
					   struct zio_channel *chan)
{
	struct zio_bi *bi;
	int err;

	pr_debug("%s\n", __func__);
	/* Create buffer, ensuring it's not reentrant */
	spin_lock(&zbuf->lock);
	bi = zbuf->b_op->create(zbuf, chan);
	spin_unlock(&zbuf->lock);
	if (IS_ERR(bi)) {
		pr_err("ZIO %s: can't create buffer, error %ld\n",
		       __func__, PTR_ERR(bi));
		goto out;
	}
	/* Initialize buffer */
	spin_lock_init(&bi->lock);
	atomic_set(&bi->use_count, 0);
	bi->b_op = zbuf->b_op;
	bi->f_op = zbuf->f_op;
	bi->v_op = zbuf->v_op;
	bi->flags |= (chan->flags & ZIO_DIR);
	/* Initialize head */
	bi->head.dev.type = &bi_device_type;
	bi->head.dev.parent = &chan->head.dev;
	bi->head.zobj_type = ZIO_BI;
	snprintf(bi->head.name, ZIO_NAME_LEN, "%s-%s-%d-%d",
		 zbuf->head.name, chan->cset->zdev->head.name,
		 chan->cset->index, chan->index);
	init_waitqueue_head(&bi->q);
	/* Copy sysfs attribute from buffer type */
	err = __zattr_set_copy(&bi->zattr_set, &zbuf->zattr_set);
	if (err) {
		zbuf->b_op->destroy(bi);
		bi = ERR_PTR(err);
	}
out:
	return bi;
}
static void __bi_destroy(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	__zattr_set_free(&bi->zattr_set);
	zbuf->b_op->destroy(bi);
}
static int __bi_register(struct zio_buffer_type *zbuf, struct zio_channel *chan,
			 struct zio_bi *bi, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	dev_set_name(&bi->head.dev, name);
	/* Create attributes */
	err = zattr_set_create(&bi->head, zbuf->s_op);
	if (err)
		goto out;
	/* Register buffer instance */
	err = device_register(&bi->head.dev);
	if (err)
		goto out_reg;

	/* Add to buffer instance list */
	spin_lock(&zbuf->lock);
	list_add(&bi->list, &zbuf->list);
	spin_unlock(&zbuf->lock);
	bi->cset = chan->cset;
	bi->chan = chan;
	/* Done. This bi->chan marks everything is running */

	return 0;

out_reg:
	zattr_set_remove(&bi->head);
out:
	return err;
}
static void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	/* Remove from buffer instance list */
	spin_lock(&zbuf->lock);
	list_del(&bi->list);
	spin_unlock(&zbuf->lock);
	device_unregister(&bi->head.dev);
	/* Remove zio attribute */
	zattr_set_remove(&bi->head);
}

/* create and initialize a new disabled trigger instance */
static struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
					   struct zio_cset *cset)
{
	int err;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	/* Create trigger, ensuring it's not reentrant */
	spin_lock(&trig->lock);
	ti = trig->t_op->create(trig, cset, NULL, 0 /* FIXME: fmode_t */);
	spin_unlock(&trig->lock);
	if (IS_ERR(ti)) {
		pr_err("ZIO %s: can't create trigger, error %ld\n",
		       __func__, PTR_ERR(ti));
		goto out;
	}
	/* This is a new requirement: warn our users */
	WARN(ti->cset != cset, "Trigger creation should set \"cset\" field\n");

	/* Initialize trigger */
	spin_lock_init(&ti->lock);
	ti->t_op = trig->t_op;
	ti->flags |= cset->flags & ZIO_DIR;
	/* Initialize head */
	ti->head.dev.type = &ti_device_type;
	ti->head.dev.parent = &cset->head.dev;
	ti->head.zobj_type = ZIO_TI;
	snprintf(ti->head.name, ZIO_NAME_LEN, "%s-%s-%d",
		 trig->head.name, cset->zdev->head.name, cset->index);
	/* Copy sysfs attribute from trigger type */
	err = __zattr_set_copy(&ti->zattr_set, &trig->zattr_set);
	if (err) {
		trig->t_op->destroy(ti);
		return ERR_PTR(err);
	}
	/* Special case: nsamples */
	__ctrl_update_nsamples(ti);

out:
	return ti;

}
static void __ti_destroy(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);
	__zattr_set_free(&ti->zattr_set);
	trig->t_op->destroy(ti);
}
static int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
			 struct zio_ti *ti, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	dev_set_name(&ti->head.dev, name);
	/* Create attributes */
	err = zattr_set_create(&ti->head, trig->s_op);
	if (err)
		goto out;
	/* Register trigger instance */
	err = device_register(&ti->head.dev);
	if (err)
		goto out_reg;
	/* Add to trigger instance list */
	spin_lock(&trig->lock);
	list_add(&ti->list, &trig->list);
	spin_unlock(&trig->lock);

	return 0;

out_reg:
	zattr_set_remove(&ti->head);
out:
	return err;
}

/* The trigger must not be armed when calling this helper */
static void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);

	/* Remove from trigger instance list */
	spin_lock(&trig->lock);
	list_del(&ti->list);
	spin_unlock(&trig->lock);
	device_unregister(&ti->head.dev);
	/* Remove zio attributes */
	zattr_set_remove(&ti->head);
}

/* This is only called in process context (through a sysfs operation) */
int zio_change_current_trigger(struct zio_cset *cset, char *name)
{
	struct zio_trigger_type *trig, *trig_old = cset->trig;
	struct zio_ti *ti, *ti_old = cset->ti;
	unsigned long flags;
	int err, i;

	pr_debug("%s\n", __func__);

	/* FIXME: parse a leading "-" to mean we want it disabled */

	if (unlikely(strcmp(name, trig_old->head.name) == 0))
		return 0; /* it is the current trigger */

	trig = zio_trigger_get(cset, name);
	if (IS_ERR(trig))
		return PTR_ERR(trig);

	/* Create and register the new trigger instance */
	ti = __ti_create_and_init(trig, cset);
	if (IS_ERR(ti)) {
		err = PTR_ERR(ti);
		goto out_put;
	}
	err = __ti_register(trig, cset, ti, "trigger-tmp");
	if (err)
		goto out_destroy;

	/* Ok, we are done. Kill the current trigger to replace it*/
	zio_trigger_abort_disable(cset, 1);
	ti_old->cset = NULL;
	__ti_unregister(trig_old, ti_old);
	__ti_destroy(trig_old, ti_old);
	zio_trigger_put(trig_old, cset->zdev->owner);

	/* Set new trigger and rename "trigger-tmp" to "trigger" */
	spin_lock_irqsave(&cset->lock, flags);
	cset->trig = trig;
	cset->ti = ti;
	err = device_rename(&ti->head.dev, "trigger");
	spin_unlock_irqrestore(&cset->lock, flags);

	WARN(err, "%s: cannot rename trigger folder for"
	     " cset%d\n", __func__, cset->index);

	/* Update current control for each channel */
	for (i = 0; i < cset->n_chan; ++i)
		__zattr_trig_init_ctrl(ti, cset->chan[i].current_ctrl);

	/* Enable this new trigger (FIXME: unless the user doesn't want it) */
	spin_lock_irqsave(&cset->lock, flags);
	ti->flags &= ~ZIO_DISABLED;
	spin_unlock_irqrestore(&cset->lock, flags);

	/* Finally, arm it if so needed */
	if (zio_cset_early_arm(cset))
		zio_arm_trigger(ti);

	return 0;

out_destroy:
	__ti_destroy(trig, ti);
out_put:
	zio_trigger_put(trig, cset->zdev->owner);
	return err;
}

/*
 * This is only called in process context (through a sysfs operation)
 *
 * The code is very similar to the change of trigger above, and it must
 * temporary disable the trigger. It will remember whether it was disabled
 * when entering this thing, but later we'll have a "-" to keep it disabled.
 */
int zio_change_current_buffer(struct zio_cset *cset, char *name)
{
	struct zio_buffer_type *zbuf, *zbuf_old = cset->zbuf;
	struct zio_ti *ti = cset->ti;
	struct zio_bi **bi_vector;
	unsigned long flags, tflags;
	int i, j, err;

	pr_debug("%s\n", __func__);

	/* FIXME: parse a leading "-" to mean we want it disabled */

	if (unlikely(strcmp(name, cset->zbuf->head.name) == 0))
		return 0; /* it is the current buffer */

	zbuf = zio_buffer_get(cset, name);
	if (IS_ERR(zbuf))
		return PTR_ERR(zbuf);

	bi_vector = kzalloc(sizeof(struct zio_bi *) * cset->n_chan,
			     GFP_KERNEL);
	if (!bi_vector) {
		err = -ENOMEM;
		goto out_put;
	}

	/* If any of the instances are busy, refuse the change */
	spin_lock_irqsave(&cset->lock, flags);
	for (i = 0, j  = 0; i < cset->n_chan; ++i) {
		cset->chan[i].bi->flags |= ZIO_DISABLED;
		j += atomic_read(&cset->chan[i].bi->use_count);
	}
	/* If busy, clear the disabled thing and let it run */
	for (i = 0; i < cset->n_chan; ++i) {
		if (j)
			cset->chan[i].bi->flags &= ~ZIO_DISABLED;
	}
	spin_unlock_irqrestore(&cset->lock, flags);

	if (j) {
		err = -EBUSY;
		goto out_put;
	}

	/* Create a new buffer instance for each channel of the cset */
	for (i = 0; i < cset->n_chan; ++i) {
		bi_vector[i] = __bi_create_and_init(zbuf, &cset->chan[i]);
		if (IS_ERR(bi_vector[i])) {
			pr_err("%s can't create buffer instance\n", __func__);
			err = PTR_ERR(bi_vector[i]);
			goto out_create;
		}
		err = __bi_register(zbuf, &cset->chan[i], bi_vector[i],
				    "buffer-tmp");
		if (err) {
			pr_err("%s can't register buffer instance\n", __func__);
			__bi_destroy(zbuf, bi_vector[i]);
			goto out_create;
		}
	}
	tflags = zio_trigger_abort_disable(cset, 1);

	for (i = 0; i < cset->n_chan; ++i) {
		/* Delete old buffer instance */
		__bi_unregister(zbuf_old, cset->chan[i].bi);
		__bi_destroy(zbuf_old, cset->chan[i].bi);
		/* Assign new buffer instance */
		cset->chan[i].bi = bi_vector[i];
		/* Rename buffer-tmp to trigger */
		err = device_rename(&cset->chan[i].bi->head.dev, "buffer");
		if (err)
			WARN(1, "%s: cannot rename buffer folder for"
				" cset%d:chan%d\n", __func__, cset->index, i);
	}
	cset->zbuf = zbuf;
	kfree(bi_vector);
	zio_buffer_put(zbuf_old, cset->zdev->owner);

	/* exit the disabled region: keep it disabled if needed */
	spin_lock_irqsave(&cset->lock, flags);
	ti->flags = (ti->flags & ~ZIO_DISABLED) | tflags;
	spin_unlock_irqrestore(&cset->lock, flags);

	/* Finally, arm the trigger if so needed */
	if (zio_cset_early_arm(cset))
		zio_arm_trigger(ti);

	return 0;

out_create:
	for (j = i-1; j >= 0; --j) {
		__bi_unregister(zbuf, bi_vector[j]);
		__bi_destroy(zbuf, bi_vector[j]);
	}
	kfree(bi_vector);
out_put:
	zio_buffer_put(zbuf, cset->zdev->owner);
	return err;
}

static int cset_set_trigger(struct zio_cset *cset)
{
	struct zio_trigger_type *trig;
	char *name = NULL;

	if (cset->trig)
		return -EINVAL;

	if (cset->default_trig) /* cset default */
		name = cset->default_trig;
	if (cset->zdev->preferred_trigger) /* preferred device trigger */
		name = cset->zdev->preferred_trigger;

	trig = zio_trigger_get(cset, name);
	if (IS_ERR(trig)) {
		dev_dbg(&cset->head.dev, "no trigger \"%s\" (error %li), using "
			 "default\n", name, PTR_ERR(trig));
		trig = zio_trigger_get(cset, ZIO_DEFAULT_TRIGGER);
	}
	if (IS_ERR(trig))
		return PTR_ERR(trig);

	cset->trig = trig;
	return 0;
}
static int cset_set_buffer(struct zio_cset *cset)
{
	struct zio_buffer_type *zbuf;
	char *name = NULL;

	if (cset->zbuf)
		return -EINVAL;

	if (cset->default_zbuf) /* cset default */
		name = cset->default_zbuf;
	if (cset->zdev->preferred_buffer) /* preferred device buffer */
		name = cset->zdev->preferred_buffer;

	zbuf = zio_buffer_get(cset, name);
	if (IS_ERR(zbuf)) {
		dev_dbg(&cset->head.dev, "no buffer \"%s\" (error %li), using "
			 "default\n", name, PTR_ERR(zbuf));
		zbuf = zio_buffer_get(cset, ZIO_DEFAULT_BUFFER);
	}
	if (IS_ERR(zbuf))
		return PTR_ERR(zbuf);

	cset->zbuf = zbuf;
	return 0;
}

/*
 * Return the resolution bit of the zio device. The function look in each
 * hierarchy level to find this value
 */
static uint16_t __get_nbits(struct zio_channel *chan)
{
	struct zio_device *zdev;
	struct zio_cset *cset;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (chan->zattr_set.std_zattr)
		if (chan->zattr_set.std_zattr[ZIO_ATTR_NBITS].value)
			return chan->zattr_set.std_zattr[ZIO_ATTR_NBITS].value;
	cset = chan->cset;
	if (cset->zattr_set.std_zattr)
		if (cset->zattr_set.std_zattr[ZIO_ATTR_NBITS].value)
			return cset->zattr_set.std_zattr[ZIO_ATTR_NBITS].value;
	zdev = cset->zdev;
	if (zdev->zattr_set.std_zattr)
		if (zdev->zattr_set.std_zattr[ZIO_ATTR_NBITS].value)
			return zdev->zattr_set.std_zattr[ZIO_ATTR_NBITS].value;

	/* The attr. is optional, so devices with no attributes are allowed */
	return chan->cset->ssize * BITS_PER_BYTE;
}
/*
 * chan_register
 *
 * @chan: channel to register
 * @chan_t: channel template
 *
 * if the channel template exists, this function copies it and registr the copy
 * as child of a cset. It is important to register or unregister all the
 * channels of a cset at the same time to prevent overlaps in the minors.
 *
 * NOTE: The channel template doesn't need a validation because ZIO already
 * done it during driver registration
 */
static int chan_register(struct zio_channel *chan, struct zio_channel *chan_t)
{
	struct zio_control *ctrl;
	struct zio_bi *bi;
	int err, i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return -EINVAL;
	chan->head.zobj_type = ZIO_CHAN;

	/* Copy from template, initialize and verify zio attributes */
	if (chan_t) { /* ZIO_CSET_CHAN_TEMPLATE is set */
		chan->flags |= chan_t->flags;
		if (chan_t->zattr_set.std_zattr)
			chan_t->zattr_set.n_std_attr = _ZIO_DEV_ATTR_STD_NUM;
		err = __zattr_set_copy(&chan->zattr_set, &chan_t->zattr_set);
		if (err)
			goto out_zattr_copy;
	}

	err = zattr_set_create(&chan->head, chan->cset->zdev->s_op);
	if (err)
		goto out_zattr_create;
	/* Check attribute hierarchy */
	err = __check_dev_zattr(&chan->cset->zattr_set, &chan->zattr_set);
	if (err)
		goto out_zattr_check;
	err = __check_dev_zattr(&chan->cset->zdev->zattr_set, &chan->zattr_set);
	if (err)
		goto out_zattr_check;

	/* Allocate, initialize and assign a current control for channel */
	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl) {
		err = -ENOMEM;
		goto out_zattr_check;
	}
	ctrl->nsamples = chan->cset->ti->nsamples;
	ctrl->nbits = __get_nbits(chan); /* may be zero */
	/* ctrl->addr.family = PF_ZIO */
	ctrl->addr.cset = chan->cset->index;
	ctrl->addr.chan = chan->index;
	strncpy(ctrl->addr.devname, chan->cset->zdev->head.name,
		sizeof(ctrl->addr.devname));
	ctrl->ssize = chan->cset->ssize;
	chan->current_ctrl = ctrl;

	/* Initialize and register channel device */
	if (strlen(chan->head.name) == 0)
		snprintf(chan->head.name, ZIO_NAME_LEN, "chan%i", chan->index);
	dev_set_name(&chan->head.dev, chan->head.name);
	chan->head.dev.type = &chan_device_type;
	chan->head.dev.parent = &chan->cset->head.dev;
	err = device_register(&chan->head.dev);
	if (err)
		goto out_ctrl_bits;
	if (ZIO_HAS_BINARY_CONTROL) {
		for (i = 0; i < __ZIO_BIN_ATTR_NUM; ++i) {
			/* Create the sysfs binary file for the current control */
			err = sysfs_create_bin_file(&chan->head.dev.kobj,
						    &zio_bin_attr[i]);
			if (err)
				goto out_bin_attr;
		}
	}
	/* Create buffer */
	bi = __bi_create_and_init(chan->cset->zbuf, chan);
	if (IS_ERR(bi)) {
		err = PTR_ERR(bi);
		goto out_bin_attr;
	}
	err = __bi_register(chan->cset->zbuf, chan, bi, "buffer");
	if (err)
		goto out_buf_reg;
	/* Assign the buffer instance to this channel */
	chan->bi = bi;
	/* Create channel char devices*/
	err = zio_create_chan_devices(chan);
	if (err)
		goto out_cdev_create;

	return 0;

out_cdev_create:
	__bi_unregister(chan->cset->zbuf, bi);
out_buf_reg:
	__bi_destroy(chan->cset->zbuf, bi);
out_bin_attr:
	if (ZIO_HAS_BINARY_CONTROL) {
		while (i--)
			sysfs_remove_bin_file(&chan->head.dev.kobj,
					      &zio_bin_attr[i]);
	}

	device_unregister(&chan->head.dev);
out_ctrl_bits:
	zio_free_control(ctrl);
out_zattr_check:
	zattr_set_remove(&chan->head);
out_zattr_create:
	if (chan_t)
		__zattr_set_free(&chan->zattr_set);
out_zattr_copy:
	return err;
}

static void chan_unregister(struct zio_channel *chan)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return;
	zio_destroy_chan_devices(chan);
	/* destroy buffer instance */
	__bi_unregister(chan->cset->zbuf, chan->bi);
	__bi_destroy(chan->cset->zbuf, chan->bi);
	if (ZIO_HAS_BINARY_CONTROL)
		for (i = 0; i < __ZIO_BIN_ATTR_NUM; ++i)
			sysfs_remove_bin_file(&chan->head.dev.kobj,
					      &zio_bin_attr[i]);
	device_unregister(&chan->head.dev);
	zio_free_control(chan->current_ctrl);
	zattr_set_remove(&chan->head);
	if (chan->cset->flags & ZIO_CSET_CHAN_TEMPLATE)
		__zattr_set_free(&chan->zattr_set);
}

/*
 * cset_registration
 *
 * @cset: cset to register
 * @cset_t: cset template
 *
 * the function copies a cset from a cset template and then it register it
 * as child of a zio device.
 *
 * NOTE: The cset template doesn't need a validation because ZIO already done
 * it during driver registration
 */
static int cset_register(struct zio_cset *cset, struct zio_cset *cset_t)
{
	int i, j, err = 0, size;
	unsigned long flags;
	struct zio_channel *chan_tmp;
	struct zio_ti *ti = NULL;

	pr_debug("%s:%d\n", __func__, __LINE__);
	cset->head.zobj_type = ZIO_CSET;

	/* Get an available minor base */
	err = zio_minorbase_get(cset);
	if (err < 0) {
		pr_err("ZIO: no minors available\n");
		return err;
	}

	/* Copy from template, initialize and verify zio attributes */
	if (cset_t->zattr_set.std_zattr)
		cset_t->zattr_set.n_std_attr = _ZIO_DEV_ATTR_STD_NUM;
	err = __zattr_set_copy(&cset->zattr_set, &cset_t->zattr_set);
	if (err)
		goto out_zattr_copy;
	err = zattr_set_create(&cset->head, cset->zdev->s_op);
	if (err)
		goto out_zattr_create;
	err = __check_dev_zattr(&cset->zdev->zattr_set, &cset->zattr_set);
	if (err)
		goto out_zattr_check;

	/* Initialize and register zio device */
	if (strlen(cset->head.name) == 0)
		snprintf(cset->head.name, ZIO_NAME_LEN, "cset%i", cset->index);
	dev_set_name(&cset->head.dev, cset->head.name);
	spin_lock_init(&cset->lock);
	cset->head.dev.type = &cset_device_type;
	cset->head.dev.parent = &cset->zdev->head.dev;
	err = device_register(&cset->head.dev);
	if (err)
		goto out_zattr_check;
	/*
	 * The cset must have a buffer type. If none is associated
	 * to the cset, ZIO selects the preferred or default one.
	 */
	err = cset_set_buffer(cset);
	if (err)
		goto out_buf;
	/*
	 * The cset must have a trigger type. If none  is associated
	 * to the cset, ZIO selects the default or preferred one.
	 * This is done late because each channel must be ready when
	 * the trigger fires.
	 */
	err = cset_set_trigger(cset);
	if (err)
		goto out_trig;
	ti = __ti_create_and_init(cset->trig, cset);
	if (IS_ERR(ti)) {
		err = PTR_ERR(ti);
		goto out_trig;
	}
	err = __ti_register(cset->trig, cset, ti, "trigger");
	if (err)
		goto out_tr;
	cset->ti = ti;
	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Allocate a new vector of channel for the new zio cset instance */
	size = sizeof(struct zio_channel) * cset->n_chan;
	cset->chan = kzalloc(size, GFP_KERNEL);
	if (!cset->chan)
		goto out_n_chan;
	if (cset->chan_template || cset_t->chan)
		cset->flags |= ZIO_CSET_CHAN_TEMPLATE;

	/* Register all child channels */
	for (i = 0; i < cset->n_chan; i++) {
		cset->chan[i].index = i;
		cset->chan[i].cset = cset;
		cset->chan[i].ti = cset->ti;
		cset->chan[i].flags |= cset->flags & ZIO_DIR;
		chan_tmp = NULL;
		if (cset->chan_template)
			chan_tmp = cset->chan_template;
		else if (cset_t->chan)
			chan_tmp = &cset->chan[i];
		mutex_init(&cset->chan[i].user_sem);
		err = chan_register(&cset->chan[i], chan_tmp);
		if (err)
			goto out_reg;
	}
	/* Private initialization function */
	if (cset->init) {
		err = cset->init(cset);
		if (err)
			goto out_reg;
	}
	spin_lock(&zstat->lock);
	list_add(&cset->list_cset, &zstat->list_cset);
	spin_unlock(&zstat->lock);

	/* Finally, enable the trigger and arm it if needed */
	spin_lock_irqsave(&cset->lock, flags);
	ti->flags &= ~ZIO_DISABLED;
	spin_unlock_irqrestore(&cset->lock, flags);

	if (zio_cset_early_arm(cset))
		zio_arm_trigger(ti);

	return 0;

out_reg:
	for (j = i-1; j >= 0; j--)
		chan_unregister(&cset->chan[j]);
	kfree(cset->chan);
out_n_chan:
	__ti_unregister(cset->trig, ti);
out_tr:
	__ti_destroy(cset->trig, ti);
out_trig:
	zio_trigger_put(cset->trig, cset->zdev->owner);
	cset->trig = NULL;
	zio_buffer_put(cset->zbuf, cset->zdev->owner);
	cset->zbuf = NULL;
out_buf:
	device_unregister(&cset->head.dev);
out_zattr_check:
	zattr_set_remove(&cset->head);
out_zattr_create:
	__zattr_set_free(&cset->zattr_set);
out_zattr_copy:
	zio_minorbase_put(cset);
	return err;
}

static void cset_unregister(struct zio_cset *cset)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!cset)
		return;
	/* Remove from csets list*/
	spin_lock(&zstat->lock);
	list_del(&cset->list_cset);
	spin_unlock(&zstat->lock);
	/* Make it idle */
	zio_trigger_abort_disable(cset, 1);
	/* Private exit function */
	if (cset->exit)
		cset->exit(cset);
	/* Unregister all child channels */
	for (i = 0; i < cset->n_chan; i++)
		chan_unregister(&cset->chan[i]);
	kfree(cset->chan);
	/* destroy instance and decrement trigger usage */
	__ti_unregister(cset->trig, cset->ti);
	__ti_destroy(cset->trig,  cset->ti);
	zio_trigger_put(cset->trig, cset->zdev->owner);
	cset->trig = NULL;

	/* decrement buffer usage */
	zio_buffer_put(cset->zbuf, cset->zdev->owner);
	cset->zbuf = NULL;

	device_unregister(&cset->head.dev);
	zattr_set_remove(&cset->head);
	__zattr_set_free(&cset->zattr_set);
	/* Release the group of minors */
	zio_minorbase_put(cset);
}

/*
 * Register a generic zio object. It can be a device, a buffer type or
 * a trigger type.
 */
static int zobj_register(struct zio_object_list *zlist,
			 struct zio_obj_head *head,
			 struct module *owner)
{
	struct zio_object_list_item *item;

	if (!owner) {
		pr_err("ZIO: missing owner for %s", head->name);
		return -EINVAL;
	}
	/* Add to object list */
	item = kmalloc(sizeof(struct zio_object_list_item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->obj_head = head;
	item->owner = owner;
	strncpy(item->name, head->name, ZIO_OBJ_NAME_LEN);
	/* add to the object list*/
	spin_lock(&zstat->lock);
	list_add(&item->list, &zlist->list);
	spin_unlock(&zstat->lock);
	return 0;
}
static void zobj_unregister(struct zio_object_list *zlist,
		struct zio_obj_head *head)
{
	struct zio_object_list_item *item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!head)
		return;
	list_for_each_entry(item, &zlist->list, list) {
		if (item->obj_head == head) {
			/* Remove from object list */
			spin_lock(&zstat->lock);
			list_del(&item->list);
			spin_unlock(&zstat->lock);
			kfree(item);
			break;
		}
	}
}

/*
 * __zdev_register
 * ZIO uses this function to create a new instance of a zio device. The new
 * instance is child of a generic zio_device registered before to make the
 * bus work correctly.
 */
int __zdev_register(struct zio_device *parent,
		    const struct zio_device_id *id)
{
	struct zio_device *zdev, *tmpl;
	const char *pname;
	int err, size, i;

	zdev = kzalloc(sizeof(struct zio_device), GFP_KERNEL);
	if (!zdev)
		return -ENOMEM;

	tmpl = id->template;

	spin_lock_init(&zdev->lock);
	zdev->priv_d = parent->priv_d;
	zdev->head.zobj_type = ZIO_DEV;
	zdev->head.dev.parent = &parent->head.dev;
	zdev->dev_id = parent->dev_id;
	zdev->head.dev.type = &zdev_device_type;
	zdev->head.dev.bus = &zio_bus_type;
	/* Name was verified during zio_register_device */
	strncpy(zdev->head.name, parent->head.name, ZIO_OBJ_NAME_LEN);
	/* +3 to cut the "hw-" prefix of the parent device */
	pname = dev_name(&parent->head.dev) + 3;
	dev_set_name(&zdev->head.dev, pname);

	zdev->owner = parent->owner; /* FIXME which owner? */
	zdev->flags = tmpl->flags;
	zdev->s_op = tmpl->s_op;
	zdev->preferred_buffer = tmpl->preferred_buffer;
	zdev->preferred_trigger = tmpl->preferred_trigger;
	zdev->n_cset = tmpl->n_cset;


	if (tmpl->zattr_set.std_zattr)
		tmpl->zattr_set.n_std_attr = _ZIO_DEV_ATTR_STD_NUM;
	/* Create standard and extended sysfs attribute for device */
	err = __zattr_set_copy(&zdev->zattr_set, &tmpl->zattr_set);
	if (err)
		goto out_copy;
	err = zattr_set_create(&zdev->head, zdev->s_op);
	if (err)
		goto out_create;

	/* Register zio device */
	err = zobj_register(&zstat->all_devices, &zdev->head, zdev->owner);
	if (err)
		goto out_reg;
	err = device_register(&zdev->head.dev);
	if (err)
		goto out_dev;


	size = sizeof(struct zio_cset) * zdev->n_cset;
	zdev->cset = kzalloc(size, GFP_KERNEL);
	if (!zdev->cset) {
		err = -ENOMEM;
		goto out_alloc_cset;
	}
	memcpy(zdev->cset, tmpl->cset, size);
	/* Register all csets */
	for (i = 0; i < zdev->n_cset; ++i) {
		zdev->cset[i].index = i;
		zdev->cset[i].zdev = zdev;
		err = cset_register(&zdev->cset[i], &tmpl->cset[i]);
		if (err)
			goto out_cset;
	}
	/* Fix extended attribute index */
	err = __zattr_dev_init_ctrl(zdev);
	if (err)
		goto out_cset;

	return 0;
out_cset:
	while (--i >= 0)
		cset_unregister(&zdev->cset[i]);
	kfree(zdev->cset);
out_alloc_cset:
	device_unregister(&zdev->head.dev);
out_dev:
	zobj_unregister(&zstat->all_devices, &zdev->head);
out_reg:
	zattr_set_remove(&zdev->head);
out_create:
	__zattr_set_free(&zdev->zattr_set);
out_copy:
	kfree(zdev);
	return err;
}
static void __zdev_unregister(struct zio_device *zdev)
{
	int i;

	for (i = 0; i < zdev->n_cset; ++i)
		cset_unregister(&zdev->cset[i]);
	kfree(zdev->cset);
	device_unregister(&zdev->head.dev);
	zobj_unregister(&zstat->all_devices, &zdev->head);
	zattr_set_remove(&zdev->head);
	__zattr_set_free(&zdev->zattr_set);
	kfree(zdev);
}

struct zio_device *zio_allocate_device(void)
{
	struct zio_device *zdev;

	/* Allocate a new zio device to use as instance of zdev_t */
	zdev = kzalloc(sizeof(struct zio_device), GFP_KERNEL);
	if (!zdev)
		return ERR_PTR(-ENOMEM);
	/* Set this device as generic zio device */
	zdev->head.dev.type = &zdevhw_device_type;
	zdev->head.dev.bus = &zio_bus_type;

	return zdev;
}
EXPORT_SYMBOL(zio_allocate_device);
void zio_free_device(struct zio_device *zdev)
{
	kfree(zdev);
}
EXPORT_SYMBOL(zio_free_device);

/*
 * zobj_unique_name
 *
 * Return the number of object registered with the same name within the same
 * list
 *
 * @zobj_list: list to use
 * @name: name to check
 */
static int zobj_unique_name(struct zio_object_list *zobj_list, const char *name)
{
	struct zio_object_list_item *cur;
	unsigned int conflict = 0;

	if (!name) {
		pr_err("ZIO: you must spicify a name\n");
		return -EINVAL;
	}
	if (!strlen(name)) {
		pr_err("ZIO: name cannot be an empty string\n");
		return -EINVAL;
	}
	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		pr_warning("ZIO: name too long, cut to %d characters\n",
			ZIO_OBJ_NAME_LEN);

	pr_debug("%s\n", __func__);
	list_for_each_entry(cur, &zobj_list->list, list) {
		if (strcmp(cur->obj_head->name, name))
			continue; /* no conflict */
		conflict++;
	}

	return conflict;
}

/*
 * zio_register_device
 *
 * Register an empty zio_device, when it match with a driver it will be
 * filled with driver information. Registration sets the correct name to
 * the device and it adds the device to the device list; then it registers
 * the device.
 *
 * @zdev: an empty zio_device allocated with zio_allocate_device
 * @name: name of the device to register
 * @dev_id: device identifier, if 0 ZIO use an autoindex
 */
int zio_register_device(struct zio_device *zdev, const char *name,
			uint32_t dev_id)
{
	int n_conflict;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Verify if it is a valid name */
	n_conflict = zobj_unique_name(&zstat->all_devices, name);
	if (n_conflict < 0)
		return n_conflict;

	strncpy(zdev->head.name, name, ZIO_OBJ_NAME_LEN);
	zdev->dev_id = dev_id ? dev_id : n_conflict;
	dev_set_name(&zdev->head.dev, "hw-%s-%04x",
		     zdev->head.name, zdev->dev_id);

	return device_register(&zdev->head.dev);
}
EXPORT_SYMBOL(zio_register_device);

/*
 * __zdev_match_child
 * ZIO uses this function only to find the real zio_device, which is child of
 * the generic zio_device
 */
static int __zdev_match_child(struct device *dev, void *data)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (dev->type == &zdev_device_type)
		return 1;
	return 0;
}
void zio_unregister_device(struct zio_device *zdev)
{
	struct device *dev;

	/*
	 * the child of a generic zio_device could be only a real zio_device.
	 * If it exists, unregister it
	 */
	dev = device_find_child(&zdev->head.dev, NULL, __zdev_match_child);
	if (dev)
		__zdev_unregister(to_zio_dev(dev));

	pr_info("ZIO: device %s removed\n", dev_name(&zdev->head.dev));
	device_unregister(&zdev->head.dev);
}
EXPORT_SYMBOL(zio_unregister_device);

/* Register a buffer into the available buffer list */
int zio_register_buf(struct zio_buffer_type *zbuf, const char *name)
{
	int err;
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zbuf)
		return -EINVAL;
	if (!zbuf->f_op) {
		pr_err("%s: no file operations provided by \"%s\" buffer\n",
		       __func__, name);
		return -EINVAL;
	}

	/* Verify if it is a valid name */
	err = zobj_unique_name(&zstat->all_buffer_types, name);
	if (err)
		return err < 0 ? err : -EBUSY;
	strncpy(zbuf->head.name, name, ZIO_OBJ_NAME_LEN);

	err = zio_init_buffer_fops(zbuf);
	if (err < 0)
		return err;

	zbuf->head.zobj_type = ZIO_BUF;
	err = zobj_register(&zstat->all_buffer_types, &zbuf->head, zbuf->owner);
	if (err) {
		zio_fini_buffer_fops(zbuf);
		return err;
	}
	if (zbuf->zattr_set.std_zattr)
		zbuf->zattr_set.n_std_attr = _ZIO_BUF_ATTR_STD_NUM;
	INIT_LIST_HEAD(&zbuf->list);
	spin_lock_init(&zbuf->lock);

	return 0;
}
EXPORT_SYMBOL(zio_register_buf);

void zio_unregister_buf(struct zio_buffer_type *zbuf)
{
	if (!zbuf)
		return;
	zio_fini_buffer_fops(zbuf);
	zobj_unregister(&zstat->all_buffer_types, &zbuf->head);
}
EXPORT_SYMBOL(zio_unregister_buf);

/* Register a trigger into the available trigger list */
int zio_register_trig(struct zio_trigger_type *trig, const char *name)
{
	struct zio_attribute *zattr;
	int err;

	if (!trig)
		return -EINVAL;
	zattr = trig->zattr_set.std_zattr;
	if (!zattr)
		goto err_nsamp;
	/*
	 * The trigger must define how many samples acquire, so POST_SAMP or
	 * PRE_SAMP attribute must be available
	 */
	if (!(zattr[ZIO_ATTR_TRIG_POST_SAMP].attr.attr.mode ||
	      zattr[ZIO_ATTR_TRIG_PRE_SAMP].attr.attr.mode))
		goto err_nsamp;
	/* Verify if it is a valid name */
	err = zobj_unique_name(&zstat->all_trigger_types, name);
	if (err)
		return err < 0 ? err : -EBUSY;
	strncpy(trig->head.name, name, ZIO_OBJ_NAME_LEN);
	trig->head.zobj_type = ZIO_TRG;
	err = zobj_register(&zstat->all_trigger_types, &trig->head,
			    trig->owner);
	if (err)
		return err;
	trig->zattr_set.n_std_attr = _ZIO_TRG_ATTR_STD_NUM;
	INIT_LIST_HEAD(&trig->list);
	spin_lock_init(&trig->lock);

	return 0;

err_nsamp:
	pr_err("%s: trigger \"%s\" lacks mandatory \"pre-sample\" or"
		"\"post-sample\" attribute", __func__, name);
	return -EINVAL;
}
EXPORT_SYMBOL(zio_register_trig);

void zio_unregister_trig(struct zio_trigger_type *trig)
{
	if (!trig)
		return;
	zobj_unregister(&zstat->all_trigger_types, &trig->head);
}
EXPORT_SYMBOL(zio_unregister_trig);
