// SPDX-License-Identifier: GPL-2.0+
/*
 * drivers/regulator/s2mpg1x-powermeter.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 */

#include <linux/mfd/samsung/s2mpg1x-meter.h>
#include <linux/mfd/samsung/s2mpg1x-register.h>
#include <linux/mfd/samsung/s2mpg10.h>
#include <linux/mfd/samsung/s2mpg11.h>
#include <linux/fs.h>
#include <linux/module.h>

#define SWITCH_ID_FUNC(id, ret, func, args...)                            \
	do {                                                              \
		switch (id) {                                             \
		case ID_S2MPG10:                                          \
			ret = s2mpg10_##func(args);                       \
			break;                                            \
		case ID_S2MPG11:                                          \
			ret = s2mpg11_##func(args);                       \
			break;                                            \
		}                                                         \
	} while (0)

static int s2mpg1x_update_reg(s2mpg1x_id_t id, struct i2c_client *i2c,
			      u8 reg, u8 val, u8 mask)
{
	int ret = -1;

	SWITCH_ID_FUNC(id, ret, update_reg, i2c, reg, val, mask);
	return ret;
}

static int s2mpg1x_read_reg(s2mpg1x_id_t id, struct i2c_client *i2c,
			    u8 reg, u8 *dest)
{
	int ret = -1;

	SWITCH_ID_FUNC(id, ret, read_reg, i2c, reg, dest);
	return ret;
}

int s2mpg1x_meter_set_async_blocking(s2mpg1x_id_t id, struct i2c_client *i2c,
				     unsigned long *jiffies_capture, u8 reg)
{
	u8 val = 0xFF;
	int ret;

	/* When 1 is written into ASYNC_RD bit, */
	/* transfer the accumulator data to readable registers->self-cleared */
	ret = s2mpg1x_update_reg(id, i2c, reg, ASYNC_RD_MASK, ASYNC_RD_MASK);
	if (ret != 0) {
		// Immediate return if failed
		return ret;
	}

	if (jiffies_capture)
		*jiffies_capture = jiffies;

	do {
		/* TODO: Expected 320 us before cleared reading */
		// usleep_range(ASYNC_MIN_SLEEP_US, ASYNC_MIN_SLEEP_US + 50);

		ret = s2mpg1x_read_reg(id, i2c, reg, &val);
	} while ((val & ASYNC_RD_MASK) != 0x00 && ret == 0);

	return ret;
}
EXPORT_SYMBOL_GPL(s2mpg1x_meter_set_async_blocking);

ssize_t s2mpg1x_format_meter_channel(char *buf, ssize_t count, int ch,
				     const char *name, const char *units,
				     u64 acc_data, u32 resolution,
				     u32 acc_count)
{
	// Note: resolution is in iq30
	// --> Convert it back to a human readable decimal value
	const u32 one_billion = 1000000000;
	u64 resolution_max = _IQ30_to_int((u64)resolution * one_billion);

	return scnprintf(buf + count, PAGE_SIZE - count,
			 "CH%d[%s]: 0x%016x * %d.%09lu / 0x%08x %s\n", ch, name,
			 acc_data, resolution_max / one_billion,
			 resolution_max % one_billion, acc_count, units);
}
EXPORT_SYMBOL_GPL(s2mpg1x_format_meter_channel);

MODULE_DESCRIPTION("SAMSUNG S2MPG1X Powermeter Driver");
MODULE_LICENSE("GPL");
