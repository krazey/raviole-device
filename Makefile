# SPDX-License-Identifier: GPL-2.0

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(@)

