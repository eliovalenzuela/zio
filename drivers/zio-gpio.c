/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * Simple driver for GPIO-based output (faked as 1 analog
 * channel).  Initially was running on the parallel port, then I
 * switched to a more generic implementation. The "analog" moves all
 * bits.  There is currently no such thing as internal timing, only
 * the last sample of a block has enduring effect.
 *
 * Missing feature: internal rate (needs a transparent buffer)
 * Missing feature: input
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/io.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>

#define ZGPIO_NOUT 8
#define ZGPIO_NIN 8

/* Output and input bits are selected at load time */
static int zgp_out[ZGPIO_NOUT];
static int zgp_nout;
module_param_array_named(out, zgp_out, int, &zgp_nout, 0444);
static int zgp_in[ZGPIO_NIN];
static int zgp_nin;
module_param_array_named(in, zgp_in, int, &zgp_nin, 0444);

ZIO_PARAM_TRIGGER(zgp_trigger);
ZIO_PARAM_BUFFER(zgp_buffer);

/* This outputs a cset, currently made up of one channel only */
static int zgp_output(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;
	struct zio_control *ctrl;
	uint8_t datum;
	int i;

	cset_for_each(cset, chan) {
		block = chan->active_block;
		if (!block)
			continue;
		ctrl = zio_get_ctrl(block);
		/* use last sample, as previous ones would be overwritten */
		i = ctrl->ssize * ctrl->nsamples;
		datum = ((unsigned char *)block->data)[i - 1];
		for (i = 0; i < zgp_nout; i++)
			gpio_set_value(zgp_out[i], datum & (1 << i));
	}
	return 1; /* done */
}

/* Similarly, this inputs a cset. Again, currently one channel only */
static int zgp_input(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;
	struct zio_control *ctrl;
	uint8_t datum;
	int i, j;

	cset_for_each(cset, chan) {
		block = chan->active_block;
		if (!block)
			continue;
		ctrl = zio_get_ctrl(block);
		/* fill the whole block */
		for (j = 0; j < ctrl->nsamples; j++) {
			datum = 0;
			for (i = 0; i < zgp_nin; i++)
				datum |= gpio_get_value(zgp_in[i]) << i;
			((unsigned char *)block->data)[j] = datum;
		}
	}
	return 1; /* done */
}

static struct zio_device_operations zgp_d_op = {
	.output_cset =		zgp_output,
	.input_cset =		zgp_input,
};

static struct zio_cset zgp_cset[] = {
	{
		.n_chan =	1,
		.ssize =	1,
		.flags =	ZIO_DIR_OUTPUT | ZCSET_TYPE_ANALOG,
	},
	{
		.n_chan =	1,
		.ssize =	1,
		.flags =	ZIO_DIR_INPUT | ZCSET_TYPE_ANALOG,
	},
};
static struct zio_device zgp_dev = {
	.owner =		THIS_MODULE,
	.d_op =			&zgp_d_op,
	.cset =			zgp_cset,
	.n_cset =		ARRAY_SIZE(zgp_cset),

};

static int __init zgp_init(void)
{
	int i, err;

	/* The above code assumes we have one one-byte sized data */
	BUILD_BUG_ON(ZGPIO_NOUT > 8);
	BUILD_BUG_ON(ZGPIO_NIN > 8);

	if (zgp_nout == 0) {
		pr_err(KBUILD_MODNAME ": please pass out= gpio list\n");
		return -ENODEV;
	}

	for (i = 0; i < zgp_nout; i++) {
		err = gpio_request(zgp_out[i], "zio-gpio-out");
		if (err) {
			pr_err(KBUILD_MODNAME ": can't request gpio %i\n",
			       zgp_out[i]);
			goto out;
		}
	}

	for (i = 0; i < zgp_nin; i++) {
		err = gpio_request(zgp_in[i], "zio-gpio-in");
		if (err) {
			pr_err(KBUILD_MODNAME ": can't request gpio %i\n",
			       zgp_in[i]);
			goto out_input;
		}
	}

	if (zgp_trigger)
		zgp_dev.preferred_trigger = zgp_trigger;
	if (zgp_buffer)
		zgp_dev.preferred_buffer = zgp_buffer;
	err = zio_register_dev(&zgp_dev, "gpio");
	if (err) {
		pr_err(KBUILD_MODNAME ": can't register zio driver "
		       "(error %i)\n", err);
		goto out_input;
	}

	for (i = 0; i < zgp_nout; i++)
		gpio_direction_output(zgp_out[i], 0);
	for (i = 0; i < zgp_nin; i++)
		gpio_direction_input(zgp_in[i]);
	return 0;

out_input:
	/* i is one more than the last registered gpio */
	for (i--; i >= 0; i--)
		gpio_free(zgp_in[i]);
	i = zgp_nout;
out:
	/* i is one more than the last registered gpio */
	for (i--; i >= 0; i--)
		gpio_free(zgp_out[i]);
	return err;

}
static void __exit zgp_exit(void)
{
	int i;
	zio_unregister_dev(&zgp_dev);
	for (i = 0; i < zgp_nout; i++)
		gpio_free(zgp_out[i]);
	for (i = 0; i < zgp_nin; i++)
		gpio_free(zgp_in[i]);
}

module_init(zgp_init);
module_exit(zgp_exit);

MODULE_LICENSE("GPL");