// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/slab.h>

#include "lce_private.h"

/* handle interrupt reasons and msgs */
static irqreturn_t lce_pf_mbx_irq(s32 irq, void *arg)
{
	return IRQ_HANDLED;
}

int lce_intr_init(struct lce_hw_device *lcehw)
{
	struct pci_dev *pdev = lcehw->pdev;
	struct device *dev = &pdev->dev;
	struct lce_msix *msix;
	int nvec;
	int ret;

	/* enable the msix vector for LCE */
	nvec = pci_msix_vec_count(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, nvec, PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(dev, "Failed to enable MSI-X IRQ");
		ret = -EFAULT;
		goto out_free_msix;
	}

	msix = kcalloc(ret, sizeof(*msix), GFP_KERNEL);
	if (!msix)
		return -ENOMEM;

	lcehw->msix_entry = msix;
	lcehw->max_vecs = ret;

	/* Request msix irq */
	snprintf(msix[0].name, sizeof(msix[0].name),
		 "lce-vfio-mbx(%s)", pci_name(pdev));

	ret = request_irq(pci_irq_vector(pdev, 0), lce_pf_mbx_irq, 0,
			  msix[0].name, lcehw);
	if (ret) {
		dev_err(dev, "failed to enable irq %d, for %s\n",
			pci_irq_vector(pdev, 0), msix[0].name);
		goto out_free_vectors;
	}

	return 0;

out_free_vectors:
	pci_free_irq_vectors(lcehw->pdev);
out_free_msix:
	kfree(lcehw->msix_entry);
	return ret;
}

void lce_intr_deinit(struct lce_hw_device *lcehw)
{
	struct lce_msix *msix = lcehw->msix_entry;

	free_irq(pci_irq_vector(lcehw->pdev, 0), lcehw);
	pci_free_irq_vectors(lcehw->pdev);
	kfree(msix);
}
