// SPDX-License-Identifier: GPL-2.0
/*
 * nv_imx585.c - imx585 sensor driver
 *
 * Copyright (c) 2016-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Copyright (c) 2026, UAB Kurokesu. All rights reserved.
 */

/* #define DEBUG */

#include <nvidia/conftest.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>
#include "imx585_mode_tbls.h"

/* No chip ID register, presence check uses standby register power-on default */
#define IMX585_STANDBY_DEFAULT (0x01)

/* Delay between XCLR release and the moment sensor accepts I2C transactions */
#define IMX585_XCLR_MIN_DELAY_US (500000)
#define IMX585_XCLR_DELAY_RANGE_US (1000)

/* Internal regulator stabilisation after stream start */
#define IMX585_STREAM_DELAY_US (25000)
#define IMX585_STREAM_DELAY_RANGE_US (1000)

/* Timing limits */
#define IMX585_FRAME_LENGTH_MIN (2250)
#define IMX585_FRAME_LENGTH_MAX (0xFFFFF)
#define IMX585_FRAME_LENGTH_DEFAULT (2250)
#define IMX585_SHR_MIN (8)
#define IMX585_COARSE_TIME_MIN (2)

/* Analog gain: 0.3 dB per register step, 72 dB max */
#define IMX585_ANALOG_GAIN_MAX (240)

/* Test patterns */
#define IMX585_TPG_PAT_000 0x00
#define IMX585_TPG_PAT_FFF 0x01
#define IMX585_TPG_PAT_555 0x02
#define IMX585_TPG_PAT_AAA 0x03
#define IMX585_TPG_PAT_H_COLOR_BARS 0x0A
#define IMX585_TPG_PAT_V_COLOR_BARS 0x0B

static const u8 imx585_tpg_val[] = {
	IMX585_TPG_PAT_000,	     IMX585_TPG_PAT_FFF,
	IMX585_TPG_PAT_555,	     IMX585_TPG_PAT_AAA,
	IMX585_TPG_PAT_H_COLOR_BARS, IMX585_TPG_PAT_V_COLOR_BARS,
};

static const struct of_device_id imx585_of_match[] = {
	{ .compatible = "sony,imx585" },
	{},
};

MODULE_DEVICE_TABLE(of, imx585_of_match);

static int test_mode;
module_param(test_mode, int, 0644);
MODULE_PARM_DESC(
	test_mode,
	"Test pattern: 0=off 1=000 2=FFF 3=555 4=AAA 5=H-bars 6=V-bars");

static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct imx585 {
	struct i2c_client *i2c_client;
	struct v4l2_subdev *subdev;
	struct camera_common_data *s_data;
	struct tegracam_device *tc_dev;
	u32 frame_length;
};

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static inline int imx585_read_reg(struct camera_common_data *s_data, u16 addr,
				  u8 *val)
{
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(s_data->regmap, addr, &reg_val);
	*val = reg_val & 0xFF;

	return err;
}

static inline int imx585_write_reg(struct camera_common_data *s_data, u16 addr,
				   u8 val)
{
	int err = 0;

	err = regmap_write(s_data->regmap, addr, val);
	if (err)
		dev_err(s_data->dev, "%s: i2c write failed, 0x%x = %x",
			__func__, addr, val);

	return err;
}

static int imx585_write_table(struct imx585 *priv, const imx585_reg table[])
{
	int err = 0;

	dev_dbg(priv->s_data->dev, "%s: Writing register table\n", __func__);

	err = regmap_util_write_table_8(priv->s_data->regmap, table, NULL, 0,
					IMX585_TABLE_WAIT_MS, IMX585_TABLE_END);

	if (err) {
		dev_err(priv->s_data->dev, "%s: Failed to write table (%d)\n",
			__func__, err);
	} else {
		dev_dbg(priv->s_data->dev,
			"%s: Register table written successfully\n", __func__);
	}

	return err;
}

static inline void imx585_get_vmax_regs(imx585_reg *regs, u32 vmax)
{
	regs->addr = IMX585_REG_VMAX_MSB;
	regs->val = (vmax >> 16) & 0x0F;

	(regs + 1)->addr = IMX585_REG_VMAX_MID;
	(regs + 1)->val = (vmax >> 8) & 0xFF;

	(regs + 2)->addr = IMX585_REG_VMAX_LSB;
	(regs + 2)->val = vmax & 0xFF;
}

