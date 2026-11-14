// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple PWM based backlight control, board code has to setup
 * 1) pin configuration so PWM waveforms can output
 * 2) platform_data being correctly configured
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/rockchip_logo_handoff.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

static bool bl_quiescent;
module_param_named(quiescent, bl_quiescent, bool, 0600);
MODULE_PARM_DESC(quiescent,
		 "pwm bl quiescent when reboot quiescent [default=false]");

struct pwm_bl_data {
	bool proof_handoff, guard_output, preserve_first_update;
	int boot_error;
	struct pwm_device	*pwm;
	struct device		*dev;
	unsigned int		lth_brightness;
	unsigned int		*levels;
	bool			enabled;
	struct regulator	*power_supply;
	struct gpio_desc	*enable_gpio;
	unsigned int		scale;
	bool			legacy;
	unsigned int		post_pwm_on_delay;
	unsigned int		pwm_off_delay;
	u64			kick_duty;
	unsigned int		kick_ms;
	int			(*notify)(struct device *,
					  int brightness);
	void			(*notify_after)(struct device *,
					int brightness);
	int			(*check_fb)(struct device *, struct fb_info *);
	void			(*exit)(struct device *);
};

/* Safety-scoped readback: pwm_get_state() alone is the framework cache.
 * Rockchip's provider callback acquires pclk and reads actual enable registers.
 * A callback which cannot read leaves the deliberately opposite sentinel.
 */
static int pwm_backlight_verify(struct pwm_bl_data *pb, bool enabled)
{
 struct pwm_state observed = { .enabled = !enabled };
 if (!pb->pwm->chip->ops->get_state)
  return -EOPNOTSUPP;
 pb->pwm->chip->ops->get_state(pb->pwm->chip, pb->pwm, &observed);
 if (observed.enabled != enabled || (enabled && !observed.period))
  return -EIO;
 return 0;
}
static int pwm_backlight_checked_off(struct pwm_bl_data *pb)
{
 struct pwm_state state;
 int error=0, ret;
 pwm_get_state(pb->pwm,&state);
 if(pb->enable_gpio) {
  ret=gpiod_direction_output(pb->enable_gpio,0);
  if(ret)error=ret;
  else if(gpiod_get_value_cansleep(pb->enable_gpio)!=0)error=-EIO;
 }
 if(pb->pwm_off_delay)msleep(pb->pwm_off_delay);
 state.enabled=false;state.duty_cycle=0;
 ret=pwm_apply_state(pb->pwm,&state);if(ret&&!error)error=ret;
 ret=pwm_backlight_verify(pb,false);if(ret&&!error)error=ret;
 if(pb->enabled) {
  ret=regulator_disable(pb->power_supply);
  if(ret&&!error)error=ret;
  if(!ret)pb->enabled=false;
 }
 return error;
}
static int pwm_backlight_checked_on(struct pwm_bl_data *pb)
{
 struct pwm_state state;int ret;
 if(pb->enabled)return pwm_backlight_verify(pb,true);
 ret=regulator_enable(pb->power_supply);if(ret)return ret;
 pb->enabled=true; /* Own exactly one regulator reference, even on unwind. */
 pwm_get_state(pb->pwm,&state);state.enabled=true;
 ret=pwm_apply_state(pb->pwm,&state);if(ret)goto fail;
 ret=pwm_backlight_verify(pb,true);if(ret)goto fail;
 if(pb->post_pwm_on_delay)msleep(pb->post_pwm_on_delay);
 if(pb->enable_gpio) {
  ret=gpiod_direction_output(pb->enable_gpio,1);if(ret)goto fail;
  if(gpiod_get_value_cansleep(pb->enable_gpio)!=1){ret=-EIO;goto fail;}
 }
 return 0;
fail:
 (void)pwm_backlight_checked_off(pb);
 return ret;
}

