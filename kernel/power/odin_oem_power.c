// SPDX-License-Identifier: GPL-2.0-only
/* Battery-provider portion of Xiaomi power_debug, using the stock prototypes.
 * Curve: Xiaomi mi_power.c@39382bf and the odin original kernel disassembly.
 * Class handling follows Tim0320/lisa_power_compat.c@7098430.
 * This does not replace Xiaomi's scheduler, interrupt or clock diagnostics.
 */
#ifdef ODIN_POWER_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#define READ_ONCE(x) (x)
#define EXPORT_SYMBOL(x)
#else
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#endif

static unsigned int odin_power_mode = 1;
static bool odin_power_debug_enable;

bool power_debug_print_enabled(void)
{
	return READ_ONCE(odin_power_debug_enable);
}
EXPORT_SYMBOL(power_debug_print_enabled);

ssize_t mi_power_save_battery_cave(ssize_t capacity)
{
	ssize_t result;

	/* Preserve invalid provider readings; transform actual percentages only. */
	if (capacity < 0 || capacity > 100 || READ_ONCE(odin_power_mode) < 2)
		return capacity;
	if (capacity > 20)
		result = ((capacity - 20) * 100 + 106) / 107 + 25;
	else
		result = (capacity * 100 + 79) / 80;
	return result > 100 ? 100 : result;
}
EXPORT_SYMBOL(mi_power_save_battery_cave);

#ifndef ODIN_POWER_HOST_TEST
static ssize_t power_mode_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(odin_power_mode));
}

static ssize_t power_mode_store(struct class *cls,
		struct class_attribute *attr, const char *buf, size_t count)
{
	unsigned int value;
	int ret = kstrtouint(buf, 10, &value);

	if (ret)
		return ret;
	if (value > 4)
		return -EINVAL;
	WRITE_ONCE(odin_power_mode, value);
	return count;
}

static ssize_t debug_suspend_show(struct class *cls,
		struct class_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", power_debug_print_enabled());
}

static ssize_t debug_suspend_store(struct class *cls,
		struct class_attribute *attr, const char *buf, size_t count)
{
	bool value;
	int ret = kstrtobool(buf, &value);

	if (ret)
		return ret;
	WRITE_ONCE(odin_power_debug_enable, value);
	return count;
}

static struct class_attribute class_attr_power_mode =
	__ATTR(power_mode, 0664, power_mode_show, power_mode_store);
static struct class_attribute class_attr_debug_suspend =
	__ATTR(debug_suspend, 0664, debug_suspend_show, debug_suspend_store);

static int __init odin_power_compat_init(void)
{
	struct class *cls;
	int ret;

	cls = class_create(THIS_MODULE, "power_debug");
	if (IS_ERR(cls))
		return PTR_ERR(cls);
	ret = class_create_file(cls, &class_attr_power_mode);
	if (ret)
		goto destroy;
	ret = class_create_file(cls, &class_attr_debug_suspend);
	if (ret) {
		class_remove_file(cls, &class_attr_power_mode);
		goto destroy;
	}
	return 0;
destroy:
	class_destroy(cls);
	return ret;
}
subsys_initcall(odin_power_compat_init);
#endif
