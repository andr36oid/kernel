// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:
 *      Chris Zhong <zyw@rock-chips.com>
 *      Nickey Yang <nickey.yang@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/ctype.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-rockchip-logo.h>
#include <linux/rockchip_logo_handoff.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <drm/drm_dsc.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_probe_helper.h>
#include <video/mipi_display.h>
#include <uapi/linux/videodev2.h>
#include <drm/bridge/dw_mipi_dsi.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_simple_kms_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_vop.h"

#define DSI_PHY_RSTZ			0xa0
#define PHY_DISFORCEPLL			0
#define PHY_ENFORCEPLL			BIT(3)
#define PHY_DISABLECLK			0
#define PHY_ENABLECLK			BIT(2)
#define PHY_RSTZ			0
#define PHY_UNRSTZ			BIT(1)
#define PHY_SHUTDOWNZ			0
#define PHY_UNSHUTDOWNZ			BIT(0)

#define DSI_PHY_IF_CFG			0xa4
#define N_LANES(n)			((((n) - 1) & 0x3) << 0)
#define PHY_STOP_WAIT_TIME(cycle)	(((cycle) & 0xff) << 8)

#define DSI_PHY_STATUS			0xb0
#define LOCK				BIT(0)
#define STOP_STATE_CLK_LANE		BIT(2)

#define DSI_PHY_TST_CTRL0		0xb4
#define PHY_TESTCLK			BIT(1)
#define PHY_UNTESTCLK			0
#define PHY_TESTCLR			BIT(0)
#define PHY_UNTESTCLR			0

#define DSI_PHY_TST_CTRL1		0xb8
#define PHY_TESTEN			BIT(16)
#define PHY_UNTESTEN			0
#define PHY_TESTDOUT(n)			(((n) & 0xff) << 8)
#define PHY_TESTDIN(n)			(((n) & 0xff) << 0)

#define DSI_INT_ST0			0xbc
#define DSI_INT_ST1			0xc0
#define DSI_INT_MSK0			0xc4
#define DSI_INT_MSK1			0xc8

#define PHY_STATUS_TIMEOUT_US		10000
#define CMD_PKT_STATUS_TIMEOUT_US	20000

#define BYPASS_VCO_RANGE	BIT(7)
#define VCO_RANGE_CON_SEL(val)	(((val) & 0x7) << 3)
#define VCO_IN_CAP_CON_DEFAULT	(0x0 << 1)
#define VCO_IN_CAP_CON_LOW	(0x1 << 1)
#define VCO_IN_CAP_CON_HIGH	(0x2 << 1)
#define REF_BIAS_CUR_SEL	BIT(0)

#define CP_CURRENT_3UA	0x1
#define CP_CURRENT_4_5UA	0x2
#define CP_CURRENT_7_5UA	0x6
#define CP_CURRENT_6UA	0x9
#define CP_CURRENT_12UA	0xb
#define CP_CURRENT_SEL(val)	((val) & 0xf)
#define CP_PROGRAM_EN		BIT(7)

#define LPF_RESISTORS_15_5KOHM	0x1
#define LPF_RESISTORS_13KOHM	0x2
#define LPF_RESISTORS_11_5KOHM	0x4
#define LPF_RESISTORS_10_5KOHM	0x8
#define LPF_RESISTORS_8KOHM	0x10
#define LPF_PROGRAM_EN		BIT(6)
#define LPF_RESISTORS_SEL(val)	((val) & 0x3f)

#define HSFREQRANGE_SEL(val)	(((val) & 0x3f) << 1)

#define INPUT_DIVIDER(val)	(((val) - 1) & 0x7f)
#define LOW_PROGRAM_EN		0
#define HIGH_PROGRAM_EN		BIT(7)
#define LOOP_DIV_LOW_SEL(val)	(((val) - 1) & 0x1f)
#define LOOP_DIV_HIGH_SEL(val)	((((val) - 1) >> 5) & 0xf)
#define PLL_LOOP_DIV_EN		BIT(5)
#define PLL_INPUT_DIV_EN	BIT(4)

#define POWER_CONTROL		BIT(6)
#define INTERNAL_REG_CURRENT	BIT(3)
#define BIAS_BLOCK_ON		BIT(2)
#define BANDGAP_ON		BIT(0)

#define TER_RESISTOR_HIGH	BIT(7)
#define	TER_RESISTOR_LOW	0
#define LEVEL_SHIFTERS_ON	BIT(6)
#define TER_CAL_DONE		BIT(5)
#define SETRD_MAX		(0x7 << 2)
#define POWER_MANAGE		BIT(1)
#define TER_RESISTORS_ON	BIT(0)

#define BIASEXTR_SEL(val)	((val) & 0x7)
#define BANDGAP_SEL(val)	((val) & 0x7)
#define TLP_PROGRAM_EN		BIT(7)
#define THS_PRE_PROGRAM_EN	BIT(7)
#define THS_ZERO_PROGRAM_EN	BIT(6)

#define PLL_BIAS_CUR_SEL_CAP_VCO_CONTROL		0x10
#define PLL_CP_CONTROL_PLL_LOCK_BYPASS			0x11
#define PLL_LPF_AND_CP_CONTROL				0x12
#define PLL_INPUT_DIVIDER_RATIO				0x17
#define PLL_LOOP_DIVIDER_RATIO				0x18
#define PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL	0x19
#define BANDGAP_AND_BIAS_CONTROL			0x20
#define TERMINATION_RESISTER_CONTROL			0x21
#define AFE_BIAS_BANDGAP_ANALOG_PROGRAMMABILITY		0x22
#define HS_RX_CONTROL_OF_LANE_0				0x44
#define HS_TX_CLOCK_LANE_REQUEST_STATE_TIME_CONTROL	0x60
#define HS_TX_CLOCK_LANE_PREPARE_STATE_TIME_CONTROL	0x61
#define HS_TX_CLOCK_LANE_HS_ZERO_STATE_TIME_CONTROL	0x62
#define HS_TX_CLOCK_LANE_TRAIL_STATE_TIME_CONTROL	0x63
#define HS_TX_CLOCK_LANE_EXIT_STATE_TIME_CONTROL	0x64
#define HS_TX_CLOCK_LANE_POST_TIME_CONTROL		0x65
#define HS_TX_DATA_LANE_REQUEST_STATE_TIME_CONTROL	0x70
#define HS_TX_DATA_LANE_PREPARE_STATE_TIME_CONTROL	0x71
#define HS_TX_DATA_LANE_HS_ZERO_STATE_TIME_CONTROL	0x72
#define HS_TX_DATA_LANE_TRAIL_STATE_TIME_CONTROL	0x73
#define HS_TX_DATA_LANE_EXIT_STATE_TIME_CONTROL		0x74

#define DW_MIPI_NEEDS_PHY_CFG_CLK	BIT(0)
#define DW_MIPI_NEEDS_GRF_CLK		BIT(1)
#define DW_MIPI_NEEDS_HCLK		BIT(2)

#define PX30_GRF_PD_VO_CON1		0x0438
#define PX30_DSI_FORCETXSTOPMODE	(0xf << 7)
#define PX30_DSI_FORCERXMODE		BIT(6)
#define PX30_DSI_TURNDISABLE		BIT(5)
#define PX30_DSI_LCDC_SEL		BIT(0)

#define RK3128_GRF_LVDS_CON0		0x0150
#define RK3128_DSI_FORCETXSTOPMODE	(0xf << 10)
#define RK3128_DSI_FORCERXMODE		(0x1 << 9)
#define RK3128_DSI_TURNDISABLE		(0x1 << 8)

#define RK3288_GRF_SOC_CON6		0x025c
#define RK3288_DSI0_LCDC_SEL		BIT(6)
#define RK3288_DSI1_LCDC_SEL		BIT(9)

#define RK3399_GRF_SOC_CON20		0x6250
#define RK3399_DSI0_LCDC_SEL		BIT(0)
#define RK3399_DSI1_LCDC_SEL		BIT(4)

#define RK3399_GRF_SOC_CON22		0x6258
#define RK3399_DSI0_TURNREQUEST		(0xf << 12)
#define RK3399_DSI0_TURNDISABLE		(0xf << 8)
#define RK3399_DSI0_FORCETXSTOPMODE	(0xf << 4)
#define RK3399_DSI0_FORCERXMODE		(0xf << 0)

#define RK3399_GRF_SOC_CON23		0x625c
#define RK3399_DSI1_TURNDISABLE		(0xf << 12)
#define RK3399_DSI1_FORCETXSTOPMODE	(0xf << 8)
#define RK3399_DSI1_FORCERXMODE		(0xf << 4)
#define RK3399_DSI1_ENABLE		(0xf << 0)

#define RK3399_GRF_SOC_CON24		0x6260
#define RK3399_TXRX_MASTERSLAVEZ	BIT(7)
#define RK3399_TXRX_ENABLECLK		BIT(6)
#define RK3399_TXRX_BASEDIR		BIT(5)

#define RK3562_SYS_GRF_VO_CON1		0x05d4
#define RK3562_DSI_FORCETXSTOPMODE	(0xf << 4)
#define RK3562_DSI_TURNDISABLE		(0x1 << 2)
#define RK3562_DSI_FORCERXMODE		(0x1 << 0)

#define RK3568_GRF_VO_CON2		0x0368
#define RK3568_GRF_VO_CON3		0x036c
#define RK3568_DSI_FORCETXSTOPMODE	(0xf << 4)
#define RK3568_DSI_TURNDISABLE		(0x1 << 2)
#define RK3568_DSI_FORCERXMODE		(0x1 << 0)

#define RV1126_GRF_DSIPHY_CON		0x10220
#define RV1126_DSI_FORCETXSTOPMODE	(0xf << 4)
#define RV1126_DSI_TURNDISABLE		(0x1 << 2)
#define RV1126_DSI_FORCERXMODE		(0x1 << 0)

#define HIWORD_UPDATE(val, mask)	(val | (mask) << 16)

