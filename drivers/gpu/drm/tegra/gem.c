/*
 * NVIDIA Tegra DRM GEM helper functions
 *
 * Copyright (C) 2012 Sascha Hauer, Pengutronix
 * Copyright (C) 2013 NVIDIA CORPORATION, All rights reserved.
 *
 * Based on the GEM/CMA helpers
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/dma-buf.h>
#include <linux/iommu.h>
#include <drm/tegra_drm.h>

#include "drm.h"
#include "gem.h"

struct tegra_gem_importer {
	struct device *dev;
	void *drvdata;
	void (*delete)(void *);
};
#define NUM_TEGRA_GEM_IMPORTER 5 /* FIXME: dynamically allocate more than this */

struct tegra_gem_object {
	struct drm_gem_object *gem;
	struct tegra_gem_importer importer[NUM_TEGRA_GEM_IMPORTER];
	struct mutex lock;
};

static inline struct tegra_bo *host1x_to_tegra_bo(struct host1x_bo *bo)
{
	return container_of(bo, struct tegra_bo, base);
}

static void tegra_bo_put(struct host1x_bo *bo)
{
	struct tegra_bo *obj = host1x_to_tegra_bo(bo);
	struct drm_device *drm = obj->gem.dev;

	mutex_lock(&drm->struct_mutex);
	drm_gem_object_unreference(&obj->gem);
	mutex_unlock(&drm->struct_mutex);
}

static dma_addr_t tegra_bo_pin(struct host1x_bo *bo, struct sg_table **sgt)
{
	struct tegra_bo *obj = host1x_to_tegra_bo(bo);

	return obj->paddr;
}

static void tegra_bo_unpin(struct host1x_bo *bo, struct sg_table *sgt)
{
}

static void *tegra_bo_mmap(struct host1x_bo *bo)
{
	struct tegra_bo *obj = host1x_to_tegra_bo(bo);

	return obj->vaddr;
}

static void tegra_bo_munmap(struct host1x_bo *bo, void *addr)
{
}

static void *tegra_bo_kmap(struct host1x_bo *bo, unsigned int page)
{
	struct tegra_bo *obj = host1x_to_tegra_bo(bo);

	if (page >= obj->num_pages) {
		WARN(1, "kmap pages beyonds bo's size: (%u : %lu).\n",
			page, obj->num_pages);
		return NULL;
	}

	return obj->vaddr + page * PAGE_SIZE;
}

static void tegra_bo_kunmap(struct host1x_bo *bo, unsigned int page,
			    void *addr)
{
}

static struct host1x_bo *tegra_bo_get(struct host1x_bo *bo)
{
	struct tegra_bo *obj = host1x_to_tegra_bo(bo);
	struct drm_device *drm = obj->gem.dev;

	mutex_lock(&drm->struct_mutex);
	drm_gem_object_reference(&obj->gem);
	mutex_unlock(&drm->struct_mutex);

	return bo;
}

static const struct host1x_bo_ops tegra_bo_ops = {
	.get = tegra_bo_get,
	.put = tegra_bo_put,
	.pin = tegra_bo_pin,
	.unpin = tegra_bo_unpin,
	.mmap = tegra_bo_mmap,
	.munmap = tegra_bo_munmap,
	.kmap = tegra_bo_kmap,
	.kunmap = tegra_bo_kunmap,
};

/*
 * A generic iommu_map_sg() function is being reviewed and will hopefully be
 * merged soon. At that point this function can be dropped in favour of the
 * one provided by the IOMMU API.
 */
static ssize_t __iommu_map_sg(struct iommu_domain *domain, unsigned long iova,
			      struct scatterlist *sg, unsigned int nents,
			      int prot)
{
	struct scatterlist *s;
	size_t offset = 0;
	unsigned int i;
	int err;

	for_each_sg(sg, s, nents, i) {
		phys_addr_t phys = page_to_phys(sg_page(s));
		size_t length = s->offset + s->length;

		err = iommu_map(domain, iova + offset, phys, length, prot);
		if (err < 0) {
			iommu_unmap(domain, iova, offset);
			return err;
		}

		offset += length;
	}

	return offset;
}

