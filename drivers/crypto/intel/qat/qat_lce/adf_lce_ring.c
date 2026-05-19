// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024, Intel Corporation */

/*
 * LCE GEN5 ring pair management.
 *
 * Programs GEN5 ring CSRs in BAR0 and provides helpers to enqueue
 * requests and poll responses.  The CPF pre-assigns ring pairs to
 * each APF (count negotiated via QS_ALLOC mailbox during probe)
 * and pre-programs service types: odd banks = DC (compression),
 * even banks = CY (crypto).
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/pci.h>

#include "adf_lce_hw_data.h"
#include "adf_lce_ring.h"

static inline void __iomem *ring_csr(struct lce_hw_device *lcehw,
				     u32 bank, u32 offset)
{
	return lcehw->iobase + LCE_RING_BUNDLE_SIZE * bank + offset;
}

static inline void ring_wr(struct lce_hw_device *lcehw, u32 bank,
			   u32 offset, u32 val)
{
	writel(val, ring_csr(lcehw, bank, offset));
}

static inline u32 ring_rd(struct lce_hw_device *lcehw, u32 bank, u32 offset)
{
	return readl(ring_csr(lcehw, bank, offset));
}

static void ring_write_base(struct lce_hw_device *lcehw, u32 bank,
			    u32 ring, dma_addr_t addr)
{
	u32 base = LCE_RING_CSR_LBASE + (ring << 2);

	ring_wr(lcehw, bank, base, lower_32_bits(addr));
	ring_wr(lcehw, bank, base + (LCE_RING_CSR_UBASE - LCE_RING_CSR_LBASE),
		upper_32_bits(addr));
}

static u32 build_ring_config(u32 size, u32 wm_nf, u32 wm_ne)
{
	return (wm_nf << LCE_RING_NF_WM_SHIFT) |
	       (wm_ne << LCE_RING_NE_WM_SHIFT) | size;
}

/**
 * lce_ring_pair_init() - Program one TX/RX ring pair.
 * @lcehw:   LCE PF private state (has iobase, pdev, mailbox).
 * @rp:      Ring pair structure to fill in.
 * @bank_id: Ring bundle index in the APF's BAR0 space.
 *
 * Allocates coherent DMA buffers for TX and RX rings and
 * programs the GEN5 ring CSRs.  The caller must have already
 * negotiated ring-pair ownership with the CPF via QS_ALLOC.
 *
 * Return: 0 on success, negative errno on failure.
 */
int lce_ring_pair_init(struct lce_hw_device *lcehw, struct lce_ring_pair *rp,
		       u32 bank_id)
{
	struct pci_dev *pdev = lcehw->pdev;
	u32 ring_cfg;

	rp->bank_id = bank_id;

	/* Allocate DMA buffers */
	rp->tx_ring_sz = LCE_TX_RING_NUM_MSGS * LCE_COMP_REQ_SIZE; /* 64K */
	rp->rx_ring_sz = LCE_RX_RING_NUM_MSGS * LCE_COMP_RESP_SIZE; /* 64K */

	rp->tx_base = dmam_alloc_coherent(&pdev->dev, rp->tx_ring_sz,
					  &rp->tx_dma, GFP_KERNEL);
	if (!rp->tx_base)
		return -ENOMEM;

	rp->rx_base = dmam_alloc_coherent(&pdev->dev, rp->rx_ring_sz,
					  &rp->rx_dma, GFP_KERNEL);
	if (!rp->rx_base)
		return -ENOMEM;

	rp->tx_tail = 0;
	rp->rx_head = 0;

	/* Program ring CSRs */

	/* Disable arbitration while configuring */
	ring_wr(lcehw, bank_id, LCE_RING_CSR_SRV_ARB_EN, 0);

	/* TX ring (ring 0): 64K, 128-byte messages, no watermark */
	ring_cfg = build_ring_config(LCE_RING_SIZE_64K, 0, 0);
	ring_wr(lcehw, bank_id, LCE_RING_CSR_CONFIG, ring_cfg);
	ring_write_base(lcehw, bank_id, 0, rp->tx_dma);

	/* RX ring (ring 1): 64K, 32-byte responses, near-empty WM */
	ring_cfg = build_ring_config(LCE_RING_SIZE_64K, LCE_RING_NE_WM_512, 0);
	ring_wr(lcehw, bank_id, LCE_RING_CSR_CONFIG + (1 << 2), ring_cfg);
	ring_write_base(lcehw, bank_id, 1, rp->rx_dma);

	/* Enable service arbitration - bit 0 enables ring 0 (TX) */
	ring_wr(lcehw, bank_id, LCE_RING_CSR_SRV_ARB_EN, 1);

	return 0;
}

/**
 * lce_ring_pair_cleanup() - Tear down a ring pair.
 * @lcehw: LCE PF private state.
 * @rp:    Ring pair to clean up.
 */