#define to_dsi(nm)	container_of(nm, struct dw_mipi_dsi_rockchip, nm)

enum {
	BANDGAP_97_07,
	BANDGAP_98_05,
	BANDGAP_99_02,
	BANDGAP_100_00,
	BANDGAP_93_17,
	BANDGAP_94_15,
	BANDGAP_95_12,
	BANDGAP_96_10,
};

enum {
	BIASEXTR_87_1,
	BIASEXTR_91_5,
	BIASEXTR_95_9,
	BIASEXTR_100,
	BIASEXTR_105_94,
	BIASEXTR_111_88,
	BIASEXTR_118_8,
	BIASEXTR_127_7,
};

enum soc_type {
	PX30,
	RK3128,
	RK3288,
	RK3399,
	RK3562,
	RK3568,
	RV1126,
};

struct cmd_header {
	u8 cmd_type;
	u8 delay;
	u8 payload_length;
};

struct rockchip_dw_dsi_chip_data {
	u32 reg;

	u32 lcdsel_grf_reg;
	u32 lcdsel_big;
	u32 lcdsel_lit;

	u32 enable_grf_reg;
	u32 enable;

	u32 lanecfg1_grf_reg;
	u32 lanecfg1;
	u32 lanecfg2_grf_reg;
	u32 lanecfg2;

	enum soc_type soc_type;
	unsigned int flags;
	unsigned int max_data_lanes;
	unsigned long max_bit_rate_per_lane;
};

struct dw_mipi_dsi_rockchip {
	struct device *dev;
	struct drm_encoder encoder;
	void __iomem *base;
	int id;

	bool c_option;
	bool scrambling_en;
	unsigned int slice_width;
	unsigned int slice_height;
	unsigned int slice_per_pkt;
	bool block_pred_enable;
	bool dsc_enable;
	u8 version_major;
	u8 version_minor;

	struct drm_dsc_picture_parameter_set *pps;
	struct regmap *grf_regmap;
	struct clk *pllref_clk;
	struct clk *pclk;
	struct clk *grf_clk;
	struct clk *phy_cfg_clk;
	struct clk *hclk;

	/* dual-channel */
	bool is_slave;
	struct dw_mipi_dsi_rockchip *slave;

	/* optional external dphy */
	bool phy_enabled;
	bool loader_phy_init;
	struct phy *phy;
	union phy_configure_opts phy_opts;

	unsigned int lane_mbps; /* per lane */
	u16 input_div;
	u16 feedback_div;
	u32 format;
	u32 mode_flags;

	struct dw_mipi_dsi *dmd;
	const struct rockchip_dw_dsi_chip_data *cdata;
	struct dw_mipi_dsi_plat_data pdata;
	int devcnt;
	struct rockchip_drm_sub_dev sub_dev;
	struct drm_panel *panel;
	struct drm_bridge *bridge;

	struct gpio_desc *te_gpio;
	bool disable_hold_mode;
	u32 attached_lanes;
	/* a refresh rate tried for 5 seconds, see preview_rate_store() */
	struct mutex preview_lock;
	struct delayed_work preview_work;
	struct drm_crtc *preview_crtc;
	struct drm_property_blob *preview_blob;
	struct drm_display_mode preview_orig;
	unsigned long preview_next;	/* jiffies, no new preview before */
	unsigned int preview_tries;
	bool preview_running;
};

struct dphy_pll_parameter_map {
	unsigned int max_mbps;
	u8 hsfreqrange;
	u8 icpctrl;
	u8 lpfctrl;
};

/* The table is based on 27MHz DPHY pll reference clock. */
static const struct dphy_pll_parameter_map dppa_map[] = {
	{  89, 0x00, CP_CURRENT_3UA, LPF_RESISTORS_13KOHM },
	{  99, 0x10, CP_CURRENT_3UA, LPF_RESISTORS_13KOHM },
	{ 109, 0x20, CP_CURRENT_3UA, LPF_RESISTORS_13KOHM },
	{ 129, 0x01, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 139, 0x11, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 149, 0x21, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 169, 0x02, CP_CURRENT_6UA, LPF_RESISTORS_13KOHM },
	{ 179, 0x12, CP_CURRENT_6UA, LPF_RESISTORS_13KOHM },
	{ 199, 0x22, CP_CURRENT_6UA, LPF_RESISTORS_13KOHM },
	{ 219, 0x03, CP_CURRENT_4_5UA, LPF_RESISTORS_13KOHM },
	{ 239, 0x13, CP_CURRENT_4_5UA, LPF_RESISTORS_13KOHM },
	{ 249, 0x23, CP_CURRENT_4_5UA, LPF_RESISTORS_13KOHM },
	{ 269, 0x04, CP_CURRENT_6UA, LPF_RESISTORS_11_5KOHM },
	{ 299, 0x14, CP_CURRENT_6UA, LPF_RESISTORS_11_5KOHM },
	{ 329, 0x05, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 359, 0x15, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 399, 0x25, CP_CURRENT_3UA, LPF_RESISTORS_15_5KOHM },
	{ 449, 0x06, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 499, 0x16, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 549, 0x07, CP_CURRENT_7_5UA, LPF_RESISTORS_10_5KOHM },
	{ 599, 0x17, CP_CURRENT_7_5UA, LPF_RESISTORS_10_5KOHM },
	{ 649, 0x08, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 699, 0x18, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 749, 0x09, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 799, 0x19, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 849, 0x29, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 899, 0x39, CP_CURRENT_7_5UA, LPF_RESISTORS_11_5KOHM },
	{ 949, 0x0a, CP_CURRENT_12UA, LPF_RESISTORS_8KOHM },
	{ 999, 0x1a, CP_CURRENT_12UA, LPF_RESISTORS_8KOHM },
	{1049, 0x2a, CP_CURRENT_12UA, LPF_RESISTORS_8KOHM },
	{1099, 0x3a, CP_CURRENT_12UA, LPF_RESISTORS_8KOHM },
	{1149, 0x0b, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1199, 0x1b, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1249, 0x2b, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1299, 0x3b, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1349, 0x0c, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1399, 0x1c, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1449, 0x2c, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM },
	{1500, 0x3c, CP_CURRENT_12UA, LPF_RESISTORS_10_5KOHM }
};

static int max_mbps_to_parameter(unsigned int max_mbps)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dppa_map); i++)
		if (dppa_map[i].max_mbps >= max_mbps)
			return i;

	return -EINVAL;
}

static inline void dsi_write(struct dw_mipi_dsi_rockchip *dsi, u32 reg, u32 val)
{
	writel(val, dsi->base + reg);
}

static inline u32 dsi_read(struct dw_mipi_dsi_rockchip *dsi, u32 reg)
{
	return readl(dsi->base + reg);
}

static inline void dsi_set(struct dw_mipi_dsi_rockchip *dsi, u32 reg, u32 mask)
{
	dsi_write(dsi, reg, dsi_read(dsi, reg) | mask);
}

static inline void dsi_update_bits(struct dw_mipi_dsi_rockchip *dsi, u32 reg,
				   u32 mask, u32 val)
{
	dsi_write(dsi, reg, (dsi_read(dsi, reg) & ~mask) | val);
}

static void dw_mipi_dsi_phy_write(struct dw_mipi_dsi_rockchip *dsi,
				  u8 test_code,
				  u8 test_data)
{
	/*
	 * With the falling edge on TESTCLK, the TESTDIN[7:0] signal content
	 * is latched internally as the current test code. Test data is
	 * programmed internally by rising edge on TESTCLK.
	 */
	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);

	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_TESTEN | PHY_TESTDOUT(0) |
					  PHY_TESTDIN(test_code));

	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_UNTESTCLK | PHY_UNTESTCLR);

	dsi_write(dsi, DSI_PHY_TST_CTRL1, PHY_UNTESTEN | PHY_TESTDOUT(0) |
					  PHY_TESTDIN(test_data));

	dsi_write(dsi, DSI_PHY_TST_CTRL0, PHY_TESTCLK | PHY_UNTESTCLR);
}

/**
 * ns2bc - Nanoseconds to byte clock cycles
 */
static inline unsigned int ns2bc(struct dw_mipi_dsi_rockchip *dsi, int ns)
{
	return DIV_ROUND_UP(ns * dsi->lane_mbps / 8, 1000);
}

/**
 * ns2ui - Nanoseconds to UI time periods
 */
static inline unsigned int ns2ui(struct dw_mipi_dsi_rockchip *dsi, int ns)
{
	return DIV_ROUND_UP(ns * dsi->lane_mbps, 1000);
}

static int dw_mipi_dsi_phy_tx_config(struct dw_mipi_dsi_rockchip *dsi)
{
	int ret;

	if (dsi->cdata->lanecfg1_grf_reg) {
		ret = regmap_write(dsi->grf_regmap, dsi->cdata->lanecfg1_grf_reg,
				   dsi->cdata->lanecfg1);
		if (ret)
			return ret;
	}
	if (dsi->cdata->lanecfg2_grf_reg) {
		ret = regmap_write(dsi->grf_regmap, dsi->cdata->lanecfg2_grf_reg,
				   dsi->cdata->lanecfg2);
		if (ret)
			return ret;
	}
	if (dsi->cdata->enable_grf_reg) {
		ret = regmap_write(dsi->grf_regmap, dsi->cdata->enable_grf_reg,
				   dsi->cdata->enable);
		if (ret)
			return ret;
	}
	return 0;
}

