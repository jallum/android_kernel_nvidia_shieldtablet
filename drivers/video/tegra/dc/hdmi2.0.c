/*
 * drivers/video/tegra/dc/hdmi2.0.c
 *
 * Copyright (c) 2014, NVIDIA CORPORATION, All rights reserved.
 * Author: Animesh Kishore <ankishore@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/clk/tegra.h>
#include <linux/nvhost.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif

#include <mach/dc.h>
#include <mach/hdmi-audio.h>
#include <mach/fb.h>

#include "dc_reg.h"
#include "dc_priv.h"
#include "sor.h"
#include "sor_regs.h"
#include "edid.h"
#include "hdmi2.0.h"
#include "hdmihdcp.h"
#include "dpaux.h"

#include "../../../../arch/arm/mach-tegra/iomap.h"

static struct tegra_hdmi *dc_hdmi;

static void tegra_clk_writel(u32 val, u32 mask, u32 offset)
{
	u32 temp = readl(IO_ADDRESS(0x60006000 + offset));

	temp &= ~(mask);
	temp |= val;
	writel(temp, IO_ADDRESS(0x60006000 + offset));
}

static int tegra_hdmi_controller_enable(struct tegra_hdmi *hdmi);
static void tegra_hdmi_config_clk(struct tegra_hdmi *hdmi, u32 clk_type);
static long tegra_dc_hdmi_setup_clk(struct tegra_dc *dc, struct clk *clk);

static inline void tegra_hdmi_irq_enable(struct tegra_hdmi *hdmi)
{
	if (tegra_platform_is_fpga())
		return;

	enable_irq(hdmi->irq);
}

static inline void tegra_hdmi_irq_disable(struct tegra_hdmi *hdmi)
{
	if (tegra_platform_is_fpga())
		return;

	disable_irq(hdmi->irq);
}

static inline bool tegra_hdmi_hpd_asserted(struct tegra_hdmi *hdmi)
{
	return tegra_dc_hpd(hdmi->dc);
}

static inline void tegra_hdmi_reset(struct tegra_hdmi *hdmi)
{
	tegra_periph_reset_assert(hdmi->sor->sor_clk);
	mdelay(20);
	tegra_periph_reset_deassert(hdmi->sor->sor_clk);
	mdelay(20);
}

static void tegra_hdmi_get_eld_header(struct tegra_hdmi *hdmi,
					u8 *eld_mem_block)
{
	struct tegra_edid_hdmi_eld *eld = &hdmi->eld;

	eld->baseline_len = HDMI_ELD_MONITOR_NAME_STR + eld->mnl +
				eld->sad_count * 3 - HDMI_ELD_CEA_EDID_VER_MNL;

	eld_mem_block[HDMI_ELD_VER] = eld->eld_ver << 3;
	eld_mem_block[HDMI_ELD_BASELINE_ELD_LEN] =
			DIV_ROUND_UP(eld->baseline_len, 4);

}

static void tegra_hdmi_get_eld_baseline(struct tegra_hdmi *hdmi,
						u8 *eld_mem_block)
{
	struct tegra_edid_hdmi_eld *eld = &hdmi->eld;
	u8 tmp;

	tmp = eld->mnl | (eld->cea_edid_ver << 5);
	eld_mem_block[HDMI_ELD_CEA_EDID_VER_MNL] = tmp;

	tmp = eld->support_hdcp | (eld->support_ai << 1) |
		(eld->conn_type << 2) | (eld->sad_count << 4);
	eld_mem_block[HDMI_ELD_SAD_CNT_CON_TYPE_S_AI_S_HDCP] = tmp;

	eld_mem_block[HDMI_ELD_AUDIO_SYNC_DELAY] = eld->aud_synch_delay;

	eld_mem_block[HDMI_ELD_RLRC_FLRC_RC_RLR_FC_LFE_FLR] = eld->spk_alloc;

	memcpy(&eld_mem_block[HDMI_ELD_PORT_ID], eld->port_id, 8);

	memcpy(&eld_mem_block[HDMI_ELD_MANUFACTURER_NAME],
					eld->manufacture_id, 2);

	memcpy(&eld_mem_block[HDMI_ELD_PRODUCT_CODE], eld->product_id, 2);

	memcpy(&eld_mem_block[HDMI_ELD_MONITOR_NAME_STR],
				eld->monitor_name, eld->mnl);

	memcpy(&eld_mem_block[HDMI_ELD_MONITOR_NAME_STR + eld->mnl],
						eld->sad, eld->sad_count * 3);
}

static void tegra_hdmi_get_eld_vendor(struct tegra_hdmi *hdmi,
						u8 *eld_mem_block)
{
	struct tegra_edid_hdmi_eld *eld = &hdmi->eld;
	u32 vendor_block_index = 4 + eld->baseline_len; /* 4 byte header */

	if (!eld->baseline_len)
		dev_err(&hdmi->dc->ndev->dev,
			"hdm: eld baseline length not populated\n");

	memset(&eld_mem_block[vendor_block_index], 0,
		HDMI_ELD_BUF - vendor_block_index + 1);
}

