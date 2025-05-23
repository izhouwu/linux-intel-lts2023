/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Authors:
 *    Dave Airlie
 *    Alon Levy
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
 */

#include <linux/file.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>
#include <drm/virtgpu_drm.h>

#include "virtgpu_drv.h"

#define VIRTGPU_BLOB_FLAG_USE_MASK (VIRTGPU_BLOB_FLAG_USE_MAPPABLE | \
				    VIRTGPU_BLOB_FLAG_USE_SHAREABLE | \
				    VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE)

/* Must be called with &virtio_gpu_fpriv.struct_mutex held. */
static void virtio_gpu_create_context_locked(struct virtio_gpu_device *vgdev,
					     struct virtio_gpu_fpriv *vfpriv)
{
	char dbgname[TASK_COMM_LEN];

	get_task_comm(dbgname, current);
	virtio_gpu_cmd_context_create(vgdev, vfpriv->ctx_id,
				      vfpriv->context_init, strlen(dbgname),
				      dbgname);

	vfpriv->context_created = true;
}

void virtio_gpu_create_context(struct drm_device *dev, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;

	mutex_lock(&vfpriv->context_lock);
	if (vfpriv->context_created)
		goto out_unlock;

	virtio_gpu_create_context_locked(vgdev, vfpriv);

out_unlock:
	mutex_unlock(&vfpriv->context_lock);
}

static int virtio_gpu_map_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_map *virtio_gpu_map = data;

	return virtio_gpu_mode_dumb_mmap(file, vgdev->ddev,
					 virtio_gpu_map->handle,
					 &virtio_gpu_map->offset);
}

static int virtio_gpu_getparam_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_getparam *param = data;
	int value;

	switch (param->param) {
	case VIRTGPU_PARAM_3D_FEATURES:
		value = vgdev->has_virgl_3d ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
		value = 1;
		break;
	case VIRTGPU_PARAM_RESOURCE_BLOB:
		value = vgdev->has_resource_blob ? 1 : 0;
		break;
	case VIRTGPU_PARAM_HOST_VISIBLE:
		value = vgdev->has_host_visible ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CROSS_DEVICE:
		value = vgdev->has_resource_assign_uuid ? 1 : 0;
		break;
	case VIRTGPU_PARAM_CONTEXT_INIT:
		value = vgdev->has_context_init ? 1 : 0;
		break;
	case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
		value = vgdev->capset_id_mask;
		break;
	case VIRTGPU_PARAM_QUERY_DEV:
		value = (strcmp(dev->dev->driver->name, "virtio-pci") == 0) ? 1 : 0;
		break;
	case VIRTGPU_PARAM_ALLOW_P2P:
		value = vgdev->has_allow_p2p ? 1 : 0;
		value = (value & 0xffff) | ((vgdev->output_cap_mask & 0xffff) << 16);
		break;
	default:
		return -EINVAL;
	}
	if (copy_to_user(u64_to_user_ptr(param->value), &value, sizeof(int)))
		return -EFAULT;

	return 0;
}

