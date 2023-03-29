// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "lce_private.h"

#define PCI_DEVICE_ID_INTEL_LCE_APF	0x1455

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

	lce_pf_wait_for_reset(lcehw);

	return 0;

out_free_reg:
	pci_release_regions(pdev);
out_disable_dev:
	pci_disable_device(pdev);
	return ret;
}

static void lce_pf_hw_deinit(struct lce_hw_device *lcehw)
{
	struct pci_dev *pdev = lcehw->pdev;

	pci_iounmap(pdev, lcehw->iobase);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static int lce_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct lce_hw_device *lcehw;
	int ret;

	lcehw = kzalloc(sizeof(*lcehw), GFP_KERNEL);
	if (!lcehw)
		return -ENOMEM;

	lcehw->pdev = pdev;
	dev_set_drvdata(&pdev->dev, lcehw);

	ret = lce_pf_hw_init(lcehw);
	if (ret) {
		dev_set_drvdata(&pdev->dev, NULL);
		kfree(lcehw);
	}

	return ret;
}

static void lce_remove(struct pci_dev *pdev)
{
	struct lce_hw_device *lcehw = dev_get_drvdata(&pdev->dev);

	lce_pf_hw_deinit(lcehw);
	dev_set_drvdata(&pdev->dev, NULL);
	kfree(lcehw);
}

static const struct pci_device_id lce_pci_id_table[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_LCE_APF), },
	{ }
};
MODULE_DEVICE_TABLE(pci, lce_pci_id_table);

static const struct pci_error_handlers lce_pci_err_handler = {
	.reset_done = lce_pci_reset_done,
};

static struct pci_driver lce_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = lce_pci_id_table,
	.probe = lce_probe,
	.remove = lce_remove,
	.err_handler = &lce_pci_err_handler,
};
module_pci_driver(lce_pci_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Purna Chandra Mandal <purna.chandra.mandal@intel.com>");
MODULE_AUTHOR("Tanvi Desai <tanvi.desai@intel.com>");
MODULE_DESCRIPTION("Intel(R) QuickAssist Technology LCE PF driver");
