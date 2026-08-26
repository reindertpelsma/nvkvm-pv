/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_session_x11.c — the X11 backend.
 *
 * DISPLAY, AND WHY THERE IS NO GL HERE EITHER.  The guest's scanout dma-buf is
 * turned into an X pixmap with DRI3 (`DRI3PixmapFromBuffers`, or
 * `DRI3PixmapFromBuffer` for an implicitly-modified buffer) and put on screen
 * with `PresentPixmap`.  Both are wire requests: the fd is passed to the X
 * server over the X socket's own SCM_RIGHTS channel and the server does the
 * import.  No EGL, no GLX, no GL context, no libnvidia-eglcore in the broker.
 *
 * The findings document listed this as reasoning rather than a compile,
 * because the xcb-dri3/xcb-present headers were absent on the box where it was
 * written.  It is now compiled.  What is compiled is not what is *run*: see
 * README.md §7 for the honest verified/not-verified split.
 *
 * TWO WINDOWS, AND THE REASON IS PRESENT'S RULE.  PresentPixmap requires the
 * pixmap and the target window to have the SAME dimensions, and a
 * window-managed toplevel is sized by the window manager, not by us.  So the
 * toplevel is the input surface (it owns focus, the grab and fullscreen) and a
 * plain child window — which no window manager ever touches — is the present
 * target, resized to exactly whatever the guest just flipped.  Input events
 * the child does not select propagate up to the toplevel, which is standard
 * X11 event propagation, so the child needs no event mask at all.
 *
 * PURE XCB, NO XLIB.  Present delivers its events as XGE generic events on the
 * connection; mixing Xlib's event queue with xcb_poll_for_event() is the
 * classic way to lose them.  Doing the whole backend in xcb keeps one queue.
 *
 * GRAB.  XGrabKeyboard/XGrabPointer really is total for anything the X server
 * routes, window-manager bindings included.  FocusOut is the focus-loss signal
 * that lets the grab be offered at all.
 *
 * FORMATS.  DRI3 has no fourcc: a pixmap has a depth and a bits-per-pixel, and
 * the channel order comes from the visual.  XRGB8888 and ARGB8888 are the two
 * the nvkvm guest head advertises and both are imported as depth 24 / 32 bpp —
 * for a scanout the alpha channel is not composited, so dropping it is
 * correct, not a shortcut.  The acceptable MODIFIERS are whatever
 * DRI3GetSupportedModifiers reports for that depth/bpp, which is the X server
 * asking the driver — not a list written here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "nvkvm_broker.h"

#ifndef NB_HAVE_X11

struct nb_session *nb_session_x11(const struct nb_config *cfg)
{
    (void)cfg;
    nb_err("the X11 backend was not compiled in (xcb, xcb-dri3 or xcb-present "
           "missing at build time)");
    return NULL;
}

#else /* NB_HAVE_X11 */

#include <linux/input-event-codes.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#ifdef NB_HAVE_XCB_RENDER
#include <xcb/render.h>
#endif
#ifdef NB_HAVE_XCB_XINPUT
#include <xcb/xinput.h>
#endif

#define NB_DRM_FORMAT_MOD_INVALID  0x00ffffffffffffffULL

#define NB_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NB_FCC_XR24  NB_FOURCC('X', 'R', '2', '4')
#define NB_FCC_AR24  NB_FOURCC('A', 'R', '2', '4')

struct nb_x11_buf {
    bool         valid;
    uint64_t     id;                /* dma-buf inode                          */
    xcb_pixmap_t pixmap;
    uint32_t     w, h, stride, offset, fourcc;
    uint64_t     modifier;
    uint64_t     used;
    /* SHM tier: no pixmap at all, just a mapping we blit from. */
    void        *map;
    size_t       map_len;
};

struct nb_x11 {
    xcb_connection_t *c;
    xcb_screen_t     *screen;
    xcb_window_t      win;          /* toplevel: input, focus, grab, fs       */
    xcb_window_t      content;      /* child: the present target              */
    xcb_present_event_t present_eid;
    uint8_t           present_opcode;
#ifdef NB_HAVE_XCB_XINPUT
    uint8_t           xi_opcode;
    bool              have_xi;
#endif
    xcb_atom_t        a_wm_state, a_fs, a_wm_proto, a_wm_delete;
    /*
     * CLIPBOARD.  X11 gets the same boundary Wayland does -- the broker is the
     * only thing that touches either selection, the guest never reaches across
     * on its own, and a paste happens only when the user presses the paste key
     * in this window.  Which display server the host happens to run is not a
     * security parameter.
     */
    xcb_atom_t        a_clipboard, a_utf8, a_targets, a_prop, a_incr;
    xcb_atom_t        a_net_wm_name;
    xcb_timestamp_t   last_time;      /* a REAL event time; X rejects guesses */
    bool              focused;
    char             *src_text;       /* what we serve as CLIPBOARD owner     */
    size_t            src_len;
    char             *clip_pending;   /* a guest copy held until focus        */
    size_t            clip_pending_len;
    bool              fetch_active;   /* a paste is in flight                 */
    uint64_t          fetch_generation;
    xcb_visualid_t    visual24;       /* the content window's visual          */

    struct nb_formats formats;
    struct nb_x11_buf bufs[NB_MAX_BUFS];
    uint64_t tick;
    int      pending, current;
    uint32_t serial;

    struct nb_sink *sink;
    struct nb_session *sess;

    bool     quit;                  /* WM_DELETE_WINDOW with no client to
                                     * defer the decision to                  */
    int      last_mode;             /* last PresentCompleteNotify mode; -1 =
                                     * nothing completed yet                  */
    uint32_t last_raw_seq;          /* de-dup for double-delivered XI2 raw    */
    int      last_raw_dx, last_raw_dy;
    int  win_w, win_h;              /* toplevel size, from ConfigureNotify    */
    int  con_w, con_h;              /* content-window size we last asked for  */
    bool grabbed, fullscreen;
    bool ptr_inside;            /* pointer is over the CONTENT window     */
    unsigned hint_w, hint_h;    /* last size we asked the guest to render */
    char      title[128];       /* the plain window name, without status  */
    xcb_cursor_t blank_cursor;  /* shown while the pointer is the guest's */
    uint64_t  notice_until_ms;  /* title-bar clipboard notice deadline    */
    xcb_gcontext_t idle_gc;     /* for CPU blits: placeholder and dialog  */
    int       scale_mode;       /* NB_SCALE_*, from the command line      */
#ifdef NB_HAVE_XCB_RENDER
    xcb_render_pictformat_t pict_fmt;   /* depth-24 format, 0 = no render */
    xcb_render_picture_t    dst_pic;    /* the content window as a Picture */
#endif
    bool      idle_shown;
    bool      client_attached;  /* a VMM is connected, frames or not      */
    xcb_window_t dlg;           /* close-confirmation child, 0 = never made */
    bool      dlg_mapped;
    int       dlg_hot;          /* hovered row, -1 = none                 */
    bool      cursor_hidden;    /* what the content window currently shows */
};

/* Clipboard: defined below the presentation code, used from the event loop. */
static void x11_clip_flush_pending(struct nb_x11 *x);
static int  x11_show_idle(struct nb_session *s);
static void x11_dlg_show(struct nb_x11 *x);
static void x11_dlg_hide(struct nb_x11 *x);
static void x11_dlg_paint(struct nb_x11 *x);
static int  x11_dlg_hit(int px, int py);
static void x11_cursor_policy(struct nb_x11 *x);
static void x11_blit(struct nb_x11 *x, xcb_drawable_t d, xcb_gcontext_t gc,
                     const uint32_t *px, int w, int h);
static void x11_clip_serve(struct nb_x11 *x,
                           const xcb_selection_request_event_t *rq);
static void x11_clip_receive(struct nb_x11 *x, struct nb_sink *sink,
                             const xcb_selection_notify_event_t *sn);

/* ── small helpers ───────────────────────────────────────────────────────── */

static xcb_atom_t x11_atom(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name),
                                                 name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;

    free(r);
    return a;
}

/*
 * X11 keycodes are evdev keycodes + 8 on every Linux X server (the XKB "evdev"
 * ruleset defines them that way, and xf86-input-evdev/libinput both produce
 * them).  We send evdev codes on the wire so the packet format is identical
 * across backends, which is the point of the whole interface.
 */
static unsigned x11_kc_to_evdev(unsigned kc)
{
    return kc >= 8 ? kc - 8 : 0;
}

static unsigned x11_btn_to_evdev(unsigned b)
{
    switch (b) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    case 8: return BTN_SIDE;
    case 9: return BTN_EXTRA;
    default: return 0;
    }
}

/* ── formats ─────────────────────────────────────────────────────────────── */

static bool x11_format_ok(struct nb_session *s, uint32_t fourcc, uint64_t mod)
{
    struct nb_x11 *x = s->priv;

    return nb_formats_has(&x->formats, fourcc, mod);
}

/*
 * Ask the X SERVER which modifiers it will accept for a 24-bit/32bpp pixmap.
 * That is DRI3 relaying what the driver advertises, so it is the X-side
 * equivalent of zwp_linux_dmabuf_v1's modifier events — HARDENING 3 on this
 * backend.  Screen modifiers are included as well as window modifiers: the
 * window list is the subset that could be scanned out directly, and a broker
 * that accepted only those would reject perfectly displayable frames.
 *
 * ASK, DO NOT INFER FROM THE VERSION NUMBER.  This used to be gated on
 * DRI3QueryVersion reporting >= 1.2, which is what the specification says the
 * gate should be — and it is wrong in practice on the one driver that matters
 * here.  The NVIDIA DDX 580.105.08 reports DRI3 **1.0** and answers
 * GetSupportedModifiers anyway, with twelve block-linear modifiers including
 * the two src/guest/nvkvm_kms.c reads off real guest bos
 * (0x0300000000606014, 0x0300000000e08014).  Under the version gate the
 * broker never asked, advertised nothing but DRM_FORMAT_MOD_INVALID, and
 * would have rejected every frame a real guest ever flips — presenting as
 * "the window is black" with no attributable error, which is precisely the
 * failure mode the zwp_linux_dmabuf_v1 version cap avoids on the other
 * backend.  So: issue the request, and let the REPLY decide.  A server that
 * genuinely lacks it answers with an error, `r` is NULL, and the implicit
 * path below is all that is advertised — same outcome, arrived at honestly.
 *
 * Returns the number of explicitly-modified pairs learned, so the caller can
 * set CAP_MODIFIERS from what happened rather than from what was claimed.
 */
