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
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>  /* #102 present path: fb geometry/format */
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "nvkvm.h"

#define NVKVM_KMS_W   1920
#define NVKVM_KMS_H   1080
#define NVKVM_KMS_HZ  60

struct nvkvm_kms {
	struct drm_connector            conn;
	struct drm_simple_display_pipe  pipe;
	struct hrtimer                  vblank;   /* software vblank source       */
	ktime_t                         period;   /* 1/refresh                    */
};

/* ── Software vblank (vkms-style): an hrtimer drives the CRTC vblank at a fixed
 * refresh so page-flips pace + complete. Headless has no real scanout timing;
 * later (host present) this slaves to the host window's actual vblank. ──────── */
static enum hrtimer_restart nvkvm_vblank_fn(struct hrtimer *t)
{
	struct nvkvm_kms *kms = container_of(t, struct nvkvm_kms, vblank);
	drm_crtc_handle_vblank(&kms->pipe.crtc);
	hrtimer_forward_now(t, kms->period);
	return HRTIMER_RESTART;
}

/* ── Connector: a single fixed mode, no EDID ─────────────────────────────── */
static int nvkvm_conn_get_modes(struct drm_connector *conn)
{
	/* One preferred mode at the virtual panel's fixed resolution. */
	return drm_add_modes_noedid(conn, NVKVM_KMS_W, NVKVM_KMS_H);
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
	drm_crtc_vblank_off(&pipe->crtc);
}

static int nvkvm_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	hrtimer_start(&kms->vblank, kms->period, HRTIMER_MODE_REL);
	return 0;
}

static void nvkvm_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct nvkvm_kms *kms = container_of(pipe, struct nvkvm_kms, pipe);
	hrtimer_cancel(&kms->vblank);
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
	if (fb) {
		__u32 stub_handle = 0;
		struct nvkvm_fd_ctx *fctx = NULL;

		if (nvkvm_fb_stub_handle(fb, &stub_handle, &fctx)) {
			int pret = nvkvm_virtio_present(
				fctx, stub_handle, fb->width, fb->height,
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
					(unsigned long long)fb->modifier,
					stub_handle);
		}
	}
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
	ddev->mode_config.max_width  = NVKVM_KMS_W;
	ddev->mode_config.max_height = NVKVM_KMS_H;
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
	kms->period = ns_to_ktime(NSEC_PER_SEC / NVKVM_KMS_HZ);

	drm_connector_helper_add(&kms->conn, &nvkvm_conn_helper_funcs);
	ret = drm_connector_init(ddev, &kms->conn, &nvkvm_conn_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(ddev, &kms->pipe, &nvkvm_pipe_funcs,
					   nvkvm_pipe_formats,
					   ARRAY_SIZE(nvkvm_pipe_formats),
					   nvkvm_pipe_modifiers, &kms->conn);
	if (ret)
		return ret;

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
	pr_info("nvkvm: virtual KMS head ready (%dx%d, 1 connector/crtc)\n",
		NVKVM_KMS_W, NVKVM_KMS_H);
	return 0;
}
