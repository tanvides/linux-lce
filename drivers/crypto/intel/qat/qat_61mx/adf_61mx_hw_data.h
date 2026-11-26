/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_61MX_HW_DATA_H_
#define ADF_61MX_HW_DATA_H_

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/pci.h>

#include <adf_accel_devices.h>

/* CPF <-> APF mailbox and misc-interrupt-mask registers (BAR0). */
#define ADF_61MX_PF_INT_MASK			0x01100004U
#define ADF_61MX_PF_INT_CPF2APF			BIT_U32(1)
#define ADF_61MX_MBX_APF2CPF			0x01100008U
#define ADF_61MX_MBX_CPF2APF			0x0110000CU

/**
 * struct adf_61mx_dev - QAT_61MX PF private state.
 * @accel_dev:	parent QAT acceleration device.
 * @pdev:	backing PCI device.
 * @iobase:	BAR0 mapping.
 * @mbx_lock:	serializes mailbox exchanges.
 * @num_banks:	number of ring bundles assigned by CPF (from QS_ALLOC).
 */
struct adf_61mx_dev {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
	void __iomem *iobase;
	struct mutex mbx_lock; /* serialize mailbox exchanges */
	u32 num_banks;
};

void adf_init_hw_data_61mx(struct adf_hw_device_data *hw_data);
void adf_clean_hw_data_61mx(struct adf_hw_device_data *hw_data);

#endif /* ADF_61MX_HW_DATA_H_ */
