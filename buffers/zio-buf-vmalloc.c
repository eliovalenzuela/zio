/* Alessandro Rubini for CERN, 2012, GNU GPLv2 or later */

/*
 * This is a vmalloc-based buffer for the ZIO framework. It supports
 * mmap from user space and can be used as basis for dma-capable I/O.
 * The prefix of all local code/data is till "zbk_" so it's easier
 * for our users to use "diff" among the two implementations and see
 * what changes.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

/*
 * We export a linear buffer to user space, for a single mmap call.
 * The circular buffer is managed by the ZIO first-fit allocator
 */
struct zbk_instance {
	struct zio_bi bi;
	struct list_head list; /* items, one per block */
	struct zio_ffa *ffa;
	void *data;
	unsigned long size;
};
#define to_zbki(bi) container_of(bi, struct zbk_instance, bi)

static struct kmem_cache *zbk_slab;


/* The list in the structure above collects a bunch of these */
struct zbk_item {
	struct zio_block block;
	struct list_head list;	/* item list */
	struct zbk_instance *instance;
	unsigned long begin;
	size_t len; /* block.datalen may change, so save this */
};
#define to_item(block) container_of(block, struct zbk_item, block);

static ZIO_ATTR_DEFINE_STD(ZIO_BUF, zbk_std_zattr) = {
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_MAXKB, S_IRUGO | S_IWUGO, 0x0, 128),
};

static int zbk_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	if (0) {
		zattr->value = usr_val;
	} else {
		/* Temporarily, until I keep track of active maps */
		return -EBUSY;
	}
	return 0;
}
struct zio_sysfs_operations zbk_sysfs_ops = {
	.conf_set = zbk_conf_set,
};

/* Alloc is called by the trigger (for input) or by f->write (for output) */
static struct zio_block *zbk_alloc_block(struct zio_bi *bi,
					 struct zio_control *ctrl,
					 size_t datalen, gfp_t gfp)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	unsigned long offset;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* alloc item and data. Control remains null at this point */
	item = kmem_cache_alloc(zbk_slab, gfp);
	offset = zio_ffa_alloc(zbki->ffa, datalen, gfp);
	if (!item || offset == ZIO_FFA_NOSPACE)
		goto out_free;
	memset(item, 0, sizeof(*item));
	item->begin = offset;
	item->len = datalen;
	item->block.data = zbki->data + offset;
	item->block.datalen = datalen;
	item->instance = zbki;

	bi->chan->current_ctrl->mem_offset = offset;
	zio_set_ctrl(&item->block, ctrl);
	return &item->block;

out_free:
	if (offset != ZIO_FFA_NOSPACE)
		zio_ffa_free_s(zbki->ffa, offset, datalen);
	if (item)
		kmem_cache_free(zbk_slab, item);
	return ERR_PTR(-ENOMEM);
}

/* Free is called by f->read (for input) or by the trigger (for output) */
static void zbk_free_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	struct zio_control *ctrl;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ctrl = zio_get_ctrl(block);
	item = to_item(block);
	zbki = item->instance;
	zio_ffa_free_s(zbki->ffa, item->begin, item->len);
	zio_free_control(ctrl);
	kmem_cache_free(zbk_slab, item);
}

/* When write() stores the first block, we try pushing it */
static inline int __try_push(struct zio_bi *bi, struct zio_channel *chan,
			     struct zio_block *block)
{
	struct zio_ti *ti = chan->cset->ti;
	int pushed;

	/* chek if trigger is disabled */
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED))
		return 0;
	/*
	 * If push succeeds and the device eats data immediately,
	 * the trigger may call retr_block right now.  So
	 * release the lock but also say we can't retrieve now.
	 */
	bi->flags |= ZIO_BI_PUSHING;
	spin_unlock(&bi->lock);
	pushed = (ti->t_op->push_block(ti, chan, block) == 0);
	spin_lock(&bi->lock);
	bi->flags &=  ~ZIO_BI_PUSHING;
	return pushed;
}

/* Store is called by the trigger (for input) or by f->write (for output) */
static int zbk_store_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zio_channel *chan = bi->chan;
	struct zbk_item *item;
	int awake = 0, pushed = 0, output, first;

	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, block);

	if (unlikely(!zio_get_ctrl(block))) {
		WARN_ON(1);
		return -EINVAL;
	}

	item = to_item(block);
	output = (bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT;

	/* add to the buffer instance or push to the trigger */
	spin_lock(&bi->lock);
	first = list_empty(&zbki->list);
	list_add_tail(&item->list, &zbki->list);
	if (first) {
		if (unlikely(output))
			pushed = __try_push(bi, chan, block);
		else
			awake = 1;
	}
	if (pushed)
		list_del(&item->list);
	spin_unlock(&bi->lock);

	/* if first input, awake user space */
	if (awake)
		wake_up_interruptible(&bi->q);
	return 0;
}