static int virtio_gpu_resource_create_ioctl(struct drm_device *dev, void *data,
					    struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_resource_create *rc = data;
	struct virtio_gpu_fence *fence;
	int ret;
	struct virtio_gpu_object *qobj;
	struct drm_gem_object *obj;
	uint32_t handle = 0;
	struct virtio_gpu_object_params params = { 0 };

	if (vgdev->has_virgl_3d) {
		virtio_gpu_create_context(dev, file);
		params.virgl = true;
		params.target = rc->target;
		params.bind = rc->bind;
		params.depth = rc->depth;
		params.array_size = rc->array_size;
		params.last_level = rc->last_level;
		params.nr_samples = rc->nr_samples;
		params.flags = rc->flags;
	} else {
		if (rc->depth > 1)
			return -EINVAL;
		if (rc->nr_samples > 1)
			return -EINVAL;
		if (rc->last_level > 1)
			return -EINVAL;
		if (rc->target != 2)
			return -EINVAL;
		if (rc->array_size > 1)
			return -EINVAL;
	}

	params.format = rc->format;
	params.width = rc->width;
	params.height = rc->height;
	params.size = rc->size;
	/* allocate a single page size object */
	if (params.size == 0)
		params.size = PAGE_SIZE;

	fence = virtio_gpu_fence_alloc(vgdev, vgdev->fence_drv.context, 0);
	if (!fence)
		return -ENOMEM;
	ret = virtio_gpu_object_create(vgdev, &params, &qobj, fence);
	dma_fence_put(&fence->f);
	if (ret < 0)
		return ret;
	obj = &qobj->base.base;

	ret = drm_gem_handle_create(file, obj, &handle);
	if (ret) {
		drm_gem_object_release(obj);
		return ret;
	}

	rc->res_handle = qobj->hw_res_handle; /* similiar to a VM address */
	rc->bo_handle = handle;

	/*
	 * The handle owns the reference now.  But we must drop our
	 * remaining reference *after* we no longer need to dereference
	 * the obj.  Otherwise userspace could guess the handle and
	 * race closing it from another thread.
	 */
	drm_gem_object_put(obj);

	return 0;
}

static int virtio_gpu_resource_info_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file)
{
	struct drm_virtgpu_resource_info *ri = data;
	struct drm_gem_object *gobj = NULL;
	struct virtio_gpu_object *qobj = NULL;

	gobj = drm_gem_object_lookup(file, ri->bo_handle);
	if (gobj == NULL)
		return -ENOENT;

	qobj = gem_to_virtio_gpu_obj(gobj);

	ri->size = qobj->base.base.size;
	ri->res_handle = qobj->hw_res_handle;
	if (qobj->host3d_blob || qobj->guest_blob)
		ri->blob_mem = qobj->blob_mem;

	drm_gem_object_put(gobj);
	return 0;
}

static int virtio_gpu_transfer_from_host_ioctl(struct drm_device *dev,
					       void *data,
					       struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct drm_virtgpu_3d_transfer_from_host *args = data;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_object_array *objs;
	struct virtio_gpu_fence *fence;
	int ret;
	u32 offset = args->offset;

	if (vgdev->has_virgl_3d == false)
		return -ENOSYS;

	virtio_gpu_create_context(dev, file);
	objs = virtio_gpu_array_from_handles(file, &args->bo_handle, 1);
	if (objs == NULL)
		return -ENOENT;

	bo = gem_to_virtio_gpu_obj(objs->objs[0]);
	if (bo->guest_blob && !bo->host3d_blob) {
		ret = -EINVAL;
		goto err_put_free;
	}

	if (!bo->host3d_blob && (args->stride || args->layer_stride)) {
		ret = -EINVAL;
		goto err_put_free;
	}

	ret = virtio_gpu_array_lock_resv(objs);
	if (ret != 0)
		goto err_put_free;

	fence = virtio_gpu_fence_alloc(vgdev, vgdev->fence_drv.context, 0);
	if (!fence) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	virtio_gpu_cmd_transfer_from_host_3d
		(vgdev, vfpriv->ctx_id, offset, args->level, args->stride,
		 args->layer_stride, &args->box, objs, fence);
	dma_fence_put(&fence->f);
	virtio_gpu_notify(vgdev);
	return 0;

err_unlock:
	virtio_gpu_array_unlock_resv(objs);
err_put_free:
	virtio_gpu_array_put_free(objs);
	return ret;
}

