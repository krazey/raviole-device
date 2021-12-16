# SPDX-License-Identifier: GPL-2.0

modules clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(@)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(@) headers_install
