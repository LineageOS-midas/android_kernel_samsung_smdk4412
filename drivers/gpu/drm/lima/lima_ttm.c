// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2018 Qiang Yu <yuq825@gmail.com> */

#include <linux/mm.h>
#include <drm/ttm/ttm_page_alloc.h>

#include "lima_drv.h"
#include "lima_device.h"
#include "lima_object.h"


static int lima_ttm_mem_global_init(struct drm_global_reference *ref)
{
	return ttm_mem_global_init(ref->object);
}

static void lima_ttm_mem_global_release(struct drm_global_reference *ref)
{
	ttm_mem_global_release(ref->object);
}

static int lima_ttm_global_init(struct lima_device *dev)
{
	struct drm_global_reference *global_ref;
	int err;

	dev->mman.mem_global_referenced = false;
	global_ref = &dev->mman.mem_global_ref;
	global_ref->global_type = DRM_GLOBAL_TTM_MEM;
	global_ref->size = sizeof(struct ttm_mem_global);
	global_ref->init = &lima_ttm_mem_global_init;
	global_ref->release = &lima_ttm_mem_global_release;

	err = drm_global_item_ref(global_ref);
	if (err != 0) {
		dev_err(dev->dev, "Failed setting up TTM memory accounting "
			"subsystem.\n");
		return err;
	}

	dev->mman.bo_global_ref.mem_glob =
		dev->mman.mem_global_ref.object;
	global_ref = &dev->mman.bo_global_ref.ref;
	global_ref->global_type = DRM_GLOBAL_TTM_BO;
	global_ref->size = sizeof(struct ttm_bo_global);
	global_ref->init = &ttm_bo_global_init;
	global_ref->release = &ttm_bo_global_release;
	err = drm_global_item_ref(global_ref);
	if (err != 0) {
		dev_err(dev->dev, "Failed setting up TTM BO subsystem.\n");
		drm_global_item_unref(&dev->mman.mem_global_ref);
		return err;
	}

	dev->mman.mem_global_referenced = true;
	return 0;
}

static void lima_ttm_global_fini(struct lima_device *dev)
{
	if (dev->mman.mem_global_referenced) {
		drm_global_item_unref(&dev->mman.bo_global_ref.ref);
		drm_global_item_unref(&dev->mman.mem_global_ref);
		dev->mman.mem_global_referenced = false;
	}
}

struct lima_tt_mgr {
	spinlock_t lock;
	unsigned long available;
};

static int lima_ttm_bo_man_init(struct ttm_mem_type_manager *man,
				unsigned long p_size)
{
	struct lima_tt_mgr *mgr;

	mgr = kmalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return -ENOMEM;

	spin_lock_init(&mgr->lock);
	mgr->available = p_size;
	man->priv = mgr;
	return 0;
}

static int lima_ttm_bo_man_takedown(struct ttm_mem_type_manager *man)
{
	struct lima_tt_mgr *mgr = man->priv;

	kfree(mgr);
	man->priv = NULL;
	return 0;
}

static int lima_ttm_bo_man_get_node(struct ttm_mem_type_manager *man,
				    struct ttm_buffer_object *bo,
				    const struct ttm_place *place,
				    struct ttm_mem_reg *mem)
{
	struct lima_tt_mgr *mgr = man->priv;

	/* don't exceed the mem limit */
	spin_lock(&mgr->lock);
	if (mgr->available < mem->num_pages) {
		spin_unlock(&mgr->lock);
		return 0;
	}
	mgr->available -= mem->num_pages;
	spin_unlock(&mgr->lock);

	/* just fake a non-null pointer to tell caller success */
	mem->mm_node = (void *)1;
	return 0;
}

static void lima_ttm_bo_man_put_node(struct ttm_mem_type_manager *man,
				     struct ttm_mem_reg *mem)
{
	struct lima_tt_mgr *mgr = man->priv;

	spin_lock(&mgr->lock);
	mgr->available += mem->num_pages;
	spin_unlock(&mgr->lock);

	mem->mm_node = NULL;
}

static void lima_ttm_bo_man_debug(struct ttm_mem_type_manager *man,
				  struct drm_printer *printer)
{
}

