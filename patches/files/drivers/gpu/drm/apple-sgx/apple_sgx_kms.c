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
 * the renders writing it are done (the plane's implicit fences), turned
 * as the plane's rotation says (M41: the copy's quad turned).
 *
 * There is no vblank interrupt.  A flip is done when its copy is: its
 * event goes then (sgx_kms_commit_tail).  A timer at the mode's refresh
 * stands for the interrupt, so that there is a vblank count: X's Present
 * has no MSC without one, and GL programs under X go unpaced.  fbcon still
 * draws into the same memory when it has something to show.
 * docs/research/p105-mesa.md, M16, M38.
 */

#include <linux/hrtimer.h>
#include <linux/log2.h>
#include <linux/of.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
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
#include <drm/drm_rect.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

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
	struct hrtimer vblank_timer;
	ktime_t vblank_period;
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
	/* a pan across has to keep the copies' alignment -- down too, turned
	 * a quarter (the screen's x is then the framebuffer's y) */
	if (plane_state->fb && ((plane_state->src.x1 >> 16) % SGX_KMS_ALIGN_PX ||
				(drm_rotation_90_or_270(plane_state->rotation) &&
				 (plane_state->src.y1 >> 16) % SGX_KMS_ALIGN_PX)))
		return -EINVAL;
	return 0;
}

/* One damaged rectangle (framebuffer coordinates) onto the screen, turned
 * as the plane is (M41: the 2D engine's quad turned -- the copy costs the
 * same).  Its edges go out to 16 pixels in the framebuffer's x, and in its
 * y too when that is the screen's x. */
static void sgx_kms_show(struct apple_sgx_kms *kms, const struct drm_plane_state *state,
			 const struct drm_rect *clip)
{
	struct drm_framebuffer *fb = state->fb;
	unsigned int rot = ilog2(state->rotation & DRM_MODE_ROTATE_MASK);
	u32 sx = state->src.x1 >> 16, sy = state->src.y1 >> 16;
	u32 sw = drm_rect_width(&state->src) >> 16, sh = drm_rect_height(&state->src) >> 16;
	struct drm_rect src = DRM_RECT_INIT(sx, sy, sw, sh), r = *clip, on;
	u32 va;
	int ret;

	r.x1 = round_down(r.x1, SGX_KMS_ALIGN_PX);
	r.x2 = round_up(r.x2, SGX_KMS_ALIGN_PX);
	if (rot & 1) {
		r.y1 = round_down(r.y1, SGX_KMS_ALIGN_PX);
		r.y2 = round_up(r.y2, SGX_KMS_ALIGN_PX);
	}
	if (!drm_rect_intersect(&r, &src))
		return;
	/* where it lands: the plane's source turned onto the screen */
	on = r;
	drm_rect_translate(&on, -sx, -sy);
	drm_rect_rotate(&on, sw, sh, state->rotation & DRM_MODE_ROTATE_MASK);
	va = apple_sgx_bo_va(fb->obj[0]) + fb->offsets[0] + r.y1 * fb->pitches[0] + r.x1 * 4;
	ret = apple_sgx_fb_show(kms->sgx, va, fb->pitches[0] / 4, drm_rect_width(&r),
				drm_rect_height(&r), on.x1, on.y1, rot);
	if (ret)
		drm_err_ratelimited(fb->dev, "the 2D engine's copy to the screen: %d\n", ret);
}

/* The plane's damage onto the screen, before the commit's event goes. */
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
	drm_crtc_vblank_on(&pipe->crtc);
}

static void sgx_kms_disable(struct drm_simple_display_pipe *pipe)
{
	drm_crtc_vblank_off(&pipe->crtc);
}

/* The vblank that is not there, at the mode's refresh while anyone waits
 * for one.  Disabled (the count's users gone) it stops: a disable that
 * missed the timer running leaves it to stop itself. */
static enum hrtimer_restart sgx_kms_vblank(struct hrtimer *timer)
{
	struct apple_sgx_kms *kms = container_of(timer, struct apple_sgx_kms, vblank_timer);

	if (!drm_crtc_handle_vblank(&kms->pipe.crtc))
		return HRTIMER_NORESTART;
	hrtimer_forward_now(timer, kms->vblank_period);
	return HRTIMER_RESTART;
}

static int sgx_kms_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);

	hrtimer_start(&kms->vblank_timer, kms->vblank_period, HRTIMER_MODE_REL);
	return 0;
}

static void sgx_kms_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);

	/* (under the vblank locks the timer's handler takes: not waited for) */
	hrtimer_try_to_cancel(&kms->vblank_timer);
}

static const struct drm_simple_display_pipe_funcs sgx_kms_pipe_funcs = {
	.mode_valid = sgx_kms_mode_valid,
	.check = sgx_kms_check,
	.enable = sgx_kms_enable,
	.disable = sgx_kms_disable,
	.update = sgx_kms_update,
	.enable_vblank = sgx_kms_enable_vblank,
	.disable_vblank = sgx_kms_disable_vblank,
};

/* drm_atomic_helper_commit_tail() but for the vblank: the planes' copies
 * are done when commit_planes returns, so a commit's event goes then, with
 * the vblank count it is (not at the next vblank), and nothing waits for
 * one before the old framebuffers go. */
static void sgx_kms_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int i;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		unsigned long flags;

		spin_lock_irqsave(&drm->event_lock, flags);
		if (crtc_state->event) {
			drm_crtc_send_vblank_event(crtc, crtc_state->event);
			crtc_state->event = NULL;
		}
		spin_unlock_irqrestore(&drm->event_lock, flags);
	}
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(drm, state);
}

static const struct drm_mode_config_helper_funcs sgx_kms_mode_config_helpers = {
	.atomic_commit_tail = sgx_kms_commit_tail,
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

static void sgx_kms_fini(struct drm_device *drm, void *data)
{
	struct apple_sgx_kms *kms = data;

	hrtimer_cancel(&kms->vblank_timer);
}

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
	/* (the screen on its side, M41: 1024 x 768 buffers too) */
	drm->mode_config.min_width = min(sgx->fb_w, sgx->fb_h);
	drm->mode_config.min_height = min(sgx->fb_w, sgx->fb_h);
	drm->mode_config.max_width = SGX_KMS_MAX_SIZE;
	drm->mode_config.max_height = SGX_KMS_MAX_SIZE;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &sgx_kms_mode_config_funcs;
	drm->mode_config.helper_private = &sgx_kms_mode_config_helpers;

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
	/* (M41) the screen turned: the copies are */
	ret = drm_plane_create_rotation_property(&kms->pipe.plane, DRM_MODE_ROTATE_0,
						 DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 |
						 DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270);
	if (ret)
		return ret;

	/* the vblank's timer: a frame of the mode (clock in kHz) */
	kms->vblank_period = ns_to_ktime(div_u64((u64)m->htotal * m->vtotal * 1000000, m->clock));
	hrtimer_init(&kms->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kms->vblank_timer.function = sgx_kms_vblank;
	ret = drmm_add_action_or_reset(drm, sgx_kms_fini, kms);
	if (ret)
		return ret;
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	drm_info(drm, "screen: %ux%u at %pa, through the 2D engine\n", sgx->fb_w, sgx->fb_h,
		 &fb.start);
	return 0;
}
