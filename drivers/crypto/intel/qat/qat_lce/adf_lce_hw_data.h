/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef ADF_LCE_HW_DATA_H_
#define ADF_LCE_HW_DATA_H_

#include <linux/io.h>
#include <linux/pci.h>

#include <adf_accel_devices.h>

#define ADF_LCE_DEVICE_NAME		"lce"
#define ADF_LCE_PCI_DEVICE_ID		0x1455
#define ADF_LCE_PCI_DEVICE_ID_PROD2	0x11E1

#define LCE_CSR_WR(csr_base, csr_offset, val)	\
	writel((val), (csr_base) + (csr_offset))

#define LCE_CSR_RD(csr_base, csr_offset)	\
	readl((csr_base) + (csr_offset))

/**
 * struct lce_hw_device - LCE PF private state.
 * @accel_dev:	parent QAT acceleration device (devmgr handle).
 * @pdev:	backing PCI device.
 * @iobase:	BAR0 mapping.
 *
 * LCE PF manages SR-IOV for user-space VF consumers (QATlib/vfio-pci)
 * and provides kernel-side compression offload via acomp (deflate/zstd).
 * It does not load firmware or run accelerator engines directly.
 */
struct lce_hw_device {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
	void __iomem *iobase;
};

extern struct adf_hw_device_class lce_class;

#endif /* ADF_LCE_HW_DATA_H_ */
