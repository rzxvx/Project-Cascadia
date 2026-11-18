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
 *
 * Without the copy (M49), where the device tree names the display
 * controller iBoot left running (apple,s5l8940x-clcd: its layer 0 base,
 * and the line it is at): a flip to a physically contiguous buffer
 * (APPLE_SGX_BO_SCANOUT) of the screen's size and pitch, unturned, with no
 * overlay, points the layer at the buffer instead, and its event goes once
 * the display has gone into the next frame, when it no longer reads the
 * buffer before.  Anything else -- a buffer drawn into on the screen
 * (damage without a flip), the overlay, a turned plane -- has the plane
 * copied onto the boot framebuffer whole and the layer pointed back at it.
 *
 * Over the plane, the overlay (the render node's OVERLAY ioctl): a buffer
 * whose listed rectangles are blended over the plane in each copy that
 * reaches them, the same 2D job -- an on-screen control over whatever runs,
 * with no compositor (tools/screenpad).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/seq_file.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
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
#include <drm/apple_sgx_drm.h>

#include "apple_sgx.h"

/* the iPad mini's panel: 163 pixels an inch */
#define SGX_KMS_UM_PER_PX	156
/* the 2D engine is given surfaces that start on 64 bytes: framebuffers'
 * pitches and offsets are multiples of it, and copies start and end on
 * 16 pixels (damage is widened) */
#define SGX_KMS_ALIGN_PX	16
#define SGX_KMS_ALIGN		64
#define SGX_KMS_MAX_SIZE	4096

/* the display controller (apple,s5l8940x-clcd): in its first window, layer
 * 0's base address; in its second, the position it scans (>> 9: the line,
 * 0 to the screen's height - 1 over a frame) */
#define SGX_CLCD_L0_BASE	0x44
#define SGX_CLCD_POS		0x54
#define SGX_CLCD_LINE_SHIFT	9

/* (M49) off: every update copied onto the boot framebuffer, as before;
 * looked at in each update */
static bool direct_scanout = true;
module_param(direct_scanout, bool, 0644);
MODULE_PARM_DESC(direct_scanout,
		 "show a flip to a contiguous buffer by moving the display's base, not copying it");

struct apple_sgx_kms {
	struct apple_sgx *sgx;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct hrtimer vblank_timer;
	ktime_t vblank_period;

	/* the layer's base and the line counter (NULL: always copies); the
	 * boot framebuffer, and what the layer reads now; flipped: this
	 * commit moved the base (its event waits for the next frame) */
	void __iomem *clcd, *clcd_pos;
	dma_addr_t boot_pa, shown;
	bool flipped;
	/* updates shown by pointing the layer, by copies (debugfs screen) */
	unsigned long n_pointed, n_copied;

	/* the overlay: its buffer, owner and rectangles (the plane's source
	 * coordinates less its origin, out to 16 pixels); the lock keeps them
	 * still through the copies of one commit */
	struct mutex ov_lock;
	struct drm_file *ov_owner;
	struct drm_gem_object *ov_obj;
	u32 ov_pitch, ov_w, ov_h;
	unsigned int ov_n;
	struct drm_rect ov_rects[APPLE_SGX_OVERLAY_MAX_RECTS];
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

/* The overlay's part of r (framebuffer coordinates, as copied): the box of
 * its rectangles that meet r, where it lands in r's copy turned.  0: none.
 * Under ov_lock. */
static bool sgx_kms_over(struct apple_sgx_kms *kms, const struct drm_plane_state *state,
			 const struct drm_rect *r, struct apple_sgx_over *over)
{
	u32 sx = state->src.x1 >> 16, sy = state->src.y1 >> 16;
	u32 sw = drm_rect_width(&state->src) >> 16, sh = drm_rect_height(&state->src) >> 16;
	struct drm_rect box = { 0 }, o;
	unsigned int i;

	if (!kms->ov_obj || kms->ov_w != sw || kms->ov_h != sh)
		return false;
	for (i = 0; i < kms->ov_n; i++) {
		o = kms->ov_rects[i];
		drm_rect_translate(&o, sx, sy);
		if (!drm_rect_intersect(&o, r))
			continue;
		if (!drm_rect_visible(&box)) {
			box = o;
		} else {
			box.x1 = min(box.x1, o.x1);
			box.y1 = min(box.y1, o.y1);
			box.x2 = max(box.x2, o.x2);
			box.y2 = max(box.y2, o.y2);
		}
	}
	if (!drm_rect_visible(&box))
		return false;
	over->va = apple_sgx_bo_va(kms->ov_obj) + (box.y1 - sy) * kms->ov_pitch + (box.x1 - sx) * 4;
	over->stride = kms->ov_pitch / 4;
	over->w = drm_rect_width(&box);
	over->h = drm_rect_height(&box);
	drm_rect_translate(&box, -r->x1, -r->y1);
	drm_rect_rotate(&box, drm_rect_width(r), drm_rect_height(r),
			state->rotation & DRM_MODE_ROTATE_MASK);
	over->x = box.x1;
	over->y = box.y1;
	over->dw = drm_rect_width(&box);
	over->dh = drm_rect_height(&box);
	return true;
}

/* One damaged rectangle (framebuffer coordinates) onto the screen, turned
 * as the plane is (M41: the 2D engine's quad turned -- the copy costs the
 * same), the overlay blended over it in the same job.  Its edges go out to
 * 16 pixels in the framebuffer's x, and in its y too when that is the
 * screen's x.  Under ov_lock. */
static void sgx_kms_show(struct apple_sgx_kms *kms, const struct drm_plane_state *state,
			 const struct drm_rect *clip)
{
	struct drm_framebuffer *fb = state->fb;
	unsigned int rot = ilog2(state->rotation & DRM_MODE_ROTATE_MASK);
	u32 sx = state->src.x1 >> 16, sy = state->src.y1 >> 16;
	u32 sw = drm_rect_width(&state->src) >> 16, sh = drm_rect_height(&state->src) >> 16;
	struct drm_rect src = DRM_RECT_INIT(sx, sy, sw, sh), r = *clip, on;
	struct apple_sgx_over over;
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
				drm_rect_height(&r), on.x1, on.y1, rot,
				sgx_kms_over(kms, state, &r, &over) ? &over : NULL);
	if (ret)
		drm_err_ratelimited(fb->dev, "the 2D engine's copy to the screen: %d\n", ret);
}

static void sgx_kms_point(struct apple_sgx_kms *kms, dma_addr_t pa)
{
	if (pa == kms->shown)
		return;
	writel(pa, kms->clcd + SGX_CLCD_L0_BASE);
	kms->shown = pa;
	kms->flipped = true;
}

/* Whether the plane can be shown by pointing the layer at its buffer, and
 * where: a contiguous buffer, the screen's size from its origin and the
 * boot framebuffer's pitch, unturned, no overlay.  Under ov_lock. */
static bool sgx_kms_direct(struct apple_sgx_kms *kms, const struct drm_plane_state *state,
			   dma_addr_t *pa)
{
	struct apple_sgx *sgx = kms->sgx;
	struct drm_framebuffer *fb = state->fb;

	if (!READ_ONCE(direct_scanout) || !kms->clcd || !apple_sgx_bo_contig(fb->obj[0], pa) ||
	    (state->rotation & DRM_MODE_ROTATE_MASK) != DRM_MODE_ROTATE_0 ||
	    state->src.x1 || state->src.y1 || drm_rect_width(&state->src) >> 16 != sgx->fb_w ||
	    drm_rect_height(&state->src) >> 16 != sgx->fb_h ||
	    fb->pitches[0] != sgx->fb_stride * 4 || (kms->ov_obj && kms->ov_n))
		return false;
	*pa += fb->offsets[0];
	return true;
}

/* Back to copies: the whole plane onto the boot framebuffer, then the
 * layer pointed at it once the copy is done.  Under ov_lock. */
static void sgx_kms_to_boot(struct apple_sgx_kms *kms, const struct drm_plane_state *state)
{
	struct drm_rect all = drm_plane_state_src(state);

	all.x1 >>= 16;
	all.y1 >>= 16;
	all.x2 >>= 16;
	all.y2 >>= 16;
	sgx_kms_show(kms, state, &all);
	apple_sgx_blt_sync(kms->sgx);
	sgx_kms_point(kms, kms->boot_pa);
}

/* The plane onto the screen before the commit's event goes: a flip to a
 * buffer the display can read, by pointing it there; otherwise its damage,
 * copied. */
static void sgx_kms_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	dma_addr_t pa;

	if (!state->fb || !state->visible || !pipe->crtc.state->active)
		return;
	mutex_lock(&kms->ov_lock);
	if (state->fb != old_state->fb && sgx_kms_direct(kms, state, &pa)) {
		sgx_kms_point(kms, pa);
		kms->n_pointed++;
	} else if (kms->clcd && kms->shown != kms->boot_pa) {
		sgx_kms_to_boot(kms, state);
		kms->n_copied++;
	} else {
		kms->n_copied++;
		drm_atomic_helper_damage_iter_init(&iter, old_state, state);
		drm_atomic_for_each_plane_damage(&iter, &clip)
			sgx_kms_show(kms, state, &clip);
	}
	mutex_unlock(&kms->ov_lock);
}

