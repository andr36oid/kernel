/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_A36_KERNEL_TRIAL_H
#define _LINUX_A36_KERNEL_TRIAL_H
#include <linux/kconfig.h>
#include <linux/types.h>
#if IS_BUILTIN(CONFIG_DW_WATCHDOG)
bool a36_kernel_trial_requested(void);
#else
static inline bool a36_kernel_trial_requested(void) { return false; }
#endif
#endif
