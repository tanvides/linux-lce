// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

/*
 * Response polling service for LCE.
 *
 * Unlike prior QAT generations, the LCE PF does not receive per-bank
 * completion MSI-X vectors: MSI-X routing for LCE APF response rings is
 * owned by the CPF and is not exposed to the host driver. Responses are
 * therefore drained by a dedicated per-device kthread that walks all
 * configured banks and invokes the framework's bank response handler
 * (adf_response_handler()) exactly as an ISR would.
 *
 * A poll cadence of 50-100us keeps end-to-end latency comparable to
 * interrupt-driven completion on typical compression workloads; wider
 * intervals produced measurable throughput degradation on 64K deflate.
 */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include <adf_accel_devices.h>

#include "adf_lce_hw_data.h"
#include "adf_lce_poll.h"
#include "adf_transport_internal.h"

#define LCE_POLL_INTERVAL_US_MIN	50
#define LCE_POLL_INTERVAL_US_MAX	100

static int lce_poll_thread(void *data)
{
	struct lce_hw_device *lcehw = data;
	struct adf_accel_dev *accel_dev = lcehw->accel_dev;

	while (!kthread_should_stop()) {
		struct adf_etr_data *etr = accel_dev->transport;
		u32 i;

		if (etr && etr->banks) {
			for (i = 0; i < lcehw->num_banks; i++)
				adf_response_handler((uintptr_t)&etr->banks[i]);
		}
		usleep_range(LCE_POLL_INTERVAL_US_MIN,
			     LCE_POLL_INTERVAL_US_MAX);
	}
	return 0;
}

int adf_lce_poll_start(struct lce_hw_device *lcehw)
{
	if (lcehw->poll_thread)
		return 0;

	lcehw->poll_thread = kthread_run(lce_poll_thread, lcehw,
					 "lce_poll/%s",
					 dev_name(&lcehw->pdev->dev));
	if (IS_ERR(lcehw->poll_thread)) {
		int ret = PTR_ERR(lcehw->poll_thread);

		lcehw->poll_thread = NULL;
		return dev_err_probe(&lcehw->pdev->dev, ret,
				     "poll kthread start failed\n");
	}
	dev_info(&lcehw->pdev->dev, "LCE polling response handler started\n");
	return 0;
}

void adf_lce_poll_stop(struct lce_hw_device *lcehw)
{
	if (lcehw->poll_thread) {
		kthread_stop(lcehw->poll_thread);
		lcehw->poll_thread = NULL;
		dev_info(&lcehw->pdev->dev,
			 "LCE polling response handler stopped\n");
	}
}
