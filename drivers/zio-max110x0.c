/* 
Davide Silvestri, based on code by Federico Vaga 

Code for the interleaved version, with 2 different SPI but obly 
1 pin as SYNC (the IRQ to start the acquisition)
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <asm/atomic.h>
#include <asm/unaligned.h>


ZIO_PARAM_TRIGGER(max110x0i_trigger);
ZIO_PARAM_BUFFER(max110x0i_buffer);

enum max110x0i_devices {
	ID_MAX11040I,
};

#define MAX110X0_ATTR_FAKE_NAME "attr-fake" /* to keep track of how it's used */
#define MAX110X0_ADDR_SHIFT	11
#define MAX110X0_PM_ADDR	   0x0300
#define MAX110X0_PM_SHIFT	  8
#define MAX110X0_VREF_ADDR	 0x0400
#define MAX110X0_VREF_SHIFT	10
#define MAX110X0_SINDUAL_ADDR  0x1000
#define MAX110X0_SINDUAL_SHIFT 12

/* MAX110X0 registers */
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

#define MAX110X0_SPI_DEVICES 		2
#define MAX110X0_CHAN_PER_MAX	 	4
#define MAX110X0_DEV_CASCADE		8 
#define MAX110X0_CHAN_PER_SPI		32  // CHAN_PER_MAX * DEV_CASCADE
#define MAX110X0_NICHANNELS 		66  // CHAN_PER_SPI * SPI_DEVICES + sequence and timestamp

#define MAX110X0_IRQ_GPIO			110
#define MAX110X0_IRQ         		gpio_to_irq(MAX110X0_IRQ_GPIO)
#define SAMPLES_PER_BUFFER 			10
#define MICOSI_BLK_SAMPLES			(MAX110X0_NICHANNELS * SAMPLES_PER_BUFFER)
#define MICOSI_BLK_SIZE				(sizeof(int32_t) * MICOSI_BLK_SAMPLES)


struct spi_context {
	struct spi_device *spi;
	uint32_t curr_sample;
};

static struct workqueue_struct *zwq;
static void max110x0_work_handler(struct work_struct *w); 
static DECLARE_WORK(max110x0_work, max110x0_work_handler);

static uint8_t *tx_buf, *rx_buf;

 /* linked in the priv_d pointer of the global zio device zdev */
struct max110x0i {
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct spi_context *cxt;
};
struct max110x0i *max110x0i_data; /* allocated at module init */

/* Standard attributes for MAX11040 */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_max11040i) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 24),
};

/* Extended attributes for MAX110[46]0 -- fake attribute by now */
static struct zio_attribute zattr_dev_ext_max110x0i[] = {
	ZIO_ATTR_EXT(MAX110X0_ATTR_FAKE_NAME, ZIO_RW_PERM,
		43 /* id ==addr */, 0x0),
};

static struct zio_attribute max110x0i_cset1_ext[] = {
	ZIO_ATTR_EXT("ns-tick", ZIO_RO_PERM, 0, 1000000),
};

/* backend for sysfs stores */
static int max110x0i_conf_set(struct device *dev, struct zio_attribute *zattr,
				uint32_t usr_val)
{
	unsigned long mask = zattr->id;
	struct max110x0i *max110x0i;

	max110x0i = to_zio_dev(dev)->priv_d;
	switch (mask) {
	case 43: /* fake, random, crap! */
		pr_info("%s: writing fake attr: %i\n", __func__, usr_val);
		break;
	default:
		pr_info("%s: writing wrong attr: %li = %i\n", __func__,
			mask, usr_val);
		break;
	}
	return 0;
}

static const struct zio_sysfs_operations max110x0i_s_op = {
	.conf_set = max110x0i_conf_set,
};

static irqreturn_t max110x0i_gpio_irq(int irq, void *arg)
{
	if (unlikely(gpio_get_value(MAX110X0_IRQ_GPIO)))
		return IRQ_HANDLED;
	
	queue_work(zwq, &max110x0_work);
	return IRQ_HANDLED;
}

