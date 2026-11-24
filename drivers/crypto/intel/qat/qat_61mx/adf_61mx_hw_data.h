/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_61MX_HW_DATA_H_
#define ADF_61MX_HW_DATA_H_

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sched.h>

#include <adf_accel_devices.h>

/* Upper limit on VFs advertised by the APF; the CPF-side firmware
 * cannot expose more than this per PF regardless of BAR configuration.
 */
#define ADF_61MX_APF_SRIOV_VF_MAX		1024

#define ADF_61MX_CSR_WR(base, off, val)	writel((val), (base) + (off))
#define ADF_61MX_CSR_RD(base, off)		readl((base) + (off))

/* CPF <-> APF mailbox and misc-interrupt-mask registers (BAR0). */
#define ADF_61MX_PF_INT_MASK			0x01100004U
#define ADF_61MX_MBX_APF2CPF			0x01100008U
#define ADF_61MX_MBX_CPF2APF			0x0110000CU

/**
 * struct adf_61mx_dev - QAT_61MX PF private state.
 * @accel_dev:	parent QAT acceleration device.
 * @pdev:	backing PCI device.
 * @iobase:	BAR0 mapping.
 * @mbx_lock:	serializes mailbox access.
 * @mbx_resp:	last mailbox response payload.
 * @num_banks:	number of ring bundles assigned by CPF (from QS_ALLOC).
 */
struct adf_61mx_dev {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
	void __iomem *iobase;
	struct mutex mbx_lock; /* serialize mailbox transactions */
	u32 mbx_resp;
	u32 num_banks;
};

void adf_init_hw_data_61mx(struct adf_hw_device_data *hw_data);
void adf_clean_hw_data_61mx(struct adf_hw_device_data *hw_data);

#endif /* ADF_61MX_HW_DATA_H_ */