static int pwm_backlight_power_on(struct pwm_bl_data *pb)
{
	struct pwm_state state;
	int err;
	if (pb->guard_output) {
		err = pwm_backlight_checked_on(pb);
		if (err && !pb->boot_error)
			pb->boot_error = err;
		return err;
	}

	pwm_get_state(pb->pwm, &state);
	if (pb->enabled)
		return 0;

	err = regulator_enable(pb->power_supply);
	if (err < 0)
		dev_err(pb->dev, "failed to enable power supply\n");

	state.enabled = true;
	pwm_apply_state(pb->pwm, &state);

	if (pb->post_pwm_on_delay)
		msleep(pb->post_pwm_on_delay);

	if (pb->enable_gpio)
		gpiod_set_value_cansleep(pb->enable_gpio, 1);

	pb->enabled = true;
	return 0;
}

static int pwm_backlight_power_off(struct pwm_bl_data *pb)
{
	struct pwm_state state;
	int ret;
	if (pb->guard_output) {
		ret = pwm_backlight_checked_off(pb);
		if (ret && !pb->boot_error)
			pb->boot_error = ret;
		return ret;
	}

	pwm_get_state(pb->pwm, &state);
	if (!pb->enabled && !state.enabled)
		return 0;

	if (pb->enable_gpio)
		gpiod_set_value_cansleep(pb->enable_gpio, 0);

	if (pb->pwm_off_delay)
		msleep(pb->pwm_off_delay);

	state.enabled = false;
	state.duty_cycle = 0;
	pwm_apply_state(pb->pwm, &state);

	/* A bootloader-enabled PWM does not give us a regulator use count. */
	if (pb->enabled)
		regulator_disable(pb->power_supply);
	pb->enabled = false;
	return 0;
}

static int compute_duty_cycle(struct pwm_bl_data *pb, int brightness)
{
	unsigned int lth = pb->lth_brightness;
	struct pwm_state state;
	u64 duty_cycle;

	pwm_get_state(pb->pwm, &state);

	if (pb->levels)
		duty_cycle = pb->levels[brightness];
	else
		duty_cycle = brightness;

	duty_cycle *= state.period - lth;
	do_div(duty_cycle, pb->scale);

	return duty_cycle + lth;
}

static int pwm_backlight_update_status(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = bl_get_data(bl);
	int brightness = backlight_get_brightness(bl);
	struct pwm_state state;
	int ret;

	if (pb->guard_output && pb->boot_error)
		return pb->boot_error;

	if (pb->notify)
		brightness = pb->notify(pb->dev, brightness);
	/* No app/property/fbcon update can expose a cold, unprepared DSI panel.
	 * A pending producer proof only preserves the already-initialized loader
	 * picture; the late live verifier must still accept it before adoption.
	 */
	if (pb->guard_output && !rockchip_logo_output_ready() && !pb->proof_handoff)
		brightness = 0;
	if (pb->preserve_first_update && brightness > 0) {
		pb->preserve_first_update = false;
		return 0;
	}
	if (brightness == 0) {
		pb->proof_handoff = false;
		pb->preserve_first_update = false;
	}


	if (brightness > 0) {
		u64 duty_cycle = compute_duty_cycle(pb, brightness);

		pwm_get_state(pb->pwm, &state);

		if (!pb->enabled && pb->kick_ms && duty_cycle < pb->kick_duty) {
			state.duty_cycle = pb->kick_duty;
			ret = pwm_apply_state(pb->pwm, &state);
			if (ret && pb->guard_output) {
				pb->boot_error = ret;
				return ret;
			}
			ret = pwm_backlight_power_on(pb);
			if (ret)
				return ret;
			msleep(pb->kick_ms);
			/* Preserve enabled=true after the pulse, restoring only duty. */
			pwm_get_state(pb->pwm, &state);
		}
		state.duty_cycle = duty_cycle;
		ret = pwm_apply_state(pb->pwm, &state);
			if (ret && pb->guard_output) {
				pb->boot_error = ret;
				return ret;
			}
		ret = pwm_backlight_power_on(pb);
			if (ret)
				return ret;
	} else {
		ret = pwm_backlight_power_off(pb);
		if (ret)
			return ret;
	}

	if (pb->notify_after)
		pb->notify_after(pb->dev, brightness);

	return 0;
}