/* After the layer's base moved: until the display is into the next frame
 * (it takes the base there, or at once -- either way the buffer before is
 * no longer read): the line counter gone round and a line further, or a
 * frame's time, sleeping through most of it. */
static void sgx_kms_wait_frame(struct apple_sgx_kms *kms)
{
	u32 h = kms->sgx->fb_h, prev, line, us;
	u32 line_ns = div_u64(ktime_to_ns(kms->vblank_period), h);
	ktime_t end = ktime_add(ktime_get(), ktime_add_us(kms->vblank_period, 200));
	bool wrapped = false;

	prev = readl(kms->clcd_pos) >> SGX_CLCD_LINE_SHIFT;
	while (ktime_before(ktime_get(), end)) {
		line = readl(kms->clcd_pos) >> SGX_CLCD_LINE_SHIFT;
		wrapped |= line < prev;
		if (wrapped && line >= 1)
			return;
		prev = line;
		/* to the frame's end at most, in steps of 2 ms or less */
		us = clamp((h - min(line, h)) * line_ns / 1000, 50u, 2000u);
		usleep_range(us, us + 50);
	}
}

static void sgx_kms_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state, struct drm_plane_state *plane_state)
{
	/* what is shown comes with the plane's update */
	drm_crtc_vblank_on(&pipe->crtc);
}

