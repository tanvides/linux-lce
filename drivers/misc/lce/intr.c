// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irqreturn.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "lce_mbx.h"
#include "lce_private.h"

#define to_dev(lcehw)	((lcehw)->pdev->dev)

/* PF mailbox registers */
#define LCE_PF_INT_MASK		0x01100004U
#define LCE_MBX_APF2CPF		0x01100008U
#define LCE_MBX_CPF2APF		0x0110000CU
#define LCE_MBX_MSGDATA0	0x01100010U
#define LCE_MBX_MSGDATA1	0x01100014U

#define LCE_MAX_PF		4
/* How long to wait for far side to acknowledge receipt */
#define LCE_MBX_ACK_DELAY_US	10
#define LCE_MBX_ACK_MAX_TIMEOUT	(10 * USEC_PER_SEC)

/* How long to wait for a response from the other side */
#define LCE_MBX_RESP_TIMEOUT_MS	100

#define LCE_MBX_COMPAT_VER_MSG_DATA	5

static void lce_pf_mbx_int_unmask(const struct lce_hw_device *lcehw)
{
	LCE_CSR_WR(lcehw->iobase, LCE_PF_INT_MASK, ~2U);
}

static void lce_pf_mbx_int_mask(const struct lce_hw_device *lcehw)
{
	LCE_CSR_WR(lcehw->iobase, LCE_PF_INT_MASK, ~0U);
}

/* handle interrupt reasons and msgs */
static irqreturn_t lce_pf_mbx_irq(s32 irq, void *arg)
{
	struct lce_hw_device *lcehw = arg;
	u32 val32;

	val32 = LCE_CSR_RD(lcehw->iobase, LCE_MBX_CPF2APF);
	if (!(val32 & LCE_MBX_MSGINT) || !(val32 & LCE_MBX_MSGV)) {
		dev_err_ratelimited(&to_dev(lcehw),
				    "Spurious interrupt, msg %X. Ignored",
				    val32);
		return IRQ_HANDLED;
	}

	/* Mask interrupt for sequential execution */
	lce_pf_mbx_int_mask(lcehw);

	switch (FIELD_GET(LCE_MBX_MSGTYPE, val32)) {
	case LCE_MBX_OP_PF_NOTIFY_RESTARTING:
	case LCE_MBX_OP_PF_NOTIFY_FATAL_ERR:
		break;
	case LCE_MBX_OP_PF_VERSION_RESP:
	case LCE_MBX_OP_PF_QS_ALLOC_RESP:
	case LCE_MBX_OP_PF_SRIOV_VF_RESP:
	case LCE_MBX_OP_PF_IDENTIFY_RESP:
		lcehw->mbx_resp = FIELD_GET(LCE_MBX_MSGDATA, val32);
		complete(&lcehw->msg_completion);
		break;
	default:
		dev_err_ratelimited(&to_dev(lcehw), "Unknown mbx msg 0x%x",
				    val32);
		break;
	}

	/* To ack, clear the INT bit */
	val32 &= ~LCE_MBX_MSGINT;
	LCE_CSR_WR(lcehw->iobase, LCE_MBX_CPF2APF, val32);

	lce_pf_mbx_int_unmask(lcehw);

	return IRQ_HANDLED;
}

static int __lce_mbx_putmsg(const struct lce_hw_device *lcehw,
			    u32 msg_type, u32 msg_data)
{
	void __iomem *regbase = lcehw->iobase;
	u32 val32;

	val32 = FIELD_PREP(LCE_MBX_MSGTYPE, msg_type) |
		FIELD_PREP(LCE_MBX_MSGDATA, msg_data) |
		LCE_MBX_MSGV | LCE_MBX_MSGINT;
	LCE_CSR_WR(regbase, LCE_MBX_APF2CPF, val32);

	/* Wait for confirmation from remote that it received the message */
	read_poll_timeout(LCE_CSR_RD,
			  val32, !(val32 & LCE_MBX_MSGINT),
			  LCE_MBX_ACK_DELAY_US,
			  LCE_MBX_ACK_MAX_TIMEOUT, 0,
			  regbase, LCE_MBX_APF2CPF);

	if (val32 & LCE_MBX_MSGINT) {
		dev_err_ratelimited(&to_dev(lcehw), "mbx ACK not received");
		val32 &= ~LCE_MBX_MSGINT;
		LCE_CSR_WR(regbase, LCE_MBX_APF2CPF, val32);
		return -EIO;
	}

	return 0;
}

