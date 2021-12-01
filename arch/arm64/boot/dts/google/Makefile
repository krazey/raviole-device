# SPDX-License-Identifier: GPL-2.0

gs101_raviole_overlays := \
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

gs101_raviole_dtbs := gs101-a0.dtb gs101-b0.dtb

# Usage:
#   $(call DTBS_template, list_of_dtbos, base_dtb, construct_suffix)
define DTBS_template
 $(1:%.dtbo=%-$(strip $3)-dtbs) := $2 $1
endef

# Create the -dtbs constructs by looping through the base dtbs and apply each
# overlay to it. Kbuild will use these -dtbs constructs to determine which dtbs
# are the base dtbs and which are the overlays. For the base dtbs, kbuild will
# expose the symbols as needed for the overlays. Additionally, kbuild will
# statically apply the overlay to the base dtb which could be used to verify
# the overlay applies cleanly.
$(foreach base, $(gs101_raviole_dtbs), \
	$(foreach overlay, $(gs101_raviole_overlays), \
		$(eval $(call DTBS_template, $(overlay), $(base), $(base:gs101-%.dtb=%)))))

dtb-$(CONFIG_SOC_GS101) += \
	$(gs101_raviole_overlays:%.dtbo=%-a0.dtb) \
	$(gs101_raviole_overlays:%.dtbo=%-b0.dtb)