static const struct ttm_mem_type_manager_func lima_bo_manager_func = {
	.init = lima_ttm_bo_man_init,
	.takedown = lima_ttm_bo_man_takedown,
	.get_node = lima_ttm_bo_man_get_node,
	.put_node = lima_ttm_bo_man_put_node,
	.debug = lima_ttm_bo_man_debug
};

static int lima_init_mem_type(struct ttm_bo_device *bdev, uint32_t type,
			      struct ttm_mem_type_manager *man)
{
	struct lima_device *dev = ttm_to_lima_dev(bdev);

	switch (type) {
	case TTM_PL_SYSTEM:
		/* System memory */
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		break;
	case TTM_PL_TT:
		man->func = &lima_bo_manager_func;
		man->flags = TTM_MEMTYPE_FLAG_MAPPABLE;
		man->available_caching = TTM_PL_MASK_CACHING;
		man->default_caching = TTM_PL_FLAG_CACHED;
		break;
	default:
		dev_err(dev->dev, "Unsupported memory type %u\n",
			(unsigned int)type);
		return -EINVAL;
	}
	return 0;
}

static int lima_ttm_backend_bind(struct ttm_tt *ttm,
				 struct ttm_mem_reg *bo_mem)
{
	return 0;
}

static int lima_ttm_backend_unbind(struct ttm_tt *ttm)
{
	return 0;
}

static void lima_ttm_backend_destroy(struct ttm_tt *ttm)
{
	struct lima_ttm_tt *tt = (void *)ttm;

	ttm_dma_tt_fini(&tt->ttm);
	kfree(tt);
}

static struct ttm_backend_func lima_ttm_backend_func = {
	.bind = &lima_ttm_backend_bind,
	.unbind = &lima_ttm_backend_unbind,
	.destroy = &lima_ttm_backend_destroy,
};

static struct ttm_tt *lima_ttm_tt_create(struct ttm_buffer_object *bo,
					 uint32_t page_flags)
{
	struct lima_ttm_tt *tt;

	tt = kzalloc(sizeof(struct lima_ttm_tt), GFP_KERNEL);
	if (tt == NULL)
		return NULL;

	tt->ttm.ttm.func = &lima_ttm_backend_func;

	if (ttm_sg_tt_init(&tt->ttm, bo, page_flags)) {
		kfree(tt);
		return NULL;
	}

	return &tt->ttm.ttm;
}

static int lima_ttm_tt_populate(struct ttm_tt *ttm,
				struct ttm_operation_ctx *ctx)
{
	struct lima_device *dev = ttm_to_lima_dev(ttm->bdev);
	struct lima_ttm_tt *tt = (void *)ttm;
	bool slave = !!(ttm->page_flags & TTM_PAGE_FLAG_SG);

	if (slave) {
		drm_prime_sg_to_page_addr_arrays(ttm->sg, ttm->pages,
						 tt->ttm.dma_address,
						 ttm->num_pages);
		ttm->state = tt_unbound;
		return 0;
	}

	return ttm_populate_and_map_pages(dev->dev, &tt->ttm, ctx);
}

static void lima_ttm_tt_unpopulate(struct ttm_tt *ttm)
{
	struct lima_device *dev = ttm_to_lima_dev(ttm->bdev);
	struct lima_ttm_tt *tt = (void *)ttm;
	bool slave = !!(ttm->page_flags & TTM_PAGE_FLAG_SG);

	if (slave)
		return;

	ttm_unmap_and_unpopulate_pages(dev->dev, &tt->ttm);
}

static int lima_invalidate_caches(struct ttm_bo_device *bdev,
				  uint32_t flags)
{
	struct lima_device *dev = ttm_to_lima_dev(bdev);

	dev_err(dev->dev, "%s not implemented\n", __FUNCTION__);
	return 0;
}

static void lima_evict_flags(struct ttm_buffer_object *tbo,
			     struct ttm_placement *placement)
{
	struct lima_bo *bo = ttm_to_lima_bo(tbo);
	struct lima_device *dev = to_lima_dev(bo->gem.dev);

	dev_err(dev->dev, "%s not implemented\n", __FUNCTION__);
}

