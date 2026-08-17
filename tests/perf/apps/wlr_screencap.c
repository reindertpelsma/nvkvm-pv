/* wlr_screencap.c — zero-copy GPU screen capture via wlr-screencopy into a
 * dma-buf (#110).  This is the path weston's SHM-only output_capture cannot do:
 * wlroots' screencopy renders the compositor's output directly into a
 * CLIENT-provided dma-buf, which the compositor imports as a render target —
 * a cross-isolate import (capture client → compositor), now supported by nvkvm.
 *
 * Captures N frames of a headless sway output into a reused GPU bo on
 * renderD128, measures sustained fps, and optionally reads back one frame to a
 * PPM to prove real composited content.  This is the capture stage of the
 * cloud-gaming present path (capture → [#106 present] → host).
 *
 *   usage: wlr_screencap <nframes> [--ppm /path]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/mman.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

static struct wl_output             *output;
static struct zwlr_screencopy_manager_v1 *scm;
static struct zwp_linux_dmabuf_v1   *dmabuf_factory;
static struct gbm_device            *gbm;
static int    gbm_fd = -1;

/* negotiated dmabuf params (from the frame's linux_dmabuf event) */
static uint32_t cap_fmt, cap_w, cap_h;
static int      have_dmabuf_fmt;

/* the single reusable capture buffer */
static struct gbm_bo   *cap_bo;
static struct wl_buffer *cap_wlbuf;
static uint32_t          cap_stride;

/* --present: flip each captured frame on the virtual KMS head → #106 → host */
static int       present;
static int       card_fd = -1;
static uint32_t  fb_id, crtc_id;
static drmModeConnector *conn;
static drmModeModeInfo   mode;
static int       crtc_set;

/* NVIDIA UNCOMPRESSED 16Bx2 block-linear modifiers (KIND=6, GEN=2, SECTOR=1),
 * GOB heights 0..5.  These are render-capable AND import cleanly across contexts
 * (the host detiles on present) — unlike gbm's default pick, a COMPRESSED variant
 * (KIND=8, 0x...e08014) whose compression state isn't shared, so the host reads
 * it as black.  The libdrm DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK macro omits the
 * KIND/GEN/SECTOR bits (yields 0x...000010+h, which NVIDIA gbm won't allocate),
 * so we use the exact values NVIDIA's gbm/EGL advertise. LINEAR is not
 * render-capable on NVIDIA, so it is intentionally absent. */
static const uint64_t cap_modifiers[] = {
	0x300000000606010ULL, 0x300000000606011ULL, 0x300000000606012ULL,
	0x300000000606013ULL, 0x300000000606014ULL, 0x300000000606015ULL,
};

/* per-frame completion state */
static volatile int frame_ready, frame_failed;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		scm = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 3);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name))
		dmabuf_factory = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ── frame events ─────────────────────────────────────────────────────────── */
static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f,
		     uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{ (void)d; (void)f; (void)fmt; (void)w; (void)h; (void)stride; /* shm — ignore */ }

static void f_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f,
			   uint32_t fmt, uint32_t w, uint32_t h)
{
	(void)d; (void)f;
	cap_fmt = fmt; cap_w = w; cap_h = h; have_dmabuf_fmt = 1;
}

