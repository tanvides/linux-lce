// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/iopoll.h>
#include <linux/mutex.h>

#include "adf_lce_hw_data.h"
#include "adf_lce_mbx.h"

#define to_dev(lcehw)	(&(lcehw)->pdev->dev)

/* PF mailbox registers */
#define LCE_PF_INT_MASK		0x01100004U
#define LCE_MBX_APF2CPF		0x01100008U
#define LCE_MBX_CPF2APF		0x0110000CU

#define LCE_MAX_PF			4
#define LCE_MBX_ACK_DELAY_US		10
#define LCE_MBX_ACK_MAX_TIMEOUT		(10 * USEC_PER_SEC)
#define LCE_MBX_RESP_POLL_US		100
#define LCE_MBX_RESP_TIMEOUT_US		(2 * USEC_PER_SEC)
#define LCE_MBX_COMPAT_VER_MSG_DATA	5

static int __lce_mbx_putmsg(const struct lce_hw_device *lcehw,
			    u32 msg_type, u32 msg_data)
{
	void __iomem *regbase = lcehw->iobase;
	u32 val32;

	val32 = FIELD_PREP(LCE_MBX_MSGTYPE, msg_type) |
		FIELD_PREP(LCE_MBX_MSGDATA, msg_data) |
		LCE_MBX_MSGV | LCE_MBX_MSGINT;
	LCE_CSR_WR(regbase, LCE_MBX_APF2CPF, val32);

	read_poll_timeout(LCE_CSR_RD, val32, !(val32 & LCE_MBX_MSGINT),
			  LCE_MBX_ACK_DELAY_US, LCE_MBX_ACK_MAX_TIMEOUT, 0,
			  regbase, LCE_MBX_APF2CPF);

	if (val32 & LCE_MBX_MSGINT) {
		dev_err_ratelimited(to_dev(lcehw), "mbx ACK not received\n");
		val32 &= ~LCE_MBX_MSGINT;
		LCE_CSR_WR(regbase, LCE_MBX_APF2CPF, val32);
		return -EIO;
	}

	return 0;
}

/*
 * Send a mailbox message and poll for the CPF response. Used only during
 * probe to negotiate with the CPF; no MSI-X vectors are needed because
 * the response is polled from the CPF2APF register.
 */
static int lce_mbx_xchange(struct lce_hw_device *lcehw, u32 msg_type,
			   u32 msg_data)
{
	void __iomem *regbase = lcehw->iobase;
	u32 val32;
	int err;

	err = __lce_mbx_putmsg(lcehw, msg_type, msg_data);
	if (err)
		return err;

	err = read_poll_timeout(LCE_CSR_RD, val32,
				(val32 & LCE_MBX_MSGV),
				LCE_MBX_RESP_POLL_US,
				LCE_MBX_RESP_TIMEOUT_US, 0,
				regbase, LCE_MBX_CPF2APF);
	if (err) {
		dev_err(to_dev(lcehw), "mbx response timeout (CPF2APF=0x%08x)\n",
			val32);
		return -EIO;
	}

	lcehw->mbx_resp = FIELD_GET(LCE_MBX_MSGDATA, val32);

	val32 &= ~LCE_MBX_MSGINT;
	LCE_CSR_WR(regbase, LCE_MBX_CPF2APF, val32);

	return 0;
}

int adf_lce_mbx_request_version(struct lce_hw_device *lcehw)
{
	int result;
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_COMPAT_VERSION,
			      LCE_MBX_COMPAT_VER_MSG_DATA);
	if (ret) {
		dev_err(to_dev(lcehw), "mbx OP_VERSION failed\n");
		goto out;
	}

	result = FIELD_GET(LCE_MBX_PF_COMPAT_RESP_RESULT, lcehw->mbx_resp);
	if (result != LCE_MBX_VERSION_COMPATIBLE) {
		dev_err(to_dev(lcehw), "mbx incompatible version %d\n", result);
		ret = -EINVAL;
	}

out:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

int adf_lce_mbx_fetch_id(struct lce_hw_device *lcehw)
{
	int ftype;
	int nid;
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_IDENTIFY, 0);
	if (ret) {
		dev_err(to_dev(lcehw), "mbx identify failed\n");
		goto out;
	}

	ftype = FIELD_GET(LCE_MBX_PF_IDENTIFY_RESP_FTYPE, lcehw->mbx_resp);
	nid = FIELD_GET(LCE_MBX_PF_IDENTIFY_RESP_NODEID, lcehw->mbx_resp);
	if (ftype != LCE_MBX_FTYPE_PF || nid >= LCE_MAX_PF) {
		dev_err(to_dev(lcehw), "mbx identify invalid ftype/nid\n");
		ret = -EINVAL;
	}

out:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

int adf_lce_mbx_num_vf(struct lce_hw_device *lcehw)
{
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_SRIOV_VF, 0);
	if (ret) {
		dev_err(to_dev(lcehw), "mbx OP_SRIOV_VF failed\n");
		ret = 0;
		goto out;
	}

	ret = lcehw->mbx_resp;

out:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

int adf_lce_mbx_alloc_qs(struct lce_hw_device *lcehw)
{
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_QS_ALLOC, 0);
	if (ret) {
		dev_err(to_dev(lcehw), "mbx QS_ALLOC failed\n");
		ret = -EIO;
		goto out;
	}

	ret = lcehw->mbx_resp;

out:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

/*
 * Initialise mailbox for polled CPF communication. No MSI-X vectors
 * are allocated here; mailbox negotiation is done synchronously via
 * register polling during probe. Ring-bundle interrupts and the misc
 * AE vector are allocated later by the QAT framework's
 * adf_isr_resource_alloc() during adf_dev_up().
 */
void adf_lce_mbx_init(struct lce_hw_device *lcehw)
{
	u32 val;

	mutex_init(&lcehw->mbx_lock);

	/* Unmask the mailbox interrupt register so the CPF will write
	 * responses. This does not enable MSI-X delivery (no vectors
	 * are allocated yet), but the CPF checks this mask before
	 * writing to CPF2APF.
	 */
	LCE_CSR_WR(lcehw->iobase, LCE_PF_INT_MASK, ~2U);
	val = LCE_CSR_RD(lcehw->iobase, LCE_PF_INT_MASK);
	dev_dbg(&lcehw->pdev->dev, "PF_INT_MASK after unmask: 0x%08x\n", val);
}

void adf_lce_mbx_cleanup(struct lce_hw_device *lcehw)
{
	LCE_CSR_WR(lcehw->iobase, LCE_PF_INT_MASK, ~0U);
	mutex_destroy(&lcehw->mbx_lock);
}

int adf_lce_mbx_free_qs(struct lce_hw_device *lcehw)
{
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_QS_FREE, 0);
	if (ret)
		dev_err(to_dev(lcehw), "mbx QS_FREE failed\n");
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}
