/* Davide Silvestri, based on code by Federico Vaga */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <asm/unaligned.h>


ZIO_PARAM_TRIGGER(max110x0_trigger);
ZIO_PARAM_BUFFER(max110x0_buffer);

enum max110x0_devices {
	ID_MAX11040,
	ID_MAX11060,
};

#define MAX110X0_ATTR_FAKE_NAME "attr-fake" /* to keep track of how it's used */
#define MAX110X0_ADDR_SHIFT 11
#define MAX110X0_PM_ADDR  0x0300
#define MAX110X0_PM_SHIFT  8
#define MAX110X0_VREF_ADDR 0x0400
#define MAX110X0_VREF_SHIFT 10
#define MAX110X0_SINDUAL_ADDR 0x1000
#define MAX110X0_SINDUAL_SHIFT 12

/* Max110x0 registers */
#define MAX110X0_REG_WR_SAMP	0x40  // data: 32 * n_dev bits
#define MAX110X0_REG_RD_SAMP	0xc0  // data: 32 * n_dev bits
#define MAX110X0_REG_WR_RATE	0x50  // 16 bits
#define MAX110X0_REG_RD_RATE	0xd0  // 16 bits
#define MAX110X0_REG_WR_CONF	0x60  // 8 x n_dev bits
#define MAX110X0_REG_RD_CONF	0xe0  // 8 x n_dev bits
#define MAX110X0_REG_RD_DATA	0xf0  // 96 x n_dev

/* Configuration register bits */
#define MAX110X0_SHDN	 	(1 << 7)
#define MAX110X0_RST 		(1 << 6)
#define MAX110X0_EN24BIT	(1 << 5)
#define MAX110X0_XTALEN 	(1 << 4)
#define MAX110X0_FAULTDIS	(1 << 3)
#define MAX110X0_PDBUF	 	(1 << 2)

/* Data-rates */
#define MAX110X0_250SPS		0x27ff
#define MAX110X0_500SPS		0x2000
#define MAX110X0_1KSPS		0x4000
#define MAX110X0_2KSPS		0x6000
#define MAX110X0_4KSPS		0x8000
#define MAX110X0_8KSPS		0xa000
#define MAX110X0_16KSPS		0x0000
#define MAX110X0_32KSPS		0xc000
#define MAX110X0_64KSPS		0xe000


struct max110x0_context {
	struct spi_message msg;
	struct spi_transfer xfer;
	struct zio_cset *cset;
	struct spi_device *spi;
	struct completion done;
	uint32_t cnt;
};

struct max110x0 {
	struct zio_device *zdev;
	enum max110x0_devices type;
	struct spi_device *spi;
	//uint16_t		cmd; -- FIXME
};

/* Standard attributes for MAX11040 */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_max11040) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 24),
};

/* Standard attributes for MAX11060 */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_max11060) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 16),
};

/* Extended attributes for MAX110[46]0 -- fake attribute by now */
static struct zio_attribute zattr_dev_ext_max110x0[] = {
	ZIO_ATTR_EXT(MAX110X0_ATTR_FAKE_NAME, ZIO_RW_PERM,
		43 /* id ==addr */, 0x0),
};

/* backend for sysfs stores */
static int max110x0_conf_set(struct device *dev, struct zio_attribute *zattr,
				uint32_t usr_val)
{
	unsigned long mask = zattr->id;
	struct max110x0 *max110x0;

	max110x0 = to_zio_dev(dev)->priv_d;
	switch (mask) {
	case 43: /* fake, random, crap! */
		printk("%s: writing fake attr: %i\n", __func__, usr_val);
		break;
	default:
		printk("%s: writing wrong attr: %li = %i\n", __func__,
			mask, usr_val);
		break;
	}
	return 0;
}

static const struct zio_sysfs_operations max110x0_s_op = {
	.conf_set = max110x0_conf_set,
};