static int alloc_cap_buffer(void)
{
	/* Render-target bo with an explicit head-compatible block-linear modifier
	 * (see cap_modifiers). Falls back to driver default if modifiers are
	 * unsupported (capture-only, no flip). */
	cap_bo = gbm_bo_create_with_modifiers(gbm, cap_w, cap_h, cap_fmt,
					      cap_modifiers,
					      sizeof(cap_modifiers)/sizeof(cap_modifiers[0]));
	if (!cap_bo)
		cap_bo = gbm_bo_create(gbm, cap_w, cap_h, cap_fmt, GBM_BO_USE_RENDERING);
	if (!cap_bo) { fprintf(stderr, "gbm_bo_create %ux%u fmt=0x%x failed\n",
				cap_w, cap_h, cap_fmt); return -1; }
	cap_stride = gbm_bo_get_stride(cap_bo);
	uint64_t mod = gbm_bo_get_modifier(cap_bo);
	int fd = gbm_bo_get_fd(cap_bo);

	struct zwp_linux_buffer_params_v1 *p =
		zwp_linux_dmabuf_v1_create_params(dmabuf_factory);
	zwp_linux_buffer_params_v1_add(p, fd, 0, 0, cap_stride,
				       (uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffff));
	cap_wlbuf = zwp_linux_buffer_params_v1_create_immed(p, cap_w, cap_h, cap_fmt, 0);
	zwp_linux_buffer_params_v1_destroy(p);
	close(fd);
	if (!cap_wlbuf) { fprintf(stderr, "dmabuf wl_buffer create failed\n"); return -1; }
	fprintf(stderr, "capture bo: %ux%u fmt=0x%x stride=%u mod=0x%llx\n",
		cap_w, cap_h, cap_fmt, cap_stride, (unsigned long long)mod);

	if (present) {
		/* KMS framebuffer for flipping on the virtual head. The bo lives on
		 * card0's gbm so its GEM handle is valid in card0's DRM file. */
		uint32_t handles[4] = { gbm_bo_get_handle(cap_bo).u32, 0, 0, 0 };
		uint32_t strides[4] = { cap_stride, 0, 0, 0 };
		uint32_t offsets[4] = { 0, 0, 0, 0 };
		uint64_t mods[4]    = { mod, 0, 0, 0 };
		int r = drmModeAddFB2WithModifiers(card_fd, cap_w, cap_h, cap_fmt,
						   handles, strides, offsets, mods,
						   &fb_id, DRM_MODE_FB_MODIFIERS);
		if (r) {  /* retry without explicit modifier */
			fprintf(stderr, "AddFB2WithModifiers(mod=0x%llx) failed: %s; retrying no-modifier\n",
				(unsigned long long)mod, strerror(errno));
			r = drmModeAddFB2(card_fd, cap_w, cap_h, cap_fmt,
					  handles, strides, offsets, &fb_id, 0);
		} else {
			fprintf(stderr, "AddFB2WithModifiers(mod=0x%llx) OK fb=%u\n",
				(unsigned long long)mod, fb_id);
		}
		if (r) { fprintf(stderr, "AddFB2 (present) failed: %s\n", strerror(errno));
			 return -1; }
	}
	return 0;
}

static void f_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f)
{
	(void)d;
	if (!have_dmabuf_fmt) { frame_failed = 1; return; }
	if (!cap_wlbuf && alloc_cap_buffer() < 0) { frame_failed = 1; return; }
	zwlr_screencopy_frame_v1_copy(f, cap_wlbuf);   /* compositor blits output → our bo */
}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl)
{ (void)d; (void)f; (void)fl; }
static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
		    uint32_t s_hi, uint32_t s_lo, uint32_t ns)
{ (void)d; (void)f; (void)s_hi; (void)s_lo; (void)ns; frame_ready = 1; }
static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f)
{ (void)d; (void)f; frame_failed = 1; }
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
		     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer       = f_buffer,
	.flags        = f_flags,
	.ready        = f_ready,
	.failed       = f_failed,
	.damage       = f_damage,
	.linux_dmabuf = f_linux_dmabuf,
	.buffer_done  = f_buffer_done,
};