void lce_ring_pair_cleanup(struct lce_hw_device *lcehw, struct lce_ring_pair *rp)
{
	u32 bank = rp->bank_id;

	/* Disable arbitration */
	ring_wr(lcehw, bank, LCE_RING_CSR_SRV_ARB_EN, 0);

	/* Clear ring bases */
	ring_write_base(lcehw, bank, 0, 0);
	ring_wr(lcehw, bank, LCE_RING_CSR_CONFIG, 0);

	ring_write_base(lcehw, bank, 1, 0);
	ring_wr(lcehw, bank, LCE_RING_CSR_CONFIG + (1 << 2), 0);

	/* DMA buffers freed by devm on device removal */
}

/**
 * lce_ring_put_msg() - Enqueue a message on the TX ring.
 * @lcehw:  LCE PF private state.
 * @rp:     Ring pair.
 * @msg:    Pointer to the message (LCE_COMP_REQ_SIZE bytes).
 * @msg_sz: Size of the message.
 *
 * Return: 0 on success, -EBUSY if ring is full.
 */
int lce_ring_put_msg(struct lce_hw_device *lcehw, struct lce_ring_pair *rp,
		     void *msg, u32 msg_sz)
{
	u32 tail = rp->tx_tail;
	u32 next_tail;
	u32 head;

	next_tail = (tail + msg_sz) % rp->tx_ring_sz;

	/* Check if ring is full (read HW head to see what's been consumed) */
	head = ring_rd(lcehw, rp->bank_id, LCE_RING_CSR_HEAD);
	if (next_tail == head)
		return -EBUSY;

	memcpy((u8 *)rp->tx_base + tail, msg, msg_sz);

	/* Ensure descriptor is written before doorbell */
	wmb();

	rp->tx_tail = next_tail;

	/* Ring doorbell: write tail pointer to CSR */
	ring_wr(lcehw, rp->bank_id, LCE_RING_CSR_TAIL, next_tail);

	return 0;
}

/**
 * lce_ring_poll_resp() - Poll the RX ring for a response.
 * @lcehw: LCE PF private state.
 * @rp:    Ring pair.
 *
 * Return: Pointer to response message, or NULL if none available.
 */
void *lce_ring_poll_resp(struct lce_hw_device *lcehw, struct lce_ring_pair *rp)
{
	void *resp = (u8 *)rp->rx_base + rp->rx_head;

	/* GEN5 response: byte 3 (hdr_flags) bit 7 = valid flag */
	if (!(*(u8 *)(resp + 3) & LCE_RING_RESP_VALID))
		return NULL;

	/* Ensure we read the response after the valid flag */
	rmb();

	return resp;
}

/**
 * lce_ring_advance_rx() - Advance the RX ring head after consuming a response.
 * @lcehw: LCE PF private state.
 * @rp:    Ring pair.
 */
void lce_ring_advance_rx(struct lce_hw_device *lcehw, struct lce_ring_pair *rp)
{
	void *resp = (u8 *)rp->rx_base + rp->rx_head;

	/* Clear the valid flag */
	*(u8 *)(resp + 3) &= ~LCE_RING_RESP_VALID;

	rp->rx_head = (rp->rx_head + LCE_COMP_RESP_SIZE) % rp->rx_ring_sz;

	/* Update HW head CSR */
	ring_wr(lcehw, rp->bank_id, LCE_RING_CSR_HEAD + (1 << 2), rp->rx_head);
}

/* ---- Kthread-based response polling ---- */

static int lce_ring_poll_thread(void *data)
{
	struct lce_ring_pair *rp = data;
	struct lce_hw_device *lcehw = rp->resp_cb_data;
	void *resp;

	while (!kthread_should_stop()) {
		resp = lce_ring_poll_resp(lcehw, rp);
		if (resp) {
			do {
				if (rp->resp_cb)
					rp->resp_cb(resp, lcehw);
				lce_ring_advance_rx(lcehw, rp);
				resp = lce_ring_poll_resp(lcehw, rp);
			} while (resp);
		} else {
			usleep_range(50, 100);
		}
	}

	return 0;
}

int lce_ring_poll_start(struct lce_ring_pair *rp,
			void (*cb)(void *resp, void *data), void *data)
{
	rp->resp_cb = cb;
	rp->resp_cb_data = data;

	rp->poll_thread = kthread_run(lce_ring_poll_thread, rp,
				      "lce_poll/%u", rp->bank_id);
	if (IS_ERR(rp->poll_thread)) {
		int ret = PTR_ERR(rp->poll_thread);

		rp->poll_thread = NULL;
		return ret;
	}

	return 0;
}

void lce_ring_poll_stop(struct lce_ring_pair *rp)
{
	if (rp->poll_thread) {
		kthread_stop(rp->poll_thread);
		rp->poll_thread = NULL;
	}
}
