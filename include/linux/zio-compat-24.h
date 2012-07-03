#ifndef __ZIO_COMPAT24_H__
#define __ZIO_COMPAT24_H__

/* Fixes for 2.6.24 */
#include <linux/version.h>

/* fmode_t appeared in v2.6.27-6510-gaeb5d72 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
typedef unsigned fmode_t;
#endif

/* WARN appeared in v2.6.26-6936-ga8f18b9 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#define WARN(condition, format...) ({pr_warning(format);})
#endif

/* dev_name() appeared in v2.6.26-rc1*/
/* dev_set_name() appeared in v2.6.26-rc6*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
static inline const char *dev_name(struct device *dev)
{
	return kobject_name(&dev->kobj);
}

#define dev_set_name(dev, fmt, ...)  ({	\
	int err = 0; \
	err = kobject_set_name(&(dev)->kobj, fmt, ## __VA_ARGS__); \
	if (!err) \
		strncpy((dev)->bus_id, (dev)->kobj.k_name, BUS_ID_SIZE); \
	err; })
#endif

/* strict_strtol() appared in v2.6. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
#define strict_strtol(_buf, _base, _val) ({		\
	int err = 0;					\
	*(_val) = simple_strtol(_buf, NULL, _base);	\
	err;						\
})
#endif

#endif