static int tegra_hdmi_eld_config(struct tegra_hdmi *hdmi)
{
	u8 *eld_mem;
	int cnt;

	eld_mem = devm_kzalloc(&hdmi->dc->ndev->dev,
				HDMI_ELD_BUF, GFP_KERNEL);
	if (!eld_mem) {
		dev_warn(&hdmi->dc->ndev->dev,
			"hdmi: eld memory allocation failed\n");
		return -ENOMEM;
	}

	tegra_hdmi_get_eld_header(hdmi, eld_mem);
	tegra_hdmi_get_eld_baseline(hdmi, eld_mem);
	tegra_hdmi_get_eld_vendor(hdmi, eld_mem);

	for (cnt = 0; cnt < HDMI_ELD_BUF; cnt++)
		tegra_sor_writel(hdmi->sor, NV_SOR_AUDIO_HDA_ELD_BUFWR,
				NV_SOR_AUDIO_HDA_ELD_BUFWR_INDEX(cnt) |
				NV_SOR_AUDIO_HDA_ELD_BUFWR_DATA(eld_mem[cnt]));

	devm_kfree(&hdmi->dc->ndev->dev, eld_mem);
	return 0;
}

int tegra_hdmi_setup_hda_presence(void)
{
	struct tegra_hdmi *hdmi = dc_hdmi;
	struct tegra_dc *dc = hdmi->dc;

	if (!hdmi)
		return -EAGAIN;

	if (hdmi->enabled && hdmi->eld_valid) {
		tegra_dc_unpowergate_locked(dc);
		tegra_dc_io_start(dc);

		/* remove hda presence while setting up eld */
		tegra_sor_writel(hdmi->sor, NV_SOR_AUDIO_HDA_PRESENCE, 0);

		tegra_hdmi_eld_config(hdmi);
		tegra_sor_writel(hdmi->sor, NV_SOR_AUDIO_HDA_PRESENCE,
				NV_SOR_AUDIO_HDA_PRESENCE_ELDV(1) |
				NV_SOR_AUDIO_HDA_PRESENCE_PD(1));

		tegra_dc_io_end(dc);
		tegra_dc_powergate_locked(dc);

		return 0;
	}

	return -ENODEV;
}

static int tegra_hdmi_ddc_i2c_xfer(struct tegra_dc *dc,
					struct i2c_msg *msgs, int num)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	return i2c_transfer(hdmi->ddc_i2c_client->adapter, msgs, num);
}

static int tegra_hdmi_ddc_init(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	struct i2c_adapter *i2c_adap;
	int err = 0;
	struct i2c_board_info i2c_dev_info = {
		.type = "tegra_hdmi2.0",
		.addr = 0x50,
	};

	hdmi->edid = tegra_edid_create(dc, tegra_hdmi_ddc_i2c_xfer);
	if (IS_ERR_OR_NULL(hdmi->edid)) {
		dev_err(&dc->ndev->dev, "hdmi: can't create edid\n");
		return PTR_ERR(hdmi->edid);
	}
	tegra_dc_set_edid(dc, hdmi->edid);

	i2c_adap = i2c_get_adapter(dc->out->ddc_bus);
	if (!i2c_adap) {
		dev_err(&dc->ndev->dev,
			"hdmi: can't get adpater for ddc bus %d\n",
			dc->out->ddc_bus);
		err = -EBUSY;
		goto fail_edid_free;
	}

	hdmi->ddc_i2c_client = i2c_new_device(i2c_adap, &i2c_dev_info);
	i2c_put_adapter(i2c_adap);
	if (!hdmi->ddc_i2c_client) {
		dev_err(&dc->ndev->dev, "hdmi: can't create new i2c device\n");
		err = -EBUSY;
		goto fail_edid_free;
	}

	return 0;
fail_edid_free:
	tegra_edid_destroy(hdmi->edid);
	return err;
}

static void tegra_hdmi_hda_clk_enable(struct tegra_hdmi *hdmi)
{
	clk_prepare_enable(hdmi->hda_clk);
	clk_prepare_enable(hdmi->hda2codec_clk);
	clk_prepare_enable(hdmi->hda2hdmi_clk);
}

static void tegra_hdmi_hda_clk_disable(struct tegra_hdmi *hdmi)
{
	clk_disable_unprepare(hdmi->hda2hdmi_clk);
	clk_disable_unprepare(hdmi->hda2codec_clk);
	clk_disable_unprepare(hdmi->hda_clk);
}

