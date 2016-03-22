/* Davide Silvestri, based on code by Federico Vaga */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
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

struct max110x0_context {
	struct spi_message message;
	struct spi_transfer transfer;
	struct zio_cset *cset;
	struct spi_device *spi;
	unsigned int chans_enabled; /* number of enabled channels */
	uint32_t current_sample, nsamples;
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
	struct max110x0_context *context = cont;
	struct zio_channel *chan;
	struct zio_cset *cset;
	uint8_t *data;
	int32_t *buf, tmp;
	int err;

	cset = context->cset;
	data = (uint8_t *) context->transfer.rx_buf;
	data += 1;  // skip the command byte

	/* demux data */
	chan_for_each(chan, cset) {
		if (!chan->active_block)
			continue;
		buf = chan->active_block->data;

		/* samples are 24-bit wide, big-endian: read unaligned */
		tmp = get_unaligned_be32(data);
		buf[context->current_sample] = tmp >> 8;
		data += 3;
	}
	if (++context->current_sample < context->nsamples) {
		err = spi_async_locked(context->spi, &context->message);
		if (err) {
			printk("merda\n"); /* FIXME: raise alarm */
			goto out;
		}
		return;
	}
out:
	/* The block is over */
	zio_trigger_data_done(cset);
	/* free context */
	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
}

static irqreturn_t max110x0_gpio_irq(int irq, void *arg)
{
	struct zio_cset *cset = arg;

	printk("%s: %p\n", __func__, cset);
	/* FIXME: fire transfer */
	return IRQ_NONE;
}


static int max110x0_input_cset(struct zio_cset *cset)
{
	int err = -EBUSY;
	struct max110x0 *max110x0;
	struct max110x0_context *context;
	uint8_t *tx_buf;
	uint32_t size, nsamples;

	/* alloc context */
	context = kzalloc(sizeof(struct max110x0_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	max110x0 = cset->zdev->priv_d;
	context->chans_enabled = zio_get_n_chan_enabled(cset);

	/* prepare SPI message and transfer */
	nsamples = cset->chan->current_ctrl->nsamples;

	 /* FIXME: 1 byte of command + 96 (24*4) bit data register
	 for every max110x0 in the daisy chain */
	size = (1 + (96 / 8 * 1));

	spi_message_init(&context->message);
	context->message.complete = max110x0_complete;
	context->message.context = context;
	context->cset = cset;
	context->spi = max110x0->spi;
	context->nsamples = nsamples;
	context->transfer.len = size;

	/* allocate the rx buffer */
	context->transfer.rx_buf = kmalloc(size, GFP_ATOMIC);
	if (!context->transfer.rx_buf) {
		err = -ENOMEM;
		goto err_alloc_rx;
	}

	/* allocate the tx buffer */
	tx_buf = kzalloc(size, GFP_ATOMIC);
	context->transfer.tx_buf = tx_buf;
	if (!tx_buf) {
		err = -ENOMEM;
		goto err_alloc_tx;
	}

	cset->priv_d = context;

	/* register GPIO interrupt -- pioA3 */
	if (request_irq(gpio_to_irq(3), max110x0_gpio_irq, 
			IRQF_TRIGGER_FALLING,
			"max110x0-sync", cset) < 0)
		printk("no irq: merda\n");

	// configure the data rate control register
	tx_buf[0] = 0x50;
	tx_buf[1] = 0x27;
	tx_buf[2] = 0xff;
	context->transfer.len = 3;

	spi_message_add_tail(&context->transfer, &context->message);

	/* start xter to configure data rate */
	err = spi_async_locked(context->spi, &context->message);
	if (!err)
		return -EAGAIN; /* success with callback */

	kfree(context->transfer.tx_buf);
err_alloc_tx:
	kfree(context->transfer.rx_buf);
err_alloc_rx:
	kfree(context);
	return err;
}

/* channel sets available */
static struct zio_cset max11040_ain_cset[] = { /* 24bit, up to 32 channels */
	{
		.raw_io = max110x0_input_cset,
		.ssize = 4,  /* FIXME: should be 3, but then should be uint24_t? */
		.n_chan = 4, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG | /* is analog */
			ZIO_DIR_INPUT /* is input */,
	},
};
static struct zio_cset max11060_ain_cset[] = { /* 16bit, up to 32 channels */
	{
		.raw_io = max110x0_input_cset,
		.ssize = 2,
		.n_chan = 4, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG | /* is analog */
			ZIO_DIR_INPUT /* is input */,
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

static int max110x0_zio_probe(struct zio_device *zdev)
{
	struct zio_attribute_set *zattr_set;
	struct max110x0 *max110x0;

	pr_info("%s:%d\n", __func__, __LINE__);
	max110x0 = zdev->priv_d;
	zattr_set = &zdev->zattr_set;
	max110x0->zdev = zdev;

	printk("%s: n_chan = %i\n", __func__, zdev->cset->n_chan);

	/* nothing special to do (maybe build spi tx command word?) */
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
	printk("%s: type: %i\n", __func__, max110x0->type);

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