/* read from MAX110[46]0 and return the pointer to the data */
static void max110x0_complete(void *cont)
{
	struct max110x0_context *cxt = cont;
	struct zio_channel *chan;
	struct zio_cset *cset;
	uint8_t *data;
	int32_t *buf, tmp;

	cset = cxt->cset;
	data = (uint8_t *) cxt->xfer.rx_buf;
	data += 1;  // skip the command byte

	/* demux data */
	chan_for_each(chan, cset) {
		/* samples are 24-bit wide, big-endian: read unaligned */
		tmp = get_unaligned_be32(data);
		data += 3;
		if (!chan->active_block)
			continue;
		buf = chan->active_block->data;
		buf[cxt->cnt] = tmp >> 8;
	}
	if (++cxt->cnt < cset->ti->nsamples)
		return; /* gpio IRQ will fire next xfer */

	/* The block is over */
	zio_trigger_data_done(cset);
	/* reset the counter */
	cxt->cnt = 0;
}

static irqreturn_t max110x0_gpio_irq(int irq, void *arg)
{
	struct zio_cset *cset = arg;
	struct max110x0_context *cxt = cset->priv_d;

	if (likely(cxt))
		spi_async_locked(cxt->spi, &cxt->msg);

	return IRQ_HANDLED;
}


static int max110x0_raw_io(struct zio_cset *cset)
{
	/* We cannot be armed if there's no block. Wait for next push */
	/*if (!cset->chan->active_block)
		return -EIO;*/
	return -EAGAIN;
}

/* channel sets available */
static struct zio_cset max11040_ain_cset[] = { /* 24bit, up to 32 channels */
	{
		.raw_io = max110x0_raw_io,
		.ssize = 4,  /* FIXME: should be 3, but then should be uint24_t? */
		.n_chan = 4, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG | /* is analog */
			ZIO_DIR_INPUT | /* is input */
			ZIO_CSET_SELF_TIMED /* is self-timed */,
	},
};
static struct zio_cset max11060_ain_cset[] = { /* 16bit, up to 32 channels */
	{
		.raw_io = max110x0_raw_io,
		.ssize = 2,
		.n_chan = 4, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG | /* is analog */
			ZIO_DIR_INPUT | /* is input */
			ZIO_CSET_SELF_TIMED /* is self-timed */,
	},
};

static struct zio_device max110x0_tmpl[] = {
	[ID_MAX11040] = {
		.owner = THIS_MODULE,
		.flags = 0,
		.cset = max11040_ain_cset,
		.n_cset = 1,
		.zattr_set = {
			.std_zattr = zattr_dev_max11040,
			.ext_zattr = zattr_dev_ext_max110x0,
		},
	},
	[ID_MAX11060] = {
		.owner = THIS_MODULE,
		.flags = 0,
		.cset = max11060_ain_cset,
		.n_cset = 1,
		.zattr_set = {
			.std_zattr = zattr_dev_max11060,
			.ext_zattr = zattr_dev_ext_max110x0,
		},
	},
};


static void free_max110x0_context(void *context) {
	struct max110x0_context *cxt = context;

	kfree(cxt->xfer.tx_buf);
	kfree(cxt->xfer.rx_buf);
	kfree(cxt);
}

static inline uint32_t max110x0_sync_gpio(struct zio_device *zdev) {
	return zdev->dev_id == 0 ? 1 : 3;
}

static inline int max110x0_write_conf(struct spi_device * spi,
		uint8_t conf, uint8_t ndevice) {

	uint8_t *buf;
	int i, ret, size = 1 + ndevice;

	buf = kmalloc(size, GFP_ATOMIC);
	if(!buf)
		return -ENOMEM;

	buf[0] = MAX110X0_REG_WR_CONF;
	for(i=0; i<ndevice; ++i)
		buf[i + 1] = conf;

	ret = spi_write(spi, buf, size);
	kfree(buf);
	return ret;
}

static inline int max110x0_write_datarate(struct spi_device * spi,
		uint16_t datarate) {

	uint8_t *buf;
	int ret, size = 3;

	buf = kmalloc(size, GFP_ATOMIC);
	if(!buf)
		return -ENOMEM;

	buf[0] = MAX110X0_REG_WR_RATE;
	put_unaligned_be16(datarate, buf + 1);

	ret = spi_write(spi, buf, size);
	kfree(buf);
	return ret;
}

