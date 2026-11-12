/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ANDR36OID_BUILD_H
#define _LINUX_ANDR36OID_BUILD_H
#if defined(CONFIG_MALI_BIFROST_G7P1_FOR_ANDROID)
#define A36_BUILD_VARIANT "g7p1"
#elif defined(CONFIG_MALI_BIFROST)
#define A36_BUILD_VARIANT "g2"
#else
#define A36_BUILD_VARIANT "none"
#endif
#if defined(CONFIG_LTO_CLANG_THIN)
#define A36_BUILD_FLAVOR "thinlto"
#elif defined(CONFIG_CC_IS_CLANG)
#define A36_BUILD_FLAVOR "clang"
#else
#define A36_BUILD_FLAVOR "gcc"
#endif
#endif
