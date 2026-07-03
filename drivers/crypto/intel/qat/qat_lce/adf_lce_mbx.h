/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_LCE_MBX_H_
#define ADF_LCE_MBX_H_

#include <linux/bitfield.h>

struct lce_hw_device;

/* LCE mailbox is a pair of 32-bit registers with interrupt-to-peer
 * capability. Both peers can read and write the pair; BIT(0) is used to
 * raise an interrupt on the peer and the remaining bits are software
 * defined.
 */
#define LCE_MBX_MSGINT		BIT(0)
#define LCE_MBX_MSGV		BIT(1)
#define LCE_MBX_MSGTYPE		GENMASK(5, 2)
#define LCE_MBX_MSGDATA		GENMASK(31, 6)

/* PF->CPF message types */
#define LCE_MBX_OP_PF_INIT		1
#define LCE_MBX_OP_PF_SHUTDOWN		2
#define LCE_MBX_OP_PF_VERSION		3
#define LCE_MBX_OP_PF_COMPAT_VERSION	4
#define LCE_MBX_OP_PF_SRIOV_VF		5
#define LCE_MBX_OP_PF_QS_ALLOC		6
#define LCE_MBX_OP_PF_QS_FREE		7
#define LCE_MBX_OP_PF_QUEUE_CONTEXT	8
#define LCE_MBX_OP_PF_MSG_BLOCK		9
#define LCE_MBX_OP_PF_QUEUE_TYPE	10
#define LCE_MBX_OP_PF_IDENTIFY		11
#define LCE_MBX_OP_PF_CAPABILITY	12
#define LCE_MBX_OP_PF_QUEUE_RESET	13

/* CPF response encoding for OP_PF_COMPAT_VERSION */
#define LCE_MBX_PF_COMPAT_RESP_VERS	GENMASK(7, 0)
#define LCE_MBX_PF_COMPAT_RESP_RESULT	GENMASK(9, 8)
#define LCE_MBX_VERSION_COMPATIBLE	1

/* CPF response encoding for OP_PF_IDENTIFY */
#define LCE_MBX_PF_IDENTIFY_RESP_NODEID	GENMASK(14, 12)
#define LCE_MBX_PF_IDENTIFY_RESP_FTYPE	GENMASK(19, 18)
#define LCE_MBX_FTYPE_PF		0

void adf_lce_mbx_init(struct lce_hw_device *lcehw);
void adf_lce_mbx_cleanup(struct lce_hw_device *lcehw);

int adf_lce_mbx_request_version(struct lce_hw_device *lcehw);
int adf_lce_mbx_fetch_id(struct lce_hw_device *lcehw);
int adf_lce_mbx_num_vf(struct lce_hw_device *lcehw);
int adf_lce_mbx_alloc_qs(struct lce_hw_device *lcehw);
int adf_lce_mbx_free_qs(struct lce_hw_device *lcehw);

#endif /* ADF_LCE_MBX_H_ */
