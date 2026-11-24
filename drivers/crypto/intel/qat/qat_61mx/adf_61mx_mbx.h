/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_61MX_MBX_H_
#define ADF_61MX_MBX_H_

#include <linux/bitfield.h>
#include <linux/interrupt.h>

struct adf_61mx_dev;

/* The CPF mailbox is a pair of 32-bit registers with interrupt-to-peer
 * capability. Both peers can read and write the pair; BIT(0) is used to
 * raise an interrupt on the peer and the remaining bits are software
 * defined.
 */
#define ADF_61MX_MBX_MSGINT		BIT(0)
#define ADF_61MX_MBX_MSGV		BIT(1)
#define ADF_61MX_MBX_MSGTYPE		GENMASK(5, 2)
#define ADF_61MX_MBX_MSGDATA		GENMASK(31, 6)

/* PF->CPF message types (values fixed by the CPF firmware protocol) */
#define ADF_61MX_MBX_OP_PF_COMPAT_VERSION	4
#define ADF_61MX_MBX_OP_PF_SRIOV_VF		5
#define ADF_61MX_MBX_OP_PF_QS_ALLOC		6
#define ADF_61MX_MBX_OP_PF_QS_FREE		7
#define ADF_61MX_MBX_OP_PF_IDENTIFY		11

/* CPF response encoding for OP_PF_COMPAT_VERSION */
#define ADF_61MX_MBX_PF_COMPAT_RESP_VERS	GENMASK(7, 0)
#define ADF_61MX_MBX_PF_COMPAT_RESP_RESULT	GENMASK(9, 8)
#define ADF_61MX_MBX_VERSION_COMPATIBLE	1

/* CPF response encoding for OP_PF_IDENTIFY */
#define ADF_61MX_MBX_PF_IDENTIFY_RESP_NODEID	GENMASK(14, 12)
#define ADF_61MX_MBX_PF_IDENTIFY_RESP_FTYPE	GENMASK(19, 18)
#define ADF_61MX_MBX_FTYPE_PF		0

void adf_61mx_mbx_init(struct adf_61mx_dev *hwdev);
void adf_61mx_mbx_cleanup(struct adf_61mx_dev *hwdev);

int adf_61mx_mbx_request_version(struct adf_61mx_dev *hwdev);
int adf_61mx_mbx_fetch_id(struct adf_61mx_dev *hwdev);
int adf_61mx_mbx_num_vf(struct adf_61mx_dev *hwdev);
int adf_61mx_mbx_alloc_qs(struct adf_61mx_dev *hwdev);
int adf_61mx_mbx_free_qs(struct adf_61mx_dev *hwdev);

irqreturn_t adf_61mx_mbx_isr(int irq, void *dev_ptr);

#endif /* ADF_61MX_MBX_H_ */
