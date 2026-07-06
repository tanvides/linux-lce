// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <adf_accel_devices.h>
#include <adf_common_drv.h>
#include <adf_transport_internal.h>

#include "adf_lce_hw_data.h"
#include "adf_lce_isr.h"

/*
 * LCE APF MSI-X vector layout is fixed in hardware:
 *   vector 0        - CPF <-> APF mailbox
 *   vectors 1..N    - ring bundles 0..N-1
 * The APF exposes no writable MSI-X routing table (the CPF programs its
 * own routing table but no equivalent CSR exists on the APF), so the
 * framework adf_isr_resource_alloc(), which assumes bank i lives at
 * vector i, cannot be reused. This file provides an LCE-specific
 * allocator that mirrors the framework code path but shifts the
 * ring-bundle vectors by one to leave vector 0 for the mailbox.
 */

static irqreturn_t adf_lce_mbx_isr(int irq, void *dev_ptr)
{
	struct adf_accel_dev *accel_dev = dev_ptr;
	struct lce_hw_device *lcehw = pci_get_drvdata(accel_to_pci_dev(accel_dev));

	/* Ack the CPF->APF mailbox event by clearing the register. */
	LCE_CSR_RD(lcehw->iobase, LCE_MBX_CPF2APF);
	LCE_CSR_WR(lcehw->iobase, LCE_MBX_CPF2APF, 0);

	return IRQ_HANDLED;
}

static irqreturn_t adf_lce_msix_isr_bundle(int irq, void *bank_ptr)
{
	struct adf_etr_bank_data *bank = bank_ptr;
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(bank->accel_dev);

	csr_ops->write_csr_int_flag_and_col(bank->csr_addr, bank->bank_number, 0);
	tasklet_hi_schedule(&bank->resp_handler);

	return IRQ_HANDLED;
}

static int adf_lce_enable_msix(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	u32 nvec = hw_data->num_banks + 1;
	int ret;

	ret = pci_alloc_irq_vectors(pci_info->pci_dev, nvec, nvec, PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&GET_DEV(accel_dev),
			"Failed to allocate %u MSI-X vectors\n", nvec);
		return ret;
	}
	return 0;
}

static int adf_lce_setup_bh(struct adf_accel_dev *accel_dev)
{
	struct adf_etr_data *etr = accel_dev->transport;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	u32 i;

	for (i = 0; i < hw_data->num_banks; i++)
		tasklet_init(&etr->banks[i].resp_handler,
			     adf_response_handler,
			     (unsigned long)&etr->banks[i]);
	return 0;
}

static void adf_lce_cleanup_bh(struct adf_accel_dev *accel_dev)
{
	struct adf_etr_data *etr = accel_dev->transport;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	u32 i;

	for (i = 0; i < hw_data->num_banks; i++) {
		tasklet_disable(&etr->banks[i].resp_handler);
		tasklet_kill(&etr->banks[i].resp_handler);
	}
}

static int adf_lce_alloc_msix_data(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	u32 nvec = hw_data->num_banks + 1;
	struct adf_irq *irqs;

	irqs = kcalloc_node(nvec, sizeof(*irqs), GFP_KERNEL,
			    dev_to_node(&GET_DEV(accel_dev)));
	if (!irqs)
		return -ENOMEM;

	accel_dev->accel_pci_dev.msix_entries.num_entries = nvec;
	accel_dev->accel_pci_dev.msix_entries.irqs = irqs;
	return 0;
}

static void adf_lce_free_msix_data(struct adf_accel_dev *accel_dev)
{
	kfree(accel_dev->accel_pci_dev.msix_entries.irqs);
	accel_dev->accel_pci_dev.msix_entries.irqs = NULL;
}

static void adf_lce_free_irqs(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_irq *irqs = pci_info->msix_entries.irqs;
	struct adf_etr_data *etr = accel_dev->transport;
	int irq;
	u32 i;

	if (irqs[0].enabled) {
		irq = pci_irq_vector(pci_info->pci_dev, 0);
		free_irq(irq, accel_dev);
	}

	for (i = 0; i < hw_data->num_banks; i++) {
		if (!irqs[i + 1].enabled)
			continue;
		irq = pci_irq_vector(pci_info->pci_dev, i + 1);
		irq_set_affinity_hint(irq, NULL);
		free_irq(irq, &etr->banks[i]);
	}
}

static int adf_lce_request_irqs(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_irq *irqs = pci_info->msix_entries.irqs;
	struct adf_etr_data *etr = accel_dev->transport;
	unsigned int cpus = num_online_cpus();
	int ret, irq;
	char *name;
	u32 i;

	/* Vector 0: CPF <-> APF mailbox */
	name = irqs[0].name;
	snprintf(name, ADF_MAX_MSIX_VECTOR_NAME, "qat-lce%d-mbx",
		 accel_dev->accel_id);
	irq = pci_irq_vector(pci_info->pci_dev, 0);
	if (irq < 0) {
		ret = irq;
		goto err;
	}
	ret = request_irq(irq, adf_lce_mbx_isr, 0, name, accel_dev);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			"Failed to allocate IRQ %d for %s\n", irq, name);
		goto err;
	}
	irqs[0].enabled = true;

	/* Vectors 1..N: ring bundles */
	for (i = 0; i < hw_data->num_banks; i++) {
		struct adf_etr_bank_data *bank = &etr->banks[i];
		unsigned int cpu;

		name = irqs[i + 1].name;
		snprintf(name, ADF_MAX_MSIX_VECTOR_NAME, "qat-lce%d-bundle%u",
			 accel_dev->accel_id, i);
		irq = pci_irq_vector(pci_info->pci_dev, i + 1);
		if (irq < 0) {
			ret = irq;
			goto err;
		}
		ret = request_irq(irq, adf_lce_msix_isr_bundle, 0, name, bank);
		if (ret) {
			dev_err(&GET_DEV(accel_dev),
				"Failed to allocate IRQ %d for %s\n", irq, name);
			goto err;
		}
		cpu = ((accel_dev->accel_id * hw_data->num_banks) + i) % cpus;
		irq_set_affinity_hint(irq, get_cpu_mask(cpu));
		irqs[i + 1].enabled = true;
	}
	return 0;

err:
	adf_lce_free_irqs(accel_dev);
	return ret;
}

int adf_lce_isr_resource_alloc(struct adf_accel_dev *accel_dev)
{
	int ret;

	ret = adf_lce_alloc_msix_data(accel_dev);
	if (ret)
		goto err_out;

	ret = adf_lce_enable_msix(accel_dev);
	if (ret)
		goto err_free_data;

	ret = adf_lce_setup_bh(accel_dev);
	if (ret)
		goto err_disable_msix;

	ret = adf_lce_request_irqs(accel_dev);
	if (ret)
		goto err_cleanup_bh;

	return 0;

err_cleanup_bh:
	adf_lce_cleanup_bh(accel_dev);
err_disable_msix:
	pci_free_irq_vectors(accel_dev->accel_pci_dev.pci_dev);
err_free_data:
	adf_lce_free_msix_data(accel_dev);
err_out:
	return ret;
}

void adf_lce_isr_resource_free(struct adf_accel_dev *accel_dev)
{
	adf_lce_free_irqs(accel_dev);
	adf_lce_cleanup_bh(accel_dev);
	pci_free_irq_vectors(accel_dev->accel_pci_dev.pci_dev);
	adf_lce_free_msix_data(accel_dev);
}