static int virtio_gpu_transfer_to_host_ioctl(struct drm_device *dev, void *data,
					     struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct drm_virtgpu_3d_transfer_to_host *args = data;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_object_array *objs;
	struct virtio_gpu_fence *fence;
	int ret;
	u32 offset = args->offset;

	objs = virtio_gpu_array_from_handles(file, &args->bo_handle, 1);
	if (objs == NULL)
		return -ENOENT;

	bo = gem_to_virtio_gpu_obj(objs->objs[0]);
	if (bo->guest_blob && !bo->host3d_blob) {
		ret = -EINVAL;
		goto err_put_free;
	}

	if (!vgdev->has_virgl_3d) {
		virtio_gpu_cmd_transfer_to_host_2d
			(vgdev, offset,
			 args->box.w, args->box.h, args->box.x, args->box.y,
			 objs, NULL);
	} else {
		virtio_gpu_create_context(dev, file);

		if (!bo->host3d_blob && (args->stride || args->layer_stride)) {
			ret = -EINVAL;
			goto err_put_free;
		}

		ret = virtio_gpu_array_lock_resv(objs);
		if (ret != 0)
			goto err_put_free;

		ret = -ENOMEM;
		fence = virtio_gpu_fence_alloc(vgdev, vgdev->fence_drv.context,
					       0);
		if (!fence)
			goto err_unlock;

		virtio_gpu_cmd_transfer_to_host_3d
			(vgdev,
			 vfpriv ? vfpriv->ctx_id : 0, offset, args->level,
			 args->stride, args->layer_stride, &args->box, objs,
			 fence);
		dma_fence_put(&fence->f);
	}
	virtio_gpu_notify(vgdev);
	return 0;

err_unlock:
	virtio_gpu_array_unlock_resv(objs);
err_put_free:
	virtio_gpu_array_put_free(objs);
	return ret;
}

static int virtio_gpu_wait_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct drm_virtgpu_3d_wait *args = data;
	struct drm_gem_object *obj;
	long timeout = 15 * HZ;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (obj == NULL)
		return -ENOENT;

	if (args->flags & VIRTGPU_WAIT_NOWAIT) {
		ret = dma_resv_test_signaled(obj->resv, DMA_RESV_USAGE_READ);
	} else {
		ret = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_READ,
					    true, timeout);
	}
	if (ret == 0)
		ret = -EBUSY;
	else if (ret > 0)
		ret = 0;

	drm_gem_object_put(obj);
	return ret;
}

static int virtio_gpu_get_caps_ioctl(struct drm_device *dev,
				void *data, struct drm_file *file)
{
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct drm_virtgpu_get_caps *args = data;
	unsigned size, host_caps_size;
	int i;
	int found_valid = -1;
	int ret;
	struct virtio_gpu_drv_cap_cache *cache_ent;
	void *ptr;

	if (vgdev->num_capsets == 0)
		return -ENOSYS;

	/* don't allow userspace to pass 0 */
	if (args->size == 0)
		return -EINVAL;

	spin_lock(&vgdev->display_info_lock);
	for (i = 0; i < vgdev->num_capsets; i++) {
		if (vgdev->capsets[i].id == args->cap_set_id) {
			if (vgdev->capsets[i].max_version >= args->cap_set_ver) {
				found_valid = i;
				break;
			}
		}
	}

	if (found_valid == -1) {
		spin_unlock(&vgdev->display_info_lock);
		return -EINVAL;
	}

	host_caps_size = vgdev->capsets[found_valid].max_size;
	/* only copy to user the minimum of the host caps size or the guest caps size */
	size = min(args->size, host_caps_size);

	list_for_each_entry(cache_ent, &vgdev->cap_cache, head) {
		if (cache_ent->id == args->cap_set_id &&
		    cache_ent->version == args->cap_set_ver) {
			spin_unlock(&vgdev->display_info_lock);
			goto copy_exit;
		}
	}
	spin_unlock(&vgdev->display_info_lock);

	/* not in cache - need to talk to hw */
	ret = virtio_gpu_cmd_get_capset(vgdev, found_valid, args->cap_set_ver,
					&cache_ent);
	if (ret)
		return ret;
	virtio_gpu_notify(vgdev);

copy_exit:
	ret = wait_event_timeout(vgdev->resp_wq,
				 atomic_read(&cache_ent->is_valid), 5 * HZ);
	if (!ret)
		return -EBUSY;

	/* is_valid check must proceed before copy of the cache entry. */
	smp_rmb();

	ptr = cache_ent->caps_cache;

	if (copy_to_user(u64_to_user_ptr(args->addr), ptr, size))
		return -EFAULT;

	return 0;
}

