// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_kms.c — guest-emulated virtual KMS head (#102 modeset Piece 1).
 *
 * A vkms-style virtual display integrated into nvkvm's OWN nvidia-drm device,
 * so the render node and the scanout head are the SAME DRM device — no cross-
 * device PRIME. Pure intra-VM state: ZERO host display calls (we never forward
 * NVKMS; see docs/design/virtual_modeset.md).
 *
 * Headless first: the virtual CRTC accepts atomic commits / page-flips and
 * completes their flip events, but performs no real scanout (there is no
 * physical connector). A compositor/desktop can run on this head and render via
 * the GPU render node. Later (Piece 1 host present) the flipped buffer's dma-buf
 * is exported to QEMU for a host window with host-paced vblank.
 *
 * Scope is deliberately minimal: one connector (fixed 1080p), one CRTC, one
 * primary plane, atomic helpers. No multi-head / HDCP / overlays.
 */
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_vblank.h>
#include <drm/drm_atomic.h>
#include <drm/drm_edid.h>      /* drm_add_modes_noedid */
#include <drm/drm_modes.h>     /* drm_cvt_mode, drm_mode_probed_add */
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>  /* #102 present path: fb geometry/format */
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "nvkvm.h"

/*
 * The virtual head's mode.  These were compile-time constants, which made
 * 1920x1080 a property of the binary rather than of the deployment -- wrong for
 * anyone whose panel is not that, and wrong for a headless guest streaming to
 * something with its own idea of a resolution.
 *
 * Guest-side only, deliberately: QEMU sizes its console from whatever the guest
 * actually scans out (nvkvm_present calls qemu_console_resize() before
 * presenting), so nothing on the host needs telling.
 *
 * 0444 -- read-only after load.  A mode change would have to renegotiate with
 * every client already holding a framebuffer, which is a different feature.
 * Set them at insmod, or in a modprobe.d conf:
 *
 *     options nvkvm-guest kms_width=2560 kms_height=1440 kms_hz=60
 */
#define NVKVM_KMS_W_DEFAULT   1920
#define NVKVM_KMS_H_DEFAULT   1080
#define NVKVM_KMS_HZ_DEFAULT  60

/*
 * THE CEILING, WHICH IS NOT THE MODE.
 *
 * These were the same number, and that is what capped the head.  The mode list
 * was built with drm_add_modes_noedid(conn, kms_w, kms_h) and
 * mode_config.max_width/max_height were set to kms_w/kms_h as well -- so the
 * largest mode a compositor could be offered, and the largest framebuffer it
 * could create, were both exactly the resolution the head happened to boot at.
 * A head like that can never be asked to grow, whatever tells it to.
 *
 * That is the whole reason the host compositor could never promote the guest's
 * buffer to a hardware plane: Mutter only scans out a surface that COVERS the
 * output, the output is 3840x2160, and a 1600x900 head cannot produce a
 * 3840x2160 buffer at any window size.  See src/broker/README.md.
 *
 * So: the ceiling is what the head COULD do, the mode is what it is doing now.
 * The ceiling bounds framebuffer allocation and the extent of the mode list;
 * it creates no modes by itself and costs nothing until something asks for a
 * bigger one.  The current mode still starts at kms_width/kms_height and is
 * then driven by the host through ui_info (nvkvm_kms_set_host_size).
 *
 * 4K by default because that is the largest display anyone is likely to put
 * this window on today, not because 4K is special.  Raise it for an 8K panel:
 *
 *     options nvkvm-guest kms_max_width=7680 kms_max_height=4320
 */
#define NVKVM_KMS_MAX_W_DEFAULT   3840
#define NVKVM_KMS_MAX_H_DEFAULT   2160

/* Bounds are sanity, not policy: reject a value that would make the mode list
 * or the vblank timer nonsense, and fall back rather than refuse to load. */
#define NVKVM_KMS_MIN         64u
#define NVKVM_KMS_MAX         16384u
#define NVKVM_KMS_HZ_MIN      1u
#define NVKVM_KMS_HZ_MAX      1000u

static unsigned int nvkvm_kms_w  = NVKVM_KMS_W_DEFAULT;
static unsigned int nvkvm_kms_h  = NVKVM_KMS_H_DEFAULT;
static unsigned int nvkvm_kms_hz = NVKVM_KMS_HZ_DEFAULT;
static unsigned int nvkvm_kms_max_w = NVKVM_KMS_MAX_W_DEFAULT;
static unsigned int nvkvm_kms_max_h = NVKVM_KMS_MAX_H_DEFAULT;

module_param_named(kms_width, nvkvm_kms_w, uint, 0444);
MODULE_PARM_DESC(kms_width, "virtual head width in pixels (default 1920)");
module_param_named(kms_height, nvkvm_kms_h, uint, 0444);
MODULE_PARM_DESC(kms_height, "virtual head height in pixels (default 1080)");
module_param_named(kms_hz, nvkvm_kms_hz, uint, 0444);
MODULE_PARM_DESC(kms_hz, "virtual head refresh rate in Hz (default 60)");
module_param_named(kms_max_width, nvkvm_kms_max_w, uint, 0444);
MODULE_PARM_DESC(kms_max_width,
		 "largest width the head can ever be asked for (default 3840)");
