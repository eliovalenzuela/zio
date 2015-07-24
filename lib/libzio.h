#ifndef __LIBZIO_H__
#define __LIBZIO_H__

#include <linux/zio-user.h>

struct zio_object {
	char sysbase[128];
};

struct zio_attribute {
	struct zio_object *parent;
	char path[128];
}

extern void zio_control_print_to_file_basic(FILE *stream,
					    struct zio_control *ctrl);
extern void zio_control_print_to_file_attr(FILE *stream,
					   enum zio_control_attr_type type,
					   struct zio_control *ctrl);
#endif
