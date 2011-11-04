
/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_H__
#define __ZIO_H__

/* ZIO_VERSION: is a zio_class attribute to identify the framework version*/
#define ZIO_MAJOR_VERSION 0
#define ZIO_MINOR_VERSION 2

#define ZIO_NAME_LEN 32 /* full name */

#ifdef __KERNEL__ /* Nothing more is for user space */

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/string.h>

/* These two maxima are kept low by now to test overflow situations */
#define ZIO_CSET_MAXNUM 16
#define ZIO_CHAN_MAXNUM 16

#define ZIO_NMAX_CSET_MINORS (ZIO_CHAN_MAXNUM * 2)
#define ZIO_NAME_OBJ 12 /* name for registered object */

/* Name the data structures */
struct zio_device; /* both type (a.k.a. driver) and instance (a.k.a. device) */
struct zio_channel; struct zio_cset;
struct zio_buffer_type; struct zio_bi; struct zio_block;
struct zio_trigger_type; struct zio_ti;

struct zio_device_operations;
struct zio_buffer_operations;
struct zio_trigger_operations;

/*
 * We use the same functions to deal with attributes, but the structures
 * we act on may be different (dev, cset, channel). Thus, all structures
 * begin with the type identifier, and zio_obj_head is used in container_of
 */
enum zio_object_type {
	ZNONE = 0,	/* reserved for non zio object */
	ZDEV, ZCSET, ZCHAN,
	ZTRIG, ZTI,	/* trigger and trigger instance */
	ZBUF, ZBI,	/* buffer and buffer instance */
};

/* zio_obj_head is for internal use only, as explained above */
struct zio_obj_head {
	struct kobject		kobj;
	enum zio_object_type	zobj_type;
	char			name[ZIO_NAME_LEN];
};
#define to_zio_head(_kobj) container_of(_kobj, struct zio_obj_head, kobj)
#define to_zio_dev(_kobj) container_of(_kobj, struct zio_device, head.kobj)
#define to_zio_cset(_kobj) container_of(_kobj, struct zio_cset, head.kobj)
#define to_zio_chan(_kobj) container_of(_kobj, struct zio_channel, head.kobj)

static inline enum zio_object_type __zio_get_object_type(struct kobject *kobj)
{
	return to_zio_head(kobj)->zobj_type;
}

enum zattr_standard {
	ZATTR_NBIT,	/* number of bits per sample */
	ZATTR_GAIN,	/* gain for signal, integer in 0.001 steps */
	ZATTR_OFFSET,	/* microvolts */
	ZATTR_MAXRATE,	/* hertz */
	ZATTR_VREFTYPE,	/* source of Vref (0 = default) */
	ZATTR_STD_ATTR_NUM,		/* used to size arrays */
};

extern const char zio_attr_names[ZATTR_STD_ATTR_NUM][ZIO_NAME_LEN];

/*
 * zio_attribute: the attribute to access device parameters.
 *
 * Many devices store configuration in hardware registers, and thus
 * many configuration steps can be reduced to a read/write from/to a
 * particular device register, located at a specific address.
 *
 * A zio_attribute provides a generic way to access those registers
 *
 * @attribute: standard attribute structure used to create a sysfs access
 * @priv.reg: register address to use as is
 * @priv.reg_descriptor: a generic pointer used to specify how access to a
 *	particular register on device. This is defined by driver developer
 * @value: is the value stored on device
 * @show: is equivalent to info_get from zio_operations
 * @store: is equivalent to  conf_set from zio_operations
 */
struct zio_attribute {
	struct attribute			attr;
	union { /* priv is sometimes a pointer and sometimes an hw addr */
		void				*ptr;
		unsigned long			addr;
	} priv;
	uint32_t				value;
	const struct zio_device_operations	*d_op;
	const struct zio_sys_operations		*s_op;
};

/* attribute -> zio_attribute */
#define to_zio_zattr(aptr) container_of(aptr, struct zio_attribute, attr)