/* Retr is called by f->read (for input) or by the trigger (for output) */
static struct zio_block *zbk_retr_block(struct zio_bi *bi)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	struct zio_ti *ti;
	struct list_head *first;
	int awake = 0;

	zbki = to_zbki(bi);

	spin_lock(&bi->lock);
	if (list_empty(&zbki->list) || bi->flags & ZIO_BI_PUSHING)
		goto out_unlock;
	first = zbki->list.next;
	item = list_entry(first, struct zbk_item, list);
	list_del(&item->list);
	awake = 1;
	spin_unlock(&bi->lock);

	if (awake && ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT))
		wake_up_interruptible(&bi->q);
	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, item);
	return &item->block;

out_unlock:
	spin_unlock(&bi->lock);
	/* There is no data in buffer, and we may pull to have data soon */
	ti = bi->cset->ti;
	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT && ti->t_op->pull_block) {
		/* chek if trigger is disabled */
		if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED))
			return NULL;
		ti->t_op->pull_block(ti, bi->chan);
	}
	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, NULL);
	return NULL;
}

/* Create is called by zio for each channel electing to use this buffer type */
static struct zio_bi *zbk_create(struct zio_buffer_type *zbuf,
				 struct zio_channel *chan)
{
	struct zbk_instance *zbki;
	struct zio_ffa *ffa;
	void *data;
	size_t size;

	pr_debug("%s:%d\n", __func__, __LINE__);

	size = 1024 * zbuf->zattr_set.std_zattr[ZIO_ATTR_ZBUF_MAXKB].value;

	zbki = kzalloc(sizeof(*zbki), GFP_KERNEL);
	ffa = zio_ffa_create(0, size);
	data = vmalloc(size);
	if (!zbki || !ffa || !data)
		goto out_nomem;
	zbki->size = size;
	zbki->ffa = ffa;
	zbki->data = data;
	INIT_LIST_HEAD(&zbki->list);

	/* all the fields of zio_bi are initialied by the caller */
	return &zbki->bi;
out_nomem:
	kfree(zbki);
	zio_ffa_destroy(ffa);
	if (data)
		vfree(data);
	return ERR_PTR(-ENOMEM);
}

/* destroy is called by zio on channel removal or if it changes buffer type */
static void zbk_destroy(struct zio_bi *bi)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	struct list_head *pos, *tmp;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* no need to lock here, zio ensures we are not active */
	list_for_each_safe(pos, tmp, &zbki->list) {
		item = list_entry(pos, struct zbk_item, list);
		zbk_free_block(&zbki->bi, &item->block);
	}
	vfree(zbki->data);
	zio_ffa_destroy(zbki->ffa);
	kfree(zbki);
}

static const struct zio_buffer_operations zbk_buffer_ops = {
	.alloc_block =	zbk_alloc_block,
	.free_block =	zbk_free_block,
	.store_block =	zbk_store_block,
	.retr_block =	zbk_retr_block,
	.create =	zbk_create,
	.destroy =	zbk_destroy,
};

/*
 * To support mmap we implement the vm operations. We'll need
 * refcounting later, to safely change the buffer size (which we
 * refuse by now)
 */
static int zbk_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct file *f = vma->vm_file;
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	struct zbk_instance *zbki = to_zbki(bi);
	long off = vmf->pgoff * PAGE_SIZE;
	struct page *p;
	void *addr;

	if (priv->type == ZIO_CDEV_CTRL)
		return VM_FAULT_SIGBUS;

	pr_debug("%s: fault at %li (size %li)\n", __func__, off, zbki->size);
	if (off > zbki->size)
		return VM_FAULT_SIGBUS;

	addr = zbki->data + off;
	pr_debug("%s: uaddr %p, off %li: kaddr %p\n", __func__,
		 vmf->virtual_address, off, addr);
	p = vmalloc_to_page(addr);
	get_page(p);
	vmf->page = p;
	return 0;
}

static struct vm_operations_struct zbk_vma_ops = {
	/* FIXME: open and close for refcounting */
	.fault = zbk_fault,
};

static struct zio_buffer_type zbk_buffer = {
	.owner =	THIS_MODULE,
	.zattr_set = {
		.std_zattr = zbk_std_zattr,
	},
	.s_op = &zbk_sysfs_ops,
	.b_op = &zbk_buffer_ops,
	.v_op = &zbk_vma_ops,
	.f_op = &zio_generic_file_operations,
};

static int __init zbk_init(void)
{
	int ret;

	/* Can't use "zbk_item" as name and KMEM_CACHE_NAMED is not there */
	zbk_slab = kmem_cache_create("zio-vmalloc", sizeof(struct zbk_item),
				     __alignof__(struct zbk_item), 0, NULL);
	if (!zbk_slab)
		return -ENOMEM;
	ret = zio_register_buf(&zbk_buffer, "vmalloc");
	if (ret < 0)
		kmem_cache_destroy(zbk_slab);
	return ret;

}

static void __exit zbk_exit(void)
{
	zio_unregister_buf(&zbk_buffer);
	kmem_cache_destroy(zbk_slab);
}

module_init(zbk_init);
module_exit(zbk_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");