static int tegra_bo_iommu_map(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	int prot = IOMMU_READ | IOMMU_WRITE;
	ssize_t err;

	if (bo->mm)
		return -EBUSY;

	bo->mm = kzalloc(sizeof(*bo->mm), GFP_KERNEL);
	if (!bo->mm)
		return -ENOMEM;

	err = drm_mm_insert_node_generic(&tegra->mm, bo->mm, bo->gem.size,
					 PAGE_SIZE, 0, 0);
	if (err < 0) {
		dev_err(tegra->drm->dev, "out of I/O virtual memory: %zd\n",
			err);
		goto free;
	}

	bo->paddr = bo->mm->start;

	err = __iommu_map_sg(tegra->domain, bo->paddr, bo->sgt->sgl,
			     bo->sgt->nents, prot);
	if (err < 0) {
		dev_err(tegra->drm->dev, "failed to map buffer: %zd\n", err);
		goto remove;
	}

	bo->size = err;

	return 0;

remove:
	drm_mm_remove_node(bo->mm);
free:
	kfree(bo->mm);
	return err;
}

static int tegra_bo_iommu_unmap(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	if (!bo->mm)
		return 0;

	iommu_unmap(tegra->domain, bo->paddr, bo->size);
	drm_mm_remove_node(bo->mm);
	kfree(bo->mm);

	return 0;
}

static struct tegra_bo *tegra_bo_alloc_object(struct drm_device *drm,
					      size_t size)
{
	struct tegra_bo *bo;
	int err;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	host1x_bo_init(&bo->base, &tegra_bo_ops);
	size = round_up(size, PAGE_SIZE);
	if (!size) {
		err = -EINVAL;
		goto free;
	}
	err = drm_gem_object_init(drm, &bo->gem, size);
	if (err < 0)
		goto free;

	err = drm_gem_create_mmap_offset(&bo->gem);
	if (err < 0)
		goto release;

	return bo;

release:
	drm_gem_object_release(&bo->gem);
free:
	kfree(bo);
	return ERR_PTR(err);
}

static void tegra_bo_free(struct drm_device *drm, struct tegra_bo *bo)
{
	if (bo->pages) {
		drm_gem_put_pages(&bo->gem, bo->pages, true, true);
		sg_free_table(bo->sgt);
		kfree(bo->sgt);
	} else {
		dma_free_writecombine(drm->dev, bo->gem.size, bo->vaddr,
				      bo->paddr);
	}
}

static int tegra_bo_get_pages(struct drm_device *drm, struct tegra_bo *bo,
			      size_t size)
{
	struct scatterlist *s;
	unsigned int i;

	bo->pages = drm_gem_get_pages(&bo->gem, GFP_KERNEL);
	if (IS_ERR(bo->pages))
		return PTR_ERR(bo->pages);

	bo->num_pages = size >> PAGE_SHIFT;

	bo->sgt = drm_prime_pages_to_sg(bo->pages, bo->num_pages);
	if (IS_ERR(bo->sgt))
		goto put_pages;

	/*
	 * Fake up the SG table so that dma_sync_sg_for_device() can be used
	 * to flush the pages associated with it.
	 *
	 * TODO: Replace this by drm_clflash_sg() once it can be implemented
	 * without relying on symbols that are not exported.
	 */
	for_each_sg(bo->sgt->sgl, s, bo->sgt->nents, i)
		sg_dma_address(s) = sg_phys(s);


	dma_sync_sg_for_device(drm->dev, bo->sgt->sgl, bo->sgt->nents,
			       DMA_TO_DEVICE);

	return 0;

put_pages:
	drm_gem_put_pages(&bo->gem, bo->pages, false, false);
	return PTR_ERR(bo->sgt);
}

static int tegra_bo_alloc(struct drm_device *drm, struct tegra_bo *bo,
			  size_t size)
{
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	if (tegra->domain) {
		err = tegra_bo_get_pages(drm, bo, size);
		if (err < 0)
			return err;

		err = tegra_bo_iommu_map(tegra, bo);
		if (err < 0) {
			tegra_bo_free(drm, bo);
			return err;
		}
	} else {
		bo->vaddr = dma_alloc_writecombine(drm->dev, size, &bo->paddr,
						   GFP_KERNEL | __GFP_NOWARN);
		if (!bo->vaddr) {
			dev_err(drm->dev,
				"failed to allocate buffer of size %zu\n",
				size);
			return -ENOMEM;
		}
	}

	return 0;
}