static int verify_blob(struct virtio_gpu_device *vgdev,
		       struct virtio_gpu_fpriv *vfpriv,
		       struct virtio_gpu_object_params *params,
		       struct drm_virtgpu_resource_create_blob *rc_blob,
		       bool *guest_blob, bool *host3d_blob)
{
	if (!vgdev->has_resource_blob)
		return -EINVAL;

	if (rc_blob->blob_flags & ~VIRTGPU_BLOB_FLAG_USE_MASK)
		return -EINVAL;

	if (rc_blob->blob_flags & VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE) {
		if (!vgdev->has_resource_assign_uuid)
			return -EINVAL;
	}

	switch (rc_blob->blob_mem) {
	case VIRTGPU_BLOB_MEM_GUEST:
		*guest_blob = true;
		break;
	case VIRTGPU_BLOB_MEM_HOST3D_GUEST:
		*guest_blob = true;
		fallthrough;
	case VIRTGPU_BLOB_MEM_HOST3D:
		*host3d_blob = true;
		break;
	default:
		return -EINVAL;
	}

	if (*host3d_blob) {
		if (!vgdev->has_virgl_3d)
			return -EINVAL;

		/* Must be dword aligned. */
		if (rc_blob->cmd_size % 4 != 0)
			return -EINVAL;

		params->ctx_id = vfpriv->ctx_id;
		params->blob_id = rc_blob->blob_id;
	} else {
		if (rc_blob->blob_id != 0)
			return -EINVAL;

		if (rc_blob->cmd_size != 0)
			return -EINVAL;
	}

	params->blob_mem = rc_blob->blob_mem;
	params->size = rc_blob->size;
	params->blob = true;
	params->blob_flags = rc_blob->blob_flags;
	return 0;
}

static int virtio_gpu_resource_create_blob_ioctl(struct drm_device *dev,
						 void *data,
						 struct drm_file *file)
{
	int ret = 0;
	uint32_t handle = 0;
	bool guest_blob = false;
	bool host3d_blob = false;
	struct drm_gem_object *obj;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_object_params params = { 0 };
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct drm_virtgpu_resource_create_blob *rc_blob = data;

	if (verify_blob(vgdev, vfpriv, &params, rc_blob,
			&guest_blob, &host3d_blob))
		return -EINVAL;

	if (vgdev->has_virgl_3d)
		virtio_gpu_create_context(dev, file);

	if (rc_blob->cmd_size) {
		void *buf;

		buf = memdup_user(u64_to_user_ptr(rc_blob->cmd),
				  rc_blob->cmd_size);

		if (IS_ERR(buf))
			return PTR_ERR(buf);

		virtio_gpu_cmd_submit(vgdev, buf, rc_blob->cmd_size,
				      vfpriv->ctx_id, NULL, NULL);
	}

	if (guest_blob)
		ret = virtio_gpu_object_create(vgdev, &params, &bo, NULL);
	else if (!guest_blob && host3d_blob)
		ret = virtio_gpu_vram_create(vgdev, &params, &bo);
	else
		return -EINVAL;

	if (ret < 0)
		return ret;

	bo->guest_blob = guest_blob;
	bo->host3d_blob = host3d_blob;
	bo->blob_mem = rc_blob->blob_mem;
	bo->blob_flags = rc_blob->blob_flags;

	obj = &bo->base.base;
	if (params.blob_flags & VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE) {
		ret = virtio_gpu_resource_assign_uuid(vgdev, bo);
		if (ret) {
			drm_gem_object_release(obj);
			return ret;
		}
	}

	ret = drm_gem_handle_create(file, obj, &handle);
	if (ret) {
		drm_gem_object_release(obj);
		return ret;
	}

	rc_blob->res_handle = bo->hw_res_handle;
	rc_blob->bo_handle = handle;

	/*
	 * The handle owns the reference now.  But we must drop our
	 * remaining reference *after* we no longer need to dereference
	 * the obj.  Otherwise userspace could guess the handle and
	 * race closing it from another thread.
	 */
	drm_gem_object_put(obj);

	return 0;
}

