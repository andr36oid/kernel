// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 * Author:Mark Yao <mark.yao@rock-chips.com>
 */

#include <drm/drm.h>
#include <drm/a36-logo-blit.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_probe_helper.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_gem.h"
#include "rockchip_drm_fb.h"
#include "rockchip_drm_fbdev.h"

#define PREFERRED_BPP		32

static int rockchip_fbdev_mmap(struct fb_info *info,
			       struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = info->par;
	struct rockchip_drm_private *private = helper->dev->dev_private;

	return rockchip_gem_mmap_buf(private->fbdev_bo, vma);
}

static const struct fb_ops rockchip_drm_fbdev_ops = {
	.owner		= THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_mmap	= rockchip_fbdev_mmap,
	.fb_fillrect	= drm_fb_helper_cfb_fillrect,
	.fb_copyarea	= drm_fb_helper_cfb_copyarea,
	.fb_imageblit	= drm_fb_helper_cfb_imageblit,
};

/* Called while the fb-helper configures its first BO, before that BO can
 * replace the loader plane. The framebuffer reference pins the reserved pixels
 * throughout the bounded copy. Failure leaves the ordinary cleared BO intact.
 */
static void rockchip_fbdev_seed_logo(struct drm_device *dev,
                                    struct rockchip_gem_object *bo,
                                    const struct drm_mode_fb_cmd2 *cmd)
{
 struct drm_crtc *crtc;
 struct drm_plane_state *ps;
 struct drm_framebuffer *fb;
 struct rockchip_logo *logo;
 struct a36_logo_pixels src, dst;
 unsigned int dbpp;
 int ret;

 if(cmd->pixel_format==DRM_FORMAT_XRGB8888)dbpp=32;
 else if(cmd->pixel_format==DRM_FORMAT_RGB888)dbpp=24;
 else if(cmd->pixel_format==DRM_FORMAT_RGB565)dbpp=16;
 else return;
 drm_for_each_crtc(crtc,dev) {
  ps=crtc->primary ? crtc->primary->state:NULL;
  fb=ps ? ps->fb:NULL;
  if(!fb || !(fb->flags & ROCKCHIP_DRM_MODE_LOGO_FB) ||
     !crtc->state->active || to_rockchip_crtc_state(crtc->state)->panel_prepare_error)
   continue;
  drm_framebuffer_get(fb);
  logo=to_rockchip_logo_fb(fb)->logo;
  src=(struct a36_logo_pixels){logo->kvaddr,logo->size,fb->width,
       fb->height,fb->pitches[0],fb->offsets[0],0};
  if(fb->format->format==DRM_FORMAT_RGB565)src.bpp=16;
  else if(fb->format->format==DRM_FORMAT_RGB888)src.bpp=24;
  else if(fb->format->format==DRM_FORMAT_XRGB8888)src.bpp=32;
  dst=(struct a36_logo_pixels){bo->kvaddr,bo->base.size,cmd->width,
       cmd->height,cmd->pitches[0],0,dbpp};
  ret=a36_logo_blit(&dst,&src,ps->crtc_x,ps->crtc_y,ps->crtc_w,ps->crtc_h,
      ps->src_x>>16,ps->src_y>>16,ps->src_w>>16,ps->src_h>>16,
      !!(ps->rotation & DRM_MODE_REFLECT_Y));
  drm_framebuffer_put(fb);
  if(!ret) {
   dev_info(dev->dev,"LOGO32: retained pixels seeded into fbdev BO\n");
   return;
  }
  dev_warn(dev->dev,"LOGO32: logo copy rejected; no out-of-bounds read\n");
 }
}

