// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF Heap Allocator - secure management
 *
 * Copyright (c) 2021 Samsung Electronics Co., Ltd.
 * Author: <hyesoo.yu@samsung.com> for Samsung
 */

#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/smc.h>
#include <linux/kmemleak.h>
#include <linux/dma-mapping.h>
#include <linux/arm-smccc.h>
#include <linux/dma-direct.h>

#include "heap_private.h"

#define SECURE_DMA_BASE	0x40000000
#define SECURE_DMA_END	0xE0000000

static struct gen_pool *secure_iova_pool;

/*
 * Alignment to a secure address larger than 16MiB is not beneficial because
 * the protection alignment just needs 64KiB by the buffer protection H/W and
 * the largest granule of H/W security firewall (the secure context of SysMMU)
 * is 16MiB.
 */
#define MAX_SECURE_VA_ALIGN	(SZ_16M / PAGE_SIZE)

static int secure_iova_alloc(unsigned long *addr, unsigned long size, unsigned int align)
{
	unsigned long out_addr;
	struct genpool_data_align alignment = {
		.align = max_t(int, PFN_DOWN(align), MAX_SECURE_VA_ALIGN),
	};

	if (WARN_ON_ONCE(!secure_iova_pool))
		return -ENODEV;

	out_addr = gen_pool_alloc_algo(secure_iova_pool, size,
				       gen_pool_first_fit_align, &alignment);

	if (out_addr == 0) {
		perrfn("failed alloc secure iova. %zu/%zu bytes used",
		       gen_pool_avail(secure_iova_pool),
		       gen_pool_size(secure_iova_pool));
		return -ENOMEM;
	}

	*addr = out_addr;

	return 0;
}

static void secure_iova_free(unsigned long addr, unsigned long size)
{
	gen_pool_free(secure_iova_pool, addr, size);
}

#define SMC_DRM_PPMP_PROT		(0x82002110)
#define SMC_DRM_PPMP_UNPROT		(0x82002111)

static inline unsigned long ppmp_smc(unsigned long cmd, unsigned long arg0,
				     unsigned long arg1, unsigned long arg2)
{
	struct arm_smccc_res res;

	arm_smccc_smc(cmd, arg0, arg1, arg2, 0, 0, 0, 0, &res);
	return (unsigned long)res.a0;
}

static int buffer_protect_smc(struct device *dev, struct buffer_prot_info *protdesc,
			      unsigned int protalign)
{
	unsigned long size = protdesc->chunk_count * protdesc->chunk_size;
	unsigned long drmret = 0, dma_addr = 0;
	unsigned long ret;

	ret = secure_iova_alloc(&dma_addr, size, max_t(u32, protalign, PAGE_SIZE));
	if (ret)
		return ret;

	protdesc->dma_addr = (unsigned int)dma_addr;

	dma_map_single(dev, protdesc, sizeof(*protdesc), DMA_TO_DEVICE);

	ret = ppmp_smc(SMC_DRM_PPMP_PROT, virt_to_phys(protdesc), 0, 0);
	if (ret) {
		secure_iova_free(dma_addr, size);
		perr("CMD %#x (err=%#lx,va=%#x,len=%#lx,cnt=%u,flg=%u)",
		     SMC_DRM_PPMP_PROT, drmret, protdesc->dma_addr, size,
		     protdesc->chunk_count, protdesc->flags);
		dma_unmap_single(dev, protdesc->dma_addr, sizeof(*protdesc),
				 DMA_TO_DEVICE);
		return -EACCES;
	}

	return 0;
}

static int buffer_unprotect_smc(struct device *dev,
				struct buffer_prot_info *protdesc)
{
	unsigned long size = protdesc->chunk_count * protdesc->chunk_size;
	unsigned long ret;

	ret = ppmp_smc(SMC_DRM_PPMP_UNPROT, virt_to_phys(protdesc), 0, 0);

	if (ret) {
		perr("CMD %#x (err=%#lx,va=%#x,len=%#lx,cnt=%u,flg=%u)",
		     SMC_DRM_PPMP_UNPROT, ret, protdesc->dma_addr,
		     size, protdesc->chunk_count, protdesc->flags);
		return -EACCES;
	}

	dma_unmap_single(dev, protdesc->dma_addr, sizeof(*protdesc),
			 DMA_TO_DEVICE);

	/*
	 * retain the secure device address if unprotection to its area fails.
	 * It might be unusable forever since we do not know the state of the
	 * secure world before returning error from ppmp_smc() above.
	 */
	secure_iova_free(protdesc->dma_addr, size);

	return 0;
}

void *samsung_dma_buffer_protect(struct samsung_dma_heap *heap, unsigned int size,
				 unsigned long phys)
{
	struct buffer_prot_info *protdesc;
	unsigned int protalign = heap->alignment;
	int ret;

	protdesc = kmalloc(sizeof(*protdesc), GFP_KERNEL);
	if (!protdesc)
		return ERR_PTR(-ENOMEM);

	protdesc->chunk_count = 1;
	protdesc->flags = heap->protection_id;
	protdesc->chunk_size = ALIGN(size, protalign);
	protdesc->bus_address = phys;

	ret = buffer_protect_smc(dma_heap_get_dev(heap->dma_heap), protdesc, protalign);
	if (ret) {
		perr("protection failure (id%u,len%u,base%#lx,align%#x",
		     heap->protection_id, size, phys, protalign);
		kfree(protdesc);
		return ERR_PTR(ret);
	}

	return protdesc;
}

int samsung_dma_buffer_unprotect(void *priv, struct device *dev)
{
	struct buffer_prot_info *protdesc = priv;
	int ret = 0;

	if (priv) {
		ret = buffer_unprotect_smc(dev, protdesc);
		kfree(protdesc);
	}

	return ret;
}

int secure_iova_pool_create(void)
{
	int ret;

	secure_iova_pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!secure_iova_pool) {
		perr("failed to create Secure IOVA pool");
		return -ENOMEM;
	}

	ret = gen_pool_add(secure_iova_pool, SECURE_DMA_BASE,
			   SECURE_DMA_END - SECURE_DMA_BASE, -1);
	if (ret) {
		perr("failed to set address range of Secure IOVA pool");
		gen_pool_destroy(secure_iova_pool);
		return ret;
	}

	return 0;
}

void secure_iova_pool_destroy(void)
{
	gen_pool_destroy(secure_iova_pool);
}
