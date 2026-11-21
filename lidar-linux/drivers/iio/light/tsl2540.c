// SPDX-License-Identifier: GPL-2.0-only
/*
 * ams TSL2540 ambient light sensor
 *
 * Datasheet: https://look.ams-osram.com/m/d83ec5eb2896cf6/original/TSL2540-DS000564.pdf
 *
 * The device has a photopic (UV and IR blocked) visible channel and an IR
 * channel, each with a 16-bit ADC. Both share one integration time and one
 * analogue gain.
 *
 * The interrupt output and the ALS thresholds are not supported.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <asm/byteorder.h>

#include <linux/iio/iio.h>

#define TSL2540_REG_ENABLE		0x80
#define   TSL2540_ENABLE_PON		BIT(0)
#define   TSL2540_ENABLE_AEN		BIT(1)
#define TSL2540_REG_ATIME		0x81
#define TSL2540_REG_PERS		0x8c
#define TSL2540_REG_CFG1		0x90
#define TSL2540_REG_ID			0x92
#define   TSL2540_ID_MASK		GENMASK(7, 2)
#define   TSL2540_ID			0xe4
#define TSL2540_REG_STATUS		0x93
#define   TSL2540_STATUS_AINT		BIT(4)
#define TSL2540_REG_VISDATA		0x94
#define TSL2540_REG_IRDATA		0x96
#define TSL2540_REG_CFG2		0x9f
#define   TSL2540_CFG2_AGAINL		BIT(2)
#define   TSL2540_CFG2_AGAINMAX		BIT(4)
#define TSL2540_REG_INTENAB		0xdd

/* Time from power-on until the device accepts I2C commands is 1.5 ms */
#define TSL2540_POWER_ON_US		2000

/* One count in ATIME is 2.81 ms; ATIME holds the number of steps minus one */
#define TSL2540_STEP_US			2810
#define TSL2540_STEPS_MAX		256
/* Shortest integration time that reaches the full 16-bit range */
#define TSL2540_STEPS_DEFAULT		64

/*
 * The datasheet does not give a lux equation. It specifies a typical visible
 * channel responsivity of 363 counts/(uW/cm^2) for a 2700 K white LED at 64x
 * gain and 36 integration steps. 1 uW/cm^2 is 0.01 W/m^2, which at 300 lm/W
 * for such a source is 3 lux, so one count is 3 / 363 * 64 * 36 = 19.04 lux
 * at 1x gain and one step, for a device in open air. Anything in front of the
 * sensor needs its own correction on top of this.
 */
#define TSL2540_NANOLUX_PER_COUNT	19041000000ULL

struct tsl2540_gain {
	u16 gain_x2;
	u8 cfg1;
	u8 cfg2;
};

/* Recommended settings from the datasheet, "AGAIN Range" */
static const struct tsl2540_gain tsl2540_gains[] = {
	{ 1, 0, 0 },
	{ 2, 0, TSL2540_CFG2_AGAINL },
	{ 8, 1, TSL2540_CFG2_AGAINL },
	{ 32, 2, TSL2540_CFG2_AGAINL },
	{ 128, 3, TSL2540_CFG2_AGAINL },
	{ 256, 3, TSL2540_CFG2_AGAINL | TSL2540_CFG2_AGAINMAX },
};

#define TSL2540_GAIN_DEFAULT		1	/* 1x */

struct tsl2540_data {
	struct regmap *regmap;
	/* Protects the configuration below and the sequences that change it */
	struct mutex lock;
	unsigned int gain;
	unsigned int steps;
	/* No measurement has completed since the configuration was written */
	bool settling;
	int scale_avail[ARRAY_SIZE(tsl2540_gains) * 2];
};

static const int tsl2540_int_time_avail[] = {
	0, TSL2540_STEP_US,
	0, TSL2540_STEP_US,
	0, TSL2540_STEPS_MAX * TSL2540_STEP_US,
};

