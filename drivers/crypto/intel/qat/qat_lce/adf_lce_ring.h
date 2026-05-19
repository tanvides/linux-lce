/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef ADF_LCE_RING_H_
#define ADF_LCE_RING_H_

#include <linux/bits.h>
#include <linux/kthread.h>
#include <linux/types.h>

struct lce_hw_device;

/*
 * LCE GEN5 ring pair.
 *
 * Each bundle has two rings: ring 0 (TX/request) and ring 1 (RX/response).
 * The APF allocates a bundle from the CPF via mailbox, then programs the
 * ring CSRs directly in BAR0.
 *
 * GEN5 ring CSR base = BAR0 + ADF_RING_BUNDLE_SIZE * bank_id.
 * ADF_RING_CSR_ADDR_OFFSET is 0 on GEN5 (vs 0x100000 on GEN4).
 */

/* Ring CSR offsets within a bundle (BAR0 + bundle_size * bank_id + offset) */
#define LCE_RING_CSR_CONFIG		0x1000
#define LCE_RING_CSR_LBASE		0x1040
#define LCE_RING_CSR_UBASE		0x1080
#define LCE_RING_CSR_HEAD		0x0C0
#define LCE_RING_CSR_TAIL		0x100
#define LCE_RING_CSR_SRV_ARB_EN		0x19C
#define LCE_RING_BUNDLE_SIZE		0x2000

/* Ring sizes - log2-based encoding */
#define LCE_RING_SIZE_64K		0x0A

/* Near watermark config bits */
#define LCE_RING_NF_WM_SHIFT		10	/* near-full watermark */
#define LCE_RING_NE_WM_SHIFT		5	/* near-empty watermark */
#define LCE_RING_NE_WM_512		0x08

/* Number of messages in a 64K request ring with 128-byte messages */
#define LCE_TX_RING_NUM_MSGS		512	/* 65536 / 128 */
/* Number of messages in a 64K response ring with 32-byte messages */
#define LCE_RX_RING_NUM_MSGS		2048	/* 65536 / 32 */

/* GEN5 compression request is 128 bytes (32 LWs) */
#define LCE_COMP_REQ_SIZE		128
/* GEN5 compression response is 32 bytes (8 LWs) */
#define LCE_COMP_RESP_SIZE		32

/* Valid flag in response hdr_flags (byte 3) */
#define LCE_RING_RESP_VALID		BIT(7)

/**
 * struct lce_ring_pair - LCE ring pair (one bundle).
 * @tx_base:   virtual address of TX ring buffer.
 * @tx_dma:    DMA address of TX ring buffer.
 * @rx_base:   virtual address of RX ring buffer.
 * @rx_dma:    DMA address of RX ring buffer.
 * @bank_id:   ring bundle index obtained from CPF.
 * @tx_tail:   software shadow of TX ring tail (producer index).
 * @rx_head:   software shadow of RX ring head (consumer index).
 * @tx_ring_sz: TX ring buffer size in bytes.
 * @rx_ring_sz: RX ring buffer size in bytes.
 * @resp_handler: kthread for polling ring responses.
 * @resp_cb:   callback invoked for each response message.
 * @resp_cb_data: opaque data passed to @resp_cb.
 */
struct lce_ring_pair {
	void *tx_base;
	dma_addr_t tx_dma;
	void *rx_base;
	dma_addr_t rx_dma;
	u32 bank_id;
	u32 tx_tail;
	u32 rx_head;
	u32 tx_ring_sz;
	u32 rx_ring_sz;
	struct task_struct *poll_thread;
	void (*resp_cb)(void *resp, void *data);
	void *resp_cb_data;
};

int lce_ring_pair_init(struct lce_hw_device *lcehw, struct lce_ring_pair *rp,
		       u32 bank_id);
void lce_ring_pair_cleanup(struct lce_hw_device *lcehw, struct lce_ring_pair *rp);
int lce_ring_put_msg(struct lce_hw_device *lcehw, struct lce_ring_pair *rp,
		     void *msg, u32 msg_sz);
void *lce_ring_poll_resp(struct lce_hw_device *lcehw, struct lce_ring_pair *rp);
void lce_ring_advance_rx(struct lce_hw_device *lcehw, struct lce_ring_pair *rp);

int lce_ring_poll_start(struct lce_ring_pair *rp,
			void (*cb)(void *resp, void *data), void *data);
void lce_ring_poll_stop(struct lce_ring_pair *rp);

#endif /* ADF_LCE_RING_H_ */
