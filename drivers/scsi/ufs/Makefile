# SPDX-License-Identifier: GPL-2.0
# Pixel UFSHCD Makefile

obj-$(CONFIG_SCSI_UFS_PIXEL) += ufs-pixel-core.o
ufs-pixel-core-y += ufs-exynos.o ufs-exynos-dbg.o ufs-pixel.o
ufs-pixel-core-$(CONFIG_SOC_GS101) += gs101/ufs-cal-if.o
ufs-pixel-core-$(CONFIG_SCSI_UFS_CRYPTO) += ufs-pixel-crypto.o

ccflags-y += -I $(srctree)/drivers/scsi/ufs

