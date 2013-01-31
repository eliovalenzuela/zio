/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef ZIO_SYSFS_H_
#define ZIO_SYSFS_H_

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

/* ZIO default permissions */
#define ZIO_RO_PERM (S_IRUGO)
#define ZIO_WO_PERM (S_IWUSR | S_IWGRP)
#define ZIO_RW_PERM (ZIO_RO_PERM | ZIO_WO_PERM)

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
 * @flags: to set attribute capabilities
 * @index [INTERNAL]: index within a group of attribute (standard or extended)
 * @id: something unique to identify the attribute. It can be the register
 *      address which this attribute refers to. It can be an index of an array
 *      which contain special information to gain access to a register.
 * @value: is the value stored on device
 * @show: is equivalent to info_get from zio_operations
 * @store: is equivalent to  conf_set from zio_operations
 */
struct zio_attribute {
	struct device_attribute			attr;
	uint32_t				flags;
	int					index;
	unsigned long				id;
	uint32_t				value;
	const struct zio_sysfs_operations	*s_op;
};
#define ZIO_ATTR_INDEX_NONE -1
enum zattr_flags {
	ZIO_ATTR_TYPE		= 0x10,
	ZIO_ATTR_TYPE_STD	= 0x00,
	ZIO_ATTR_TYPE_EXT	= 0x10,
	ZIO_ATTR_CONTROL	= 0x20,
};

struct zio_sysfs_operations {
	int (*info_get)(struct device *dev, struct zio_attribute *zattr,
			uint32_t *usr_val);
	int (*conf_set)(struct device *dev, struct zio_attribute *zattr,
			uint32_t  usr_val);
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
	unsigned int		n_std_attr;
	struct zio_attribute	*ext_zattr;
	unsigned int		n_ext_attr;
};

#define ZIO_ATTR_VERSION (ZIO_MAX_STD_ATTR - 1)
enum zio_dev_std_attr {
	ZIO_ATTR_NBITS,			/* number of bits per sample */
	ZIO_ATTR_GAIN,			/* gain for signal, 0.001 steps */
	ZIO_ATTR_OFFSET,		/* microvolts */
	ZIO_ATTR_MAXRATE,		/* hertz */
	ZIO_ATTR_VREFTYPE,		/* source of Vref (0 = default) */

	/* Specials attributes */
	ZIO_ATTR_DEV_VERSION = ZIO_ATTR_VERSION,	/* attribute set version */
	_ZIO_DEV_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_trg_std_attr {
	ZIO_ATTR_TRIG_N_SHOTS = 0,	/* trigger programmed shots (0: infinite) */
	ZIO_ATTR_TRIG_POST_SAMP,	/* samples after trigger fire */
	ZIO_ATTR_TRIG_PRE_SAMP,		/* samples before trigger fire */

	/* Specials attributes */
	ZIO_ATTR_TRIG_VERSION = ZIO_ATTR_VERSION,	/* attribute set version */
	_ZIO_TRG_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_buf_std_attr {
	ZIO_ATTR_ZBUF_MAXLEN = 0,/* max number of element in buffer */
	ZIO_ATTR_ZBUF_MAXKB,	/* max number of kB in buffer */
	ZIO_ATTR_ZBUF_ALLOC_LEN,/* number of allocated elements in buffer */
	ZIO_ATTR_ZBUF_ALLOC_KB,	/* number of allocated  kB in buffer */

	/* Specials attributes */
	ZIO_ATTR_ZBUF_VERSION = ZIO_ATTR_VERSION,	/* attribute set version */
	_ZIO_BUF_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_chn_bin_attr {
	ZIO_BIN_CTRL = 0,		/* current control */
	ZIO_BIN_ADDR,			/* zio_address */
	__ZIO_BIN_ATTR_NUM,
};

extern const char zio_zdev_attr_names[_ZIO_DEV_ATTR_STD_NUM][ZIO_NAME_LEN];
extern const char zio_trig_attr_names[_ZIO_TRG_ATTR_STD_NUM][ZIO_NAME_LEN];
extern const char zio_zbuf_attr_names[_ZIO_BUF_ATTR_STD_NUM][ZIO_NAME_LEN];

#define ZIO_ATTR_DEFINE_STD(_type, _name) struct zio_attribute \
	_name[_##_type##_ATTR_STD_NUM]

/*
 * @ZIO_ATTR: define a zio attribute
 * @ZIO_ATTR_EXT: define a zio extended attribute
 * @ZIO_PARAM_EXT: define a zio attribute parameter (not included in ctrl)
 */
#define ZIO_ATTR(zobj, _type, _mode, _add, _val)[_type] = {		\
		.attr = {						\
			.attr = {					\
				.name = zio_##zobj##_attr_names[_type],	\
				.mode = _mode				\
			},						\
		},							\
		.id = _add,						\
		.value = _val,						\
		.flags = ZIO_ATTR_CONTROL,				\
}
#define ZIO_ATTR_EXT(_name, _mode, _add, _val) {			\
		.attr = { .attr = {.name = _name, .mode = _mode},},	\
		.id = _add,						\
		.value = _val,						\
		.flags = ZIO_ATTR_CONTROL,				\
}
#define ZIO_PARAM_EXT(_name, _mode, _add, _val) {			\
		.attr = { .attr = {.name = _name, .mode = _mode},},	\
		.id = _add,						\
		.value = _val,						\
		.flags = 0,						\
}

/*
 * This macro defines the standard attribute 'version' for an attribute_set of
 * a zio object. In the macro there is an explicit reference to the device
 * (zdev) but it does not care because the show function and the sysfs name it
 * is the same for every ZIO object.
 *
 * The ZIO core handles directly this attribute without useing the driver,
 * trigger or buffer sysfs operations
 */
#define ZIO_SET_ATTR_VERSION(_hexversion) \
	ZIO_ATTR(zdev, ZIO_ATTR_VERSION, ZIO_RO_PERM, 0, _hexversion)

/*
 * The attribute set version is in the format (a.b c). Fields 'a' and 'b' are
 * the major and minor version number. The field 'c' is free choice of the
 * driver developer
 */
#define ZIO_HEX_VERSION(_a, _b, _c) (((_a & 0xFF) << 24) + \
		((_b & 0xFF) << 16) + (_c & 0xFFFF))

#endif /* ZIO_SYSFS_H_ */
