# SPDX-License-Identifier: GPL-2.0-only
ccflags-y := -I $(srctree)/$(src)/../qat_common
obj-$(CONFIG_CRYPTO_DEV_QAT_LCE) += qat_lce.o
qat_lce-y := adf_drv.o \
             adf_lce_hw_data.o \
             adf_lce_mbx.o \
             adf_lce_poll.o