static void tsl2540_scale(unsigned int gain, unsigned int steps,
			  int *val, int *val2)
{
	u64 nano = div_u64(2 * TSL2540_NANOLUX_PER_COUNT,
			   tsl2540_gains[gain].gain_x2 * steps);
	u32 rem;

	*val = div_u64_rem(nano, NANO, &rem);
	*val2 = rem;
}

static int tsl2540_configure(struct tsl2540_data *data, unsigned int gain,
			     unsigned int steps)
{
	struct regmap *regmap = data->regmap;
	unsigned int i;
	int ret;

	/* Until the new configuration is complete, no sample is current */
	data->settling = true;

	/* The configuration has to be in place before ALS is enabled */
	ret = regmap_write(regmap, TSL2540_REG_ENABLE, 0);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TSL2540_REG_INTENAB, 0);
	if (ret)
		return ret;

	/* Flag the end of every ALS cycle in STATUS.AINT */
	ret = regmap_write(regmap, TSL2540_REG_PERS, 0);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TSL2540_REG_ATIME, steps - 1);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TSL2540_REG_CFG1, tsl2540_gains[gain].cfg1);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, TSL2540_REG_CFG2,
				 TSL2540_CFG2_AGAINL | TSL2540_CFG2_AGAINMAX,
				 tsl2540_gains[gain].cfg2);
	if (ret)
		return ret;

	/* From here the chip runs with the new gain and integration time */
	data->gain = gain;
	data->steps = steps;
	for (i = 0; i < ARRAY_SIZE(tsl2540_gains); i++)
		tsl2540_scale(i, steps, &data->scale_avail[2 * i],
			      &data->scale_avail[2 * i + 1]);

	/* Setting both bits in one write runs autozero before the first cycle */
	ret = regmap_write(regmap, TSL2540_REG_ENABLE,
			   TSL2540_ENABLE_PON | TSL2540_ENABLE_AEN);
	if (ret)
		return ret;

	/*
	 * Drop the flag of a cycle that ran with the previous configuration.
	 * Should the first new cycle already be over, the next one is awaited.
	 */
	ret = regmap_write(regmap, TSL2540_REG_STATUS, TSL2540_STATUS_AINT);
	if (ret)
		return ret;

	return 0;
}

static int tsl2540_read_channel(struct tsl2540_data *data, unsigned int reg,
				int *val)
{
	unsigned int cycle_us = data->steps * TSL2540_STEP_US;
	unsigned int status;
	__le16 buf;
	int ret;

	if (data->settling) {
		ret = regmap_read_poll_timeout(data->regmap, TSL2540_REG_STATUS,
					       status,
					       status & TSL2540_STATUS_AINT,
					       5 * USEC_PER_MSEC,
					       2 * cycle_us + 100 * USEC_PER_MSEC);
		if (ret)
			return ret;

		data->settling = false;
	}

	ret = regmap_bulk_read(data->regmap, reg, &buf, sizeof(buf));
	if (ret)
		return ret;

	*val = le16_to_cpu(buf);

	return 0;
}

static int tsl2540_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct tsl2540_data *data = iio_priv(indio_dev);
	int ret;

	guard(mutex)(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = tsl2540_read_channel(data, chan->address, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		tsl2540_scale(data->gain, data->steps, val, val2);
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0;
		*val2 = data->steps * TSL2540_STEP_US;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int tsl2540_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct tsl2540_data *data = iio_priv(indio_dev);
	unsigned int steps;
	unsigned int i;

	guard(mutex)(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		for (i = 0; i < ARRAY_SIZE(tsl2540_gains); i++) {
			if (val == data->scale_avail[2 * i] &&
			    val2 == data->scale_avail[2 * i + 1])
				return tsl2540_configure(data, i, data->steps);
		}

		return -EINVAL;
	case IIO_CHAN_INFO_INT_TIME:
		if (val || val2 < 0)
			return -EINVAL;

		steps = DIV_ROUND_CLOSEST(val2, TSL2540_STEP_US);
		if (steps < 1 || steps > TSL2540_STEPS_MAX)
			return -EINVAL;

		/* The gain is kept, so the scale changes with the time */
		return tsl2540_configure(data, data->gain, steps);
	default:
		return -EINVAL;
	}
}

static int tsl2540_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_INT_TIME:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int tsl2540_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	struct tsl2540_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*vals = data->scale_avail;
		*type = IIO_VAL_INT_PLUS_NANO;
		*length = ARRAY_SIZE(data->scale_avail);
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_INT_TIME:
		*vals = tsl2540_int_time_avail;
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(tsl2540_int_time_avail);
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}

