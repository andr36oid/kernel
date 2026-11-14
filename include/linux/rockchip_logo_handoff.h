/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_ROCKCHIP_LOGO_HANDOFF_H
#define _LINUX_ROCKCHIP_LOGO_HANDOFF_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct device_node;
struct drm_display_mode;
struct drm_connector;
#if IS_REACHABLE(CONFIG_DRM_ROCKCHIP)
bool rockchip_logo_backlight_handoff(struct device_node *node);
bool rockchip_logo_backlight_guarded(struct device_node *node);
bool rockchip_logo_output_ready(void);
bool rockchip_logo_prefer_initial_mode(struct drm_connector *connector);
bool rockchip_logo_phy_adopting(void);
int rockchip_logo_copy_proof(void *record);
int rockchip_logo_check_registers(unsigned int first,unsigned int count);
int rockchip_logo_match_mode(const struct drm_display_mode *mode);
void rockchip_logo_reject(void);
void rockchip_logo_adopting_phy(bool on);
void rockchip_logo_first_frame(void);
#else
static inline bool rockchip_logo_backlight_handoff(struct device_node *n){return false;}
static inline bool rockchip_logo_backlight_guarded(struct device_node *n){return false;}
static inline bool rockchip_logo_output_ready(void){return false;}
static inline bool rockchip_logo_prefer_initial_mode(struct drm_connector *c){return false;}
static inline bool rockchip_logo_phy_adopting(void){return false;}
static inline int rockchip_logo_copy_proof(void *p){return -1;}
static inline int rockchip_logo_check_registers(unsigned int f,unsigned int n){return -1;}
static inline int rockchip_logo_match_mode(const struct drm_display_mode *m){return -1;}
static inline void rockchip_logo_reject(void){}
static inline void rockchip_logo_adopting_phy(bool on){}
static inline void rockchip_logo_first_frame(void){}
#endif
#endif
