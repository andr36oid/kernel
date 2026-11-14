/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) STMicroelectronics SA 2017
 *
 * Authors: Philippe Cornu <philippe.cornu@st.com>
 *          Yannick Fertre <yannick.fertre@st.com>
 */

#ifndef __DW_MIPI_DSI__
#define __DW_MIPI_DSI__

#include <linux/types.h>

#include <drm/drm_modes.h>

struct drm_display_mode;
struct drm_encoder;
struct drm_bridge;
struct drm_atomic_state;
struct dw_mipi_dsi;
struct mipi_dsi_device;
struct platform_device;

struct dw_mipi_dsi_dphy_timing {
	u16 data_hs2lp;
	u16 data_lp2hs;
	u16 clk_hs2lp;
	u16 clk_lp2hs;
};

struct dw_mipi_dsi_phy_ops {
	int (*adopt_loader)(void *priv_data);
	int (*hold_loader)(void *priv_data, bool on);
	int (*init)(void *priv_data);
	void (*power_on)(void *priv_data);
	int (*power_on_checked)(void *priv_data);
	int (*power_off_checked)(void *priv_data);
	void (*power_off)(void *priv_data);
	int (*get_lane_mbps)(void *priv_data,
			     const struct drm_display_mode *mode,
			     unsigned long mode_flags, u32 lanes, u32 format,
			     unsigned int *lane_mbps);
	int (*get_timing)(void *priv_data, unsigned int lane_mbps,
			  struct dw_mipi_dsi_dphy_timing *timing);
	int (*get_esc_clk_rate)(void *priv_data, unsigned int *esc_clk_rate);
};

struct dw_mipi_dsi_host_ops {
	int (*attach)(void *priv_data,
		      struct mipi_dsi_device *dsi);
	int (*detach)(void *priv_data,
		      struct mipi_dsi_device *dsi);
};

struct dw_mipi_dsi_plat_data {
	bool prepare_before_crtc;
	int (*frame_ready)(void *priv_data);
	int (*quiesce_before_panel_off)(void *priv_data);
	void (*panel_fail)(void *priv_data, int error);
	void __iomem *base;
	unsigned int max_data_lanes;

	enum drm_mode_status (*mode_valid)(void *priv_data,
					   const struct drm_display_mode *mode);

	const struct dw_mipi_dsi_phy_ops *phy_ops;
	const struct dw_mipi_dsi_host_ops *host_ops;
	void (*stream_standby)(void *priv_data, bool standby);

	void *priv_data;
};

struct dw_mipi_dsi *dw_mipi_dsi_probe(struct platform_device *pdev,
				      const struct dw_mipi_dsi_plat_data
				      *plat_data);
void dw_mipi_dsi_remove(struct dw_mipi_dsi *dsi);
int dw_mipi_dsi_bind(struct dw_mipi_dsi *dsi, struct drm_encoder *encoder);
void dw_mipi_dsi_unbind(struct dw_mipi_dsi *dsi);
void dw_mipi_dsi_set_slave(struct dw_mipi_dsi *dsi, struct dw_mipi_dsi *slave);
struct drm_connector *dw_mipi_dsi_get_connector(struct dw_mipi_dsi *dsi);

#if IS_REACHABLE(CONFIG_DRM_DW_MIPI_DSI)
bool dw_mipi_dsi_needs_checked_prepare(struct drm_bridge *bridge);
int dw_mipi_dsi_output_is_off(struct drm_bridge *bridge);
int dw_mipi_dsi_adopt_loader(struct drm_bridge *bridge, const struct drm_display_mode *mode);
int dw_mipi_dsi_blank_before_prepare(struct drm_bridge *bridge);
void dw_mipi_dsi_abort_before_crtc(struct drm_bridge *bridge, int error);
int dw_mipi_dsi_prepare_before_crtc(struct drm_bridge *bridge,
				 struct drm_atomic_state *state);
#else
static inline int dw_mipi_dsi_adopt_loader(struct drm_bridge *b,const struct drm_display_mode *m)
{return -1;}
static inline int dw_mipi_dsi_output_is_off(struct drm_bridge *b){return 0;}
static inline bool dw_mipi_dsi_needs_checked_prepare(struct drm_bridge *bridge)
{
	return false;
}
static inline void dw_mipi_dsi_abort_before_crtc(struct drm_bridge *bridge, int error)
{
}
static inline int dw_mipi_dsi_blank_before_prepare(struct drm_bridge *bridge)
{
	return 0;
}
static inline int dw_mipi_dsi_prepare_before_crtc(struct drm_bridge *bridge,
					       struct drm_atomic_state *state)
{
	return 0;
}
#endif
#endif /* __DW_MIPI_DSI__ */