static int pwm_backlight_check_fb(struct backlight_device *bl,
				  struct fb_info *info)
{
	struct pwm_bl_data *pb = bl_get_data(bl);

	return !pb->check_fb || pb->check_fb(pb->dev, info);
}

static const struct backlight_ops pwm_backlight_ops = {
	.update_status	= pwm_backlight_update_status,
	.check_fb	= pwm_backlight_check_fb,
};

#ifdef CONFIG_OF
#define PWM_LUMINANCE_SHIFT	16
#define PWM_LUMINANCE_SCALE	(1 << PWM_LUMINANCE_SHIFT) /* luminance scale */

/*
 * CIE lightness to PWM conversion.
 *
 * The CIE 1931 lightness formula is what actually describes how we perceive
 * light:
 *          Y = (L* / 903.3)           if L* ≤ 8
 *          Y = ((L* + 16) / 116)^3    if L* > 8
 *
 * Where Y is the luminance, the amount of light coming out of the screen, and
 * is a number between 0.0 and 1.0; and L* is the lightness, how bright a human
 * perceives the screen to be, and is a number between 0 and 100.
 *
 * The following function does the fixed point maths needed to implement the
 * above formula.
 */
static u64 cie1931(unsigned int lightness)
{
	u64 retval;

	/*
	 * @lightness is given as a number between 0 and 1, expressed
	 * as a fixed-point number in scale
	 * PWM_LUMINANCE_SCALE. Convert to a percentage, still
	 * expressed as a fixed-point number, so the above formulas
	 * can be applied.
	 */
	lightness *= 100;
	if (lightness <= (8 * PWM_LUMINANCE_SCALE)) {
		retval = DIV_ROUND_CLOSEST(lightness * 10, 9033);
	} else {
		retval = (lightness + (16 * PWM_LUMINANCE_SCALE)) / 116;
		retval *= retval * retval;
		retval += 1ULL << (2*PWM_LUMINANCE_SHIFT - 1);
		retval >>= 2*PWM_LUMINANCE_SHIFT;
	}

	return retval;
}

/*
 * Create a default correction table for PWM values to create linear brightness
 * for LED based backlights using the CIE1931 algorithm.
 */
static
int pwm_backlight_brightness_default(struct device *dev,
				     struct platform_pwm_backlight_data *data,
				     unsigned int period)
{
	unsigned int i;
	u64 retval;

	/*
	 * Once we have 4096 levels there's little point going much higher...
	 * neither interactive sliders nor animation benefits from having
	 * more values in the table.
	 */
	data->max_brightness =
		min((int)DIV_ROUND_UP(period, fls(period)), 4096);

	data->levels = devm_kcalloc(dev, data->max_brightness,
				    sizeof(*data->levels), GFP_KERNEL);
	if (!data->levels)
		return -ENOMEM;

	/* Fill the table using the cie1931 algorithm */
	for (i = 0; i < data->max_brightness; i++) {
		retval = cie1931((i * PWM_LUMINANCE_SCALE) /
				 data->max_brightness) * period;
		retval = DIV_ROUND_CLOSEST_ULL(retval, PWM_LUMINANCE_SCALE);
		if (retval > UINT_MAX)
			return -EINVAL;
		data->levels[i] = (unsigned int)retval;
	}

	data->dft_brightness = data->max_brightness / 2;
	data->max_brightness--;

	return 0;
}

