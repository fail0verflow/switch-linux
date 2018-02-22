// SPDX-License-Identifier: GPL-2.0
/*
 * ROHM BH1730 ambient light sensor driver
 *
 * Copyright (c) 2018 Google, Inc.
 * Author: Pierre Bourdon <delroth@google.com>
 *
 * Based on a previous non-iio BH1730FVC driver:
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
 * Author: Won Huh <won.huh@samsung.com>
 *
 * Data sheets:
 *  http://www.rohm.com/web/global/datasheet/BH1730FVC/bh1730fvc-e
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/time.h>

#define BH1730_CMD_BIT BIT(7)

#define BH1730_REG_CONTROL	0x00
#define BH1730_REG_TIMING	0x01
#define BH1730_REG_INTERRUPT	0x02
#define BH1730_REG_THLLOW	0x03
#define BH1730_REG_THLHIGH	0x04
#define BH1730_REG_THHLOW	0x05
#define BH1730_REG_THHHIGH	0x06
#define BH1730_REG_GAIN		0x07
#define BH1730_REG_ID		0x12
#define BH1730_REG_DATA0LOW	0x14
#define BH1730_REG_DATA0HIGH	0x15
#define BH1730_REG_DATA1LOW	0x16
#define BH1730_REG_DATA1HIGH	0x17

#define BH1730_CONTROL_POWER_ON		BIT(0)
#define BH1730_CONTROL_MEASURE		BIT(1)

#define BH1730_INTERNAL_CLOCK_NS	2800ULL

#define BH1730_DEFAULT_INTEG_MS		150

enum bh1730_gain {
	BH1730_GAIN_1X = 0,
	BH1730_GAIN_2X,
	BH1730_GAIN_64X,
	BH1730_GAIN_128X,
};

struct bh1730_data {
	struct i2c_client *client;
	enum bh1730_gain gain;
	int itime;
};

static int bh1730_read_word(struct bh1730_data *bh1730, u8 reg)
{
	int ret = i2c_smbus_read_word_data(bh1730->client,
					   BH1730_CMD_BIT | reg);
	if (ret < 0)
		dev_err(&bh1730->client->dev,
			"i2c read failed error %d, register %01x\n",
			ret, reg);
	return ret;
}

static int bh1730_write(struct bh1730_data *bh1730, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(bh1730->client,
					    BH1730_CMD_BIT | reg,
					    val);
	if (ret < 0)
		dev_err(&bh1730->client->dev,
			"i2c write failed error %d, register %01x\n",
			ret, reg);
	return ret;
}

static int gain_setting_to_multiplier(enum bh1730_gain gain)
{
	switch (gain) {
	case BH1730_GAIN_1X:
		return 1;
	case BH1730_GAIN_2X:
		return 2;
	case BH1730_GAIN_64X:
		return 64;
	case BH1730_GAIN_128X:
		return 128;
	default:
		return -1;
	}
}

static int bh1730_gain_multiplier(struct bh1730_data *bh1730)
{
	int multiplier = gain_setting_to_multiplier(bh1730->gain);

	if (multiplier < 0) {
		dev_warn(&bh1730->client->dev,
			 "invalid gain multiplier settings: %d\n",
			 bh1730->gain);
		bh1730->gain = BH1730_GAIN_1X;
		multiplier = 1;
	}

	return multiplier;
}

static u64 bh1730_itime_ns(struct bh1730_data *bh1730)
{
	return BH1730_INTERNAL_CLOCK_NS * 964 * (256 - bh1730->itime);
}

static int bh1730_set_gain(struct bh1730_data *bh1730, enum bh1730_gain gain)
{
	int ret = bh1730_write(bh1730, BH1730_REG_GAIN, gain);

	if (ret < 0)
		return ret;

	bh1730->gain = gain;
	return 0;
}

static int bh1730_set_integration_time_ms(struct bh1730_data *bh1730,
					  int time_ms)
{
	int ret;
	u64 time_ns = time_ms * (u64)NSEC_PER_MSEC;
	u64 itime_step_ns = BH1730_INTERNAL_CLOCK_NS * 964;
	int itime = 256 - (int)DIV_ROUND_CLOSEST_ULL(time_ns, itime_step_ns);

	/* ITIME == 0 is reserved for manual integration mode. */
	if (itime <= 0 || itime > 255) {
		dev_warn(&bh1730->client->dev,
			 "integration time out of range: %dms\n", time_ms);
		return -ERANGE;
	}

	ret = bh1730_write(bh1730, BH1730_REG_TIMING, itime);
	if (ret < 0)
		return ret;

	bh1730->itime = itime;
	return 0;
}

static void bh1730_wait_for_next_measurement(struct bh1730_data *bh1730)
{
	ndelay(bh1730_itime_ns(bh1730) + BH1730_INTERNAL_CLOCK_NS * 714);
}

static int bh1730_adjust_gain(struct bh1730_data *bh1730)
{
	int visible, ir, highest, ret, i;

	visible = bh1730_read_word(bh1730, BH1730_REG_DATA0LOW);
	if (visible < 0)
		return visible;

	ir = bh1730_read_word(bh1730, BH1730_REG_DATA1LOW);
	if (ir < 0)
		return ir;

	highest = max(visible, ir);

	/*
	 * If the read value is being clamped, assume the worst and go to the
	 * lowest possible gain. The alternative is doing multiple
	 * recalibrations, which would be slower and have the same effect.
	 */
	if (highest == USHRT_MAX)
		highest *= 128;
	else
		highest = (highest * 128) / bh1730_gain_multiplier(bh1730);

	/*
	 * Find the lowest gain multiplier which puts the measured values
	 * above 1024. This threshold is chosen to match the gap between 2X
	 * multiplier and 64X (next available) while keeping some margin.
	 */
	for (i = BH1730_GAIN_1X; i < BH1730_GAIN_128X; ++i) {
		int adj = highest * gain_setting_to_multiplier(i) / 128;

		if (adj >= 1024)
			break;
	}

	if (i != bh1730->gain) {
		ret = bh1730_set_gain(bh1730, i);
		if (ret < 0)
			return ret;

		bh1730_wait_for_next_measurement(bh1730);
	}
	return 0;
}

