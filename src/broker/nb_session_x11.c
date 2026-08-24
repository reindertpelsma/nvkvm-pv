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
    xcb_visualid_t    visual24;       /* the content window's visual          */

    struct nb_formats formats;
    struct nb_x11_buf bufs[NB_MAX_BUFS];
    uint64_t tick;
    int      pending, current;
    uint32_t serial;

    struct nb_sink *sink;
    struct nb_session *sess;

    int  win_w, win_h;              /* toplevel size, from ConfigureNotify    */
    int  con_w, con_h;              /* content-window size we last asked for  */
    bool grabbed, fullscreen;
};

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
 */
static void x11_collect_formats(struct nb_x11 *x)
{
    xcb_dri3_get_supported_modifiers_reply_t *r;
    uint64_t *m;
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
        }
        m = xcb_dri3_get_supported_modifiers_screen_modifiers(r);
        n = xcb_dri3_get_supported_modifiers_screen_modifiers_length(r);
        for (i = 0; i < n; i++) {
            nb_formats_add(&x->formats, NB_FCC_XR24, m[i]);
            nb_formats_add(&x->formats, NB_FCC_AR24, m[i]);
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
}

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

    /* PresentPixmap needs pixmap and window to agree on size, and only we can
     * change the child, so this always converges in one step. */
    if (x->con_w != (int)sl->w || x->con_h != (int)sl->h) {
        x11_size_content(x, (int)sl->w, (int)sl->h);
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
    nb_sink_surface(sink, sl->w, sl->h);
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
    case XCB_PRESENT_COMPLETE_NOTIFY:
        /* Pacing.  This is the X11 equivalent of a wl frame callback: the
         * frame is on screen, another may be drawn. */
        nb_sink_frame(sink);
        break;
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
            nb_sink_focus(sink, true);
            break;
        case XCB_FOCUS_OUT:
            /* The property that stops the grab being a keylogger. */
            nb_sink_focus(sink, false);
            break;
        case XCB_ENTER_NOTIFY:
            nb_sink_pointer(sink, true);
            break;
        case XCB_LEAVE_NOTIFY:
            nb_sink_pointer(sink, false);
            break;
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE: {
            xcb_key_press_event_t *k = (void *)ev;

            nb_sink_key(sink, x11_kc_to_evdev(k->detail),
                        (ev->response_type & 0x7f) == XCB_KEY_PRESS);
            break;
        }
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            xcb_button_press_event_t *b = (void *)ev;
            bool down = (ev->response_type & 0x7f) == XCB_BUTTON_PRESS;
            unsigned d = b->detail;

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

            nb_sink_abs(sink, m->event_x, m->event_y,
                        (unsigned)x->win_w, (unsigned)x->win_h);
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *c = (void *)ev;

            if (c->window == x->win) {
                x->win_w = c->width;
                x->win_h = c->height;
                nb_sink_surface(sink, c->width, c->height);
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
    return 0;
}

static int x11_set_grab(struct nb_session *s, bool on)
{
    struct nb_x11 *x = s->priv;

    if (x->grabbed == on) {
        return 0;
    }
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
    have_mods = (dv->major_version > 1 ||
                 (dv->major_version == 1 && dv->minor_version >= 2));
    nb_log("DRI3 %u.%u%s", dv->major_version, dv->minor_version,
           have_mods ? "" : " (< 1.2: no explicit modifiers)");
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

    /* The toplevel: input, focus, grab, fullscreen. */
    x->win = xcb_generate_id(x->c);
    mask = XCB_CW_EVENT_MASK;
    vals[0] = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
              XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
              XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
              XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE |
              XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_create_window(x->c, XCB_COPY_FROM_PARENT, x->win, x->screen->root,
                      0, 0, (uint16_t)x->win_w, (uint16_t)x->win_h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, x->screen->root_visual,
                      mask, vals);
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
    xcb_map_window(x->c, x->content);
    xcb_map_window(x->c, x->win);

    /* Present event stream: pacing (complete) and buffer release (idle). */
    x->present_eid = xcb_generate_id(x->c);
    xcb_present_select_input(x->c, x->present_eid, x->content,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                             XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);

    x->a_wm_state  = x11_atom(x->c, "_NET_WM_STATE");
    x->a_fs        = x11_atom(x->c, "_NET_WM_STATE_FULLSCREEN");
    x->a_wm_proto  = x11_atom(x->c, "WM_PROTOCOLS");
    x->a_wm_delete = x11_atom(x->c, "WM_DELETE_WINDOW");
    if (x->a_wm_proto != XCB_ATOM_NONE && x->a_wm_delete != XCB_ATOM_NONE) {
        xcb_change_property(x->c, XCB_PROP_MODE_REPLACE, x->win, x->a_wm_proto,
                            XCB_ATOM_ATOM, 32, 1, &x->a_wm_delete);
    }
    xcb_flush(x->c);

    if (have_mods) {
        x11_collect_formats(x);
    } else {
        nb_formats_add(&x->formats, NB_FCC_XR24, NB_DRM_FORMAT_MOD_INVALID);
        nb_formats_add(&x->formats, NB_FCC_AR24, NB_DRM_FORMAT_MOD_INVALID);
    }
    nb_formats_log(&x->formats, "the X server");

    s->width = (uint32_t)x->win_w;
    s->height = (uint32_t)x->win_h;
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
    return 0;

fail:
    if (x->c) {
        xcb_disconnect(x->c);
        x->c = NULL;
    }
    return -ENOTSUP;
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