module_param_named(kms_max_height, nvkvm_kms_max_h, uint, 0444);
MODULE_PARM_DESC(kms_max_height,
		 "largest height the head can ever be asked for (default 2160)");

/* The single virtual head.  One head by construction, so a pointer rather than
 * a lookup; NULL until nvkvm_kms_init() has finished building it, which is why
 * nvkvm_kms_set_host_size() must tolerate that.  The lock is the lifetime
 * handoff between the softirq virtqueue callback and process-context teardown:
 * shutdown unpublishes under it before synchronously cancelling resize work. */
static struct nvkvm_kms *nvkvm_kms_head;
static DEFINE_SPINLOCK(nvkvm_kms_head_lock);

static void nvkvm_kms_clamp_mode(void)
{
	if (nvkvm_kms_w < NVKVM_KMS_MIN || nvkvm_kms_w > NVKVM_KMS_MAX) {
		pr_warn("nvkvm: kms_width=%u out of range [%u,%u], using %u\n",
			nvkvm_kms_w, NVKVM_KMS_MIN, NVKVM_KMS_MAX,
			NVKVM_KMS_W_DEFAULT);
		nvkvm_kms_w = NVKVM_KMS_W_DEFAULT;
	}
	if (nvkvm_kms_h < NVKVM_KMS_MIN || nvkvm_kms_h > NVKVM_KMS_MAX) {
		pr_warn("nvkvm: kms_height=%u out of range [%u,%u], using %u\n",
			nvkvm_kms_h, NVKVM_KMS_MIN, NVKVM_KMS_MAX,
			NVKVM_KMS_H_DEFAULT);
		nvkvm_kms_h = NVKVM_KMS_H_DEFAULT;
	}
	if (nvkvm_kms_hz < NVKVM_KMS_HZ_MIN || nvkvm_kms_hz > NVKVM_KMS_HZ_MAX) {
		pr_warn("nvkvm: kms_hz=%u out of range [%u,%u], using %u\n",
			nvkvm_kms_hz, NVKVM_KMS_HZ_MIN, NVKVM_KMS_HZ_MAX,
			NVKVM_KMS_HZ_DEFAULT);
		nvkvm_kms_hz = NVKVM_KMS_HZ_DEFAULT;
	}
	if (nvkvm_kms_max_w < NVKVM_KMS_MIN || nvkvm_kms_max_w > NVKVM_KMS_MAX) {
		pr_warn("nvkvm: kms_max_width=%u out of range [%u,%u], using %u\n",
			nvkvm_kms_max_w, NVKVM_KMS_MIN, NVKVM_KMS_MAX,
			NVKVM_KMS_MAX_W_DEFAULT);
		nvkvm_kms_max_w = NVKVM_KMS_MAX_W_DEFAULT;
	}
	if (nvkvm_kms_max_h < NVKVM_KMS_MIN || nvkvm_kms_max_h > NVKVM_KMS_MAX) {
		pr_warn("nvkvm: kms_max_height=%u out of range [%u,%u], using %u\n",
			nvkvm_kms_max_h, NVKVM_KMS_MIN, NVKVM_KMS_MAX,
			NVKVM_KMS_MAX_H_DEFAULT);
		nvkvm_kms_max_h = NVKVM_KMS_MAX_H_DEFAULT;
	}
	/* The ceiling must contain the mode, or the head boots into a size it is
	 * not allowed to allocate.  Raise the ceiling rather than shrink the
	 * mode: the operator asked for the mode explicitly and only defaulted
	 * into the ceiling. */
	if (nvkvm_kms_max_w < nvkvm_kms_w)
		nvkvm_kms_max_w = nvkvm_kms_w;
	if (nvkvm_kms_max_h < nvkvm_kms_h)
		nvkvm_kms_max_h = nvkvm_kms_h;

}

struct nvkvm_kms {
	struct drm_connector            conn;
	struct drm_simple_display_pipe  pipe;
	struct hrtimer                  vblank;   /* software vblank source       */
	ktime_t                         period;   /* 1/refresh                    */

	/*
	 * Asynchronous present.
	 *
	 * nvkvm_virtio_present() is a SYNCHRONOUS virtio round-trip: it puts the
	 * request on VQ_TX and blocks until QEMU replies.  Calling it straight
	 * from nvkvm_pipe_update() means the atomic commit -- and therefore the
	 * guest compositor -- stalls for a full host round-trip on every single
	 * flip.  Measured on RTX 4070 / 595.84 that capped the pipeline at
	 * ~21 fps (~47 ms/frame) even with the host-side copy already removed by
	 * the zero-copy GL path.
	 *
	 * Hand the send to an ordered workqueue so the commit returns at once.
	 * Only the newest frame is kept: if a flip arrives while one is still
	 * queued, the older framebuffer is dropped.  That is safe because QEMU's
	 * present slot already discards any frame the display has not drained.
	 */
	struct workqueue_struct         *present_wq;
	struct work_struct              present_work;
	struct work_struct              resize_work;
	struct drm_framebuffer          *pending_fb;
	spinlock_t                      pending_lock;
	unsigned int                    cur_w;
	unsigned int                    cur_h;
	unsigned int                    pending_w;
	unsigned int                    pending_h;
	bool                            active;
	bool                            stopping;
};