static int tegra_hdmi_hda_clk_get(struct tegra_hdmi *hdmi)
{
	int err;
	struct tegra_dc *dc = hdmi->dc;

	hdmi->hda_clk = clk_get_sys("tegra30-hda", "hda");
	if (IS_ERR_OR_NULL(hdmi->hda_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't get hda clock\n");
		err = -ENOENT;
		goto err_put_clock;
	}

	hdmi->hda2codec_clk = clk_get_sys("tegra30-hda", "hda2codec");
	if (IS_ERR_OR_NULL(hdmi->hda_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't get hda clock\n");
		err = -ENOENT;
		goto err_put_clock;
	}

	hdmi->hda2hdmi_clk = clk_get_sys("tegra30-hda", "hda2hdmi");
	if (IS_ERR_OR_NULL(hdmi->hda_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: can't get hda clock\n");
		err = -ENOENT;
		goto err_put_clock;
	}

	return 0;
err_put_clock:
	if (!IS_ERR_OR_NULL(hdmi->hda2hdmi_clk))
		clk_put(hdmi->hda2hdmi_clk);
	if (!IS_ERR_OR_NULL(hdmi->hda2codec_clk))
		clk_put(hdmi->hda2codec_clk);
	if (!IS_ERR_OR_NULL(hdmi->hda_clk))
		clk_put(hdmi->hda_clk);
	return err;
}

static bool tegra_hdmi_check_dc_constraint(const struct fb_videomode *mode)
{
	return (mode->hsync_len >= 1) && (mode->vsync_len >= 1) &&
		(mode->lower_margin + mode->vsync_len +
		mode->upper_margin > 1) &&
		(mode->xres >= 16) && (mode->yres >= 16);
}

__maybe_unused
static bool tegra_hdmi_fb_mode_filter(const struct tegra_dc *dc,
					struct fb_videomode *mode)
{
	if (!mode->pixclock)
		return false;

	if (mode->xres > 4096)
		return false;

	if (mode->pixclock &&
		mode->pixclock > tegra_dc_get_out_max_pixclock(dc))
		return false;

	if (!tegra_hdmi_check_dc_constraint(mode))
		return false;

	return true;
}

static inline void tegra_hdmi_ddc_enable(struct tegra_hdmi *hdmi)
{
	/*
	 * hdmi uses i2c lane muxed on dpaux1 pad.
	 * Enable dpaux1 pads and configure the mux.
	 */
	tegra_dpaux_config_pad_mode(hdmi->dc, TEGRA_DPAUX_INSTANCE_1,
					TEGRA_DPAUX_PAD_MODE_I2C);
}

static inline void tegra_hdmi_ddc_disable(struct tegra_hdmi *hdmi)
{
	/*
	 * hdmi uses i2c lane muxed on dpaux1 pad.
	 * Disable dpaux1 pads.
	 */
	tegra_dpaux_pad_power(hdmi->dc, TEGRA_DPAUX_INSTANCE_1, false);
}

static int tegra_hdmi_get_mon_spec(struct tegra_hdmi *hdmi)
{
#define MAX_RETRY 5
#define MIN_RETRY_DELAY_US 200
#define MAX_RETRY_DELAY_US (MIN_RETRY_DELAY_US + 200)

	size_t attempt_cnt = 0;
	int err = 0;

	if (IS_ERR_OR_NULL(hdmi->edid)) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: edid not initialized\n");
		return PTR_ERR(hdmi->edid);
	}

	hdmi->mon_spec_valid = false;
	if (hdmi->mon_spec_valid)
		fb_destroy_modedb(hdmi->mon_spec.modedb);
	memset(&hdmi->mon_spec, 0, sizeof(hdmi->mon_spec));

	do {
		err = tegra_edid_get_monspecs(hdmi->edid, &hdmi->mon_spec);
		(err < 0) ?
		({usleep_range(MIN_RETRY_DELAY_US, MAX_RETRY_DELAY_US); }) :
		({break; });
	} while (++attempt_cnt < MAX_RETRY);

	if (err < 0) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: edid read failed\n");
		return err;
	}

	hdmi->mon_spec_valid = true;
	return 0;

#undef MAX_RETRY_DELAY_US
#undef MIN_RETRY_DELAY_US
#undef MAX_RETRY
}

static inline int tegra_hdmi_edid_read(struct tegra_hdmi *hdmi)
{
	return tegra_hdmi_get_mon_spec(hdmi);
}

static int tegra_hdmi_get_eld(struct tegra_hdmi *hdmi)
{
	int err;

	hdmi->eld_valid = false;
	memset(&hdmi->eld, 0, sizeof(hdmi->eld));

	err = tegra_edid_get_eld(hdmi->edid, &hdmi->eld);
	if (err < 0) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: eld not available\n");
		return err;
	}

	hdmi->eld_valid = true;
	return 0;
}

static inline int tegra_hdmi_eld_read(struct tegra_hdmi *hdmi)
{
	return tegra_hdmi_get_eld(hdmi);
}

static void tegra_hdmi_edid_config(struct tegra_hdmi *hdmi)
{
#define CM_TO_MM(x) (x * 10)

	struct tegra_dc *dc = hdmi->dc;

	if (!hdmi->mon_spec_valid)
		return;

	dc->out->h_size = CM_TO_MM(hdmi->mon_spec.max_x);
	dc->out->v_size = CM_TO_MM(hdmi->mon_spec.max_y);

#undef CM_TO_MM
}

static void tegra_hdmi_hotplug_notify(struct tegra_hdmi *hdmi,
					bool is_asserted)
{
	struct tegra_dc *dc = hdmi->dc;
	struct fb_monspecs *mon_spec;

	if (is_asserted)
		mon_spec = &hdmi->mon_spec;
	else
		mon_spec = NULL;

#ifdef CONFIG_ADF_TEGRA
	if (dc->adf)
		tegra_adf_process_hotplug_connected(hdmi->dc->adf, mon_spec);
#else
	if (dc->fb)
		tegra_fb_update_monspecs(hdmi->dc->fb, mon_spec, NULL);
#endif

	dc->connected = is_asserted;
	tegra_dc_ext_process_hotplug(dc->ndev->id);

#ifdef CONFIG_SWITCH
	switch_set_state(&hdmi->audio_switch, is_asserted ? 1 : 0);
#endif
}