static inline void imx585_get_shr_regs(imx585_reg *regs, u32 shr)
{
	regs->addr = IMX585_REG_SHR_MSB;
	regs->val = (shr >> 16) & 0x0F;

	(regs + 1)->addr = IMX585_REG_SHR_MID;
	(regs + 1)->val = (shr >> 8) & 0xFF;

	(regs + 2)->addr = IMX585_REG_SHR_LSB;
	(regs + 2)->val = shr & 0xFF;
}

static inline void imx585_get_gain_regs(imx585_reg *regs, u16 gain)
{
	regs->addr = IMX585_REG_ANALOG_GAIN_MSB;
	regs->val = (gain >> 8) & 0x07;

	(regs + 1)->addr = IMX585_REG_ANALOG_GAIN_LSB;
	(regs + 1)->val = gain & 0xFF;
}

static int imx585_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	struct device *dev = tc_dev->dev;
	imx585_reg reg_list[2];
	u16 gain;
	int err, i;

	if (val < mode->control_properties.min_gain_val)
		val = mode->control_properties.min_gain_val;
	else if (val > mode->control_properties.max_gain_val)
		val = mode->control_properties.max_gain_val;

	/* val is dB * gain_factor (10), register steps are 0.3 dB */
	gain = (u16)(val / 3);
	if (gain > IMX585_ANALOG_GAIN_MAX)
		gain = IMX585_ANALOG_GAIN_MAX;

	dev_dbg(dev, "%s: val: %lld, gain_reg: %u\n", __func__, val, gain);

	imx585_get_gain_regs(reg_list, gain);

	for (i = 0; i < ARRAY_SIZE(reg_list); i++) {
		err = imx585_write_reg(s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err) {
			dev_dbg(dev, "%s: gain write error\n", __func__);
			return err;
		}
	}

	return err;
}

static int imx585_set_coarse_time(struct imx585 *priv, s64 val)
{
	struct camera_common_data *s_data = priv->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	struct device *dev = priv->tc_dev->dev;
	imx585_reg reg_list[3];
	u32 coarse_time;
	u32 shr;
	int err, i;

	if (mode->control_properties.exposure_factor == 0 ||
	    mode->image_properties.line_length == 0) {
		dev_err(dev, "%s: line_len=%d, exposure_factor=%d\n", __func__,
			mode->image_properties.line_length,
			mode->control_properties.exposure_factor);
		return -EINVAL;
	}

	coarse_time = DIV_ROUND_CLOSEST(
		mode->signal_properties.pixel_clock.val * val /
			mode->image_properties.line_length,
		mode->control_properties.exposure_factor);

	if (priv->frame_length == 0)
		priv->frame_length = IMX585_FRAME_LENGTH_DEFAULT;

	/* SHR = VMAX - coarse_time, clamped so that SHR >= IMX585_SHR_MIN */
	if (coarse_time > (priv->frame_length - IMX585_SHR_MIN))
		coarse_time = priv->frame_length - IMX585_SHR_MIN;
	if (coarse_time < IMX585_COARSE_TIME_MIN)
		coarse_time = IMX585_COARSE_TIME_MIN;

	/* SHR must be even */
	shr = (priv->frame_length - coarse_time) & ~1U;
	if (shr < IMX585_SHR_MIN)
		shr = IMX585_SHR_MIN;

	dev_dbg(dev, "%s: coarse_time:%u, SHR:%u, FL:%u\n", __func__,
		coarse_time, shr, priv->frame_length);

	imx585_get_shr_regs(reg_list, shr);

	for (i = 0; i < ARRAY_SIZE(reg_list); i++) {
		err = imx585_write_reg(s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err) {
			dev_dbg(dev, "%s: SHR write error\n", __func__);
			return err;
		}
	}

	return 0;
}

static int imx585_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct imx585 *priv = (struct imx585 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	int err;

	dev_dbg(dev, "%s: val: %lld\n", __func__, val);

	err = imx585_set_coarse_time(priv, val);
	if (err)
		dev_dbg(dev, "%s: error setting exposure\n", __func__);

	return err;
}