static unsigned x11_collect_formats(struct nb_x11 *x)
{
    xcb_dri3_get_supported_modifiers_reply_t *r;
    uint64_t *m;
    unsigned got = 0;
    int n, i;

    r = xcb_dri3_get_supported_modifiers_reply(
            x->c, xcb_dri3_get_supported_modifiers(x->c, x->content, 24, 32),
            NULL);
    if (!r) {
        nb_log("DRI3GetSupportedModifiers failed; only implicitly-modified "
               "buffers will be accepted on this server");
    } else {
        m = xcb_dri3_get_supported_modifiers_window_modifiers(r);
        n = xcb_dri3_get_supported_modifiers_window_modifiers_length(r);
        for (i = 0; i < n; i++) {
            nb_formats_add(&x->formats, NB_FCC_XR24, m[i]);
            nb_formats_add(&x->formats, NB_FCC_AR24, m[i]);
            got++;
        }
        /*
         * The window list can be empty while the screen list is not — that is
         * exactly what the NVIDIA DDX does (0 window, 12 screen).  A broker
         * that read only the window list would advertise nothing and reject
         * every frame, so both lists are read.
         */
        m = xcb_dri3_get_supported_modifiers_screen_modifiers(r);
        n = xcb_dri3_get_supported_modifiers_screen_modifiers_length(r);
        for (i = 0; i < n; i++) {
            nb_formats_add(&x->formats, NB_FCC_XR24, m[i]);
            nb_formats_add(&x->formats, NB_FCC_AR24, m[i]);
            got++;
        }
        free(r);
    }
    /*
     * The implicit layout is always available: DRI3 1.0's PixmapFromBuffer
     * takes no modifier and lets the server work it out.  Listed explicitly so
     * it goes through the same nb_formats_has() gate as everything else rather
     * than being a special case in the validator.
     */
    nb_formats_add(&x->formats, NB_FCC_XR24, NB_DRM_FORMAT_MOD_INVALID);
    nb_formats_add(&x->formats, NB_FCC_AR24, NB_DRM_FORMAT_MOD_INVALID);
    return got;
}

/* ── buffers ─────────────────────────────────────────────────────────────── */

static void x11_buf_free(struct nb_x11 *x, int i)
{
    if (!x->bufs[i].valid) {
        return;
    }
    if (x->bufs[i].pixmap) {
        xcb_free_pixmap(x->c, x->bufs[i].pixmap);
    }
    if (x->bufs[i].map) {
        munmap(x->bufs[i].map, x->bufs[i].map_len);
    }
    memset(&x->bufs[i], 0, sizeof(x->bufs[i]));
}

static int x11_attach(struct nb_session *s, const struct nb_buf_desc *d)
{
    struct nb_x11 *x = s->priv;
    xcb_pixmap_t pix;
    xcb_void_cookie_t ck;
    xcb_generic_error_t *err;
    int i, victim = -1, dupfd;
    uint64_t oldest = UINT64_MAX;

    x->tick++;
    /*
     * THE TIER THAT CANNOT BE REFUSED, on X11 too.
     *
     * `is_shm` was documented as "presented through wl_shm", and only Wayland
     * implemented it -- so the universal last resort was universal on exactly
     * one backend.  On X11 the equivalent is core-protocol PutImage from the
     * mapping: not MIT-SHM, deliberately, because an SHM pixmap derives its
     * stride from its width and cannot express the guest's, and a tier whose
     * job is to always work must not depend on the stride happening to match
     * or on an extension being present.  It copies; that is the price of the
     * rung, and it is only reached when both dma-buf rungs have failed.
     */
    if (d->is_shm) {
        size_t need = (size_t)d->stride * d->height + d->offset;
        void *m;

        if (d->size < need) {
            nb_err("ATTACH: shm buffer is %llu bytes, needs %zu",
                   (unsigned long long)d->size, need);
            return -EINVAL;
        }
        m = mmap(NULL, need, PROT_READ, MAP_SHARED, d->fd, 0);
        if (m == MAP_FAILED) {
            nb_err("ATTACH: could not map the shm buffer: %s", strerror(errno));
            return -errno;
        }
        for (i = 0; i < NB_MAX_BUFS; i++) {
            if (i != x->current && i != x->pending) {
                break;
            }
        }
        if (i >= NB_MAX_BUFS) {
            munmap(m, need);
            return -EBUSY;
        }
        x11_buf_free(x, i);
        x->bufs[i].valid  = true;
        x->bufs[i].id     = d->id;
        x->bufs[i].w      = d->width;
        x->bufs[i].h      = d->height;
        x->bufs[i].stride = d->stride;
        x->bufs[i].offset = d->offset;
        x->bufs[i].fourcc = d->fourcc;
        x->bufs[i].map     = m;
        x->bufs[i].map_len = need;
        x->bufs[i].used   = x->tick;
        x->pending = i;
        return 0;
    }
    for (i = 0; i < NB_MAX_BUFS; i++) {
        struct nb_x11_buf *sl = &x->bufs[i];

        if (sl->valid && sl->id == d->id && sl->w == d->width &&
            sl->h == d->height && sl->stride == d->stride &&
            sl->offset == d->offset && sl->fourcc == d->fourcc &&
            sl->modifier == d->modifier) {
            sl->used = x->tick;
            x->pending = i;
            return 0;
        }
    }
    for (i = 0; i < NB_MAX_BUFS; i++) {
        if (i == x->current || i == x->pending) {
            continue;
        }
        if (!x->bufs[i].valid) {
            victim = i;
            break;
        }
        if (x->bufs[i].used < oldest) {
            oldest = x->bufs[i].used;
            victim = i;
        }
    }
    if (victim < 0) {
        return -ENOSPC;
    }

    /*
     * xcb takes ownership of every fd handed to it and closes it once sent
     * (xcbext.h: "the file descriptor given is owned by xcb").  Our caller
     * owns desc->fd, so we must hand over a duplicate.
     */
    dupfd = fcntl(d->fd, F_DUPFD_CLOEXEC, 0);
    if (dupfd < 0) {
        return -errno;
    }

    pix = xcb_generate_id(x->c);
    if (d->modifier == NB_DRM_FORMAT_MOD_INVALID) {
        /*
         * THE IMPLICIT PATH IS NOT SAFE FOR A BUFFER THAT IS NOT REALLY
         * LINEAR, and it fails silently.  Measured on the NVIDIA DDX
         * 580.105.08: a block-linear bo (0x0300000000606014) handed to
         * PixmapFromBuffer is accepted with no X error and then presented as
         * shredded scanlines — the server reads the tiled bytes as linear.
         * There is no error to catch and no way to tell from this side, so the
         * only defence is to say out loud that the client asked for it.  The
         * geometry bound still holds either way: nothing is read past
         * offset + stride*height, which was checked against the real fd size.
         */
        if (x->formats.n > 2) {
            nb_log("WARNING: importing via the implicit (no-modifier) DRI3 1.0 "
                   "path on a server that DOES advertise explicit modifiers. "
                   "If this buffer is not genuinely linear the image will be "
                   "silently garbled, not rejected. The client declared "
                   "DRM_FORMAT_MOD_INVALID; it should declare the real "
                   "modifier.");
        }
        /* DRI3 1.0.  `size` is the whole buffer; the server derives the rest. */
        uint32_t size = (uint32_t)(d->size > 0xffffffffu ? 0xffffffffu
                                                         : d->size);
        ck = xcb_dri3_pixmap_from_buffer_checked(
                 x->c, pix, x->content, size,
                 (uint16_t)d->width, (uint16_t)d->height,
                 (uint16_t)d->stride, 24, 32, dupfd);
    } else {
        /* DRI3 1.2.  Single plane; the other three strides/offsets are 0. */
        int32_t fds[1] = { dupfd };

        ck = xcb_dri3_pixmap_from_buffers_checked(
                 x->c, pix, x->content, 1,
                 (uint16_t)d->width, (uint16_t)d->height,
                 d->stride, d->offset, 0, 0, 0, 0, 0, 0,
                 24, 32, d->modifier, fds);
    }
    /*
     * _checked + request_check is a round trip, but it is the only way to hear
     * "the server refused this buffer" at all: an unchecked DRI3 error arrives
     * later as an event with nothing to attribute it to, and the pixmap id
     * silently refers to nothing.  It happens once per NEW buffer — three or
     * four times for the whole life of a VM, not once per frame — because the
     * cache above catches every repeat.
     */
    err = xcb_request_check(x->c, ck);
    if (err) {
        nb_err("DRI3 pixmap import refused: X error %u (major %u minor %u)",
               err->error_code, err->major_code, err->minor_code);
        free(err);
        return -EINVAL;
    }

    x11_buf_free(x, victim);
    x->bufs[victim] = (struct nb_x11_buf){
        .valid = true, .id = d->id, .pixmap = pix,
        .w = d->width, .h = d->height, .stride = d->stride,
        .offset = d->offset, .fourcc = d->fourcc, .modifier = d->modifier,
        .used = x->tick,
    };
    x->pending = victim;
    return 0;
}

static void x11_size_content(struct nb_x11 *x, int w, int h)
{
    uint32_t vals[4];
    int px = (x->win_w - w) / 2, py = (x->win_h - h) / 2;

    if (px < 0) { px = 0; }
    if (py < 0) { py = 0; }
    vals[0] = (uint32_t)px;
    vals[1] = (uint32_t)py;
    vals[2] = (uint32_t)w;
    vals[3] = (uint32_t)h;
    xcb_configure_window(x->c, x->content,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         vals);
    x->con_w = w;
    x->con_h = h;
    /*
     * Repaint the letterbox NOW.  The background above makes the exposed area
     * a colour, but X only paints it on expose, and shrinking a child does not
     * always generate one for the parent.  Clearing explicitly is one request
     * and removes the whole class of "stale pixels beside the guest".
     */
    xcb_clear_area(x->c, 0, x->win, 0, 0, 0, 0);
}

/*
 * WHERE THE GUEST'S FRAME GOES IN THE WINDOW.
 *
 * The Wayland backend has wp_viewport and can ask the compositor to scale into
 * an arbitrary rectangle.  X11's Present cannot scale at all -- it blits 1:1 --
 * so a window that is not exactly the guest's mode showed bars on BOTH axes and
 * the guest's picture stayed small until the guest happened to re-mode to match.
 * That made --scale a no-op on this backend and left the result depending on the
 * guest, which is precisely what it should not do.
 *
 * So compute the destination here, the same way Wayland does, and let
 * x11_present_scaled() get the pixels there.
 */
static void x11_dest_rect(struct nb_x11 *x, int sw, int sh, int *dw, int *dh)
{
    if (x->scale_mode == NB_SCALE_STRETCH) {
        *dw = x->win_w; *dh = x->win_h;
    } else if (x->scale_mode == NB_SCALE_ASPECT &&
               sw > 0 && sh > 0 && x->win_w > 0 && x->win_h > 0) {
        /* Largest rectangle of the source's aspect that fits the window. */
        long by_w = (long)x->win_w * sh;
        long by_h = (long)x->win_h * sw;

        if (by_w <= by_h) {         /* width-limited */
            *dw = x->win_w;
            *dh = (int)((long)x->win_w * sh / sw);
        } else {                    /* height-limited */
            *dh = x->win_h;
            *dw = (int)((long)x->win_h * sw / sh);
        }
    } else {
        *dw = sw; *dh = sh;         /* NB_SCALE_NONE: 1:1, centred */
    }
    if (*dw < 1) { *dw = 1; }
    if (*dh < 1) { *dh = 1; }
}

#ifdef NB_HAVE_XCB_RENDER
/* Scale one buffer into the content window with the X server's own compositor.
 * Returns false if Render is unusable, so the caller can fall back to Present. */