static int dw_mipi_dsi_phy_init(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	int i, vco, ret;

	ret = dw_mipi_dsi_phy_tx_config(dsi);
	if (ret)
		return ret;

	if (dsi->phy)
		return 0;

	/*
	 * Get vco from frequency(lane_mbps)
	 * vco	frequency table
	 * 000 - between   80 and  200 MHz
	 * 001 - between  200 and  300 MHz
	 * 010 - between  300 and  500 MHz
	 * 011 - between  500 and  700 MHz
	 * 100 - between  700 and  900 MHz
	 * 101 - between  900 and 1100 MHz
	 * 110 - between 1100 and 1300 MHz
	 * 111 - between 1300 and 1500 MHz
	 */
	vco = (dsi->lane_mbps < 200) ? 0 : (dsi->lane_mbps + 100) / 200;

	i = max_mbps_to_parameter(dsi->lane_mbps);
	if (i < 0) {
		DRM_DEV_ERROR(dsi->dev,
			      "failed to get parameter for %dmbps clock\n",
			      dsi->lane_mbps);
		return i;
	}

	dw_mipi_dsi_phy_write(dsi, PLL_BIAS_CUR_SEL_CAP_VCO_CONTROL,
			      BYPASS_VCO_RANGE |
			      VCO_RANGE_CON_SEL(vco) |
			      VCO_IN_CAP_CON_LOW |
			      REF_BIAS_CUR_SEL);

	dw_mipi_dsi_phy_write(dsi, PLL_CP_CONTROL_PLL_LOCK_BYPASS,
			      CP_CURRENT_SEL(dppa_map[i].icpctrl));
	dw_mipi_dsi_phy_write(dsi, PLL_LPF_AND_CP_CONTROL,
			      CP_PROGRAM_EN | LPF_PROGRAM_EN |
			      LPF_RESISTORS_SEL(dppa_map[i].lpfctrl));

	dw_mipi_dsi_phy_write(dsi, HS_RX_CONTROL_OF_LANE_0,
			      HSFREQRANGE_SEL(dppa_map[i].hsfreqrange));

	dw_mipi_dsi_phy_write(dsi, PLL_INPUT_DIVIDER_RATIO,
			      INPUT_DIVIDER(dsi->input_div));
	dw_mipi_dsi_phy_write(dsi, PLL_LOOP_DIVIDER_RATIO,
			      LOOP_DIV_LOW_SEL(dsi->feedback_div) |
			      LOW_PROGRAM_EN);
	/*
	 * We need set PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL immediately
	 * to make the configured LSB effective according to IP simulation
	 * and lab test results.
	 * Only in this way can we get correct mipi phy pll frequency.
	 */
	dw_mipi_dsi_phy_write(dsi, PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL,
			      PLL_LOOP_DIV_EN | PLL_INPUT_DIV_EN);
	dw_mipi_dsi_phy_write(dsi, PLL_LOOP_DIVIDER_RATIO,
			      LOOP_DIV_HIGH_SEL(dsi->feedback_div) |
			      HIGH_PROGRAM_EN);
	dw_mipi_dsi_phy_write(dsi, PLL_INPUT_AND_LOOP_DIVIDER_RATIOS_CONTROL,
			      PLL_LOOP_DIV_EN | PLL_INPUT_DIV_EN);

	dw_mipi_dsi_phy_write(dsi, AFE_BIAS_BANDGAP_ANALOG_PROGRAMMABILITY,
			      LOW_PROGRAM_EN | BIASEXTR_SEL(BIASEXTR_127_7));
	dw_mipi_dsi_phy_write(dsi, AFE_BIAS_BANDGAP_ANALOG_PROGRAMMABILITY,
			      HIGH_PROGRAM_EN | BANDGAP_SEL(BANDGAP_96_10));

	dw_mipi_dsi_phy_write(dsi, BANDGAP_AND_BIAS_CONTROL,
			      POWER_CONTROL | INTERNAL_REG_CURRENT |
			      BIAS_BLOCK_ON | BANDGAP_ON);

	dw_mipi_dsi_phy_write(dsi, TERMINATION_RESISTER_CONTROL,
			      TER_RESISTOR_LOW | TER_CAL_DONE |
			      SETRD_MAX | TER_RESISTORS_ON);
	dw_mipi_dsi_phy_write(dsi, TERMINATION_RESISTER_CONTROL,
			      TER_RESISTOR_HIGH | LEVEL_SHIFTERS_ON |
			      SETRD_MAX | POWER_MANAGE |
			      TER_RESISTORS_ON);

	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_REQUEST_STATE_TIME_CONTROL,
			      TLP_PROGRAM_EN | ns2bc(dsi, 60));
	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_PREPARE_STATE_TIME_CONTROL,
			      THS_PRE_PROGRAM_EN | ns2ui(dsi, 40));
	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_HS_ZERO_STATE_TIME_CONTROL,
			      THS_ZERO_PROGRAM_EN | ns2bc(dsi, 300));
	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_TRAIL_STATE_TIME_CONTROL,
			      THS_PRE_PROGRAM_EN | ns2ui(dsi, 100));
	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_EXIT_STATE_TIME_CONTROL,
			      BIT(5) | ns2bc(dsi, 100));
	dw_mipi_dsi_phy_write(dsi, HS_TX_CLOCK_LANE_POST_TIME_CONTROL,
			      BIT(5) | (ns2bc(dsi, 60) + 7));

	dw_mipi_dsi_phy_write(dsi, HS_TX_DATA_LANE_REQUEST_STATE_TIME_CONTROL,
			      TLP_PROGRAM_EN | ns2bc(dsi, 60));
	dw_mipi_dsi_phy_write(dsi, HS_TX_DATA_LANE_PREPARE_STATE_TIME_CONTROL,
			      THS_PRE_PROGRAM_EN | (ns2ui(dsi, 50) + 20));
	dw_mipi_dsi_phy_write(dsi, HS_TX_DATA_LANE_HS_ZERO_STATE_TIME_CONTROL,
			      THS_ZERO_PROGRAM_EN | (ns2bc(dsi, 140) + 2));
	dw_mipi_dsi_phy_write(dsi, HS_TX_DATA_LANE_TRAIL_STATE_TIME_CONTROL,
			      THS_PRE_PROGRAM_EN | (ns2ui(dsi, 60) + 8));
	dw_mipi_dsi_phy_write(dsi, HS_TX_DATA_LANE_EXIT_STATE_TIME_CONTROL,
			      BIT(5) | ns2bc(dsi, 100));

	return 0;
}

static int dw_mipi_dsi_phy_hold_loader(void *priv_data,bool on)
{
 struct dw_mipi_dsi_rockchip *dsi=priv_data;
 return inno_video_phy_hold_loader(dsi->phy,on);
}

static int dw_mipi_dsi_phy_adopt_loader(void *priv_data)
{
 struct dw_mipi_dsi_rockchip *dsi=priv_data;int ret;
 if(!dsi->phy || dsi->phy_enabled || dsi->slave)return -EOPNOTSUPP;
 ret=phy_set_mode(dsi->phy,PHY_MODE_MIPI_DPHY);if(ret)return ret;
 ret=phy_init(dsi->phy);if(ret)return ret;
 rockchip_logo_adopting_phy(true);
 ret=phy_power_on(dsi->phy);
 rockchip_logo_adopting_phy(false);
 if(ret){phy_exit(dsi->phy);return ret;}
 dsi->phy_enabled=true;dsi->loader_phy_init=true;return 0;
}

static int dw_mipi_dsi_phy_power_on_checked(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	int ret;

	if (dsi->phy_enabled)
		return 0;
	ret = phy_power_on(dsi->phy);
	if (!ret)
		dsi->phy_enabled = true;
	return ret;
}

static void dw_mipi_dsi_phy_power_on(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;

	if (dsi->phy_enabled)
		return;

	phy_power_on(dsi->phy);
	dsi->phy_enabled = true;
}

static int dw_mipi_dsi_phy_power_off_checked(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	int ret;

	if (!dsi->phy_enabled)
		return 0;
	ret = phy_power_off(dsi->phy);
	if (!ret) {
		dsi->phy_enabled = false;
		if (dsi->loader_phy_init) {
			ret = phy_exit(dsi->phy);
			if (!ret)
				dsi->loader_phy_init = false;
		}
	}
	return ret;
}

static void dw_mipi_dsi_phy_power_off(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;

	if (!dsi->phy_enabled)
		return;

	phy_power_off(dsi->phy);
	dsi->phy_enabled = false;
}

static int dw_mipi_dsi_rockchip_host_attach(void *data,
					 struct mipi_dsi_device *device)
{
	struct dw_mipi_dsi_rockchip *dsi = data;

	dsi->attached_lanes = device->lanes;
	dsi->format = device->format;
	dsi->mode_flags = device->mode_flags;
	return 0;
}

static const struct dw_mipi_dsi_host_ops dw_mipi_dsi_rockchip_host_ops = {
	.attach = dw_mipi_dsi_rockchip_host_attach,
};

/* Uncapped lane requirement, in bits/second. Native PHY programming is unchanged. */
static u64 dw_mipi_dsi_clock_to_lane_rate(struct dw_mipi_dsi_rockchip *dsi,
					int clock)
{
	int bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	u32 lanes = dsi->attached_lanes;

	/* The core may divide a total lane count, or pair two full hosts. */
	if (dsi->slave && lanes <= dsi->cdata->max_data_lanes)
		lanes *= 2;
	if (bpp <= 0 || !lanes || clock <= 0)
		return U64_MAX;
	return DIV_ROUND_UP_ULL((u64)clock * 1000 * bpp * 10, lanes * 9);
}

static enum drm_mode_status
dw_mipi_dsi_rockchip_mode_valid(void *data, const struct drm_display_mode *mode)
{
	struct dw_mipi_dsi_rockchip *dsi = data;

	if (of_property_read_bool(dsi->dev->of_node, "rockchip,lane-rate") ||
	    (mode->type & DRM_MODE_TYPE_PREFERRED))
		return MODE_OK;
	if (dw_mipi_dsi_clock_to_lane_rate(dsi, mode->clock) >
	    dsi->cdata->max_bit_rate_per_lane)
		return MODE_CLOCK_HIGH;
	return MODE_OK;
}