static int lce_mbx_xchange(struct lce_hw_device *lcehw, u32 msg_type, u32 msg_data)
{
	unsigned long timeout = msecs_to_jiffies(LCE_MBX_RESP_TIMEOUT_MS);
	int err;

	reinit_completion(&lcehw->msg_completion);

	err = __lce_mbx_putmsg(lcehw, msg_type, msg_data);
	if (err)
		return err;

	/* wait for immediate response */
	err = wait_for_completion_timeout(&lcehw->msg_completion, timeout);
	if (!err) {
		dev_err(&to_dev(lcehw), "mbx response timeout");
		return -EIO;
	}

	return 0;
}

int lce_mbx_request_version(struct lce_hw_device *lcehw)
{
	int result;
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_COMPAT_VERSION,
			      LCE_MBX_COMPAT_VER_MSG_DATA);
	if (ret) {
		dev_err(&to_dev(lcehw), "mbx OP_VERSION failed\n");
		goto out_mutex_unlock;
	}

	result = FIELD_GET(LCE_MBX_PF_COMPAT_RESP_RESULT, lcehw->mbx_resp);
	if (result != LCE_MBX_VERSION_COMPATIBLE) {
		dev_err(&to_dev(lcehw), "mbx incompatible version %d\n", result);
		ret = -EINVAL;
	}

out_mutex_unlock:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

int lce_mbx_fetch_id(struct lce_hw_device *lcehw)
{
	int ftype;
	int nid;
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_IDENTIFY, 0);
	if (ret) {
		dev_err(&to_dev(lcehw), "mbx identify failed\n");
		goto out_mutex_unlock;
	}

	ftype = FIELD_GET(LCE_MBX_PF_IDENTIFY_RESP_FTYPE, lcehw->mbx_resp);
	nid = FIELD_GET(LCE_MBX_PF_IDENTIFY_RESP_NODEID, lcehw->mbx_resp);
	if (ftype != LCE_MBX_FTYPE_PF || nid >= LCE_MAX_PF) {
		dev_err(&to_dev(lcehw), "mbx identify invalid ftype/nid\n");
		ret = -EINVAL;
	}

out_mutex_unlock:
	mutex_unlock(&lcehw->mbx_lock);

	return ret;
}

int lce_mbx_num_vf(struct lce_hw_device *lcehw)
{
	int ret;

	mutex_lock(&lcehw->mbx_lock);
	ret = lce_mbx_xchange(lcehw, LCE_MBX_OP_PF_SRIOV_VF, 0);
	if (ret) {
		dev_err(&to_dev(lcehw), "mbx OP_SRIOV_VF failed\n");
		ret = 0;
		goto out_mutex_unlock;
	}

	ret = lcehw->mbx_resp;

out_mutex_unlock:
	mutex_unlock(&lcehw->mbx_lock);
	return ret;
}

int lce_intr_init(struct lce_hw_device *lcehw)
{
	struct pci_dev *pdev = lcehw->pdev;
	struct device *dev = &pdev->dev;
	struct lce_msix *msix;
	int nvec;
	int ret;

	init_completion(&lcehw->msg_completion);

	/* enable the msix vectors for LCE */
	nvec = pci_msix_vec_count(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, nvec, PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(dev, "Failed to enable MSI-X IRQ");
		ret = -EFAULT;
		goto out_free_msix;
	}

	msix = kcalloc(ret, sizeof(*msix), GFP_KERNEL);
	if (!msix)
		return -ENOMEM;

	lcehw->msix_entry = msix;
	lcehw->max_vecs = ret;
	mutex_init(&lcehw->mbx_lock);

	/* Request msix irq */
	snprintf(msix[0].name, sizeof(msix[0].name),
		 "lce-vfio-mbx(%s)", pci_name(pdev));

	ret = request_irq(pci_irq_vector(pdev, 0), lce_pf_mbx_irq, 0,
			  msix[0].name, lcehw);
	if (ret) {
		dev_err(dev, "failed to enable irq %d, for %s\n",
			pci_irq_vector(pdev, 0), msix[0].name);
		goto out_free_vectors;
	}
	lce_pf_mbx_int_unmask(lcehw);

	return 0;

out_free_vectors:
	mutex_destroy(&lcehw->mbx_lock);
	pci_free_irq_vectors(lcehw->pdev);
out_free_msix:
	kfree(lcehw->msix_entry);
	return ret;
}

void lce_intr_deinit(struct lce_hw_device *lcehw)
{
	struct lce_msix *msix = lcehw->msix_entry;

	lce_pf_mbx_int_mask(lcehw);
	free_irq(pci_irq_vector(lcehw->pdev, 0), lcehw);
	mutex_destroy(&lcehw->mbx_lock);
	pci_free_irq_vectors(lcehw->pdev);
	kfree(msix);
}