/* ── Software vblank (vkms-style): an hrtimer drives the CRTC vblank at a fixed
 * refresh so page-flips pace + complete. Headless has no real scanout timing;
 * later (host present) this slaves to the host window's actual vblank. ──────── */
static enum hrtimer_restart nvkvm_vblank_fn(struct hrtimer *t)
{
	struct nvkvm_kms *kms = container_of(t, struct nvkvm_kms, vblank);

	if (READ_ONCE(kms->stopping))
		return HRTIMER_NORESTART;
	drm_crtc_handle_vblank(&kms->pipe.crtc);
	hrtimer_forward_now(t, kms->period);
	return HRTIMER_RESTART;
}

/* ── Connector: a mode list bounded by the ceiling, preferring the current
 *    size ────────────────────────────────────────────────────────────────── */

/* Is (w,h) already among the modes we have probed onto this connector? */
static bool nvkvm_mode_listed(struct drm_connector *conn, unsigned int w,
			      unsigned int h)
{
	struct drm_display_mode *m;

	list_for_each_entry(m, &conn->probed_modes, head) {
		if (m->hdisplay == (int)w && m->vdisplay == (int)h)
			return true;
	}
	return false;
}

static int nvkvm_conn_get_modes(struct drm_connector *conn)
{
	struct nvkvm_kms *kms = container_of(conn, struct nvkvm_kms, conn);
	unsigned long flags;
	unsigned int w, h;
	struct drm_display_mode *mode;
	int count;

	/* Width and height are one state transition.  Reading two unrelated
	 * globals allowed get_modes() to observe a width from one broker event
	 * and a height from the next. */
	spin_lock_irqsave(&kms->pending_lock, flags);
	w = kms->cur_w;
	h = kms->cur_h;
	spin_unlock_irqrestore(&kms->pending_lock, flags);

	/*
	 * The standard table, up to the CEILING -- not up to the mode we happen
	 * to be in.  A compositor can only ever choose a mode that was offered,
	 * so a list that ends at the current size is a head that can never grow.
	 */
	count = drm_add_modes_noedid(conn, nvkvm_kms_max_w, nvkvm_kms_max_h);

	/*
	 * AND THE ONE THAT ACTUALLY MATTERS, WHICH THAT CALL CANNOT PRODUCE.
	 *
	 * drm_add_modes_noedid() offers the VESA DMT table, and DMT stops at
	 * 2560x1600 -- there is no 3840x2160 in it.  Verified on hardware: with
	 * kms_width=3840 kms_height=2160 the connector's mode list still topped
	 * out at 2560x1600.  So raising the ceiling alone reaches nothing; the
	 * mode has to be synthesised.
	 *
	 * The host's window size is not a standard mode either -- a dragged
	 * window is 2560x1412, not 2560x1440 -- so the same synthesis serves
	 * both.  CVT with reduced blanking, because this head has no real
	 * timing: the numbers exist only so the mode is well-formed.
	 *
	 * Matching the host output EXACTLY is the point.  Mutter promotes a
	 * surface to a hardware plane only when it covers the output, so
	 * "close to 3840x2160" is worth exactly as much as 1600x900.
	 */
	if (w && h && !nvkvm_mode_listed(conn, w, h)) {
		mode = drm_cvt_mode(conn->dev, w, h, nvkvm_kms_hz, true, false,
				    false);
		if (mode) {
			mode->type |= DRM_MODE_TYPE_DRIVER;
			drm_mode_probed_add(conn, mode);
			count++;
		}
	}

	/* drm_add_modes_noedid() marks NONE of its modes preferred, so a client
	 * picks by its own heuristics -- Xorg chose 1400x1050 on a 1920x1080
	 * panel.  Flag ours, the way vkms/virtio-gpu do. */
	drm_set_preferred_mode(conn, w, h);
	return count;
}

/*
 * THE HOST'S WINDOW CHANGED SIZE.
 *
 * Called from the virtio event path when QEMU forwards ui_info -- which is
 * itself the broker's EV_SURFACE, i.e. the size of the window a person just
 * dragged.  Without this the guest never learns the window changed, and the
 * host compositor is left resampling a fixed-size guest frame into a different
 * sized window: the picture the user described as "only sharp at the initial
 * window size or smaller".
 *
 * This does NOT set the mode.  It moves the head's idea of its native size and
 * fires a hotplug, and the guest's own compositor then decides whether to take
 * it -- which is the right split, because a mode switch is the guest's business
 * and a window drag is the host's.
 */
