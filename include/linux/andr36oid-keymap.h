/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ANDR36OID_KEYMAP_H
#define _LINUX_ANDR36OID_KEYMAP_H
#include <linux/input.h>
#include <linux/property.h>

#define A36_RG351_NORMALIZED "andr36oid,rg351-normalized"
#define A36_RG351_EXTRA_FIRST (BTN_TRIGGER_HAPPY1 + 5)
#define A36_RG351_EXTRA_LAST (BTN_TRIGGER_HAPPY16 + 5)

static inline bool a36_rg351_core_key(unsigned int code)
{
	switch (code) {
	case BTN_SOUTH: case BTN_EAST: case BTN_NORTH: case BTN_WEST:
	case BTN_TL: case BTN_TR: case BTN_TL2: case BTN_TR2:
	case BTN_DPAD_UP: case BTN_DPAD_DOWN:
	case BTN_DPAD_LEFT: case BTN_DPAD_RIGHT:
	case BTN_TRIGGER_HAPPY1: case BTN_TRIGGER_HAPPY2:
	case BTN_TRIGGER_HAPPY3: case BTN_TRIGGER_HAPPY4:
	case BTN_TRIGGER_HAPPY5:
		return true;
	default:
		return false;
	}
}

static inline bool a36_rg351_output_key(unsigned int code)
{
	return a36_rg351_core_key(code) ||
	       (code >= A36_RG351_EXTRA_FIRST && code <= A36_RG351_EXTRA_LAST);
}

/* DT inputs are Android identities, not guesses based on their numeric range. */
static inline int a36_android_to_rg351_key(unsigned int code)
{
	if (code >= BTN_TRIGGER_HAPPY1 && code <= BTN_TRIGGER_HAPPY16)
		return code + 5;

	switch (code) {
	case KEY_UP: case BTN_DPAD_UP: return BTN_DPAD_UP;
	case KEY_DOWN: case BTN_DPAD_DOWN: return BTN_DPAD_DOWN;
	case KEY_LEFT: case BTN_DPAD_LEFT: return BTN_DPAD_LEFT;
	case KEY_RIGHT: case BTN_DPAD_RIGHT: return BTN_DPAD_RIGHT;
	case BTN_SOUTH: return BTN_EAST;
	case BTN_EAST: return BTN_SOUTH;
	case BTN_NORTH: case BTN_WEST:
	case BTN_TL: case BTN_TR: case BTN_TL2: case BTN_TR2:
		return code;
	case BTN_SELECT: return BTN_TRIGGER_HAPPY1;
	case BTN_START: return BTN_TRIGGER_HAPPY2;
	case KEY_BACK: case BTN_THUMBL: return BTN_TRIGGER_HAPPY3;
	case KEY_LEFTMETA: case BTN_THUMBR: return BTN_TRIGGER_HAPPY4;
	case KEY_HOMEPAGE: return BTN_TRIGGER_HAPPY5;
	default:
		return -EINVAL;
	}
}

static inline bool a36_rg351_input_normalized(struct input_dev *input)
{
	return device_property_read_bool(&input->dev, A36_RG351_NORMALIZED);
}

static inline int a36_rg351_mark_input(struct input_dev *input)
{
	static const struct property_entry props[] = {
		PROPERTY_ENTRY_BOOL(A36_RG351_NORMALIZED),
		{ }
	};

	return device_create_managed_software_node(&input->dev, props, NULL);
}
#endif
