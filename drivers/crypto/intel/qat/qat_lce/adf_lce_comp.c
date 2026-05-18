// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation */

/*
 * LCE GEN5 compression algorithm registration.
 *
 * Registers "deflate" and "zstd" acomp algorithms backed by GEN5 LCE hardware.
 * Builds 128-byte GEN5 descriptors (struct lce_comp_req) and submits them on a
 * TX ring. A dedicated kthread polls the RX ring for completions.
 */

#include <linux/crypto.h>
#include <linux/bitfield.h>
#include <crypto/acompress.h>
#include <crypto/internal/acompress.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <icp_qat_fw.h>
#include <icp_qat_fw_comp.h>

#include "adf_lce_hw_data.h"
#include "adf_lce_ring.h"
#include "adf_lce_comp.h"

/* Module-level state: protected by algs_lock */
static DEFINE_MUTEX(algs_lock);
static unsigned int active_devs;
static struct lce_hw_device *lce_comp_dev;
static struct lce_ring_pair lce_comp_rp;

/*
 * GEN5 LCE SGL buffer list format (DMA-coherent, 64-byte aligned).
 * Matches the standard QAT SGL layout used in request descriptors (ptr_type=SGL).
 */
struct lce_buf_entry {
	__le32 len;
	__le32 resrvd;
	__le64 addr;
} __packed;

struct lce_buf_list {
	__le32 ad_lo;		/* AD metadata low (PASID, FuncID) */
	__le32 ad_hi;		/* AD metadata high (NodeID, FuncType, ReqID) */
	__le32 num_bufs;
	__le32 num_mapped_bufs;
	struct lce_buf_entry buffers[];
} __packed;

/* Tracks DMA state for a mapped SGL buffer list */
struct lce_sgl_buf {
	struct lce_buf_list *bl;	/* buffer list (DMA-coherent) */
	dma_addr_t blp;			/* DMA address of buffer list */
	size_t bl_size;			/* allocation size of buffer list */
	int nents;			/* number of SGL entries mapped */
	enum dma_data_direction dir;
};

/* Per-request async context (stored in acomp_request_ctx) */
struct lce_async_req {
	struct lce_sgl_buf src_sb;
	struct lce_sgl_buf dst_sb;
	void *crc_buf;
	dma_addr_t crc_buf_dma;
};

static int lce_map_sgl(struct device *dev, struct scatterlist *sgl,
		       unsigned int len, enum dma_data_direction dir,
		       struct lce_sgl_buf *sb)
{
	struct scatterlist *sg;
	int n = sg_nents(sgl);
	size_t bl_size;
	int i;

	sb->dir = dir;
	sb->nents = 0;
	bl_size = struct_size(sb->bl, buffers, n);

	sb->bl = dma_alloc_coherent(dev, bl_size, &sb->blp, GFP_KERNEL);
	if (!sb->bl)
		return -ENOMEM;

	sb->bl_size = bl_size;
	sb->bl->ad_lo = LCE_AD_META_LO;
	sb->bl->ad_hi = LCE_AD_META_HI;
	sb->bl->num_bufs = cpu_to_le32(n);
	sb->bl->num_mapped_bufs = cpu_to_le32(n);

	for_each_sg(sgl, sg, n, i) {
		sb->bl->buffers[i].len = cpu_to_le32(sg->length);
		sb->bl->buffers[i].resrvd = 0;
		sb->bl->buffers[i].addr = cpu_to_le64(
			dma_map_single(dev, sg_virt(sg), sg->length, dir));
		if (dma_mapping_error(dev,
				      le64_to_cpu(sb->bl->buffers[i].addr)))
			goto unwind;
		sb->nents++;
	}

	return 0;

unwind:
	for (i = 0; i < sb->nents; i++)
		dma_unmap_single(dev, le64_to_cpu(sb->bl->buffers[i].addr),
				 le32_to_cpu(sb->bl->buffers[i].len), dir);
	dma_free_coherent(dev, sb->bl_size, sb->bl, sb->blp);
	sb->bl = NULL;
	return -ENOMEM;
}

static void lce_unmap_sgl(struct device *dev, struct lce_sgl_buf *sb)
{
	int i;

	for (i = 0; i < sb->nents; i++)
		dma_unmap_single(dev, le64_to_cpu(sb->bl->buffers[i].addr),
				 le32_to_cpu(sb->bl->buffers[i].len), sb->dir);
	dma_free_coherent(dev, sb->bl_size, sb->bl, sb->blp);
}