static void nvkvm_resize_work_fn(struct work_struct *work)
{
	struct nvkvm_kms *kms = container_of(work, struct nvkvm_kms,
					     resize_work);
	unsigned long flags;
	unsigned int w, h;

	spin_lock_irqsave(&kms->pending_lock, flags);
	if (kms->stopping ||
	    (kms->cur_w == kms->pending_w && kms->cur_h == kms->pending_h)) {
		spin_unlock_irqrestore(&kms->pending_lock, flags);
		return;
	}
	w = kms->pending_w;
	h = kms->pending_h;
	kms->cur_w = w;
	kms->cur_h = h;
	spin_unlock_irqrestore(&kms->pending_lock, flags);

	pr_info("nvkvm: host window is %ux%u; offering it as the preferred mode\n",
		w, h);

	/* Process context is mandatory for this helper.  The virtqueue callback
	 * only coalesces into resize_work; it never calls DRM directly. */
	drm_kms_helper_hotplug_event(kms->conn.dev);
}

void nvkvm_kms_set_host_size(unsigned int w, unsigned int h)
{
	struct nvkvm_kms *kms;
	unsigned long head_flags, pending_flags;

	/* Bound it before it reaches the mode list: this number comes from
	 * outside the guest, and mode_config.max_* is what the rest of DRM will
	 * enforce anyway.  Clamp rather than reject -- a window larger than the
	 * ceiling is a reasonable thing for a user to make, and the head should
	 * simply stop growing at that point. */
	if (w < NVKVM_KMS_MIN || h < NVKVM_KMS_MIN)
		return;
	if (w > nvkvm_kms_max_w)
		w = nvkvm_kms_max_w;
	if (h > nvkvm_kms_max_h)
		h = nvkvm_kms_max_h;

	/* VQ_EVT runs in softirq context.  Hold the publication lock through
	 * queue_work(), so fini either sees and cancels this generation or wins
	 * first and leaves no pointer for us to queue through. */
	spin_lock_irqsave(&nvkvm_kms_head_lock, head_flags);
	kms = nvkvm_kms_head;
	if (!kms) {
		spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);
		return;
	}
	spin_lock_irqsave(&kms->pending_lock, pending_flags);
	if (!kms->stopping &&
	    (w != kms->pending_w || h != kms->pending_h)) {
		kms->pending_w = w;
		kms->pending_h = h;
		if (kms->active)
			schedule_work(&kms->resize_work);
	}
	spin_unlock_irqrestore(&kms->pending_lock, pending_flags);
	spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);
}

/* A virtual panel is always present: report connected on every probe so the
 * connector advertises its mode and compositors will drive it. */
static enum drm_connector_status
nvkvm_conn_detect(struct drm_connector *conn, bool force)
{
	(void)conn; (void)force;
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs nvkvm_conn_helper_funcs = {
	.get_modes = nvkvm_conn_get_modes,
};

static const struct drm_connector_funcs nvkvm_conn_funcs = {
	.detect                 = nvkvm_conn_detect,
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.reset                  = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

/* ── Display pipe (CRTC + primary plane + encoder) ───────────────────────── */
/*
 * A CRTC must have vblank turned ON while it is enabled, or the vblank
 * timestamp/sequence bookkeeping the core hands back to userspace never
 * advances.  We used to skip this entirely.
 *
 * The symptom was not a missing flip -- flips completed, because
 * nvkvm_pipe_update() falls back to drm_crtc_send_vblank_event() when
 * drm_crtc_vblank_get() fails.  It was that weston's desktop-shell fade-in
 * never finished: its scene graph showed a "desktop shell fade surface"
 * covering the whole output, fully opaque, solid black, sitting above the
 * panel and every client.  Weston was compositing correctly the whole time --
 * it was animating against a clock that never moved, so the screen stayed
 * black and looked like a rendering failure.
 */
static void nvkvm_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	(void)crtc_state; (void)plane_state;
	drm_crtc_vblank_on(&pipe->crtc);
}

static void nvkvm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	struct drm_framebuffer *fb;
	unsigned long flags;

	drm_crtc_vblank_off(&pipe->crtc);

	/* Nothing is scanned out any more: flush the queued present and drop
	 * the framebuffer reference so the bo is not pinned past disable. */
	flush_work(&kms->present_work);
	spin_lock_irqsave(&kms->pending_lock, flags);
	fb = kms->pending_fb;
	kms->pending_fb = NULL;
	spin_unlock_irqrestore(&kms->pending_lock, flags);
	if (fb)
		drm_framebuffer_put(fb);
}

static int nvkvm_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&kms->pending_lock, flags);
	if (kms->stopping)
		ret = -ENODEV;
	else
		hrtimer_start(&kms->vblank, kms->period, HRTIMER_MODE_REL);
	spin_unlock_irqrestore(&kms->pending_lock, flags);
	return ret;
}

static void nvkvm_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	hrtimer_cancel(&kms->vblank);
}

/* Send one framebuffer's present to QEMU.  Runs in process context: the
 * virtio round-trip inside nvkvm_virtio_present() sleeps. */
