/* Davide Silvestri, based on code by Federico Vaga */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>


ZIO_PARAM_TRIGGER(max11040_trigger);
ZIO_PARAM_BUFFER(max11040_buffer);

enum max11040_devices {
	ID_MAX11040,
	ID_MAX11060,
};

#define MAX11040_ATTR_FAKE_NAME	"attr-fake" /* to keep track of how it's used */
#define MAX11040_ADDR_SHIFT	11
#define MAX11040_PM_ADDR		0x0300
#define MAX11040_PM_SHIFT		8
#define MAX11040_VREF_ADDR	0x0400
#define MAX11040_VREF_SHIFT	10
#define MAX11040_SINDUAL_ADDR	0x1000
#define MAX11040_SINDUAL_SHIFT	12

struct max11040_context {
	struct spi_message	message;
	struct spi_transfer	transfer;
	struct zio_cset		*cset;
	unsigned int		chan_enable; /* number of enabled channel */
	uint32_t		nsamples; /* number of samples */
};

struct max11040 {
	struct zio_device	*zdev;
	enum max11040_devices	type;
	struct spi_device	*spi;
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
	ZIO_ATTR_EXT(MAX11040_ATTR_FAKE_NAME, ZIO_RW_PERM,
		     43 /* id ==addr */, 0x0),
};

/* backend for sysfs stores */
static int max11040_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	unsigned long mask = zattr->id;
	struct max11040 *max11040;

	max11040 = to_zio_dev(dev)->priv_d;
	switch (mask) {
	case 43:  /* fake, random, crap! */
		printk("%s: writing fake attr: %i\n", __func__, usr_val);
		break;
	default:
		printk("%s: writing wrong attr: %li = %i\n", __func__,
		       mask, usr_val);
		break;
	}
	return 0;
}

static const struct zio_sysfs_operations max11040_s_op = {
	.conf_set = max11040_conf_set,
};

/* read from MAX11040 and return the pointer to the data */
static void max11040_complete(void *cont)
{
	struct max11040_context *context = cont;
	struct zio_channel *chan;
	struct zio_cset *cset;
	uint16_t *data, *buf;
	int i, j = 0;

	cset = context->cset;
	data = (uint16_t *) context->transfer.rx_buf;
	if (0) {
		/* FIXME: if, by chance, first sample is not good, dicard it */
		data += 1;
	}
	/* demux data */
	chan_for_each(chan, cset) {
		if (!chan->active_block)
			continue;
		buf = (uint16_t *)chan->active_block->data;
		for (i = 0; i < context->nsamples; ++i)
			buf[i] = data[i * context->chan_enable + j];
		++j;
	}
	zio_trigger_data_done(cset);
	/* free context */
	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
}

/* local helper: alloc and fill TX buffer with SPI data for ADC device */
static inline uint16_t *max11040_build_tx(struct max11040 *max11040,
					     struct max11040_context *context,
					     uint32_t size)
{
	//struct zio_channel *chan;
	uint16_t *tx_buf;

	/* configure transfer buffer*/
	tx_buf = kmalloc(size, GFP_ATOMIC);
	if (!tx_buf)
		return ERR_PTR(-ENOMEM);

	/* FIXME: fill TX buffer with MAX commands */

	return tx_buf;
}

