/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef LCE_PRIVATE_H
#define LCE_PRIVATE_H

#include <linux/pci.h>

struct lce_hw_device {
	struct pci_dev *pdev;
};

#endif /* LCE_PRIVATE_H */