static void nvkvm_present_send(struct drm_framebuffer *fb)
{
	__u32 stub_handle = 0;
	struct nvkvm_fd_ctx *fctx = NULL;
	int pret;

	/* Re-derive owner + handle from the fb we hold a reference on, rather
	 * than caching them at commit time: the reference keeps the proxy GEM
	 * (and so the ctx behind it) alive for exactly as long as we need it. */
	if (!fb || !nvkvm_fb_stub_handle(fb, &stub_handle, &fctx))
		return;

	pret = nvkvm_virtio_present(fctx, stub_handle, fb->width, fb->height,
				    fb->pitches[0],
				    fb->format ? fb->format->format : 0,
				    fb->modifier);
	if (pret)
		pr_info_ratelimited(
			"nvkvm present: export failed %d (flip %ux%u stub_handle=0x%x)\n",
			pret, fb->width, fb->height, stub_handle);
	else
		pr_info_ratelimited(
			"nvkvm present: flip %ux%u pitch=%u fmt=0x%08x mod=0x%llx stub_handle=0x%x → exported\n",
			fb->width, fb->height, fb->pitches[0],
			fb->format ? fb->format->format : 0,
			(unsigned long long)fb->modifier, stub_handle);
}

static void nvkvm_present_work_fn(struct work_struct *w)
{
	struct nvkvm_kms *kms = container_of(w, struct nvkvm_kms, present_work);
	struct drm_framebuffer *fb;
	unsigned long flags;

	spin_lock_irqsave(&kms->pending_lock, flags);
	fb = kms->pending_fb;
	kms->pending_fb = NULL;
	spin_unlock_irqrestore(&kms->pending_lock, flags);

	if (fb) {
		nvkvm_present_send(fb);
		drm_framebuffer_put(fb);
	}
}

/* Queue @fb for presentation and return immediately.  Replaces any frame still
 * waiting, so a slow host cannot build a backlog in the guest. */
static void nvkvm_present_queue(struct nvkvm_kms *kms,
				struct drm_framebuffer *fb)
{
	struct drm_framebuffer *old;
	unsigned long flags;

	if (!fb)
		return;

	drm_framebuffer_get(fb);
	spin_lock_irqsave(&kms->pending_lock, flags);
	if (kms->stopping || !kms->present_wq) {
		spin_unlock_irqrestore(&kms->pending_lock, flags);
		drm_framebuffer_put(fb);
		return;
	}
	old = kms->pending_fb;
	kms->pending_fb = fb;
	/* Queue under the same lock shutdown uses to set stopping.  Once fini
	 * observes the flag, no late producer can race behind cancel_work_sync(). */
	queue_work(kms->present_wq, &kms->present_work);
	spin_unlock_irqrestore(&kms->pending_lock, flags);
	if (old)
		drm_framebuffer_put(old);
}

