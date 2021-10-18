/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021, STMicroelectronics - All Rights Reserved
 */

enum typec_state {
	TYPEC_UNATTACHED,
	TYPEC_ATTACHED,
};

enum typec_data_role {
	TYPEC_DEVICE,
	TYPEC_HOST,
};

/**
 * struct typec_ops - driver I/O operations for TYPEC uclass
 *
 * Drivers should support 2 operations. These operations is intended
 * to be used by uclass code, not directly from other code.
 */
struct typec_ops {
	/**
	 * is_attached() - Return if cable is attached
	 *
	 * @dev: TYPEC device to read from
	 * @con_idx: connector index (0 is the first one)
	 * @return  TYPEC_UNATTACHED if not attached, TYPEC_ATTACHED if attached, -ve on error
	 */
	int (*is_attached)(struct udevice *dev, u8 con_idx);

	/**
	 * get_data_role() - Return data role (HOST or DEVICE)
	 *
	 * @dev: TYPEC device to read from
	 * @con_idx: connector index (0 is the first one)
	 * @return: TYPEC_DEVICE if device role, TYPEC_HOST if host role, -ve on error
	 */
	int (*get_data_role)(struct udevice *dev, u8 con_idx);

	/**
	 * get_nb_connector() - Return connector number managed by TypeC controller.
	 *
	 * @dev: TYPEC device to read from
	 * @return: number of connector managed by TypeC controller, -ve on error
	 */
	u8 (*get_nb_connector)(struct udevice *dev);
};

#ifdef CONFIG_TYPEC
/**
 * typec_is_attached() - Test if Type-C connector is attached
 *
 * @return TYPEC_ATTACHED if attached, TYPEC_UNATTACHED is not attached,
 * or -ve on error.
 */
int typec_is_attached(struct udevice *dev, u8 con_idx);

/**
 * typec_get_data_role() - Return current Type-C data role
 *
 * @return TYPEC_DEVICE if attached to a host, TYPEC_HOST is attached to a
 * device or -ve on error.
 */
int typec_get_data_role(struct udevice *dev, u8 con_idx);

/**
 * typec_get_nb_connector() - Return Type-C connector supported by controller
 *
 * @return Type-C connector number or -ve on error.
 */
int typec_get_nb_connector(struct udevice *dev);
#else
static inline int typec_is_attached(struct udevice *dev, u8 con_idx)
{
	return -ENODEV;
}

static inline int typec_get_data_role(struct udevice *dev, u8 con_idx)
{
	return -EINVAL;
}

static inline int typec_get_nb_connector(struct udevice *dev)
{
	return -EINVAL;
}
#endif

