/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef LCE_PRIVATE_H
#define LCE_PRIVATE_H

#include <linux/pci.h>

#define LCE_CSR_WR(csr_base, csr_offset, val)	\
	writel((val), (csr_base) + (csr_offset))

#define LCE_CSR_RD(csr_base, csr_offset)	\
	readl((csr_base) + (csr_offset))

struct lce_hw_device {
	struct pci_dev *pdev;
	void __iomem *iobase;
};

#endif /* LCE_PRIVATE_H */