static unsigned int dw_mipi_dsi_calculate_lane_mpbs(struct dw_mipi_dsi_rockchip *dsi,
						    const struct drm_display_mode *mode,
						    u32 lanes, int bpp)
{
	struct device *dev = dsi->dev;
	unsigned int target_mbps = 1000;
	unsigned int max_mbps;
	unsigned int value;
	unsigned long mpclk, tmp;

	if (dsi->is_slave)
		return dsi->lane_mbps;

	max_mbps = dsi->cdata->max_bit_rate_per_lane / USEC_PER_SEC;

	/* optional override of the desired bandwidth */
	if (!of_property_read_u32(dev->of_node, "rockchip,lane-rate", &value)) {
		target_mbps = value;
	} else {
		mpclk = DIV_ROUND_UP(mode->clock, MSEC_PER_SEC);
		if (mpclk) {
			/* take 1 / 0.9, since mbps must big than bandwidth of RGB */
			tmp = mpclk * (bpp / lanes) * 10 / 9;
			if (tmp < max_mbps)
				target_mbps = tmp;
			else {
				DRM_DEV_ERROR(dsi->dev,
					      "DPHY clock frequency is out of range\n");
				target_mbps = max_mbps;
			}
		}
	}

	if (dsi->slave)
		dsi->slave->lane_mbps = target_mbps;

	return target_mbps;
}

static int
dw_mipi_dsi_get_lane_mbps(void *priv_data, const struct drm_display_mode *mode,
			  unsigned long mode_flags, u32 lanes, u32 format,
			  unsigned int *lane_mbps)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	unsigned long best_freq = 0;
	unsigned long fvco_min, fvco_max, fin, fout;
	unsigned int min_prediv, max_prediv;
	unsigned int _prediv, best_prediv;
	unsigned long _fbdiv, best_fbdiv;
	unsigned long min_delta = ULONG_MAX;
	unsigned long target_pclk, hs_clk_rate;
	unsigned int target_mbps;
	int bpp, ret;

	dsi->format = format;
	bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	if (bpp <= 0) {
		DRM_DEV_ERROR(dsi->dev,
			      "failed to get bpp for pixel format %d\n",
			      dsi->format);
		return bpp;
	}

	target_mbps = dw_mipi_dsi_calculate_lane_mpbs(dsi, mode, lanes, bpp);

	/* for external phy only a the mipi_dphy_config is necessary */
	if (dsi->phy) {
		target_pclk = DIV_ROUND_CLOSEST_ULL(target_mbps * lanes, bpp);
		phy_mipi_dphy_get_default_config(target_pclk * USEC_PER_SEC,
						 bpp, lanes,
						 &dsi->phy_opts.mipi_dphy);
		/* dArkOSen: request the target lane rate directly, as 4.4 did */
		dsi->phy_opts.mipi_dphy.hs_clk_rate = target_mbps * USEC_PER_SEC;
		ret = phy_set_mode(dsi->phy, PHY_MODE_MIPI_DPHY);
		if (ret) {
			DRM_DEV_ERROR(dsi->dev,
				      "failed to set phy mode: %d\n", ret);
			return ret;
		}

		ret = phy_configure(dsi->phy, &dsi->phy_opts);
		if (ret)
			return ret;
		hs_clk_rate = dsi->phy_opts.mipi_dphy.hs_clk_rate;
		dsi->lane_mbps = DIV_ROUND_UP(hs_clk_rate, USEC_PER_SEC);
		*lane_mbps = dsi->lane_mbps;

		return 0;
	}

	fin = clk_get_rate(dsi->pllref_clk);
	fout = target_mbps * USEC_PER_SEC;

	/* constraint: 5Mhz <= Fref / N <= 40MHz */
	min_prediv = DIV_ROUND_UP(fin, 40 * USEC_PER_SEC);
	max_prediv = fin / (5 * USEC_PER_SEC);

	/* constraint: 80MHz <= Fvco <= 1500Mhz */
	fvco_min = 80 * USEC_PER_SEC;
	fvco_max = 1500 * USEC_PER_SEC;

	for (_prediv = min_prediv; _prediv <= max_prediv; _prediv++) {
		u64 tmp;
		u32 delta;
		/* Fvco = Fref * M / N */
		tmp = (u64)fout * _prediv;
		do_div(tmp, fin);
		_fbdiv = tmp;
		/*
		 * Due to the use of a "by 2 pre-scaler," the range of the
		 * feedback multiplication value M is limited to even division
		 * numbers, and m must be greater than 6, not bigger than 512.
		 */
		if (_fbdiv < 6 || _fbdiv > 512)
			continue;

		_fbdiv += _fbdiv % 2;

		tmp = (u64)_fbdiv * fin;
		do_div(tmp, _prediv);
		if (tmp < fvco_min || tmp > fvco_max)
			continue;

		delta = abs(fout - tmp);
		if (delta < min_delta) {
			best_prediv = _prediv;
			best_fbdiv = _fbdiv;
			min_delta = delta;
			best_freq = tmp;
		}
	}

	if (best_freq) {
		dsi->lane_mbps = DIV_ROUND_UP(best_freq, USEC_PER_SEC);
		*lane_mbps = dsi->lane_mbps;
		dsi->input_div = best_prediv;
		dsi->feedback_div = best_fbdiv;
	} else {
		DRM_DEV_ERROR(dsi->dev, "Can not find best_freq for DPHY\n");
		return -EINVAL;
	}

	return 0;
}

struct hstt {
	unsigned int maxfreq;
	struct dw_mipi_dsi_dphy_timing timing;
};

struct dw_mipi_dsi_dphy_timing dphy_hstt = {
	.clk_lp2hs = 0x40,
	.clk_hs2lp = 0x40,
	.data_lp2hs = 0x10,
	.data_hs2lp = 0x14,
};

static int
dw_mipi_dsi_phy_get_timing(void *priv_data, unsigned int lane_mbps,
			   struct dw_mipi_dsi_dphy_timing *timing)
{
	*timing = dphy_hstt;

	return 0;
}

static const struct dw_mipi_dsi_phy_ops dw_mipi_dsi_rockchip_phy_ops = {
	.init = dw_mipi_dsi_phy_init,
	.power_on = dw_mipi_dsi_phy_power_on,
	.power_on_checked = dw_mipi_dsi_phy_power_on_checked,
	.adopt_loader = dw_mipi_dsi_phy_adopt_loader,
	.hold_loader = dw_mipi_dsi_phy_hold_loader,
	.power_off_checked = dw_mipi_dsi_phy_power_off_checked,
	.power_off = dw_mipi_dsi_phy_power_off,
	.get_lane_mbps = dw_mipi_dsi_get_lane_mbps,
	.get_timing = dw_mipi_dsi_phy_get_timing,
};

static void dw_mipi_dsi_rockchip_vop_routing(struct dw_mipi_dsi_rockchip *dsi)
{
	int mux;

	mux = drm_of_encoder_active_endpoint_id(dsi->dev->of_node,
						&dsi->encoder);
	if (mux < 0)
		return;

	if (dsi->cdata->lcdsel_grf_reg) {
		regmap_write(dsi->grf_regmap, dsi->cdata->lcdsel_grf_reg,
			mux ? dsi->cdata->lcdsel_lit : dsi->cdata->lcdsel_big);

		if (dsi->slave && dsi->slave->cdata->lcdsel_grf_reg)
			regmap_write(dsi->slave->grf_regmap,
				     dsi->slave->cdata->lcdsel_grf_reg,
				     mux ? dsi->slave->cdata->lcdsel_lit :
				     dsi->slave->cdata->lcdsel_big);
	}
}

static int
dw_mipi_dsi_encoder_atomic_check(struct drm_encoder *encoder,
				 struct drm_crtc_state *crtc_state,
				 struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct dw_mipi_dsi_rockchip *dsi = to_dsi(encoder);
	struct drm_connector *connector = conn_state->connector;
	struct drm_display_info *info = &connector->display_info;

	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		s->output_mode = ROCKCHIP_OUT_MODE_P888;
		break;
	case MIPI_DSI_FMT_RGB666:
		s->output_mode = ROCKCHIP_OUT_MODE_P666;
		break;
	case MIPI_DSI_FMT_RGB565:
		s->output_mode = ROCKCHIP_OUT_MODE_P565;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	if (info->num_bus_formats)
		s->bus_format = info->bus_formats[0];
	else
		s->bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	s->bus_flags = info->bus_flags;
	/* rk356x series drive mipi pixdata on posedge */
	if (dsi->cdata->soc_type == RK3568) {
		s->bus_flags &= ~DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;
		s->bus_flags |= DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE;
	}

	s->output_type = DRM_MODE_CONNECTOR_DSI;
	s->tv_state = &conn_state->tv;
	s->color_space = V4L2_COLORSPACE_DEFAULT;
	s->output_if = dsi->id ? VOP_OUTPUT_IF_MIPI1 : VOP_OUTPUT_IF_MIPI0;
	if (dsi->slave) {
		s->output_flags |= ROCKCHIP_OUTPUT_DUAL_CHANNEL_LEFT_RIGHT_MODE;
		s->output_if |= VOP_OUTPUT_IF_MIPI1;
	}

	/* dual link dsi for rk3399 */
	if (dsi->id && dsi->cdata->soc_type == RK3399)
		s->output_flags |= ROCKCHIP_OUTPUT_DATA_SWAP;

	if (!(dsi->mode_flags & MIPI_DSI_MODE_VIDEO)) {
		s->output_flags |= ROCKCHIP_OUTPUT_MIPI_DS_MODE;
		s->soft_te = dsi->te_gpio ? true : false;
		s->hold_mode = dsi->disable_hold_mode ? false : true;
	}

	if (dsi->dsc_enable) {
		s->dsc_enable = 1;
		s->dsc_sink_cap.version_major = dsi->version_major;
		s->dsc_sink_cap.version_minor = dsi->version_minor;
		s->dsc_sink_cap.slice_width = dsi->slice_width;
		s->dsc_sink_cap.slice_height = dsi->slice_height;
		/* only can support rgb888 panel now */
		s->dsc_sink_cap.target_bits_per_pixel_x16 = 8 << 4;
		s->dsc_sink_cap.block_pred = dsi->block_pred_enable;
		s->dsc_sink_cap.native_420 = 0;

		memcpy(&s->pps, dsi->pps, sizeof(struct drm_dsc_picture_parameter_set));
	}

	return 0;
}