struct tegra_bo *tegra_bo_create(struct drm_device *drm, unsigned int size,
				 unsigned long flags)
{
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_alloc_object(drm, size);
	if (IS_ERR(bo))
		return bo;

	err = tegra_bo_alloc(drm, bo, size);
	if (err < 0)
		goto release;

	if (flags & DRM_TEGRA_GEM_CREATE_TILED)
		bo->tiling.mode = TEGRA_BO_TILING_MODE_TILED;

	if (flags & DRM_TEGRA_GEM_CREATE_BOTTOM_UP)
		bo->flags |= TEGRA_BO_BOTTOM_UP;

	return bo;

release:
	drm_gem_object_release(&bo->gem);
	kfree(bo);
	return ERR_PTR(err);
}

struct tegra_bo *tegra_bo_create_with_handle(struct drm_file *file,
					     struct drm_device *drm,
					     unsigned int size,
					     unsigned long flags,
					     unsigned int *handle)
{
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_create(drm, size, flags);
	if (IS_ERR(bo))
		return bo;

	err = drm_gem_handle_create(file, &bo->gem, handle);
	if (err) {
		tegra_bo_free_object(&bo->gem);
		return ERR_PTR(err);
	}

	drm_gem_object_unreference_unlocked(&bo->gem);

	return bo;
}

static struct tegra_bo *tegra_bo_import(struct drm_device *drm,
					struct dma_buf *buf)
{
	struct tegra_drm *tegra = drm->dev_private;
	struct dma_buf_attachment *attach;
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_alloc_object(drm, buf->size);
	if (IS_ERR(bo))
		return bo;

	attach = dma_buf_attach(buf, drm->dev);
	if (IS_ERR(attach)) {
		err = PTR_ERR(attach);
		goto free;
	}

	get_dma_buf(buf);

	bo->sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE);
	if (!bo->sgt) {
		err = -ENOMEM;
		goto detach;
	}

	if (IS_ERR(bo->sgt)) {
		err = PTR_ERR(bo->sgt);
		goto detach;
	}

	if (tegra->domain) {
		err = tegra_bo_iommu_map(tegra, bo);
		if (err < 0)
			goto detach;
	} else {
		if (bo->sgt->nents > 1) {
			err = -EINVAL;
			goto detach;
		}

		bo->paddr = sg_dma_address(bo->sgt->sgl);
	}

	bo->gem.import_attach = attach;

	return bo;

detach:
	if (!IS_ERR_OR_NULL(bo->sgt))
		dma_buf_unmap_attachment(attach, bo->sgt, DMA_TO_DEVICE);

	dma_buf_detach(buf, attach);
	dma_buf_put(buf);
free:
	drm_gem_object_release(&bo->gem);
	kfree(bo);
	return ERR_PTR(err);
}

void tegra_bo_free_object(struct drm_gem_object *gem)
{
	struct tegra_drm *tegra = gem->dev->dev_private;
	struct tegra_bo *bo = to_tegra_bo(gem);

	if (tegra->domain)
		tegra_bo_iommu_unmap(tegra, bo);

	if (gem->import_attach) {
		dma_buf_unmap_attachment(gem->import_attach, bo->sgt,
					 DMA_TO_DEVICE);
		drm_prime_gem_destroy(gem, NULL);
	} else {
		tegra_bo_free(gem->dev, bo);
	}

	drm_gem_free_mmap_offset(&bo->gem);
	drm_gem_object_release(gem);
	kfree(bo);
}

int tegra_bo_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_bo *bo;

	min_pitch = round_up(min_pitch, tegra->pitch_align);
	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	if (args->size == 0)
		return -ENOMEM;

	bo = tegra_bo_create_with_handle(file, drm, args->size, 0,
					 &args->handle);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	return 0;
}

int tegra_bo_dumb_map_offset(struct drm_file *file, struct drm_device *drm,
			     uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	mutex_lock(&drm->struct_mutex);

	gem = drm_gem_object_lookup(drm, file, handle);
	if (!gem) {
		dev_err(drm->dev, "failed to lookup GEM object\n");
		mutex_unlock(&drm->struct_mutex);
		return -EINVAL;
	}

	bo = to_tegra_bo(gem);

	*offset = drm_vma_node_offset_addr(&bo->gem.vma_node);

	drm_gem_object_unreference(gem);

	mutex_unlock(&drm->struct_mutex);

	return 0;
}

