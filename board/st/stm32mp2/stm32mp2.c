// SPDX-License-Identifier: GPL-2.0-or-later OR BSD-3-Clause
/*
 * Copyright (C) 2022, STMicroelectronics - All Rights Reserved
 */

#define LOG_CATEGORY LOGC_BOARD

#include <common.h>
#include <button.h>
#include <config.h>
#include <dm.h>
#include <env.h>
#include <env_internal.h>
#include <fdt_support.h>
#include <g_dnl.h>
#include <i2c.h>
#include <led.h>
#include <log.h>
#include <misc.h>
#include <mmc.h>
#include <init.h>
#include <net.h>
#include <netdev.h>
#include <phy.h>
#include <regmap.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <asm/arch/sys_proto.h>
#include <dm/device.h>
#include <dm/device-internal.h>
#include <dm/ofnode.h>
#include <dm/uclass.h>
#include <dt-bindings/gpio/gpio.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iopoll.h>

#define SYSCFG_ETHCR_ETH_SEL_MII	0
#define SYSCFG_ETHCR_ETH_SEL_RGMII	BIT(4)
#define SYSCFG_ETHCR_ETH_SEL_RMII	BIT(6)
#define SYSCFG_ETHCR_ETH_CLK_SEL	BIT(1)
#define SYSCFG_ETHCR_ETH_REF_CLK_SEL	BIT(0)
/* CLOCK feed to PHY*/
#define ETH_CK_F_25M	25000000
#define ETH_CK_F_50M	50000000
#define ETH_CK_F_125M	125000000

#define ILITEK_REG_ID		0x40
#define ILITEK_ID_LEN		7
#define ADV7511_REG_CHIP_REVISION	0x00
#define ADV7511_CHIP_REVISION_LEN	256

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

/* touchscreen driver: only used for pincontrol configuration */
static const struct udevice_id touchscreen_ids[] = {
	{ .compatible = "ilitek,ili251x", },
	{ }
};

U_BOOT_DRIVER(touchscreen) = {
	.name		= "touchscreen",
	.id		= UCLASS_I2C_GENERIC,
	.of_match	= touchscreen_ids,
};

static int i2c_read(ofnode node, u16 reg, u8 *buf, int len, uint wlen)
{
	ofnode bus_node;
	struct udevice *dev;
	struct udevice *bus;
	struct i2c_msg msgs[2];
	u32 chip_addr;
	__be16 wbuf;
	int ret;

	/* parent should be an I2C bus */
	bus_node = ofnode_get_parent(node);
	ret = uclass_get_device_by_ofnode(UCLASS_I2C, bus_node, &bus);
	if (ret) {
		log_debug("can't find I2C bus for node %s\n", ofnode_get_name(bus_node));
		return ret;
	}

	ret = ofnode_read_u32(node, "reg", &chip_addr);
	if (ret) {
		log_debug("can't read I2C address in %s\n", ofnode_get_name(node));
		return ret;
	}

	ret = dm_i2c_probe(bus, chip_addr, 0, &dev);
	if (ret)
		return false;

	if (wlen == 2)
		wbuf = cpu_to_be16(reg);
	else
		wbuf = reg;

	msgs[0].flags = 0;
	msgs[0].addr  = chip_addr;
	msgs[0].len   = wlen;
	msgs[0].buf   = (u8 *)&wbuf;

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = chip_addr;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = dm_i2c_xfer(dev, msgs, 2);

	return ret;
}

static bool reset_gpio(ofnode node)
{
	struct gpio_desc reset_gpio;

	gpio_request_by_name_nodev(node, "reset-gpios", 0, &reset_gpio, GPIOD_IS_OUT);

	if (!dm_gpio_is_valid(&reset_gpio))
		return false;

	dm_gpio_set_value(&reset_gpio, true);
	mdelay(1);
	dm_gpio_set_value(&reset_gpio, false);
	mdelay(10);

	dm_gpio_free(NULL, &reset_gpio);

	return true;
}

/* HELPER: search detected driver */
struct detect_info_t {
	bool (*detect)(void);
	char *compatible;
};