static int tegra_hdmi_edid_eld_setup(struct tegra_hdmi *hdmi)
{
	int err;

	tegra_hdmi_ddc_enable(hdmi);

	err = tegra_hdmi_edid_read(hdmi);
	if (err < 0)
		goto fail;

	err = tegra_hdmi_eld_read(hdmi);
	if (err < 0)
		goto fail;

	tegra_hdmi_edid_config(hdmi);

	/*
	 * eld is configured when audio needs it
	 * via tegra_hdmi_edid_config()
	 */

	tegra_hdmi_hotplug_notify(hdmi, true);
	return 0;
fail:
	return err;
}

static int (*tegra_hdmi_plug_func[])(struct tegra_hdmi *) = {
	tegra_hdmi_edid_eld_setup,
};

enum tegra_hdmi_plug_states {
	TEGRA_EDID_ELD_SETUP,
	TEGRA_HDMI_MONITOR_ENABLE,
};

static int tegra_hdmi_plugged(struct tegra_hdmi *hdmi,
				enum tegra_hdmi_plug_states state_level)
{
	int err;

	if (state_level >= TEGRA_HDMI_MONITOR_ENABLE)
		return state_level;

	err = tegra_hdmi_plug_func[state_level](hdmi);

	/*
	 * If error return failing state level,
	 * otherwise goto next state level
	 */
	return (err < 0) ? state_level :
		tegra_hdmi_plugged(hdmi, state_level + 1);
}

static int tegra_hdmi_controller_disable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	struct tegra_dc *dc = hdmi->dc;

	tegra_dc_get(dc);

	tegra_dc_sor_detach(sor);
	tegra_sor_power_lanes(sor, 4, false);
	tegra_sor_hdmi_pad_power_down(sor);
	tegra_hdmi_hda_clk_disable(hdmi);
	tegra_nvhdcp_set_plug(hdmi->nvhdcp, 0);
	tegra_hdmi_reset(hdmi);
	tegra_sor_clk_disable(sor);

	tegra_dc_put(dc);

	return 0;
}

static int tegra_hdmi_disable(struct tegra_hdmi *hdmi)
{
	if (!hdmi->enabled)
		return 0;

	hdmi->enabled = false;
	hdmi->eld_valid = false;
	hdmi->mon_spec_valid = false;

	tegra_dc_disable(hdmi->dc);

	tegra_hdmi_hotplug_notify(hdmi, false);

	return 0;
}

static int (*tegra_hdmi_unplug_func[])(struct tegra_hdmi *) = {
	tegra_hdmi_disable,
};

enum tegra_hdmi_unplug_states {
	TEGRA_HDMI_DISABLE,
	TEGRA_HDMI_MONITOR_DISABLE,
};

static int tegra_hdmi_unplugged(struct tegra_hdmi *hdmi,
				enum tegra_hdmi_unplug_states state_level)
{
	int err;

	if (state_level >= TEGRA_HDMI_MONITOR_DISABLE)
		return state_level;

	err = tegra_hdmi_unplug_func[state_level](hdmi);

	/*
	 * If error return failing state level,
	 * otherwise goto next state level
	 */
	return (err < 0) ? state_level :
		tegra_hdmi_unplugged(hdmi, state_level + 1);
}

static void tegra_hdmi_hpd_worker(struct work_struct *work)
{
	struct tegra_hdmi *hdmi = container_of(to_delayed_work(work),
				struct tegra_hdmi, hpd_worker);
	int level;

	mutex_lock(&hdmi->hpd_lock);

	if (tegra_dc_hpd(hdmi->dc)) {
		level = tegra_hdmi_plugged(hdmi, TEGRA_EDID_ELD_SETUP);
		if (level >= TEGRA_HDMI_MONITOR_ENABLE)
			dev_info(&hdmi->dc->ndev->dev, "hdmi: plugged\n");
		else
			dev_info(&hdmi->dc->ndev->dev,
				"hdmi state %d failed during plug\n", level);
	} else {
		level = tegra_hdmi_unplugged(hdmi, TEGRA_HDMI_DISABLE);
		if (level >= TEGRA_HDMI_MONITOR_DISABLE)
			dev_info(&hdmi->dc->ndev->dev, "hdmi: unplugged\n");
		else
			dev_info(&hdmi->dc->ndev->dev,
				"hdmi state %d failed during unplug\n", level);
	}

	mutex_unlock(&hdmi->hpd_lock);
}

static irqreturn_t tegra_hdmi_hpd_irq_handler(int irq, void *ptr)
{
	struct tegra_dc *dc = ptr;
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	cancel_delayed_work(&hdmi->hpd_worker);
	schedule_delayed_work(&hdmi->hpd_worker,
				msecs_to_jiffies(HDMI_HPD_DEBOUNCE_DELAY_MS));

	return IRQ_HANDLED;
}