static void dw_mipi_dsi_encoder_enable(struct drm_encoder *encoder)
{
	struct dw_mipi_dsi_rockchip *dsi = to_dsi(encoder);

	dw_mipi_dsi_rockchip_vop_routing(dsi);
}

static void dw_mipi_dsi_encoder_disable(struct drm_encoder *encoder)
{
}

static void dw_mipi_dsi_rockchip_loader_protect(struct dw_mipi_dsi_rockchip *dsi, bool on)
{
	if (on) {
		pm_runtime_get_sync(dsi->dev);
		phy_init(dsi->phy);
		dsi->phy_enabled = true;
		if (dsi->phy)
			dsi->phy->power_count++;
	} else {
		pm_runtime_put(dsi->dev);
		phy_exit(dsi->phy);
		dsi->phy_enabled = false;
		if (dsi->phy)
			dsi->phy->power_count--;
	}

	if (dsi->slave)
		dw_mipi_dsi_rockchip_loader_protect(dsi->slave, on);
}

static int dw_mipi_dsi_rockchip_quiesce_panel(void *priv_data)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	struct drm_encoder *encoder = &dsi->encoder;
	struct rockchip_drm_private *private = encoder->dev->dev_private;
	struct drm_crtc *crtc = encoder->crtc;
	int pipe;

	if (!crtc)
		return -ENODEV;
	pipe = drm_crtc_index(crtc);
	if (!private->crtc_funcs[pipe] ||
	    !private->crtc_funcs[pipe]->panel_quiesce)
		return -EOPNOTSUPP;
	return private->crtc_funcs[pipe]->panel_quiesce(crtc);
}

static void dw_mipi_dsi_rockchip_panel_fail(void *priv_data, int error)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	struct drm_encoder *encoder = &dsi->encoder;
	struct rockchip_drm_private *private = encoder->dev->dev_private;
	struct drm_crtc *crtc = encoder->crtc;
	int pipe;

	if (!crtc)
		return;
	pipe = drm_crtc_index(crtc);
	if (private->crtc_funcs[pipe] && private->crtc_funcs[pipe]->panel_fail)
		private->crtc_funcs[pipe]->panel_fail(crtc, error);
}

static int dw_mipi_dsi_rockchip_frame_ready(void *priv_data)
{
 struct dw_mipi_dsi_rockchip *dsi=priv_data;
 struct drm_crtc *crtc=dsi->encoder.crtc;struct rockchip_drm_private *priv;
 unsigned int pipe;
 if(!crtc)return -ENODEV;
 priv=crtc->dev->dev_private;pipe=drm_crtc_index(crtc);
 if(!priv->crtc_funcs[pipe] || !priv->crtc_funcs[pipe]->panel_frame_ready)
  return -EOPNOTSUPP;
 return priv->crtc_funcs[pipe]->panel_frame_ready(crtc);
}

static int dw_mipi_dsi_rockchip_encoder_loader_protect(struct drm_encoder *encoder,
					      bool on)
{
	struct dw_mipi_dsi_rockchip *dsi = to_dsi(encoder);
	int ret = 0;

	dev_info(dsi->dev, "DISPLAY-HANDOFF: loader protect on=%u\n", on);
	/* Guarded direct DSI panels must never acquire a fictitious init state. */
	if (dsi->pdata.prepare_before_crtc)
		return on ? -EPERM : 0;

	if (dsi->panel) {
		if (on)
			ret = panel_simple_loader_protect(dsi->panel);
		else
			ret = panel_simple_loader_unprotect(dsi->panel);
		if (ret && on)
			return ret;
	}

	dw_mipi_dsi_rockchip_loader_protect(dsi, on);

	return ret;
}

static const struct drm_encoder_helper_funcs
dw_mipi_dsi_encoder_helper_funcs = {
	.atomic_check = dw_mipi_dsi_encoder_atomic_check,
	.enable = dw_mipi_dsi_encoder_enable,
	.disable = dw_mipi_dsi_encoder_disable,
};

static int rockchip_dsi_drm_create_encoder(struct dw_mipi_dsi_rockchip *dsi,
					   struct drm_device *drm_dev)
{
	struct drm_encoder *encoder = &dsi->encoder;
	int ret;

	encoder->possible_crtcs = rockchip_drm_of_find_possible_crtcs(drm_dev,
								      dsi->dev->of_node);

