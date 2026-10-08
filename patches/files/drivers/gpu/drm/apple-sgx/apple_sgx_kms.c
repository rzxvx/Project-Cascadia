// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2 -- the screen, through KMS.
 *
 * The display pipe is iBoot's: it scans out a framebuffer at a fixed
 * physical address (the device tree's simple-framebuffer, which simplefb
 * keeps for fbcon), and nothing here programs it.  What is here is KMS on
 * that memory, so that GBM, EGL and compositors have a screen: one plane,
 * CRTC, encoder and connector, the framebuffer's one mode.  A KMS
 * framebuffer is any of the render node's buffers (Mesa's through GBM, or
 * a dumb buffer), and showing it is a copy onto the screen by the 2D
 * engine -- the damaged rectangles, or the whole plane on a flip -- once
 * the renders writing it are done (the plane's implicit fences).
 *
 * There is no vblank interrupt: a flip is done when its copy is
 * (drm_atomic_helper_fake_vblank).  fbcon still draws into the same memory
 * when it has something to show.  docs/research/p105-mesa.md, M16.
 */

#include <linux/of.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "apple_sgx.h"

/* the iPad mini's panel: 163 pixels an inch */
#define SGX_KMS_UM_PER_PX	156
/* the 2D engine is given surfaces that start on 64 bytes: framebuffers'
 * pitches and offsets are multiples of it, and copies start and end on
 * 16 pixels (damage is widened) */
#define SGX_KMS_ALIGN_PX	16
#define SGX_KMS_ALIGN		64
#define SGX_KMS_MAX_SIZE	4096

struct apple_sgx_kms {
	struct apple_sgx *sgx;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
};

static const u32 sgx_kms_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };

static int sgx_kms_get_modes(struct drm_connector *connector)
{
	struct apple_sgx_kms *kms = container_of(connector, struct apple_sgx_kms, connector);

	return drm_connector_helper_get_modes_fixed(connector, &kms->mode);
}

static const struct drm_connector_helper_funcs sgx_kms_connector_helper = {
	.get_modes = sgx_kms_get_modes,
};

static const struct drm_connector_funcs sgx_kms_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status sgx_kms_mode_valid(struct drm_simple_display_pipe *pipe,
					       const struct drm_display_mode *mode)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);

	if (mode->hdisplay != kms->mode.hdisplay || mode->vdisplay != kms->mode.vdisplay)
		return MODE_ONE_SIZE;
	return MODE_OK;
}

static int sgx_kms_check(struct drm_simple_display_pipe *pipe,
			 struct drm_plane_state *plane_state, struct drm_crtc_state *crtc_state)
{
	/* a pan across has to keep the copies' alignment */
	if (plane_state->fb && ((plane_state->src.x1 >> 16) % SGX_KMS_ALIGN_PX))
		return -EINVAL;
	return 0;
}

/* One damaged rectangle (framebuffer coordinates) onto the screen. */
static void sgx_kms_show(struct apple_sgx_kms *kms, const struct drm_plane_state *state,
			 const struct drm_rect *clip)
{
	struct drm_framebuffer *fb = state->fb;
	u32 sx = state->src.x1 >> 16, sy = state->src.y1 >> 16;
	u32 x1 = round_down(clip->x1, SGX_KMS_ALIGN_PX);
	u32 x2 = min_t(u32, round_up(clip->x2, SGX_KMS_ALIGN_PX), fb->width);
	u32 va;
	int ret;

	if (x1 < sx)
		x1 = sx;
	if (x2 > sx + kms->mode.hdisplay)
		x2 = sx + kms->mode.hdisplay;
	if (x2 <= x1 || clip->y2 <= clip->y1)
		return;
	va = apple_sgx_bo_va(fb->obj[0]) + fb->offsets[0] + clip->y1 * fb->pitches[0] + x1 * 4;
	ret = apple_sgx_fb_show(kms->sgx, va, fb->pitches[0] / 4, x1 - sx, clip->y1 - sy,
				x2 - x1, clip->y2 - clip->y1);
	if (ret)
		drm_err_ratelimited(fb->dev, "the 2D engine's copy to the screen: %d\n", ret);
}

