// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * Copyright (C) 2022, STMicroelectronics - All Rights Reserved
 */

#define LOG_CATEGORY LOGC_BOARD

#include <common.h>
#include <config.h>
#include <env.h>
#include <env_internal.h>
#include <fdt_support.h>
#include <g_dnl.h>
#include <log.h>
#include <misc.h>
#include <asm/global_data.h>
#include <asm/arch/sys_proto.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <dm/uclass.h>

/*
 * Get a global data pointer
 */
DECLARE_GLOBAL_DATA_PTR;

int checkboard(void)
{
	int ret;
	u32 otp;
	struct udevice *dev;
	const char *fdt_compat;
	int fdt_compat_len;

	fdt_compat = ofnode_get_property(ofnode_root(), "compatible", &fdt_compat_len);

	log_info("Board: stm32mp2 (%s)\n", fdt_compat && fdt_compat_len ? fdt_compat : "");

	/* display the STMicroelectronics board identification */
	if (CONFIG_IS_ENABLED(CMD_STBOARD)) {
		ret = uclass_get_device_by_driver(UCLASS_MISC,
						  DM_DRIVER_GET(stm32mp_bsec),
						  &dev);
		if (!ret)
			ret = misc_read(dev, STM32_BSEC_SHADOW(BSEC_OTP_BOARD),
					&otp, sizeof(otp));
		if (ret > 0 && otp)
			log_info("Board: MB%04x Var%d.%d Rev.%c-%02d\n",
				 otp >> 16,
				 (otp >> 12) & 0xF,
				 (otp >> 4) & 0xF,
				 ((otp >> 8) & 0xF) - 1 + 'A',
				 otp & 0xF);
	}

	return 0;
}

#ifdef CONFIG_USB_GADGET_DOWNLOAD
#define STM32MP1_G_DNL_DFU_PRODUCT_NUM 0xdf11
int g_dnl_bind_fixup(struct usb_device_descriptor *dev, const char *name)
{
	if (IS_ENABLED(CONFIG_DFU_OVER_USB) &&
	    !strcmp(name, "usb_dnl_dfu"))
		put_unaligned(STM32MP1_G_DNL_DFU_PRODUCT_NUM, &dev->idProduct);
	else
		put_unaligned(CONFIG_USB_GADGET_PRODUCT_NUM, &dev->idProduct);

	return 0;
}
#endif /* CONFIG_USB_GADGET_DOWNLOAD */

/* board dependent setup after realloc */
int board_init(void)
{
	return 0;
}

enum env_location env_get_location(enum env_operation op, int prio)
{
	u32 bootmode = get_bootmode();

	if (prio)
		return ENVL_UNKNOWN;

	switch (bootmode & TAMP_BOOT_DEVICE_MASK) {
	case BOOT_FLASH_SD:
	case BOOT_FLASH_EMMC:
		if (CONFIG_IS_ENABLED(ENV_IS_IN_MMC))
			return ENVL_MMC;
		else
			return ENVL_NOWHERE;
	default:
		return ENVL_NOWHERE;
	}
}

int board_late_init(void)
{
	const void *fdt_compat;
	int fdt_compat_len;
	char dtb_name[256];
	int buf_len;

	if (IS_ENABLED(CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG)) {
		fdt_compat = fdt_getprop(gd->fdt_blob, 0, "compatible",
					 &fdt_compat_len);
		if (fdt_compat && fdt_compat_len) {
			if (strncmp(fdt_compat, "st,", 3) != 0) {
				env_set("board_name", fdt_compat);
			} else {
				env_set("board_name", fdt_compat + 3);

				buf_len = sizeof(dtb_name);
				strncpy(dtb_name, fdt_compat + 3, buf_len);
				buf_len -= strlen(fdt_compat + 3);
				strncat(dtb_name, ".dtb", buf_len);
				env_set("fdtfile", dtb_name);
			}
		}
	}

	return 0;
}

#if defined(CONFIG_USB_DWC3) && defined(CONFIG_CMD_STM32PROG_USB)
#include <dfu.h>
/*
 * TEMP: force USB BUS reset forced to false, because it is not supported
 *       in DWC3 USB driver
 * avoid USB bus reset support in DFU stack is required to reenumeration in
 * stm32prog command after flashlayout load or after "dfu-util -e -R"
 */
bool dfu_usb_get_reset(void)
{
	return false;
}
#endif