static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data)
{
	struct device_node *node = dev->of_node;
	unsigned int num_levels = 0;
	unsigned int levels_count;
	unsigned int num_steps = 0;
	struct property *prop;
	unsigned int *table;
	int length;
	u32 value;
	int ret;

	if (!node)
		return -ENODEV;

	memset(data, 0, sizeof(*data));

	/*
	 * These values are optional and set as 0 by default, the out values
	 * are modified only if a valid u32 value can be decoded.
	 */
	of_property_read_u32(node, "post-pwm-on-delay-ms",
			     &data->post_pwm_on_delay);
	of_property_read_u32(node, "pwm-off-delay-ms", &data->pwm_off_delay);

	/*
	 * Determine the number of brightness levels, if this property is not
	 * set a default table of brightness levels will be used.
	 */
	prop = of_find_property(node, "brightness-levels", &length);
	if (!prop)
		return 0;

	data->max_brightness = length / sizeof(u32);

	/* read brightness levels from DT property */
	if (data->max_brightness > 0) {
		size_t size = sizeof(*data->levels) * data->max_brightness;
		unsigned int i, j, n = 0;

		data->levels = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!data->levels)
			return -ENOMEM;

		ret = of_property_read_u32_array(node, "brightness-levels",
						 data->levels,
						 data->max_brightness);
		if (ret < 0)
			return ret;

		ret = of_property_read_u32(node, "default-brightness-level",
					   &value);
		if (ret < 0)
			return ret;

		data->dft_brightness = value;

		/*
		 * This property is optional, if is set enables linear
		 * interpolation between each of the values of brightness levels
		 * and creates a new pre-computed table.
		 */
		of_property_read_u32(node, "num-interpolated-steps",
				     &num_steps);

		/*
		 * Make sure that there is at least two entries in the
		 * brightness-levels table, otherwise we can't interpolate
		 * between two points.
		 */
		if (num_steps) {
			if (data->max_brightness < 2) {
				dev_err(dev, "can't interpolate\n");
				return -EINVAL;
			}

			/*
			 * Recalculate the number of brightness levels, now
			 * taking in consideration the number of interpolated
			 * steps between two levels.
			 */
			for (i = 0; i < data->max_brightness - 1; i++) {
				if ((data->levels[i + 1] - data->levels[i]) /
				   num_steps)
					num_levels += num_steps;
				else
					num_levels++;
			}
			num_levels++;
			dev_dbg(dev, "new number of brightness levels: %d\n",
				num_levels);

			/*
			 * Create a new table of brightness levels with all the
			 * interpolated steps.
			 */
			size = sizeof(*table) * num_levels;
			table = devm_kzalloc(dev, size, GFP_KERNEL);
			if (!table)
				return -ENOMEM;

			/* Fill the interpolated table. */
			levels_count = 0;
			for (i = 0; i < data->max_brightness - 1; i++) {
				value = data->levels[i];
				n = (data->levels[i + 1] - value) / num_steps;
				if (n > 0) {
					for (j = 0; j < num_steps; j++) {
						table[levels_count] = value;
						value += n;
						levels_count++;
					}
				} else {
					table[levels_count] = data->levels[i];
					levels_count++;
				}
			}
			table[levels_count] = data->levels[i];

			/*
			 * As we use interpolation lets remove current
			 * brightness levels table and replace for the
			 * new interpolated table.
			 */
			devm_kfree(dev, data->levels);
			data->levels = table;

			/*
			 * Reassign max_brightness value to the new total number
			 * of brightness levels.
			 */
			data->max_brightness = num_levels;
		}

		data->max_brightness--;
	}

	return 0;
}

static const struct of_device_id pwm_backlight_of_match[] = {
	{ .compatible = "pwm-backlight" },
	{ }
};

MODULE_DEVICE_TABLE(of, pwm_backlight_of_match);
#else
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data)
{
	return -ENODEV;
}

static
int pwm_backlight_brightness_default(struct device *dev,
				     struct platform_pwm_backlight_data *data,
				     unsigned int period)
{
	return -ENODEV;
}
#endif