static void sgx_kms_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;

	if (!state->fb || !state->visible || !pipe->crtc.state->active)
		return;
	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip)
		sgx_kms_show(kms, state, &clip);
}

static void sgx_kms_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state, struct drm_plane_state *plane_state)
{
	/* what is shown comes with the plane's update */
}

static const struct drm_simple_display_pipe_funcs sgx_kms_pipe_funcs = {
	.mode_valid = sgx_kms_mode_valid,
	.check = sgx_kms_check,
	.enable = sgx_kms_enable,
	.update = sgx_kms_update,
};

/* Framebuffers the 2D engine can read: the render node's own buffers (no
 * imports from other devices), 64-byte pitches and offsets. */
static struct drm_framebuffer *sgx_kms_fb_create(struct drm_device *drm, struct drm_file *file,
						 const struct drm_mode_fb_cmd2 *cmd)
{
	if (cmd->pitches[0] % SGX_KMS_ALIGN || cmd->offsets[0] % SGX_KMS_ALIGN)
		return ERR_PTR(-EINVAL);
	return drm_gem_fb_create_with_dirty(drm, file, cmd);
}

static const struct drm_mode_config_funcs sgx_kms_mode_config_funcs = {
	.fb_create = sgx_kms_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Before the device is registered.  -ENODEV: no framebuffer, no screen. */
int apple_sgx_kms_init(struct drm_device *drm, struct apple_sgx *sgx)
{
	struct apple_sgx_kms *kms;
	struct drm_display_mode *m;
	struct resource fb;
	int ret;

	ret = apple_sgx_fb_find(sgx, &fb);
	if (ret)
		return -ENODEV;
	kms = drmm_kzalloc(drm, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;
	kms->sgx = sgx;

	/* the one mode: the framebuffer's size, a 60 Hz refresh with token
	 * blanking (nothing is programmed from it) */
	m = &kms->mode;
	m->hdisplay = sgx->fb_w;
	m->hsync_start = sgx->fb_w + 8;
	m->hsync_end = sgx->fb_w + 16;
	m->htotal = sgx->fb_w + 32;
	m->vdisplay = sgx->fb_h;
	m->vsync_start = sgx->fb_h + 2;
	m->vsync_end = sgx->fb_h + 4;
	m->vtotal = sgx->fb_h + 8;
	m->clock = DIV_ROUND_UP(m->htotal * m->vtotal * 60, 1000);
	m->width_mm = sgx->fb_w * SGX_KMS_UM_PER_PX / 1000;
	m->height_mm = sgx->fb_h * SGX_KMS_UM_PER_PX / 1000;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(m);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = sgx->fb_w;
	drm->mode_config.min_height = sgx->fb_h;
	drm->mode_config.max_width = SGX_KMS_MAX_SIZE;
	drm->mode_config.max_height = SGX_KMS_MAX_SIZE;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &sgx_kms_mode_config_funcs;

	ret = drmm_connector_init(drm, &kms->connector, &sgx_kms_connector_funcs,
				  DRM_MODE_CONNECTOR_Unknown, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&kms->connector, &sgx_kms_connector_helper);
	kms->connector.display_info.width_mm = m->width_mm;
	kms->connector.display_info.height_mm = m->height_mm;

	ret = drm_simple_display_pipe_init(drm, &kms->pipe, &sgx_kms_pipe_funcs, sgx_kms_formats,
					   ARRAY_SIZE(sgx_kms_formats), NULL, &kms->connector);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&kms->pipe.plane);
	drm_mode_config_reset(drm);
	drm_info(drm, "screen: %ux%u at %pa, through the 2D engine\n", sgx->fb_w, sgx->fb_h,
		 &fb.start);
	return 0;
}