static bool x11_render_scaled(struct nb_x11 *x, struct nb_x11_buf *sl,
                              int dw, int dh)
{
    xcb_render_picture_t src;
    xcb_render_transform_t tr;
    static const char filter[] = "bilinear";

    if (!x->pict_fmt) {
        return false;
    }
    if (!x->dst_pic) {
        x->dst_pic = xcb_generate_id(x->c);
        xcb_render_create_picture(x->c, x->dst_pic, x->content, x->pict_fmt,
                                  0, NULL);
    }
    src = xcb_generate_id(x->c);
    xcb_render_create_picture(x->c, src, sl->pixmap, x->pict_fmt, 0, NULL);

    /*
     * The transform maps DESTINATION coordinates back to the SOURCE, so it
     * carries src/dst, not dst/src.  16.16 fixed point.
     */
    memset(&tr, 0, sizeof tr);
    tr.matrix11 = (xcb_render_fixed_t)(((int64_t)sl->w << 16) / (dw > 0 ? dw : 1));
    tr.matrix22 = (xcb_render_fixed_t)(((int64_t)sl->h << 16) / (dh > 0 ? dh : 1));
    tr.matrix33 = 1 << 16;
    xcb_render_set_picture_transform(x->c, src, tr);
    xcb_render_set_picture_filter(x->c, src, sizeof filter - 1, filter, 0, NULL);
    xcb_render_composite(x->c, XCB_RENDER_PICT_OP_SRC, src,
                         XCB_RENDER_PICTURE_NONE, x->dst_pic,
                         0, 0, 0, 0, 0, 0, (uint16_t)dw, (uint16_t)dh);
    xcb_render_free_picture(x->c, src);
    return true;
}
#endif

static int x11_commit(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_x11 *x = s->priv;
    struct nb_x11_buf *sl;

    if (x->pending < 0) {
        return -ENOENT;
    }
    sl = &x->bufs[x->pending];
    if (!sl->valid) {
        x->pending = -1;
        return -ENOENT;
    }

    /*
     * SHM tier: there is no pixmap to present, only bytes to push.  Packed
     * into a temporary first when the guest's stride is not width*4, because
     * PutImage carries rows packed and cannot be told otherwise.
     */
    if (sl->map) {
        const uint8_t *src = (const uint8_t *)sl->map + sl->offset;
        unsigned packed = sl->w * 4u;
        uint32_t *tmp = NULL;
        int dw, dh;

        x11_dest_rect(x, (int)sl->w, (int)sl->h, &dw, &dh);
        if (x->con_w != (int)sl->w || x->con_h != (int)sl->h) {
            /* No scaler on this rung: XRender needs a Picture and there is no
             * pixmap here.  1:1 and centred, which is what "it always works"
             * costs. */
            x11_size_content(x, (int)sl->w, (int)sl->h);
        }
        if (!x->idle_gc) {
            x->idle_gc = xcb_generate_id(x->c);
            xcb_create_gc(x->c, x->idle_gc, x->win, 0, NULL);
        }
        if (sl->stride != packed) {
            unsigned y;

            tmp = malloc((size_t)packed * sl->h);
            if (!tmp) {
                return -ENOMEM;
            }
            for (y = 0; y < sl->h; y++) {
                memcpy((uint8_t *)tmp + (size_t)y * packed,
                       src + (size_t)y * sl->stride, packed);
            }
            src = (const uint8_t *)tmp;
        }
        x11_blit(x, x->content, x->idle_gc, (const uint32_t *)src,
                 (int)sl->w, (int)sl->h);
        xcb_flush(x->c);
        free(tmp);
        x->current = x->pending;
        x->pending = -1;
        x->idle_shown = false;
        x11_cursor_policy(x);
        /* PutImage copies, so the buffer is the guest's again immediately. */
        nb_sink_release(sink, sl->id);
        return 0;
    }

    {
        int dw, dh;

        x11_dest_rect(x, (int)sl->w, (int)sl->h, &dw, &dh);
        if (x->con_w != dw || x->con_h != dh) {
            x11_size_content(x, dw, dh);
        }
#ifdef NB_HAVE_XCB_RENDER
        /*
         * Only when it is actually a different size.  At 1:1 Present is the
         * better path -- it can reach a hardware plane, which a composite
         * never can -- so scaling costs nothing when nothing needs scaling.
         */
        if ((dw != (int)sl->w || dh != (int)sl->h) &&
            x11_render_scaled(x, sl, dw, dh)) {
            xcb_flush(x->c);
            x->current = x->pending;
            x->pending = -1;
            x->idle_shown = false;
            x11_cursor_policy(x);
            /* Composite copies, so the buffer is free the moment the server
             * has read it -- there is no PresentIdleNotify coming for it. */
            nb_sink_release(sink, sl->id);
            return 0;
        }
#endif
    }

    xcb_present_pixmap(x->c, x->content, sl->pixmap, ++x->serial,
                       XCB_NONE /* valid */, XCB_NONE /* update */,
                       0, 0, XCB_NONE /* target_crtc */,
                       XCB_NONE /* wait_fence */, XCB_NONE /* idle_fence */,
                       0 /* options */, 0 /* target_msc: as soon as possible */,
                       0, 0, 0, NULL);
    xcb_flush(x->c);

    x->current = x->pending;
    x->pending = -1;
    x->idle_shown = false;
    x11_cursor_policy(x);       /* a frame is up: the guest draws the cursor */
    /*
     * DO NOT REPORT THE SIZE WE JUST PRESENTED.
     *
     * EV_SURFACE is an INSTRUCTION -- "guest, render this size" -- and the
     * only thing entitled to issue it is the WINDOW, from ConfigureNotify.
     * What arrives here is an OBSERVATION: the size the guest already chose.
     * Sending it back told the guest to render what it had just rendered, so
     * on any window whose size differs from the guest's mode the two traded
     * places on every frame -- the content child was reconfigured and the
     * pixmaps recreated each time, which is what the corruption after a resize
     * actually was.
     *
     * The Wayland backend carries the same rule and the same warning; this
     * call is the one that was left behind.
     */
    (void)sink;
    return 0;
}

static int x11_resize(struct nb_session *s, unsigned w, unsigned h)
{
    struct nb_x11 *x = s->priv;
    uint32_t vals[2] = { w, h };

    /*
     * A request, not a command: the window manager decides.  Whatever it
     * settles on comes back as ConfigureNotify and is reported as EV_SURFACE,
     * so the client is never told a size that did not happen.
     */
    xcb_configure_window(x->c, x->win,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         vals);
    xcb_flush(x->c);
    return 0;
}

/* ── input / event loop ──────────────────────────────────────────────────── */

static int x11_pollfds(struct nb_session *s, struct pollfd *out, int max)
{
    struct nb_x11 *x = s->priv;

    if (max < 1) {
        return 0;
    }
    /* Non-blocking: xcb_flush writes what it can and buffers the rest. */
    xcb_flush(x->c);
    out[0].fd = xcb_get_file_descriptor(x->c);
    out[0].events = POLLIN;
    out[0].revents = 0;
    return 1;
}

