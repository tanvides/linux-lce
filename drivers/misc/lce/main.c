// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "lce_private.h"

#define PCI_DEVICE_ID_INTEL_LCE_APF	0x1455

static int lce_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct lce_hw_device *lcehw;

	lcehw = kzalloc(sizeof(*lcehw), GFP_KERNEL);
	if (!lcehw)
		return -ENOMEM;

	lcehw->pdev = pdev;
	dev_set_drvdata(&pdev->dev, lcehw);

	return 0;
}

static void lce_remove(struct pci_dev *pdev)
{
	struct lce_hw_device *lcehw = dev_get_drvdata(&pdev->dev);

	dev_set_drvdata(&pdev->dev, NULL);
	kfree(lcehw);
}

static const struct pci_device_id lce_pci_id_table[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_LCE_APF), },
	{ }
};
MODULE_DEVICE_TABLE(pci, lce_pci_id_table);

static struct pci_driver lce_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = lce_pci_id_table,
	.probe = lce_probe,
	.remove = lce_remove,
};
module_pci_driver(lce_pci_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Purna Chandra Mandal <purna.chandra.mandal@intel.com>");
MODULE_AUTHOR("Tanvi Desai <tanvi.desai@intel.com>");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology LCE PF driver");
