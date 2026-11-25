/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */
#ifndef ADF_61MX_FW_H_
#define ADF_61MX_FW_H_

#include <linux/types.h>

#include <icp_qat_fw.h>

/* QAT 3.0 compression request descriptor */

#define ADF_61MX_FW_GEN_FLAG_BITPOS	5
#define ADF_61MX_FW_GEN_FLAG_MASK	0x3
#define ADF_61MX_FW_GEN_5		3

#define ADF_61MX_FW_HDR_GEN_FLAG_SET(hdr_t, val) \
	QAT_FIELD_SET((hdr_t.hdr_flags), (val), \
		      ADF_61MX_FW_GEN_FLAG_BITPOS, ADF_61MX_FW_GEN_FLAG_MASK)

enum adf_61mx_fw_comp_cmd_id {
	ADF_61MX_FW_COMP_CMD_DEFLATE_COMP = 1,
	ADF_61MX_FW_COMP_CMD_DEFLATE_DECOMP = 2,
};

#define ADF_61MX_FW_COMP_SEARCH_DEPTH_1		1
#define ADF_61MX_FW_COMP_HIST_BUFF_SIZE_32K	5
#define ADF_61MX_FW_COMP_DEFLATE_TYPE_DYNAMIC	1

#define ADF_61MX_FW_COMP_CFG_BUILD(depth, hist, deflate_type) \
	((((depth) & 0xFF) << 0) | \
	 (((hist) & 0xFF) << 8) | \
	 (((deflate_type) & 0x1) << 16))

#define ADF_61MX_FW_COMP_FLAG_CNV	0x1

/* Same 128-byte size as struct icp_qat_fw_comp_req */
struct adf_61mx_fw_comp_req {
	struct icp_qat_fw_comn_req_hdr comn_hdr;
	__u64 key_buffer;
	__u32 comp_cfg;
	__u32 resrvd1;
	struct icp_qat_fw_comn_req_mid comn_mid;
	/* LW14-15 get the GEN4 comp_len and out_buffer_sz, ignored by firmware */
	__u32 resrvd2[ICP_QAT_FW_NUM_LONGWORDS_6];
	__u32 cmp_flags;
	__u32 crc_ctrl[ICP_QAT_FW_NUM_LONGWORDS_3];
	__u64 ad_info[ICP_QAT_FW_NUM_LONGWORDS_4];
};

#endif /* ADF_61MX_FW_H_ */
