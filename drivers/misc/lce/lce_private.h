/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef LCE_PRIVATE_H
#define LCE_PRIVATE_H

#include <linux/io.h>
#include <linux/pci.h>

#define LCE_CSR_WR(csr_base, csr_offset, val)	\
	writel((val), (csr_base) + (csr_offset))

#define LCE_CSR_RD(csr_base, csr_offset)	\
	readl((csr_base) + (csr_offset))

/* Constants */
#define LCE_APF_SRIOV_VF_MAX		1024

/* msix interrupt related parameters */
#define LCE_MAX_MSIX_VECTOR_NAME	32

struct lce_msix {
	char name[LCE_MAX_MSIX_VECTOR_NAME];
};

struct lce_hw_device {
	struct pci_dev *pdev;
	void __iomem *iobase;
	struct lce_msix *msix_entry;
	struct mutex mbx_lock; /* serialize mbx access */
	struct completion msg_completion;
	u32 mbx_resp;
	u32 max_vecs;
};

int lce_intr_init(struct lce_hw_device *lcehw);
int lce_mbx_request_version(struct lce_hw_device *lcehw);
int lce_mbx_fetch_id(struct lce_hw_device *lcehw);
int lce_mbx_num_vf(struct lce_hw_device *lcehw);

void lce_intr_deinit(struct lce_hw_device *lcehw);

#endif /* LCE_PRIVATE_H */
