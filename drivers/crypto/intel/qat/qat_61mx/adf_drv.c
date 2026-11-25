// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_cfg_strings.h>
#include <adf_common_drv.h>
#include <adf_dbgfs.h>

#include "adf_61mx_hw_data.h"
#include "adf_61mx_mbx.h"

/* PF reset registers */
#define ADF_61MX_PF_RSTGEN_CTRL		0x0104000CU
#define ADF_61MX_PF_RSTGEN_STAT		0x01040008U
#define ADF_61MX_PF_RST_POLL_US		10
#define ADF_61MX_PF_RST_TIMEOUT_US	(2 * USEC_PER_SEC)
#define ADF_61MX_PF_RST_CLEAR_US	11

static const struct pci_device_id adf_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_QAT_61MX) },
	/* MEV shares the MMG-400 ring bundle layout and CPF mailbox protocol */
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_QAT_61VX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, adf_pci_tbl);

static void adf_61mx_pf_wait_for_reset(struct adf_61mx_dev *hwdev)
{
	void __iomem *iobase = hwdev->iobase;
	u32 val;

	if (!readl_poll_timeout(iobase + ADF_61MX_PF_RSTGEN_CTRL, val, !val,
				ADF_61MX_PF_RST_POLL_US,
				ADF_61MX_PF_RST_TIMEOUT_US))
		return;

	dev_warn(&hwdev->pdev->dev, "PF reset did not complete, clearing it\n");
	ADF_CSR_WR(iobase, ADF_61MX_PF_RSTGEN_CTRL, 0);
	ADF_CSR_WR(iobase, ADF_61MX_PF_RSTGEN_STAT, 1);
	fsleep(ADF_61MX_PF_RST_CLEAR_US);
}

/* Return the number of ring pairs the CPF assigned to the PF */
static int adf_61mx_cpf_connect(struct adf_61mx_dev *hwdev)
{
	int ret;

	ret = adf_61mx_mbx_request_version(hwdev);
	if (ret)
		return ret;

	ret = adf_61mx_mbx_fetch_id(hwdev);
	if (ret)
		return ret;

	ret = adf_61mx_mbx_alloc_qs(hwdev);
	if (ret < 0)
		return ret;

	/* hw_data->num_banks is a u8 */
	if (ret > U8_MAX) {
		dev_err(&hwdev->pdev->dev, "Invalid number of ring pairs: %d\n",
			ret);
		return -EINVAL;
	}

	return ret;
}

static void adf_61mx_pci_reset_prepare(struct pci_dev *pdev)
{
	struct adf_61mx_dev *hwdev = pci_get_drvdata(pdev);
	struct adf_accel_dev *accel_dev = hwdev->accel_dev;

	set_bit(ADF_STATUS_RESTARTING, &accel_dev->status);
	hwdev->restart = adf_dev_started(accel_dev);
	if (hwdev->restart)
		adf_dev_down(accel_dev);
}

static void adf_61mx_pci_reset_done(struct pci_dev *pdev)
{
	struct adf_61mx_dev *hwdev = pci_get_drvdata(pdev);
	struct adf_accel_dev *accel_dev = hwdev->accel_dev;
	int ret;

	adf_61mx_pf_wait_for_reset(hwdev);
	adf_61mx_mbx_enable(hwdev);

	ret = adf_61mx_cpf_connect(hwdev);
	if (ret >= 0 && ret != hwdev->num_banks) {
		dev_err(&pdev->dev, "CPF ring pair assignment changed\n");
		ret = -EINVAL;
	}
	if (ret >= 0 && hwdev->restart)
		ret = adf_dev_up(accel_dev, false);

	clear_bit(ADF_STATUS_RESTARTING, &accel_dev->status);
	if (ret < 0)
		dev_err(&pdev->dev, "Failed to recover from reset: %d\n", ret);
}