struct lce_comp_ctx {
	struct lce_hw_device *lcehw;
	struct lce_ring_pair *rp;
};

/*
 * Response callback - invoked from poll thread for each completed response.
 * Parses status, unmaps DMA, completes the acomp request.
 */
static void lce_comp_resp_cb(void *resp_msg, void *data)
{
	struct icp_qat_fw_comp_resp *resp = resp_msg;
	struct lce_hw_device *lcehw = data;
	struct device *dev = &lcehw->pdev->dev;
	struct acomp_req *areq;
	struct lce_async_req *lreq;
	int ret = 0;

	areq = (struct acomp_req *)(uintptr_t)resp->opaque_data;
	lreq = acomp_request_ctx(areq);

	/* Check response status */
	if (ICP_QAT_FW_COMN_RESP_CMP_STAT_GET(resp->comn_resp.comn_status) ||
	    resp->comn_resp.comn_error.cmp_err_code) {
		ret = -EIO;
	} else {
		areq->dlen = resp->comp_resp_pars.output_byte_counter;
	}

	/* Unmap DMA and free resources */
	lce_unmap_sgl(dev, &lreq->dst_sb);
	lce_unmap_sgl(dev, &lreq->src_sb);
	dma_free_coherent(dev, LCE_CRC_BUF_SIZE, lreq->crc_buf, lreq->crc_buf_dma);

	acomp_request_complete(areq, ret);
}

/*
 * Build a GEN5 compression/decompression request using struct lce_comp_req.
 */
static void lce_build_comp_req(struct lce_comp_req *req, u8 cmd_id,
			       u8 search_depth, u8 hist_buff, u8 deflate_type,
			       dma_addr_t src_dma, u32 src_len,
			       dma_addr_t dst_dma, u32 dst_len,
			       u64 opaque, dma_addr_t crc_buf_dma)
{
	memset(req, 0, sizeof(*req));

	/* LW0-1: Common request header */
	req->comn_hdr.service_cmd_id = cmd_id;
	req->comn_hdr.service_type = ICP_QAT_FW_COMN_REQ_CPM_FW_COMP;
	req->comn_hdr.hdr_flags = LCE_HDR_FLAGS_VALID;
	req->comn_hdr.comn_req_flags =
		ICP_QAT_FW_COMN_FLAGS_BUILD(QAT_COMN_CD_FLD_TYPE_16BYTE_DATA,
					    QAT_COMN_PTR_TYPE_SGL);

	/* LW4: Service-specific cd_pars (search depth, hist buff, deflate type) */
	req->cd_pars = cpu_to_le32(
		FIELD_PREP(LCE_COMP_SEARCH_DEPTH_MASK, search_depth) |
		FIELD_PREP(LCE_COMP_HIST_BUFF_MASK, hist_buff) |
		(deflate_type ? LCE_COMP_DEFLATE_TYPE_BIT : 0));

	/* LW6-13: Common middle section */
	req->comn_mid.opaque_data = opaque;
	req->comn_mid.src_data_addr = src_dma;
	req->comn_mid.dest_data_addr = dst_dma;
	req->comn_mid.src_length = src_len;
	req->comn_mid.dst_length = dst_len;

	/* LW21: CRC control - enable CRC32C generation for compression */
	if (cmd_id == LCE_CMD_DEFLATE_COMP || cmd_id == LCE_CMD_ZSTD_COMP) {
		req->crc_ctrl.crc_algo = LCE_CRC_ALGO_CRC32C;
		req->crc_ctrl.gen_flags = LCE_CRC_GEN_CRCC_CRCU;
	}

	/* LW22-23: CRC buffer address */
	req->crc_addr = cpu_to_le64(crc_buf_dma);
}

static int lce_comp_submit(struct lce_comp_ctx *ctx, u8 cmd_id,
			   u8 search_depth, u8 hist_buff, u8 deflate_type,
			   struct acomp_req *areq,
			   struct lce_async_req *lreq, unsigned int dlen)
{
	struct lce_hw_device *lcehw = ctx->lcehw;
	struct lce_ring_pair *rp = ctx->rp;
	struct lce_comp_req req;
	int ret;