static const char *detect_device(const struct detect_info_t *info, u8 size)
{
	u8 i;

	for (i = 0; i < size; i++) {
		if (info[i].detect())
			return info[i].compatible;
	}

	return NULL;
}

bool detect_stm32mp25x_etml0700zxxdha(void)
{
	ofnode node;
	char id[ILITEK_ID_LEN];
	int ret;

	node = ofnode_by_compatible(ofnode_null(), "ilitek,ili251x");
	if (!ofnode_valid(node))
		return false;

	if (!reset_gpio(node))
		return false;

	mdelay(200);

	ret = i2c_read(node, ILITEK_REG_ID, id, sizeof(id), 1);
	if (ret)
		return false;

	/* FW panel ID is starting at the 4th byte */
	if (!strncmp(&id[4], "WSV", sizeof(id) - 4))
		return true;

	return false;
}

bool detect_stm32mp25x_adv7535(void)
{
	ofnode node;
	char id[ADV7511_CHIP_REVISION_LEN];
	int ret;

	node = ofnode_by_compatible(ofnode_null(),  "adi,adv7535");
	if (!ofnode_valid(node))
		return false;

	if (!reset_gpio(node))
		return false;

	mdelay(10);

	ret = i2c_read(node, ADV7511_REG_CHIP_REVISION, id, sizeof(id), 1);

	if (id[0] == 0x14)
		return true;

	return false;
}

static const struct detect_info_t stm32mp25x_panels[] = {
	{
		.detect = detect_stm32mp25x_etml0700zxxdha,
		.compatible = "edt,etml0700z9ndha",
	},
};

static const struct detect_info_t stm32mp25x_bridges[] = {
	{
		.detect = detect_stm32mp25x_adv7535,
		.compatible = "adi,adv7535",
	},

};

static void board_stm32mp25x_eval_init(void)
{
	const char *compatible;

	/* auto detection of connected panels */
	compatible = detect_device(stm32mp25x_panels, ARRAY_SIZE(stm32mp25x_panels));

	if (!compatible)
		/* remove the panel in environment */
		env_set("panel", "");
	else
		/* save the detected compatible in environment */
		env_set("panel", compatible);

	/* auto detection of connected hdmi bridge */
	compatible = detect_device(stm32mp25x_bridges, ARRAY_SIZE(stm32mp25x_bridges));

	if (!compatible)
		/* remove the hdmi bridge in environment */
		env_set("hdmi", "");
	else
		/* save the detected compatible in environment */
		env_set("hdmi", compatible);
}

static int get_led(struct udevice **dev, char *led_string)
{
	const char *led_name;
	int ret;

	led_name = ofnode_conf_read_str(led_string);
	if (!led_name) {
		log_debug("could not find %s config string\n", led_string);
		return -ENOENT;
	}
	ret = led_get_by_label(led_name, dev);
	if (ret) {
		log_debug("get=%d\n", ret);
		return ret;
	}

	return 0;
}

static int setup_led(enum led_state_t cmd)
{
	struct udevice *dev;
	int ret;

	if (!CONFIG_IS_ENABLED(LED))
		return 0;

	ret = get_led(&dev, "blue-led");
	if (ret)
		return ret;

	ret = led_set_state(dev, cmd);
	return ret;
}

static void check_user_button(void)
{
	struct udevice *button;
	int i;

	if (!IS_ENABLED(CONFIG_CMD_STM32PROG) || !IS_ENABLED(CONFIG_BUTTON))
		return;

	if (button_get_by_label("User-2", &button))
		return;

	for (i = 0; i < 21; ++i) {
		if (button_get_state(button) != BUTTON_ON)
			return;
		if (i < 20)
			mdelay(50);
	}

	log_notice("entering download mode...\n");
	clrsetbits_le32(TAMP_BOOT_CONTEXT, TAMP_BOOT_FORCED_MASK, BOOT_STM32PROG);
}