static int max110x0_setup(struct zio_device *zdev) {

	struct max110x0_context *data_cxt;
	struct max110x0 *max110x0 = zdev->priv_d;
	struct zio_cset *cset = zdev->cset;
	uint8_t *tx_buf, *rx_buf;
	uint32_t size, ndevice;
	int err;

	/* FIXME: max110x0_context for conf and data-rate messages is too much.
	spi_message(s) and spi_transfer(s) are enough */

	printk("write conf\n");
	ndevice = cset->n_chan >> 2; // each device has 4 channels
	err = max110x0_write_conf(max110x0->spi, MAX110X0_EN24BIT, ndevice);
	if (err) {
		printk("error writing conf message");
		goto errout;
	}
	printk("conf writed\n");

	/* alloc context for the read data register command */
	data_cxt = kzalloc(sizeof(struct max110x0_context), GFP_ATOMIC);
	if (!data_cxt) {
		err = -ENOMEM;
		goto errout;
	}

	/* information for the irq handler callback */
	data_cxt->cset = cset;
	data_cxt->spi = max110x0->spi;
	cset->priv_d = data_cxt;

	/* prepare read data register command message and buffers */
	spi_message_init(&data_cxt->msg);
	data_cxt->msg.complete = max110x0_complete;
	data_cxt->msg.context = data_cxt;
	/* 1 byte for the command + 24bit for each chan */
	size = 1 + cset->n_chan * 3;
	data_cxt->xfer.len = size;

	rx_buf = kmalloc(size, GFP_ATOMIC);
	tx_buf = kzalloc(size, GFP_ATOMIC);

	data_cxt->xfer.rx_buf = rx_buf;
	data_cxt->xfer.tx_buf = tx_buf;

	if (!tx_buf || !rx_buf) {
		err = -ENOMEM;
		goto free_data_cxt;
	}
	/* read data register command */
	tx_buf[0] = MAX110X0_REG_RD_DATA;

	spi_message_add_tail(&data_cxt->xfer, &data_cxt->msg);

	printk("register irq\n");
	/* register GPIO interrupt to handle data transfer */
	// FIXME: pioA1 for spi1, pioA3 for spi2. How to use a module param?
	if (request_irq(gpio_to_irq(max110x0_sync_gpio(zdev)),
			max110x0_gpio_irq, IRQF_TRIGGER_FALLING,
	 		dev_name(&zdev->head.dev), cset) < 0) {
		printk("no irq: merda\n");
		err = -EBUSY;
		goto free_data_cxt;
	}

	printk("write data-rate\n");
	/* configure data rate and start fire data transfer by the IRQ handler */
	err = max110x0_write_datarate(max110x0->spi, MAX110X0_1KSPS);
	if (!err)
		return 0;

	printk("error writing datarate");
	free_irq(gpio_to_irq(max110x0_sync_gpio(zdev)), cset);
free_data_cxt:
	free_max110x0_context(data_cxt);
errout:
	return err;
}

static int max110x0_zio_probe(struct zio_device *zdev)
{
	struct zio_attribute_set *zattr_set;
	struct max110x0 *max110x0;

	max110x0 = zdev->priv_d;
	zattr_set = &zdev->zattr_set;
	max110x0->zdev = zdev;

	/* configure max110x0 to start the self-timed acquisition */
	return max110x0_setup(zdev);
}

static void max110x0_last_complete(void *cont)
{
	struct max110x0_context *cxt = cont;
	struct zio_cset *cset = cxt->cset;

	/* deregister data_cxt handler */
	free_irq(gpio_to_irq(max110x0_sync_gpio(cset->zdev)), cset);
	complete_all(&cxt->done);
}

