// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/version.c
 *
 *  Copyright (C) 1992  Theodore Ts'o
 *
 *  May be freely distributed as part of Linux.
 */

#include <generated/compile.h>
#include <linux/build-salt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/andr36oid-build.h>
#include <linux/export.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>
#include <linux/version.h>
#include <linux/proc_ns.h>

#ifndef CONFIG_KALLSYMS
#define version(a) Version_ ## a
#define version_string(a) version(a)

extern int version_string(LINUX_VERSION_CODE);
int version_string(LINUX_VERSION_CODE);
#endif

struct uts_namespace init_uts_ns = {
	.kref = KREF_INIT(2),
	.name = {
		.sysname	= UTS_SYSNAME,
		.nodename	= UTS_NODENAME,
		.release	= UTS_RELEASE,
		.version	= UTS_VERSION,
		.machine	= UTS_MACHINE,
		.domainname	= UTS_DOMAINNAME,
	},
	.user_ns = &init_user_ns,
	.ns.inum = PROC_UTS_INIT_INO,
#ifdef CONFIG_UTS_NS
	.ns.ops = &utsns_operations,
#endif
};
EXPORT_SYMBOL_GPL(init_uts_ns);

/* FIXED STRINGS! Don't touch! */
const char linux_banner[] =
	"Linux version " UTS_RELEASE " (" LINUX_COMPILE_BY "@"
	LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION "\n";

const char linux_proc_banner[] =
	"%s version %s"
	" (" LINUX_COMPILE_BY "@" LINUX_COMPILE_HOST ")"
	" (" LINUX_COMPILER ") %s\n";

BUILD_SALT;

/* Immutable identity independent of the owner-selected uname release suffix. */
static ssize_t variant_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", A36_BUILD_VARIANT);
}
static ssize_t build_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", UTS_VERSION);
}
static ssize_t flavor_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", A36_BUILD_FLAVOR);
}
static struct kobj_attribute variant_attr = __ATTR_RO(variant);
static struct kobj_attribute build_attr = __ATTR_RO(build);
static struct kobj_attribute flavor_attr = __ATTR_RO(flavor);
static struct attribute *a36_build_attrs[] = {
	&variant_attr.attr, &build_attr.attr, &flavor_attr.attr, NULL,
};
static const struct attribute_group a36_build_group = {
	.attrs = a36_build_attrs,
};
static int __init a36_build_identity_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("andr36oid", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &a36_build_group);
	if (ret)
		kobject_put(kobj);
	return ret;
}
subsys_initcall(a36_build_identity_init);
