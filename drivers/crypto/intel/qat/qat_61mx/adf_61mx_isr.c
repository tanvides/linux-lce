// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation */

#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <adf_accel_devices.h>
#include <adf_common_drv.h>
#include <adf_transport_internal.h>

#include "adf_61mx_hw_data.h"
#include "adf_61mx_mbx.h"

/* The CPF maps the mailbox to MSI-X entry 0 and ring bundle N to entry N + 1 */
#define ADF_61MX_MSIX_CTRL_VEC		0
#define ADF_61MX_MSIX_BANK_VEC(bank)	((bank) + 1)

static irqreturn_t adf_61mx_bundle_isr(int irq, void *bank_ptr)
{
	struct adf_etr_bank_data *bank = bank_ptr;
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(bank->accel_dev);

	csr_ops->write_csr_int_flag_and_col(bank->csr_addr, bank->bank_number, 0);
	tasklet_hi_schedule(&bank->resp_handler);
	return IRQ_HANDLED;
}

static void adf_61mx_response_handler(uintptr_t bank_addr)
{
	struct adf_etr_bank_data *bank = (void *)bank_addr;
	struct adf_hw_csr_ops *csr_ops = GET_CSR_OPS(bank->accel_dev);

	adf_response_handler(bank_addr);

	/* The ring interrupt is edge-like: a response landing during the drain is not signalled */
	if (~csr_ops->read_csr_e_stat(bank->csr_addr, bank->bank_number) &
	    bank->irq_mask) {
		csr_ops->write_csr_int_flag_and_col(bank->csr_addr,
						    bank->bank_number, 0);
		tasklet_hi_schedule(&bank->resp_handler);
	}
}

static void adf_61mx_free_irqs(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_dev_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_irq *irqs = pci_dev_info->msix_entries.irqs;
	struct adf_etr_data *etr_data = accel_dev->transport;
	int irq, i;

	for (i = 0; i < hw_data->num_banks; i++) {
		int vec = ADF_61MX_MSIX_BANK_VEC(i);

		if (!irqs[vec].enabled)
			continue;
		irq = pci_irq_vector(pci_dev_info->pci_dev, vec);
		irq_set_affinity_hint(irq, NULL);
		free_irq(irq, &etr_data->banks[i]);
		irqs[vec].enabled = false;
	}

	if (irqs[ADF_61MX_MSIX_CTRL_VEC].enabled) {
		irq = pci_irq_vector(pci_dev_info->pci_dev, ADF_61MX_MSIX_CTRL_VEC);
		free_irq(irq, accel_dev);
		irqs[ADF_61MX_MSIX_CTRL_VEC].enabled = false;
	}
}

static int adf_61mx_request_irq(struct adf_accel_dev *accel_dev, int vec,
				irq_handler_t handler, void *dev_id)
{
	struct adf_accel_pci *pci_dev_info = &accel_dev->accel_pci_dev;
	struct adf_irq *irq_data = &pci_dev_info->msix_entries.irqs[vec];
	int irq, ret;

	irq = pci_irq_vector(pci_dev_info->pci_dev, vec);
	if (irq < 0) {
		dev_err(&GET_DEV(accel_dev),
			"Failed to get IRQ number of device vector %d - %s\n",
			vec, irq_data->name);
		return irq;
	}

	ret = request_irq(irq, handler, 0, irq_data->name, dev_id);
	if (ret) {
		dev_err(&GET_DEV(accel_dev), "Failed to allocate IRQ %d for %s\n",
			irq, irq_data->name);
		return ret;
	}
	irq_data->enabled = true;
	return irq;
}

static int adf_61mx_request_irqs(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_dev_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_irq *irqs = pci_dev_info->msix_entries.irqs;
	struct adf_etr_data *etr_data = accel_dev->transport;
	unsigned int cpus = num_online_cpus();
	int irq, i;

	for (i = 0; i < hw_data->num_banks; i++) {
		int vec = ADF_61MX_MSIX_BANK_VEC(i);
		unsigned int cpu;

		snprintf(irqs[vec].name, ADF_MAX_MSIX_VECTOR_NAME,
			 "qat%d-bundle%d", accel_dev->accel_id, i);
		irq = adf_61mx_request_irq(accel_dev, vec, adf_61mx_bundle_isr,
					   &etr_data->banks[i]);
		if (irq < 0)
			goto err;

		cpu = ((accel_dev->accel_id * hw_data->num_banks) + i) % cpus;
		irq_set_affinity_hint(irq, get_cpu_mask(cpu));
	}

	snprintf(irqs[ADF_61MX_MSIX_CTRL_VEC].name, ADF_MAX_MSIX_VECTOR_NAME,
		 "qat%d-ctrl", accel_dev->accel_id);
	irq = adf_61mx_request_irq(accel_dev, ADF_61MX_MSIX_CTRL_VEC,
				   adf_61mx_mbx_isr, accel_dev);
	if (irq < 0)
		goto err;

	return 0;

err:
	adf_61mx_free_irqs(accel_dev);
	return irq;
}

static void adf_61mx_cleanup_bh(struct adf_accel_dev *accel_dev)
{
	struct adf_etr_data *etr_data = accel_dev->transport;
	int i;

	for (i = 0; i < accel_dev->hw_device->num_banks; i++) {
		tasklet_disable(&etr_data->banks[i].resp_handler);
		tasklet_kill(&etr_data->banks[i].resp_handler);
	}
}

int adf_61mx_alloc_irq(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_dev_info = &accel_dev->accel_pci_dev;
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	struct adf_etr_data *etr_data = accel_dev->transport;
	u32 num_vecs = hw_data->num_banks + 1;
	struct adf_irq *irqs;
	int ret, i;

	irqs = kcalloc_node(num_vecs, sizeof(*irqs), GFP_KERNEL,
			    dev_to_node(&GET_DEV(accel_dev)));
	if (!irqs)
		return -ENOMEM;

	ret = pci_alloc_irq_vectors(pci_dev_info->pci_dev, num_vecs, num_vecs,
				    PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&GET_DEV(accel_dev), "Failed to allocate %u MSI-X vectors\n",
			num_vecs);
		kfree(irqs);
		return ret;
	}

	pci_dev_info->msix_entries.irqs = irqs;
	pci_dev_info->msix_entries.num_entries = num_vecs;

	for (i = 0; i < hw_data->num_banks; i++)
		tasklet_init(&etr_data->banks[i].resp_handler,
			     adf_61mx_response_handler,
			     (unsigned long)&etr_data->banks[i]);

	ret = adf_61mx_request_irqs(accel_dev);
	if (ret)
		goto err;

	return 0;

err:
	adf_61mx_cleanup_bh(accel_dev);
	pci_free_irq_vectors(pci_dev_info->pci_dev);
	kfree(irqs);
	pci_dev_info->msix_entries.irqs = NULL;
	return ret;
}

void adf_61mx_free_irq(struct adf_accel_dev *accel_dev)
{
	struct adf_accel_pci *pci_dev_info = &accel_dev->accel_pci_dev;

	adf_61mx_free_irqs(accel_dev);
	adf_61mx_cleanup_bh(accel_dev);
	pci_free_irq_vectors(pci_dev_info->pci_dev);
	kfree(pci_dev_info->msix_entries.irqs);
	pci_dev_info->msix_entries.irqs = NULL;
}