	ret = drm_simple_encoder_init(drm_dev, encoder, DRM_MODE_ENCODER_DSI);
	if (ret) {
		DRM_ERROR("Failed to initialize encoder with drm\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &dw_mipi_dsi_encoder_helper_funcs);

	return 0;
}

static struct device
*dw_mipi_dsi_rockchip_find_second(struct dw_mipi_dsi_rockchip *dsi)
{
	struct device_node *node = NULL;
	struct platform_device *pdev;
	struct dw_mipi_dsi_rockchip *dsi2;

	node = of_parse_phandle(dsi->dev->of_node, "rockchip,dual-channel", 0);
	if (node) {
		pdev = of_find_device_by_node(node);
		if (!pdev)
			return ERR_PTR(-EPROBE_DEFER);

		dsi2 = platform_get_drvdata(pdev);
		if (!dsi2) {
			platform_device_put(pdev);
			return ERR_PTR(-EPROBE_DEFER);
		}

		return &pdev->dev;
	}

	return NULL;
}

static irqreturn_t dw_mipi_dsi_te_irq_handler(int irq, void *dev_id)
{
	struct dw_mipi_dsi_rockchip *dsi = (struct dw_mipi_dsi_rockchip *)dev_id;
	struct drm_encoder *encoder = &dsi->encoder;

	if (encoder->crtc)
		rockchip_drm_te_handle(encoder->crtc);

	return IRQ_HANDLED;
}

static int dw_mipi_dsi_get_dsc_info_from_sink(struct dw_mipi_dsi_rockchip *dsi,
					      struct drm_panel *panel,
					      struct drm_bridge *bridge)
{
	struct drm_dsc_picture_parameter_set *pps = NULL;
	struct device_node *np = NULL;
	struct cmd_header *header;
	const void *data;
	char *d;
	uint8_t *dsc_packed_pps;
	int len;

	if (!panel && !bridge)
		return -ENODEV;

	if (panel)
		np = panel->dev->of_node;
	else
		np = bridge->of_node;

	dsi->c_option = of_property_read_bool(np, "phy-c-option");
	dsi->scrambling_en = of_property_read_bool(np, "scrambling-enable");
	dsi->dsc_enable = of_property_read_bool(np, "compressed-data");
	dsi->block_pred_enable = of_property_read_bool(np, "blk-pred-enable");
	of_property_read_u32(np, "dsi,flags", &dsi->mode_flags);
	of_property_read_u32(np, "slice-width", &dsi->slice_width);
	of_property_read_u32(np, "slice-height", &dsi->slice_height);
	of_property_read_u32(np, "slice-per-pkt", &dsi->slice_per_pkt);
	of_property_read_u8(np, "version-major", &dsi->version_major);
	of_property_read_u8(np, "version-minor", &dsi->version_minor);

	data = of_get_property(np, "panel-init-sequence", &len);
	if (!data)
		return -EINVAL;

	d = devm_kmemdup(dsi->dev, data, len, GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	while (len > sizeof(*header)) {
		header = (struct cmd_header *)d;
		d += sizeof(*header);
		len -= sizeof(*header);

		if (header->payload_length > len)
			return -EINVAL;

		if (header->cmd_type == MIPI_DSI_PICTURE_PARAMETER_SET) {
			dsc_packed_pps = devm_kmemdup(dsi->dev, d,
						      header->payload_length, GFP_KERNEL);
			if (!dsc_packed_pps)
				return -ENOMEM;

			pps = (struct drm_dsc_picture_parameter_set *)dsc_packed_pps;
			break;
		}

		d += header->payload_length;
		len -= header->payload_length;
	}
	dsi->pps = pps;

	return 0;
}

/*
 * Refresh rate preview: root writes a rate in Hz (up to three decimals) to
 * preview_rate on this device, and the panel's own mode at that rate is shown
 * for 5 seconds. Then the mode from before comes back, whatever userspace does
 * meanwhile; the timer is here, not in userspace. Writing 0 brings it back at
 * once. A new preview can start 2 seconds after the last one ended.
 *
 * The rate is set with a commit of our own that changes only the CRTC's mode,
 * planes and everything else stay. The Rockchip HWC sends MODE_ID only when it
 * changes the display config or switches the screen on or off, never with a
 * frame, so it doesn't undo the preview. If it does set a mode meanwhile, its
 * mode stays: that one is from the mode list, and the preview is over anyway.
 */
#define DSI_PREVIEW_MS			5000
#define DSI_PREVIEW_COOLDOWN_MS		2000
#define DSI_PREVIEW_RETRY_MS		1000
#define DSI_PREVIEW_TRIES		5
#define DSI_PREVIEW_MIN_MHZ		30000
#define DSI_PREVIEW_MAX_MHZ		300000

/* "77.5" -> 77500, in milli-Hz; 0 when it doesn't parse */
static unsigned int dw_mipi_dsi_parse_mhz(const char *s)
{
	unsigned int hz = 0, frac = 0;
	int digits = -1;

	if (!isdigit(*s))
		return 0;
	for (; *s; s++) {
		if (*s == '.' && digits < 0) {
			digits = 0;
			continue;
		}
		if (!isdigit(*s))
			return 0;
		if (digits < 0) {
			hz = hz * 10 + (*s - '0');
			if (hz > DSI_PREVIEW_MAX_MHZ / 1000)
				return 0;
		} else {
			if (digits == 3)
				return 0;
			frac = frac * 10 + (*s - '0');
			digits++;
		}
	}
	for (; digits >= 0 && digits < 3; digits++)
		frac *= 10;

	return hz * 1000 + frac;
}

/* the panel's own mode; with all modeset locks held */
static struct drm_display_mode *dw_mipi_dsi_native_mode(struct dw_mipi_dsi_rockchip *dsi)
{
	struct drm_display_mode *m;

	list_for_each_entry(m, &dsi->sub_dev.connector->modes, head)
		if (m->type & DRM_MODE_TYPE_PREFERRED)
			return m;

	return NULL;
}

/* set a mode on the CRTC and keep everything else; with all modeset locks held */
static int dw_mipi_dsi_commit_mode(struct dw_mipi_dsi_rockchip *dsi,
				   struct drm_crtc *crtc,
				   const struct drm_display_mode *mode)
{
	struct drm_device *drm = dsi->sub_dev.connector->dev;
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	int ret;

	state = drm_atomic_state_alloc(drm);
	if (!state)
		return -ENOMEM;
	state->acquire_ctx = drm->mode_config.acquire_ctx;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
	if (ret)
		goto out;
	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (ret)
		goto out;
	ret = drm_atomic_add_affected_planes(state, crtc);
	if (ret)
		goto out;

	ret = drm_atomic_commit(state);
out:
	drm_atomic_state_put(state);
	return ret;
}

static unsigned int dw_mipi_dsi_mode_mhz(const struct drm_display_mode *m)
{
	if (!m->htotal || !m->vtotal)
		return 0;

	return DIV_ROUND_CLOSEST_ULL((u64)m->clock * 1000000,
				     m->htotal * m->vtotal);
}

static void dw_mipi_dsi_preview_end(struct dw_mipi_dsi_rockchip *dsi)
{
	drm_property_blob_put(dsi->preview_blob);
	dsi->preview_blob = NULL;
	dsi->preview_crtc = NULL;
	dsi->preview_running = false;
	dsi->preview_next = jiffies + msecs_to_jiffies(DSI_PREVIEW_COOLDOWN_MS);
}

static void dw_mipi_dsi_preview_restore(struct work_struct *work)
{
	struct dw_mipi_dsi_rockchip *dsi = container_of(to_delayed_work(work),
					       struct dw_mipi_dsi_rockchip, preview_work);
	struct drm_device *drm = dsi->sub_dev.connector->dev;
	struct drm_display_mode *native;
	struct drm_crtc *crtc;
	unsigned int mhz;
	int ret;

	mutex_lock(&dsi->preview_lock);
	if (!dsi->preview_running)
		goto unlock;

	drm_modeset_lock_all(drm);
	crtc = dsi->preview_crtc;
	if (crtc->state->mode_blob != dsi->preview_blob) {
		/* screen off, or the HWC set a mode from its list meanwhile */
		pr_info("dw-mipi-dsi: preview over, the mode was changed meanwhile, nothing to restore\n");
		goto done;
	}

	mhz = dw_mipi_dsi_mode_mhz(&dsi->preview_orig);
	ret = dw_mipi_dsi_commit_mode(dsi, crtc, &dsi->preview_orig);
	if (ret) {
		pr_err("dw-mipi-dsi: preview restore to %u.%03u Hz failed (%d), trying the panel's own mode\n",
		       mhz / 1000, mhz % 1000, ret);
		native = dw_mipi_dsi_native_mode(dsi);
		ret = native ? dw_mipi_dsi_commit_mode(dsi, crtc, native) : -ENOENT;
		if (!ret)
			mhz = dw_mipi_dsi_mode_mhz(native);
	}
	if (ret) {
		if (++dsi->preview_tries < DSI_PREVIEW_TRIES) {
			pr_err("dw-mipi-dsi: preview restore failed (%d), again in 1 s\n",
			       ret);
			drm_modeset_unlock_all(drm);
			schedule_delayed_work(&dsi->preview_work,
					msecs_to_jiffies(DSI_PREVIEW_RETRY_MS));
			goto unlock;
		}
		pr_err("dw-mipi-dsi: preview restore failed (%d), giving up\n",
		       ret);
		goto done;
	}
	pr_info("dw-mipi-dsi: preview over, back to %u.%03u Hz\n",
		mhz / 1000, mhz % 1000);
done:
	dw_mipi_dsi_preview_end(dsi);
	drm_modeset_unlock_all(drm);
unlock:
	mutex_unlock(&dsi->preview_lock);
}

static int dw_mipi_dsi_preview_start(struct dw_mipi_dsi_rockchip *dsi,
				     unsigned int rate_mhz)
{
	struct drm_device *drm = dsi->sub_dev.connector->dev;
	struct drm_connector_state *conn_state;
	struct drm_property_blob *orig_blob;
	struct drm_display_mode *native;
	struct drm_display_mode mode = {};
	struct drm_crtc *crtc;
	unsigned int orig_mhz;
	int ret;

	mutex_lock(&dsi->preview_lock);
	if (dsi->preview_running || time_before(jiffies, dsi->preview_next)) {
		ret = -EBUSY;
		goto unlock;
	}

	drm_modeset_lock_all(drm);
	conn_state = dsi->sub_dev.connector->state;
	crtc = conn_state ? conn_state->crtc : NULL;
	if (!crtc || !crtc->state->active || !crtc->state->mode_blob) {
		ret = -ENODEV;
		goto unlock_all;
	}
	native = dw_mipi_dsi_native_mode(dsi);
	if (!native || !native->htotal || !native->vtotal) {
		ret = -ENODEV;
		goto unlock_all;
	}

	/* as panel-simple builds its extra rates: the clock scaled, porches kept */
	drm_mode_copy(&mode, native);
	mode.type = DRM_MODE_TYPE_DRIVER;
	mode.clock = DIV_ROUND_CLOSEST_ULL((u64)native->htotal * native->vtotal *
					   rate_mhz, 1000000);
	if (dw_mipi_dsi_rockchip_mode_valid(dsi, &mode) != MODE_OK) {
		pr_info("dw-mipi-dsi: preview of %u.%03u Hz refused, the DSI lanes can't carry %d kHz\n",
			rate_mhz / 1000, rate_mhz % 1000, mode.clock);
		ret = -ERANGE;
		goto unlock_all;
	}

	drm_mode_copy(&dsi->preview_orig, &crtc->state->mode);
	orig_mhz = dw_mipi_dsi_mode_mhz(&dsi->preview_orig);
	orig_blob = crtc->state->mode_blob;

	ret = dw_mipi_dsi_commit_mode(dsi, crtc, &mode);
	if (ret) {
		pr_info("dw-mipi-dsi: preview of %u.%03u Hz failed (%d)\n",
			rate_mhz / 1000, rate_mhz % 1000, ret);
		/* a failed commit changes nothing, but make sure */
		if (crtc->state->mode_blob != orig_blob)
			dw_mipi_dsi_commit_mode(dsi, crtc, &dsi->preview_orig);
		goto unlock_all;
	}

	dsi->preview_crtc = crtc;
	dsi->preview_blob = drm_property_blob_get(crtc->state->mode_blob);
	dsi->preview_tries = 0;
	dsi->preview_running = true;
	schedule_delayed_work(&dsi->preview_work,
			      msecs_to_jiffies(DSI_PREVIEW_MS));
	pr_info("dw-mipi-dsi: previewing %u.%03u Hz (%d kHz) for 5 s, then back to %u.%03u Hz\n",
		rate_mhz / 1000, rate_mhz % 1000, mode.clock,
		orig_mhz / 1000, orig_mhz % 1000);

unlock_all:
	drm_modeset_unlock_all(drm);
unlock:
	mutex_unlock(&dsi->preview_lock);
	return ret;
}

/* write-only, root only: a rate in Hz like 77.5 to preview, 0 to end it now */
static ssize_t preview_rate_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);
	unsigned int mhz;
	bool running;
	char tmp[16];
	int ret;

	if (sysfs_streq(buf, "0")) {
		mutex_lock(&dsi->preview_lock);
		running = dsi->preview_running;
		if (running)
			mod_delayed_work(system_wq, &dsi->preview_work, 0);
		mutex_unlock(&dsi->preview_lock);
		if (running)
			flush_delayed_work(&dsi->preview_work);
		return count;
	}

	if (count >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	mhz = dw_mipi_dsi_parse_mhz(strim(tmp));
	if (mhz < DSI_PREVIEW_MIN_MHZ || mhz > DSI_PREVIEW_MAX_MHZ)
		return -EINVAL;

	ret = dw_mipi_dsi_preview_start(dsi, mhz);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(preview_rate);


static int dw_mipi_dsi_rockchip_bind(struct device *dev,
				     struct device *master,
				     void *data)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct device *second;
	int ret;

	dev_dbg(dev, "DSI bind begin\n");
	second = dw_mipi_dsi_rockchip_find_second(dsi);
	if (IS_ERR(second))
		return PTR_ERR(second);

	if (second) {
		/* we are the slave in dual-DSI */
		dsi->slave = dev_get_drvdata(second);
		if (!dsi->slave) {
			DRM_DEV_ERROR(dev, "could not get slaves data\n");
			return -ENODEV;
		}

		dsi->slave->is_slave = true;
		dw_mipi_dsi_set_slave(dsi->dmd, dsi->slave->dmd);
		put_device(second);
	}

	if (dsi->is_slave)
		return 0;

	ret = drm_of_find_panel_or_bridge(dsi->dev->of_node, 1, -1,
					  &dsi->panel, &dsi->bridge);
	if (ret) {
		dev_err(dsi->dev, "failed to find panel or bridge: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(dsi->pllref_clk);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to enable pllref_clk: %d\n", ret);
		return ret;
	}

	ret = rockchip_dsi_drm_create_encoder(dsi, drm_dev);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to create drm encoder\n");
		return ret;
	}

	ret = dw_mipi_dsi_bind(dsi->dmd, &dsi->encoder);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to bind: %d\n", ret);
		return ret;
	}