static void sgx_kms_disable(struct drm_simple_display_pipe *pipe)
{
	struct apple_sgx_kms *kms = container_of(pipe, struct apple_sgx_kms, pipe);

	/* the boot framebuffer back (fbcon's; stale until it draws) */
	if (kms->clcd)
		sgx_kms_point(kms, kms->boot_pa);
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
 * are queued when commit_planes returns, so a commit's event goes then,
 * with the vblank count it is (not at the next vblank) -- the program
 * draws its next frame into the other buffer meanwhile; the next copy
 * waits for this one.  An old framebuffer goes once no copy reads it. */
static void sgx_kms_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *drm = state->dev;
	struct drm_plane_state *old_ps, *new_ps;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	int i;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, 0);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct apple_sgx_kms *kms = container_of(crtc, struct apple_sgx_kms, pipe.crtc);

		/* the layer moved: the event, and the old buffer, once the
		 * display no longer reads it */
		if (kms->flipped) {
			sgx_kms_wait_frame(kms);
			kms->flipped = false;
		}
	}
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
	for_each_oldnew_plane_in_state(state, plane, old_ps, new_ps, i) {
		struct apple_sgx_kms *kms = container_of(plane, struct apple_sgx_kms, pipe.plane);

		if (old_ps->fb && old_ps->fb != new_ps->fb)
			apple_sgx_blt_sync(kms->sgx);
	}
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

static void sgx_kms_overlay_drop(struct apple_sgx_kms *kms);

static void sgx_kms_fini(struct drm_device *drm, void *data)
{
	struct apple_sgx_kms *kms = data;

	kms->sgx->kms = NULL;
	sgx_kms_overlay_drop(kms);

	hrtimer_cancel(&kms->vblank_timer);
	if (kms->clcd) {
		writel(kms->boot_pa, kms->clcd + SGX_CLCD_L0_BASE);
		iounmap(kms->clcd);
		iounmap(kms->clcd_pos);
	}
}

/* ---- the overlay ---------------------------------------------------------- */

/* The screen under rects (overlay coordinates) shown again: the plane's
 * copy, the overlay's blend.  Under the modeset locks and ov_lock. */