static bool pwm_backlight_is_linear(struct platform_pwm_backlight_data *data)
{
	unsigned int nlevels = data->max_brightness + 1;
	unsigned int min_val = data->levels[0];
	unsigned int max_val = data->levels[nlevels - 1];
	/*
	 * Multiplying by 128 means that even in pathological cases such
	 * as (max_val - min_val) == nlevels the error at max_val is less
	 * than 1%.
	 */
	unsigned int slope = (128 * (max_val - min_val)) / nlevels;
	unsigned int margin = (max_val - min_val) / 20; /* 5% */
	int i;

	for (i = 1; i < nlevels; i++) {
		unsigned int linear_value = min_val + ((i * slope) / 128);
		unsigned int delta = abs(linear_value - data->levels[i]);

		if (delta > margin)
			return false;
	}

	return true;
}

static int pwm_backlight_initial_power_state(const struct pwm_bl_data *pb)
{
	struct device_node *node = pb->dev->of_node;
	bool active = true;

	/* Boards opting in hand control to drm_panel's backlight helpers.
	 * In particular, do not light a bootloader-initialized but unscanned
	 * panel while the kernel is rebuilding its display pipeline.
	 */
	if ((pb->guard_output && !pb->proof_handoff) ||
	    (node && of_property_read_bool(node, "andr36oid,wait-for-panel") &&
	     !pb->proof_handoff)) {
		dev_info(pb->dev, "andr36oid: backlight held off until panel enable\n");
		return FB_BLANK_POWERDOWN;
	}

	/*
	 * If the enable GPIO is present, observable (either as input
	 * or output) and off then the backlight is not currently active.
	 * */
	if (pb->enable_gpio && gpiod_get_value_cansleep(pb->enable_gpio) == 0)
		active = false;

	if (!regulator_is_enabled(pb->power_supply))
		active = false;

	if (!pwm_is_enabled(pb->pwm))
		active = false;

	/*
	 * Synchronize the enable_gpio with the observed state of the
	 * hardware.
	 */
	if (pb->enable_gpio)
		gpiod_direction_output(pb->enable_gpio, active);

	/*
	 * Do not change pb->enabled here! pb->enabled essentially
	 * tells us if we own one of the regulator's use counts and
	 * right now we do not.
	 */

	/* Not booted with device tree or no phandle link to the node */
	if (!node || !node->phandle)
		return FB_BLANK_UNBLANK;

	/*
	 * If the driver is probed from the device tree and there is a
	 * phandle link pointing to the backlight node, it is safe to
	 * assume that another driver will enable the backlight at the
	 * appropriate time. Therefore, if it is disabled, keep it so.
	 */
	return active ? FB_BLANK_UNBLANK: FB_BLANK_POWERDOWN;
}

