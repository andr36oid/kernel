/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ROCKCHIP_PANEL_SAFETY_H__
#define __ROCKCHIP_PANEL_SAFETY_H__

#include <linux/types.h>
#include <linux/compiler.h>

/* No device I/O here: callers supply checked operations. This same protocol is
 * exercised by the VM fault fixture, including the boot-lifetime error latch.
 */
enum rockchip_panel_safe_step {
	RK_PANEL_SAFE_NONE,
	RK_PANEL_SAFE_BLANK,
	RK_PANEL_SAFE_STOP,
	RK_PANEL_SAFE_CLOCK,
	RK_PANEL_SAFE_PREPARE,
};

struct rockchip_panel_safe_state {
	int error;
	enum rockchip_panel_safe_step step;
	bool reported;
};

struct rockchip_panel_safe_ops {
	int (*blank)(void *data);
	int (*stop)(void *data);
	int (*clock)(void *data);
	int (*prepare)(void *data);
	void (*abort)(void *data, int error);
};

static inline int
rockchip_panel_safe_latch(struct rockchip_panel_safe_state *s,
			 enum rockchip_panel_safe_step step, int error)
{
	if (error && !READ_ONCE(s->error)) {
		WRITE_ONCE(s->error, error);
		s->step = step;
	}
	return READ_ONCE(s->error);
}

static inline const char *
rockchip_panel_safe_step_name(enum rockchip_panel_safe_step step)
{
	switch (step) {
	case RK_PANEL_SAFE_BLANK: return "backlight-blank";
	case RK_PANEL_SAFE_STOP: return "vop-hold-ack";
	case RK_PANEL_SAFE_CLOCK: return "pixel-clock";
	case RK_PANEL_SAFE_PREPARE: return "phy-panel-init";
	default: return "none";
	}
}

static inline int
rockchip_panel_safe_run(struct rockchip_panel_safe_state *s,
			 const struct rockchip_panel_safe_ops *ops, void *data)
{
	int blank, stop, ret;

	/* Never automatically retry even a full modeset after a failed attempt. */
	if (READ_ONCE(s->error))
		return READ_ONCE(s->error);
	blank = ops->blank(data);
	/* A failed backlight operation must not prevent trying to stop scanout. */
	stop = ops->stop(data);
	if (stop)
		return rockchip_panel_safe_latch(s, RK_PANEL_SAFE_STOP, stop);
	if (blank) {
		ret = rockchip_panel_safe_latch(s, RK_PANEL_SAFE_BLANK, blank);
		goto abort;
	}
	ret = ops->clock(data);
	if (ret) {
		ret = rockchip_panel_safe_latch(s, RK_PANEL_SAFE_CLOCK, ret);
		goto abort;
	}
	ret = ops->prepare(data);
	if (ret) {
		ret = rockchip_panel_safe_latch(s, RK_PANEL_SAFE_PREPARE, ret);
		goto abort;
	}
	return 0;
abort:
	/* Only an acknowledged stop authorizes GPIO reset/power/link teardown. */
	ops->abort(data, ret);
	return ret;
}
#endif
