/*
 * Copyright (C) 2017 SwtcR <swtcr0.gmail.com>
 *
 * Based on Sharp ls043t1le01 panel driver by Werner Johansson <werner.johansson@sonymobile.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct nintendo_switch_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct backlight_device *backlight;
	struct regulator *supply1;
	struct regulator *supply2;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

struct init_cmd {
	u8 cmd;
	int length;
	u8 data[0x40];
};

struct init_cmd init_cmds_0x10[] = {
	{ 0xb9, 3,    { 0xff, 0x83, 0x94 }},
	{ 0xbd, 1,    { 0x00 }},
	{ 0xd8, 0x18, { 0xaa, 0xaa, 0xaa, 0xeb, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0xeb, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0xeb, 0xaa, 0xaa,
			0xaa, 0xaa, 0xaa, 0xeb, 0xaa, 0xaa }},
	{ 0xbd, 1,    { 0x01 }},
	{ 0xd8, 0x26, { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},
	{ 0xbd, 1,    { 0x02 }},
	{ 0xd8, 0xe,  { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},
	{ 0xbd, 1,    { 0x00 }},
	{ 0xd9, 1,    { 0x06 }},
	{ 0xb9, 3,    { 0x00, 0x00, 0x00 }},
	{ 0x00, -1,   { 0x00, }},
};

static inline struct nintendo_switch_panel *to_nintendo_switch_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nintendo_switch_panel, base);
}

static int nintendo_switch_panel_init(struct nintendo_switch_panel *nintendo_switch)
{
	struct mipi_dsi_device *dsi = nintendo_switch->dsi;
	int ret;
	u8 display_id[3] = {0};

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_set_maximum_return_packet_size(dsi, 3);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_DISPLAY_ID, display_id,
		sizeof(display_id));
	if (ret < 0) {
		dev_err(&dsi->dev, "failed to read panel ID: %d\n", ret);
	} else {
		dev_info(&dsi->dev, "display ID[%d]: %02x %02x %02x\n",
			ret, display_id[0], display_id[1], display_id[2]);
	}

	struct init_cmd *init_cmds = NULL;

	switch (display_id[0]) {
		case 0x10:
			dev_info(&dsi->dev, "using init sequence for ID 0x10\n");
			break;
		default:
			dev_info(&dsi->dev, "unknown display, no extra init\n");
			break;
	}

	while (init_cmds && init_cmds->length != -1) {
		ret = mipi_dsi_dcs_write(dsi, init_cmds->cmd, init_cmds->data,
					 init_cmds->length);
		if (ret < 0)
			return ret;
		init_cmds++;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	msleep(180);

	return 0;
}

static int nintendo_switch_panel_on(struct nintendo_switch_panel *nintendo_switch)
{
	struct mipi_dsi_device *dsi = nintendo_switch->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	msleep(20);

	return 0;
}

static int nintendo_switch_panel_off(struct nintendo_switch_panel *nintendo_switch)
{
	struct mipi_dsi_device *dsi = nintendo_switch->dsi;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	return 0;
}


static int nintendo_switch_panel_disable(struct drm_panel *panel)
{
	struct nintendo_switch_panel *nintendo_switch = to_nintendo_switch_panel(panel);

	if (!nintendo_switch->enabled)
		return 0;

	if (nintendo_switch->backlight) {
		nintendo_switch->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(nintendo_switch->backlight);
	}

	nintendo_switch->enabled = false;

	return 0;
}

static int nintendo_switch_panel_unprepare(struct drm_panel *panel)
{
	struct nintendo_switch_panel *nintendo_switch = to_nintendo_switch_panel(panel);
	int ret;

	if (!nintendo_switch->prepared)
		return 0;

	ret = nintendo_switch_panel_off(nintendo_switch);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);
		return ret;
	}

	if (nintendo_switch->reset_gpio)
		gpiod_set_value(nintendo_switch->reset_gpio, 0);

	msleep(10);
	regulator_disable(nintendo_switch->supply2);
	msleep(10);
	regulator_disable(nintendo_switch->supply1);

	nintendo_switch->prepared = false;

	return 0;
}

static int nintendo_switch_panel_prepare(struct drm_panel *panel)
{
	struct nintendo_switch_panel *nintendo_switch = to_nintendo_switch_panel(panel);
	int ret;

	if (nintendo_switch->prepared)
		return 0;

	ret = regulator_enable(nintendo_switch->supply1);
	if (ret < 0)
		return ret;
	msleep(10);
	ret = regulator_enable(nintendo_switch->supply2);
	if (ret < 0)
		goto poweroff1;
	msleep(10);

	if (nintendo_switch->reset_gpio) {
		gpiod_set_value(nintendo_switch->reset_gpio, 0);
		msleep(10);
		gpiod_set_value(nintendo_switch->reset_gpio, 1);
		msleep(60);
	}

	ret = nintendo_switch_panel_init(nintendo_switch);
	if (ret < 0) {
		dev_err(panel->dev, "failed to init panel: %d\n", ret);
		goto reset;
	}

	ret = nintendo_switch_panel_on(nintendo_switch);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel on: %d\n", ret);
		goto reset;
	}

	nintendo_switch->prepared = true;

	return 0;

reset:
	if (nintendo_switch->reset_gpio)
		gpiod_set_value(nintendo_switch->reset_gpio, 0);
	regulator_disable(nintendo_switch->supply2);
poweroff1:
	regulator_disable(nintendo_switch->supply1);
	return ret;
}

static int nintendo_switch_panel_enable(struct drm_panel *panel)
{
	struct nintendo_switch_panel *nintendo_switch = to_nintendo_switch_panel(panel);

	if (nintendo_switch->enabled)
		return 0;

	if (nintendo_switch->backlight) {
		nintendo_switch->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(nintendo_switch->backlight);
	}

	nintendo_switch->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 78000,
	.hdisplay = 720,
	.hsync_start = 720 + 136,
	.hsync_end = 720 + 136 + 72,
	.htotal = 720 + 136 + 72 + 72,
	.vdisplay = 1280,
	.vsync_start = 1280 + 10,
	.vsync_end = 1280 + 10 + 2,
	.vtotal = 1280 + 10 + 1 + 9,
	.vrefresh = 60,
};

static int nintendo_switch_panel_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
				default_mode.hdisplay, default_mode.vdisplay,
				default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = 77;
	panel->connector->display_info.height_mm = 137;

	return 1;
}

static const struct drm_panel_funcs nintendo_switch_panel_funcs = {
	.disable = nintendo_switch_panel_disable,
	.unprepare = nintendo_switch_panel_unprepare,
	.prepare = nintendo_switch_panel_prepare,
	.enable = nintendo_switch_panel_enable,
	.get_modes = nintendo_switch_panel_get_modes,
};

static int nintendo_switch_panel_add(struct nintendo_switch_panel *nintendo_switch)
{
	struct device *dev = &nintendo_switch->dsi->dev;
	struct device_node *np;
	int ret;

	nintendo_switch->mode = &default_mode;

	nintendo_switch->supply1 = devm_regulator_get(dev, "vdd1");
	if (IS_ERR(nintendo_switch->supply1))
		return PTR_ERR(nintendo_switch->supply1);

	nintendo_switch->supply2 = devm_regulator_get(dev, "vdd2");
	if (IS_ERR(nintendo_switch->supply2))
		return PTR_ERR(nintendo_switch->supply2);

	nintendo_switch->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(nintendo_switch->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(nintendo_switch->reset_gpio));
		nintendo_switch->reset_gpio = NULL;
	} else {
		gpiod_set_value(nintendo_switch->reset_gpio, 0);
	}

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (np) {
		nintendo_switch->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!nintendo_switch->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&nintendo_switch->base);
	nintendo_switch->base.funcs = &nintendo_switch_panel_funcs;
	nintendo_switch->base.dev = &nintendo_switch->dsi->dev;

	ret = drm_panel_add(&nintendo_switch->base);
	if (ret < 0)
		goto put_backlight;

	return 0;

put_backlight:
	if (nintendo_switch->backlight)
		put_device(&nintendo_switch->backlight->dev);

	return ret;
}

static void nintendo_switch_panel_del(struct nintendo_switch_panel *nintendo_switch)
{
	if (nintendo_switch->base.dev)
		drm_panel_remove(&nintendo_switch->base);

	if (nintendo_switch->backlight)
		put_device(&nintendo_switch->backlight->dev);
}

static int nintendo_switch_panel_probe(struct mipi_dsi_device *dsi)
{
	struct nintendo_switch_panel *nintendo_switch;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_CLOCK_NON_CONTINUOUS;

	nintendo_switch = devm_kzalloc(&dsi->dev, sizeof(*nintendo_switch), GFP_KERNEL);
	if (!nintendo_switch)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, nintendo_switch);

	nintendo_switch->dsi = dsi;

	ret = nintendo_switch_panel_add(nintendo_switch);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int nintendo_switch_panel_remove(struct mipi_dsi_device *dsi)
{
	struct nintendo_switch_panel *nintendo_switch = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = nintendo_switch_panel_disable(&nintendo_switch->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_detach(&nintendo_switch->base);
	nintendo_switch_panel_del(nintendo_switch);

	return 0;
}

static void nintendo_switch_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct nintendo_switch_panel *nintendo_switch = mipi_dsi_get_drvdata(dsi);

	nintendo_switch_panel_disable(&nintendo_switch->base);
}

static const struct of_device_id nintendo_switch_of_match[] = {
	{ .compatible = "nintendo,lpm062m326a", },
	{ }
};
MODULE_DEVICE_TABLE(of, nintendo_switch_of_match);

static struct mipi_dsi_driver nintendo_switch_panel_driver = {
	.driver = {
		.name = "panel-nintendo-lpm062m326a",
		.of_match_table = nintendo_switch_of_match,
	},
	.probe = nintendo_switch_panel_probe,
	.remove = nintendo_switch_panel_remove,
	.shutdown = nintendo_switch_panel_shutdown,
};
module_mipi_dsi_driver(nintendo_switch_panel_driver);

MODULE_AUTHOR("SwtcR <swtcr0@gmail.com>");
MODULE_DESCRIPTION("Nintendo Switch LPM062M326A (720x1280) panel driver");
MODULE_LICENSE("GPL v2");
