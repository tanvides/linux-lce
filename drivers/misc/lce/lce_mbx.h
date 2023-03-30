/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef LCE_MBX_H
#define LCE_MBX_H

#include <linux/bitfield.h>

/* LCE mailbox is a pair of 32-bit registers with interrupt-to-peer
 * capability. These registers are fully accessible to both sides.
 * One side writes message and peer reads from the register. Usually,
 * only 1 writer is allowed, conflict is managed by driver at peer.
 *
 * BIT(0) is used for generating interrupt to peer. And remaining
 * bits are software defined and used same way by the entities.
 *
 * Here is the overall definition of LCE mailbox registers
 */
#define LCE_MBX_MSGINT          BIT(0)
#define LCE_MBX_MSGV            BIT(1)
#define LCE_MBX_MSGTYPE         GENMASK(5, 2)
#define LCE_MBX_MSGDATA         GENMASK(31, 6)

/* Following are the message types exchanged between two peers. */

/* PF->CPF messages */
#define LCE_MBX_OP_PF_INIT		1
#define LCE_MBX_OP_PF_SHUTDOWN		2
#define LCE_MBX_OP_PF_VERSION		3
#define LCE_MBX_OP_PF_COMPAT_VERSION	4
#define LCE_MBX_OP_PF_SRIOV_VF		5
#define LCE_MBX_OP_PF_QS_ALLOC		6
#define LCE_MBX_OP_PF_QS_FREE		7
#define LCE_MBX_OP_PF_QUEUE_CONTEXT	8 /* queue context */
#define LCE_MBX_OP_PF_MSG_BLOCK		9
#define LCE_MBX_OP_PF_QUEUE_TYPE	10
#define LCE_MBX_OP_PF_IDENTIFY		11
#define LCE_MBX_OP_PF_CAPABILITY	12 /* capability */
#define LCE_MBX_OP_PF_QUEUE_RESET	13
#define LCE_MBX_OP_PF_MAX		14

/* CPF->PF messages */
#define LCE_MBX_OP_PF_NOTIFY_RESTARTING	1
#define LCE_MBX_OP_PF_VERSION_RESP	2
#define LCE_MBX_OP_PF_NOTIFY_FATAL_ERR	3
#define LCE_MBX_OP_PF_SRIOV_VF_RESP	4
#define LCE_MBX_OP_PF_QS_ALLOC_RESP	5
#define LCE_MBX_OP_PF_IDENTIFY_RESP	6
#define LCE_MBX_OP_PF_CAPABILITY_RESP	7
#define LCE_MBX_OP_PF_MSG_BLOCK_RESP	8

/* VF->CPF messages */
#define LCE_MBX_OP_VF_INIT		3
#define LCE_MBX_OP_VF_SHUTDOWN		4
#define LCE_MBX_OP_VF_VERSION		5
#define LCE_MBX_OP_VF_COMPAT_VERSION	6
#define LCE_MBX_OP_VF_QUEUE_TYPE	7
#define LCE_MBX_OP_VF_QUEUE_RESET	8
#define LCE_MBX_OP_VF_MSG_BLOCK		9
#define LCE_MBX_OP_VF_IDENTIFY		10

/* CPF->VF messages */
#define LCE_MBX_OP_VF_NOTIFY_RESTARTING	1
#define LCE_MBX_OP_VF_VERSION_RESP	2
#define LCE_MBX_OP_VF_BLOCK_RESP	3
#define LCE_MBX_OP_VF_NOTIFY_FATAL_ERR	4
#define LCE_MBX_OP_VF_IDENTIFY_RESP	10

/* LCE_MBX_OP_PF_COMPAT_VERSION  */
#define LCE_MBX_PF_COMPAT_VERS		GENMASK(7, 0)

/* CPF->APF Version Response - LCE_CPFAPF_MSGTYPE_VERSION_RESP */
#define LCE_MBX_PF_COMPAT_RESP_VERS	GENMASK(7, 0)
#define LCE_MBX_PF_COMPAT_RESP_RESULT	GENMASK(9, 8)

#define LCE_MBX_VERSION_COMPATIBLE	1
#define LCE_MBX_VERSION_INCOMPATIBLE	2
#define LCE_MBX_VERSION_UNKNOWN		3

/* LCE_MBX_OP_PF_IDENTIFY_RESP */
#define LCE_MBX_PF_IDENTIFY_RESP_VFID		GENMASK(11, 1)
#define LCE_MBX_PF_IDENTIFY_RESP_NODEID		GENMASK(14, 12)
#define LCE_MBX_PF_IDENTIFY_RESP_PFID		GENMASK(17, 15)
#define LCE_MBX_PF_IDENTIFY_RESP_FTYPE		GENMASK(19, 18)

#define LCE_MBX_FTYPE_PF	0
#define LCE_MBX_FTYPE_VF	1

/* LCE_MBX_OP_PF_QUEUE_TYPE or LCE_MBX_OP_PF_QUEUE_CONTEXT */
#define LCE_MBX_PF_MSGDATA_QID		GENMASK(11, 0)
#define LCE_MBX_PF_MSGDATA_SVC_TYPE	GENMASK(15, 12)

/* For  LCE_MBX_OP_PF_QUEUE_RESET */
#define LCE_MBX_PF_MSGDATA_NUM		GENMASK(24, 12)

/* mbx message data format for extended cap message */
#define LCE_MBX_PF_MSG_BLOCK_CMD	GENMASK(3, 0)
#define LCE_MBX_PF_MSG_BLOCK_LEN	GENMASK(15, 4)
#define LCE_MBX_PF_MSG_BLOCK_POS	GENMASK(20, 16)

#define LCE_MBX_PF_MSG_BLOCK_MD		BIT(21)
#define LCE_MBX_PF_MSG_BLOCK_AAD	BIT(22)

/* VF->PF Compatible Version Request - LCE_MBX_OP_VF_VERSION */
#define LCE_MBX_VF_COMPAT_VERS		GENMASK(7, 0)

/* PF->VF Version Response - LCE_MBX_OP_VF_VERSION_RESP */
#define LCE_MBX_VF_COMPAT_RESP_VERS	GENMASK(7, 0)
#define LCE_MBX_VF_COMPAT_RESP_RESULT	GENMASK(9, 8)

/* PF-VF identify response -  LCE_MBX_OP_VF_IDENTIFY_RESP */
#define LCE_MBX_VF_IDENTIFY_RESP_VFID		GENMASK(10, 1)
#define LCE_MBX_VF_IDENTIFY_RESP_NODEID		GENMASK(13, 11)
#define LCE_MBX_VF_IDENTIFY_RESP_PFID		GENMASK(16, 14)
#define LCE_MBX_VF_IDENTIFY_RESP_FTYPE		BIT(17)

#endif /* LCE_MBX_H */
