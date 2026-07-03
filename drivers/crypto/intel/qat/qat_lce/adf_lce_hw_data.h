/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_LCE_HW_DATA_H_
#define ADF_LCE_HW_DATA_H_

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sched.h>

#include <adf_accel_devices.h>

#define ADF_LCE_DEVICE_NAME		"lce"
#define ADF_LCE_PCI_DEVICE_ID		0x1455
#define ADF_LCE_PCI_DEVICE_ID_PROD2	0x11E1

/* Upper limit on VFs advertised by the LCE APF; the CPF-side firmware
 * cannot expose more than this per PF regardless of BAR configuration.
 */
#define LCE_APF_SRIOV_VF_MAX		1024

#define LCE_CSR_WR(base, off, val)	writel((val), (base) + (off))
#define LCE_CSR_RD(base, off)		readl((base) + (off))

/**
 * struct lce_hw_device - LCE PF private state.
 * @accel_dev:	parent QAT acceleration device.
 * @pdev:	backing PCI device.
 * @iobase:	BAR0 mapping.
 * @mbx_lock:	serializes mailbox access.
 * @mbx_resp:	last mailbox response payload.
 * @num_banks:	number of ring bundles assigned by CPF (from QS_ALLOC).
 * @poll_thread: response polling kthread (see adf_lce_poll.c).
 */
struct lce_hw_device {
	struct adf_accel_dev *accel_dev;
	struct pci_dev *pdev;
	void __iomem *iobase;
	struct mutex mbx_lock; /* serialize mailbox transactions */
	u32 mbx_resp;
	u32 num_banks;
	struct task_struct *poll_thread;
};

void adf_init_hw_data_lce(struct adf_hw_device_data *hw_data);
void adf_clean_hw_data_lce(struct adf_hw_device_data *hw_data);

#endif /* ADF_LCE_HW_DATA_H_ */