static int virtio_gpu_context_init_ioctl(struct drm_device *dev,
					 void *data, struct drm_file *file)
{
	int ret = 0;
	uint32_t num_params, i, param, value;
	uint64_t valid_ring_mask;
	size_t len;
	struct drm_virtgpu_context_set_param *ctx_set_params = NULL;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	struct virtio_gpu_fpriv *vfpriv = file->driver_priv;
	struct drm_virtgpu_context_init *args = data;

	num_params = args->num_params;
	len = num_params * sizeof(struct drm_virtgpu_context_set_param);

	if (!vgdev->has_context_init || !vgdev->has_virgl_3d)
		return -EINVAL;

	/* Number of unique parameters supported at this time. */
	if (num_params > 3)
		return -EINVAL;

	ctx_set_params = memdup_user(u64_to_user_ptr(args->ctx_set_params),
				     len);

	if (IS_ERR(ctx_set_params))
		return PTR_ERR(ctx_set_params);

	mutex_lock(&vfpriv->context_lock);
	if (vfpriv->context_created) {
		ret = -EEXIST;
		goto out_unlock;
	}

	for (i = 0; i < num_params; i++) {
		param = ctx_set_params[i].param;
		value = ctx_set_params[i].value;

		switch (param) {
		case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
			if (value > MAX_CAPSET_ID) {
				ret = -EINVAL;
				goto out_unlock;
			}

			if ((vgdev->capset_id_mask & (1ULL << value)) == 0) {
				ret = -EINVAL;
				goto out_unlock;
			}

			/* Context capset ID already set */
			if (vfpriv->context_init &
			    VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK) {
				ret = -EINVAL;
				goto out_unlock;
			}

			vfpriv->context_init |= value;
			break;
		case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
			if (vfpriv->base_fence_ctx) {
				ret = -EINVAL;
				goto out_unlock;
			}

			if (value > MAX_RINGS) {
				ret = -EINVAL;
				goto out_unlock;
			}

			vfpriv->base_fence_ctx = dma_fence_context_alloc(value);
			vfpriv->num_rings = value;
			break;
		case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
			if (vfpriv->ring_idx_mask) {
				ret = -EINVAL;
				goto out_unlock;
			}

			vfpriv->ring_idx_mask = value;
			break;
		default:
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	if (vfpriv->ring_idx_mask) {
		valid_ring_mask = 0;
		for (i = 0; i < vfpriv->num_rings; i++)
			valid_ring_mask |= 1ULL << i;

		if (~valid_ring_mask & vfpriv->ring_idx_mask) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	virtio_gpu_create_context_locked(vgdev, vfpriv);
	virtio_gpu_notify(vgdev);

out_unlock:
	mutex_unlock(&vfpriv->context_lock);
	kfree(ctx_set_params);
	return ret;
}

struct drm_ioctl_desc virtio_gpu_ioctls[DRM_VIRTIO_NUM_IOCTLS] = {
	DRM_IOCTL_DEF_DRV(VIRTGPU_MAP, virtio_gpu_map_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_EXECBUFFER, virtio_gpu_execbuffer_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_GETPARAM, virtio_gpu_getparam_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_RESOURCE_CREATE,
			  virtio_gpu_resource_create_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_RESOURCE_INFO, virtio_gpu_resource_info_ioctl,
			  DRM_RENDER_ALLOW),

	/* make transfer async to the main ring? - no sure, can we
	 * thread these in the underlying GL
	 */
	DRM_IOCTL_DEF_DRV(VIRTGPU_TRANSFER_FROM_HOST,
			  virtio_gpu_transfer_from_host_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VIRTGPU_TRANSFER_TO_HOST,
			  virtio_gpu_transfer_to_host_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_WAIT, virtio_gpu_wait_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_GET_CAPS, virtio_gpu_get_caps_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_RESOURCE_CREATE_BLOB,
			  virtio_gpu_resource_create_blob_ioctl,
			  DRM_RENDER_ALLOW),

	DRM_IOCTL_DEF_DRV(VIRTGPU_CONTEXT_INIT, virtio_gpu_context_init_ioctl,
			  DRM_RENDER_ALLOW),
};
