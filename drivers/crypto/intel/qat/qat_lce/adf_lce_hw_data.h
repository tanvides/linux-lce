/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef ADF_LCE_HW_DATA_H_
#define ADF_LCE_HW_DATA_H_

#include <linux/pci.h>

#include <adf_accel_devices.h>

#define ADF_LCE_DEVICE_NAME		"lce"
#define ADF_LCE_PCI_DEVICE_ID		0x1455
#define ADF_LCE_PCI_DEVICE_ID_PROD2	0x11E1

/**
 * struct lce_hw_device - LCE PF private state.
 * @accel_dev:	parent QAT acceleration device (devmgr handle).
 * @pdev:	backing PCI device.
 *
 * LCE PF manages SR-IOV for user-space VF consumers (QATlib/vfio-pci)
 * and provides kernel-side compression offload via acomp (deflate/zstd).
 * It does not load firmware or run accelerator engines directly.
 */
struct lce_hw_device {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
};

extern struct adf_hw_device_class lce_class;

#endif /* ADF_LCE_HW_DATA_H_ */