	lce_build_comp_req(&req, cmd_id, search_depth, hist_buff, deflate_type,
			   lreq->src_sb.blp, areq->slen,
			   lreq->dst_sb.blp, dlen,
			   (u64)(uintptr_t)areq, lreq->crc_buf_dma);

	ret = lce_ring_put_msg(lcehw, rp, &req, LCE_COMP_REQ_SIZE);
	if (ret)
		return -EBUSY;

	return -EINPROGRESS;
}

static int lce_acomp_init_tfm(struct crypto_acomp *acomp_tfm)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(tfm);

	mutex_lock(&algs_lock);
	ctx->lcehw = lce_comp_dev;
	ctx->rp = &lce_comp_rp;
	mutex_unlock(&algs_lock);

	if (!ctx->lcehw)
		return -ENODEV;

	return 0;
}

static void lce_acomp_exit_tfm(struct crypto_acomp *acomp_tfm)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->lcehw = NULL;
	ctx->rp = NULL;
}

static int lce_zstd_compress(struct acomp_req *req)
{
	struct crypto_acomp *acomp_tfm = crypto_acomp_reqtfm(req);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(crypto_acomp_tfm(acomp_tfm));
	struct lce_async_req *lreq = acomp_request_ctx(req);
	struct device *dev = &ctx->lcehw->pdev->dev;
	unsigned int dlen;
	int ret;

	if (!req->src || !req->slen)
		return -EINVAL;

	/* ZSTD dest sizing: src + overhead (magic+hdr+checksum+skidpad+tables) */
	dlen = req->dlen ?: (req->slen + LCE_ZSTD_DST_OVERHEAD +
			     ((req->slen >> 17) + 1) * 3);
	if (dlen < LCE_ZSTD_DST_MIN)
		dlen = LCE_ZSTD_DST_MIN;
	if (!req->dst) {
		req->dst = sgl_alloc(dlen, GFP_KERNEL, NULL);
		if (!req->dst)
			return -ENOMEM;
		req->dlen = dlen;
	}

	ret = lce_map_sgl(dev, req->src, req->slen, DMA_TO_DEVICE, &lreq->src_sb);
	if (ret)
		return ret;

	ret = lce_map_sgl(dev, req->dst, dlen, DMA_FROM_DEVICE, &lreq->dst_sb);
	if (ret)
		goto unmap_src;

	lreq->crc_buf = dma_alloc_coherent(dev, LCE_CRC_BUF_SIZE, &lreq->crc_buf_dma, GFP_KERNEL);
	if (!lreq->crc_buf) {
		ret = -ENOMEM;
		goto unmap_dst;
	}

	ret = lce_comp_submit(ctx, LCE_CMD_ZSTD_COMP,
			      LCE_SEARCH_DEPTH_L1, LCE_HIST_BUFF_64K, 0,
			      req, lreq, dlen);
	if (ret == -EINPROGRESS)
		return ret;

	dma_free_coherent(dev, LCE_CRC_BUF_SIZE, lreq->crc_buf, lreq->crc_buf_dma);
unmap_dst:
	lce_unmap_sgl(dev, &lreq->dst_sb);
unmap_src:
	lce_unmap_sgl(dev, &lreq->src_sb);
	return ret;
}

static int lce_zstd_decompress(struct acomp_req *req)
{
	struct crypto_acomp *acomp_tfm = crypto_acomp_reqtfm(req);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(crypto_acomp_tfm(acomp_tfm));
	struct lce_async_req *lreq = acomp_request_ctx(req);
	struct device *dev = &ctx->lcehw->pdev->dev;
	unsigned int dlen;
	int ret;

	if (!req->src || !req->slen)
		return -EINVAL;

	dlen = req->dlen ?: (req->slen * 4);
	if (!req->dst) {
		req->dst = sgl_alloc(dlen, GFP_KERNEL, NULL);
		if (!req->dst)
			return -ENOMEM;
		req->dlen = dlen;
	}

	ret = lce_map_sgl(dev, req->src, req->slen, DMA_TO_DEVICE, &lreq->src_sb);
	if (ret)
		return ret;

	ret = lce_map_sgl(dev, req->dst, dlen, DMA_FROM_DEVICE, &lreq->dst_sb);
	if (ret)
		goto unmap_src;

	lreq->crc_buf = dma_alloc_coherent(dev, LCE_CRC_BUF_SIZE, &lreq->crc_buf_dma, GFP_KERNEL);
	if (!lreq->crc_buf) {
		ret = -ENOMEM;
		goto unmap_dst;
	}

	ret = lce_comp_submit(ctx, LCE_CMD_ZSTD_DECOMP,
			      0, LCE_HIST_BUFF_64K, 0,
			      req, lreq, dlen);
	if (ret == -EINPROGRESS)
		return ret;

	dma_free_coherent(dev, LCE_CRC_BUF_SIZE, lreq->crc_buf, lreq->crc_buf_dma);
unmap_dst:
	lce_unmap_sgl(dev, &lreq->dst_sb);
unmap_src:
	lce_unmap_sgl(dev, &lreq->src_sb);
	return ret;
}

