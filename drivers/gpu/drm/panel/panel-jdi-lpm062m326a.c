/*
 * Copyright (C) 2018 SwtcR <swtcr0@gmail.com>
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

struct jdi_panel {
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
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},
	{ 0xbd, 1,    { 0x00 }},
	{ 0xd9, 1,    { 0x06 }},
	{ 0xb9, 3,    { 0x00, 0x00, 0x00 }},
	{ 0x00, -1,   { 0x00, }},
};

static inline struct jdi_panel *to_jdi_panel(struct drm_panel *panel)
{
	return container_of(panel, struct jdi_panel, base);
}

static int jdi_panel_init(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	int ret;
	u8 display_id[3] = {0};
	struct init_cmd *init_cmds = NULL;

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

	switch (display_id[0]) {
		case 0x10:
			dev_info(&dsi->dev, "using init sequence for ID 0x10\n");
			init_cmds = init_cmds_0x10;
			break;
		default:
			dev_info(&dsi->dev, "unknown display, no extra init\n");
			break;
	}

	msleep(10);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	while (init_cmds && init_cmds->length != -1) {
		ret = mipi_dsi_dcs_write(dsi, init_cmds->cmd, init_cmds->data,
					 init_cmds->length);
		if (ret < 0)
			return ret;
		init_cmds++;
	}

	msleep(180);

	ret = mipi_dsi_dcs_set_column_address(dsi, 0,
				jdi->mode->hdisplay - 1);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_page_address(dsi, 0,
				jdi->mode->vdisplay - 1);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0)
		return ret;

// 	ret = mipi_dsi_dcs_set_address_mode(dsi, false, false, false,
// 			false, false, false, false, false);
// 	if (ret < 0)
// 		return ret;

	ret = mipi_dsi_dcs_set_pixel_format(dsi, MIPI_DCS_PIXEL_FMT_24BIT);
	if (ret < 0)
		return ret;

	return 0;
}

static int jdi_panel_on(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	msleep(20);

	return 0;
}

static int jdi_panel_off(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
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


static int jdi_panel_disable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);

	if (!jdi->enabled)
		return 0;

	if (jdi->backlight) {
		jdi->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(jdi->backlight);
	}

	jdi->enabled = false;

	return 0;
}

static int jdi_panel_unprepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	int ret;

	if (!jdi->prepared)
		return 0;

	ret = jdi_panel_off(jdi);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);
		return ret;
	}

	if (jdi->reset_gpio)
		gpiod_set_value(jdi->reset_gpio, 0);

	msleep(10);
	regulator_disable(jdi->supply2);
	msleep(10);
	regulator_disable(jdi->supply1);

	jdi->prepared = false;

	return 0;
}

static int jdi_panel_prepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	int ret;

	if (jdi->prepared)
		return 0;

	ret = regulator_enable(jdi->supply1);
	if (ret < 0)
		return ret;
	msleep(10);
	ret = regulator_enable(jdi->supply2);
	if (ret < 0)
		goto poweroff1;
	msleep(10);

	if (jdi->reset_gpio) {
		gpiod_set_value(jdi->reset_gpio, 0);
		msleep(10);
		gpiod_set_value(jdi->reset_gpio, 1);
		msleep(60);
	}

	ret = jdi_panel_init(jdi);
	if (ret < 0) {
		dev_err(panel->dev, "failed to init panel: %d\n", ret);
		goto reset;
	}

	ret = jdi_panel_on(jdi);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel on: %d\n", ret);
		goto reset;
	}

	jdi->prepared = true;

	return 0;

reset:
	if (jdi->reset_gpio)
		gpiod_set_value(jdi->reset_gpio, 0);
	regulator_disable(jdi->supply2);
poweroff1:
	regulator_disable(jdi->supply1);
	return ret;
}

static int jdi_panel_enable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);

	if (jdi->enabled)
		return 0;

	if (jdi->backlight) {
		jdi->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(jdi->backlight);
	}

	jdi->enabled = true;

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

static int jdi_panel_get_modes(struct drm_panel *panel)
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

static const struct drm_panel_funcs jdi_panel_funcs = {
	.disable = jdi_panel_disable,
	.unprepare = jdi_panel_unprepare,
	.prepare = jdi_panel_prepare,
	.enable = jdi_panel_enable,
	.get_modes = jdi_panel_get_modes,
};

static int jdi_panel_add(struct jdi_panel *jdi)
{
	struct device *dev = &jdi->dsi->dev;
	struct device_node *np;
	int ret;

	jdi->mode = &default_mode;

	jdi->supply1 = devm_regulator_get(dev, "vdd1");
	if (IS_ERR(jdi->supply1))
		return PTR_ERR(jdi->supply1);

	jdi->supply2 = devm_regulator_get(dev, "vdd2");
	if (IS_ERR(jdi->supply2))
		return PTR_ERR(jdi->supply2);

	jdi->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(jdi->reset_gpio));
		jdi->reset_gpio = NULL;
	} else {
		gpiod_set_value(jdi->reset_gpio, 0);
	}

	np = of_parse_phandle(dev->of_node, "backlight", 0);
	if (np) {
		jdi->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!jdi->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_init(&jdi->base);
	jdi->base.funcs = &jdi_panel_funcs;
	jdi->base.dev = &jdi->dsi->dev;

	ret = drm_panel_add(&jdi->base);
	if (ret < 0)
		goto put_backlight;

	return 0;

put_backlight:
	if (jdi->backlight)
		put_device(&jdi->backlight->dev);

	return ret;
}

static void jdi_panel_del(struct jdi_panel *jdi)
{
	if (jdi->base.dev)
		drm_panel_remove(&jdi->base);

	if (jdi->backlight)
		put_device(&jdi->backlight->dev);
}

static int jdi_panel_probe(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_CLOCK_NON_CONTINUOUS |
			MIPI_DSI_MODE_EOT_PACKET;

	jdi = devm_kzalloc(&dsi->dev, sizeof(*jdi), GFP_KERNEL);
	if (!jdi)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, jdi);

	jdi->dsi = dsi;

	ret = jdi_panel_add(jdi);
	if (ret < 0)
		return ret;

	return mipi_dsi_attach(dsi);
}

static int jdi_panel_remove(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = jdi_panel_disable(&jdi->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_detach(&jdi->base);
	jdi_panel_del(jdi);

	return 0;
}

static void jdi_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);

	jdi_panel_disable(&jdi->base);
}

static const struct of_device_id jdi_of_match[] = {
	{ .compatible = "jdi,lpm062m326a", },
	{ }
};
MODULE_DEVICE_TABLE(of, jdi_of_match);

static struct mipi_dsi_driver jdi_panel_driver = {
	.driver = {
		.name = "panel-jdi-lpm062m326a",
		.of_match_table = jdi_of_match,
	},
	.probe = jdi_panel_probe,
	.remove = jdi_panel_remove,
	.shutdown = jdi_panel_shutdown,
};
module_mipi_dsi_driver(jdi_panel_driver);

MODULE_AUTHOR("SwtcR <swtcr0@gmail.com>");
MODULE_DESCRIPTION("JDI LPM062M326A (720x1280) panel driver");
MODULE_LICENSE("GPL v2");
