/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */

#ifndef ADF_LCE_COMP_H_
#define ADF_LCE_COMP_H_

#include <linux/types.h>
#include <icp_qat_fw.h>
#include <icp_qat_fw_comp.h>

struct lce_hw_device;

/* GEN5 (QAT 3.0) compression command IDs */
enum lce_comp_cmd_id {
	LCE_CMD_DEFLATE_COMP	= 1,
	LCE_CMD_DEFLATE_DECOMP	= 2,
	LCE_CMD_ZSTD_COMP	= 10,
	LCE_CMD_ZSTD_DECOMP	= 11,
};

/* GEN5 cd_pars.serv_specif_fields (LW4) bit layout */
#define LCE_COMP_SEARCH_DEPTH_MASK	GENMASK(7, 0)
#define LCE_COMP_HIST_BUFF_MASK		GENMASK(15, 8)
#define LCE_COMP_DEFLATE_TYPE_BIT	BIT(16)

/* History buffer sizes */
#define LCE_HIST_BUFF_32K	5
#define LCE_HIST_BUFF_64K	6

/* Search depths (compression levels) */
#define LCE_SEARCH_DEPTH_L1	1
#define LCE_SEARCH_DEPTH_L6	3
#define LCE_SEARCH_DEPTH_L9	4

/* Deflate type */
#define LCE_DEFLATE_DYNAMIC	1

/* GEN5 CRC control (LW21) byte layout */
struct lce_crc_ctrl {
	__u8 crc_algo;		/* bits[3:0] = algorithm (2 = CRC32C) */
	__u8 verify_flags;
	__u8 gen_flags;		/* bit4 = gen CRCcd, bit5 = gen CRCud */
	__u8 append_flags;
} __packed;

#define LCE_CRC_ALGO_CRC32C		0x02
#define LCE_CRC_GEN_CRCC_CRCU		0x30	/* Generate both CRCcd and CRCud */

/* GEN5 Address Domain metadata (8 bytes) */
struct lce_ad_metadata {
	__le32 lo;	/* PASID[19:0], FuncID[30:20], rsvd[31] */
	__le32 hi;	/* PASID_valid[0], rsvd[15:1], PF_ID[21:16],
			 * NodeID[24:22], FuncType[26:25], ReqID[29:27], rsvd[31:30]
			 */
} __packed;

/*
 * GEN5 (QAT 3.0) compression request descriptor - 128 bytes (32 LWs).
 *
 * Reuses struct icp_qat_fw_comn_req_hdr (LW0-1) and
 * struct icp_qat_fw_comn_req_mid (LW6-13) from the existing kernel FW headers.
 */
struct lce_comp_req {
	/* LW0-1: Common request header */
	struct icp_qat_fw_comn_req_hdr comn_hdr;

	/* LW2-3: Key buffer address (unused for compression, set to 0) */
	__le64 key_buffer;

	/* LW4: Content descriptor / service-specific fields */
	__le32 cd_pars;

	/* LW5: Reserved */
	__le32 resrvd1;

	/* LW6-13: Common request middle section */
	struct icp_qat_fw_comn_req_mid comn_mid;

	/* LW14-19: Reserved */
	__le32 resrvd2[6];

	/* LW20: Compression flags (CnV, ASB, CnVDfx) */
	__le32 comp_flags;

	/* LW21: CRC control */
	struct lce_crc_ctrl crc_ctrl;

	/* LW22-23: CRC buffer address */
	__le64 crc_addr;

	/* LW24-25: Source AD metadata (flat buffer mode only; SGL uses BL header) */
	struct lce_ad_metadata src_ad;

	/* LW26-27: Reserved */
	__le64 resrvd3;

	/* LW28-29: Dest AD metadata (flat buffer mode only; SGL uses BL header) */
	struct lce_ad_metadata dst_ad;

	/* LW30-31: Reserved */
	__le64 resrvd4;
} __packed;

/* Verify GEN5 request is 128 bytes */
static_assert(sizeof(struct lce_comp_req) == 128);

/* GEN5 hdr_flags for valid request with GEN5 layout indicator */
#define LCE_HDR_FLAGS_VALID	0xE0

/* Address Domain metadata value for Host PF (nodeId=0, reqId=2, funcType=PF) */
#define LCE_AD_META_HI		cpu_to_le32(0x14000000)
#define LCE_AD_META_LO		cpu_to_le32(0x00000000)

/* Destination buffer sizing constants from SAL */
#define LCE_ZSTD_DST_OVERHEAD		660
#define LCE_ZSTD_DST_MIN		1024
#define LCE_DEFLATE_DST_EXTRA_STATIC	1029
#define LCE_DEFLATE_DST_MIN		1024

/* Size of DMA-coherent CRC output buffer required by FW */
#define LCE_CRC_BUF_SIZE		64

int lce_comp_algs_register(struct lce_hw_device *lcehw);
void lce_comp_algs_unregister(struct lce_hw_device *lcehw);

#endif /* ADF_LCE_COMP_H_ */