static void nvkvm_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_framebuffer *fb = pipe->plane.state ? pipe->plane.state->fb : NULL;

	(void)old_state;

	/* Present path (#106) — the host buffer behind this scanout frame. A real
	 * compositor (weston) flips an NVIDIA bo, which surfaces here as one of our
	 * proxy GEMs carrying the stub handle + owning isolate. Notify QEMU so it
	 * exports that buffer's host dma-buf and routes it to the host display /
	 * codec. Best-effort: a failed present must not stall the flip completion
	 * (we still arm the vblank event below). A plain shmem dumb fb (modetest)
	 * is not a proxy → nothing to present. */
	if (fb)
		nvkvm_present_queue(container_of(pipe, struct nvkvm_kms, pipe), fb);
	/* Headless: no real scanout. Pace the flip completion to the software
	 * vblank so a compositor renders at the refresh rate, not unbounded. */
	if (event) {
		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs nvkvm_pipe_funcs = {
	.enable         = nvkvm_pipe_enable,
	.disable        = nvkvm_pipe_disable,
	.update         = nvkvm_pipe_update,
	.enable_vblank  = nvkvm_pipe_enable_vblank,
	.disable_vblank = nvkvm_pipe_disable_vblank,
};

static const uint32_t nvkvm_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

/*
 * Present path (#102/#109): the head must accept the buffers a real NVIDIA
 * client renders. NVIDIA scanout surfaces are BLOCK-LINEAR (tiled in VRAM), not
 * linear — advertising only LINEAR makes AddFB2 reject them (EINVAL), so a
 * compositor composits but can never flip. Advertise the canonical NVIDIA
 * 16Bx2 block-linear scanout family (GOB heights 0..5) plus LINEAR so gbm /
 * compositors negotiate a modifier we accept.
 *
 * We do NOT read these pixels in the guest (they live in host VRAM, tiled); the
 * head only holds the buffer + its modifier and forwards (stub_handle, modifier,
 * geometry) to QEMU, where the host NVIDIA driver imports the exported dma-buf
 * (detiling on the GPU) for the present path. So "accepting" the modifier is
 * correct support, not a fake — the host honours the real layout.
 */
/*
 * The DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(h) macro omits the KIND/GEN/SECTOR bits
 * (yields 0x...000010+h), but REAL NVIDIA gbm/EGL buffers carry them:
 * uncompressed scanout/render bos are BLOCK_LINEAR_2D(c=0,s=1,g=2,k=6,h) =
 * 0x...606010+h, and compressed (k=8) = 0x...e08010+h.  Advertising only the
 * macro values made AddFB2 reject real bos (modifier mismatch) → the present
 * path fell back to a no-modifier fb (forwarded mod=0), so the host could not
 * detile and read black.  Advertise the real families (GOB heights 0..5) so
 * AddFB2 keeps the true modifier and the host detiles correctly on present.
 * Uncompressed (k=6) imports cleanly cross-context (use these for capture/
 * present); compressed (k=8) is accepted for completeness but its compression
 * state isn't shared, so a compositor capture target should use k=6.
 */
#define NVKVM_MOD_BL2D(c, s, g, k, h) \
	fourcc_mod_code(NVIDIA, (0x10ULL | ((h) & 0xf) | (((uint64_t)(k) & 0xff) << 12) | \
				 (((uint64_t)(g) & 0x3) << 20) | (((uint64_t)(s) & 0x1) << 22) | \
				 (((uint64_t)(c) & 0x7) << 23)))
static const uint64_t nvkvm_pipe_modifiers[] = {
	/*
	 * ONLY modifiers this driver is known to implement, read off real bos
	 * that NVIDIA's own GBM produced in-guest:
	 *
	 *   SCANOUT|RENDERING -> 0x0300000000606014   (= BL2D(0,1,2,6,4))
	 *   RENDERING         -> 0x0300000000e08014   (= BL2D(0,1,2,14,4))
	 *
	 * Both import as EGLImages and give a COMPLETE FBO, verified in-guest.
	 * Publishing anything beyond these is actively harmful now that the
	 * IN_FORMATS blob is rebuilt and compositors actually see the list: a
	 * client that picks an invented modifier gets a buffer the driver
	 * cannot use as a render target, and fails exactly where wlroots does
	 * ("Failed to create FBO") and Xorg/glamor does ("Failed to create
	 * pixmap").  LINEAR is excluded for the same reason -- see the accept
	 * callback, which still allows it for cursors.
	 */
	NVKVM_MOD_BL2D(0, 1, 2, 6, 4),    /* 0x0300000000606014 scanout+render */
	NVKVM_MOD_BL2D(0, 1, 2, 14, 4),   /* 0x0300000000e08014 render         */
	DRM_FORMAT_MOD_INVALID
};

/*
 * Plane modifier validation (#110 host-visible last mile).
 *
 * drm_simple_display_pipe installs drm_simple_kms_plane_funcs, which has NO
 * .format_mod_supported callback. On this kernel that makes
 * drm_any_plane_has_format() reject AddFB2WithModifiers() for our block-linear
 * scanout modifiers even though they ARE in the plane's modifier list /
 * IN_FORMATS blob — so a compositor flipping a real NVIDIA scanout bo
 * (mod 0x...606014) falls back to a no-modifier fb (forwarded mod=0) and the
 * host imports block-linear data as LINEAR → black frame.
 *
 * Provide an explicit callback that accepts exactly the modifiers we advertise.
 * With format_mod_supported present, drm_plane_check_pixel_format() consults it
 * directly, so the true modifier survives to the present path (#106) and the
 * host detiles correctly. We accept any format here — the format itself is
 * validated against plane->format_types before this is called.
 */

/*
 * Rebuild the plane's IN_FORMATS blob.
 *
 * drm_simple_display_pipe_init() builds that blob during init, filtering our
 * modifier list through drm_simple_kms_format_mod_supported -- which accepts
 * ONLY DRM_FORMAT_MOD_LINEAR.  Every block-linear modifier we advertise is
 * therefore dropped before our own callback is grafted on, and a compositor
 * reading IN_FORMATS is offered exactly XRGB8888 + LINEAR.
 *
 * That is fatal rather than merely suboptimal: NVIDIA cannot use a LINEAR
 * dma-buf as an EGLImage render target (measured in-guest: GL_INVALID_OPERATION
 * on bind, incomplete FBO), and that import is how a compositor obtains its
 * output framebuffer.  weston and wlroots both take LINEAR, fail to create
 * their render FBO and composite nothing -- so we export buffers nobody drew
 * into, and the screen is black.
 *
 * Replace the blob with one that actually lists what we support, now that
 * plane->funcs is ours.
 */
#ifndef NVKVM_FORMAT_BLOB_VERSION
#define NVKVM_FORMAT_BLOB_VERSION 1
#endif
static int nvkvm_plane_rebuild_in_formats(struct drm_plane *plane,
					  const u32 *formats, unsigned int nf,
					  const u64 *mods, unsigned int nm)
{
	struct drm_device *dev = plane->dev;
	struct drm_format_modifier_blob *bh;
	struct drm_format_modifier *fm;
	struct drm_property_blob *blob;
	size_t formats_size, mods_size, blob_size;
	unsigned int i, j;
	u32 *fmt;
	u64 mask;

	if (!dev->mode_config.modifiers_property || !nf || !nm)
		return -EINVAL;
	if (nf > 64)              /* the per-modifier format mask is 64 bits */
		nf = 64;

	formats_size = sizeof(u32) * nf;
	mods_size    = sizeof(struct drm_format_modifier) * nm;
	blob_size    = sizeof(*bh) + ALIGN(formats_size, 8) + mods_size;

	blob = drm_property_create_blob(dev, blob_size, NULL);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	bh = blob->data;
	bh->version          = NVKVM_FORMAT_BLOB_VERSION;
	bh->flags            = 0;
	bh->count_formats    = nf;
	bh->formats_offset   = sizeof(*bh);
	bh->count_modifiers  = nm;
	bh->modifiers_offset = bh->formats_offset + ALIGN(formats_size, 8);

	fmt = (u32 *)((char *)bh + bh->formats_offset);
	memcpy(fmt, formats, formats_size);

	mask = (nf == 64) ? ~0ULL : ((1ULL << nf) - 1);
	fm = (struct drm_format_modifier *)((char *)bh + bh->modifiers_offset);
	for (i = 0, j = 0; i < nm; i++) {
		fm[j].formats  = mask;   /* every format, for this modifier */
		fm[j].offset   = 0;
		fm[j].pad      = 0;
		fm[j].modifier = mods[i];
		j++;
	}

	drm_object_property_set_value(&plane->base,
				      dev->mode_config.modifiers_property,
				      blob->base.id);
	return 0;
}

static bool nvkvm_plane_format_mod_supported(struct drm_plane *plane,
					     u32 format, u64 modifier)
{
	const uint64_t *m;

	(void)plane; (void)format;
	/*
	 * LINEAR is ACCEPTED here but deliberately not ADVERTISED (it is absent
	 * from nvkvm_pipe_modifiers, which is what the IN_FORMATS blob offers).
	 * The two lists are consulted by different callers and want different
	 * answers:
	 *
	 *   - IN_FORMATS is what a compositor PICKS a render target from, and
	 *     NVIDIA cannot use a LINEAR dma-buf as an EGLImage render target,
	 *     so offering LINEAR there makes wlroots/glamor fail to create an
	 *     FBO and composite nothing.
	 *   - drm_any_plane_has_format() consults this callback for EVERY
	 *     framebuffer creation, including plain CPU-rendered dumb buffers
	 *     that are never an EGL render target.  Xorg's modesetting DDX
	 *     scans out exactly such a buffer, and the legacy ADDFB ioctl it
	 *     uses has no modifier field at all -- the core fills in
	 *     DRM_FORMAT_MOD_LINEAR.  Rejecting it here failed every AddFB with
	 *     -EINVAL, which cost native Xorg twice over: the DDX's 32bpp probe
	 *     fell back to a 24bpp packed fb and switched glamor OFF (silent
	 *     software rendering), and the scanout fb could not be created at
	 *     all ("failed to set mode: Invalid argument").
	 */
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return true;
	for (m = nvkvm_pipe_modifiers; *m != DRM_FORMAT_MOD_INVALID; m++)
		if (*m == modifier)
			return true;
	return false;
}

/* A copy of whatever funcs drm_simple_display_pipe installed, plus the modifier
 * callback (filled in at init so we stay version-agnostic). One virtual head. */
static struct drm_plane_funcs nvkvm_plane_funcs;

/* ── Mode config ─────────────────────────────────────────────────────────── */
static const struct drm_mode_config_funcs nvkvm_kms_mode_funcs = {
	.fb_create     = drm_gem_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Set up the virtual head on `ddev`. Called from nvkvm_drm_init BEFORE
 * drm_dev_register (mode objects must exist before the device goes live). */
int nvkvm_kms_init(struct drm_device *ddev)
{
	struct nvkvm_kms *kms;
	int ret;

	ret = drmm_mode_config_init(ddev);
	if (ret)
		return ret;
	ddev->mode_config.min_width  = 0;
	ddev->mode_config.min_height = 0;
	nvkvm_kms_clamp_mode();
	/* The CEILING, not the mode -- see NVKVM_KMS_MAX_W_DEFAULT.  This bounds
	 * framebuffer creation, so a head whose max_* equals its boot mode can
	 * never scan out anything larger however the mode list is built. */
	ddev->mode_config.max_width  = nvkvm_kms_max_w;
	ddev->mode_config.max_height = nvkvm_kms_max_h;
	/* Reported as DRM_CAP_DUMB_PREFERRED_DEPTH; left at 0 a client has to
	 * guess, and Xorg's modesetting DDX guesses by probing a scanout fb. */
	ddev->mode_config.preferred_depth = 24;
	ddev->mode_config.funcs      = &nvkvm_kms_mode_funcs;

	/* Flip events need vblank bookkeeping even though we complete them
	 * immediately (drm_crtc_send_vblank_event reads the vblank state). */
	ret = drm_vblank_init(ddev, 1);
	if (ret)
		return ret;

	kms = drmm_kzalloc(ddev, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;

	nvkvm_hrtimer_setup(&kms->vblank, nvkvm_vblank_fn,
			    CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	kms->period = ns_to_ktime(NSEC_PER_SEC / nvkvm_kms_hz);

	/* Ordered: presents must reach QEMU in flip order. */
	spin_lock_init(&kms->pending_lock);
	INIT_WORK(&kms->present_work, nvkvm_present_work_fn);
	INIT_WORK(&kms->resize_work, nvkvm_resize_work_fn);
	kms->cur_w = nvkvm_kms_w;
	kms->cur_h = nvkvm_kms_h;
	kms->pending_w = nvkvm_kms_w;
	kms->pending_h = nvkvm_kms_h;
	kms->present_wq = alloc_ordered_workqueue("nvkvm-present", WQ_MEM_RECLAIM);
	if (!kms->present_wq)
		return -ENOMEM;

	drm_connector_helper_add(&kms->conn, &nvkvm_conn_helper_funcs);
	ret = drm_connector_init(ddev, &kms->conn, &nvkvm_conn_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		goto err_workqueue;

	ret = drm_simple_display_pipe_init(ddev, &kms->pipe, &nvkvm_pipe_funcs,
					   nvkvm_pipe_formats,
					   ARRAY_SIZE(nvkvm_pipe_formats),
					   nvkvm_pipe_modifiers, &kms->conn);
	if (ret)
		goto err_workqueue;

	/* #110: drm_simple_display_pipe installs drm_simple_kms_format_mod_supported,
	 * which accepts ONLY DRM_FORMAT_MOD_LINEAR and ignores the plane's modifier
	 * list — so AddFB2WithModifiers() rejected our block-linear scanout modifiers
	 * (EINVAL) even though they were advertised, and the present path fell back to
	 * a no-modifier fb (forwarded mod=0) → the host imported block-linear as LINEAR
	 * → black. Graft our own callback that accepts exactly what we advertise. Copy
	 * the funcs the helper installed (version-agnostic) and override the callback. */
	{
		struct drm_plane *pl = &kms->pipe.plane;

		nvkvm_plane_funcs = *pl->funcs;
		nvkvm_plane_funcs.format_mod_supported = nvkvm_plane_format_mod_supported;
		pl->funcs = &nvkvm_plane_funcs;

		/* The blob built during init used the helper's LINEAR-only
		 * filter; rebuild it now that the callback above is ours. */
		ret = nvkvm_plane_rebuild_in_formats(pl, nvkvm_pipe_formats,
						     ARRAY_SIZE(nvkvm_pipe_formats),
						     nvkvm_pipe_modifiers,
						     ARRAY_SIZE(nvkvm_pipe_modifiers) - 1);
		if (ret)
			pr_warn("nvkvm: could not rebuild IN_FORMATS (%d); compositors will only see LINEAR\n",
				ret);
	}

	drm_mode_config_reset(ddev);
	/* Publish the fully constructed object, but leave it inactive until
	 * drm_dev_register() succeeds.  UI events in that interval update the
	 * pending pair without scheduling a hotplug against an unregistered DRM
	 * device; nvkvm_kms_activate() delivers the latest pair afterwards. */
	spin_lock_irq(&nvkvm_kms_head_lock);
	nvkvm_kms_head = kms;
	spin_unlock_irq(&nvkvm_kms_head_lock);
	pr_info("nvkvm: virtual KMS head ready (%ux%u, up to %ux%u, 1 connector/crtc)\n",
		kms->cur_w, kms->cur_h,
		nvkvm_kms_max_w, nvkvm_kms_max_h);
	return 0;

err_workqueue:
	destroy_workqueue(kms->present_wq);
	kms->present_wq = NULL;
	return ret;
}

/* Called immediately after drm_dev_register() succeeds. */
void nvkvm_kms_activate(void)
{
	struct nvkvm_kms *kms;
	unsigned long head_flags, pending_flags;

	spin_lock_irqsave(&nvkvm_kms_head_lock, head_flags);
	kms = nvkvm_kms_head;
	if (!kms) {
		spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);
		return;
	}
	spin_lock_irqsave(&kms->pending_lock, pending_flags);
	if (!kms->stopping) {
		kms->active = true;
		if (kms->cur_w != kms->pending_w ||
		    kms->cur_h != kms->pending_h)
			schedule_work(&kms->resize_work);
	}
	spin_unlock_irqrestore(&kms->pending_lock, pending_flags);
	spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);
}

/* Stop every path that can name drmm-owned KMS state before the DRM device or
 * the virtio transport is released.  Publication and queueing share a lock;
 * work/timer cancellation is synchronous after the pointer disappears. */
void nvkvm_kms_fini(void)
{
	struct nvkvm_kms *kms;
	struct workqueue_struct *present_wq;
	struct drm_framebuffer *fb;
	unsigned long head_flags, pending_flags;

	spin_lock_irqsave(&nvkvm_kms_head_lock, head_flags);
	kms = nvkvm_kms_head;
	if (!kms) {
		spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);
		return;
	}
	nvkvm_kms_head = NULL;
	spin_lock_irqsave(&kms->pending_lock, pending_flags);
	kms->active = false;
	kms->stopping = true;
	spin_unlock_irqrestore(&kms->pending_lock, pending_flags);
	spin_unlock_irqrestore(&nvkvm_kms_head_lock, head_flags);

	cancel_work_sync(&kms->resize_work);
	hrtimer_cancel(&kms->vblank);
	cancel_work_sync(&kms->present_work);

	spin_lock_irqsave(&kms->pending_lock, pending_flags);
	fb = kms->pending_fb;
	kms->pending_fb = NULL;
	present_wq = kms->present_wq;
	kms->present_wq = NULL;
	spin_unlock_irqrestore(&kms->pending_lock, pending_flags);
	if (fb)
		drm_framebuffer_put(fb);
	if (present_wq)
		destroy_workqueue(present_wq);
}
