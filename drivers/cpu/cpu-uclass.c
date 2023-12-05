// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 */

#define LOG_CATEGORY UCLASS_CPU

#include <common.h>
#include <cpu.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <dm/lists.h>
#include <dm/root.h>
#include <linux/err.h>
#include <relocate.h>

DECLARE_GLOBAL_DATA_PTR;

int cpu_probe_all(void)
{
	struct udevice *cpu;
	int ret;

	ret = uclass_first_device(UCLASS_CPU, &cpu);
	if (ret) {
		debug("%s: No CPU found (err = %d)\n", __func__, ret);
		return ret;
	}

	while (cpu) {
		ret = uclass_next_device(&cpu);
		if (ret) {
			debug("%s: Error while probing CPU (err = %d)\n",
			      __func__, ret);
			return ret;
		}
	}

	return 0;
}

int cpu_is_current(struct udevice *cpu)
{
	struct cpu_ops *ops = cpu_get_ops(cpu);

	if (ops->is_current) {
		if (ops->is_current(cpu))
			return 1;
	}

	return -ENOSYS;
}

struct udevice *cpu_get_current_dev(void)
{
	struct udevice *cpu;
	int ret;

	uclass_foreach_dev_probe(UCLASS_CPU, cpu) {
		if (cpu_is_current(cpu) > 0)
			return cpu;
	}

	/* If can't find current cpu device, use the first dev instead */
	ret = uclass_first_device_err(UCLASS_CPU, &cpu);
	if (ret) {
		debug("%s: Could not get CPU device (err = %d)\n",
		      __func__, ret);
		return NULL;
	}

	return cpu;
}

int cpu_get_desc(const struct udevice *dev, char *buf, int size)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_desc)
		return -ENOSYS;

	return ops->get_desc(dev, buf, size);
}

int cpu_get_info(const struct udevice *dev, struct cpu_info *info)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_info)
		return -ENOSYS;

	/* Init cpu_info to 0 */
	memset(info, 0, sizeof(struct cpu_info));

	return ops->get_info(dev, info);
}

int cpu_get_count(const struct udevice *dev)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_count)
		return -ENOSYS;

	return ops->get_count(dev);
}

int cpu_get_vendor(const struct udevice *dev, char *buf, int size)
{
	struct cpu_ops *ops = cpu_get_ops(dev);

	if (!ops->get_vendor)
		return -ENOSYS;

	return ops->get_vendor(dev, buf, size);
}

U_BOOT_DRIVER(cpu_bus) = {
	.name	= "cpu_bus",
	.id	= UCLASS_SIMPLE_BUS,
	.per_child_plat_auto	= sizeof(struct cpu_plat),
};

static int uclass_cpu_init(struct uclass *uc)
{
	struct udevice *dev;
	ofnode node;
	int ret;

	node = ofnode_path("/cpus");
	if (!ofnode_valid(node))
		return 0;

	ret = device_bind_driver_to_node(dm_root(), "cpu_bus", "cpus", node,
					 &dev);

	return ret;
}

static int uclass_cpu_post_bind(struct udevice *dev)
{
	if (IS_ENABLED(CONFIG_NEEDS_MANUAL_RELOC) &&
	    (gd->flags & GD_FLG_RELOC)) {
		struct cpu_ops *ops = cpu_get_ops(dev);
		static int reloc_done;

		if (!reloc_done) {
			if (ops->get_desc)
				MANUAL_RELOC(ops->get_desc);
			if (ops->get_info)
				MANUAL_RELOC(ops->get_info);
			if (ops->get_count)
				MANUAL_RELOC(ops->get_count);
			if (ops->get_vendor)
				MANUAL_RELOC(ops->get_vendor);
			if (ops->is_current)
				MANUAL_RELOC(ops->is_current);

			reloc_done++;
		}
	}

	return 0;
}

UCLASS_DRIVER(cpu) = {
	.id		= UCLASS_CPU,
	.name		= "cpu",
	.flags		= DM_UC_FLAG_SEQ_ALIAS,
	.init		= uclass_cpu_init,
	.post_bind	= uclass_cpu_post_bind,
};