/*
 * Every object has both std attributes (whole length is known)
 * and extended attributes (as we need to be told how many).
 * Then, the sysfs attribute_groups are what we build to actually register
 */
struct zio_attribute_set {
	struct zio_attribute	*std_zattr;
	struct zio_attribute	*ext_zattr;
	unsigned int		n_ext_attr;
	struct attribute_group	std_attr;
	struct attribute_group	ext_attr;
};

/* FIXME: check this DECLARE stuff */
#define ZATTRS_DECLARE(name) struct zio_attribute name[ZATTR_STD_ATTR_NUM]

/*
 * @ZATTR_REG: define a zio attribute with address register
 * @ZATTR_PRV: define a zio attribute with private register
 * @ZATTR_EXT_REG: define a zio extended attribute with address register
 * @ZATTR_EXT_PRV: define a zio extended attribute with private register
 */
#define ZATTR_REG(_type, _mode, _add, _val) [_type] = {			\
		.attr = {.name = zio_attr_names[_type], .mode = _mode},	\
		.priv.addr = _add,					\
		.value = _val,						\
}
#define ZATTR_PRV(_type, _mode, _add, _val) [_type] = {			\
		.attr = {.name = zio_attr_names[_type], .mode = _mode},	\
		.priv.ptr = _add,					\
		.value = _val,						\
}
#define ZATTR_EXT_REG(_name, _mode, _add, _val) {			\
		.attr = {.name = _name, .mode = _mode},			\
		.priv.addr = _add,					\
		.value = _val,						\
}
#define ZATTR_EXT_PRV(_name, _mode, _add, _val) {			\
		.attr = {.name = _name, .mode = _mode},			\
		.priv.ptr = _add,					\
		.value = _val,						\
}

/* Bits 0..3 are reserved for use in all objects. By now only bit 1 is used */
enum zobj_flags {
	ZIO_DISABLED		= 0x1,	/* 0 (default) is enabled */
};

/*
 * zio_device -- the top-level hardware description
 */
struct zio_device {
	struct zio_obj_head			head;
	struct module				*owner;
	spinlock_t				lock; /* for all attr ops */
	unsigned long				flags;
	struct zio_attribute_set		zattr_set;
	const struct zio_sys_operations		*s_op;
	const struct zio_device_operations	*d_op;

	/* The full device is an array of csets */
	struct zio_cset			*cset;
	unsigned int			n_cset;
};
struct zio_sys_operations {
	int (*info_get)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t *usr_val);
	int (*conf_set)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t  usr_val);
};
struct zio_device_operations {
	int (*info_get)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t *usr_val);
	int (*conf_set)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t  usr_val);
	int (*input_block)(struct zio_channel *chan, struct zio_block *block);
	int (*output_block)(struct zio_channel *chan, struct zio_block *block);
};

int __must_check zio_register_dev(struct zio_device *zdev, const char *name);
void zio_unregister_dev(struct zio_device *zio_dev);

/*
 * zio_cset -- channel set: a group of channels with the same features
 */
struct zio_cset {
	struct zio_obj_head	head;
	struct zio_device	*zdev;		/* parent zio device */
	struct zio_buffer_type	*zbuf;		/* buffer type for bi */
	struct zio_trigger_type *trig;		/* trigger tyèe for ti*/
	struct zio_ti		*ti;		/* trigger instance */
	unsigned		ssize;		/* sample size (bytes) */
	unsigned		index;		/* index within parent */
	unsigned long		flags;
	struct zio_attribute_set zattr_set;
	struct zio_attribute_set zattr_set_chan; /* model for channel attrs */

	/* The cset is an array of channels */
	struct zio_channel	*chan;
	unsigned int		n_chan;

	int (*init)(struct zio_cset *zcset);
	void (*exit)(struct zio_cset *zcset);

	struct list_head	list_cset;	/* for cset global list */
	dev_t			basedev;	/* base for the minors */

	struct zio_attribute	*cset_attrs; /* FIXME: set buf, set trig */
};