static int tegra_bo_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *gem = vma->vm_private_data;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct page *page;
	pgoff_t offset;
	int err;

	if (!bo->pages)
		return VM_FAULT_SIGBUS;

	offset = ((unsigned long)vmf->virtual_address - vma->vm_start) >> PAGE_SHIFT;
	page = bo->pages[offset];

	err = vm_insert_page(vma, (unsigned long)vmf->virtual_address, page);
	switch (err) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		return VM_FAULT_NOPAGE;

	case -ENOMEM:
		return VM_FAULT_OOM;
	}

	return VM_FAULT_SIGBUS;
}

const struct vm_operations_struct tegra_bo_vm_ops = {
	.fault = tegra_bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

int tegra_drm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	int ret;

	ret = drm_gem_mmap(file, vma);
	if (ret)
		return ret;

	gem = vma->vm_private_data;
	bo = to_tegra_bo(gem);

	if (!bo->pages) {
		unsigned long vm_pgoff = vma->vm_pgoff;

		vma->vm_flags &= ~VM_PFNMAP;
		vma->vm_pgoff = 0;

		ret = dma_mmap_writecombine(gem->dev->dev, vma, bo->vaddr,
					    bo->paddr, gem->size);
		if (ret) {
			drm_gem_vm_close(vma);
			return ret;
		}

		vma->vm_pgoff = vm_pgoff;
	} else {
		pgprot_t prot = vm_get_page_prot(vma->vm_flags);

		vma->vm_flags |= VM_MIXEDMAP;
		vma->vm_flags &= ~VM_PFNMAP;

		vma->vm_page_prot = pgprot_writecombine(prot);
	}

	return 0;
}

static struct sg_table *
tegra_gem_prime_map_dma_buf(struct dma_buf_attachment *attach,
			    enum dma_data_direction dir)
{
	struct tegra_gem_object *priv = attach->dmabuf->priv;
	struct drm_gem_object *gem = priv->gem;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct sg_table *sgt;

	if (bo->pages) {
		sgt = drm_prime_pages_to_sg(bo->pages, bo->num_pages);

		if (dma_map_sg(attach->dev, sgt->sgl, sgt->nents, dir) == 0)
			goto free;
	} else {
		sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
		if (!sgt)
			return NULL;

		if (sg_alloc_table(sgt, 1, GFP_KERNEL))
			goto free;

		sg_dma_address(sgt->sgl) = bo->paddr;
		sg_dma_len(sgt->sgl) = gem->size;
	}

	return sgt;

free:
	sg_free_table(sgt);
	kfree(sgt);
	return NULL;
}

static void tegra_gem_prime_unmap_dma_buf(struct dma_buf_attachment *attach,
					  struct sg_table *sgt,
					  enum dma_data_direction dir)
{
	struct tegra_gem_object *priv = attach->dmabuf->priv;
	struct drm_gem_object *gem = priv->gem;
	struct tegra_bo *bo = to_tegra_bo(gem);

	if (bo->pages)
		dma_unmap_sg(attach->dev, sgt->sgl, sgt->nents, dir);

	sg_free_table(sgt);
	kfree(sgt);
}

static void tegra_gem_prime_release(struct dma_buf *buf)
{
	struct tegra_gem_object *priv = buf->priv;
	int i;

	for (i = 0; i < ARRAY_SIZE(priv->importer); i++) {
		struct tegra_gem_importer *imp = &priv->importer[i];

		if (imp->dev && imp->delete)
			imp->delete(imp->drvdata);
	}

	drm_gem_object_unreference_unlocked(priv->gem);

	kfree(priv);
}

static void *tegra_gem_prime_kmap_atomic(struct dma_buf *buf,
					 unsigned long page)
{
	return NULL;
}

static void tegra_gem_prime_kunmap_atomic(struct dma_buf *buf,
					  unsigned long page,
					  void *addr)
{
}

static void *tegra_gem_prime_kmap(struct dma_buf *buf, unsigned long page)
{
	return NULL;
}

static void tegra_gem_prime_kunmap(struct dma_buf *buf, unsigned long page,
				   void *addr)
{
}

