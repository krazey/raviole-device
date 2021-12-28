# SPDX-License-Identifier: GPL-2.0

# make $(src) as absolute path if it is not already, by prefixing $(srctree)
# This is to prevent any build issue due to wrong path.
src:=$(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))

subdir-ccflags-y += \
		-I$(src)/include \
		-I$(src)/include/uapi \

obj-y += drivers/phy/

obj-y += drivers/pinctrl/

obj-y += drivers/pci/controller/dwc/

obj-y += drivers/clk/gs/

obj-y += drivers/dma/

obj-y += drivers/soc/google/

obj-y += drivers/regulator/

obj-y += drivers/tty/serial/

obj-y += drivers/char/hw_random/

obj-y += drivers/iommu/

obj-y += drivers/gpu/

obj-y += drivers/misc/

obj-y += drivers/mfd/

obj-y += drivers/dma-buf/heaps/

obj-y += drivers/scsi/ufs/

obj-y += drivers/spi/

obj-y += drivers/usb/

obj-y += drivers/input/

obj-y += drivers/rtc/

obj-y += drivers/i2c/busses/

obj-y += drivers/media/platform/

obj-y += drivers/power/

obj-y += drivers/thermal/

obj-y += drivers/trusty/

obj-y += drivers/watchdog/

obj-y += drivers/cpufreq/

obj-y += drivers/clocksource/

obj-y += drivers/devfreq/google/

obj-y += drivers/iio/

obj-y += drivers/staging/android/

obj-y += drivers/bts/