static int tegra_hdmi_hpd_init(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	int hotplug_gpio = dc->out->hotplug_gpio;
	int hotplug_irq;
	int err;

	if (!gpio_is_valid(hotplug_gpio)) {
		dev_err(&dc->ndev->dev, "hdmi: invalid hotplug gpio\n");
		return -EINVAL;
	}

	hotplug_irq = gpio_to_irq(hotplug_gpio);
	if (hotplug_irq < 0) {
		dev_err(&dc->ndev->dev,
			"hdmi: hotplug gpio to irq map failed\n");
		return -EINVAL;
	}

	err = gpio_request(hotplug_gpio, "hdmi2.0_hpd");
	if (err < 0)
		dev_err(&dc->ndev->dev,
			"hdmi: hpd gpio_request failed %d\n", err);
	gpio_direction_input(hotplug_gpio);

	err = request_threaded_irq(hotplug_irq,
				NULL, tegra_hdmi_hpd_irq_handler,
				(IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT),
				dev_name(&dc->ndev->dev), dc);
	if (err) {
		dev_err(&dc->ndev->dev,
			"hdmi: request_threaded_irq failed: %d\n", err);
		goto fail;
	}


	INIT_DELAYED_WORK(&hdmi->hpd_worker, tegra_hdmi_hpd_worker);

	mutex_init(&hdmi->hpd_lock);
	hdmi->irq = hotplug_irq;

	return 0;
fail:
	gpio_free(hotplug_gpio);
	return err;
}

static int tegra_dc_hdmi_init(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi;
	int err;

	hdmi = devm_kzalloc(&dc->ndev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->sor = tegra_dc_sor_init(dc, NULL);
	if (IS_ERR_OR_NULL(hdmi->sor)) {
		err = PTR_ERR(hdmi->sor);
		goto fail;
	}

	hdmi->pdata = dc->pdata->default_out->hdmi_out;
	hdmi->dc = dc;
	dc_hdmi = hdmi;
	hdmi->nvhdcp = NULL;
	hdmi->mon_spec_valid = false;
	hdmi->eld_valid = false;
	hdmi->enabled = false;

#ifdef CONFIG_TEGRA_HDMIHDCP
	hdmi->nvhdcp = tegra_nvhdcp_create(hdmi, dc->ndev->id,
			dc->out->ddc_bus);
	if (IS_ERR_OR_NULL(hdmi->nvhdcp)) {
		err = PTR_ERR(hdmi->nvhdcp);
		goto fail;
	}
#endif

	tegra_hdmi_hda_clk_get(hdmi);

	tegra_hdmi_ddc_init(hdmi);

	tegra_hdmi_hpd_init(hdmi);

	tegra_dc_set_outdata(dc, hdmi);

#ifdef CONFIG_SWITCH
	hdmi->audio_switch.name = "hdmi_audio";
	err = switch_dev_register(&hdmi->audio_switch);
	if (err)
		dev_err(&dc->ndev->dev,
			"hdmi: failed to register audio switch %d\n", err);
#endif

	return 0;
fail:
	devm_kfree(&dc->ndev->dev, hdmi);
	return err;
}

static void tegra_dc_hdmi_destroy(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	tegra_dc_sor_destroy(hdmi->sor);
	tegra_edid_destroy(hdmi->edid);
	tegra_nvhdcp_destroy(hdmi->nvhdcp);
	free_irq(gpio_to_irq(dc->out->hotplug_gpio), dc);
	gpio_free(dc->out->hotplug_gpio);
	devm_kfree(&dc->ndev->dev, hdmi);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&hdmi->audio_switch);
#endif
	clk_put(hdmi->hda_clk);
	clk_put(hdmi->hda2codec_clk);
	clk_put(hdmi->hda2hdmi_clk);
}

static void tegra_hdmi_config(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	struct tegra_dc *dc = hdmi->dc;
	u32 h_pulse_start, h_pulse_end;
	u32 dispclk_div_8_2;

	tegra_sor_write_field(sor, NV_SOR_INPUT_CONTROL,
			NV_SOR_INPUT_CONTROL_ARM_VIDEO_RANGE_LIMITED |
			NV_SOR_INPUT_CONTROL_HDMI_SRC_SELECT_DISPLAYB,
			NV_SOR_INPUT_CONTROL_ARM_VIDEO_RANGE_LIMITED |
			NV_SOR_INPUT_CONTROL_HDMI_SRC_SELECT_DISPLAYB);

	dispclk_div_8_2 = clk_get_rate(hdmi->sor->sor_clk) / 1000000 * 4;
	tegra_sor_writel(sor, NV_SOR_REFCLK,
			NV_SOR_REFCLK_DIV_INT(dispclk_div_8_2 >> 2) |
			NV_SOR_REFCLK_DIV_FRAC(dispclk_div_8_2));
	tegra_sor_writel(sor, NV_SOR_HDMI_CTRL, 0x40020038);

	tegra_dc_writel(dc, 0x180, DC_DISP_H_PULSE2_CONTROL);
	h_pulse_start = dc->mode.h_ref_to_sync +
					dc->mode.h_sync_width +
					dc->mode.h_back_porch - 10;
	h_pulse_end = h_pulse_start + 8;
	tegra_dc_writel(dc, PULSE_START(h_pulse_start) | PULSE_END(h_pulse_end),
		  DC_DISP_H_PULSE2_POSITION_A);
	tegra_dc_writel(dc, 0x1000, DC_DISP_DISP_SIGNAL_OPTIONS0);
}