static bool board_is_stm32mp257_eval(void)
{
	if (CONFIG_IS_ENABLED(TARGET_ST_STM32MP25X) &&
	    (of_machine_is_compatible("st,stm32mp257f-ev1")))
		return true;

	if (CONFIG_IS_ENABLED(STM32MP25_REVA) &&
	    (of_machine_is_compatible("st,stm32mp257f-ev1-revB")))
		return true;

	return false;
}

/* board dependent setup after realloc */
int board_init(void)
{
	setup_led(LEDST_ON);

	check_user_button();

	return 0;
}

/* eth init function : weak called in eqos driver */
int board_interface_eth_init(struct udevice *dev,
			     phy_interface_t interface_type, ulong rate)
{
	struct regmap *regmap;
	uint regmap_mask, regmap_offset;
	int ret;
	u32 value;
	bool ext_phyclk;

	/* Ethernet PHY have no cristal or need to be clock by RCC */
	ext_phyclk = dev_read_bool(dev, "st,ext-phyclk");

	regmap = syscon_regmap_lookup_by_phandle(dev,"st,syscon");

	if (!IS_ERR(regmap)) {
		u32 fmp[3];

		ret = dev_read_u32_array(dev, "st,syscon", fmp, 3);
		if (ret) {
			pr_err("%s: Need to specify Offset and Mask of syscon register\n", __func__);
			return ret;
		}
		else {
			regmap_mask = fmp[2];
			regmap_offset = fmp[1];
		}
	} else
		return -ENODEV;

	switch (interface_type) {
	case PHY_INTERFACE_MODE_MII:
		value = SYSCFG_ETHCR_ETH_SEL_MII;
		debug("%s: PHY_INTERFACE_MODE_MII\n", __func__);
		break;
	case PHY_INTERFACE_MODE_RMII:
		if ((rate == ETH_CK_F_50M) && ext_phyclk)
			value = SYSCFG_ETHCR_ETH_SEL_RMII |
				SYSCFG_ETHCR_ETH_REF_CLK_SEL;
		else
			value = SYSCFG_ETHCR_ETH_SEL_RMII;
		debug("%s: PHY_INTERFACE_MODE_RMII\n", __func__);
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		if ((rate == ETH_CK_F_125M) && ext_phyclk)
			value = SYSCFG_ETHCR_ETH_SEL_RGMII |
				SYSCFG_ETHCR_ETH_CLK_SEL;
		else
			value = SYSCFG_ETHCR_ETH_SEL_RGMII;
		debug("%s: PHY_INTERFACE_MODE_RGMII\n", __func__);
		break;
	default:
		debug("%s: Do not manage %d interface\n",
		      __func__, interface_type);
		/* Do not manage others interfaces */
		return -EINVAL;
	}

	ret = regmap_update_bits(regmap, regmap_offset, regmap_mask, value);

	return ret;
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

	case BOOT_FLASH_NAND:
	case BOOT_FLASH_SPINAND:
		if (CONFIG_IS_ENABLED(ENV_IS_IN_UBI))
			return ENVL_UBI;
		else
			return ENVL_NOWHERE;

	case BOOT_FLASH_NOR:
		if (CONFIG_IS_ENABLED(ENV_IS_IN_SPI_FLASH))
			return ENVL_SPI_FLASH;
		else
			return ENVL_NOWHERE;

	case BOOT_FLASH_HYPERFLASH:
		if (CONFIG_IS_ENABLED(ENV_IS_IN_FLASH))
			return ENVL_FLASH;
		else
			return ENVL_NOWHERE;

	default:
		return ENVL_NOWHERE;
	}
}

int mmc_get_boot(void)
{
	struct udevice *dev;
	u32 boot_mode = get_bootmode();
	unsigned int instance = (boot_mode & TAMP_BOOT_INSTANCE_MASK) - 1;
	char cmd[20];
	const u32 sdmmc_addr[] = {
		STM32_SDMMC1_BASE,
		STM32_SDMMC2_BASE,
		STM32_SDMMC3_BASE
	};

	if (instance > ARRAY_SIZE(sdmmc_addr))
		return 0;

	/* search associated sdmmc node in devicetree */
	snprintf(cmd, sizeof(cmd), "mmc@%x", sdmmc_addr[instance]);
	if (uclass_get_device_by_name(UCLASS_MMC, cmd, &dev)) {
		log_err("mmc%d = %s not found in device tree!\n", instance, cmd);
		return 0;
	}

	return dev_seq(dev);
};