static int rockchip_drm_fbdev_create(struct drm_fb_helper *helper,
				     struct drm_fb_helper_surface_size *sizes)
{
	struct rockchip_drm_private *private = helper->dev->dev_private;
	struct drm_mode_fb_cmd2 mode_cmd = { 0 };
	struct drm_device *dev = helper->dev;
	struct rockchip_gem_object *rk_obj;
	struct drm_framebuffer *fb;
	unsigned int bytes_per_pixel;
	unsigned long offset;
	struct fb_info *fbi;
	size_t size;
	int ret;

	bytes_per_pixel = DIV_ROUND_UP(sizes->surface_bpp, 8);

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;
	mode_cmd.pitches[0] = sizes->surface_width * bytes_per_pixel;
	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
		sizes->surface_depth);

	size = mode_cmd.pitches[0] * mode_cmd.height;

	rk_obj = rockchip_gem_create_object(dev, size, true, 0);
	if (IS_ERR(rk_obj))
		return -ENOMEM;

	private->fbdev_bo = &rk_obj->base;
	rockchip_fbdev_seed_logo(dev, rk_obj, &mode_cmd);

	fbi = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(fbi)) {
		DRM_DEV_ERROR(dev->dev, "Failed to create framebuffer info.\n");
		ret = PTR_ERR(fbi);
		goto out;
	}

	helper->fb = rockchip_drm_framebuffer_init(dev, &mode_cmd,
						   private->fbdev_bo);
	if (IS_ERR(helper->fb)) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to allocate DRM framebuffer.\n");
		ret = PTR_ERR(helper->fb);
		goto out;
	}

	fbi->fbops = &rockchip_drm_fbdev_ops;

	fb = helper->fb;
	drm_fb_helper_fill_info(fbi, helper, sizes);

	offset = fbi->var.xoffset * bytes_per_pixel;
	offset += fbi->var.yoffset * fb->pitches[0];

	dev->mode_config.fb_base = 0;
	fbi->screen_base = rk_obj->kvaddr + offset;
	fbi->screen_size = rk_obj->base.size;
	fbi->fix.smem_len = rk_obj->base.size;

	DRM_DEBUG_KMS("FB [%dx%d]-%d kvaddr=%p offset=%ld size=%zu\n",
		      fb->width, fb->height, fb->format->depth,
		      rk_obj->kvaddr,
		      offset, size);

	return 0;

out:
	drm_gem_object_put(&rk_obj->base);
	return ret;
}

static const struct drm_fb_helper_funcs rockchip_drm_fb_helper_funcs = {
	.fb_probe = rockchip_drm_fbdev_create,
};

int rockchip_drm_fbdev_init(struct drm_device *dev)
{
	struct rockchip_drm_private *private = dev->dev_private;
	struct drm_fb_helper *helper;
	int ret;

	if (!dev->mode_config.num_crtc || !dev->mode_config.num_connector)
		return -EINVAL;

	helper = devm_kzalloc(dev->dev, sizeof(*helper), GFP_KERNEL);
	if (!helper)
		return -ENOMEM;
	private->fbdev_helper = helper;

	drm_fb_helper_prepare(dev, helper, &rockchip_drm_fb_helper_funcs);

	ret = drm_fb_helper_init(dev, helper);
	if (ret < 0) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to initialize drm fb helper - %d.\n",
			      ret);
		return ret;
	}

	ret = drm_fb_helper_initial_config(helper, PREFERRED_BPP);
	if (ret < 0) {
		DRM_DEV_ERROR(dev->dev,
			      "Failed to set initial hw config - %d.\n",
			      ret);
		goto err_drm_fb_helper_fini;
	}

	return 0;

err_drm_fb_helper_fini:
	drm_fb_helper_fini(helper);
	return ret;
}

void rockchip_drm_fbdev_fini(struct drm_device *dev)
{
	struct rockchip_drm_private *private = dev->dev_private;
	struct drm_fb_helper *helper = private->fbdev_helper;

	if (!helper)
		return;

	drm_fb_helper_unregister_fbi(helper);

	if (helper->fb)
		drm_framebuffer_put(helper->fb);

	drm_fb_helper_fini(helper);
}