static int max110x0_zio_remove(struct zio_device *zdev)
{
	struct zio_cset *cset = zdev->cset;
	struct max110x0_context *cxt = cset->priv_d;

	/* free the context */
	init_completion(&cxt->done);
	cxt->msg.complete = max110x0_last_complete;
	wait_for_completion(&cxt->done);
	free_max110x0_context(cxt);

	/* FIXME: how to stop already queued spi messages ? */

	return 0;
}

static const struct zio_device_id max110x0_table[] = {
	{"max11040", &max110x0_tmpl[ID_MAX11040]},
	{"max11060", &max110x0_tmpl[ID_MAX11060]},
	{},
};

static struct zio_driver max110x0_zdrv = {
	.driver = {
		.name = "zio-max110x0",
		.owner = THIS_MODULE,
	},
	.id_table = max110x0_table,
	.probe = max110x0_zio_probe,
	.remove = max110x0_zio_remove,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};


/* We create a ZIO device when our SPI driver gets access to a physical dev */
static int max110x0_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	struct zio_device *zdev;
	struct max110x0 *max110x0;
	int err = 0;
	uint32_t dev_id;

	max110x0 = kzalloc(sizeof(struct max110x0), GFP_KERNEL);
	if (!max110x0)
		return -ENOMEM;

	/* FIXME: autodetect number of channels */
	//max11040_ain_cset->n_chan = 4 + 4 * spi->chip_select;

	/* Configure SPI */
	/* FIXME: default to 8 (as ctrl reg)
	   NOTE spi_transfer.bits_per_word can override this for each transfer */
	// spi->bits_per_word = 16;
	err = spi_setup(spi);
	if (err)
		goto errout;
	err = -ENOENT;
	spi_id = spi_get_device_id(spi);
	if (!spi_id)
		goto errout;
	max110x0->spi = spi;
	max110x0->type = spi_id->driver_data; /* FIXME: check this */

	/* zdev here is the generic device */
	zdev = zio_allocate_device();
	zdev->priv_d = max110x0;
	zdev->owner = THIS_MODULE;
	spi_set_drvdata(spi, zdev);

	dev_id = spi->chip_select | (32766 - spi->master->bus_num);

	/* Register a ZIO device */
	err = zio_register_device(zdev, spi_id->name, dev_id);
errout:
	if (err)
		kfree(max110x0);
	return err;
}

static int max110x0_spi_remove(struct spi_device *spi)
{
	struct zio_device *zdev;
	struct max110x0 *max110x0;

	/* zdev here is the generic device */
	/* FIXME: but SPI? May I kfree all even with an active command */
	zdev = spi_get_drvdata(spi);
	max110x0 = zdev->priv_d;
	zio_unregister_device(zdev);
	kfree(max110x0);
	zio_free_device(zdev);
	return 0;
}

static const struct spi_device_id max110x0_id[] = {
	{"max11040", ID_MAX11040},
	{"max11060", ID_MAX11060},
	{}
};

static struct spi_driver max110x0_driver = {
	.driver = {
		.name = "max110x0",
		.bus = &spi_bus_type,
		.owner = THIS_MODULE,
	},
	.id_table = max110x0_id,
	.probe  = max110x0_spi_probe,
	.remove  = max110x0_spi_remove,
};

static int __init max110x0_init(void)
{
	int err, i;
	for (i = 0; i < ARRAY_SIZE(max110x0_tmpl); ++i) {
		if (max110x0_trigger)
			max110x0_tmpl[i].preferred_trigger = max110x0_trigger;
		if (max110x0_buffer)
			max110x0_tmpl[i].preferred_buffer = max110x0_buffer;
	}
	err = zio_register_driver(&max110x0_zdrv);
	if (err)
		return err;
	// FIXME: check for errors? And unregister the zio_device in case
	return spi_register_driver(&max110x0_driver);
}

static void __exit max110x0_exit(void)
{
	driver_unregister(&max110x0_driver.driver);
	zio_unregister_driver(&max110x0_zdrv);
}

module_init(max110x0_init);
module_exit(max110x0_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Davide Silvestri");
MODULE_DESCRIPTION("MAX11040/MAX11060 driver for ZIO framework");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
