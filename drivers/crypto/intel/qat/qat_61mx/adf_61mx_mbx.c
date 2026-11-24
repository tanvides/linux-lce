// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>

#include "adf_61mx_hw_data.h"
#include "adf_61mx_mbx.h"

#define to_dev(hwdev)	(&(hwdev)->pdev->dev)

#define ADF_61MX_MAX_PF			4
#define ADF_61MX_MBX_ACK_DELAY_US		10
#define ADF_61MX_MBX_ACK_MAX_TIMEOUT		(10 * USEC_PER_SEC)
#define ADF_61MX_MBX_RESP_POLL_US		100
#define ADF_61MX_MBX_RESP_TIMEOUT_US		(2 * USEC_PER_SEC)
#define ADF_61MX_MBX_COMPAT_VER_MSG_DATA	5

static int __adf_61mx_mbx_putmsg(const struct adf_61mx_dev *hwdev,
				 u32 msg_type, u32 msg_data)
{
	void __iomem *regbase = hwdev->iobase;
	u32 val32;

	val32 = FIELD_PREP(ADF_61MX_MBX_MSGTYPE, msg_type) |
		FIELD_PREP(ADF_61MX_MBX_MSGDATA, msg_data) |
		ADF_61MX_MBX_MSGV | ADF_61MX_MBX_MSGINT;
	ADF_61MX_CSR_WR(regbase, ADF_61MX_MBX_APF2CPF, val32);

	read_poll_timeout(ADF_61MX_CSR_RD, val32, !(val32 & ADF_61MX_MBX_MSGINT),
			  ADF_61MX_MBX_ACK_DELAY_US, ADF_61MX_MBX_ACK_MAX_TIMEOUT, 0,
			  regbase, ADF_61MX_MBX_APF2CPF);

	if (val32 & ADF_61MX_MBX_MSGINT) {
		dev_err_ratelimited(to_dev(hwdev), "mbx ACK not received\n");
		val32 &= ~ADF_61MX_MBX_MSGINT;
		ADF_61MX_CSR_WR(regbase, ADF_61MX_MBX_APF2CPF, val32);
		return -EIO;
	}

	return 0;
}

/*
 * Send a mailbox message and poll for the CPF response. Used only during
 * probe to negotiate with the CPF; no MSI-X vectors are needed because
 * the response is polled from the CPF2APF register.
 */
static int adf_61mx_mbx_xchange(struct adf_61mx_dev *hwdev, u32 msg_type,
				u32 msg_data)
{
	void __iomem *regbase = hwdev->iobase;
	u32 val32;
	int err;

	err = __adf_61mx_mbx_putmsg(hwdev, msg_type, msg_data);
	if (err)
		return err;

	err = read_poll_timeout(ADF_61MX_CSR_RD, val32,
				(val32 & ADF_61MX_MBX_MSGV),
				ADF_61MX_MBX_RESP_POLL_US,
				ADF_61MX_MBX_RESP_TIMEOUT_US, 0,
				regbase, ADF_61MX_MBX_CPF2APF);
	if (err) {
		dev_err(to_dev(hwdev), "mbx response timeout (CPF2APF=0x%08x)\n",
			val32);
		return -EIO;
	}

	hwdev->mbx_resp = FIELD_GET(ADF_61MX_MBX_MSGDATA, val32);

	val32 &= ~ADF_61MX_MBX_MSGINT;
	ADF_61MX_CSR_WR(regbase, ADF_61MX_MBX_CPF2APF, val32);

	return 0;
}

int adf_61mx_mbx_request_version(struct adf_61mx_dev *hwdev)
{
	int result;
	int ret;

	mutex_lock(&hwdev->mbx_lock);
	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_COMPAT_VERSION,
				   ADF_61MX_MBX_COMPAT_VER_MSG_DATA);
	if (ret) {
		dev_err(to_dev(hwdev), "mbx OP_VERSION failed\n");
		goto out;
	}

	result = FIELD_GET(ADF_61MX_MBX_PF_COMPAT_RESP_RESULT, hwdev->mbx_resp);
	if (result != ADF_61MX_MBX_VERSION_COMPATIBLE) {
		dev_err(to_dev(hwdev), "mbx incompatible version %d\n", result);
		ret = -EINVAL;
	}

