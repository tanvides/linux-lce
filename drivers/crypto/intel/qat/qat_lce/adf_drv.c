// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>

#include <adf_accel_devices.h>
#include <adf_common_drv.h>

#include "adf_lce_hw_data.h"

/* Reset registers */
#define LCE_PF_RSTGEN_CTRL	0x0104000CU
#define LCE_PF_RSTGEN_STAT	0x01040008U

static void lce_pf_wait_for_reset(const struct lce_hw_device *lcehw)
{
	int ret;
	int val;

	ret = read_poll_timeout(LCE_CSR_RD, val, val == 0,
				10, 2 * USEC_PER_SEC, 1,
				lcehw->iobase, LCE_PF_RSTGEN_CTRL);

	if (ret) {
		/* If reset state is not clear, force clear it */
		LCE_CSR_WR(lcehw->iobase, LCE_PF_RSTGEN_CTRL, 0);
		LCE_CSR_WR(lcehw->iobase, LCE_PF_RSTGEN_STAT, 1);
		fsleep(11);
	}
}

static void lce_pci_reset_done(struct pci_dev *pdev)
{
	const struct lce_hw_device *lcehw = dev_get_drvdata(&pdev->dev);

	if (!lcehw)
		return;

	lce_pf_wait_for_reset(lcehw);
}

static int lce_pf_hw_init(struct lce_hw_device *lcehw)
{
	struct pci_dev *pdev = lcehw->pdev;
	int ret;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	if (pci_request_regions(pdev, KBUILD_MODNAME)) {
		ret = -EFAULT;
		goto out_disable_dev;
	}

	lcehw->iobase = pci_iomap(pdev, 0, 0);
	if (!lcehw->iobase) {
		ret = -ENOMEM;
		goto out_free_reg;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		goto out_unmap;

	lce_pf_wait_for_reset(lcehw);
	pci_set_master(pdev);

	ret = adf_lce_intr_init(lcehw);
	if (ret)
		goto out_clear_master;

	return 0;

out_clear_master:
	pci_clear_master(pdev);
out_unmap:
	pci_iounmap(pdev, lcehw->iobase);
out_free_reg:
	pci_release_regions(pdev);
out_disable_dev:
	pci_disable_device(pdev);
	return ret;
}

static void lce_pf_hw_deinit(struct lce_hw_device *lcehw)
{
	struct pci_dev *pdev = lcehw->pdev;

	adf_lce_intr_deinit(lcehw);
	pci_clear_master(pdev);
	pci_iounmap(pdev, lcehw->iobase);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static int adf_lce_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct adf_hw_device_data *hw_data;
	struct adf_accel_dev *accel_dev;
	struct lce_hw_device *lcehw;
	int ret;

	accel_dev = devm_kzalloc(&pdev->dev, sizeof(*accel_dev), GFP_KERNEL);
	if (!accel_dev)
		return -ENOMEM;

	INIT_LIST_HEAD(&accel_dev->crypto_list);
	INIT_LIST_HEAD(&accel_dev->compression_list);
	accel_dev->accel_pci_dev.pci_dev = pdev;
	accel_dev->owner = THIS_MODULE;
	accel_dev->is_vf = false;

	hw_data = devm_kzalloc(&pdev->dev, sizeof(*hw_data), GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	hw_data->dev_class = &lce_class;
	hw_data->instance_id = lce_class.instances++;
	accel_dev->hw_device = hw_data;

	lcehw = devm_kzalloc(&pdev->dev, sizeof(*lcehw), GFP_KERNEL);
	if (!lcehw) {
		ret = -ENOMEM;
		goto out_class_dec;
	}

	lcehw->pdev = pdev;
	lcehw->accel_dev = accel_dev;
	dev_set_drvdata(&pdev->dev, lcehw);

	ret = adf_devmgr_add_dev(accel_dev, NULL);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add accel dev to devmgr\n");
		goto out_class_dec;
	}

	ret = lce_pf_hw_init(lcehw);
	if (ret)
		goto out_devmgr_rm;

	return 0;

out_devmgr_rm:
	adf_devmgr_rm_dev(accel_dev, NULL);
out_class_dec:
	lce_class.instances--;
	dev_set_drvdata(&pdev->dev, NULL);
	return ret;
}

static void adf_lce_remove(struct pci_dev *pdev)
{
	struct lce_hw_device *lcehw = dev_get_drvdata(&pdev->dev);

	if (!lcehw)
		return;

	lce_pf_hw_deinit(lcehw);
	adf_devmgr_rm_dev(lcehw->accel_dev, NULL);
	lce_class.instances--;
	dev_set_drvdata(&pdev->dev, NULL);
}

static const struct pci_device_id adf_lce_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, ADF_LCE_PCI_DEVICE_ID), },
	{ PCI_VDEVICE(INTEL, ADF_LCE_PCI_DEVICE_ID_PROD2), },
	{ }
};
MODULE_DEVICE_TABLE(pci, adf_lce_pci_tbl);

static const struct pci_error_handlers adf_lce_err_handler = {
	.reset_done = lce_pci_reset_done,
};

static struct pci_driver adf_lce_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= adf_lce_pci_tbl,
	.probe		= adf_lce_probe,
	.remove		= adf_lce_remove,
	.err_handler	= &adf_lce_err_handler,
};
module_pci_driver(adf_lce_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tanvi Desai <tanvi.desai@intel.com>");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology LCE PF driver");
MODULE_SOFTDEP("pre: intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