static s64 bh1730_get_millilux(struct bh1730_data *bh1730)
{
	int visible, ir, visible_coef, ir_coef;
	u64 itime_us = bh1730_itime_ns(bh1730) / NSEC_PER_USEC;
	u64 millilux;

	visible = bh1730_read_word(bh1730, BH1730_REG_DATA0LOW);
	if (visible < 0)
		return visible;

	ir = bh1730_read_word(bh1730, BH1730_REG_DATA1LOW);
	if (ir < 0)
		return ir;

	if (ir * 1000 / visible < 500) {
		visible_coef = 5002;
		ir_coef = 7502;
	} else if (ir * 1000 / visible < 754) {
		visible_coef = 2250;
		ir_coef = 2000;
	} else if (ir * 1000 / visible < 1029) {
		visible_coef = 1999;
		ir_coef = 1667;
	} else if (ir * 1000 / visible < 1373) {
		visible_coef = 885;
		ir_coef = 583;
	} else if (ir * 1000 / visible < 1879) {
		visible_coef = 309;
		ir_coef = 165;
	} else {
		return 0;
	}

	millilux = (u64)USEC_PER_MSEC * (visible_coef * visible - ir_coef * ir);
	millilux /= bh1730_gain_multiplier(bh1730);
	millilux *= 103;
	millilux /= itime_us;
	return millilux;
}

static int bh1730_power_on(struct bh1730_data *bh1730)
{
	return bh1730_write(bh1730, BH1730_REG_CONTROL,
			    BH1730_CONTROL_POWER_ON | BH1730_CONTROL_MEASURE);
}

static int bh1730_set_defaults(struct bh1730_data *bh1730)
{
	int ret;

	ret = bh1730_set_gain(bh1730, BH1730_GAIN_1X);
	if (ret < 0)
		return ret;

	ret = bh1730_set_integration_time_ms(bh1730, BH1730_DEFAULT_INTEG_MS);
	if (ret < 0)
		return ret;

	bh1730_wait_for_next_measurement(bh1730);
	return 0;
}

static int bh1730_power_off(struct bh1730_data *bh1730)
{
	return bh1730_write(bh1730, BH1730_REG_CONTROL, 0);
}

static int bh1730_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bh1730_data *bh1730 = iio_priv(indio_dev);
	int ret;
	s64 millilux;

	ret = bh1730_adjust_gain(bh1730);
	if (ret < 0)
		return ret;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		millilux = bh1730_get_millilux(bh1730);
		if (millilux < 0)
			return millilux;
		*val = millilux / 1000;
		*val2 = (millilux % 1000) * 1000;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_RAW:
		switch (chan->channel2) {
		case IIO_MOD_LIGHT_CLEAR:
			ret = bh1730_read_word(bh1730, BH1730_REG_DATA0LOW);
			if (ret < 0)
				return ret;
			*val = ret;
			return IIO_VAL_INT;
		case IIO_MOD_LIGHT_IR:
			ret = bh1730_read_word(bh1730, BH1730_REG_DATA1LOW);
			if (ret < 0)
				return ret;
			*val = ret;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		*val = bh1730_gain_multiplier(bh1730);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info bh1730_info = {
	.read_raw = bh1730_read_raw,
};

static const struct iio_chan_spec bh1730_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_CLEAR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_IR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int bh1730_probe(struct i2c_client *client)
{
	struct bh1730_data *bh1730;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*bh1730));
	if (!indio_dev)
		return -ENOMEM;

	bh1730 = iio_priv(indio_dev);
	bh1730->client = client;
	i2c_set_clientdata(client, indio_dev);

	ret = bh1730_power_on(bh1730);
	if (ret < 0)
		return ret;

	ret = bh1730_set_defaults(bh1730);
	if (ret < 0)
		return ret;

	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &bh1730_info;
	indio_dev->name = "bh1730";
	indio_dev->channels = bh1730_channels;
	indio_dev->num_channels = ARRAY_SIZE(bh1730_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = iio_device_register(indio_dev);
	if (ret)
		goto out_power_off;
	return 0;

out_power_off:
	bh1730_power_off(bh1730);
	return ret;
}

static int bh1730_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct bh1730_data *bh1730 = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	return bh1730_power_off(bh1730);
}

#ifdef CONFIG_OF
static const struct of_device_id of_bh1730_match[] = {
	{ .compatible = "rohm,bh1730fvc" },
	{},
};
MODULE_DEVICE_TABLE(of, of_bh1730_match);
#endif

static struct i2c_driver bh1730_driver = {
	.probe_new = bh1730_probe,
	.remove = bh1730_remove,
	.driver = {
		.name = "bh1730",
		.of_match_table = of_match_ptr(of_bh1730_match),
	},
};
module_i2c_driver(bh1730_driver);

MODULE_AUTHOR("Pierre Bourdon <delroth@google.com>");
MODULE_DESCRIPTION("ROHM BH1730FVC driver");
MODULE_LICENSE("GPL v2");
