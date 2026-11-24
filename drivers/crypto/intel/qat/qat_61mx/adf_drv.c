// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_common_drv.h>
#include <adf_dbgfs.h>

#include "adf_61mx_hw_data.h"

/* PF reset registers */
#define ADF_61MX_PF_RSTGEN_CTRL	0x0104000CU
#define ADF_61MX_PF_RSTGEN_STAT	0x01040008U

static const struct pci_device_id adf_pci_tbl[] = {
	/* MEV shares the MMG-400 ring bundle layout and CPF mailbox protocol */
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_QAT_61VX) },
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_QAT_61MX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, adf_pci_tbl);

static void adf_61mx_pf_wait_for_reset(void __iomem *iobase)
{
	int ret;
	int val;

	ret = read_poll_timeout(readl, val, val == 0,
				10, 2 * USEC_PER_SEC, true,
				iobase + ADF_61MX_PF_RSTGEN_CTRL);
	if (ret) {
		writel(0, iobase + ADF_61MX_PF_RSTGEN_CTRL);
		writel(1, iobase + ADF_61MX_PF_RSTGEN_STAT);
		fsleep(11);
	}
}

static void adf_61mx_pci_reset_done(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);
	struct adf_bar *bar;

	if (!accel_dev)
		return;

	bar = &accel_dev->accel_pci_dev.pci_bars[0];
	if (bar->virt_addr)
		adf_61mx_pf_wait_for_reset(bar->virt_addr);
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
	adf_61mx_pf_wait_for_reset(hwdev->iobase);

	accel_dev->hw_device = hw_data;
	adf_init_hw_data_61mx(hw_data);

	ret = devm_add_action_or_reset(dev, adf_cleanup_hw_data, accel_dev);
	if (ret)
		return ret;

	ret = adf_cfg_dev_add(accel_dev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, adf_cfg_device_remove, accel_dev);
	if (ret)
		return ret;

	/*
	 * The PCI config space is saved at this point and will be restored
	 * after a Function Level Reset (FLR) as the FLR does not completely
	 * restore it.
	 */
	ret = pci_save_state(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to save pci state.\n");

	adf_dbgfs_init(accel_dev);

	return devm_add_action_or_reset(dev, adf_dbgfs_cleanup, accel_dev);
}

static const struct pci_error_handlers adf_61mx_err_handler = {
	.reset_done = adf_61mx_pci_reset_done,
};

static struct pci_driver adf_driver = {
	.id_table = adf_pci_tbl,
	.name = ADF_61MX_DEVICE_NAME,
	.probe = adf_probe,
	.err_handler = &adf_61mx_err_handler,
};
module_pci_driver(adf_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology for GEN61 Devices");
MODULE_SOFTDEP("pre: crypto-intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