	if (dsi->panel)
		dw_mipi_dsi_get_dsc_info_from_sink(dsi, dsi->panel, NULL);

	dsi->sub_dev.connector = dw_mipi_dsi_get_connector(dsi->dmd);
	if (IS_ERR(dsi->sub_dev.connector)) {
		ret = PTR_ERR(dsi->sub_dev.connector);
		dsi->sub_dev.connector = NULL;
		dw_mipi_dsi_unbind(dsi->dmd);
		clk_disable_unprepare(dsi->pllref_clk);
		return ret;
	}
	if (dsi->sub_dev.connector) {
		dsi->sub_dev.of_node = dev->of_node;
		dsi->sub_dev.loader_protect = dw_mipi_dsi_rockchip_encoder_loader_protect;
		dsi->sub_dev.guarded_panel_init = dsi->cdata->soc_type == PX30;
		rockchip_drm_register_sub_dev(&dsi->sub_dev);
		if (device_create_file(dev, &dev_attr_preview_rate))
			DRM_DEV_ERROR(dev, "Failed to add preview_rate\n");
	}

	return 0;
}

static void dw_mipi_dsi_rockchip_unbind(struct device *dev,
					struct device *master,
					void *data)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);

	if (dsi->is_slave)
		return;

	device_remove_file(dev, &dev_attr_preview_rate);
	cancel_delayed_work_sync(&dsi->preview_work);
	mutex_lock(&dsi->preview_lock);
	if (dsi->preview_running)
		dw_mipi_dsi_preview_end(dsi);
	mutex_unlock(&dsi->preview_lock);

	if (dsi->sub_dev.connector)
		rockchip_drm_unregister_sub_dev(&dsi->sub_dev);

	dw_mipi_dsi_unbind(dsi->dmd);
	dsi->sub_dev.connector = NULL;

	clk_disable_unprepare(dsi->pllref_clk);
}

static const struct component_ops dw_mipi_dsi_rockchip_ops = {
	.bind	= dw_mipi_dsi_rockchip_bind,
	.unbind	= dw_mipi_dsi_rockchip_unbind,
};

static int dw_mipi_dsi_rockchip_component_add(struct dw_mipi_dsi_rockchip *dsi)
{
	int ret;

	ret = component_add(dsi->dev, &dw_mipi_dsi_rockchip_ops);
	if (ret) {
		DRM_DEV_ERROR(dsi->dev, "Failed to register component: %d\n",
					ret);
		return ret;
	}

	return 0;
}

static int dw_mipi_dsi_rockchip_component_del(struct dw_mipi_dsi_rockchip *dsi)
{
	component_del(dsi->dev, &dw_mipi_dsi_rockchip_ops);

	return 0;
}

static void
dw_mipi_dsi_rockchip_stream_standby(void *priv_data, bool standby)
{
	struct dw_mipi_dsi_rockchip *dsi = priv_data;
	struct drm_encoder *encoder = &dsi->encoder;

	rockchip_drm_crtc_standby(encoder->crtc, standby);
}

static int dw_mipi_dsi_rockchip_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct dw_mipi_dsi_rockchip *dsi;
	struct resource *res;
	const struct rockchip_dw_dsi_chip_data *cdata =
				of_device_get_match_data(dev);
	int ret, i;

	dsi = devm_kzalloc(dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dsi->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(dsi->base)) {
		DRM_DEV_ERROR(dev, "Unable to get dsi registers\n");
		return PTR_ERR(dsi->base);
	}

	i = 0;
	while (cdata[i].reg) {
		if (cdata[i].reg == res->start) {
			dsi->cdata = &cdata[i];
			dsi->id = i;
			break;
		}

		i++;
	}

	if (!dsi->cdata) {
		DRM_DEV_ERROR(dev, "no dsi-config for %s node\n", np->name);
		return -EINVAL;
	}

	/* try to get a possible external dphy */
	dsi->phy = devm_phy_optional_get(dev, "dphy");
	if (!dsi->phy)
		dsi->phy = devm_phy_optional_get(dev, "mipi_dphy");
	if (IS_ERR(dsi->phy)) {
		ret = PTR_ERR(dsi->phy);
		DRM_DEV_ERROR(dev, "failed to get mipi dphy: %d\n", ret);
		return ret;
	}

	dsi->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(dsi->pclk)) {
		ret = PTR_ERR(dsi->pclk);
		dev_err(dev, "Unable to get pclk: %d\n", ret);
		return ret;
	}

	dsi->pllref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(dsi->pllref_clk)) {
		if (dsi->phy) {
			/*
			 * if external phy is present, pll will be
			 * generated there.
			 */
			dsi->pllref_clk = NULL;
		} else {
			ret = PTR_ERR(dsi->pllref_clk);
			DRM_DEV_ERROR(dev,
				      "Unable to get pll reference clock: %d\n",
				      ret);
			return ret;
		}
	}

	if (dsi->cdata->flags & DW_MIPI_NEEDS_PHY_CFG_CLK) {
		dsi->phy_cfg_clk = devm_clk_get(dev, "phy_cfg");
		if (IS_ERR(dsi->phy_cfg_clk)) {
			ret = PTR_ERR(dsi->phy_cfg_clk);
			DRM_DEV_ERROR(dev,
				      "Unable to get phy_cfg_clk: %d\n", ret);
			return ret;
		}
	}

	if (dsi->cdata->flags & DW_MIPI_NEEDS_GRF_CLK) {
		dsi->grf_clk = devm_clk_get(dev, "grf");
		if (IS_ERR(dsi->grf_clk)) {
			ret = PTR_ERR(dsi->grf_clk);
			DRM_DEV_ERROR(dev, "Unable to get grf_clk: %d\n", ret);
			return ret;
		}
	}

	if (dsi->cdata->flags & DW_MIPI_NEEDS_HCLK) {
		dsi->hclk = devm_clk_get(dev, "hclk");
		if (IS_ERR(dsi->hclk)) {
			ret = PTR_ERR(dsi->hclk);
			DRM_DEV_ERROR(dev, "Unable to get hclk: %d\n", ret);
			return ret;
		}
	}

	dsi->grf_regmap = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(dsi->grf_regmap)) {
		DRM_DEV_ERROR(dsi->dev, "Unable to get rockchip,grf\n");
		return PTR_ERR(dsi->grf_regmap);
	}

	if (device_property_read_bool(dev, "disable-hold-mode"))
		dsi->disable_hold_mode = true;

	dsi->te_gpio = devm_gpiod_get_optional(dev, "te", GPIOD_IN);
	if (IS_ERR(dsi->te_gpio))
		dsi->te_gpio = NULL;

	if (dsi->te_gpio) {
		ret = devm_request_irq(dev, gpiod_to_irq(dsi->te_gpio),
				       dw_mipi_dsi_te_irq_handler,
				       IRQF_TRIGGER_RISING, "PANEL-TE", dsi);
		if (ret) {
			DRM_DEV_ERROR(dev, "failed to request TE IRQ: %d\n", ret);
			return ret;
		}
	}

	dsi->dev = dev;
	mutex_init(&dsi->preview_lock);
	INIT_DELAYED_WORK(&dsi->preview_work, dw_mipi_dsi_preview_restore);
	dsi->pdata.base = dsi->base;
	dsi->pdata.max_data_lanes = dsi->cdata->max_data_lanes;
	dsi->pdata.phy_ops = &dw_mipi_dsi_rockchip_phy_ops;
	dsi->pdata.host_ops = &dw_mipi_dsi_rockchip_host_ops;
	dsi->pdata.mode_valid = dw_mipi_dsi_rockchip_mode_valid;
	dsi->pdata.priv_data = dsi;
	dsi->pdata.prepare_before_crtc = dsi->cdata->soc_type == PX30;
	dsi->pdata.frame_ready = dw_mipi_dsi_rockchip_frame_ready;
	if (dsi->pdata.prepare_before_crtc) {
		dsi->pdata.quiesce_before_panel_off = dw_mipi_dsi_rockchip_quiesce_panel;
		dsi->pdata.panel_fail = dw_mipi_dsi_rockchip_panel_fail;
	}

	if (dsi->cdata->soc_type == RK3568 || dsi->cdata->soc_type == RK3562)
		dsi->pdata.stream_standby = dw_mipi_dsi_rockchip_stream_standby;

	platform_set_drvdata(pdev, dsi);

	dsi->dmd = dw_mipi_dsi_probe(pdev, &dsi->pdata);
	if (IS_ERR(dsi->dmd)) {
		ret = PTR_ERR(dsi->dmd);
		if (ret != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev,
				      "Failed to probe dw_mipi_dsi: %d\n", ret);
		goto err_clkdisable;
	}

	ret = dw_mipi_dsi_rockchip_component_add(dsi);
	if (ret < 0) {
		dw_mipi_dsi_remove(dsi->dmd);
		goto err_clkdisable;
	}

	return 0;

err_clkdisable:
	clk_disable_unprepare(dsi->pllref_clk);
	return ret;
}

static int dw_mipi_dsi_rockchip_remove(struct platform_device *pdev)
{
	struct dw_mipi_dsi_rockchip *dsi = platform_get_drvdata(pdev);


	dw_mipi_dsi_rockchip_component_del(dsi);
	dw_mipi_dsi_remove(dsi->dmd);

	return 0;
}

static __maybe_unused int dw_mipi_dsi_runtime_suspend(struct device *dev)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);

	clk_disable_unprepare(dsi->grf_clk);
	clk_disable_unprepare(dsi->pclk);
	clk_disable_unprepare(dsi->hclk);
	clk_disable_unprepare(dsi->phy_cfg_clk);

	return 0;
}

static __maybe_unused int dw_mipi_dsi_runtime_resume(struct device *dev)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(dsi->phy_cfg_clk);
	if (ret)
		return ret;
	ret = clk_prepare_enable(dsi->hclk);
	if (ret)
		goto disable_phy_cfg;
	ret = clk_prepare_enable(dsi->pclk);
	if (ret)
		goto disable_hclk;
	ret = clk_prepare_enable(dsi->grf_clk);
	if (!ret)
		return 0;
	clk_disable_unprepare(dsi->pclk);
