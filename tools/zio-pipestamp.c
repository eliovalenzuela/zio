/*
 * Trivial utility that reports the ZIO overhead
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

/* zio-sysfs.h cannot be included by user space, at this point: copy it */

enum zio_dev_std_attr {
	ZIO_ATTR_NBITS,			/* number of bits per sample */
	ZIO_ATTR_GAIN,			/* gain for signal, 0.001 steps */
	ZIO_ATTR_OFFSET,		/* microvolts */
	ZIO_ATTR_MAXRATE,		/* hertz */
	ZIO_ATTR_VREFTYPE,		/* source of Vref (0 = default) */
	ZIO_ATTR_ALLOC_TIME,		/* if CONFIG_ZIO_PIPESTAMP */
	ZIO_ATTR_STORE_TIME,		/* if CONFIG_ZIO_PIPESTAMP */
	ZIO_ATTR_RETR_TIME,		/* if CONFIG_ZIO_PIPESTAMP */
	ZIO_ATTR_FREE_TIME,		/* if CONFIG_ZIO_PIPESTAMP */
	_ZIO_DEV_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_trg_std_attr {
	ZIO_ATTR_TRIG_REENABLE = 0,	/* re-arm trigger */
	ZIO_ATTR_TRIG_POST_SAMP,	/* samples after trigger fire */
	ZIO_ATTR_TRIG_PRE_SAMP,		/* samples before trigger fire */
	ZIO_ATTR_TRIG_ARM_TIME,		/* if CONFIG_ZIO_PIPESTAMP */
	ZIO_ATTR_TRIG_DONE_TIME,	/* if CONFIG_ZIO_PIPESTAMP */
	_ZIO_TRG_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_buf_std_attr {
	ZIO_ATTR_ZBUF_MAXLEN = 0,	/* max number of element in buffer */
	ZIO_ATTR_ZBUF_MAXKB,		/* max number of kB in buffer */
	_ZIO_BUF_ATTR_STD_NUM,		/* used to size arrays */
};
enum zio_chn_bin_attr {
	ZIO_BIN_CTRL = 0,		/* current control */
	ZIO_BIN_ADDR,			/* zio_address */
	__ZIO_BIN_ATTR_NUM,
};

/* end of zio-sysfs.h copy */


#define  MODULE (4LL * 1000 * 1000 * 1000)

/* A time is earlier (i.e., after our 4s modulus) if earlier than 100ms */
int earlier(uint32_t a, uint32_t b)
{
	if (a > b)
		return 0;
	if (b - a < 100 * 1000 * 1000)
		return 0;
	return 1;
}

void report_one(struct zio_control *ctrl)
{
	long long t0, t1, t2, t3;

	t0 = ctrl->attr_channel.std_val[ZIO_ATTR_ALLOC_TIME];
	t1 = ctrl->attr_channel.std_val[ZIO_ATTR_STORE_TIME];
	t2 = ctrl->attr_channel.std_val[ZIO_ATTR_RETR_TIME];
	t3 = ctrl->attr_channel.std_val[ZIO_ATTR_FREE_TIME];

	if (earlier(t1, t0)) t1 = t1 + MODULE - t0; else t1 = t1 - t0;
	if (earlier(t2, t0)) t2 = t2 + MODULE - t0; else t2 = t2 - t0;
	if (earlier(t3, t0)) t3 = t3 + MODULE - t0; else t3 = t3 - t0;

	printf("store %9lli       retr %9lli      free   %9lli\n",
	       t1, t2, t3);

	t1 = ctrl->attr_trigger.std_val[ZIO_ATTR_TRIG_ARM_TIME];
	t2 = ctrl->attr_trigger.std_val[ZIO_ATTR_TRIG_DONE_TIME];

	if (earlier(t1, t0)) t1 = t1 + MODULE - t0; else t1 = t1 - t0;
	if (earlier(t2, t0)) t2 = t2 + MODULE - t0; else t2 = t2 - t0;

	printf("  arm %9lli       done %9lli      (delta %9lli)\n\n",
	       t1, t2, t2 - t1);
}

int main(int argc, char **argv)
{
	int fd;
	char *fname = "/dev/zio-sniff.ctrl";

	struct zio_control ctrl; /* FIXME: no TLV support */

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], fname,
			strerror(errno));
		exit(1);
	}

	while (read(fd, &ctrl, sizeof(ctrl)) == sizeof(ctrl))
		report_one(&ctrl);
	return 0;
}
