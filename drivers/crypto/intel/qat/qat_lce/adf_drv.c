// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/module.h>

#include <adf_accel_devices.h>
#include <adf_common_drv.h>

#include "adf_lce_hw_data.h"

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

	return 0;

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

static struct pci_driver adf_lce_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= adf_lce_pci_tbl,
	.probe		= adf_lce_probe,
	.remove		= adf_lce_remove,
};
module_pci_driver(adf_lce_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tanvi Desai <tanvi.desai@intel.com>");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology LCE PF driver");
MODULE_SOFTDEP("pre: intel_qat");
MODULE_IMPORT_NS("CRYPTO_QAT");