static void max110x0_work_handler(struct work_struct *w) {
	struct zio_cset *cset = max110x0i_data->cset;
	struct zio_ti *ti = cset->ti;
	struct zio_channel *chan = &(cset->chan[MAX110X0_NICHANNELS]);
	struct zio_block *block = chan->active_block;
	struct spi_context *cxt = max110x0i_data->cxt;
	struct spi_message msg;
	struct spi_transfer xfer;
	int32_t *buf = 0;
	int ret;
	unsigned long flags;
	int32_t nsamples = chan->current_ctrl->nsamples,
		avail_space = nsamples - cxt->curr_sample;

	memset(&xfer, 0, sizeof(xfer));
	if (unlikely(!block)) {
		spin_lock_irqsave(&cset->lock, flags);
		ti->flags &= ~ZIO_TI_ARMED;
		spin_unlock_irqrestore(&cset->lock, flags);
		chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_TRIGGER;
		//printk("buffer full\n");
		cxt->curr_sample = 0;
		xfer.rx_buf = 0;
	} else if (unlikely(avail_space < MICOSI_BLK_SAMPLES)) { // do nothing if no block or there is not enough space available 
		chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_TRIGGER;
		//printk("buffer unaligned: %d required, %d avaliable \n", min_space, avail_space);
		cxt->curr_sample = 0;
		xfer.rx_buf = 0;
	} else {
		buf = block->data;
		buf += cxt->curr_sample;
		xfer.rx_buf = rx_buf;
		//xfer.rx_buf = buf;
	}
	xfer.tx_buf = tx_buf;
	xfer.len = MICOSI_BLK_SIZE;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	ret = spi_sync(cxt->spi, &msg);
	if (ret) {
		printk("error in spi_sync\n");
	}
	if (buf)
		memcpy(buf, rx_buf, MICOSI_BLK_SIZE);
	cxt->curr_sample += MICOSI_BLK_SAMPLES;

	if (unlikely(cxt->curr_sample >= nsamples)) {
        cxt->curr_sample = 0;
        zio_trigger_data_done(cset);
    }
}

static int max110x0i_raw_io(struct zio_cset *cset)
{
	//struct zio_channel *chan;
	//
	// chan_for_each(chan, cset) {
	// 		if (!chan->active_block)
	// 			return -EIO;
	// 		//printk(" %d %p", chan->index, block);
	// }
	return -EAGAIN;
}

/* channel sets available */
static struct zio_cset max11040i_ain_cset[] = { /* 24bit, up to 32 channels */
	{
		.raw_io = max110x0i_raw_io,
		.ssize =  4,
		.n_chan = MAX110X0_NICHANNELS,  // 64 channels
		.flags = ZIO_CSET_TYPE_ANALOG | /* is analog */
			ZIO_DIR_INPUT | /* is input */
			ZIO_CSET_SELF_TIMED | /* is self-timed */
			ZIO_CSET_CHAN_INTERLEAVE | ZIO_CSET_INTERLEAVE_ONLY /* interleaved */,
		.zattr_set = {
			//.std_zattr = zattr_dev_max11040i,
			.ext_zattr = max110x0i_cset1_ext,
			.n_ext_attr = ARRAY_SIZE(max110x0i_cset1_ext),
		},
	},
};

static struct zio_device max110x0i_tmpl[] = {
	[ID_MAX11040I] = {
		.owner = THIS_MODULE,
		.flags = 0,
		.cset = max11040i_ain_cset,
		.n_cset = 1,
		.zattr_set = {
			.std_zattr = zattr_dev_max11040i,
			.ext_zattr = zattr_dev_ext_max110x0i,
		},
	},
};


static int max110x0i_zio_probe(struct zio_device *zdev)
{
	max110x0i_data->cset = zdev->cset;
	return 0;
}


static int max110x0i_zio_remove(struct zio_device *zdev)
{
	return 0;
}

static const struct zio_device_id max110x0i_table[] = {
	{"max11040i", &max110x0i_tmpl[ID_MAX11040I]},
	{},
};

