/*
 * Copyright 2011 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Dave Airlie
 */

#include <drm/drmP.h>
#include <drm/drm_legacy.h>
#include <linux/dma-buf.h>

#include "nouveau_drv.h"
#include "nouveau_gem.h"

static int nouveau_gem_prime_begin_cpu_access(struct dma_buf *buf,
					      enum dma_data_direction direction)
{
	struct nouveau_bo *bo = nouveau_gem_object(buf->priv);

	nouveau_bo_sync_for_cpu(bo);

	return 0;
}

static int nouveau_gem_prime_end_cpu_access(struct dma_buf *buf,
					    enum dma_data_direction direction)
{
	struct nouveau_bo *bo = nouveau_gem_object(buf->priv);

	nouveau_bo_sync_for_device(bo);

	return 0;
}

static void *nouveau_gem_prime_kmap_atomic(struct dma_buf *buf,
					   unsigned long page)
{
	return NULL;
}

static void nouveau_gem_prime_kunmap_atomic(struct dma_buf *buf,
					    unsigned long page, void *addr)
{
}

static void *nouveau_gem_prime_kmap(struct dma_buf *buf, unsigned long page)
{
	return NULL;
}

static void nouveau_gem_prime_kunmap(struct dma_buf *buf, unsigned long page,
				     void *addr)
{
}

static inline u64 nouveau_bo_mmap_offset(struct nouveau_bo *bo)
{
	return drm_vma_node_offset_addr(&bo->bo.vma_node) >> PAGE_SHIFT;
}

static int nouveau_gem_prime_mmap(struct dma_buf *buf,
				  struct vm_area_struct *vma)
{
	struct nouveau_bo *bo = nouveau_gem_object(buf->priv);
	struct drm_gem_object *obj = buf->priv;
	int ret;

	/* check for valid size */
	if (obj->size < vma->vm_end - vma->vm_start)
		return -EINVAL;

	vma->vm_pgoff += nouveau_bo_mmap_offset(bo);

	if (unlikely(vma->vm_pgoff < DRM_FILE_PAGE_OFFSET))
		return drm_legacy_mmap(vma->vm_file, vma);

	ret = drm_vma_node_allow(&obj->vma_node, vma->vm_file->private_data);
	if (ret)
		return ret;

	ret = ttm_bo_mmap(vma->vm_file, vma, bo->bo.bdev);
	drm_vma_node_revoke(&obj->vma_node, vma->vm_file->private_data);

	return ret;
}

static void *nouveau_gem_prime_vmap(struct dma_buf *buf)
{
	struct nouveau_bo *bo = nouveau_gem_object(buf->priv);
	int ret;

	ret = ttm_bo_kmap(&bo->bo, 0, bo->bo.num_pages, &bo->dma_buf_vmap);
	if (ret)
		return ERR_PTR(ret);

	return bo->dma_buf_vmap.virtual;
}

static void nouveau_gem_prime_vunmap(struct dma_buf *buf, void *vaddr)
{
	struct nouveau_bo *bo = nouveau_gem_object(buf->priv);

	ttm_bo_kunmap(&bo->dma_buf_vmap);
}

static const struct dma_buf_ops nouveau_gem_prime_dmabuf_ops = {
	.attach = drm_gem_map_attach,
	.detach = drm_gem_map_detach,
	.map_dma_buf = drm_gem_map_dma_buf,
	.unmap_dma_buf = drm_gem_unmap_dma_buf,
	.release = drm_gem_dmabuf_release,
	.begin_cpu_access = nouveau_gem_prime_begin_cpu_access,
	.end_cpu_access = nouveau_gem_prime_end_cpu_access,
	.map_atomic = nouveau_gem_prime_kmap_atomic,
	.unmap_atomic = nouveau_gem_prime_kunmap_atomic,
	.map = nouveau_gem_prime_kmap,
	.unmap = nouveau_gem_prime_kunmap,
	.mmap = nouveau_gem_prime_mmap,
	.vmap = nouveau_gem_prime_vmap,
	.vunmap = nouveau_gem_prime_vunmap,
};

struct dma_buf *nouveau_gem_prime_export(struct drm_device *dev,
					 struct drm_gem_object *obj,
					 int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);

	info.exp_name = KBUILD_MODNAME;
	info.owner = dev->driver->fops->owner;
	info.ops = &nouveau_gem_prime_dmabuf_ops;
	info.size = obj->size;
	info.flags = flags;
	info.priv = obj;

	if (dev->driver->gem_prime_res_obj)
		info.resv = dev->driver->gem_prime_res_obj(obj);

	return drm_gem_dmabuf_export(dev, &info);
}

struct drm_gem_object *nouveau_gem_prime_import(struct drm_device *dev,
						struct dma_buf *buf)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct dma_buf_attachment *attach;
	struct drm_gem_object *obj;
	u32 flags = TTM_PL_FLAG_TT;
	struct nouveau_bo *bo;
	struct sg_table *sgt;
	int ret;

	if (buf->ops == &nouveau_gem_prime_dmabuf_ops) {
		obj = buf->priv;

		if (obj->dev == dev) {
			/*
			 * Importing a DMA-BUF exported from our own GEM
			 * increases the reference count on the GEM itself
			 * instead of the f_count of the DMA-BUF.
			 */
			drm_gem_object_get(obj);
			return obj;
		}
	}

	attach = dma_buf_attach(buf, dev->dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(buf);

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto detach;
	}

	ww_mutex_lock(&attach->dmabuf->resv->lock, NULL);
	ret = nouveau_bo_new(&drm->client, attach->dmabuf->size, 0, flags, 0,
			     0, sgt, attach->dmabuf->resv, &bo);
	ww_mutex_unlock(&attach->dmabuf->resv->lock);
	if (ret)
		goto unmap;

	bo->valid_domains = NOUVEAU_GEM_DOMAIN_GART;

	/*
	 * Initialize the embedded GEM object. We return a single GEM reference
	 * to the caller, instead of a normal nouveau_bo TTM reference.
	 */
	ret = drm_gem_object_init(dev, &bo->gem, bo->bo.mem.size);
	if (ret)
		goto unref;

	bo->gem.import_attach = attach;

	return &bo->gem;

unref:
	nouveau_bo_ref(NULL, &bo);
unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(buf, attach);
	dma_buf_put(buf);

	return ERR_PTR(ret);
}

int nouveau_gem_prime_pin(struct drm_gem_object *obj)
{
	struct nouveau_bo *nvbo = nouveau_gem_object(obj);
	int ret;

	/* pin buffer into GTT */
	ret = nouveau_bo_pin(nvbo, TTM_PL_FLAG_TT, false);
	if (ret)
		return -EINVAL;

	return 0;
}

void nouveau_gem_prime_unpin(struct drm_gem_object *obj)
{
	struct nouveau_bo *nvbo = nouveau_gem_object(obj);

	nouveau_bo_unpin(nvbo);
}

struct reservation_object *nouveau_gem_prime_res_obj(struct drm_gem_object *obj)
{
	struct nouveau_bo *nvbo = nouveau_gem_object(obj);

	return nvbo->bo.resv;
}

struct sg_table *nouveau_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct nouveau_bo *nvbo = nouveau_gem_object(obj);
	int npages = nvbo->bo.num_pages;

	return drm_prime_pages_to_sg(nvbo->bo.ttm->pages, npages);
}