out:
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

int adf_61mx_mbx_fetch_id(struct adf_61mx_dev *hwdev)
{
	int ftype;
	int nid;
	int ret;

	mutex_lock(&hwdev->mbx_lock);
	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_IDENTIFY, 0);
	if (ret) {
		dev_err(to_dev(hwdev), "mbx identify failed\n");
		goto out;
	}

	ftype = FIELD_GET(ADF_61MX_MBX_PF_IDENTIFY_RESP_FTYPE, hwdev->mbx_resp);
	nid = FIELD_GET(ADF_61MX_MBX_PF_IDENTIFY_RESP_NODEID, hwdev->mbx_resp);
	if (ftype != ADF_61MX_MBX_FTYPE_PF || nid >= ADF_61MX_MAX_PF) {
		dev_err(to_dev(hwdev), "mbx identify invalid ftype/nid\n");
		ret = -EINVAL;
	}

out:
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

int adf_61mx_mbx_num_vf(struct adf_61mx_dev *hwdev)
{
	int ret;

	mutex_lock(&hwdev->mbx_lock);
	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_SRIOV_VF, 0);
	if (ret) {
		dev_err(to_dev(hwdev), "mbx OP_SRIOV_VF failed\n");
		ret = 0;
		goto out;
	}

	ret = hwdev->mbx_resp;

out:
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

int adf_61mx_mbx_alloc_qs(struct adf_61mx_dev *hwdev)
{
	int ret;

	mutex_lock(&hwdev->mbx_lock);
	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_QS_ALLOC, 0);
	if (ret) {
		dev_err(to_dev(hwdev), "mbx QS_ALLOC failed\n");
		ret = -EIO;
		goto out;
	}

	ret = hwdev->mbx_resp;

out:
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

/*
 * Initialise the mailbox for polled CPF communication used during
 * probe. Mailbox negotiation runs before the device is brought up, so
 * no MSI-X vectors are allocated yet.
 */
void adf_61mx_mbx_init(struct adf_61mx_dev *hwdev)
{
	u32 val;

	mutex_init(&hwdev->mbx_lock);

	/* Unmask the mailbox interrupt register so the CPF will write
	 * responses. This does not enable MSI-X delivery (no vectors
	 * are allocated yet), but the CPF checks this mask before
	 * writing to CPF2APF.
	 */
	ADF_61MX_CSR_WR(hwdev->iobase, ADF_61MX_PF_INT_MASK, ~2U);
	val = ADF_61MX_CSR_RD(hwdev->iobase, ADF_61MX_PF_INT_MASK);
	dev_dbg(&hwdev->pdev->dev, "PF_INT_MASK after unmask: 0x%08x\n", val);
}

void adf_61mx_mbx_cleanup(struct adf_61mx_dev *hwdev)
{
	ADF_61MX_CSR_WR(hwdev->iobase, ADF_61MX_PF_INT_MASK, ~0U);
	mutex_destroy(&hwdev->mbx_lock);
}

int adf_61mx_mbx_free_qs(struct adf_61mx_dev *hwdev)
{
	int ret;

	mutex_lock(&hwdev->mbx_lock);
	ret = adf_61mx_mbx_xchange(hwdev, ADF_61MX_MBX_OP_PF_QS_FREE, 0);
	if (ret)
		dev_err(to_dev(hwdev), "mbx QS_FREE failed\n");
	mutex_unlock(&hwdev->mbx_lock);
	return ret;
}

irqreturn_t adf_61mx_mbx_isr(int irq, void *dev_ptr)
{
	struct adf_accel_dev *accel_dev = dev_ptr;
	struct adf_61mx_dev *hwdev = pci_get_drvdata(accel_to_pci_dev(accel_dev));

	/* Ack the CPF->APF mailbox event by clearing the register. The
	 * mailbox reply path itself is polled; this handler only quiesces
	 * the control vector so the line does not stay asserted.
	 */
	ADF_61MX_CSR_RD(hwdev->iobase, ADF_61MX_MBX_CPF2APF);
	ADF_61MX_CSR_WR(hwdev->iobase, ADF_61MX_MBX_CPF2APF, 0);

	return IRQ_HANDLED;
}