static int pwm_backlight_probe(struct platform_device *pdev)
{
	struct platform_pwm_backlight_data *data = dev_get_platdata(&pdev->dev);
	struct platform_pwm_backlight_data defdata;
	struct backlight_properties props;
	struct backlight_device *bl;
	struct device_node *node = pdev->dev.of_node;
	struct pwm_bl_data *pb;
	struct pwm_state state;
	unsigned int i;
	int ret;

	if (!data) {
		ret = pwm_backlight_parse_dt(&pdev->dev, &defdata);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to find platform data\n");
			return ret;
		}

		data = &defdata;
	}

	if (data->init) {
		ret = data->init(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
	if (!pb) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	pb->notify = data->notify;
	pb->notify_after = data->notify_after;
	pb->check_fb = data->check_fb;
	pb->exit = data->exit;
	pb->dev = &pdev->dev;
	pb->enabled = false;
	pb->post_pwm_on_delay = data->post_pwm_on_delay;
	pb->pwm_off_delay = data->pwm_off_delay;

	pb->enable_gpio = devm_gpiod_get_optional(&pdev->dev, "enable",
						  GPIOD_ASIS);
	if (IS_ERR(pb->enable_gpio)) {
		ret = PTR_ERR(pb->enable_gpio);
		goto err_alloc;
	}

	pb->power_supply = devm_regulator_get(&pdev->dev, "power");
	if (IS_ERR(pb->power_supply)) {
		ret = PTR_ERR(pb->power_supply);
		goto err_alloc;
	}

	pb->pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(pb->pwm) && PTR_ERR(pb->pwm) != -EPROBE_DEFER && !node) {
		dev_err(&pdev->dev, "unable to request PWM, trying legacy API\n");
		pb->legacy = true;
		pb->pwm = pwm_request(data->pwm_id, "pwm-backlight");
	}

	if (IS_ERR(pb->pwm)) {
		ret = PTR_ERR(pb->pwm);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "unable to request PWM\n");
		goto err_alloc;
	}

	dev_dbg(&pdev->dev, "got pwm for backlight\n");

	pb->guard_output = rockchip_logo_backlight_guarded(node);
	pb->proof_handoff = !bl_quiescent &&
		rockchip_logo_backlight_handoff(node);
	pwm_get_state(pb->pwm, &state);
	if (pb->proof_handoff) {
		struct pwm_state observed = { 0 };
		if (pb->pwm->chip->ops->get_state)
			pb->pwm->chip->ops->get_state(pb->pwm->chip, pb->pwm, &observed);
		state = observed;
	}
	if (pb->proof_handoff && (!state.enabled || !state.period ||
	    state.period != pb->pwm->args.period ||
	    state.polarity != pb->pwm->args.polarity ||
	    !state.duty_cycle || state.duty_cycle > state.period)) {
		rockchip_logo_reject();
		pb->proof_handoff = false;
	}
	if (pb->proof_handoff) {
		ret = regulator_is_enabled(pb->power_supply);
		if (ret <= 0 || (pb->enable_gpio &&
		    gpiod_get_value_cansleep(pb->enable_gpio) != 1)) {
			rockchip_logo_reject();
			pb->proof_handoff = false;
		}
	}
	if (pb->proof_handoff) {
		ret = regulator_enable(pb->power_supply);
		if (ret) {
			rockchip_logo_reject();
			pb->proof_handoff = false;
		} else {
			pb->enabled = true; /* One balanced regulator reference. */
			pb->preserve_first_update = true;
			dev_info(pb->dev, "LOGO32: preserving proven loader PWM duty\n");
		}
	}
	/* Sync up PWM state only when not preserving a proven live signal. */
	if (!pb->proof_handoff)
		pwm_init_state(pb->pwm, &state);

	/*
	 * The DT case will set the pwm_period_ns field to 0 and store the
	 * period, parsed from the DT, in the PWM device. For the non-DT case,
	 * set the period from platform data if it has not already been set
	 * via the PWM lookup table.
	 */
	if (!state.period && (data->pwm_period_ns > 0))
		state.period = data->pwm_period_ns;

	ret = pb->proof_handoff ? 0 : pwm_apply_state(pb->pwm, &state);
	if (ret) {
		dev_err(&pdev->dev, "failed to apply initial PWM state: %d\n",
			ret);
		goto err_alloc;
	}

	memset(&props, 0, sizeof(struct backlight_properties));

	if (data->levels) {
		pb->levels = data->levels;

		/*
		 * For the DT case, only when brightness levels is defined
		 * data->levels is filled. For the non-DT case, data->levels
		 * can come from platform data, however is not usual.
		 */
		for (i = 0; i <= data->max_brightness; i++)
			if (data->levels[i] > pb->scale)
				pb->scale = data->levels[i];

		if (pwm_backlight_is_linear(data))
			props.scale = BACKLIGHT_SCALE_LINEAR;
		else
			props.scale = BACKLIGHT_SCALE_NON_LINEAR;
	} else if (!data->max_brightness) {
		/*
		 * If no brightness levels are provided and max_brightness is
		 * not set, use the default brightness table. For the DT case,
		 * max_brightness is set to 0 when brightness levels is not
		 * specified. For the non-DT case, max_brightness is usually
		 * set to some value.
		 */

		/* Get the PWM period (in nanoseconds) */
		pwm_get_state(pb->pwm, &state);

		ret = pwm_backlight_brightness_default(&pdev->dev, data,
						       state.period);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"failed to setup default brightness table\n");
			goto err_alloc;
		}

		for (i = 0; i <= data->max_brightness; i++) {
			if (data->levels[i] > pb->scale)
				pb->scale = data->levels[i];

			pb->levels = data->levels;
		}

		props.scale = BACKLIGHT_SCALE_NON_LINEAR;
	} else {
		/*
		 * That only happens for the non-DT case, where platform data
		 * sets the max_brightness value.
		 */
		pb->scale = data->max_brightness;
	}

	if (!pb->proof_handoff)
		pwm_adjust_config(pb->pwm);

	pb->lth_brightness = data->lth_brightness * (div_u64(state.period,
				pb->scale));

	if (node) {
		u32 percent = 0;

		of_property_read_u32(node, "andr36oid,kick-percent", &percent);
		of_property_read_u32(node, "andr36oid,kick-ms", &pb->kick_ms);
		pb->kick_duty = div_u64(state.period * min_t(u32, percent, 100), 100);
		if (!pb->kick_duty)
			pb->kick_ms = 0;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness;
	bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
				       &pwm_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		if (pb->legacy)
			pwm_free(pb->pwm);
		goto err_alloc;
	}

	if (data->dft_brightness > data->max_brightness) {
		dev_warn(&pdev->dev,
			 "invalid default brightness level: %u, using %u\n",
			 data->dft_brightness, data->max_brightness);
		data->dft_brightness = data->max_brightness;
	}

	/* set brightness 0, when boot quiescent */
	if (bl_quiescent)
		bl->props.brightness = 0;
	else
		bl->props.brightness = data->dft_brightness;

	bl->props.power = pwm_backlight_initial_power_state(pb);
	ret = backlight_update_status(bl);
	if (ret && pb->guard_output) {
		/* Keep the provider registered: DRM must bind and ACK-stop VOP
		 * even if blanking failed. A failed provider probe would defer
		 * the entire display before that safety gate can execute.
		 */
		if (!pb->boot_error)
			pb->boot_error = ret;
		dev_err(pb->dev, "LOGO32: backlight error %d latched; display must stay stopped\n", ret);
	}

	platform_set_drvdata(pdev, bl);
	return 0;