static int imx585_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct imx585 *priv = (struct imx585 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	imx585_reg vmax_regs[3];
	u32 vmax;
	int err, i;

	if (val == 0 || mode->image_properties.line_length == 0)
		return -EINVAL;

	vmax = mode->signal_properties.pixel_clock.val *
	       mode->control_properties.framerate_factor /
	       mode->image_properties.line_length / val;

	if (vmax < IMX585_FRAME_LENGTH_MIN)
		vmax = IMX585_FRAME_LENGTH_MIN;
	else if (vmax > IMX585_FRAME_LENGTH_MAX)
		vmax = IMX585_FRAME_LENGTH_MAX;

	/* VMAX must be even */
	vmax &= ~1U;

	dev_dbg(dev, "%s: val: %llde-6 [fps], vmax: %u [lines]\n", __func__,
		val, vmax);

	imx585_get_vmax_regs(vmax_regs, vmax);
	for (i = 0; i < 3; i++) {
		err = imx585_write_reg(s_data, vmax_regs[i].addr,
				       vmax_regs[i].val);
		if (err) {
			dev_err(dev, "%s: VMAX write error\n", __func__);
			return err;
		}
	}

	priv->frame_length = vmax;

	return err;
}

static int imx585_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	int err = 0;

	err = imx585_write_reg(s_data, IMX585_REG_REGHOLD, val);
	if (err)
		dev_dbg(dev, "%s: Group hold control error\n", __func__);

	return err;
}

static struct tegracam_ctrl_ops imx585_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = imx585_set_gain,
	.set_exposure = imx585_set_exposure,
	.set_frame_rate = imx585_set_frame_rate,
	.set_group_hold = imx585_set_group_hold,
};

static int imx585_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s\n", __func__);
	if (pdata && pdata->power_on) {
		err = pdata->power_on(pw);
		if (err)
			dev_err(dev, "%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	if (pw->reset_gpio) {
		if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
			gpio_set_value_cansleep(pw->reset_gpio, 0);
		else
			gpio_set_value(pw->reset_gpio, 0);
	}

	if (unlikely(!(pw->avdd || pw->iovdd || pw->dvdd)))
		goto skip_power_seqn;

	usleep_range(10, 20);

	if (pw->avdd) {
		err = regulator_enable(pw->avdd);
		if (err)
			goto imx585_avdd_fail;
	}

	if (pw->iovdd) {
		err = regulator_enable(pw->iovdd);
		if (err)
			goto imx585_iovdd_fail;
	}

	if (pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (err)
			goto imx585_dvdd_fail;
	}

	usleep_range(10, 20);

skip_power_seqn:
	if (pw->reset_gpio) {
		if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
			gpio_set_value_cansleep(pw->reset_gpio, 1);
		else
			gpio_set_value(pw->reset_gpio, 1);
	}

	/* Sensor is not I2C-ready until long after XCLR release */
	usleep_range(IMX585_XCLR_MIN_DELAY_US,
		     IMX585_XCLR_MIN_DELAY_US + IMX585_XCLR_DELAY_RANGE_US);

	pw->state = SWITCH_ON;

	return 0;

imx585_dvdd_fail:
	regulator_disable(pw->iovdd);

imx585_iovdd_fail:
	regulator_disable(pw->avdd);

imx585_avdd_fail:
	dev_err(dev, "%s failed.\n", __func__);

	return -ENODEV;
}

static int imx585_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s\n", __func__);

	if (pdata && pdata->power_off) {
		err = pdata->power_off(pw);
		if (err) {
			dev_err(dev, "%s failed.\n", __func__);
			return err;
		}
	} else {
		if (pw->reset_gpio) {
			if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
				gpio_set_value_cansleep(pw->reset_gpio, 0);
			else
				gpio_set_value(pw->reset_gpio, 0);
		}

		usleep_range(10, 10);

		if (pw->dvdd)
			regulator_disable(pw->dvdd);
		if (pw->iovdd)
			regulator_disable(pw->iovdd);
		if (pw->avdd)
			regulator_disable(pw->avdd);
	}

	pw->state = SWITCH_OFF;

	return 0;
}

static int imx585_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (likely(pw->dvdd))
		devm_regulator_put(pw->dvdd);

	if (likely(pw->avdd))
		devm_regulator_put(pw->avdd);

	if (likely(pw->iovdd))
		devm_regulator_put(pw->iovdd);

	pw->dvdd = NULL;
	pw->avdd = NULL;
	pw->iovdd = NULL;

	if (likely(pw->reset_gpio))
		gpio_free(pw->reset_gpio);

	return 0;
}