static void tegra_hdmi_infoframe_pkt_write(struct tegra_hdmi *hdmi,
						u32 header_reg, u8 pkt_type,
						u8 pkt_vs, u8 pkt_len,
						void *reg_payload,
						u32 reg_payload_len)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	u32 val;
	u32 *data = reg_payload;
	u32 data_reg = header_reg + 1;

	val = NV_SOR_HDMI_INFOFRAME_HEADER_TYPE(pkt_type) |
		NV_SOR_HDMI_INFOFRAME_HEADER_VERSION(pkt_vs) |
		NV_SOR_HDMI_INFOFRAME_HEADER_LEN(pkt_len);
	tegra_sor_writel(sor, header_reg, val);

	for (val = 0; val < reg_payload_len; val += 4, data_reg++, data++)
		tegra_sor_writel(sor, data_reg, *data);
}

static int tegra_hdmi_find_cea_vic(const struct tegra_dc_mode *mode)
{
	struct fb_videomode m;
	unsigned i;
	unsigned best = 0;

	tegra_dc_to_fb_videomode(&m, mode);

	m.vmode &= ~FB_VMODE_STEREO_MASK; /* stereo modes have the same VICs */

	for (i = 1; i < CEA_MODEDB_SIZE; i++) {
		const struct fb_videomode *curr = &cea_modes[i];

		if (!fb_mode_is_equal(&m, curr))
			continue;

		if (!best)
			best = i;
		/* if either flag is set, then match is required */
		if (curr->flag & (FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9)) {
			if (m.flag & curr->flag & FB_FLAG_RATIO_4_3)
				best = i;
			else if (m.flag & curr->flag & FB_FLAG_RATIO_16_9)
				best = i;
		} else {
			best = i;
		}
	}
	return best;
}

static void tegra_hdmi_avi_infoframe_update(struct tegra_hdmi *hdmi)
{
	struct hdmi_avi_infoframe *avi = &hdmi->avi;

	avi->act_fmt_valid = 1;
	avi->act_format = HDMI_AVI_ACTIVE_FORMAT_SAME;

	avi->video_format = tegra_hdmi_find_cea_vic(&hdmi->dc->mode);
}

static void tegra_hdmi_avi_infoframe(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;

	/* disable avi infoframe before configuring */
	tegra_sor_writel(sor, NV_SOR_HDMI_AVI_INFOFRAME_CTRL, 0);

	tegra_hdmi_avi_infoframe_update(hdmi);

	tegra_hdmi_infoframe_pkt_write(hdmi, NV_SOR_HDMI_AVI_INFOFRAME_HEADER,
					HDMI_INFOFRAME_TYPE_AVI,
					HDMI_INFOFRAME_VS_AVI,
					HDMI_INFOFRAME_LEN_AVI,
					&hdmi->avi, sizeof(hdmi->avi));

	/* Send infoframe every frame, checksum hw generated */
	tegra_sor_writel(sor, NV_SOR_HDMI_AVI_INFOFRAME_CTRL,
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_ENABLE_YES |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_OTHER_DISABLE |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_SINGLE_DISABLE |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_CHECKSUM_ENABLE);
}

static void tegra_hdmi_audio_infoframe_update(struct tegra_hdmi *hdmi)
{
	hdmi->audio.channel_cnt = HDMI_AUDIO_CHANNEL_CNT_2;
}

static void tegra_hdmi_audio_infoframe(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;

	/* disable audio infoframe before configuring */
	tegra_sor_writel(sor, NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL, 0);

	tegra_hdmi_audio_infoframe_update(hdmi);

	tegra_hdmi_infoframe_pkt_write(hdmi, NV_SOR_HDMI_AUDIO_INFOFRAME_HEADER,
					HDMI_INFOFRAME_TYPE_AUDIO,
					HDMI_INFOFRAME_VS_AUDIO,
					HDMI_INFOFRAME_LEN_AUDIO,
					&hdmi->audio, sizeof(hdmi->audio));

	/* Send infoframe every frame, checksum hw generated */
	tegra_sor_writel(sor, NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL,
		NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL_ENABLE_YES |
		NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL_OTHER_DISABLE |
		NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL_SINGLE_DISABLE |
		NV_SOR_HDMI_AUDIO_INFOFRAME_CTRL_CHECKSUM_ENABLE);
}