static int lima_verify_access(struct ttm_buffer_object *tbo,
			      struct file *filp)
{
	struct lima_bo *bo = ttm_to_lima_bo(tbo);

	return drm_vma_node_verify_access(&bo->gem.vma_node,
					  filp->private_data);
}

static int lima_ttm_io_mem_reserve(struct ttm_bo_device *bdev,
				   struct ttm_mem_reg *mem)
{
	struct ttm_mem_type_manager *man = &bdev->man[mem->mem_type];

	mem->bus.addr = NULL;
	mem->bus.offset = 0;
	mem->bus.size = mem->num_pages << PAGE_SHIFT;
	mem->bus.base = 0;
	mem->bus.is_iomem = false;

	if (!(man->flags & TTM_MEMTYPE_FLAG_MAPPABLE))
		return -EINVAL;

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
	case TTM_PL_TT:
		return 0;
	default:
		return -EINVAL;
	}
	return 0;
}

static void lima_ttm_io_mem_free(struct ttm_bo_device *bdev,
				 struct ttm_mem_reg *mem)
{

}

static void lima_bo_move_notify(struct ttm_buffer_object *tbo, bool evict,
				struct ttm_mem_reg *new_mem)
{
	struct lima_bo *bo = ttm_to_lima_bo(tbo);
	struct lima_device *dev = to_lima_dev(bo->gem.dev);

	if (evict)
		dev_err(dev->dev, "%s not implemented\n", __FUNCTION__);
}

static void lima_bo_swap_notify(struct ttm_buffer_object *tbo)
{
	struct lima_bo *bo = ttm_to_lima_bo(tbo);
	struct lima_device *dev = to_lima_dev(bo->gem.dev);

	dev_err(dev->dev, "%s not implemented\n", __FUNCTION__);
}

static struct ttm_bo_driver lima_bo_driver = {
	.ttm_tt_create = lima_ttm_tt_create,
	.ttm_tt_populate = lima_ttm_tt_populate,
	.ttm_tt_unpopulate = lima_ttm_tt_unpopulate,
	.invalidate_caches = lima_invalidate_caches,
	.init_mem_type = lima_init_mem_type,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = lima_evict_flags,
	.verify_access = lima_verify_access,
	.io_mem_reserve = lima_ttm_io_mem_reserve,
	.io_mem_free = lima_ttm_io_mem_free,
	.move_notify = lima_bo_move_notify,
	.swap_notify = lima_bo_swap_notify,
};

int lima_ttm_init(struct lima_device *dev)
{
	int err;
	bool need_dma32;
	u64 gtt_size;

	err = lima_ttm_global_init(dev);
	if (err)
		return err;

#if defined(CONFIG_ARM) && !defined(CONFIG_ARM_LPAE)
	need_dma32 = false;
#else
	need_dma32 = true;
#endif

	err = ttm_bo_device_init(&dev->mman.bdev,
				 dev->mman.bo_global_ref.ref.object,
				 &lima_bo_driver,
				 dev->ddev->anon_inode->i_mapping,
				 DRM_FILE_PAGE_OFFSET,
				 need_dma32);
	if (err) {
		dev_err(dev->dev, "failed initializing buffer object "
			"driver(%d).\n", err);
		goto err_out0;
	}

	if (lima_max_mem < 0) {
		struct sysinfo si;
		si_meminfo(&si);
		/* TODO: better to have lower 32 mem size */
		gtt_size = min(((u64)si.totalram * si.mem_unit * 3/4),
			       0x100000000ULL);
	}
	else
		gtt_size = (u64)lima_max_mem << 20;

	err = ttm_bo_init_mm(&dev->mman.bdev, TTM_PL_TT, gtt_size >> PAGE_SHIFT);
	if (err) {
		dev_err(dev->dev, "Failed initializing GTT heap.\n");
		goto err_out1;
	}
	return 0;

err_out1:
	ttm_bo_device_release(&dev->mman.bdev);
err_out0:
	lima_ttm_global_fini(dev);
	return err;
}

void lima_ttm_fini(struct lima_device *dev)
{
	ttm_bo_device_release(&dev->mman.bdev);
	lima_ttm_global_fini(dev);
	dev_info(dev->dev, "ttm finalized\n");
}