static void x11_present_event(struct nb_x11 *x, struct nb_sink *sink,
                              xcb_ge_generic_event_t *ge)
{
    switch (ge->event_type) {
    case XCB_PRESENT_COMPLETE_NOTIFY: {
        xcb_present_complete_notify_event_t *ce = (void *)ge;

        /*
         * THE ONE PLACE EITHER STACK TELLS YOU WHETHER YOU GOT A PLANE.
         *
         * README §6 says a hardware plane can only be qualified for, never
         * requested, and that the compositor will not tell you.  On X11 that
         * is not quite true: PresentCompleteNotify carries `mode`, and FLIP
         * means the frame went to the CRTC instead of being composited.  It
         * costs nothing to read, and without it "is this actually zero-copy
         * to the screen" is unanswerable from the outside.  Logged on every
         * CHANGE, not every frame, so it stays one line in normal use.
         */
        if (ce->mode != x->last_mode) {
            static const char *modes[] = { "COPY", "FLIP", "SKIP",
                                           "SUBOPTIMAL_COPY" };

            x->last_mode = ce->mode;
            nb_log("Present: %s%s", ce->mode < 4 ? modes[ce->mode] : "?",
                   ce->mode == XCB_PRESENT_COMPLETE_MODE_FLIP
                       ? "  — the frame reached a hardware plane; nothing "
                         "composited it"
                       : "  — the frame is being composited, not scanned out");
        }
        /* Pacing.  This is the X11 equivalent of a wl frame callback: the
         * frame is on screen, another may be drawn. */
        nb_sink_frame(sink);
        break;
    }
    case XCB_PRESENT_IDLE_NOTIFY: {
        xcb_present_idle_notify_event_t *ie = (void *)ge;
        int i;

        for (i = 0; i < NB_MAX_BUFS; i++) {
            if (x->bufs[i].valid && x->bufs[i].pixmap == ie->pixmap) {
                nb_sink_release(sink, x->bufs[i].id);
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}

static int x11_dispatch(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_x11 *x = s->priv;
    xcb_generic_event_t *ev;

    x->sink = sink;

    if (xcb_connection_has_error(x->c)) {
        return -EIO;
    }
    while ((ev = xcb_poll_for_event(x->c)) != NULL) {
        switch (ev->response_type & 0x7f) {
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT: {
            xcb_focus_in_event_t *f = (void *)ev;

            /*
             * A GRAB IS NOT A FOCUS CHANGE, AND X11 REPORTS IT AS ONE.
             *
             * FocusOut/FocusIn carry a `mode`.  NotifyNormal and
             * NotifyWhileGrabbed mean the keyboard focus really moved.
             * NotifyGrab and NotifyUngrab mean *this* client's own
             * XGrabKeyboard took or released the keyboard — the window has
             * not lost anything.  Acting on those is self-defeating here,
             * because the grab-induced FocusOut arrives immediately after
             * CTRL+ALT+G and the focus-loss rule then drops the grab that
             * caused it.  Measured on X.Org 1.21.1.4 with the NVIDIA DDX and
             * KWin: every grab lasted a few milliseconds —
             *     GRAB x=1 ; FOCUS x=0 ; GRAB x=0
             * — so grab on the X11 backend never actually held.  The selftest
             * could not have caught it: --backend test synthesises focus from
             * stdin and has no X server to generate a NotifyGrab.
             *
             * The security property is unchanged: a REAL focus loss is still
             * NotifyNormal/NotifyWhileGrabbed and still drops the grab.  Only
             * the two self-inflicted modes are ignored.
             */
            if (f->mode == XCB_NOTIFY_MODE_GRAB ||
                f->mode == XCB_NOTIFY_MODE_UNGRAB) {
                break;
            }
            /* The property that stops the grab being a keylogger. */
            x->focused = (ev->response_type & 0x7f) == XCB_FOCUS_IN;
            nb_sink_focus(sink, x->focused);
            /* A copy the guest made while we were unfocused is applied HERE,
             * which is also the first moment the user can see it happen. */
            x11_clip_flush_pending(x);
            break;
        }
        case XCB_SELECTION_REQUEST:
            x11_clip_serve(x, (xcb_selection_request_event_t *)ev);
            break;

        case XCB_SELECTION_CLEAR:
            /* Someone else took the clipboard.  Ours is stale; drop it rather
             * than keep serving the guest's text after the user moved on. */
            free(x->src_text);
            x->src_text = NULL;
            x->src_len = 0;
            break;

        case XCB_SELECTION_NOTIFY:
            x11_clip_receive(x, sink, (xcb_selection_notify_event_t *)ev);
            break;

        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *cm = (void *)ev;

            /*
             * WM_DELETE_WINDOW -- the title bar's X, drawn by the window
             * manager on this backend.  The atom was already advertised in
             * WM_PROTOCOLS; without this case the WM sent it and nothing
             * listened, so the close button did nothing at all.
             *
             * Same meaning and same path as the Wayland backend: tell the
             * VMM and let it decide.  The broker does not close a VM.
             */
            if (cm->type == x->a_wm_proto &&
                cm->data.data32[0] == x->a_wm_delete) {
                /*
                 * No confirmation overlay on this backend yet -- the Wayland
                 * one draws its own, and the equivalent here is a second
                 * override-redirect window plus its own hit-testing.  Until
                 * that exists, take the graceful choice, which is the same
                 * default the Wayland dialog offers on Enter.
                 */
                /*
                 * ASK, do not decide.  WM_DELETE_WINDOW is a request, and the
                 * three possible answers -- shut the guest down, force it off,
                 * or just close the display -- are not interchangeable.  With
                 * no VM attached there is nothing to ask about, so the X really
                 * does mean close.
                 */
                if (x->current < 0 && x->idle_shown) {
                    x->quit = true;
                } else {
                    x11_dlg_show(x);
                }
            }
            break;
        }
        case XCB_EXPOSE:
            if (x->dlg_mapped) {
                x11_dlg_paint(x);
                break;
            }
            /* Only meaningful while the placeholder is what is on screen; a
             * live guest repaints itself on the next present. */
            if (x->idle_shown && x->current < 0) {
                x11_show_idle(s);
            }
            break;
        case XCB_ENTER_NOTIFY:
            /* Deliberately NOT asserting "inside" here: entering the toplevel
             * may mean entering the letterbox.  Motion decides, above. */
            break;
        case XCB_LEAVE_NOTIFY: {
            xcb_leave_notify_event_t *l = (void *)ev;

            /* NotifyInferior means the pointer moved from this window INTO its
             * own child -- the content window.  That is not leaving. */
            if (l->detail != XCB_NOTIFY_DETAIL_INFERIOR && x->ptr_inside) {
                x->ptr_inside = false;
                nb_sink_pointer(sink, false);
            }
            break;
        }
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE: {
            xcb_key_press_event_t *k = (void *)ev;

            /*
             * Keep a REAL server timestamp.  Taking and converting a selection
             * both need one -- ICCCM says use the time of the event that
             * prompted it, and XCB_CURRENT_TIME is exactly what a strict owner
             * rejects.  The paste key is a key event, so this is always fresh
             * by the time it matters.
             */
            x->last_time = k->time;

            /*
             * RECEIVING A KEY IS PROOF OF FOCUS.  The X server only routes key
             * events to the focus window, so if we are holding one and still
             * believe we are unfocused, our belief is what is wrong.
             *
             * That happens for real: FocusIn is edge-triggered, openbox
             * focuses a window when it maps -- before the VM has connected --
             * and comparing GetInputFocus against our own window id fails
             * under a REPARENTING window manager, which is most of them.
             * MEASURED: CTRL+ALT+G toggled the grab (handled before the focus
             * gate) while no key or pointer event ever reached the guest, and
             * the log carried not one "window active" line.
             */
            if (!x->focused) {
                x->focused = true;
                nb_sink_focus(sink, true);
                x11_clip_flush_pending(x);
            }
            if (x->dlg_mapped) {
                /* Modal: ESC dismisses, everything else is swallowed rather
                 * than half-delivered to a guest that is being asked about. */
                if (x11_kc_to_evdev(k->detail) == 1 /* KEY_ESC */ &&
                    (ev->response_type & 0x7f) == XCB_KEY_RELEASE) {
                    x11_dlg_hide(x);
                }
                break;
            }
            nb_sink_key(sink, x11_kc_to_evdev(k->detail),
                        (ev->response_type & 0x7f) == XCB_KEY_PRESS);
            break;
        }
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            xcb_button_press_event_t *b = (void *)ev;
            bool down = (ev->response_type & 0x7f) == XCB_BUTTON_PRESS;
            unsigned d = b->detail;

            x->last_time = b->time;
            if (x->dlg_mapped) {
                /* PRESS ARMS, RELEASE ACTS -- none of these four should fire
                 * from a mis-click, and a click anywhere else dismisses. */
                if (b->event != x->dlg) {
                    if (!down) {
                        x11_dlg_hide(x);
                    }
                    break;
                }
                if (!down) {
                    int hit = x11_dlg_hit(b->event_x, b->event_y);

                    x11_dlg_hide(x);
                    if (hit == 0) {
                        if (!nb_sink_close_request(sink,
                                NVKVM_BROKER_CLOSE_POWERDOWN)) {
                            x->quit = true;
                        }
                    } else if (hit == 1) {
                        if (!nb_sink_close_request(sink,
                                NVKVM_BROKER_CLOSE_FORCE)) {
                            x->quit = true;
                        }
                    } else if (hit == 2) {
                        nb_sink_detach(sink, "the user closed the display");
                    }
                }
                break;      /* never reaches the guest while the dialog is up */
            }

            if (d == 4 || d == 5) {
                if (down) {
                    nb_sink_wheel(sink, d == 4 ? 1 : -1, 0);
                }
            } else if (d == 6 || d == 7) {
                if (down) {
                    nb_sink_wheel(sink, 0, d == 6 ? -1 : 1);
                }
            } else {
                unsigned e = x11_btn_to_evdev(d);

                if (e) {
                    nb_sink_btn(sink, e, down);
                }
            }
            break;
        }
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *m = (void *)ev;
            int cx = m->event_x, cy = m->event_y;

            if (x->dlg_mapped) {
                if (m->event == x->dlg) {
                    int hit = x11_dlg_hit(m->event_x, m->event_y);

                    if (hit != x->dlg_hot) {
                        x->dlg_hot = hit;
                        x11_dlg_paint(x);
                    }
                }
                break;
            }

            /*
             * Map into the CONTENT window, which is the guest's framebuffer.
             * Motion arrives from whichever window it happened over, and the
             * content child is letterboxed inside the toplevel -- so reporting
             * toplevel coordinates against the toplevel size put the guest
             * pointer at the wrong place by exactly the letterbox offset, and
             * scaled it wrongly too.
             */
            if (m->event != x->content) {
                cx -= (x->win_w - x->con_w) / 2;
                cy -= (x->win_h - x->con_h) / 2;
            }
            if (x->con_w <= 0 || x->con_h <= 0) {
                break;
            }
            if (cx < 0 || cy < 0 || cx >= x->con_w || cy >= x->con_h) {
                /* Over the letterbox, not over the guest. */
                if (x->ptr_inside) {
                    x->ptr_inside = false;
                    nb_sink_pointer(sink, false);
                }
                break;
            }
            /* Motion over the content IS the pointer being inside it. */
            if (!x->ptr_inside) {
                x->ptr_inside = true;
                nb_sink_pointer(sink, true);
            }
            nb_sink_abs(sink, cx, cy, (unsigned)x->con_w, (unsigned)x->con_h);
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *c = (void *)ev;

            if (c->window == x->win) {
                unsigned hw, hh;

                x->win_w = c->width;
                x->win_h = c->height;
                /*
                 * ALIGN THE HINT DOWN TO A MULTIPLE OF 8, exactly as the
                 * Wayland backend does and for the same reason: the guest
                 * builds its mode with drm_cvt_mode(), whose CVT_H_GRANULARITY
                 * is 8, so an odd width produces a mode whose stride is not the
                 * one the framebuffer was allocated with.
                 *
                 * MEASURED: dragging the window told the guest "host window is
                 * 2731x1303" and the picture came back skewed and duplicated
                 * with bands of stripes -- a classic pitch mismatch, and 2731
                 * is not a multiple of 8.  Down, never up, so the suggestion
                 * still fits the window we measured.
                 */
                hw = (unsigned)c->width & ~7u;
                hh = (unsigned)c->height;
                if (hw == 0 || hh == 0) {
                    break;
                }
                /* Idempotent: a configure that did not change the hint is not
                 * news, and a re-mode the guest does not need is a visible
                 * flicker at best. */
                if (x->idle_shown && x->current < 0) {
                    x11_show_idle(s);   /* the placeholder is window-sized */
                }
                if (hw == x->hint_w && hh == x->hint_h) {
                    if (x->con_w > 0 && x->current >= 0) {
                        x11_size_content(x, x->con_w, x->con_h);
                    }
                    break;
                }
                x->hint_w = hw;
                x->hint_h = hh;
                nb_sink_surface(sink, hw, hh, 0 /* no refresh source */);
                /* Keep the content window centred in its new parent. */
                if (x->con_w > 0) {
                    x11_size_content(x, x->con_w, x->con_h);
                }
            }
            break;
        }
        case XCB_GE_GENERIC: {
            xcb_ge_generic_event_t *ge = (void *)ev;

            if (ge->extension == x->present_opcode) {
                x11_present_event(x, sink, ge);
            }
#ifdef NB_HAVE_XCB_XINPUT
            else if (x->have_xi && ge->extension == x->xi_opcode &&
                     ge->event_type == XCB_INPUT_RAW_MOTION) {
                /*
                 * XI2 raw motion is the only source of true relative deltas
                 * under a pointer grab: core MotionNotify is clamped once the
                 * pointer hits the confine boundary, so a fast turn in a game
                 * would simply stop.
                 */
                xcb_input_raw_motion_event_t *re = (void *)ev;
                xcb_input_fp3232_t *vals =
                    xcb_input_raw_button_press_axisvalues_raw(re);
                int nvals =
                    xcb_input_raw_button_press_axisvalues_raw_length(re);
                int dx = 0, dy = 0;

                if (nvals >= 1) {
                    dx = (int)vals[0].integral;
                }
                if (nvals >= 2) {
                    dy = (int)vals[1].integral;
                }
                if (nb_verbose) {
                    nb_log("XI RawMotion seq=%u dev=%u src=%u dx=%d dy=%d",
                           (unsigned)re->full_sequence,
                           (unsigned)re->deviceid, (unsigned)re->sourceid,
                           dx, dy);
                }
                /*
                 * UNDER AN ACTIVE GRAB THE SERVER DELIVERS EACH RAW MOTION
                 * TWICE, and the guest turns at exactly double speed.
                 *
                 * Measured on X.Org 1.21.1.4 + NVIDIA DDX 580.105.08: with
                 * the pointer grabbed, one physical motion arrives once via
                 * the root-window raw selection and once more because this
                 * client also holds the grab.  Both copies carry the SAME
                 * full_sequence, the same deviceid and the same valuators —
                 * they are one event delivered twice, not two events:
                 *     RawMotion seq=23 dev=2 src=4 dx=33 dy=44
                 *     RawMotion seq=23 dev=2 src=4 dx=33 dy=44
                 * Ungrabbed, the same motion arrives exactly once.  Nothing
                 * without a real X server and a real grab can see this, which
                 * is why it survived 42 selftests.
                 *
                 * Dropped by matching sequence AND deltas, which is the
                 * conservative pairing: a genuine second motion that shares a
                 * serial almost always differs in its deltas, and if it does
                 * not, the cost is one lost delta rather than a permanent 2x.
                 * The clean fix is to grab with XIGrabDevice instead of core
                 * XGrabPointer, which does not double-deliver — that is a
                 * larger change to the grab path and is left deliberate
                 * rather than smuggled in behind a bug fix.
                 */
                if (x->grabbed && re->full_sequence == x->last_raw_seq &&
                    dx == x->last_raw_dx && dy == x->last_raw_dy) {
                    x->last_raw_seq = 0;   /* only ever drop the pair's twin */
                    break;
                }
                x->last_raw_seq = re->full_sequence;
                x->last_raw_dx = dx;
                x->last_raw_dy = dy;
                nb_sink_rel(sink, dx, dy);
            }
#endif
            break;
        }
        case 0: {
            xcb_generic_error_t *e = (void *)ev;

            /* Asynchronous X errors: log, do not die.  A refused present is a
             * lost frame, not a lost session. */
            nb_log("X error %u (major %u minor %u)", e->error_code,
                   e->major_code, e->minor_code);
            break;
        }
        default:
            break;
        }
        free(ev);
    }
    if (xcb_connection_has_error(x->c)) {
        return -EIO;
    }
    if (x->quit) {
        /* The close box with nobody connected.  Same shape as the display
         * going away: main() says goodbye and tears the session down. */
        return -ECONNRESET;
    }
    return 0;
}

/*
 * STATUS IN THE WINDOW TITLE.  The Wayland backend draws its own title bar and
 * writes the status into it; on X11 the window manager owns the decoration, so
 * the only channel is the name itself.  Same rule as there: status AND title
 * when both fit, status alone when they do not, because a status nobody can
 * read is not worth the space.
 */
static void x11_set_status(struct nb_x11 *x, const char *status)
{
    char buf[256];
    const char *txt = x->title;

    if (status && x->title[0]) {
        snprintf(buf, sizeof buf, "%s - %s", status, x->title);
        txt = buf;
    } else if (status) {
        txt = status;
    }
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, x->a_net_wm_name,
                        x->a_utf8, 8, (uint32_t)strlen(txt), txt);
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, (uint32_t)strlen(txt), txt);
    xcb_flush(x->c);
}