/* first 4bit are reserved for zio object universal flags */
enum zcset_flags {
	ZCSET_DIR		= 0x10,	/* 0 is input  - 1 is output*/
	ZCSET_DIR_INPUT		= 0x00,
	ZCSET_DIR_OUTPUT	= 0x10,
	ZCSET_TYPE		= 0x20,	/* 0 is digital - 1 is analog*/
	ZCSET_TYPE_DIGITAL	= 0x00,
	ZCSET_TYPE_ANALOG	= 0x20,
	ZCSET_CHAN_ALLOC	= 0x40, /* 1 if channels are allocated by zio*/
	ZCSET_CHAN_ALLOC_ON	= 0x40,
	ZCSET_CHAN_ALLOC_OFF	= 0x00,
};

/*
 * zio_channel -- an individual channel within the cset
 */

struct zio_channel {
	struct zio_obj_head	head;
	struct zio_cset		*cset;		/* parent cset */
	struct zio_ti		*ti;		/* cset trigger instance */
	struct zio_bi		*bi;		/* buffer instance */
	unsigned int		index;		/* index within parent */
	unsigned long		flags;
	struct zio_attribute_set zattr_set;

	struct device		*ctrl_dev;	/* control char device */
	struct device		*data_dev;	/* data char device */

	struct zio_block	*current_block;	/* the one being transferred */
	void			*t_priv;	/* used by trigger */
};

/* first 4bit are reserved for zio object universal flags */
enum zchan_flag_mask {
	ZCHAN_POLAR		= 0x10,	/* 0 is positive - 1 is negative*/
	ZCHAN_POLAR_POSITIVE	= 0x00,
	ZCHAN_POLAR_NEGATIVE	= 0x10,
};

/* get each channel from cset */
static inline struct zio_channel *__first_enabled_chan(struct zio_cset *cset,
						struct zio_channel *chan)
{
	if (unlikely(chan - cset->chan >= cset->n_chan))
		return NULL;
	while (1) {
		if (!(chan->flags & ZIO_DISABLED))
			return chan; /* if is enabled, use this */
		if (chan->index+1 == cset->n_chan)
			return NULL; /* no more channels */
		chan++;
	}
}
#define cset_for_each(cset, cptr)				\
		for (cptr = cset->chan;				\
		     (cptr = __first_enabled_chan(cset, cptr));	\
		     cptr++)

#ifdef __ZIO_INTERNAL__

/* This list is used in the core to keep track of registered objects */
struct zio_object_list {
	struct kobject		*kobj;	/* for sysfs folder, no attrs */
	enum zio_object_type	zobj_type;
	struct list_head	list;
};
struct zio_object_list_item {
	struct list_head	list;
	struct module		*owner;
	struct zio_obj_head	*obj_head;
};

/* Global framework status (i.e., globals in zio-core) */
struct zio_status {
	/* a pointer to set up standard ktype with create */
	struct kobject		*kobj;
	/*
	 * The bmask represent the minors region for zio; each bit is
	 * a block of minors available for a single cset. When a new cset
	 * is declared, zio look for the first available block of minors:
	 * set 1 to the correspondent bit on bitmask to set the block
	 * as busy
	 */
	DECLARE_BITMAP(cset_minors_mask, ZIO_CSET_MAXNUM);
	struct cdev		chrdev;
	dev_t			basedev;
	spinlock_t		lock;

	/* List of cset, used to retrieve a cset from a minor base*/
	struct list_head	list_cset;

	/* The three lists of registered devices, with owner module */
	struct zio_object_list	all_devices;
	struct zio_object_list	all_trigger_types;
	struct zio_object_list	all_buffer_types;
};

extern struct zio_status zstat;
int __zio_minorbase_get(struct zio_cset *zcset);
void __zio_minorbase_put(struct zio_cset *zcset);

int __zio_register_cdev(void);
void __zio_unregister_cdev(void);

int chan_create_device(struct zio_channel *zchan);
void chan_destroy_device(struct zio_channel *zchan);
#endif /* INTERNAL */

#endif /* __KERNEL__ */
#endif /* __ZIO_H__ */