static const struct iio_info tsl2540_info = {
	.read_raw = tsl2540_read_raw,
	.write_raw = tsl2540_write_raw,
	.write_raw_get_fmt = tsl2540_write_raw_get_fmt,
	.read_avail = tsl2540_read_avail,
};

/*
 * The gain is set through the illuminance scale and applies to the IR
 * channel as well, which is in arbitrary units.
 */
static const struct iio_chan_spec tsl2540_channels[] = {
	{
		.type = IIO_LIGHT,
		.address = TSL2540_REG_VISDATA,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_INT_TIME),
	},
	{
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_IR,
		.address = TSL2540_REG_IRDATA,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),
		.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_INT_TIME),
	},
};

static const struct regmap_config tsl2540_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TSL2540_REG_INTENAB,
};

static void tsl2540_power_down(void *regmap)
{
	regmap_write(regmap, TSL2540_REG_ENABLE, 0);
}

static int tsl2540_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tsl2540_data *data;
	struct iio_dev *indio_dev;
	unsigned int id;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, data);

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd\n");

	fsleep(TSL2540_POWER_ON_US);

	data->regmap = devm_regmap_init_i2c(client, &tsl2540_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "failed to initialise regmap\n");

	ret = regmap_read(data->regmap, TSL2540_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");

	if ((id & TSL2540_ID_MASK) != TSL2540_ID)
		dev_info(dev, "unexpected device ID 0x%02x\n", id);

	ret = devm_add_action_or_reset(dev, tsl2540_power_down, data->regmap);
	if (ret)
		return ret;

	ret = tsl2540_configure(data, TSL2540_GAIN_DEFAULT,
				TSL2540_STEPS_DEFAULT);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure device\n");

	indio_dev->name = "tsl2540";
	indio_dev->info = &tsl2540_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = tsl2540_channels;
	indio_dev->num_channels = ARRAY_SIZE(tsl2540_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static int tsl2540_suspend(struct device *dev)
{
	struct tsl2540_data *data = dev_get_drvdata(dev);

	guard(mutex)(&data->lock);

	return regmap_write(data->regmap, TSL2540_REG_ENABLE, 0);
}

static int tsl2540_resume(struct device *dev)
{
	struct tsl2540_data *data = dev_get_drvdata(dev);

	guard(mutex)(&data->lock);

	return tsl2540_configure(data, data->gain, data->steps);
}

static DEFINE_SIMPLE_DEV_PM_OPS(tsl2540_pm_ops, tsl2540_suspend,
				tsl2540_resume);

static const struct i2c_device_id tsl2540_id[] = {
	{ "tsl2540" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tsl2540_id);

static const struct of_device_id tsl2540_of_match[] = {
	{ .compatible = "ams,tsl2540" },
	{ }
};
MODULE_DEVICE_TABLE(of, tsl2540_of_match);

static struct i2c_driver tsl2540_driver = {
	.driver = {
		.name = "tsl2540",
		.of_match_table = tsl2540_of_match,
		.pm = pm_sleep_ptr(&tsl2540_pm_ops),
	},
	.probe = tsl2540_probe,
	.id_table = tsl2540_id,
};
module_i2c_driver(tsl2540_driver);

MODULE_DESCRIPTION("ams TSL2540 ambient light sensor driver");
MODULE_LICENSE("GPL");
