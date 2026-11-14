/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PHY_ROCKCHIP_LOGO_H
#define _PHY_ROCKCHIP_LOGO_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct phy;
#if IS_REACHABLE(CONFIG_PHY_ROCKCHIP_INNO_VIDEO_COMBO_PHY)
int inno_video_phy_hold_loader(struct phy *phy,bool on);
#else
static inline int inno_video_phy_hold_loader(struct phy *p,bool on){return -1;}
#endif
#endif