/* An empty cursor, so the host pointer disappears over the guest's own. */
static void x11_cursor_init(struct nb_x11 *x)
{
    xcb_pixmap_t pix = xcb_generate_id(x->c);

    x->blank_cursor = xcb_generate_id(x->c);
    xcb_create_pixmap(x->c, 1, pix, x->win, 1, 1);
    xcb_create_cursor(x->c, x->blank_cursor, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
    xcb_free_pixmap(x->c, pix);
}

/*
 * WHO OWNS THE POINTER, decided in one place.
 *
 * The blank cursor is an attribute of the CONTENT window, so X applies it
 * exactly while the pointer is over the guest's picture and hands the normal
 * one back over the letterbox and the window manager's frame -- no enter/leave
 * bookkeeping, and no way for the two to disagree.
 *
 * Hidden whenever the guest is drawing its own cursor underneath, which is any
 * time a real frame is on screen: the guest gets absolute positions in normal
 * mode just as it does under grab, so "two cursors" is not a grab-only
 * problem.  It was, however, only fixed for grab first time round.
 *
 * Visible again for the placeholder (no guest, no guest cursor) and for the
 * close dialog (the user is being asked a question and has to answer it).
 */
static void x11_cursor_policy(struct nb_x11 *x)
{
    bool hide = x->current >= 0 && !x->idle_shown && !x->dlg_mapped;
    uint32_t v;

    if (!x->blank_cursor || hide == x->cursor_hidden) {
        return;                 /* idempotent: this runs on every frame */
    }
    x->cursor_hidden = hide;
    v = hide ? x->blank_cursor : XCB_CURSOR_NONE;
    xcb_change_window_attributes(x->c, x->content, XCB_CW_CURSOR, &v);
    xcb_flush(x->c);
}

static void x11_show_cursor(struct nb_x11 *x, bool show)
{
    (void)show;
    x11_cursor_policy(x);
}

/*
 * Blit a CPU-rendered ARGB buffer to a drawable, in bands.
 *
 * PutImage carries the pixels inside the request, and a request has a maximum
 * length -- a 4K frame is ~33 MB and would simply be refused.  Splitting into
 * bands that fit is the whole trick; there is no shm here on purpose, because
 * the placeholder and the dialog are drawn once per event, not per frame.
 */
static void x11_blit(struct nb_x11 *x, xcb_drawable_t d, xcb_gcontext_t gc,
                     const uint32_t *px, int w, int h)
{
    uint32_t maxreq = xcb_get_maximum_request_length(x->c);   /* in 4-byte units */
    int rows = (int)((maxreq > 4096 ? maxreq - 4096 : 1024) / (uint32_t)(w > 0 ? w : 1));
    int y;

    if (rows < 1) { rows = 1; }
    for (y = 0; y < h; y += rows) {
        int n = h - y < rows ? h - y : rows;

        xcb_put_image(x->c, XCB_IMAGE_FORMAT_Z_PIXMAP, d, gc,
                      (uint16_t)w, (uint16_t)n, 0, (int16_t)y, 0, 24,
                      (uint32_t)(w * n * 4), (const uint8_t *)(px + (size_t)y * w));
    }
}

/* ── close dialog ────────────────────────────────────────────────────────── */
/*
 * Same four choices and the same geometry as the Wayland overlay, drawn as a
 * child window because on X11 the window manager owns the frame and its close
 * button: WM_DELETE_WINDOW is a REQUEST, and answering it by killing the guest
 * outright is not a decision the broker gets to make for the user.
 */
#define NB_DLG_W     560
#define NB_DLG_BTN_H  40
#define NB_DLG_GAP     8
#define NB_DLG_PAD    18
#define NB_DLG_N       4
#define NB_DLG_TITLE  26
#define NB_DLG_H  (NB_DLG_PAD * 2 + NB_DLG_TITLE + NB_DLG_GAP + \
                   NB_DLG_N * (NB_DLG_BTN_H + NB_DLG_GAP))

static const char *const x11_dlg_label[NB_DLG_N] = {
    "SHUT DOWN THE GUEST (ACPI)",
    "FORCE OFF THE VM",
    "CLOSE THE DISPLAY ONLY",
    "CANCEL",
};

static void x11_dlg_hide(struct nb_x11 *x)
{
    if (!x->dlg_mapped) {
        return;
    }
    xcb_unmap_window(x->c, x->dlg);
    x->dlg_mapped = false;
    x->dlg_hot = -1;
    /* Whatever the pointer policy was before the dialog, it is again. */
    x11_show_cursor(x, !x->grabbed);
    xcb_flush(x->c);
}

static void x11_dlg_paint(struct nb_x11 *x)
{
    uint32_t *px = calloc((size_t)NB_DLG_W * NB_DLG_H, 4);
    int i;

    if (!px) {
        return;
    }
    nb_placeholder_fill(px, NB_DLG_W, NB_DLG_H, NB_DLG_W, 0xff101418u);
    nb_placeholder_text(px, NB_DLG_W, NB_DLG_H, NB_DLG_W, NB_DLG_PAD,
                        NB_DLG_PAD, "CLOSE THIS WINDOW?", 2, 0xffe6edf3u);
    for (i = 0; i < NB_DLG_N; i++) {
        int top = NB_DLG_PAD + NB_DLG_TITLE + NB_DLG_GAP +
                  i * (NB_DLG_BTN_H + NB_DLG_GAP);
        uint32_t bg = i == x->dlg_hot ? 0xff2b3947u : 0xff1b2027u;

        nb_placeholder_fill(px + (size_t)top * NB_DLG_W + NB_DLG_PAD,
                            NB_DLG_W - 2 * NB_DLG_PAD, NB_DLG_BTN_H,
                            NB_DLG_W, bg);
        nb_placeholder_text(px, NB_DLG_W, NB_DLG_H, NB_DLG_W,
                            NB_DLG_PAD + 12, top + (NB_DLG_BTN_H - 14) / 2,
                            x11_dlg_label[i], 2,
                            i == 1 ? 0xffff6b6bu : 0xffe6edf3u);
    }
    if (!x->idle_gc) {
        x->idle_gc = xcb_generate_id(x->c);
        xcb_create_gc(x->c, x->idle_gc, x->win, 0, NULL);
    }
    x11_blit(x, x->dlg, x->idle_gc, px, NB_DLG_W, NB_DLG_H);
    xcb_flush(x->c);
    free(px);
}

static void x11_dlg_show(struct nb_x11 *x)
{
    uint32_t vals[4];

    if (x->dlg_mapped) {
        return;
    }
    if (!x->dlg) {
        uint32_t m = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t v[2] = { x->screen->black_pixel,
                          XCB_EVENT_MASK_BUTTON_PRESS |
                          XCB_EVENT_MASK_BUTTON_RELEASE |
                          XCB_EVENT_MASK_POINTER_MOTION |
                          XCB_EVENT_MASK_EXPOSURE };

        x->dlg = xcb_generate_id(x->c);
        xcb_create_window(x->c, XCB_COPY_FROM_PARENT, x->dlg, x->win,
                          0, 0, NB_DLG_W, NB_DLG_H, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          x->screen->root_visual, m, v);
    }
    vals[0] = (uint32_t)((x->win_w - NB_DLG_W) / 2 > 0 ? (x->win_w - NB_DLG_W) / 2 : 0);
    vals[1] = (uint32_t)((x->win_h - NB_DLG_H) / 2 > 0 ? (x->win_h - NB_DLG_H) / 2 : 0);
    vals[2] = NB_DLG_W;
    vals[3] = NB_DLG_H;
    xcb_configure_window(x->c, x->dlg,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         vals);
    xcb_map_window(x->c, x->dlg);
    /* Raise above the content, which is the guest's frame. */
    { uint32_t st = XCB_STACK_MODE_ABOVE;
      xcb_configure_window(x->c, x->dlg, XCB_CONFIG_WINDOW_STACK_MODE, &st); }
    x->dlg_mapped = true;
    x->dlg_hot = -1;
    /* The user is being asked a question: give them a pointer to answer it. */
    x11_show_cursor(x, true);
    x11_dlg_paint(x);
}

static int x11_dlg_hit(int px, int py)
{
    int i;

    if (px < NB_DLG_PAD || px >= NB_DLG_W - NB_DLG_PAD) {
        return -1;
    }
    for (i = 0; i < NB_DLG_N; i++) {
        int top = NB_DLG_PAD + NB_DLG_TITLE + NB_DLG_GAP +
                  i * (NB_DLG_BTN_H + NB_DLG_GAP);

        if (py >= top && py < top + NB_DLG_BTN_H) {
            return i;
        }
    }
    return -1;
}