int mmc_get_env_dev(void)
{
	const int mmc_env_dev = CONFIG_IS_ENABLED(ENV_IS_IN_MMC, (CONFIG_SYS_MMC_ENV_DEV), (-1));

	if (mmc_env_dev >= 0)
		return mmc_env_dev;

	/* use boot instance to select the correct mmc device identifier */
	return mmc_get_boot();
}

int board_late_init(void)
{
	const void *fdt_compat;
	int fdt_compat_len;
	char dtb_name[256];
	int buf_len;

	if (board_is_stm32mp257_eval())
		board_stm32mp25x_eval_init();

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

static int fixup_stm32mp257_eval_panel(void *blob)
{
	char const *panel = env_get("panel");
	char const *hdmi = env_get("hdmi");
	bool detect_etml0700z9ndha = false;
	bool detect_adv7535 = false;
	int nodeoff = 0;
	enum fdt_status status;

	if (panel)
		detect_etml0700z9ndha = !strcmp(panel, "edt,etml0700z9ndha");

	if (hdmi)
		detect_adv7535 = !strcmp(hdmi, "adi,adv7535");

	/* update LVDS panel "edt,etml0700z9ndha" */
	status = detect_etml0700z9ndha ? FDT_STATUS_OKAY : FDT_STATUS_DISABLED;
	nodeoff = fdt_set_status_by_compatible(blob, "edt,etml0700z9ndha", status);
	if (nodeoff < 0)
		return nodeoff;
	nodeoff = fdt_set_status_by_compatible(blob, "ilitek,ili251x", status);
	if (nodeoff < 0)
		return nodeoff;
	nodeoff = fdt_set_status_by_pathf(blob, status, "/panel-lvds-backlight");
	if (nodeoff < 0)
		return nodeoff;
	nodeoff = fdt_set_status_by_compatible(blob, "st,stm32-lvds", status);
	if (nodeoff < 0)
		return nodeoff;

	/* update HDMI bridge "adi,adv7535" */
	status = detect_adv7535 ? FDT_STATUS_OKAY : FDT_STATUS_DISABLED;
	nodeoff = fdt_set_status_by_compatible(blob, "adi,adv7535", status);
	/* Do not force disable status for sound card. Keep default status instead */
	if (status == FDT_STATUS_OKAY) {
		if (nodeoff < 0)
			return nodeoff;
		nodeoff = fdt_set_status_by_compatible(blob, "st,stm32mp25-i2s", status);
		if (nodeoff < 0)
			return nodeoff;
		nodeoff = fdt_set_status_by_pathf(blob, status, "/sound");
		if (nodeoff < 0)
			return nodeoff;
		nodeoff = fdt_status_okay_by_compatible(blob, "st,stm32-dsi");
		if (nodeoff < 0)
			return nodeoff;
	}

	if (!detect_adv7535 && !detect_etml0700z9ndha) {
		nodeoff = fdt_status_disabled_by_compatible(blob, "st,stm32-ltdc");
		if (nodeoff < 0)
			return nodeoff;
	}

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	int ret;

	fdt_copy_fixed_partitions(blob);

	if (board_is_stm32mp257_eval()) {
		ret = fixup_stm32mp257_eval_panel(blob);
		if (ret)
			log_err("Error during panel fixup ! (%d)\n", ret);
	}

	return 0;
}

void board_quiesce_devices(void)
{
	setup_led(LEDST_OFF);
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

/* weak function called from common/board_r.c */
int is_flash_available(void)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_driver(UCLASS_MTD,
					  DM_DRIVER_GET(stm32_hyperbus),
					  &dev);
	if (ret)
		return 0;

	return 1;
}

/* weak function called from env/sf.c */
void *env_sf_get_env_addr(void)
{
	return NULL;
}