static void sgx_kms_overlay_redraw(struct apple_sgx_kms *kms, const struct drm_rect *rects,
				   unsigned int n)
{
	struct drm_plane_state *state = kms->pipe.plane.state;
	struct drm_rect r;
	unsigned int i;

	if (!state || !state->fb || !state->visible || !kms->pipe.crtc.state->active)
		return;
	/* (the layer on the plane's buffer: the boot framebuffer back, whole) */
	if (kms->clcd && kms->shown != kms->boot_pa && kms->ov_obj && kms->ov_n) {
		sgx_kms_to_boot(kms, state);
		return;
	}
	for (i = 0; i < n; i++) {
		r = rects[i];
		drm_rect_translate(&r, state->src.x1 >> 16, state->src.y1 >> 16);
		sgx_kms_show(kms, state, &r);
	}
}

static void sgx_kms_overlay_drop(struct apple_sgx_kms *kms)
{
	/* (a copy queued may still be reading it) */
	if (kms->ov_obj) {
		apple_sgx_blt_sync(kms->sgx);
		drm_gem_object_put(kms->ov_obj);
	}
	kms->ov_obj = NULL;
	kms->ov_owner = NULL;
	kms->ov_n = 0;
}

/* DRM_IOCTL_APPLE_SGX_OVERLAY (apple_sgx_drm.c): set, change or take away
 * the overlay, or (QUERY) say what the plane is now. */
int apple_sgx_kms_overlay(struct apple_sgx *sgx, struct drm_file *file,
			  struct drm_apple_sgx_overlay *args)
{
	struct apple_sgx_kms *kms = sgx->kms;
	struct drm_rect rects[APPLE_SGX_OVERLAY_MAX_RECTS], old[APPLE_SGX_OVERLAY_MAX_RECTS];
	struct drm_modeset_acquire_ctx ctx;
	struct drm_gem_object *obj = NULL;
	struct drm_plane_state *state;
	struct drm_device *drm;
	unsigned int i, j, n = 0, nold;
	int ret;

	if (!kms)
		return -ENODEV;
	drm = kms->pipe.plane.dev;
	if (args->flags & ~APPLE_SGX_OVERLAY_QUERY || args->nrects > APPLE_SGX_OVERLAY_MAX_RECTS)
		return -EINVAL;
	if (!(args->flags & APPLE_SGX_OVERLAY_QUERY) && args->handle) {
		if (!args->width || !args->height || args->pitch % SGX_KMS_ALIGN ||
		    args->pitch < args->width * 4)
			return -EINVAL;
		obj = drm_gem_object_lookup(file, args->handle);
		if (!obj)
			return -ENOENT;
		if (obj->size < (size_t)args->pitch * args->height) {
			drm_gem_object_put(obj);
			return -EINVAL;
		}
		/* each rectangle out to 16 pixels, within the buffer */
		for (i = 0; i < args->nrects; i++) {
			const struct drm_apple_sgx_overlay_rect *a = &args->rects[i];
			struct drm_rect r = DRM_RECT_INIT(a->x, a->y, a->w, a->h);
			struct drm_rect all = DRM_RECT_INIT(0, 0, args->width, args->height);

			r.x1 = round_down(r.x1, SGX_KMS_ALIGN_PX);
			r.y1 = round_down(r.y1, SGX_KMS_ALIGN_PX);
			r.x2 = round_up(r.x2, SGX_KMS_ALIGN_PX);
			r.y2 = round_up(r.y2, SGX_KMS_ALIGN_PX);
			if (drm_rect_intersect(&r, &all))
				rects[n++] = r;
		}
	}

