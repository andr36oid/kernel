/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ANDR36OID_GUEST_INPUT_H
#define _ANDR36OID_GUEST_INPUT_H
#include <linux/pid_namespace.h>
#include <linux/input.h>
#include <linux/andr36oid-keymap.h>

/* Map the Android GO pad wiring to the unmodified dArkOSen rg351 ABI.
 * Applied to a reader, never to the shared input_dev or its physical driver.
 */
static inline bool a36_guest_pad(struct input_dev *dev)
{
	/* The physical driver already supplies this ABI to every reader. */
	if (a36_rg351_input_normalized(dev))
		return false;
	return READ_ONCE(task_active_pid_ns(current)->andr36oid_guest) &&
		dev->id.vendor == 0x484b && dev->id.product == 0x1100 &&
		test_bit(KEY_UP, dev->keybit) && test_bit(KEY_HOMEPAGE, dev->keybit);
}
static inline unsigned int a36_guest_key(unsigned int code)
{
	int mapped = a36_android_to_rg351_key(code);

	/* Retain legacy reader behavior for devices not owned by the new driver. */
	return mapped < 0 ? code : mapped;
}
static inline void a36_guest_bits(unsigned long *dst, const unsigned long *src)
{
	unsigned int key;
	bitmap_zero(dst, KEY_CNT);
	for_each_set_bit(key, src, KEY_CNT)
		__set_bit(a36_guest_key(key), dst);
}
#endif