static int adf_61mx_sriov_configure(struct pci_dev *pdev, int vfs)
{
	int ret;

	if (vfs > 0) {
		ret = pci_enable_sriov(pdev, vfs);
		return ret ? ret : vfs;
	}

	if (pci_num_vf(pdev))
		pci_disable_sriov(pdev);

	return 0;
}

static void adf_device_down(void *accel_dev)
{
	adf_dev_down(accel_dev);
}

static void adf_dbgfs_cleanup(void *accel_dev)
{
	adf_dbgfs_exit(accel_dev);
}

static void adf_cfg_device_remove(void *accel_dev)
{
	adf_cfg_dev_remove(accel_dev);
}

static void adf_cleanup_hw_data(void *accel_dev)
{
	struct adf_accel_dev *accel_device = accel_dev;

	if (accel_device->hw_device) {
		adf_clean_hw_data_61mx(accel_device->hw_device);
		accel_device->hw_device = NULL;
	}
}

static void adf_devmgr_remove(void *accel_dev)
{
	adf_devmgr_rm_dev(accel_dev, NULL);
}

static void adf_61mx_mbx_teardown(void *hwdev)
{
	adf_61mx_mbx_disable(hwdev);
}

static void adf_61mx_disable_sriov(void *pdev)
{
	pci_disable_sriov(pdev);
}

/* Crypto services run on VFs; only compression is offloaded by the PF */
static int adf_61mx_cfg_dev_init(struct adf_accel_dev *accel_dev)
{
	int ret;

	ret = adf_cfg_section_add(accel_dev, ADF_GENERAL_SEC);
	if (ret)
		return ret;

	return adf_cfg_add_key_value_param(accel_dev, ADF_GENERAL_SEC,
					   ADF_SERVICES_ENABLED,
					   ADF_CFG_SYM ADF_SERVICES_DELIMITER
					   ADF_CFG_ASYM ADF_SERVICES_DELIMITER
					   ADF_CFG_DC,
					   ADF_STR);
}

/*
 * Negotiate with the CPF over the polled mailbox. This must complete before
 * adf_dev_up(): the number of ring bundles assigned by the CPF is needed to
 * set up the transport.
 */
