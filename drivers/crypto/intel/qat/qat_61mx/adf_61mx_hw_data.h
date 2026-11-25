/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_61MX_HW_DATA_H_
#define ADF_61MX_HW_DATA_H_

#include <linux/pci.h>

#include <adf_accel_devices.h>

/**
 * struct adf_61mx_dev - QAT_61MX PF private state.
 * @accel_dev:	parent QAT acceleration device.
 * @pdev:	backing PCI device.
 * @iobase:	BAR0 mapping.
 */
struct adf_61mx_dev {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
	void __iomem *iobase;
};

void adf_init_hw_data_61mx(struct adf_hw_device_data *hw_data);
void adf_clean_hw_data_61mx(struct adf_hw_device_data *hw_data);

#endif /* ADF_61MX_HW_DATA_H_ */
