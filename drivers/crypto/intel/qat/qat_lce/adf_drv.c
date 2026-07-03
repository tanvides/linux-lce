// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_common_drv.h>

#include "adf_lce_hw_data.h"

/* PF reset registers */
#define LCE_PF_RSTGEN_CTRL	0x0104000CU
#define LCE_PF_RSTGEN_STAT	0x01040008U

static const struct pci_device_id adf_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, ADF_LCE_PCI_DEVICE_ID), },
	/*
	 * PROD2 is a second-source of the LCE PF using the same
	 * host-visible interface as the base LCE PF; both IDs share this
	 * driver's ring bundle layout and mailbox contract.
	 */
	{ PCI_VDEVICE(INTEL, ADF_LCE_PCI_DEVICE_ID_PROD2), },
	{ }
};
MODULE_DEVICE_TABLE(pci, adf_pci_tbl);

static void lce_pf_wait_for_reset(void __iomem *iobase)
{
	int ret;
	int val;

	ret = read_poll_timeout(readl, val, val == 0,
				10, 2 * USEC_PER_SEC, true,
				iobase + LCE_PF_RSTGEN_CTRL);
	if (ret) {
		writel(0, iobase + LCE_PF_RSTGEN_CTRL);
		writel(1, iobase + LCE_PF_RSTGEN_STAT);
		fsleep(11);
	}
}

static void lce_pci_reset_done(struct pci_dev *pdev)
{
	struct adf_accel_dev *accel_dev = adf_devmgr_pci_to_accel_dev(pdev);
	struct adf_bar *bar;

	if (!accel_dev)
		return;

	bar = &accel_dev->accel_pci_dev.pci_bars[0];
	if (bar->virt_addr)
		lce_pf_wait_for_reset(bar->virt_addr);
}

static void adf_cleanup_accel(struct adf_accel_dev *accel_dev)
{
	if (accel_dev->hw_device) {
		adf_clean_hw_data_lce(accel_dev->hw_device);
		accel_dev->hw_device = NULL;
	}
	adf_cfg_dev_remove(accel_dev);
	adf_devmgr_rm_dev(accel_dev, NULL);
}

static int adf_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct adf_accel_dev *accel_dev;
	struct adf_accel_pci *accel_pci_dev;
	struct adf_hw_device_data *hw_data;
	struct lce_hw_device *lcehw;
	struct adf_bar *bar;
	int ret;

	if (num_possible_nodes() > 1 && dev_to_node(&pdev->dev) < 0)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Invalid NUMA configuration\n");

	accel_dev = devm_kzalloc(&pdev->dev, sizeof(*accel_dev), GFP_KERNEL);
	if (!accel_dev)
		return -ENOMEM;

	INIT_LIST_HEAD(&accel_dev->crypto_list);
	INIT_LIST_HEAD(&accel_dev->compression_list);
	accel_pci_dev = &accel_dev->accel_pci_dev;
	accel_pci_dev->pci_dev = pdev;
	accel_dev->owner = THIS_MODULE;
	accel_dev->is_vf = false;

	ret = adf_devmgr_add_dev(accel_dev, NULL);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to add accelerator device\n");

	hw_data = devm_kzalloc(&pdev->dev, sizeof(*hw_data), GFP_KERNEL);
	if (!hw_data) {
		ret = -ENOMEM;
		goto out_err;
	}
	accel_dev->hw_device = hw_data;

	lcehw = devm_kzalloc(&pdev->dev, sizeof(*lcehw), GFP_KERNEL);
	if (!lcehw) {
		ret = -ENOMEM;
		goto out_err;
	}
	lcehw->pdev = pdev;
	lcehw->accel_dev = accel_dev;
	dev_set_drvdata(&pdev->dev, lcehw);

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Can't enable PCI device\n");
		goto out_err;
	}

	ret = pcim_request_all_regions(pdev, pci_name(pdev));
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Failed to request PCI regions\n");
		goto out_err;
	}

	bar = &accel_pci_dev->pci_bars[0];
	bar->virt_addr = pcim_iomap(pdev, 0, 0);
	if (!bar->virt_addr) {
		ret = dev_err_probe(&pdev->dev, -ENOMEM,
				    "Failed to ioremap BAR0\n");
		goto out_err;
	}
	lcehw->iobase = bar->virt_addr;

	/* Mirror BAR0 into the PMISC BAR slot so GEN4 CSR helpers reach
	 * the LCE misc register block via the same virtual mapping.
	 */
	accel_pci_dev->pci_bars[1].virt_addr = bar->virt_addr;
	accel_pci_dev->pci_bars[1].base_addr = bar->base_addr;
	accel_pci_dev->pci_bars[1].size = bar->size;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "No usable DMA configuration\n");
		goto out_err;
	}

	pci_set_master(pdev);
	lce_pf_wait_for_reset(lcehw->iobase);

	adf_init_hw_data_lce(hw_data);

	return 0;

out_err:
	adf_cleanup_accel(accel_dev);
	dev_set_drvdata(&pdev->dev, NULL);
	return ret;
}

static void adf_remove(struct pci_dev *pdev)
{
	struct lce_hw_device *lcehw = dev_get_drvdata(&pdev->dev);
	struct adf_accel_dev *accel_dev;

	if (!lcehw)
		return;

	accel_dev = lcehw->accel_dev;
	adf_cleanup_accel(accel_dev);
	dev_set_drvdata(&pdev->dev, NULL);
}

static const struct pci_error_handlers adf_lce_err_handler = {
	.reset_done = lce_pci_reset_done,
};

static struct pci_driver adf_lce_driver = {
	.name		 = KBUILD_MODNAME,
	.id_table	 = adf_pci_tbl,
	.probe		 = adf_probe,
	.remove		 = adf_remove,
	.err_handler	 = &adf_lce_err_handler,
};
module_pci_driver(adf_lce_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology LCE PF driver");
MODULE_SOFTDEP("pre: crypto-intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