static int adf_61mx_cpf_init(struct adf_61mx_dev *hwdev)
{
	struct device *dev = &hwdev->pdev->dev;
	int max_vfs;
	int ret;

	ret = devm_mutex_init(dev, &hwdev->mbx_lock);
	if (ret)
		return ret;

	adf_61mx_mbx_enable(hwdev);

	ret = devm_add_action_or_reset(dev, adf_61mx_mbx_teardown, hwdev);
	if (ret)
		return ret;

	ret = adf_61mx_cpf_connect(hwdev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "CPF negotiation failed.\n");
	hwdev->num_banks = ret;
	dev_dbg(dev, "CPF assigned %u ring pairs\n", hwdev->num_banks);

	max_vfs = adf_61mx_mbx_num_vf(hwdev);
	if (max_vfs < 0)
		return dev_err_probe(dev, max_vfs, "Failed to get number of VFs.\n");
	if (max_vfs > ADF_61MX_APF_SRIOV_VF_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid number of SR-IOV VFs: %d.\n", max_vfs);

	ret = pci_sriov_set_totalvfs(hwdev->pdev, max_vfs);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set total VFs.\n");

	return 0;
}

static int adf_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct adf_accel_pci *accel_pci_dev;
	struct adf_hw_device_data *hw_data;
	struct device *dev = &pdev->dev;
	struct adf_accel_dev *accel_dev;
	struct adf_61mx_dev *hwdev;
	struct adf_bar *bar;
	int ret;

	if (num_possible_nodes() > 1 && dev_to_node(dev) < 0) {
		/*
		 * If the accelerator is connected to a node with no memory
		 * there is no point in using the accelerator since the remote
		 * memory transaction will be very slow.
		 */
		return dev_err_probe(dev, -EINVAL, "Invalid NUMA configuration.\n");
	}

	accel_dev = devm_kzalloc(dev, sizeof(*accel_dev), GFP_KERNEL);
	if (!accel_dev)
		return -ENOMEM;

	INIT_LIST_HEAD(&accel_dev->crypto_list);
	INIT_LIST_HEAD(&accel_dev->compression_list);
	accel_pci_dev = &accel_dev->accel_pci_dev;
	accel_pci_dev->pci_dev = pdev;
	accel_dev->owner = THIS_MODULE;

	hw_data = devm_kzalloc(dev, sizeof(*hw_data), GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	hwdev = devm_kzalloc(dev, sizeof(*hwdev), GFP_KERNEL);
	if (!hwdev)
		return -ENOMEM;

	hwdev->pdev = pdev;
	hwdev->accel_dev = accel_dev;
	pci_set_drvdata(pdev, hwdev);

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot enable PCI device.\n");

	ret = adf_devmgr_add_dev(accel_dev, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add new accelerator device.\n");

	ret = devm_add_action_or_reset(dev, adf_devmgr_remove, accel_dev);
	if (ret)
		return ret;

	bar = &accel_pci_dev->pci_bars[0];
	bar->virt_addr = pcim_iomap_region(pdev, 0, pci_name(pdev));
	if (IS_ERR(bar->virt_addr))
		return dev_err_probe(dev, PTR_ERR(bar->virt_addr),
				     "Failed to ioremap PCI region.\n");
	hwdev->iobase = bar->virt_addr;

	/* The misc CSRs live in BAR0 too; alias it for the PMISC accessors */
	accel_pci_dev->pci_bars[1] = *bar;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return dev_err_probe(dev, ret, "No usable DMA configuration.\n");

	pci_set_master(pdev);
	adf_61mx_pf_wait_for_reset(hwdev);

	ret = adf_61mx_cpf_init(hwdev);
	if (ret)
		return ret;

	accel_dev->hw_device = hw_data;
	adf_init_hw_data_61mx(hw_data, hwdev->num_banks);

	ret = devm_add_action_or_reset(dev, adf_cleanup_hw_data, accel_dev);
	if (ret)
		return ret;

	ret = adf_cfg_dev_add(accel_dev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, adf_cfg_device_remove, accel_dev);
	if (ret)
		return ret;

	ret = adf_61mx_cfg_dev_init(accel_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize configuration.\n");

	/*
	 * The PCI config space is saved at this point and will be restored
	 * after a Function Level Reset (FLR) as the FLR does not completely
	 * restore it.
	 */
	ret = pci_save_state(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to save pci state.\n");

	adf_dbgfs_init(accel_dev);

	ret = devm_add_action_or_reset(dev, adf_dbgfs_cleanup, accel_dev);
	if (ret)
		return ret;

	if (hwdev->num_banks) {
		ret = adf_dev_up(accel_dev, true);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(dev, adf_device_down, accel_dev);
		if (ret)
			return ret;
	} else {
		dev_info(dev, "No ring pairs assigned by the CPF, SR-IOV only.\n");
	}

	/* VFs must be gone before the PF is brought down */
	ret = devm_add_action_or_reset(dev, adf_61mx_disable_sriov, pdev);
	if (ret)
		return ret;

	return adf_sysfs_init(accel_dev);
}

static void adf_shutdown(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);

	adf_dev_down(accel_dev);
}

static const struct pci_error_handlers adf_61mx_err_handler = {
	.reset_prepare = adf_61mx_pci_reset_prepare,
	.reset_done = adf_61mx_pci_reset_done,
};

static struct pci_driver adf_driver = {
	.id_table = adf_pci_tbl,
	.name = ADF_61MX_DEVICE_NAME,
	.probe = adf_probe,
	.shutdown = adf_shutdown,
	.sriov_configure = adf_61mx_sriov_configure,
	.err_handler = &adf_61mx_err_handler,
};
module_pci_driver(adf_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology for GEN6.1 Devices");
MODULE_SOFTDEP("pre: crypto-intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
