#ifndef __LIBZIO_H__
#define __LIBZIO_H__

#include <stdint.h>
#include <stdio.h>
#include <linux/zio-user.h>

#define UZIO_SYS_DIR "/sys/bus/zio"
#define UZIO_SYS_DIR_DEV UZIO_SYS_DIR"/devices"

#define UZIO_MAX_PATH_LEN (256)

#define UZIO_CSET_FLAG_DIRECTION (1<<0)
#define UZIO_CSET_FLAG_DIRECTION_OUT UZIO_CSET_FLAG_DIRECTION
#define UZIO_CSET_FLAG_DIRECTION_IN  (0)

/**
 * ZIO errors
 */
enum uzio_errno {
	EUZIONODEV = 19860705,
	EUZIOVERSION,
	EUZIONOMODLIST,
	EUZIOBLKCTRLWRONG,
	EUZIONOCDEV,
	EUZIOIDEV, /**< Invalid device */
	EUZIOICSET, /**< Invalid device */
	EUZIOICHAN, /**< Invalid device */
	EUZIOBLKDIRECTION, /**< Wrong direction */
	EUZIOIDATA, /**< Invalid data buffer */
	__EUZIO_LAST_ERROR,
};


/**
 * Data structure used to list available zio modules (devices, buffer and
 * triggers)
 */
struct uzio_module_list {
	unsigned int len; /**< number of modules */
	char **names; /**< list of modules' name */
};

struct uzio_object;

struct uzio_attribute {
	struct uzio_object *parent;
	char path[UZIO_MAX_PATH_LEN];
};

struct uzio_object {
	struct uzio_object *parent;
	char sysbase[UZIO_MAX_PATH_LEN];
	char name[ZIO_OBJ_NAME_FULL_LEN];
	char devname[ZIO_OBJ_NAME_FULL_LEN];
	enum zio_object_type type;

	struct uzio_attribute enable;
	struct uzio_attribute __name;
	struct uzio_attribute __devname;
	struct uzio_attribute __type;
	struct uzio_attribute std[ZIO_MAX_STD_ATTR];
	struct uzio_attribute ext[ZIO_MAX_EXT_ATTR];
};

struct uzio_buffer {
	struct uzio_object head;

	struct uzio_attribute flush;
};

struct uzio_trigger {
	struct uzio_object head;
};

struct uzio_channel {
	struct uzio_object head;
	int fd_data;
	int fd_ctrl;
	struct uzio_attribute current_ctrl;
	struct uzio_attribute alarms;

	struct uzio_buffer buffer;
};

struct uzio_cset {
	struct uzio_object head;
	struct uzio_attribute direction;
	struct uzio_attribute current_buffer;
	struct uzio_attribute current_trigger;

	unsigned long flags;

	struct uzio_trigger trigger;

	struct uzio_channel *chan;
	unsigned int n_chan;
};

struct uzio_device {
	struct uzio_object head;

	struct uzio_cset *cset;
	unsigned int n_cset;
};

struct uzio_block {
	struct zio_control ctrl;
	void *data;
	size_t datalen;
};

/**
 * @defgroup util Utilities
 * Collection of utilities
 * @{
 */
extern char *uzio_strerror(unsigned int err);
extern void uzio_str_to_enum_type(enum zio_object_type *type, char *str,
				  unsigned int n);
extern void uzio_enum_to_str_type(char *str, enum zio_object_type type,
				  unsigned int n);
extern struct uzio_module_list *uzio_module_list(const struct uzio_attribute *a);
extern void uzio_module_list_free(struct uzio_module_list *list);
extern void zio_control_print_to_file_basic(FILE *stream,
					    struct zio_control *ctrl);
extern void zio_control_print_to_file_attr(FILE *stream,
					   enum zio_control_attr_type type,
					   struct zio_control *ctrl);
/**@}*/

/**
 * @defgroup attr Attribute
 * Functions to handle ZIO attributes
 * @{
 */
extern int uzio_attr_value_get(struct uzio_attribute *attr, uint32_t *val);
extern int uzio_attr_value_set(struct uzio_attribute *attr, uint32_t val);
extern int uzio_attr_string_get(struct uzio_attribute *attr,
				char *str, unsigned int n);
extern int uzio_attr_string_set(struct uzio_attribute *attr,
				char *str, unsigned int n);
/**@}*/


/**
 * @defgroup obj ZIO Objects
 * Functions to handle generic ZIO instances
 * @{
 */
extern int uzio_object_enable(struct uzio_object *zobj, unsigned int enable);
/**@}*/

/**
 * @defgroup dev Device
 * Functions to handle device instances
 * @{
 */
extern struct uzio_module_list *uzio_device_list(void);
extern struct uzio_device *uzio_device_open_by_name(char *name);
extern struct uzio_device *uzio_device_open(char *name, uint32_t dev_id);
extern void uzio_device_close(struct uzio_device *dev);
extern int uzio_device_enable(struct uzio_device *dev, unsigned int enable);
/**@}*/

/**
 * @defgroup cset Channel Set
 * Functions to handle channel set instances
 * @{
 */
extern int uzio_cset_enable(struct uzio_cset *cset, unsigned int enable);
/**@}*/

/**
 * @defgroup chan Channel
 * Functions to handle channel instances
 * @{
 */
extern int uzio_channel_enable(struct uzio_channel *chan, unsigned int enable);
/**@}*/

/**
 * @defgroup trg Trigger
 * Functions to handle trigger instances
 * @{
 */
extern struct uzio_module_list *uzio_trigger_list(void);
extern int uzio_trigger_enable(struct uzio_trigger *trig, unsigned int enable);
extern int uzio_trigger_change(struct uzio_cset *cset, char *name,
			       unsigned int n);
/**@}*/

/**
 * @defgroup buf Buffer
 * Functions to handle buffer instances
 * @{
 */
extern struct uzio_module_list *uzio_buffer_list(void);
extern int uzio_buffer_enable(struct uzio_buffer *buf, unsigned int enable);
extern int uzio_buffer_flush(struct uzio_channel *chan);
extern int uzio_buffer_flush_cset(struct uzio_cset *cset);
extern int uzio_buffer_change(struct uzio_cset *cset, char *name,
			      unsigned int n);
/**@}*/


/**
 * @defgroup ctrl MetaData
 * Functions to handle block's metadata
 * @{
 */
extern int uzio_ctrl_get(struct uzio_channel *chan, struct zio_control *ctrl);
extern int uzio_ctrl_set(struct uzio_channel *chan, struct zio_control *ctrl);
/**@}*/


/**
 * @defgroup block Block management
 * Functions to handle block's metadata
 * @{
 */
extern int uzio_block_ctrl_write_raw(struct uzio_channel *uchan,
				     struct zio_control *ctrl);
extern int uzio_block_ctrl_read_raw(struct uzio_channel *uchan,
				    struct zio_control *ctrl);
extern int uzio_block_data_write_raw(struct uzio_channel *uchan, void *data,
				     size_t datalen);
extern int uzio_block_data_read_raw(struct uzio_channel *uchan, void *data,
				    size_t datalen);
extern struct uzio_block *uzio_block_read(struct uzio_channel *uchan);
extern int uzio_block_write(struct uzio_channel *chan,
			    struct uzio_block *block);
extern struct uzio_block *uzio_block_alloc(size_t datalen);
extern void uzio_block_free(struct uzio_block *block);
/**@}*/

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE, MEMBER)  __compiler_offsetof(TYPE, MEMBER)
#else
#define offsetof(TYPE, MEMBER)  ((size_t)&((TYPE *)0)->MEMBER)
#endif

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif
