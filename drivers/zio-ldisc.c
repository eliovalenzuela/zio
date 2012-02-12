/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/bitops.h>
#include <asm/unaligned.h>

#include <linux/zio.h>
#include <linux/zio-trigger.h>

ZIO_PARAM_BUFFER(zld_buffer);
ZIO_PARAM_TRIGGER(zld_trigger);

#define ZLD_NCHAN 8
#define ZLD_SSIZE 2

/*
 * We have no concept of driver instance yet, until we have the bus
 * abstraction implemented. So this is just one instance, with global data
 */
static struct zio_cset zld_cset[]; /* Declared later, needed in ldisc */

/*
 * In prospective, this is the complete protocol:
 * Bytes 0, 1:  start of packet: 0xff 0xff
 * Byte 2: protocol and ssize:   0x02 (4-bit protocol == 0, sample size is 2)
 * Byte 3: number of samples:    0x08 (we have 8 channels by now)
 * Data follows, little-endian   (16 == 0x10 bytes in this case)
 *
 * Currently, only 02,08 is supported so the packet is 0x14 bytes
 */
#define ZLD_PACKET 0x14

/* 
 * This driver is made by two parts: one line discipline that collects
 * data from the serial port and a ZIO driver part.
 *
 * Let's start with the line discipline.
 */
#define ZLD_NUMBER (NR_LDISCS-1) /* last available, bah... */

/* Registering and unregistering the zio device happens at these times */
static int zld_open(struct tty_struct *tty);
static void zld_close(struct tty_struct *tty);

/* This looks for one packet-worth of data in a buffer, returns handled bytes */
static int __zld_parse(struct zio_cset *cset, unsigned char *b, int blen)
{
	static unsigned char header[] = {0xff, 0xff, 0x02, 0x08};
	struct zio_device *zdev = cset->zdev;
	struct zio_channel *chan;
	unsigned long flags;
	unsigned char *hptr;
	uint16_t datum;
	int done = 0;
	int i;

	if (blen < ZLD_PACKET)
		return 0;

	if (unlikely(memcmp(b, header, sizeof(header)))) {
		/* datasize is there, but no header is at the head */
		hptr = memchr(b + 1, 0xff, blen);
		if (hptr) {
			return hptr - b;
		}
		return blen; /* no header, eat it all */
	}
	/* So, the header is there and we have enough data: eat it */
	for (i = 0; i < ZLD_NCHAN; i++) {
		struct zio_block *block;

		datum = get_unaligned_le16(b + 4 + i * 2);
		chan = zld_cset->chan + i;
		spin_lock_irqsave(&zdev->lock, flags);
		if (!(block = chan->active_block)) {
			spin_unlock_irqrestore(&zdev->lock, flags);
			continue;
		}
		if (block->uoff < block->datalen) {
			((typeof(datum)*)(block->data + block->uoff))[0]
				= datum;
			block->uoff += sizeof(datum);
		}
		if (block->uoff == block->datalen) {
			done++;
			block->uoff = 0; /* FIXME: use priv_d */
		}
		spin_unlock_irqrestore(&zdev->lock, flags);
		printk("%s: chan %i uoff %i\n", __func__, i, block->uoff);
	}
	if (unlikely(done && done != ZLD_NCHAN)) {
		pr_warning("%s: some blocks are full, but not all (%i/%i)\n",
			   __func__, done, ZLD_NCHAN);
	}
	if (done)
		cset->ti->t_op->data_done(cset);
	return ZLD_PACKET;
}

/* New data is arriving. Collect samples. FIXME: horribly inefficient */
static void zld_receive_buf(struct tty_struct *tty, const unsigned char *cp,
			 char *fp, int count)
{
	static unsigned char buffer[512]; /* FIXME: only one instance by now */
	static int bpos;
	int eaten;

	printk("%i (%i)\n", count, bpos);
	if (count + bpos > sizeof(buffer))
		count = sizeof(buffer) - bpos;
	memcpy(buffer + bpos, cp, count);
	bpos += count;

	/* Loop over data, while packets are there */
	while ( (eaten = __zld_parse(zld_cset, buffer, bpos)) ) {
		memmove(buffer, buffer + eaten, bpos - eaten);
		bpos -= eaten;
	}
}

static struct tty_ldisc_ops zld_ldisc = {
	.magic = TTY_LDISC_MAGIC,
	.owner = THIS_MODULE,
	.open = zld_open,
	.close = zld_close,
	/* FIXME: write should change the input rate */
	.receive_buf = zld_receive_buf,
};

/*
 * What follows is the ZIO driver
 */

/* This method is called by the trigger */
static int zld_input(struct zio_cset *cset)
{
	return -EAGAIN; /* will call data_done later */
}

static struct zio_cset zld_cset[] = {
	{
		.n_chan =	ZLD_NCHAN,
		.ssize =	ZLD_SSIZE,
		.flags =	ZIO_DIR_INPUT | ZCSET_TYPE_ANALOG,
		.raw_io =	zld_input,
	},
};
static struct zio_device zld_dev = {
	.owner =		THIS_MODULE,
	.cset =			zld_cset,
	.n_cset =		ARRAY_SIZE(zld_cset),

};

/* we register when the line discipline is activated */
static unsigned long zld_in_use;

static int zld_open(struct tty_struct *tty)
{
	int err;

	if (test_and_set_bit(0, &zld_in_use)) {
		/* already in use */
		pr_err("%s: line discipline in use\n", __func__);
		return -EBUSY;
	}
	err = zio_register_dev(&zld_dev, "zio-ldisc");
	if (err) {
		clear_bit(0, &zld_in_use);
		return err;
	}
	pr_info("%s: activated ldisc for ADC\n", __func__);
	return 0;
}

static void zld_close(struct tty_struct *tty)
{
	zio_unregister_dev(&zld_dev);
	clear_bit(0, &zld_in_use);
	pr_info("%s: released ZIO ldisc\n", __func__);
}

static int __init zld_init(void)
{
        if (zld_trigger)
                zld_dev.preferred_trigger = zld_trigger;
        if (zld_buffer)
                zld_dev.preferred_buffer = zld_buffer;
	return tty_register_ldisc(ZLD_NUMBER, &zld_ldisc);
}
static void __exit zld_exit(void)
{
	tty_unregister_ldisc(ZLD_NUMBER);
}

module_init(zld_init);
module_exit(zld_exit);

MODULE_LICENSE("GPL");