static int lce_deflate_compress(struct acomp_req *req)
{
	struct crypto_acomp *acomp_tfm = crypto_acomp_reqtfm(req);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(crypto_acomp_tfm(acomp_tfm));
	struct lce_async_req *lreq = acomp_request_ctx(req);
	struct device *dev = &ctx->lcehw->pdev->dev;
	unsigned int dlen;
	int ret;

	if (!req->src || !req->slen)
		return -EINVAL;

	/* Deflate dest sizing: ceil(9*src/8) + 1029 (static overhead) */
	dlen = req->dlen ?: (req->slen + req->slen / 8 +
			     LCE_DEFLATE_DST_EXTRA_STATIC);
	if (dlen < LCE_DEFLATE_DST_MIN)
		dlen = LCE_DEFLATE_DST_MIN;
	if (!req->dst) {
		req->dst = sgl_alloc(dlen, GFP_KERNEL, NULL);
		if (!req->dst)
			return -ENOMEM;
		req->dlen = dlen;
	}

	ret = lce_map_sgl(dev, req->src, req->slen, DMA_TO_DEVICE, &lreq->src_sb);
	if (ret)
		return ret;

	ret = lce_map_sgl(dev, req->dst, dlen, DMA_FROM_DEVICE, &lreq->dst_sb);
	if (ret)
		goto unmap_src;

	lreq->crc_buf = dma_alloc_coherent(dev, LCE_CRC_BUF_SIZE, &lreq->crc_buf_dma, GFP_KERNEL);
	if (!lreq->crc_buf) {
		ret = -ENOMEM;
		goto unmap_dst;
	}

	ret = lce_comp_submit(ctx, LCE_CMD_DEFLATE_COMP,
			      LCE_SEARCH_DEPTH_L1, LCE_HIST_BUFF_32K,
			      LCE_DEFLATE_DYNAMIC,
			      req, lreq, dlen);
	if (ret == -EINPROGRESS)
		return ret;

	dma_free_coherent(dev, LCE_CRC_BUF_SIZE, lreq->crc_buf, lreq->crc_buf_dma);
unmap_dst:
	lce_unmap_sgl(dev, &lreq->dst_sb);
unmap_src:
	lce_unmap_sgl(dev, &lreq->src_sb);
	return ret;
}

static int lce_deflate_decompress(struct acomp_req *req)
{
	struct crypto_acomp *acomp_tfm = crypto_acomp_reqtfm(req);
	struct lce_comp_ctx *ctx = crypto_tfm_ctx(crypto_acomp_tfm(acomp_tfm));
	struct lce_async_req *lreq = acomp_request_ctx(req);
	struct device *dev = &ctx->lcehw->pdev->dev;
	unsigned int dlen;
	int ret;

	if (!req->src || !req->slen)
		return -EINVAL;

	dlen = req->dlen ?: (req->slen * 4);
	if (!req->dst) {
		req->dst = sgl_alloc(dlen, GFP_KERNEL, NULL);
		if (!req->dst)
			return -ENOMEM;
		req->dlen = dlen;
	}

	ret = lce_map_sgl(dev, req->src, req->slen, DMA_TO_DEVICE, &lreq->src_sb);
	if (ret)
		return ret;

	ret = lce_map_sgl(dev, req->dst, dlen, DMA_FROM_DEVICE, &lreq->dst_sb);
	if (ret)
		goto unmap_src;

	lreq->crc_buf = dma_alloc_coherent(dev, LCE_CRC_BUF_SIZE, &lreq->crc_buf_dma, GFP_KERNEL);
	if (!lreq->crc_buf) {
		ret = -ENOMEM;
		goto unmap_dst;
	}

	ret = lce_comp_submit(ctx, LCE_CMD_DEFLATE_DECOMP,
			      0, LCE_HIST_BUFF_32K, 0,
			      req, lreq, dlen);
	if (ret == -EINPROGRESS)
		return ret;

	dma_free_coherent(dev, LCE_CRC_BUF_SIZE, lreq->crc_buf, lreq->crc_buf_dma);
unmap_dst:
	lce_unmap_sgl(dev, &lreq->dst_sb);
unmap_src:
	lce_unmap_sgl(dev, &lreq->src_sb);
	return ret;
}