/* HW generated CTS and N */
static void tegra_hdmi_audio_acr(struct tegra_hdmi *hdmi, u32 audio_freq)
{
#define GET_AVAL(n, fs_khz) ((24000 * n) / (128 * fs_khz))

	struct tegra_dc_sor_data *sor = hdmi->sor;
	u32 val;

	tegra_sor_writel(sor, NV_SOR_HDMI_ACR_CTRL, 0x0);

	val = NV_SOR_HDMI_SPARE_HW_CTS_ENABLE |
		NV_SOR_HDMI_SPARE_CTS_RESET_VAL(1) |
		NV_SOR_HDMI_SPARE_ACR_PRIORITY_HIGH;
	tegra_sor_writel(sor, NV_SOR_HDMI_SPARE, val);

	tegra_sor_writel(sor, NV_SOR_HDMI_ACR_0441_SUBPACK_LOW,
			NV_SOR_HDMI_ACR_SUBPACK_USE_HW_CTS);
	tegra_sor_writel(sor, NV_SOR_HDMI_ACR_0441_SUBPACK_HIGH,
			NV_SOR_HDMI_ACR_SUBPACK_ENABLE);

	val = NV_SOR_HDMI_AUDIO_N_RESET_ASSERT |
		NV_SOR_HDMI_AUDIO_N_LOOKUP_ENABLE;
	tegra_sor_writel(sor, NV_SOR_HDMI_AUDIO_N, val);

	/* N from table 7.1, 7.2, 7.3 hdmi spec v1.4 */
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_0320, 4096);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_0320,
			GET_AVAL(4096, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_0441, 6272);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_0441,
			GET_AVAL(6272, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_0882, 12544);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_0882,
			GET_AVAL(12544, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_1764, 25088);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_1764,
			GET_AVAL(25088, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_0480, 6144);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_0480,
			GET_AVAL(6144, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_0960, 12288);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_0960,
			GET_AVAL(12288, audio_freq));
	tegra_sor_writel(sor, NV_SOR_AUDIO_NVAL_1920, 24576);
	tegra_sor_writel(sor, NV_SOR_AUDIO_AVAL_1920,
			GET_AVAL(24576, audio_freq));

	tegra_sor_write_field(sor, NV_SOR_HDMI_AUDIO_N,
				NV_SOR_HDMI_AUDIO_N_RESET_ASSERT,
				NV_SOR_HDMI_AUDIO_N_RESET_DEASSERT);
#undef GET_AVAL
}

static void tegra_hdmi_audio_config(struct tegra_hdmi *hdmi,
				u32 audio_freq,
				u32 audio_src)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	u32 val;

	/* hda is the only audio source */
	val = NV_SOR_AUDIO_CTRL_AFIFO_FLUSH |
		NV_SOR_AUDIO_CTRL_SRC_HDA;
	if (hdmi->null_sample_inject)
		val |= NV_SOR_AUDIO_CTRL_NULL_SAMPLE_EN;
	tegra_sor_writel(sor, NV_SOR_AUDIO_CTRL, val);

	tegra_hdmi_audio_acr(hdmi, audio_freq);
	tegra_hdmi_audio_infoframe(hdmi);
}

int tegra_hdmi_setup_audio_freq_source(unsigned audio_freq,
					unsigned audio_source)
{
	struct tegra_hdmi *hdmi = dc_hdmi;
	bool valid_freq;

	if (!hdmi)
		return -ENODEV;

	valid_freq = AUDIO_FREQ_32K == audio_freq ||
			AUDIO_FREQ_44_1K == audio_freq ||
			AUDIO_FREQ_48K == audio_freq ||
			AUDIO_FREQ_88_2K == audio_freq ||
			AUDIO_FREQ_96K == audio_freq ||
			AUDIO_FREQ_176_4K == audio_freq ||
			AUDIO_FREQ_192K == audio_freq;
	if (valid_freq) {
		tegra_dc_io_start(hdmi->dc);
		tegra_hdmi_audio_config(hdmi, audio_freq, audio_source);
		tegra_dc_io_end(hdmi->dc);
		hdmi->audio_freq = audio_freq;
	} else {
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(tegra_hdmi_setup_audio_freq_source);

int tegra_hdmi_audio_null_sample_inject(bool on)
{
	struct tegra_hdmi *hdmi = dc_hdmi;

	if (!hdmi)
		return -ENODEV;

	if (on && !hdmi->null_sample_inject)
		tegra_sor_write_field(hdmi->sor,
					NV_SOR_AUDIO_CTRL,
					NV_SOR_AUDIO_CTRL_NULL_SAMPLE_EN,
					NV_SOR_AUDIO_CTRL_NULL_SAMPLE_EN);
	else if (!on && hdmi->null_sample_inject)
		tegra_sor_write_field(hdmi->sor,
					NV_SOR_AUDIO_CTRL,
					NV_SOR_AUDIO_CTRL_NULL_SAMPLE_EN,
					NV_SOR_AUDIO_CTRL_NULL_SAMPLE_DIS);

	hdmi->null_sample_inject = on;

	return 0;
}
EXPORT_SYMBOL(tegra_hdmi_audio_null_sample_inject);

static void tegra_hdmi_config_lanes(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;

	tegra_sor_writel(sor, NV_SOR_LANE_DRIVE_CURRENT(sor->portnum),
			0x22222222);
	tegra_sor_writel(sor, NV_SOR_PR(sor->portnum), 0x0);
}

static void tegra_hdmi_config_xbar(struct tegra_hdmi *hdmi)
{
	/* TODO: confirm with HW */
	tegra_sor_writel(hdmi->sor, NV_SOR_XBAR_CTRL, 0x8d111828);
}

static int tegra_hdmi_controller_enable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	struct tegra_dc_sor_data *sor = hdmi->sor;

	tegra_dc_io_start(dc);
	tegra_dc_get(dc);

	clk_prepare_enable(sor->safe_clk);
	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_SAFE_CLK);
	tegra_sor_clk_enable(sor);
	tegra_hdmi_hda_clk_enable(hdmi);

	tegra_sor_hdmi_pad_power_up(sor);

	tegra_hdmi_config_lanes(hdmi);
	tegra_sor_power_lanes(sor, 4, true);

	tegra_dc_sor_set_internal_panel(sor, false);
	tegra_hdmi_config(hdmi);
	tegra_hdmi_avi_infoframe(hdmi);
	tegra_hdmi_audio_config(hdmi, AUDIO_FREQ_32K, HDA);

	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_BRICK_CLK);
	tegra_dc_sor_attach(sor);
	tegra_nvhdcp_set_plug(hdmi->nvhdcp, tegra_dc_hpd(dc));
	tegra_dc_io_end(dc);

	tegra_dc_setup_clk(dc, dc->clk);
	tegra_dc_hdmi_setup_clk(dc, hdmi->sor->sor_clk);

	tegra_hdmi_config_xbar(hdmi);

	/* TODO: Confirm sequence with HW */
	tegra_sor_writel(sor,  NV_SOR_SEQ_INST(0), 0x8080);
	tegra_sor_writel(sor,  NV_SOR_PWR, 0x80000000);
	tegra_sor_writel(sor,  NV_SOR_PWR, 0x80000001);

	return 0;
}

