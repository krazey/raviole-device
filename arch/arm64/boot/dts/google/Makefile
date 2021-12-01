# SPDX-License-Identifier: GPL-2.0

gs101_overlays := \
	gs101-slider.dtbo \
	gs101-slider2.dtbo \
	gs101-slider2-o6r4.dtbo \
	gs101-whitefin.dtbo \
	gs101-whitefin2.dtbo \
	gs101-whitefin2v2.dtbo \
	gs101-oriole.dtbo \
	gs101-oriole-evt1_1.dtbo \
	gs101-oriole-evt-wingboard.dtbo \
	gs101-oriole-evt1.dtbo \
	gs101-oriole-proto1.dtbo \
	gs101-oriole-proto1_1.dtbo \
	gs101-raven.dtbo \
	gs101-raven-evt1_1.dtbo \
	gs101-raven-evt-wingboard.dtbo \
	gs101-raven-evt1.dtbo \
	gs101-raven-proto1.dtbo \
	gs101-raven-proto1_1.dtbo

dtb-$(CONFIG_SOC_GS101) += gs101-a0.dtb gs101-b0.dtb $(gs101_overlays)

MKDTIMG_FLAGS += --page_size=4096  --id=/:board_id --rev=/:board_rev