static struct acomp_alg lce_acomp_algs[] = {
{
	.init = lce_acomp_init_tfm,
	.exit = lce_acomp_exit_tfm,
	.compress = lce_deflate_compress,
	.decompress = lce_deflate_decompress,
	.dst_free = sgl_free,
	.reqsize = sizeof(struct lce_async_req),
	.base = {
		.cra_name = "deflate",
		.cra_driver_name = "qat_lce_deflate",
		.cra_priority = 4001,
		.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_ALLOCATES_MEMORY,
		.cra_ctxsize = sizeof(struct lce_comp_ctx),
		.cra_module = THIS_MODULE,
	},
}, {
	.init = lce_acomp_init_tfm,
	.exit = lce_acomp_exit_tfm,
	.compress = lce_zstd_compress,
	.decompress = lce_zstd_decompress,
	.dst_free = sgl_free,
	.reqsize = sizeof(struct lce_async_req),
	.base = {
		.cra_name = "zstd",
		.cra_driver_name = "qat_lce_zstd",
		.cra_priority = 4001,
		.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_ALLOCATES_MEMORY,
		.cra_ctxsize = sizeof(struct lce_comp_ctx),
		.cra_module = THIS_MODULE,
	},
}};

/* CPF programs odd banks for DC (compression), even for CY (crypto).
 * Use bank 1 (first odd bank) for compression offload.
 */
#define LCE_COMP_BANK_ID	1
#define LCE_COMP_MIN_BANKS	2

int lce_comp_algs_register(struct lce_hw_device *lcehw)
{
	int ret = 0;

	mutex_lock(&algs_lock);
	if (++active_devs > 1) {
		mutex_unlock(&algs_lock);
		return 0;
	}

	if (lcehw->num_banks < LCE_COMP_MIN_BANKS) {
		dev_err(&lcehw->pdev->dev,
			"Need %d banks for comp, got %u\n",
			LCE_COMP_MIN_BANKS, lcehw->num_banks);
		ret = -ENODEV;
		goto out_dec;
	}

	ret = lce_ring_pair_init(lcehw, &lce_comp_rp, LCE_COMP_BANK_ID);
	if (ret)
		goto out_dec;

	lcehw->comp_rp = &lce_comp_rp;
	ret = lce_ring_poll_start(&lce_comp_rp, lce_comp_resp_cb, lcehw);
	if (ret) {
		lce_ring_pair_cleanup(lcehw, &lce_comp_rp);
		goto out_dec;
	}

	lce_comp_dev = lcehw;
	mutex_unlock(&algs_lock);

	ret = crypto_register_acomps(lce_acomp_algs, ARRAY_SIZE(lce_acomp_algs));
	if (ret) {
		mutex_lock(&algs_lock);
		lce_ring_poll_stop(&lce_comp_rp);
		lce_ring_pair_cleanup(lcehw, &lce_comp_rp);
		lce_comp_dev = NULL;
		active_devs--;
		mutex_unlock(&algs_lock);
	}

	return ret;

out_dec:
	active_devs--;
	mutex_unlock(&algs_lock);
	return ret;
}

void lce_comp_algs_unregister(struct lce_hw_device *lcehw)
{
	mutex_lock(&algs_lock);
	if (--active_devs > 0) {
		mutex_unlock(&algs_lock);
		return;
	}
	mutex_unlock(&algs_lock);

	crypto_unregister_acomps(lce_acomp_algs, ARRAY_SIZE(lce_acomp_algs));

	mutex_lock(&algs_lock);
	lce_ring_poll_stop(&lce_comp_rp);
	lcehw->comp_rp = NULL;
	lce_ring_pair_cleanup(lcehw, &lce_comp_rp);
	lce_comp_dev = NULL;
	mutex_unlock(&algs_lock);
}