static void tegra_dc_hdmi_enable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (hdmi->enabled)
		return;

	tegra_hdmi_controller_enable(hdmi);
	hdmi->enabled = true;
}

static void tegra_hdmi_config_clk(struct tegra_hdmi *hdmi, u32 clk_type)
{
	if (clk_type == hdmi->clk_type)
		return;

	if (clk_type == TEGRA_HDMI_BRICK_CLK) {
		u32 val;

		/* TODO: Set sor divider */
		tegra_clk_writel(0, 0xff, 0x410);

		/* Select brick muxes */
		val = (hdmi->dc->mode.pclk < 340000000) ?
			NV_SOR_CLK_CNTRL_DP_LINK_SPEED_G2_7 :
			NV_SOR_CLK_CNTRL_DP_LINK_SPEED_G5_4;
		val |= NV_SOR_CLK_CNTRL_DP_CLK_SEL_SINGLE_PCLK;
		tegra_sor_writel(hdmi->sor, NV_SOR_CLK_CNTRL, val);
		usleep_range(250, 300); /* sor brick pll stabilization delay */

		/* TODO: Select sor clock muxes */
		tegra_clk_writel((3 << 14), (0x3 << 14), 0x410);

		tegra_dc_writel(hdmi->dc, 0x0, DC_DISP_DISP_CLOCK_CONTROL);

		hdmi->clk_type = TEGRA_HDMI_BRICK_CLK;
	} else if (clk_type == TEGRA_HDMI_SAFE_CLK) {
		/* Select sor clock muxes */
		tegra_clk_cfg_ex(hdmi->sor->sor_clk, TEGRA_CLK_SOR_CLK_SEL, 0);

		hdmi->clk_type = TEGRA_HDMI_SAFE_CLK;
	} else {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: incorrect clk type configured\n");
	}
}

static long tegra_dc_hdmi_setup_clk(struct tegra_dc *dc, struct clk *clk)
{
	struct clk *parent_clk = clk_get_sys(NULL,
				dc->out->parent_clk ? : "pll_d2");

	if (IS_ERR_OR_NULL(parent_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: parent clk get failed\n");
		return 0;
	}

	if (clk_get_parent(clk) != parent_clk)
		clk_set_parent(clk, parent_clk);

	/* TODO: Set parent manually */
	if (clk == dc->clk)
		tegra_clk_writel(5 << 29, (0x7 << 29), 0x13c);
	else
		tegra_clk_writel((5 << 29), (0x7 << 29), 0x410);

	if (clk_get_rate(parent_clk) != dc->mode.pclk)
		clk_set_rate(parent_clk, dc->mode.pclk);

	return tegra_dc_pclk_round_rate(dc, dc->mode.pclk);
}

static void tegra_dc_hdmi_disable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	hdmi->enabled = false;

	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_SAFE_CLK);
	tegra_hdmi_controller_disable(hdmi);
	return;
}

static bool tegra_dc_hdmi_detect(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (tegra_dc_hpd(dc)) {
		cancel_delayed_work(&hdmi->hpd_worker);
		schedule_delayed_work(&hdmi->hpd_worker,
				msecs_to_jiffies(HDMI_HPD_DEBOUNCE_DELAY_MS));
	}

	return tegra_dc_hpd(dc);
}

struct tegra_dc_out_ops tegra_dc_hdmi2_0_ops = {
	.init = tegra_dc_hdmi_init,
	.destroy = tegra_dc_hdmi_destroy,
	.enable = tegra_dc_hdmi_enable,
	.disable = tegra_dc_hdmi_disable,
	.setup_clk = tegra_dc_hdmi_setup_clk,
	.detect = tegra_dc_hdmi_detect,
};