// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/bitops.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_cfg_services.h>
#include <adf_common_drv.h>
#include <adf_dc.h>
#include <adf_gen6_shared.h>

#include "icp_qat_fw_comp.h"
#include "icp_qat_hw_51_comp.h"

#include "adf_lce_hw_data.h"
#include "adf_lce_isr.h"

#define ADF_LCE_NUM_RINGS_PER_BANK	2
#define ADF_LCE_TX_RINGS_MASK		0x1
#define ADF_LCE_RX_RINGS_OFFSET		1

static struct adf_hw_device_class lce_class = {
	.name = ADF_LCE_DEVICE_NAME,
	.type = DEV_LCE,
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
	return ADF_ACCEL_CAPABILITIES_COMPRESSION;
}

static void enable_error_correction(struct adf_accel_dev *accel_dev)
{
	/* LCE exposes no error correction block to the host driver. */
}

static void enable_ints(struct adf_accel_dev *accel_dev)
{
	/* Ring bundle interrupts are enabled via MSI-X allocation. */
}

static int send_admin_init(struct adf_accel_dev *accel_dev)
{
	/* LCE has no admin message channel; no firmware download is
	 * driven from the host.
	 */
	return 0;
}

static void disable_iov(struct adf_accel_dev *accel_dev)
{
	struct pci_dev *pdev = accel_to_pci_dev(accel_dev);

	if (pci_num_vf(pdev))
		pci_disable_sriov(pdev);
}

static int enable_pfvf_comms(struct adf_accel_dev *accel_dev)
{
	/* LCE PF-VF signalling is handled by the CPF, not by the QAT
	 * framework's PFVF pipe.
	 */
	return 0;
}

static bool services_supported(unsigned long mask)
{
	/* LCE exposes compression only. */
	return mask == BIT(SVC_DC) || mask == BIT(SVC_DECOMP);
}

static int lce_dev_config(struct adf_accel_dev *accel_dev)
{
	int ret;

	ret = adf_cfg_section_add(accel_dev, ADF_KERNEL_SEC);
	if (ret)
		return ret;

	ret = adf_cfg_section_add(accel_dev, "Accelerator0");
	if (ret)
		return ret;

	ret = adf_gen6_comp_dev_config(accel_dev);
	if (ret) {
		dev_err(&GET_DEV(accel_dev), "Failed to configure LCE device\n");
		return ret;
	}

	set_bit(ADF_STATUS_CONFIGURED, &accel_dev->status);
	return 0;
}

static int adf_lce_build_comp_block(void *ctx, enum adf_dc_algo algo)
{
	struct icp_qat_fw_comp_req *req_tmpl = ctx;
	struct icp_qat_fw_comp_req_hdr_cd_pars *cd_pars = &req_tmpl->cd_pars;
	struct icp_qat_hw_comp_51_config_csr_lower hw_comp_lower_csr = { };
	struct icp_qat_fw_comn_req_hdr *header = &req_tmpl->comn_hdr;
	u32 lower_val;

	switch (algo) {
	case QAT_DEFLATE:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_DYNAMIC;
		break;
	default:
		return -EINVAL;
	}

	hw_comp_lower_csr.lllbd = ICP_QAT_HW_COMP_51_LLLBD_CTRL_LLLBD_DISABLED;
	hw_comp_lower_csr.sd = ICP_QAT_HW_COMP_51_SEARCH_DEPTH_LEVEL_1;
	lower_val = ICP_QAT_FW_COMP_51_BUILD_CONFIG_LOWER(hw_comp_lower_csr);
	cd_pars->u.sl.comp_slice_cfg_word[0] = lower_val;
	cd_pars->u.sl.comp_slice_cfg_word[1] = 0;

	return 0;
}

static int adf_lce_build_decomp_block(void *ctx, enum adf_dc_algo algo)
{
	struct icp_qat_fw_comp_req *req_tmpl = ctx;
	struct icp_qat_fw_comp_req_hdr_cd_pars *cd_pars = &req_tmpl->cd_pars;
	struct icp_qat_fw_comn_req_hdr *header = &req_tmpl->comn_hdr;

	switch (algo) {
	case QAT_DEFLATE:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_DECOMPRESS;
		break;
	default:
		return -EINVAL;
	}

	cd_pars->u.sl.comp_slice_cfg_word[0] = 0;
	cd_pars->u.sl.comp_slice_cfg_word[1] = 0;

	return 0;
}

static void adf_lce_init_dc_ops(struct adf_dc_ops *dc_ops)
{
	dc_ops->build_comp_block = adf_lce_build_comp_block;
	dc_ops->build_decomp_block = adf_lce_build_decomp_block;
}

void adf_init_hw_data_lce(struct adf_hw_device_data *hw_data, u32 num_banks)
{
	hw_data->dev_class = &lce_class;
	hw_data->instance_id = lce_class.instances++;
	hw_data->num_banks = num_banks;
	hw_data->num_banks_per_vf = 1;
	hw_data->num_rings_per_bank = ADF_LCE_NUM_RINGS_PER_BANK;
	hw_data->num_accel = 1;
	hw_data->num_engines = 0;
	hw_data->num_logical_accel = 1;
	hw_data->tx_rx_gap = ADF_LCE_RX_RINGS_OFFSET;
	hw_data->tx_rings_mask = ADF_LCE_TX_RINGS_MASK;
	hw_data->alloc_irq = adf_lce_isr_resource_alloc;
	hw_data->free_irq = adf_lce_isr_resource_free;
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
	hw_data->dev_config = lce_dev_config;
	hw_data->services_supported = services_supported;
	hw_data->accel_capabilities_mask = ADF_ACCEL_CAPABILITIES_COMPRESSION;
	hw_data->pfvf_ops.enable_comms = enable_pfvf_comms;

	adf_gen6_init_hw_csr_ops(&hw_data->csr_ops);
	adf_lce_init_dc_ops(&hw_data->dc_ops);
}

void adf_clean_hw_data_lce(struct adf_hw_device_data *hw_data)
{
	if (hw_data->dev_class)
		hw_data->dev_class->instances--;
}