static double now_s(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int nframes = argc > 1 ? atoi(argv[1]) : 120;
	const char *ppm = NULL;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
		else if (!strcmp(argv[i], "--present")) present = 1;
	}

	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);   /* output geometry */
	if (!scm || !output || !dmabuf_factory) {
		fprintf(stderr, "missing globals: scm=%p output=%p dmabuf=%p\n",
			(void*)scm, (void*)output, (void*)dmabuf_factory);
		return 2;
	}

	if (present) {
		/* Allocate the capture bo on card0 (so its GEM handle is valid for
		 * AddFB2) and drive the virtual head. */
		card_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		if (card_fd < 0) { perror("open card0"); return 3; }
		drmSetMaster(card_fd);
		gbm = gbm_create_device(card_fd);
		drmModeRes *res = drmModeGetResources(card_fd);
		for (int i = 0; res && i < res->count_connectors; i++) {
			drmModeConnector *c = drmModeGetConnector(card_fd, res->connectors[i]);
			if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) { conn = c; break; }
			if (c) drmModeFreeConnector(c);
		}
		if (!conn) { fprintf(stderr, "present: no connected head\n"); return 3; }
		mode = conn->modes[0];
		drmModeEncoder *enc = drmModeGetEncoder(card_fd, conn->encoders[0]);
		crtc_id = (enc && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];
	} else {
		gbm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		gbm = gbm_create_device(gbm_fd);
	}
	if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 3; }

	int ok = 0;
	double t0 = now_s();
	for (int i = 0; i < nframes; i++) {
		frame_ready = frame_failed = 0;
		struct zwlr_screencopy_frame_v1 *f =
			zwlr_screencopy_manager_v1_capture_output(scm, 0, output);
		zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, NULL);
		while (!frame_ready && !frame_failed) {
			if (wl_display_dispatch(dpy) < 0) { frame_failed = 1; break; }
		}
		zwlr_screencopy_frame_v1_destroy(f);
		if (!frame_ready) { fprintf(stderr, "frame %d failed\n", i); break; }
		ok++;

		/* Present the freshly-captured frame on the virtual head → #106 →
		 * host (the composited desktop reaches the host display). */
		if (present && fb_id) {
			int r;
			if (!crtc_set) {
				r = drmModeSetCrtc(card_fd, crtc_id, fb_id, 0, 0,
						   &conn->connector_id, 1, &mode);
				crtc_set = 1;
			} else {
				r = drmModePageFlip(card_fd, crtc_id, fb_id,
						    DRM_MODE_PAGE_FLIP_EVENT, NULL);
				if (!r) {
					struct pollfd pfd = { .fd = card_fd, .events = POLLIN };
					if (poll(&pfd, 1, 100) > 0) {
						drmEventContext ev = { .version = 2 };
						drmHandleEvent(card_fd, &ev);
					}
				}
			}
			if (r) { fprintf(stderr, "frame %d: flip: %s\n", i, strerror(errno)); break; }
		}
	}
	double dt = now_s() - t0;
	fprintf(stderr, "RESULT captured=%d/%d  %.1f fps (%.0f ms)\n",
		ok, nframes, ok / dt, dt * 1000);

	if (ppm && cap_bo && ok > 0) {
		uint32_t str; void *map_data = NULL;
		void *m = gbm_bo_map(cap_bo, 0, 0, cap_w, cap_h, GBM_BO_TRANSFER_READ,
				     &str, &map_data);
		if (m && m != MAP_FAILED) {
			FILE *fp = fopen(ppm, "w");
			if (fp) {
				fprintf(fp, "P6\n%u %u\n255\n", cap_w, cap_h);
				for (uint32_t y = 0; y < cap_h; y++) {
					uint8_t *row = (uint8_t*)m + y * str;
					for (uint32_t x = 0; x < cap_w; x++) {
						/* XRGB8888 little-endian: B,G,R,X */
						fputc(row[x*4+2], fp);
						fputc(row[x*4+1], fp);
						fputc(row[x*4+0], fp);
					}
				}
				fclose(fp);
				fprintf(stderr, "wrote %s (%ux%u)\n", ppm, cap_w, cap_h);
			}
			gbm_bo_unmap(cap_bo, map_data);
		} else {
			fprintf(stderr, "gbm_bo_map failed (content is GPU-only)\n");
		}
	}
	return ok == nframes ? 0 : 5;
}