/* The "no VM attached" screen.  Same painter the Wayland backend uses. */
static void x11_dismiss_dialog(struct nb_session *s)
{
    x11_dlg_hide(s->priv);
}

static int x11_show_idle(struct nb_session *s)
{
    struct nb_x11 *x = s->priv;
    int w = x->win_w > 0 ? x->win_w : 1280;
    int h = x->win_h > 0 ? x->win_h : 800;
    uint32_t *px;

    if (!x->idle_gc) {
        x->idle_gc = xcb_generate_id(x->c);
        xcb_create_gc(x->c, x->idle_gc, x->win, 0, NULL);
    }
    px = calloc((size_t)w * h, 4);
    if (!px) {
        return -ENOMEM;
    }
    /*
     * SHRINK FIRST, PAINT SECOND.  x11_size_content() ends in a clear_area --
     * added so a narrowing guest cannot leave stale pixels beside itself -- so
     * running it after the blit wiped the placeholder to black the instant it
     * was drawn.  Two changes each correct alone.
     */
    x11_size_content(x, 1, 1);
    /*
     * "ATTACHED BUT SILENT" IS ITS OWN STATE, and the one a user actually
     * meets: a guest still booting, wedged, or with a broken present path looks
     * exactly like a broken broker if the window is simply black.  Say which it
     * is.
     */
    if (x->client_attached) {
        nb_placeholder_paint(px, (unsigned)w, (unsigned)h, (unsigned)w,
                             "VM ATTACHED - NO PICTURE YET",
                             "THE GUEST HAS NOT PRESENTED A FRAME");
    } else {
        nb_placeholder_paint(px, (unsigned)w, (unsigned)h, (unsigned)w,
                             "NO VM ATTACHED",
                             "WAITING FOR THE VMM TO CONNECT");
    }
    /* Onto the TOPLEVEL: the content child is the guest's and may still be
     * holding the last frame at the guest's size, not the window's. */
    x11_blit(x, x->win, x->idle_gc, px, w, h);
    xcb_flush(x->c);
    free(px);
    x->idle_shown = true;
    /* Nothing of the guest is on screen, so the host pointer belongs to the
     * user again. */
    x11_show_cursor(x, true);
    return 0;
}

static int x11_set_grab(struct nb_session *s, bool on)
{
    struct nb_x11 *x = s->priv;

    if (x->grabbed == on) {
        return 0;
    }
    /* Both are how the user knows the grab is on -- and the hidden pointer is
     * also the point: under grab the guest draws its own. */
    x11_show_cursor(x, !on);
    x11_set_status(x, on ? "GRABBED - CTRL+ALT+G TO RELEASE" : NULL);
    if (on) {
        xcb_grab_keyboard_reply_t *kr;
        xcb_grab_pointer_reply_t *pr;

        kr = xcb_grab_keyboard_reply(
                 x->c, xcb_grab_keyboard(x->c, 1, x->win, XCB_CURRENT_TIME,
                                         XCB_GRAB_MODE_ASYNC,
                                         XCB_GRAB_MODE_ASYNC), NULL);
        if (!kr || kr->status != XCB_GRAB_STATUS_SUCCESS) {
            nb_err("GrabKeyboard refused (status %d) — another client holds "
                   "the keyboard", kr ? kr->status : -1);
            free(kr);
            return -EBUSY;
        }
        free(kr);
        pr = xcb_grab_pointer_reply(
                 x->c, xcb_grab_pointer(x->c, 1, x->win,
                                        XCB_EVENT_MASK_BUTTON_PRESS |
                                        XCB_EVENT_MASK_BUTTON_RELEASE |
                                        XCB_EVENT_MASK_POINTER_MOTION,
                                        XCB_GRAB_MODE_ASYNC,
                                        XCB_GRAB_MODE_ASYNC,
                                        x->win /* confine_to */, XCB_NONE,
                                        XCB_CURRENT_TIME), NULL);
        if (!pr || pr->status != XCB_GRAB_STATUS_SUCCESS) {
            nb_err("GrabPointer refused (status %d)", pr ? pr->status : -1);
            free(pr);
            xcb_ungrab_keyboard(x->c, XCB_CURRENT_TIME);
            xcb_flush(x->c);
            return -EBUSY;
        }
        free(pr);
    } else {
        xcb_ungrab_pointer(x->c, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(x->c, XCB_CURRENT_TIME);
    }
    xcb_flush(x->c);
    x->grabbed = on;
    return 0;
}

static int x11_set_fullscreen(struct nb_session *s, bool on)
{
    struct nb_x11 *x = s->priv;
    xcb_client_message_event_t ev;

    if (x->a_wm_state == XCB_ATOM_NONE || x->a_fs == XCB_ATOM_NONE) {
        return -ENOTSUP;
    }
    /*
     * Fullscreen is where the frame can actually reach a hardware plane: an
     * X compositor UNREDIRECTS a fullscreen override window and the frame goes
     * straight to the CRTC.  Like Wayland's direct scanout it can only be
     * qualified for, never requested — this asks for the precondition.
     */
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = x->win;
    ev.type = x->a_wm_state;
    ev.data.data32[0] = on ? 1 : 0;     /* _NET_WM_STATE_ADD / _REMOVE */
    ev.data.data32[1] = x->a_fs;
    ev.data.data32[3] = 1;              /* source: normal application */
    xcb_send_event(x->c, 0, x->screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char *)&ev);
    xcb_flush(x->c);
    x->fullscreen = on;
    return 0;
}

static void x11_close(struct nb_session *s)
{
    struct nb_x11 *x = s->priv;
    int i;

    if (!x) {
        return;
    }
    free(x->src_text);
    x->src_text = NULL;
    free(x->clip_pending);
    x->clip_pending = NULL;
    if (x->c && !xcb_connection_has_error(x->c)) {
        x11_set_grab(s, false);
        for (i = 0; i < NB_MAX_BUFS; i++) {
            x11_buf_free(x, i);
        }
        xcb_flush(x->c);
    }
    if (x->c) {
        xcb_disconnect(x->c);
    }
    free(x);
    s->priv = NULL;
    free(s);
}

static int x11_open(struct nb_session *s, const struct nb_config *cfg)
{
    struct nb_x11 *x = s->priv;
    const xcb_query_extension_reply_t *ext;
    xcb_dri3_query_version_reply_t *dv;
    xcb_present_query_version_reply_t *pv;
    uint32_t mask, vals[2];
    int screen_n = 0;
    bool have_mods = false;

    x->sess = s;
    x->pending = -1;
    x->current = -1;
    x->last_mode = -1;
    x->win_w = (int)cfg->win_w;
    x->win_h = (int)cfg->win_h;

    x->c = xcb_connect(NULL, &screen_n);
    if (!x->c || xcb_connection_has_error(x->c)) {
        nb_err("xcb_connect failed (DISPLAY=%s)",
               getenv("DISPLAY") ? getenv("DISPLAY") : "");
        return -ENOENT;
    }
    {
        xcb_screen_iterator_t it =
            xcb_setup_roots_iterator(xcb_get_setup(x->c));
        int k;

        for (k = 0; k < screen_n && it.rem; k++) {
            xcb_screen_next(&it);
        }
        x->screen = it.data;
    }
    if (!x->screen) {
        nb_err("no X screen");
        goto fail;
    }
    /*
     * The content window must be depth 24, because PresentPixmap requires the
     * pixmap and the window to have the same depth and both XRGB8888 and
     * ARGB8888 are imported as depth 24 (a scanout's alpha is not composited).
     * The ROOT visual is not necessarily depth 24 -- on a server whose root is
     * 32-bit it is not -- so find a depth-24 TrueColor visual explicitly rather
     * than assuming, which would be a BadMatch at CreateWindow.
     */
    {
        xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(x->screen);

        for (; di.rem && !x->visual24; xcb_depth_next(&di)) {
            xcb_visualtype_iterator_t vi;

            if (di.data->depth != 24) {
                continue;
            }
            vi = xcb_depth_visuals_iterator(di.data);
            for (; vi.rem; xcb_visualtype_next(&vi)) {
                if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR &&
                    vi.data->red_mask == 0xff0000 &&
                    vi.data->green_mask == 0x00ff00 &&
                    vi.data->blue_mask == 0x0000ff) {
                    x->visual24 = vi.data->visual_id;
                    break;
                }
            }
        }
    }
    if (!x->visual24) {
        nb_err("this X screen has no depth-24 TrueColor visual with an "
               "XRGB8888 channel layout, so the guest's scanout cannot be "
               "presented without a conversion this program will not guess at");
        goto fail;
    }

    /*
     * THE SHM TIER NEEDS NEITHER DRI3 NOR PRESENT.  It hands the guest's
     * pixels over as plain shared memory and puts them on screen with core
     * PutImage, importing no dma-buf and flipping no pixmap -- so demanding
     * these extensions here refused to start on exactly the servers the tier
     * exists to serve: an X server with no DRI3 at all (Xvfb, VNC/remote X,
     * anything old or software-only).
     *
     * MEASURED: `--backend x11 --present-mode=shm` against Xvfb died with "the
     * X server has no DRI3 extension" before opening a window.  That is the
     * fallback rung failing for the one reason the rung exists.
     *
     * This mirrors the same gate on the Wayland side, where requiring dma-buf
     * formats in shm mode was the identical bug.
     */
    if (nb_tier == NB_TIER_SHM) {
        nb_log("shm tier: not requiring DRI3 or Present (frames arrive as "
               "shared memory and are put on screen with core PutImage)");
        goto skip_dmabuf_extensions;
    }

    /* DRI3 >= 1.0 is required at all; >= 1.2 buys explicit modifiers. */
    ext = xcb_get_extension_data(x->c, &xcb_dri3_id);
    if (!ext || !ext->present) {
        nb_err("the X server has no DRI3 extension, so it cannot take a "
               "dma-buf at all.  There is no fallback here on purpose: a copy "
               "path would need a GL context in the privileged process, which "
               "is what this design removes.");
        goto fail;
    }
    dv = xcb_dri3_query_version_reply(x->c, xcb_dri3_query_version(x->c, 1, 2),
                                      NULL);
    if (!dv) {
        nb_err("DRI3QueryVersion failed");
        goto fail;
    }
    /*
     * The reported version is LOGGED, not believed.  See x11_collect_formats()
     * — the NVIDIA DDX reports 1.0 and serves the 1.2 requests regardless, so
     * whether explicit modifiers are available is decided by asking, further
     * down, once the content window exists.
     */
    nb_log("DRI3 %u.%u reported%s", dv->major_version, dv->minor_version,
           (dv->major_version == 1 && dv->minor_version < 2)
               ? " (below 1.2 — asking for modifiers anyway, some drivers "
                 "under-report)" : "");
    free(dv);

    ext = xcb_get_extension_data(x->c, &xcb_present_id);
    if (!ext || !ext->present) {
        nb_err("the X server has no Present extension");
        goto fail;
    }
    x->present_opcode = ext->major_opcode;
    pv = xcb_present_query_version_reply(
             x->c, xcb_present_query_version(x->c, 1, 2), NULL);
    if (!pv) {
        nb_err("PresentQueryVersion failed");
        goto fail;
    }
    nb_log("Present %u.%u", pv->major_version, pv->minor_version);
    free(pv);