static int imx585_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct clk *parent;
	int err = 0;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}

	/* Sensor MCLK (aka. INCK) */
	if (pdata->mclk_name) {
		pw->mclk = devm_clk_get(dev, pdata->mclk_name);
		if (IS_ERR(pw->mclk)) {
			dev_err(dev, "unable to get clock %s\n",
				pdata->mclk_name);
			return PTR_ERR(pw->mclk);
		}

		if (pdata->parentclk_name) {
			parent = devm_clk_get(dev, pdata->parentclk_name);
			if (IS_ERR(parent)) {
				dev_err(dev, "unable to get parent clock %s",
					pdata->parentclk_name);
			} else
				clk_set_parent(pw->mclk, parent);
		}
	}

	/* analog 3.3v */
	if (pdata->regulators.avdd)
		err |= camera_common_regulator_get(dev, &pw->avdd,
						   pdata->regulators.avdd);
	/* IO 1.8v */
	if (pdata->regulators.iovdd)
		err |= camera_common_regulator_get(dev, &pw->iovdd,
						   pdata->regulators.iovdd);
	/* dig 1.1v */
	if (pdata->regulators.dvdd)
		err |= camera_common_regulator_get(dev, &pw->dvdd,
						   pdata->regulators.dvdd);
	if (err) {
		dev_err(dev, "%s: unable to get regulator(s)\n", __func__);
		goto done;
	}

	/* Reset or ENABLE GPIO */
	pw->reset_gpio = pdata->reset_gpio;
	err = gpio_request(pw->reset_gpio, "cam_reset_gpio");
	if (err < 0) {
		dev_err(dev, "%s: unable to request reset_gpio (%d)\n",
			__func__, err);
		goto done;
	}

done:
	pw->state = SWITCH_OFF;

	return err;
}

