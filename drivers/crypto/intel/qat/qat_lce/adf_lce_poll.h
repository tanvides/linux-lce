/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */

#ifndef ADF_LCE_POLL_H_
#define ADF_LCE_POLL_H_

struct lce_hw_device;

int adf_lce_poll_start(struct lce_hw_device *lcehw);
void adf_lce_poll_stop(struct lce_hw_device *lcehw);

#endif /* ADF_LCE_POLL_H_ */
