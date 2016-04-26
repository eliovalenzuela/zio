/*
 * Copyright 2012 Federico Vaga <federico.vaga@gmail.com>
 *
 * lszio is a ZIO tool which show details about connected ZIO device.
 * It is a clone of lspci, but for ZIO devices.
 */
#include <stdlib.h>
#include <getopt.h>
#include <glob.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>

#include <libzio.h>

const char *lszio_version = "0.5";

/* print program help */
static void zds_help()
{
	printf("\nlszio version: %s\n", lszio_version);
	printf("\nlszio [options]\n\n");
	printf("The program list connected ZIO devices\n\n");
	printf("Options:\n");
	printf("-t: show available triggers\n");
	printf("-b: show available buffers\n");
	printf("-v: verbose output, show device attributes status\n");
	printf("-d <device name>: look for a particular device\n");
	printf("-w: show ZIO hardware device\n");
	printf("-h: show this help\n\n");
}


static void lszio_show_module(const char *type, struct uzio_module_list *list)
{
	int i;

	if (!list) {
		fprintf(stderr, "Cannot retrieve %s list: %s\n",
			type, uzio_strerror(errno));
		return;
	}
	fprintf(stdout, "Available %s:\n", type);
	for (i = 0; i < list->len; ++i) {
		fprintf(stdout, "  %s\n", list->names[i]);
	}
	fprintf(stdout, "\n");
}


static inline void lszio_show_buffer()
{
	struct uzio_module_list *list;

	list = uzio_buffer_list();
	lszio_show_module("buffers", list);
	uzio_module_list_free(list);
}


static inline void lszio_show_trigger()
{
	struct uzio_module_list *list;

	list = uzio_trigger_list();
	lszio_show_module("triggers", list);
	uzio_module_list_free(list);
}

static void zds_print_attributes(struct uzio_object *zobj)
{
	uint32_t val;
	int i;

	fprintf(stdout, "    Standard Attributes:\n");
	for (i = 0; strlen(zobj->std[i].path) && i < ZIO_MAX_STD_ATTR; i++) {
		uzio_attr_value_get(&zobj->std[i], &val);
		fprintf(stdout, "      %s : %d\n",
			basename(zobj->std[i].path), val);
	}

	fprintf(stdout, "    Extended Attributes:\n");
	for (i = 0; strlen(zobj->ext[i].path) && i < ZIO_MAX_EXT_ATTR; i++) {
		uzio_attr_value_get(&zobj->ext[i], &val);
		fprintf(stdout, "      %s : %d\n",
			basename(zobj->ext[i].path), val);
	}
}

#define STR_LEN (32)
static void zds_print_cset_overview(struct uzio_cset *cset)
{
	char tmp[STR_LEN];
	int err;

	printf("  %s\n", cset->head.sysbase);
	printf("    name : %s\n", cset->head.name);
	printf("    devname : %s\n", cset->head.devname);
	uzio_enum_to_str_type(tmp, cset->head.type, STR_LEN);
	printf("    type : %s\n", tmp);

	printf("    channels : %d\n", cset->n_chan);
	printf("    flags : 0x%lx\n", cset->flags);

	err = uzio_attr_string_get(&cset->direction, tmp, STR_LEN);
	if (err)
		printf("    direction : %s\n", tmp);
	else
		printf("    direction : N/A (err: %s)\n",
		       uzio_strerror(errno));

	err = uzio_attr_string_get(&cset->current_trigger, tmp, STR_LEN);
	if (err)
		printf("    trigger : %s\n", tmp);
	else
		printf("    trigger : N/A (err: %s)\n",
		       uzio_strerror(errno));

	err = uzio_attr_string_get(&cset->current_buffer, tmp, STR_LEN);
	if (err)
		printf("    buffer : %s\n", tmp);
	else
		printf("    buffer : N/A (err: %s)\n",
		       uzio_strerror(errno));
}


/*
 * zds_print_v
 * It prints the first level of verbosity
 */
static void zds_print_v(struct uzio_device *zdev)
{
	int i;

	printf("  %s\n", zdev->head.sysbase);
	for (i = 0; i < zdev->n_cset; ++i) {
		zds_print_cset_overview(&zdev->cset[i]);
	}
}

static void zds_print_vv(struct uzio_device *zdev)
{
	int i;

	printf("  %s\n", zdev->head.sysbase);
	for (i = 0; i < zdev->n_cset; ++i) {
		zds_print_cset_overview(&zdev->cset[i]);
		zds_print_attributes(&zdev->cset[i].head);
	}
}


static void zds_print_verbose(char *name, unsigned int level)
{
	struct uzio_device *uzdev;

	uzdev = uzio_device_open_by_name(name);
	if (!uzdev) {
	  printf("Error : %s\n", uzio_strerror(errno));
		exit(1);
	}

	switch(level) {
	case 1:
		zds_print_v(uzdev);
		break;
	case 2:
		zds_print_vv(uzdev);
		break;
	}

	uzio_device_close(uzdev);
}

int main(int argc, char *argv[])
{
	struct uzio_module_list *dev_list;
	int c, v = 0, i;
	int show_trg = 0, show_buf = 0;
	/* char *device = NULL; */

	while ((c = getopt (argc, argv, "tbd:vwh")) != -1) {
		switch (c) {
		case 't':
			show_trg = 1;
			break;
		case 'b':
			show_buf = 1;
			break;
		/* case 'd': */
		/* 	device = optarg; */
		/* 	break; */
		case 'v':
			v++;
			break;
		case 'h':
			zds_help();
			exit(1);
			break;
		}
	}

	if (show_trg)
		lszio_show_trigger();
	if (show_buf)
		lszio_show_buffer();


	dev_list = uzio_device_list();
	if (!dev_list) {
		fprintf(stderr, "Cannot retrive the list of devices: %s\n",
			uzio_strerror(errno));
	}

	fprintf(stdout, "Available devices:\n");
	if (v) {
		 for (i = 0; i < dev_list->len; ++i)
			 zds_print_verbose(dev_list->names[i], v);
	} else {
		for (i = 0; i < dev_list->len; ++i)
			fprintf(stdout, "  %s\n", dev_list->names[i]);
	}

	uzio_module_list_free(dev_list);

	exit(0);
}