static struct camera_common_pdata *
imx585_parse_dt(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	struct camera_common_pdata *ret = NULL;
	int err = 0;
	int gpio;

	if (!np)
		return NULL;

	match = of_match_device(imx585_of_match, dev);
	if (!match) {
		dev_err(dev, "failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata =
		devm_kzalloc(dev, sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio < 0) {
		if (gpio == -EPROBE_DEFER)
			ret = ERR_PTR(-EPROBE_DEFER);
		dev_err(dev, "reset-gpios not found\n");
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	err = of_property_read_string(np, "mclk", &board_priv_pdata->mclk_name);
	if (err)
		dev_dbg(dev,
			"mclk name not present, assume sensor driven externally\n");

	err = of_property_read_string(np, "avdd-reg",
				      &board_priv_pdata->regulators.avdd);
	err |= of_property_read_string(np, "iovdd-reg",
				       &board_priv_pdata->regulators.iovdd);
	err |= of_property_read_string(np, "dvdd-reg",
				       &board_priv_pdata->regulators.dvdd);
	if (err)
		dev_dbg(dev,
			"avdd, iovdd and/or dvdd reglrs. not present, assume sensor powered independently\n");

	board_priv_pdata->has_eeprom = of_property_read_bool(np, "has-eeprom");

	return board_priv_pdata;

error:
	devm_kfree(dev, board_priv_pdata);

	return ret;
}

static int imx585_set_mode(struct tegracam_device *tc_dev)
{
	struct imx585 *priv = (struct imx585 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	int mode_ix = s_data->mode;
	int err = 0;

	if (mode_ix < 0 || mode_ix >= IMX585_MODE_COMMON) {
		dev_err(tc_dev->dev, "%s: invalid mode %d\n", __func__,
			mode_ix);
		return -EINVAL;
	}

	dev_dbg(tc_dev->dev, "%s: mode %d (%ux%u)\n", __func__, mode_ix,
		imx585_frmfmt[mode_ix].size.width,
		imx585_frmfmt[mode_ix].size.height);

	err = imx585_write_table(priv, mode_table[IMX585_MODE_COMMON]);
	if (err)
		return err;

	err = imx585_write_table(priv, mode_table[mode_ix]);
	if (err)
		return err;

	priv->frame_length = IMX585_FRAME_LENGTH_DEFAULT;

	return 0;
}

static int imx585_start_streaming(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	int err = 0;

	dev_dbg(tc_dev->dev, "%s:\n", __func__);

	if (test_mode) {
		dev_dbg(tc_dev->dev, "test pattern mode %d\n", test_mode);

		err = imx585_write_reg(s_data, IMX585_REG_TPG_PATSEL,
				       imx585_tpg_val[test_mode - 1]);
		if (err)
			return err;

		err = imx585_write_reg(s_data, IMX585_REG_TPG_EN_DUOUT, 0x01);
		if (err)
			return err;

		err = imx585_write_reg(s_data, IMX585_REG_TPG_TESTCLKEN, 0x0A);
		if (err)
			return err;
	}

	err = imx585_write_reg(s_data, IMX585_REG_XMSTA, 0x00);
	if (err)
		return err;

	err = imx585_write_reg(s_data, IMX585_REG_STANDBY,
			       IMX585_MODE_STREAMING);
	if (err)
		return err;

	/* Internal regulator stabilisation */
	usleep_range(IMX585_STREAM_DELAY_US,
		     IMX585_STREAM_DELAY_US + IMX585_STREAM_DELAY_RANGE_US);

	return err;
}

static int imx585_stop_streaming(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	int err = 0;

	dev_dbg(tc_dev->dev, "%s:\n", __func__);
	err = imx585_write_reg(s_data, IMX585_REG_STANDBY, IMX585_MODE_STANDBY);

	return err;
}

static struct camera_common_sensor_ops imx585_common_ops = {
	.numfrmfmts = ARRAY_SIZE(imx585_frmfmt),
	.frmfmt_table = imx585_frmfmt,
	.power_on = imx585_power_on,
	.power_off = imx585_power_off,
	.write_reg = imx585_write_reg,
	.read_reg = imx585_read_reg,
	.parse_dt = imx585_parse_dt,
	.power_get = imx585_power_get,
	.power_put = imx585_power_put,
	.set_mode = imx585_set_mode,
	.start_streaming = imx585_start_streaming,
	.stop_streaming = imx585_stop_streaming,
};

static int imx585_board_setup(struct imx585 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct device *dev = s_data->dev;
	u8 reg_val;
	int err = 0;

	/* Skip mclk enable as this camera module has an on-board oscillator */

	err = imx585_power_on(s_data);
	if (err) {
		dev_err(dev, "error during power on sensor (%d)\n", err);
		goto done;
	}

	err = imx585_read_reg(s_data, IMX585_REG_STANDBY, &reg_val);
	if (err) {
		dev_err(dev, "%s: error during i2c read probe (%d)\n", __func__,
			err);
		goto err_reg_probe;
	}

	if (reg_val != IMX585_STANDBY_DEFAULT) {
		dev_err(dev,
			"%s: unexpected standby reg value: 0x%02x (expected 0x%02x)\n",
			__func__, reg_val, IMX585_STANDBY_DEFAULT);
		err = -ENODEV;
		goto err_reg_probe;
	}

err_reg_probe:
	imx585_power_off(s_data);

done:
	return err;
}

static int imx585_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);

	return 0;
}

static const struct v4l2_subdev_internal_ops imx585_subdev_internal_ops = {
	.open = imx585_open,
};

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int imx585_probe(struct i2c_client *client)
#else
static int imx585_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct imx585 *priv;
	int err = 0;

	dev_dbg(dev, "probing v4l2 sensor at addr 0x%0x\n", client->addr);

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(struct imx585), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	priv->i2c_client = tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, "imx585", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &sensor_regmap_config;
	tc_dev->sensor_ops = &imx585_common_ops;
	tc_dev->v4l2sd_internal_ops = &imx585_subdev_internal_ops;
	tc_dev->tcctrl_ops = &imx585_ctrl_ops;

	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	priv->subdev = &tc_dev->s_data->subdev;
	tegracam_set_privdata(tc_dev, (void *)priv);

	err = imx585_board_setup(priv);
	if (err) {
		dev_err(dev, "board setup failed\n");
		return err;
	}

	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "tegra camera subdev registration failed\n");
		return err;
	}

	dev_dbg(dev, "detected imx585 sensor\n");

	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int imx585_remove(struct i2c_client *client)
#else
static void imx585_remove(struct i2c_client *client)
#endif
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct imx585 *priv;

	if (!s_data) {
		dev_err(&client->dev, "camera common data is NULL\n");
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
		return -EINVAL;
#else
		return;
#endif
	}
	priv = (struct imx585 *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
	return 0;
#endif
}

static const struct i2c_device_id imx585_id[] = { { "imx585", 0 }, {} };

MODULE_DEVICE_TABLE(i2c, imx585_id);

static struct i2c_driver imx585_i2c_driver = {
	.driver = {
		.name = "imx585",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(imx585_of_match),
	},
	.probe = imx585_probe,
	.remove = imx585_remove,
	.id_table = imx585_id,
};

module_i2c_driver(imx585_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for Sony IMX585");
MODULE_AUTHOR("UAB Kurokesu");
MODULE_LICENSE("GPL v2");
