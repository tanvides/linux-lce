// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/iopoll.h>
#include <linux/mutex.h>

#include "adf_61mx_hw_data.h"
#include "adf_61mx_mbx.h"

#define to_dev(hwdev)	(&(hwdev)->pdev->dev)

#define ADF_61MX_MAX_PF			4
#define ADF_61MX_MBX_POLL_US			10
#define ADF_61MX_MBX_RESP_TIMEOUT_US		(2 * USEC_PER_SEC)
#define ADF_61MX_MBX_ACK_TIMEOUT_US		(500 * USEC_PER_MSEC)
#define ADF_61MX_MBX_COMPAT_VER_MSG_DATA	5

/*
 * Send a message to the CPF and get the data of its response. The CPF
 * acknowledges a message only after its response has been consumed.
 */
static int adf_61mx_mbx_xchange(struct adf_61mx_dev *hwdev, u32 msg_type,
				u32 msg_data, u32 resp_type, u32 *resp)
{
	void __iomem *regbase = hwdev->iobase;
	u32 val, ack;
	int ret;

	mutex_lock(&hwdev->mbx_lock);

	/* Drop a stale message, such as an unsolicited CPF notification */
	ADF_CSR_WR(regbase, ADF_61MX_MBX_CPF2APF, 0);
	ADF_CSR_WR(regbase, ADF_61MX_MBX_APF2CPF,
		   FIELD_PREP(ADF_61MX_MBX_MSGTYPE, msg_type) |
		   FIELD_PREP(ADF_61MX_MBX_MSGDATA, msg_data) |
		   ADF_61MX_MBX_MSGV | ADF_61MX_MBX_MSGINT);

	ret = read_poll_timeout(ADF_CSR_RD, val, val & ADF_61MX_MBX_MSGINT,
				ADF_61MX_MBX_POLL_US,
				ADF_61MX_MBX_RESP_TIMEOUT_US, false,
				regbase, ADF_61MX_MBX_CPF2APF);
	if (ret) {
		dev_err(to_dev(hwdev), "No CPF response to message type %u\n",
			msg_type);
		ADF_CSR_WR(regbase, ADF_61MX_MBX_APF2CPF, 0);
		goto out;
	}

	ADF_CSR_WR(regbase, ADF_61MX_MBX_CPF2APF, 0);

	ret = read_poll_timeout(ADF_CSR_RD, ack, !(ack & ADF_61MX_MBX_MSGINT),
				ADF_61MX_MBX_POLL_US,
				ADF_61MX_MBX_ACK_TIMEOUT_US, false,
				regbase, ADF_61MX_MBX_APF2CPF);
	if (ret) {
		dev_err(to_dev(hwdev), "No CPF ack for message type %u\n",
			msg_type);
		ADF_CSR_WR(regbase, ADF_61MX_MBX_APF2CPF, 0);
		goto out;
	}

	if (FIELD_GET(ADF_61MX_MBX_MSGTYPE, val) != resp_type) {
		dev_err(to_dev(hwdev),
			"Unexpected CPF response 0x%08x to message type %u\n",
			val, msg_type);
		ret = -EIO;
		goto out;
	}

	*resp = FIELD_GET(ADF_61MX_MBX_MSGDATA, val);
out:
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

int adf_61mx_mbx_request_version(struct adf_61mx_dev *hwdev)
{
	u32 result;
	u32 resp;
	int ret;

	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_COMPAT_VERSION,
				   ADF_61MX_MBX_COMPAT_VER_MSG_DATA,
				   ADF_61MX_MBX_RESP_COMPAT_VERSION, &resp);
	if (ret)
		return ret;

	result = FIELD_GET(ADF_61MX_MBX_PF_COMPAT_RESP_RESULT, resp);
	if (result != ADF_61MX_MBX_VERSION_COMPATIBLE) {
		dev_err(to_dev(hwdev), "Incompatible CPF version, result %u\n",
			result);
		return -EINVAL;
	}

	return 0;
}

int adf_61mx_mbx_fetch_id(struct adf_61mx_dev *hwdev)
{
	u32 ftype;
	u32 resp;
	u32 nid;
	int ret;

	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_IDENTIFY, 0,
				   ADF_61MX_MBX_RESP_IDENTIFY, &resp);
	if (ret)
		return ret;

	ftype = FIELD_GET(ADF_61MX_MBX_PF_IDENTIFY_RESP_FTYPE, resp);
	nid = FIELD_GET(ADF_61MX_MBX_PF_IDENTIFY_RESP_NODEID, resp);
	if (ftype != ADF_61MX_MBX_FTYPE_PF || nid >= ADF_61MX_MAX_PF) {
		dev_err(to_dev(hwdev), "Invalid CPF identity, ftype %u node %u\n",
			ftype, nid);
		return -EINVAL;
	}

	return 0;
}

int adf_61mx_mbx_num_vf(struct adf_61mx_dev *hwdev)
{
	u32 resp;
	int ret;

	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_SRIOV_VF, 0,
				   ADF_61MX_MBX_RESP_SRIOV_VF, &resp);

	return ret ? ret : resp;
}

int adf_61mx_mbx_alloc_qs(struct adf_61mx_dev *hwdev)
{
	u32 resp;
	int ret;

	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_QS_ALLOC, 0,
				   ADF_61MX_MBX_RESP_QS_ALLOC, &resp);

	return ret ? ret : resp;
}

/* The CPF writes to CPF2APF only while its interrupt is unmasked */
void adf_61mx_mbx_enable(struct adf_61mx_dev *hwdev)
{
	ADF_CSR_WR(hwdev->iobase, ADF_61MX_PF_INT_MASK, ~ADF_61MX_PF_INT_CPF2APF);
}

void adf_61mx_mbx_disable(struct adf_61mx_dev *hwdev)
{
	ADF_CSR_WR(hwdev->iobase, ADF_61MX_PF_INT_MASK, ~0U);
}

