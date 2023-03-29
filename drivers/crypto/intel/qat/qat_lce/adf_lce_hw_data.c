// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation */

#include <adf_accel_devices.h>

#include "adf_lce_hw_data.h"

/*
 * LCE PF carries no acceleration engines or firmware. The hw_device_class
 * exists so adf_devmgr_add_dev() can assign a stable accel_id and user
 * space sees the device through the usual QAT plumbing.
 */
struct adf_hw_device_class lce_class = {
	.name = ADF_LCE_DEVICE_NAME,
	.type = DEV_LCE,
	.instances = 0,
};