	DRM_MODESET_LOCK_ALL_BEGIN(drm, ctx, 0, ret);
	mutex_lock(&kms->ov_lock);
	if (!(args->flags & APPLE_SGX_OVERLAY_QUERY)) {
		if (kms->ov_owner && kms->ov_owner != file) {
			ret = -EBUSY;
		} else {
			/* (an old rectangle inside a new one is redrawn with it) */
			for (i = nold = 0; i < kms->ov_n; i++) {
				for (j = 0; j < n; j++) {
					struct drm_rect in = kms->ov_rects[i];

					if (obj && drm_rect_intersect(&in, &rects[j]) &&
					    drm_rect_equals(&in, &kms->ov_rects[i]))
						break;
				}
				if (!obj || j == n)
					old[nold++] = kms->ov_rects[i];
			}
			sgx_kms_overlay_drop(kms);
			if (obj) {
				kms->ov_obj = obj;
				obj = NULL;
				kms->ov_owner = file;
				kms->ov_pitch = args->pitch;
				kms->ov_w = args->width;
				kms->ov_h = args->height;
				kms->ov_n = n;
				memcpy(kms->ov_rects, rects, sizeof(rects[0]) * n);
			}
			/* what was under the old rectangles, then the new ones */
			sgx_kms_overlay_redraw(kms, old, nold);
			sgx_kms_overlay_redraw(kms, kms->ov_rects, kms->ov_n);
		}
	}
	state = kms->pipe.plane.state;
	if (state && state->fb && state->visible) {
		args->out_width = drm_rect_width(&state->src) >> 16;
		args->out_height = drm_rect_height(&state->src) >> 16;
		args->out_rotation = state->rotation;
	} else {
		args->out_width = args->out_height = 0;
		args->out_rotation = DRM_MODE_ROTATE_0;
	}
	mutex_unlock(&kms->ov_lock);
	DRM_MODESET_LOCK_ALL_END(drm, ctx, ret);
	if (obj)
		drm_gem_object_put(obj);
	return ret;
}

/* The file that set the overlay closed: it goes, and the screen under it
 * is shown again. */
void apple_sgx_kms_overlay_close(struct apple_sgx *sgx, struct drm_file *file)
{
	struct apple_sgx_kms *kms = sgx->kms;
	struct drm_apple_sgx_overlay args = { 0 };

	if (kms && READ_ONCE(kms->ov_owner) == file)
		apple_sgx_kms_overlay(sgx, file, &args);
}

/* debugfs apple-sgx/screen: where the layer reads, and how updates went */
static int sgx_kms_screen_show(struct seq_file *m, void *unused)
{
	struct apple_sgx_kms *kms = m->private;

	if (!kms->clcd) {
		seq_puts(m, "no display controller: updates copied\n");
		return 0;
	}
	seq_printf(m, "layer 0 base %pad (boot framebuffer %pad)\n", &kms->shown, &kms->boot_pa);
	seq_printf(m, "updates shown by pointing the layer %lu, by copies %lu\n",
		   READ_ONCE(kms->n_pointed), READ_ONCE(kms->n_copied));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgx_kms_screen);

/* The display controller, if the device tree has it and its layer reads
 * the boot framebuffer (what iBoot left; anything else is not touched). */
static void sgx_kms_find_clcd(struct apple_sgx_kms *kms, const struct resource *fb)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-clcd");

	if (!np)
		return;
	kms->clcd = of_iomap(np, 0);
	kms->clcd_pos = of_iomap(np, 1);
	of_node_put(np);
	if (kms->clcd && kms->clcd_pos && readl(kms->clcd + SGX_CLCD_L0_BASE) == fb->start &&
	    kms->sgx->fb_stride * 4 * kms->sgx->fb_h <= resource_size(fb)) {
		kms->boot_pa = fb->start;
		kms->shown = fb->start;
		return;
	}
	if (kms->clcd)
		iounmap(kms->clcd);
	if (kms->clcd_pos)
		iounmap(kms->clcd_pos);
	kms->clcd = kms->clcd_pos = NULL;
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
	ret = drmm_mutex_init(drm, &kms->ov_lock);
	if (ret)
		return ret;

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
	/* (M46) a flip without waiting for a vblank: what every flip here is
	 * (there is none; the copy goes once the renders writing the buffer
	 * are done) -- and what X's Present asks for a client that swaps
	 * without vsync (PPSSPP): without the cap it copied the window into
	 * the screen instead, a render of the X server's each frame */
	drm->mode_config.async_page_flip = true;

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
	sgx_kms_find_clcd(kms, &fb);
	if (sgx->debugfs)
		debugfs_create_file("screen", 0400, sgx->debugfs, kms, &sgx_kms_screen_fops);
	sgx->kms = kms;
	drm_info(drm, "screen: %ux%u at %pa, through the 2D engine%s\n", sgx->fb_w, sgx->fb_h,
		 &fb.start, kms->clcd ? " or the display's layer base" : "");
	return 0;
}
