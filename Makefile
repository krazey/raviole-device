# SPDX-License-Identifier: GPL-2.0

# make $(src) as absolute path if it is not already, by prefixing $(srctree)
# This is to prevent any build issue due to wrong path.
src:=$(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(@)