skip_dmabuf_extensions:

    /* The toplevel: input, focus, grab, fullscreen. */
    x->win = xcb_generate_id(x->c);
    /*
     * A BACKGROUND, so the letterbox is a colour and not whatever was last on
     * screen.  X does not clear a parent when a child shrinks, and the content
     * child is exactly that -- letterboxed inside this window at the guest's
     * mode size.  Without a background, narrowing the guest left the newly
     * exposed columns holding stale pixels: REPORTED as "left/right resize
     * leaves glitches, top/bottom resize is clean and repairs them", which is
     * precisely the asymmetry, because only a width change exposes new area
     * that nothing else happens to repaint.
     */
    mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    vals[0] = x->screen->black_pixel;
    vals[1] = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
              XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
              XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
              XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE |
              XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE;
    xcb_create_window(x->c, XCB_COPY_FROM_PARENT, x->win, x->screen->root,
                      0, 0, (uint16_t)x->win_w, (uint16_t)x->win_h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, x->screen->root_visual,
                      mask, vals);
    /*
     * _NET_WM_NAME in UTF8_STRING, not WM_NAME in STRING.  WM_NAME's STRING
     * type is Latin-1 by definition, so the default title's em dash -- three
     * UTF-8 bytes -- was rendered as its first byte and the rest swallowed:
     * the window came up called "SteamOS a".  Every window manager since EWMH
     * prefers _NET_WM_NAME anyway.  WM_NAME is still set, for anything old
     * enough not to look at _NET_WM_NAME, and gets the same bytes: mildly
     * wrong for a pre-EWMH WM, versus visibly wrong for everyone.
     */
    /*
     * Intern HERE, not with the rest of the atoms: those are interned further
     * down, after this point, so using them here passed atom 0 and the server
     * answered BadAtom -- "X error 5 (major 18)" in the log, and the property
     * silently never existed.  A round trip once at startup is cheaper than a
     * property that is not there.
     */
    x->a_net_wm_name = x11_atom(x->c, "_NET_WM_NAME");
    x->a_utf8        = x11_atom(x->c, "UTF8_STRING");
    snprintf(x->title, sizeof x->title, "%s", cfg->title);
    x->scale_mode = cfg->scale_mode;
#ifdef NB_HAVE_XCB_RENDER
    {
        /* One depth-24 format is all the scaler needs; without it we simply
         * keep the 1:1 Present path and say so. */
        xcb_render_query_pict_formats_reply_t *pf =
            xcb_render_query_pict_formats_reply(
                x->c, xcb_render_query_pict_formats(x->c), NULL);

        if (pf) {
            xcb_render_pictforminfo_iterator_t it =
                xcb_render_query_pict_formats_formats_iterator(pf);

            for (; it.rem; xcb_render_pictforminfo_next(&it)) {
                if (it.data->depth == 24 &&
                    it.data->type == XCB_RENDER_PICT_TYPE_DIRECT) {
                    x->pict_fmt = it.data->id;
                    break;
                }
            }
            free(pf);
        }
        nb_log("scaling: %s", x->pict_fmt
               ? "XRender (the window fills even when the guest's mode differs)"
               : "NONE - no usable XRender format; frames stay 1:1 and letterboxed");
    }
#else
    nb_log("scaling: NONE - built without xcb-render; frames stay 1:1");
#endif
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, x->a_net_wm_name,
                        x->a_utf8, 8, (uint32_t)strlen(cfg->title),
                        cfg->title);
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, (uint32_t)strlen(cfg->title),
                        cfg->title);
    xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win,
                        XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof("nvkvm\0nvkvm-display-broker") - 1,
                        "nvkvm\0nvkvm-display-broker");

    /*
     * The content child.  Depth 24 always: both XRGB8888 and ARGB8888 are
     * imported as depth 24 / 32 bpp, because a scanout's alpha is not
     * composited and PresentPixmap requires pixmap and window depths to match.
     * A window manager never touches a child window, so its size is ours and
     * Present's same-size rule is satisfiable.
     */
    x->content = xcb_generate_id(x->c);
    xcb_create_window(x->c, 24, x->content, x->win, 0, 0,
                      (uint16_t)x->win_w, (uint16_t)x->win_h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, x->visual24,
                      0, NULL);
    x->con_w = x->win_w;
    x->con_h = x->win_h;
    /* Build the empty cursor now; the grab shows it and hides the host's. */
    x11_cursor_init(x);
    xcb_map_window(x->c, x->content);
    xcb_map_window(x->c, x->win);

    /*
     * Present event stream: pacing (complete) and buffer release (idle).
     * NOT IN THE SHM TIER: there is no Present extension to select on, and
     * merely ISSUING a request for an absent extension makes xcb tear the
     * whole connection down (XCB_CONN_CLOSED_EXT_NOTSUPPORTED = 2) -- it does
     * not fail the one call.  Skipping the version *check* while still making
     * the call is therefore not a fallback, it is a delayed crash.
     */
    if (nb_tier != NB_TIER_SHM) {
        x->present_eid = xcb_generate_id(x->c);
        xcb_present_select_input(x->c, x->present_eid, x->content,
                                 XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                                 XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    }

    x->a_wm_state  = x11_atom(x->c, "_NET_WM_STATE");
    x->a_fs        = x11_atom(x->c, "_NET_WM_STATE_FULLSCREEN");
    x->a_wm_proto  = x11_atom(x->c, "WM_PROTOCOLS");
    x->a_wm_delete = x11_atom(x->c, "WM_DELETE_WINDOW");
    x->a_clipboard = x11_atom(x->c, "CLIPBOARD");
    x->a_utf8      = x11_atom(x->c, "UTF8_STRING");
    x->a_targets   = x11_atom(x->c, "TARGETS");
    x->a_prop      = x11_atom(x->c, "NVKVM_CLIP");
    x->a_incr      = x11_atom(x->c, "INCR");
    x->a_net_wm_name = x11_atom(x->c, "_NET_WM_NAME");
    if (x->a_wm_proto != XCB_ATOM_NONE && x->a_wm_delete != XCB_ATOM_NONE) {
        xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, x->a_wm_proto,
                            XCB_ATOM_ATOM, 32, 1, &x->a_wm_delete);
    }
    xcb_flush(x->c);

    /*
     * Same reason: x11_collect_formats() asks DRI3 for modifiers, and on a
     * server with no DRI3 that request alone closes the connection.  The shm
     * tier advertises nothing and imports nothing, so there is nothing to ask.
     */
    if (nb_tier == NB_TIER_SHM) {
        nb_log("shm tier: advertising no dma-buf format (frames are shared "
               "memory), so no modifier query is made");
        have_mods = false;
        goto formats_done;
    }
    have_mods = x11_collect_formats(x) > 0;
    nb_formats_log(&x->formats, "the X server");
    if (!have_mods) {
        nb_log("this X server offers NO explicit modifiers: only implicitly-"
               "modified (DRM_FORMAT_MOD_INVALID) buffers will be accepted, "
               "and a guest that flips a block-linear bo will be rejected");
    }
formats_done:

    s->width = (uint32_t)x->win_w;
    s->height = (uint32_t)x->win_h;
    /* Both directions, same as Wayland: the boundary the broker draws must not
     * depend on which display server the host happens to run. */
    s->clipboard_caps = NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G;
    s->caps = NVKVM_BROKER_CAP_KEYBOARD | NVKVM_BROKER_CAP_ABS_POINTER |
              NVKVM_BROKER_CAP_POINTER_LOCK | NVKVM_BROKER_CAP_TOTAL_GRAB |
              NVKVM_BROKER_CAP_FOCUS_EVENTS | NVKVM_BROKER_CAP_FULLSCREEN |
              NVKVM_BROKER_CAP_DMABUF | NVKVM_BROKER_CAP_RELEASE;
    if (have_mods) {
        s->caps |= NVKVM_BROKER_CAP_MODIFIERS;
    }

#ifdef NB_HAVE_XCB_XINPUT
    ext = xcb_get_extension_data(x->c, &xcb_input_id);
    if (ext && ext->present) {
        xcb_input_xi_query_version_reply_t *iv =
            xcb_input_xi_query_version_reply(
                x->c, xcb_input_xi_query_version(x->c, 2, 2), NULL);

        if (iv) {
            struct {
                xcb_input_event_mask_t head;
                uint32_t               mask;
            } em = { { XCB_INPUT_DEVICE_ALL_MASTER, 1 },
                     XCB_INPUT_XI_EVENT_MASK_RAW_MOTION };

            x->xi_opcode = ext->major_opcode;
            x->have_xi = true;
            xcb_input_xi_select_events(x->c, x->screen->root, 1, &em.head);
            xcb_flush(x->c);
            s->caps |= NVKVM_BROKER_CAP_REL_POINTER;
            free(iv);
        }
    }
    if (!x->have_xi) {
        snprintf(s->grab_caveat, sizeof(s->grab_caveat),
                 "XInput2 is absent, so there is NO relative motion under "
                 "grab: the pointer is confined but a fast turn stops at the "
                 "window edge");
    }
#else
    snprintf(s->grab_caveat, sizeof(s->grab_caveat),
             "this build has no xcb-xinput, so there is NO relative motion "
             "under grab: the pointer is confined but a fast turn stops at "
             "the window edge (install libxcb-xinput-dev and rebuild)");
#endif
    if (!s->grab_caveat[0]) {
        snprintf(s->grab_caveat, sizeof(s->grab_caveat),
                 "X11 grabs are total for everything the X server routes, "
                 "window-manager bindings included; input the kernel handles "
                 "below X (SysRq, and VT switching on some setups) is not "
                 "interceptable by any X client");
    }
    /*
     * THE SHM TIER'S BUFFER IS A MEMFD, NOT A DMA-BUF, so the shared ATTACH
     * validator has to be told this backend can take one -- otherwise every
     * shm frame is refused with "the fd is not a dma-buf" and the tier that
     * exists to work anywhere works nowhere.  The Wayland and test backends
     * already set this; X11 never did, which is why shm-on-X presented a
     * rejection for every frame it was handed.
     *
     * Only in the shm tier: on the dma-buf tiers a memfd really is a bug, and
     * accepting one there would silently drop the import path's guarantees.
     */
    if (nb_tier == NB_TIER_SHM) {
        s->accept_memfd = true;
    }
    return 0;

fail:
    if (x->c) {
        xcb_disconnect(x->c);
        x->c = NULL;
    }
    return -ENOTSUP;
}

/* ── clipboard ───────────────────────────────────────────────────────────── */
/*
 * The X11 half of the same policy the Wayland backend implements, using the
 * two primitives X has had since forever: we OWN the CLIPBOARD selection to
 * hand the guest's text out, and we CONVERT it to read the host's text in.
 *
 * Both are asynchronous and both are driven from the existing event loop, so
 * unlike Wayland there is no extra fd to poll: the reply arrives as an event
 * on the connection we already watch.
 */

