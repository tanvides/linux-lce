// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <adf_accel_devices.h>
#include <adf_common_drv.h>

#include "adf_61mx_hw_data.h"

static struct adf_hw_device_class adf_61mx_class = {
	.name = ADF_61MX_DEVICE_NAME,
	.type = DEV_61MX,
	.instances = 0,
};

static u32 get_accel_mask(struct adf_hw_device_data *self)
{
	return 1;
}

static u32 get_ae_mask(struct adf_hw_device_data *self)
{
	return 0;
}

static u32 get_num_accels(struct adf_hw_device_data *self)
{
	return 1;
}

static u32 get_num_aes(struct adf_hw_device_data *self)
{
	return 0;
}

static u32 get_etr_bar_id(struct adf_hw_device_data *self)
{
	return 0;
}

static u32 get_misc_bar_id(struct adf_hw_device_data *self)
{
	return 0;
}

static enum dev_sku_info get_sku(struct adf_hw_device_data *self)
{
	return DEV_SKU_UNKNOWN;
}

static void enable_error_correction(struct adf_accel_dev *accel_dev)
{
	/* GEN61 exposes no error correction block to the host driver. */
}

static void enable_ints(struct adf_accel_dev *accel_dev)
{
	/* Ring bundle interrupts are enabled via MSI-X allocation. */
}

static int send_admin_init(struct adf_accel_dev *accel_dev)
{
	/* GEN61 has no admin message channel; no firmware download is
	 * driven from the host.
	 */
	return 0;
}

static int enable_pfvf_comms(struct adf_accel_dev *accel_dev)
{
	/* GEN61 PF-VF signalling is handled by the CPF, not by the QAT
	 * framework's PFVF pipe.
	 */
	return 0;
}

void adf_init_hw_data_61mx(struct adf_hw_device_data *hw_data)
{
	hw_data->dev_class = &adf_61mx_class;
	hw_data->instance_id = adf_61mx_class.instances++;
	hw_data->num_accel = 1;
	hw_data->num_engines = 0;
	hw_data->num_logical_accel = 1;
	hw_data->enable_error_correction = enable_error_correction;
	hw_data->get_accel_mask = get_accel_mask;
	hw_data->get_ae_mask = get_ae_mask;
	hw_data->get_num_accels = get_num_accels;
	hw_data->get_num_aes = get_num_aes;
	hw_data->get_etr_bar_id = get_etr_bar_id;
	hw_data->get_misc_bar_id = get_misc_bar_id;
	hw_data->get_sku = get_sku;
	hw_data->send_admin_init = send_admin_init;
	hw_data->enable_ints = enable_ints;
	hw_data->pfvf_ops.enable_comms = enable_pfvf_comms;
}

void adf_clean_hw_data_61mx(struct adf_hw_device_data *hw_data)
{
	if (hw_data->dev_class)
		hw_data->dev_class->instances--;
}
