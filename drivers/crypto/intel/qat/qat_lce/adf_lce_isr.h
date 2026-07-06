/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2026 Intel Corporation */
#ifndef ADF_LCE_ISR_H_
#define ADF_LCE_ISR_H_

struct adf_accel_dev;

int adf_lce_isr_resource_alloc(struct adf_accel_dev *accel_dev);
void adf_lce_isr_resource_free(struct adf_accel_dev *accel_dev);

#endif /* ADF_LCE_ISR_H_ */
