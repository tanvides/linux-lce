// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/bitops.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_cfg_services.h>
#include <adf_common_drv.h>
#include <adf_gen6_shared.h>

#include "adf_61mx_hw_data.h"

#define ADF_61MX_NUM_RINGS_PER_BANK	2
#define ADF_61MX_TX_RINGS_MASK		0x1
#define ADF_61MX_RX_RINGS_OFFSET		1

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

static u32 get_accel_cap(struct adf_accel_dev *accel_dev)
{
	return 0;
}

static void enable_error_correction(struct adf_accel_dev *accel_dev)
{
	/* GEN6.1 exposes no error correction block to the host driver. */
}

static void enable_ints(struct adf_accel_dev *accel_dev)
{
	/* Ring bundle interrupts are enabled via MSI-X allocation. */
}

static int send_admin_init(struct adf_accel_dev *accel_dev)
{
	/*
	 * GEN6.1 has no admin message channel; no firmware download is
	 * driven from the host.
	 */
	return 0;
}

static void disable_iov(struct adf_accel_dev *accel_dev)
{
	/* VFs are provisioned by the CPF and keep running while the PF is down */
}

static int enable_pfvf_comms(struct adf_accel_dev *accel_dev)
{
	/*
	 * GEN6.1 PF-VF signalling is handled by the CPF, not by the QAT
	 * framework's PFVF pipe.
	 */
	return 0;
}

static bool services_supported(unsigned long mask)
{
	const unsigned long supported = BIT(SVC_SYM) | BIT(SVC_ASYM);

	return mask && !(mask & ~supported);
}

static int adf_61mx_dev_config(struct adf_accel_dev *accel_dev)
{
	int ret;

	if (!GET_HW_DATA(accel_dev)->num_banks)
		return -EOPNOTSUPP;

	ret = adf_cfg_section_add(accel_dev, ADF_KERNEL_SEC);
	if (ret)
		return ret;

	ret = adf_cfg_section_add(accel_dev, "Accelerator0");
	if (ret)
		return ret;

	/* Crypto is serviced by VF users only; the PF registers no crypto algs */
	ret = adf_gen6_no_dev_config(accel_dev);
	if (ret) {
		dev_err(&GET_DEV(accel_dev), "Failed to configure device\n");
		return ret;
	}

	set_bit(ADF_STATUS_CONFIGURED, &accel_dev->status);
	return 0;
}

void adf_init_hw_data_61mx(struct adf_hw_device_data *hw_data, u32 num_banks)
{
	hw_data->dev_class = &adf_61mx_class;
	hw_data->instance_id = adf_61mx_class.instances++;
	hw_data->num_banks = num_banks;
	hw_data->num_banks_per_vf = 1;
	hw_data->num_rings_per_bank = ADF_61MX_NUM_RINGS_PER_BANK;
	hw_data->num_accel = 1;
	hw_data->num_engines = 0;
	hw_data->num_logical_accel = 1;
	hw_data->tx_rx_gap = ADF_61MX_RX_RINGS_OFFSET;
	hw_data->tx_rings_mask = ADF_61MX_TX_RINGS_MASK;
	hw_data->alloc_irq = adf_61mx_alloc_irq;
	hw_data->free_irq = adf_61mx_free_irq;
	hw_data->enable_error_correction = enable_error_correction;
	hw_data->get_accel_mask = get_accel_mask;
	hw_data->get_ae_mask = get_ae_mask;
	hw_data->get_num_accels = get_num_accels;
	hw_data->get_num_aes = get_num_aes;
	hw_data->get_etr_bar_id = get_etr_bar_id;
	hw_data->get_misc_bar_id = get_misc_bar_id;
	hw_data->get_accel_cap = get_accel_cap;
	hw_data->get_sku = get_sku;
	hw_data->send_admin_init = send_admin_init;
	hw_data->enable_ints = enable_ints;
	hw_data->disable_iov = disable_iov;
	hw_data->dev_config = adf_61mx_dev_config;
	hw_data->services_supported = services_supported;
	hw_data->pfvf_ops.enable_comms = enable_pfvf_comms;

	adf_61mx_init_hw_csr_ops(&hw_data->csr_ops);
}

void adf_clean_hw_data_61mx(struct adf_hw_device_data *hw_data)
{
	if (hw_data->dev_class)
		hw_data->dev_class->instances--;
}