static int tegra_gem_prime_mmap(struct dma_buf *buf, struct vm_area_struct *vma)
{
	struct tegra_gem_object *obj = buf->priv;
	int ret;

	if (obj->gem->size < vma->vm_end - vma->vm_start)
		return -EINVAL;

	if (!obj->gem->filp)
		return -ENODEV;

	ret = obj->gem->filp->f_op->mmap(obj->gem->filp, vma);
	if (ret)
		return ret;

	fput(vma->vm_file);
	vma->vm_file = get_file(obj->gem->filp);

	return 0;
}

static void *tegra_gem_prime_vmap(struct dma_buf *buf)
{
	struct tegra_gem_object *priv = buf->priv;
	struct drm_gem_object *gem = priv->gem;
	struct tegra_bo *bo = to_tegra_bo(gem);

	return bo->vaddr;
}

static void tegra_gem_prime_vunmap(struct dma_buf *buf, void *vaddr)
{
}


static int tegra_gem_set_drvdata(struct dma_buf *dmabuf, struct device *dev,
		void *drvdata, void (*delete)(void *))
{
	struct tegra_gem_object *priv = dmabuf->priv;
	struct tegra_gem_importer *imp;
	int i, empty = -1, err = 0;

	mutex_lock(&priv->lock);
	for (i = 0; i < ARRAY_SIZE(priv->importer); i++) {
		imp = &priv->importer[i];
		if ((empty == -1) && !imp->dev)
			empty = i;

		if (dev == imp->dev)
			break;
	}

	if (i == ARRAY_SIZE(priv->importer)) {
		if (empty == -1) {
			WARN(1, "tegra_gem: Needs more importer space\n");
			err = -ENOMEM;
			goto out;
		}
		imp = &priv->importer[empty];
		i = empty;
	}

	imp->dev = dev;
	imp->drvdata = drvdata;
	imp->delete = delete;
out:
	mutex_unlock(&priv->lock);
	dev_dbg(dev, "%s() dmabuf=%p err=%d i=%d priv=%p\n",
		__func__, dmabuf, err, i, priv);
	return err;
}

static void *tegra_gem_get_drvdata(struct dma_buf *dmabuf,
		struct device *dev)
{
	struct tegra_gem_object *priv = dmabuf->priv;
	void *drvdata = NULL;
	int i;

	mutex_lock(&priv->lock);
	for (i = 0; i < ARRAY_SIZE(priv->importer); i++) {
		struct tegra_gem_importer *imp;

		imp = &priv->importer[i];
		if (dev == imp->dev) {
			drvdata = imp->drvdata;
			break;
		}
	}
	mutex_unlock(&priv->lock);
	dev_dbg(dev, "%s() dmabuf=%p i=%d priv=%p\n",
		__func__, dmabuf, i, priv);
	return drvdata;
}

static const struct dma_buf_ops tegra_gem_prime_dmabuf_ops = {
	.map_dma_buf = tegra_gem_prime_map_dma_buf,
	.unmap_dma_buf = tegra_gem_prime_unmap_dma_buf,
	.release = tegra_gem_prime_release,
	.kmap_atomic = tegra_gem_prime_kmap_atomic,
	.kunmap_atomic = tegra_gem_prime_kunmap_atomic,
	.kmap = tegra_gem_prime_kmap,
	.kunmap = tegra_gem_prime_kunmap,
	.mmap = tegra_gem_prime_mmap,
	.vmap = tegra_gem_prime_vmap,
	.vunmap = tegra_gem_prime_vunmap,
	.set_drvdata = tegra_gem_set_drvdata,
	.get_drvdata = tegra_gem_get_drvdata,
};

struct dma_buf *tegra_gem_prime_export(struct drm_device *drm,
				       struct drm_gem_object *gem,
				       int flags)
{
	struct tegra_gem_object *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	priv->gem = gem;
	mutex_init(&priv->lock);
	return dma_buf_export(priv, &tegra_gem_prime_dmabuf_ops, gem->size,
			      flags);
}

struct drm_gem_object *tegra_gem_prime_import(struct drm_device *drm,
					      struct dma_buf *buf)
{
	struct tegra_bo *bo;

	if (buf->ops == &tegra_gem_prime_dmabuf_ops) {
		struct tegra_gem_object *priv = buf->priv;
		struct drm_gem_object *gem = priv->gem;

		if (gem->dev == drm) {
			drm_gem_object_reference(gem);
			return gem;
		}
	}

	bo = tegra_bo_import(drm, buf);
	if (IS_ERR(bo))
		return ERR_CAST(bo);

	return &bo->gem;
}