err_alloc:
	if (pb->proof_handoff && pb->enabled) {
		regulator_disable(pb->power_supply);
		pb->enabled = false;
		rockchip_logo_reject();
	}
	if (data->exit)
		data->exit(&pdev->dev);
	return ret;
}

static int pwm_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	backlight_device_unregister(bl);
	pwm_backlight_power_off(pb);

	if (pb->exit)
		pb->exit(&pdev->dev);
	if (pb->legacy)
		pwm_free(pb->pwm);

	return 0;
}

static void pwm_backlight_shutdown(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	pwm_backlight_power_off(pb);
}

#ifdef CONFIG_PM_SLEEP
static int pwm_backlight_suspend(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	if (pb->notify)
		pb->notify(pb->dev, 0);

	pwm_backlight_power_off(pb);

	if (pb->notify_after)
		pb->notify_after(pb->dev, 0);

	return 0;
}

static int pwm_backlight_resume(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);

	backlight_update_status(bl);

	return 0;
}
#endif

static const struct dev_pm_ops pwm_backlight_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = pwm_backlight_suspend,
	.resume = pwm_backlight_resume,
	.poweroff = pwm_backlight_suspend,
	.restore = pwm_backlight_resume,
#endif
};

static struct platform_driver pwm_backlight_driver = {
	.driver		= {
		.name		= "pwm-backlight",
		.pm		= &pwm_backlight_pm_ops,
		.of_match_table	= of_match_ptr(pwm_backlight_of_match),
	},
	.probe		= pwm_backlight_probe,
	.remove		= pwm_backlight_remove,
	.shutdown	= pwm_backlight_shutdown,
};

module_platform_driver(pwm_backlight_driver);

MODULE_DESCRIPTION("PWM based Backlight Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:pwm-backlight");