static struct zio_driver max110x0i_zdrv = {
	.driver = {
		.name = "zio-max110x0i",
		.owner = THIS_MODULE,
	},
	.id_table = max110x0i_table,
	.probe = max110x0i_zio_probe,
	.remove = max110x0i_zio_remove,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

/* We create a ZIO device when our SPI driver gets access to a physical dev */
static int max110x0i_spi_probe(struct spi_device *spi)
{
	struct spi_context *cxt;
	int err = 0;

	/* Configure SPI */
	err = spi_setup(spi);
	if (err) {
		return err;
	}

	cxt = max110x0i_data->cxt;
	cxt->spi = spi;
	/* setup the spi to be ready to handle data */
	tx_buf = kzalloc(MICOSI_BLK_SIZE, GFP_ATOMIC);
	if (!tx_buf) {
		return -ENOMEM;
	}
	rx_buf = kzalloc(MICOSI_BLK_SIZE, GFP_ATOMIC);
	if (!rx_buf) {
		kfree(tx_buf);
		return -ENOMEM;
	}

	return 0;	
}

static int max110x0i_spi_remove(struct spi_device *spi)
{
	kfree(tx_buf);
	kfree(rx_buf);

	/* reset the pointer for the spi */
	/*if (dev_id < MAX110X0_SPI_DEVICES) {
		spi_cxt = &(max110x0i_data->cxt);
		xfer = &(spi_cxt->xfer);
		kfree(xfer->rx_buf);
		kfree(xfer->tx_buf);
	}*/
	return 0;
}

static const struct spi_device_id max110x0i_id[] = {
	{"max11040i", ID_MAX11040I},
	{}
};

static struct spi_driver max110x0i_driver = {
	.driver = {
		.name = "max110x0i",
		.bus = &spi_bus_type,
		.owner = THIS_MODULE,
	},
	.id_table = max110x0i_id,
	.probe  = max110x0i_spi_probe,
	.remove  = max110x0i_spi_remove,
};

static int __init max110x0i_init(void)
{
	struct zio_device *zdev;
	int err, i;

	zwq = create_workqueue("zio-max110x0i-workqueue");
	if (!zwq)
		return -1;

	max110x0i_data = kzalloc(sizeof(struct max110x0i), GFP_KERNEL);
	if (!max110x0i_data) {
		return -ENOMEM;
	}

	/* allocate space for every spi device used by the zio device */
	max110x0i_data->cxt = kzalloc(sizeof(struct spi_context), GFP_KERNEL);
	if (!max110x0i_data->cxt) {
		err = -ENOMEM;
		goto errout;
	}

	for (i = 0; i < ARRAY_SIZE(max110x0i_tmpl); ++i) {
		if (max110x0i_trigger)
			max110x0i_tmpl[i].preferred_trigger = max110x0i_trigger;
		if (max110x0i_buffer)
			max110x0i_tmpl[i].preferred_buffer = max110x0i_buffer;
	}
	err = zio_register_driver(&max110x0i_zdrv);
	if (err)
		goto errout;
	
	/* zdev (global) here is the generic device */
	zdev = zio_allocate_device();
	if (IS_ERR(zdev)) {
		err = PTR_ERR(zdev);
		goto errout;
	}
	zdev->owner = THIS_MODULE;
	max110x0i_data->zdev = zdev;

	/* Register the single ZIO device */
	err = zio_register_device(zdev, "max11040i", 0);
	if (err) 
		goto errdev;

	err = spi_register_driver(&max110x0i_driver);
	if (err)
		goto errdev;
	
	//getnstimeofday(&max110x0i_data->last_ts);
	err = request_irq(MAX110X0_IRQ, max110x0i_gpio_irq, 
            IRQF_TRIGGER_FALLING, dev_name(&zdev->head.dev), max110x0i_data->cxt);
	if (!err)
		return 0;

errdev:
	zio_free_device(zdev);
errout:
	kfree(max110x0i_data->cxt);
	kfree(max110x0i_data);
	return err;
}

static void __exit max110x0i_exit(void)
{
	struct zio_device *zdev = max110x0i_data->zdev;

	/* deregister data_cxt handler */
	free_irq(MAX110X0_IRQ, max110x0i_data->cxt);

	// cancel delayed work and wait to finish
	flush_workqueue(zwq);
	destroy_workqueue(zwq);

	driver_unregister(&max110x0i_driver.driver); /* call spi_remove */
	zio_unregister_device(zdev);
	zio_free_device(zdev);
	kfree(max110x0i_data->cxt);
	kfree(max110x0i_data);
	zio_unregister_driver(&max110x0i_zdrv);
}

module_init(max110x0i_init);
module_exit(max110x0i_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Davide Silvestri");
MODULE_DESCRIPTION("MAX11040/MAX11060 interleaved driver for ZIO framework");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