static int max11040_input_cset(struct zio_cset *cset)
{
	int err = -EBUSY;
	struct max11040 *max11040;
	struct max11040_context *context;
	uint32_t size, nsamples;

	/* alloc context */
	context = kzalloc(sizeof(struct max11040_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	max11040 = cset->zdev->priv_d;
	context->chan_enable = zio_get_n_chan_enabled(cset);

	/* prepare SPI message and transfer */
	nsamples = cset->chan->current_ctrl->nsamples;

	/*
	 * Calculate buffer size (FIXME)
	 * nsamples + 1: we need one extra fake sample because SPI answer is
	 *               shifted by 1
	 */
	size = (context->chan_enable * nsamples * cset->ssize);

	spi_message_init(&context->message);
	context->message.complete = max11040_complete;
	context->message.context = context;
	context->cset = cset;
	context->nsamples = nsamples;
	context->transfer.len = size;

	context->transfer.rx_buf = kmalloc(size, GFP_ATOMIC);
	if (!context->transfer.rx_buf) {
		err = -ENOMEM;
		goto err_alloc_rx;
	}

	/* allocate configure tx buffer*/
	context->transfer.tx_buf = max11040_build_tx(max11040, context, size);
	if (IS_ERR(context->transfer.tx_buf)) {
		err = PTR_ERR(context->transfer.tx_buf);
		goto err_alloc_tx;
	}

	spi_message_add_tail(&context->transfer, &context->message);

	/* start acquisition */
	err = spi_async_locked(max11040->spi, &context->message);
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
		.raw_io = max11040_input_cset,
		.ssize = 3,
		.n_chan = 32, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct zio_cset max11060_ain_cset[] = { /* 16bit, up to 32 channels */
	{
		.raw_io = max11040_input_cset,
		.ssize = 2,
		.n_chan = 32, /* FIXME: change at runtime */
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};

static struct zio_device max11040_tmpl[] = {
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

static int max11040_zio_probe(struct zio_device *zdev)
{
	struct zio_attribute_set *zattr_set;
	struct max11040 *max11040;

	pr_info("%s:%d\n", __func__, __LINE__);
	max11040 = zdev->priv_d;
	zattr_set = &zdev->zattr_set;
	max11040->zdev = zdev;

	/* FIXME: autodetect number of channels */
	printk("%s: n_chan = %i\n", __func__, zdev->cset->n_chan);
	zdev->cset->n_chan = 8;

	/* nothing special to do (maybe build spi tx command word?) */
	return 0;
}

static const struct zio_device_id max11040_table[] = {
	{"max11040", &max11040_tmpl[ID_MAX11040]},
	{"max11060", &max11040_tmpl[ID_MAX11060]},
	{},
};
static struct zio_driver max11040_zdrv = {
	.driver = {
		.name = "zio-max11040",
		.owner = THIS_MODULE,
	},
	.id_table = max11040_table,
	.probe = max11040_zio_probe,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(__ZIO_MAJOR_VERSION,
				   __ZIO_MINOR_VERSION,
				   0),
};


/* We create a ZIO device when our SPI driver gets access to a physical dev */
static int max11040_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	struct zio_device *zdev;
	struct max11040 *max11040;
	int err = 0;
	uint32_t dev_id;

	max11040 = kzalloc(sizeof(struct max11040), GFP_KERNEL);
	if (!max11040)
		return -ENOMEM;

	/* Configure SPI */
	spi->bits_per_word = 16; /* FIXME.... 24? */
	err = spi_setup(spi);
	if (err)
		goto errout;
	err = -ENOENT;
	spi_id = spi_get_device_id(spi);
	if (!spi_id)
		goto errout;
	max11040->spi = spi;
	max11040->type = spi_id->driver_data; /* FIXME: check this */
	printk("%s: type: %i\n", __func__, max11040->type);

	/* zdev here is the generic device */
	zdev = zio_allocate_device();
	zdev->priv_d = max11040;
	zdev->owner = THIS_MODULE;
	spi_set_drvdata(spi, zdev);

	dev_id = spi->chip_select | (spi->master->bus_num << 8);

	/* Register a ZIO device */
	err = zio_register_device(zdev, spi_id->name, dev_id);
errout:
	if (err)
		kfree(max11040);
	return err;
}

static int max11040_spi_remove(struct spi_device *spi)
{
	struct zio_device *zdev;
	struct max11040 *max11040;

	/* zdev here is the generic device */
	zdev = spi_get_drvdata(spi);
	max11040 = zdev->priv_d;
	zio_unregister_device(zdev);
	kfree(max11040);
	zio_free_device(zdev);
	return 0;
}

static const struct spi_device_id max11040_id[] = {
	{"max11040", ID_MAX11040},
	{"max11060", ID_MAX11060},
	{}
};

static struct spi_driver max11040_driver = {
	.driver = {
		.name	= "max110x0",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= max11040_id,
	.probe		= max11040_spi_probe,
	.remove		= max11040_spi_remove,
};

static int __init max11040_init(void)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(max11040_tmpl); ++i) {
		if (max11040_trigger)
			max11040_tmpl[i].preferred_trigger = max11040_trigger;
		if (max11040_buffer)
			max11040_tmpl[i].preferred_buffer = max11040_buffer;
	}
	err = zio_register_driver(&max11040_zdrv);
	if (err)
		return err;
	return spi_register_driver(&max11040_driver);
}
static void __exit max11040_exit(void)
{
	driver_unregister(&max11040_driver.driver);
	zio_unregister_driver(&max11040_zdrv);
}

module_init(max11040_init);
module_exit(max11040_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Davide Silvestri");
MODULE_DESCRIPTION("MAX11040/MAX11060 driver for ZIO framework");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