/* Guest -> host.  0 = it IS the clipboard now, 1 = held until focus, <0 error. */
static int x11_set_clipboard(struct nb_session *s, const char *text, size_t len)
{
    struct nb_x11 *x = s->priv;
    char *copy;

    copy = malloc(len + 1);
    if (!copy) {
        return -ENOMEM;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';

    /*
     * NOT FOCUSED: hold it.  Same rule as Wayland, for the same reason -- a
     * background VM must not replace what you copied somewhere else -- and
     * dropping it would lose the copy entirely, because the guest believes it
     * succeeded and never re-sends.
     */
    if (!x->focused) {
        free(x->clip_pending);
        x->clip_pending = copy;
        x->clip_pending_len = len;
        return 1;
    }

    free(x->src_text);
    x->src_text = copy;
    x->src_len = len;

    /*
     * A REAL timestamp, not XCB_CURRENT_TIME.  ICCCM says an owner must use
     * the time of the event that prompted it, and some clients police it.
     */
    xcb_set_selection_owner(x->c, x->win, x->a_clipboard, x->last_time);
    xcb_flush(x->c);
    return 0;
}

static void x11_clip_flush_pending(struct nb_x11 *x)
{
    char *text;
    size_t len;

    if (!x->clip_pending || !x->focused) {
        return;
    }
    text = x->clip_pending; len = x->clip_pending_len;
    x->clip_pending = NULL; x->clip_pending_len = 0;
    if (x->sess && x11_set_clipboard(x->sess, text, len) == 0) {
        nb_log("the VM's clipboard text is now on yours (it was held while "
               "the window was not focused)");
    }
    free(text);
}

/* Someone asked us for the selection we own. */
static void x11_clip_serve(struct nb_x11 *x,
                           const xcb_selection_request_event_t *rq)
{
    xcb_selection_notify_event_t ev;
    xcb_atom_t prop = rq->property != XCB_ATOM_NONE ? rq->property : rq->target;
    bool ok = false;

    if (x->src_text && rq->target == x->a_targets) {
        const xcb_atom_t targets[] = { x->a_targets, x->a_utf8, XCB_ATOM_STRING };

        xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, rq->requestor, prop,
                            XCB_ATOM_ATOM, 32,
                            (uint32_t)(sizeof targets / sizeof targets[0]),
                            targets);
        ok = true;
    } else if (x->src_text &&
               (rq->target == x->a_utf8 || rq->target == XCB_ATOM_STRING)) {
        xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, rq->requestor, prop,
                            rq->target, 8, (uint32_t)x->src_len, x->src_text);
        ok = true;
    }

    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.time      = rq->time;
    ev.requestor = rq->requestor;
    ev.selection = rq->selection;
    ev.target    = rq->target;
    ev.property  = ok ? prop : XCB_ATOM_NONE;   /* NONE = refused, per ICCCM */
    xcb_send_event(x->c, 0, rq->requestor, XCB_EVENT_MASK_NO_EVENT,
                   (const char *)&ev);
    xcb_flush(x->c);
}

/* Host -> guest: ask for the selection.  The answer arrives as an event. */
static int x11_fetch_clipboard(struct nb_session *s, struct nb_sink *sink,
                               uint64_t generation)
{
    struct nb_x11 *x = s->priv;

    (void)sink;
    if (x->fetch_active) {
        return -EBUSY;                  /* one at a time, as on Wayland */
    }
    /*
     * Nobody owns it -- there is nothing to paste.  Reported as ENOENT so the
     * caller releases the held keystroke rather than waiting for a reply that
     * will never come.
     */
    xcb_get_selection_owner_reply_t *own =
        xcb_get_selection_owner_reply(x->c,
            xcb_get_selection_owner(x->c, x->a_clipboard), NULL);
    bool have_owner = own && own->owner != XCB_WINDOW_NONE;
    free(own);
    if (!have_owner) {
        return -ENOENT;
    }

    xcb_delete_property(x->c, x->win, x->a_prop);
    xcb_convert_selection(x->c, x->win, x->a_clipboard, x->a_utf8,
                          x->a_prop, x->last_time);
    xcb_flush(x->c);
    x->fetch_active = true;
    x->fetch_generation = generation;
    return 0;
}

/* The selection we asked for has landed (or been refused). */
static void x11_clip_receive(struct nb_x11 *x, struct nb_sink *sink,
                             const xcb_selection_notify_event_t *sn)
{
    xcb_get_property_reply_t *pr;
    bool sent = false;

    if (!x->fetch_active) {
        return;
    }
    x->fetch_active = false;

    if (sn->property == XCB_ATOM_NONE) {
        nb_log("clipboard: the selection owner refused to give us text");
        nb_sink_clip_finish(sink, x->fetch_generation, false);
        x->fetch_generation = 0;
        return;
    }
    pr = xcb_get_property_reply(x->c,
            xcb_get_property(x->c, 1 /* delete */, x->win, x->a_prop,
                             XCB_GET_PROPERTY_TYPE_ANY, 0,
                             (NVKVM_BROKER_CLIP_MAX_BYTES + 3) / 4), NULL);
    if (pr) {
        int len = xcb_get_property_value_length(pr);
        const char *val = xcb_get_property_value(pr);

        if (pr->type == x->a_incr) {
            /*
             * INCR means the owner wants to stream it in chunks, which it only
             * does for something far larger than our 7 KiB cap.  Refuse rather
             * than implement a protocol we would immediately truncate.
             */
            nb_log("clipboard: the host selection is too large to paste "
                   "(offered incrementally; the cap is %u bytes)",
                   NVKVM_BROKER_CLIP_MAX_BYTES);
        } else if (len > 0 && (unsigned)len <= NVKVM_BROKER_CLIP_MAX_BYTES) {
            sent = nb_sink_send_clipboard(sink, x->fetch_generation, val,
                                          (size_t)len);
        } else if (len > 0) {
            nb_log("clipboard: host selection is larger than the %u-byte cap; "
                   "not pasting it", NVKVM_BROKER_CLIP_MAX_BYTES);
        }
        free(pr);
    }
    nb_sink_clip_finish(sink, x->fetch_generation, sent);
    x->fetch_generation = 0;
}

/*
 * Tell a freshly-attached client where focus and the pointer actually are,
 * by ASKING the server rather than replaying an edge we may never have seen.
 */
static void x11_resync(struct nb_session *s)
{
    struct nb_x11 *x = s->priv;
    xcb_get_input_focus_reply_t *f;
    xcb_query_pointer_reply_t *q;

    /* resync runs BEFORE the first dispatch, which is where x->sink is
     * normally set, so take it from the session or this returns having done
     * nothing at all -- which is exactly what it used to do. */
    if (!x->sink) {
        x->sink = s->sink;
    }
    if (!x->sink || !x->c || xcb_connection_has_error(x->c)) {
        return;
    }
    f = xcb_get_input_focus_reply(x->c, xcb_get_input_focus(x->c), NULL);
    if (f) {
        /*
         * WALK UP, do not compare ids.  Under a reparenting window manager --
         * which is nearly all of them, openbox included -- the focus window is
         * our window, but it may equally be a frame the WM wrapped around it,
         * and a bare `focus == win` test then answers "not focused" forever.
         */
        xcb_window_t w = f->focus;
        int hops = 0;

        x->focused = false;
        while (w != XCB_WINDOW_NONE && hops++ < 8) {
            if (w == x->win || w == x->content) {
                x->focused = true;
                break;
            }
            xcb_query_tree_reply_t *t =
                xcb_query_tree_reply(x->c, xcb_query_tree(x->c, w), NULL);
            if (!t) {
                break;
            }
            w = (t->parent == t->root) ? XCB_WINDOW_NONE : t->parent;
            free(t);
        }
        free(f);
        nb_sink_focus(x->sink, x->focused);
    }
    q = xcb_query_pointer_reply(x->c, xcb_query_pointer(x->c, x->content),
                                NULL);
    if (q && x->con_w > 0 && x->con_h > 0) {
        bool inside = q->same_screen &&
                      q->win_x >= 0 && q->win_y >= 0 &&
                      q->win_x < x->con_w && q->win_y < x->con_h;

        x->ptr_inside = inside;
        nb_sink_pointer(x->sink, inside);
        if (inside) {
            nb_sink_abs(x->sink, q->win_x, q->win_y,
                        (unsigned)x->con_w, (unsigned)x->con_h);
        }
    }
    free(q);
    x11_clip_flush_pending(x);
    /*
     * A client just attached.  Until it presents something there is nothing of
     * the guest on screen, so keep the placeholder up and change what it says,
     * rather than leaving a black rectangle nobody can interpret.
     */
    x->client_attached = true;
    if (x->current < 0) {
        x11_show_idle(s);
    }
}

static uint64_t x11_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* The guest changed the host clipboard: say so, briefly. */
static void x11_notify_clipboard(struct nb_session *s)
{
    struct nb_x11 *x = s->priv;

    x->notice_until_ms = x11_now_ms() + 4000u;
    x11_set_status(x, "THE VM CHANGED YOUR CLIPBOARD");
}

/* Clock-driven work: expire that notice.  Without this the main loop sleeps in
 * poll() forever and a "transient" notice is permanent. */
static int x11_tick(struct nb_session *s)
{
    struct nb_x11 *x = s->priv;
    uint64_t now;

    if (!x->notice_until_ms) {
        return -1;
    }
    now = x11_now_ms();
    if (now >= x->notice_until_ms) {
        x->notice_until_ms = 0;
        x11_set_status(x, x->grabbed ? "GRABBED - CTRL+ALT+G TO RELEASE" : NULL);
        return -1;
    }
    return (int)(x->notice_until_ms - now);
}

static void x11_client_detach_clip(struct nb_session *s, uint64_t generation)
{
    struct nb_x11 *x = s->priv;

    x->client_attached = false;
    if (x->fetch_active && x->fetch_generation == generation) {
        x->fetch_active = false;
        x->fetch_generation = 0;
    }
}

static const struct nb_session_ops x11_ops = {
    .name = "x11",
    .open = x11_open,
    .close = x11_close,
    .pollfds = x11_pollfds,
    .dispatch = x11_dispatch,
    .set_grab = x11_set_grab,
    .set_fullscreen = x11_set_fullscreen,
    .format_ok = x11_format_ok,
    .attach = x11_attach,
    .commit = x11_commit,
    .resize = x11_resize,
    .show_idle = x11_show_idle,
    .dismiss_dialog = x11_dismiss_dialog,
    .resync = x11_resync,
    .tick = x11_tick,
    .notify_clipboard = x11_notify_clipboard,
    .set_clipboard = x11_set_clipboard,
    .fetch_clipboard = x11_fetch_clipboard,
    .client_detach = x11_client_detach_clip,
};

struct nb_session *nb_session_x11(const struct nb_config *cfg)
{
    struct nb_session *s = calloc(1, sizeof(*s));
    struct nb_x11 *x = calloc(1, sizeof(*x));

    if (!s || !x) {
        free(s);
        free(x);
        return NULL;
    }
    s->ops = &x11_ops;
    s->priv = x;

    if (x11_ops.open(s, cfg) != 0) {
        free(x);
        free(s);
        return NULL;
    }
    return s;
}

#endif /* NB_HAVE_X11 */