disable_hclk:
	clk_disable_unprepare(dsi->hclk);
disable_phy_cfg:
	clk_disable_unprepare(dsi->phy_cfg_clk);
	return ret;
}

/*
 * A refresh rate preview still running when the console goes to sleep is
 * ended now, before any device sleeps: its restore is a modeset, and run
 * later it would hit the VOP and DSI with their power domain off.
 */
static int __maybe_unused dw_mipi_dsi_prepare(struct device *dev)
{
	struct dw_mipi_dsi_rockchip *dsi = dev_get_drvdata(dev);
	bool running;

	if (!dsi)
		return 0;
	mutex_lock(&dsi->preview_lock);
	running = dsi->preview_running;
	if (running)
		mod_delayed_work(system_wq, &dsi->preview_work, 0);
	mutex_unlock(&dsi->preview_lock);
	if (running)
		flush_delayed_work(&dsi->preview_work);
	mutex_lock(&dsi->preview_lock);
	running = dsi->preview_running;
	mutex_unlock(&dsi->preview_lock);
	return running ? -EBUSY : 0;
}

static const struct dev_pm_ops dw_mipi_dsi_rockchip_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.prepare = dw_mipi_dsi_prepare,
#endif
	SET_RUNTIME_PM_OPS(dw_mipi_dsi_runtime_suspend,
			   dw_mipi_dsi_runtime_resume, NULL)
};

static const struct rockchip_dw_dsi_chip_data px30_chip_data[] = {
	{
		.reg = 0xff450000,
		.lcdsel_grf_reg = PX30_GRF_PD_VO_CON1,
		.lcdsel_big = HIWORD_UPDATE(0, PX30_DSI_LCDC_SEL),
		.lcdsel_lit = HIWORD_UPDATE(PX30_DSI_LCDC_SEL,
					    PX30_DSI_LCDC_SEL),

		.lanecfg1_grf_reg = PX30_GRF_PD_VO_CON1,
		.lanecfg1 = HIWORD_UPDATE(0, PX30_DSI_TURNDISABLE |
					     PX30_DSI_FORCERXMODE |
					     PX30_DSI_FORCETXSTOPMODE),

		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1000000000UL,
		.soc_type = PX30,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rk3128_chip_data[] = {
	{
		.reg = 0x10110000,
		.lanecfg1_grf_reg = RK3128_GRF_LVDS_CON0,
		.lanecfg1 = HIWORD_UPDATE(0, RK3128_DSI_TURNDISABLE |
					     RK3128_DSI_FORCETXSTOPMODE |
					     RK3128_DSI_FORCERXMODE),
		.flags = DW_MIPI_NEEDS_HCLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1000000000UL,
		.soc_type = RK3128,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rk3288_chip_data[] = {
	{
		.reg = 0xff960000,
		.lcdsel_grf_reg = RK3288_GRF_SOC_CON6,
		.lcdsel_big = HIWORD_UPDATE(0, RK3288_DSI0_LCDC_SEL),
		.lcdsel_lit = HIWORD_UPDATE(RK3288_DSI0_LCDC_SEL, RK3288_DSI0_LCDC_SEL),

		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1500000000UL,
		.soc_type = RK3288,
	},
	{
		.reg = 0xff964000,
		.lcdsel_grf_reg = RK3288_GRF_SOC_CON6,
		.lcdsel_big = HIWORD_UPDATE(0, RK3288_DSI1_LCDC_SEL),
		.lcdsel_lit = HIWORD_UPDATE(RK3288_DSI1_LCDC_SEL, RK3288_DSI1_LCDC_SEL),

		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1500000000UL,
		.soc_type = RK3288,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rk3399_chip_data[] = {
	{
		.reg = 0xff960000,
		.lcdsel_grf_reg = RK3399_GRF_SOC_CON20,
		.lcdsel_big = HIWORD_UPDATE(0, RK3399_DSI0_LCDC_SEL),
		.lcdsel_lit = HIWORD_UPDATE(RK3399_DSI0_LCDC_SEL,
					    RK3399_DSI0_LCDC_SEL),

		.lanecfg1_grf_reg = RK3399_GRF_SOC_CON22,
		.lanecfg1 = HIWORD_UPDATE(0, RK3399_DSI0_TURNREQUEST |
					     RK3399_DSI0_TURNDISABLE |
					     RK3399_DSI0_FORCETXSTOPMODE |
					     RK3399_DSI0_FORCERXMODE),

		.flags = DW_MIPI_NEEDS_PHY_CFG_CLK | DW_MIPI_NEEDS_GRF_CLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1500000000UL,
		.soc_type = RK3399,
	},
	{
		.reg = 0xff968000,
		.lcdsel_grf_reg = RK3399_GRF_SOC_CON20,
		.lcdsel_big = HIWORD_UPDATE(0, RK3399_DSI1_LCDC_SEL),
		.lcdsel_lit = HIWORD_UPDATE(RK3399_DSI1_LCDC_SEL,
					    RK3399_DSI1_LCDC_SEL),

		.lanecfg1_grf_reg = RK3399_GRF_SOC_CON23,
		.lanecfg1 = HIWORD_UPDATE(0, RK3399_DSI1_TURNDISABLE |
					     RK3399_DSI1_FORCETXSTOPMODE |
					     RK3399_DSI1_FORCERXMODE |
					     RK3399_DSI1_ENABLE),

		.lanecfg2_grf_reg = RK3399_GRF_SOC_CON24,
		.lanecfg2 = HIWORD_UPDATE(RK3399_TXRX_MASTERSLAVEZ |
					  RK3399_TXRX_ENABLECLK,
					  RK3399_TXRX_MASTERSLAVEZ |
					  RK3399_TXRX_ENABLECLK |
					  RK3399_TXRX_BASEDIR),

		.enable_grf_reg = RK3399_GRF_SOC_CON23,
		.enable = HIWORD_UPDATE(RK3399_DSI1_ENABLE, RK3399_DSI1_ENABLE),

		.flags = DW_MIPI_NEEDS_PHY_CFG_CLK | DW_MIPI_NEEDS_GRF_CLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1500000000UL,
		.soc_type = RK3399,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rk3562_chip_data[] = {
	{
		.reg = 0xffb10000,

		.lanecfg1_grf_reg = RK3562_SYS_GRF_VO_CON1,
		.lanecfg1 = HIWORD_UPDATE(0, RK3562_DSI_TURNDISABLE |
					     RK3562_DSI_FORCERXMODE |
					     RK3562_DSI_FORCETXSTOPMODE),

		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1200000000UL,
		.soc_type = RK3562,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rk3568_chip_data[] = {
	{
		.reg = 0xfe060000,

		.lanecfg1_grf_reg = RK3568_GRF_VO_CON2,
		.lanecfg1 = HIWORD_UPDATE(0, RK3568_DSI_TURNDISABLE |
					     RK3568_DSI_FORCERXMODE |
					     RK3568_DSI_FORCETXSTOPMODE),

		.flags = DW_MIPI_NEEDS_HCLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1200000000UL,
		.soc_type = RK3568,
	},
	{
		.reg = 0xfe070000,

		.lanecfg1_grf_reg = RK3568_GRF_VO_CON3,
		.lanecfg1 = HIWORD_UPDATE(0, RK3568_DSI_TURNDISABLE |
					     RK3568_DSI_FORCERXMODE |
					     RK3568_DSI_FORCETXSTOPMODE),

		.flags = DW_MIPI_NEEDS_HCLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1200000000UL,
		.soc_type = RK3568,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dw_dsi_chip_data rv1126_chip_data[] = {
	{
		.reg = 0xffb30000,

		.lanecfg1_grf_reg = RV1126_GRF_DSIPHY_CON,
		.lanecfg1 = HIWORD_UPDATE(0, RV1126_DSI_TURNDISABLE |
					     RV1126_DSI_FORCERXMODE |
					     RV1126_DSI_FORCETXSTOPMODE),
		.flags = DW_MIPI_NEEDS_HCLK,
		.max_data_lanes = 4,
		.max_bit_rate_per_lane = 1000000000UL,
		.soc_type = RV1126,
	},
	{ /* sentinel */ }
};

static const struct of_device_id dw_mipi_dsi_rockchip_dt_ids[] = {
	{
	 .compatible = "rockchip,px30-mipi-dsi",
	 .data = &px30_chip_data,
	}, {
	 .compatible = "rockchip,rk3128-mipi-dsi",
	 .data = &rk3128_chip_data,
	}, {
	 .compatible = "rockchip,rk3288-mipi-dsi",
	 .data = &rk3288_chip_data,
	}, {
	 .compatible = "rockchip,rk3399-mipi-dsi",
	 .data = &rk3399_chip_data,
	}, {
	 .compatible = "rockchip,rk3562-mipi-dsi",
	 .data = &rk3562_chip_data,
	}, {
	 .compatible = "rockchip,rk3568-mipi-dsi",
	 .data = &rk3568_chip_data,
	}, {
	 .compatible = "rockchip,rv1126-mipi-dsi",
	 .data = &rv1126_chip_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dw_mipi_dsi_rockchip_dt_ids);

struct platform_driver dw_mipi_dsi_rockchip_driver = {
	.probe		= dw_mipi_dsi_rockchip_probe,
	.remove		= dw_mipi_dsi_rockchip_remove,
	.driver		= {
		.of_match_table = dw_mipi_dsi_rockchip_dt_ids,
		.pm = &dw_mipi_dsi_rockchip_pm_ops,
		.name	= "dw-mipi-dsi-rockchip",
		/*
		 * For dual-DSI display, one DSI pokes at the other DSI's
		 * drvdata in dw_mipi_dsi_rockchip_find_second(). This is not
		 * safe for asynchronous probe.
		 */
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
	},
};
