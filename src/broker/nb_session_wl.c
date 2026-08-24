/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_session_wl.c — the Wayland backend.
 *
 * DISPLAY.  An ordinary xdg_toplevel window.  The guest's scanout dma-buf
 * arrives from the VMM as an fd; we wrap it in a wl_buffer through
 * zwp_linux_dmabuf_v1 and attach it to our wl_surface.  Nothing is imported
 * into a GL context, nothing is blitted, and this file contains no GL call:
 * the compositor takes the guest's own allocation as the surface's content.
 *
 * That is one blit fewer than QEMU's GTK/SDL path, which imports the same
 * dma-buf as a texture and glBlitFramebuffer's it into a window framebuffer it
 * owns.  It is also the only arrangement under which the frame can reach a
 * hardware plane at all: a compositor promotes a surface to DIRECT SCANOUT
 * when it is fullscreen, opaque, unoccluded and its format/modifier suits the
 * plane.  That cannot be requested — only qualified for — which is why
 * CTRL+ALT+F is worth having.
 *
 * FORMATS.  The set of acceptable (fourcc, modifier) pairs is whatever the
 * compositor advertised on zwp_linux_dmabuf_v1, and nothing else.  We bind at
 * version 3 deliberately: from version 4 the compositor is required NOT to
 * send `format`/`modifier` events (feedback objects replace them), and a
 * broker that binds 4 would end up with an empty advertised set and reject
 * every frame.
 *
 * INPUT.  Three optional protocols, each independently absent:
 *   zwp_keyboard_shortcuts_inhibit_manager_v1  keyboard, including the
 *                                              compositor's own bindings
 *   zwp_pointer_constraints_v1 (locked pointer) pointer lock under grab
 *   zwp_relative_pointer_manager_v1             relative deltas under grab
 * Missing ones are reported at startup, reflected in the capability bits and
 * spelled out in grab_caveat — a grab that quietly leaks Super to the
 * compositor is exactly the failure a user finds mid-game.
 *
 * FOCUS.  wl_keyboard.leave is the focus-out signal.  It is what makes the
 * grab safe to offer at all: without it the grab would be unescapable, and an
 * unescapable grab is a keylogger.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvkvm_broker.h"

#ifndef NB_HAVE_WAYLAND

struct nb_session *nb_session_wayland(const struct nb_config *cfg)
{
    (void)cfg;
    nb_err("the Wayland backend was not compiled in (wayland-client or "
           "wayland-protocols missing at build time)");
    return NULL;
}

#else /* NB_HAVE_WAYLAND */

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* Defined here rather than pulled from libdrm: with the DRM-lease design gone
 * this backend has no other reason to link libdrm, and one constant is not
 * worth a dependency. */
#define NB_DRM_FORMAT_MOD_INVALID  0x00ffffffffffffffULL

struct nb_wl_buf {
    bool      valid;
    uint64_t  id;                   /* dma-buf inode                          */
    struct wl_buffer *buf;
    uint32_t  w, h, stride, offset, fourcc;
    uint64_t  modifier;
    uint64_t  used;                 /* LRU tick                               */
    struct nb_wl *owner;
};

struct nb_wl {
    struct wl_display    *dpy;
    struct wl_registry   *reg;
    struct wl_compositor *comp;
    struct wl_seat       *seat;
    struct wl_keyboard   *kbd;
    struct wl_pointer    *ptr;
    struct wl_surface    *surf;
    struct xdg_wm_base   *wm_base;
    struct xdg_surface   *xdg_surf;
    struct xdg_toplevel  *toplevel;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    uint32_t              dmabuf_ver;

    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;
    struct zwp_keyboard_shortcuts_inhibitor_v1       *inhibitor;
    struct zwp_pointer_constraints_v1                *constraints;
    struct zwp_locked_pointer_v1                     *lock;
    struct zwp_relative_pointer_manager_v1           *relptr_mgr;
    struct zwp_relative_pointer_v1                   *relptr;

    struct nb_formats formats;

    /* The sink is not reachable from Wayland callbacks otherwise.  NULL until
     * the first dispatch(), so every callback must tolerate that. */
    struct nb_sink *sink;
    struct nb_session *sess;
    /* The pollfd main filled for us, so dispatch can tell whether the
     * compositor fd is actually readable.  Valid only between a
     * wl_pollfds() and the dispatch() that follows it. */
    struct pollfd *pfd;
    bool   flush_blocked;       /* the compositor socket would not take it all */

    struct nb_wl_buf bufs[NB_MAX_BUFS];
    uint64_t tick;
    int      pending;           /* index of the ATTACHed-but-not-COMMITted buf */
    int      current;           /* index of the buffer the surface holds       */
    bool     frame_inflight;

    int    surf_w, surf_h;
    bool   grabbed;
    bool   fullscreen;
    bool   configured;
};

/* ── buffers ─────────────────────────────────────────────────────────────── */

static void buf_release(void *data, struct wl_buffer *b)
{
    struct nb_wl_buf *slot = data;
    (void)b;

    /*
     * The compositor has finished reading this buffer.  Telling the client is
     * what lets it recycle without guessing; it is advisory (nvkvm's guest
     * cycles its own bos regardless), which is why a missed release only costs
     * the client an optimisation.
     */
    if (slot->owner->sink) {
        nb_sink_release(slot->owner->sink, slot->id);
    }
}
static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void wl_buf_destroy(struct nb_wl *w, int i)
{
    if (!w->bufs[i].valid) {
        return;
    }
    if (w->bufs[i].buf) {
        wl_buffer_destroy(w->bufs[i].buf);
    }
    memset(&w->bufs[i], 0, sizeof(w->bufs[i]));
}

/* ── ops: format policy ──────────────────────────────────────────────────── */

static bool wl_format_ok(struct nb_session *s, uint32_t fourcc, uint64_t mod)
{
    struct nb_wl *w = s->priv;

    /*
     * HARDENING 3, verbatim: what the GPU advertises, not a list we wrote.
     * When the compositor could only tell us formats (dmabuf version 2, no
     * `modifier` event) the only thing it has committed to is the implicit
     * layout, so an explicit modifier is not something it agreed to accept.
     */
    if (!(s->caps & NVKVM_BROKER_CAP_MODIFIERS)) {
        return mod == NB_DRM_FORMAT_MOD_INVALID &&
               nb_formats_has(&w->formats, fourcc, NB_DRM_FORMAT_MOD_INVALID);
    }
    return nb_formats_has(&w->formats, fourcc, mod);
}

/* ── ops: attach / commit ────────────────────────────────────────────────── */

static int wl_attach(struct nb_session *s, const struct nb_buf_desc *d)
{
    struct nb_wl *w = s->priv;
    struct zwp_linux_buffer_params_v1 *params;
    struct wl_buffer *b;
    int i, victim = -1;
    uint64_t oldest = UINT64_MAX;

    w->tick++;

    /* Already imported?  Same reasoning as the import cache in
     * nvkvm_present_egl.c: the guest cycles a handful of scanout bos, and the
     * dma-buf inode is their stable identity across dup(2) and SCM_RIGHTS. */
    for (i = 0; i < NB_MAX_BUFS; i++) {
        struct nb_wl_buf *sl = &w->bufs[i];

        if (sl->valid && sl->id == d->id && sl->w == d->width &&
            sl->h == d->height && sl->stride == d->stride &&
            sl->offset == d->offset && sl->fourcc == d->fourcc &&
            sl->modifier == d->modifier) {
            sl->used = w->tick;
            w->pending = i;
            return 0;
        }
    }

    /* Pick a victim.  Never the buffer the surface is holding and never the
     * one already staged, or the compositor is left reading freed content. */
    for (i = 0; i < NB_MAX_BUFS; i++) {
        if (i == w->current || i == w->pending) {
            continue;
        }
        if (!w->bufs[i].valid) {
            victim = i;
            break;
        }
        if (w->bufs[i].used < oldest) {
            oldest = w->bufs[i].used;
            victim = i;
        }
    }
    if (victim < 0) {
        return -ENOSPC;         /* cannot happen with NB_MAX_BUFS >= 3 */
    }

    params = zwp_linux_dmabuf_v1_create_params(w->dmabuf);
    if (!params) {
        return -EIO;
    }
    zwp_linux_buffer_params_v1_add(params, d->fd, 0, d->offset, d->stride,
                                   (uint32_t)(d->modifier >> 32),
                                   (uint32_t)(d->modifier & 0xffffffffu));
    /*
     * create_immed, not create: `create` answers asynchronously, and waiting
     * for the answer means a roundtrip to the compositor inside the path that
     * also carries input.  Input must never block on rendering — this project
     * shipped that bug once already.  create_immed reports failure as a
     * protocol error on the params object instead, which we surface as a lost
     * display rather than as a stall.
     */
    b = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)d->width,
                                                (int32_t)d->height, d->fourcc,
                                                0 /* flags */);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!b) {
        return -EIO;
    }

    wl_buf_destroy(w, victim);
    w->bufs[victim] = (struct nb_wl_buf){
        .valid = true, .id = d->id, .buf = b,
        .w = d->width, .h = d->height, .stride = d->stride,
        .offset = d->offset, .fourcc = d->fourcc, .modifier = d->modifier,
        .used = w->tick, .owner = w,
    };
    wl_buffer_add_listener(b, &buf_listener, &w->bufs[victim]);
    w->pending = victim;
    return 0;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    struct nb_wl *w = data;
    (void)t;

    wl_callback_destroy(cb);
    w->frame_inflight = false;
    if (w->sink) {
        nb_sink_frame(w->sink);
    }
}

static int wl_commit(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_wl *w = s->priv;
    struct nb_wl_buf *sl;
    struct wl_callback *cb;

    if (w->pending < 0) {
        return -ENOENT;         /* COMMIT with nothing attached; not fatal */
    }
    sl = &w->bufs[w->pending];
    if (!sl->valid) {
        w->pending = -1;
        return -ENOENT;
    }

    wl_surface_attach(w->surf, sl->buf, 0, 0);
    wl_surface_damage_buffer(w->surf, 0, 0, (int32_t)sl->w, (int32_t)sl->h);
    /*
     * An opaque region is not cosmetic here: a compositor will not promote a
     * surface to a hardware plane while it might have to blend it, so
     * declaring opacity is one of the preconditions for direct scanout.  The
     * guest head only ever flips X8/A8 32-bit formats and composites its own
     * cursor into them, so the whole surface is opaque by construction.
     *
     * Only on a size change: the region is sticky surface state, so
     * creating and destroying a protocol object every frame is traffic
     * for nothing.
     */
    if ((int)sl->w != w->surf_w || (int)sl->h != w->surf_h) {
        struct wl_region *opaque = wl_compositor_create_region(w->comp);

        if (opaque) {
            wl_region_add(opaque, 0, 0, (int32_t)sl->w, (int32_t)sl->h);
            wl_surface_set_opaque_region(w->surf, opaque);
            wl_region_destroy(opaque);
        }
    }
    if (!w->frame_inflight) {
        cb = wl_surface_frame(w->surf);
        if (cb) {
            wl_callback_add_listener(cb, &frame_listener, w);
            w->frame_inflight = true;
        }
    }
    wl_surface_commit(w->surf);

    w->current = w->pending;
    w->pending = -1;
    if ((int)sl->w != w->surf_w || (int)sl->h != w->surf_h) {
        w->surf_w = (int)sl->w;
        w->surf_h = (int)sl->h;
        nb_sink_surface(sink, sl->w, sl->h);
    }
    return 0;
}

static int wl_resize(struct nb_session *s, unsigned wd, unsigned ht)
{
    struct nb_wl *w = s->priv;

    /*
     * On Wayland a surface's size IS its buffer's size, so there is nothing to
     * request: the next ATTACH at the new geometry resizes the window, and
     * EV_SURFACE then reports what actually happened.  Recorded so the window
     * has a sensible size before the first frame arrives.
     */
    w->surf_w = (int)wd;
    w->surf_h = (int)ht;
    return 0;
}

/* ── seat ────────────────────────────────────────────────────────────────── */

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd,
                       uint32_t sz)
{
    (void)d; (void)k; (void)f; (void)sz;
    /* We forward evdev keycodes, not symbols, so the keymap is the guest's
     * business.  Closing it is required by the protocol. */
    close(fd);
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys)
{
    struct nb_wl *w = d;
    (void)k; (void)serial; (void)s; (void)keys;
    if (w->sink) {
        nb_sink_focus(w->sink, true);
    }
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s)
{
    struct nb_wl *w = d;
    (void)k; (void)serial; (void)s;
    /* The property that stops the grab being a keylogger. */
    if (w->sink) {
        nb_sink_focus(w->sink, false);
    }
}
static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    struct nb_wl *w = d;
    (void)k; (void)serial; (void)time;
    /* wl_keyboard.key carries the evdev code directly — no translation. */
    if (w->sink) {
        nb_sink_key(w->sink, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    }
}
static void kbd_mods(void *d, struct wl_keyboard *k, uint32_t serial,
                     uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)grp;
    /* Ignored on purpose: the sink derives modifier state from the same key
     * stream the client sees, so the two cannot disagree. */
}
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dly)
{
    (void)d; (void)k; (void)r; (void)dly;
}
static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave,
    .key = kbd_key, .modifiers = kbd_mods, .repeat_info = kbd_repeat,
};

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    struct nb_wl *w = d;
    (void)p; (void)serial; (void)s; (void)x; (void)y;
    if (w->sink) {
        nb_sink_pointer(w->sink, true);
    }
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s)
{
    struct nb_wl *w = d;
    (void)p; (void)serial; (void)s;
    if (w->sink) {
        nb_sink_pointer(w->sink, false);
    }
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
                       wl_fixed_t x, wl_fixed_t y)
{
    struct nb_wl *w = d;
    (void)p; (void)t;
    if (w->sink) {
        nb_sink_abs(w->sink, wl_fixed_to_int(x), wl_fixed_to_int(y),
                    (unsigned)w->surf_w, (unsigned)w->surf_h);
    }
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t t, uint32_t button, uint32_t state)
{
    struct nb_wl *w = d;
    (void)p; (void)serial; (void)t;
    /* wl_pointer.button carries the evdev BTN_* code directly. */
    if (w->sink) {
        nb_sink_btn(w->sink, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis,
                     wl_fixed_t value)
{
    struct nb_wl *w = d;
    int v = wl_fixed_to_int(value);
    (void)p; (void)t;

    if (!w->sink) {
        return;
    }
    /* Wayland axis is "down is positive"; evdev REL_WHEEL is "up is positive". */
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        nb_sink_wheel(w->sink, v > 0 ? -1 : (v < 0 ? 1 : 0), 0);
    } else {
        nb_sink_wheel(w->sink, 0, v > 0 ? 1 : (v < 0 ? -1 : 0));
    }
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
                          uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_src, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_disc,
};

static void relptr_motion(void *d, struct zwp_relative_pointer_v1 *r,
                          uint32_t th, uint32_t tl,
                          wl_fixed_t dx, wl_fixed_t dy,
                          wl_fixed_t udx, wl_fixed_t udy)
{
    struct nb_wl *w = d;
    (void)r; (void)th; (void)tl; (void)dx; (void)dy;
    /* Unaccelerated deltas: a guest applies its own acceleration. */
    if (w->sink) {
        nb_sink_rel(w->sink, wl_fixed_to_int(udx), wl_fixed_to_int(udy));
    }
}
static const struct zwp_relative_pointer_v1_listener relptr_listener = {
    .relative_motion = relptr_motion,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    struct nb_wl *w = d;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->kbd) {
        w->kbd = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(w->kbd, &kbd_listener, w);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->ptr) {
        w->ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(w->ptr, &ptr_listener, w);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps, .name = seat_name,
};

/* ── zwp_linux_dmabuf_v1: the advertised format set ──────────────────────── */

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t fmt)
{
    struct nb_wl *w = d;
    (void)z;
    /* Version 1/2 only ever say "this format, implicit layout". */
    nb_formats_add(&w->formats, fmt, NB_DRM_FORMAT_MOD_INVALID);
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z,
                            uint32_t fmt, uint32_t hi, uint32_t lo)
{
    struct nb_wl *w = d;
    (void)z;
    nb_formats_add(&w->formats, fmt, ((uint64_t)hi << 32) | lo);
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format, .modifier = dmabuf_modifier,
};

/* ── xdg-shell ───────────────────────────────────────────────────────────── */

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
    (void)d;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct nb_wl *w = d;

    xdg_surface_ack_configure(s, serial);
    w->configured = true;
    /* Do NOT commit a buffer here: the first real content arrives as an
     * ATTACH+COMMIT from the client.  An empty commit is enough to make the
     * surface exist. */
    if (w->current < 0) {
        wl_surface_commit(w->surf);
    }
}
static const struct xdg_surface_listener xdg_listener = {
    .configure = xdg_configure,
};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t wd,
                          int32_t ht, struct wl_array *states)
{
    struct nb_wl *w = d;
    (void)t; (void)states;

    if (wd > 0 && ht > 0 && (wd != w->surf_w || ht != w->surf_h)) {
        w->surf_w = wd;
        w->surf_h = ht;
        if (w->sink) {
            nb_sink_surface(w->sink, (unsigned)wd, (unsigned)ht);
        }
    }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure, .close = top_close,
};

/* ── registry ────────────────────────────────────────────────────────────── */

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    struct nb_wl *w = data;

#define BIND(I, V) wl_registry_bind(r, name, &I, (ver < (V)) ? ver : (V))
    if (!strcmp(iface, wl_compositor_interface.name)) {
        w->comp = BIND(wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_seat_interface.name) && !w->seat) {
        w->seat = BIND(wl_seat_interface, 5);
        wl_seat_add_listener(w->seat, &seat_listener, w);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        w->wm_base = BIND(xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wm_base, &wm_listener, w);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) &&
               !w->dmabuf) {
        /*
         * Cap at 3 ON PURPOSE.  From version 4 the compositor MUST NOT send
         * the `format`/`modifier` events — the per-surface feedback object
         * replaces them — so binding 4 would leave the advertised set empty
         * and every ATTACH would be rejected as an unadvertised format.
         */
        w->dmabuf_ver = ver < 3 ? ver : 3;
        w->dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface,
                                     w->dmabuf_ver);
        zwp_linux_dmabuf_v1_add_listener(w->dmabuf, &dmabuf_listener, w);
    } else if (!strcmp(iface,
               zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
        w->inhibit_mgr =
            BIND(zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
        w->constraints = BIND(zwp_pointer_constraints_v1_interface, 1);
    } else if (!strcmp(iface,
               zwp_relative_pointer_manager_v1_interface.name)) {
        w->relptr_mgr = BIND(zwp_relative_pointer_manager_v1_interface, 1);
    }
#undef BIND
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_remove,
};

/* ── ops: event loop ─────────────────────────────────────────────────────── */

static int wl_pollfds(struct nb_session *s, struct pollfd *out, int max)
{
    struct nb_wl *w = s->priv;
    int r;

    if (max < 1) {
        return 0;
    }
    /*
     * Flush before the poll, non-blocking.  wl_display_flush() returns -1 with
     * EAGAIN when the compositor socket is full; we then ask poll() for
     * POLLOUT and try again, rather than looping on write.  This is the rule
     * "input must never block on rendering": the same thread carries both, so
     * a blocking flush to a wedged compositor would stall the keyboard.
     */
    r = wl_display_flush(w->dpy);
    w->flush_blocked = (r < 0 && errno == EAGAIN);

    out[0].fd = wl_display_get_fd(w->dpy);
    out[0].events = POLLIN | (w->flush_blocked ? POLLOUT : 0);
    out[0].revents = 0;
    w->pfd = out;
    return 1;
}

/*
 * The main loop calls dispatch() on every wakeup, not only when the compositor
 * fd is readable — the client socket or a signal get us here too.
 * wl_display_read_events() BLOCKS, so it may only be called when poll actually
 * said POLLIN; otherwise the broker would stall on the compositor and stop
 * forwarding input, which is exactly the coupling this design forbids.
 */
static int wl_dispatch_session(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_wl *w = s->priv;
    bool readable = w->pfd && (w->pfd->revents & (POLLIN | POLLHUP | POLLERR));

    w->sink = sink;

    if (w->pfd && (w->pfd->revents & POLLOUT)) {
        wl_display_flush(w->dpy);
    }
    if (wl_display_prepare_read(w->dpy) != 0) {
        /* Another queue already has events buffered; drain those first. */
        if (wl_display_dispatch_pending(w->dpy) < 0) {
            return -EIO;
        }
        return 0;
    }
    if (!readable) {
        wl_display_cancel_read(w->dpy);
        if (wl_display_dispatch_pending(w->dpy) < 0) {
            return -EIO;
        }
        return 0;
    }
    if (wl_display_read_events(w->dpy) < 0) {
        return -EIO;
    }
    if (wl_display_dispatch_pending(w->dpy) < 0) {
        return -EIO;
    }
    return 0;
}

static int wl_set_grab(struct nb_session *s, bool on)
{
    struct nb_wl *w = s->priv;

    if (w->grabbed == on) {
        return 0;
    }
    if (on) {
        if (w->inhibit_mgr && !w->inhibitor) {
            w->inhibitor =
                zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                    w->inhibit_mgr, w->surf, w->seat);
        }
        if (w->constraints && w->ptr && !w->lock) {
            w->lock = zwp_pointer_constraints_v1_lock_pointer(
                w->constraints, w->surf, w->ptr, NULL,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        }
        if (w->relptr_mgr && w->ptr && !w->relptr) {
            w->relptr = zwp_relative_pointer_manager_v1_get_relative_pointer(
                w->relptr_mgr, w->ptr);
            zwp_relative_pointer_v1_add_listener(w->relptr,
                                                 &relptr_listener, w);
        }
    } else {
        if (w->inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(w->inhibitor);
            w->inhibitor = NULL;
        }
        if (w->lock) {
            zwp_locked_pointer_v1_destroy(w->lock);
            w->lock = NULL;
        }
        if (w->relptr) {
            zwp_relative_pointer_v1_destroy(w->relptr);
            w->relptr = NULL;
        }
    }
    w->grabbed = on;
    return 0;
}

static int wl_set_fullscreen(struct nb_session *s, bool on)
{
    struct nb_wl *w = s->priv;

    if (!w->toplevel) {
        return -ENOTSUP;
    }
    /*
     * Fullscreen is where the frame can actually reach a hardware plane: a
     * compositor promotes an opaque, unoccluded, correctly formatted surface
     * covering an output to DIRECT SCANOUT.  It cannot be requested, only
     * qualified for — this is the request for the precondition.
     */
    if (on) {
        xdg_toplevel_set_fullscreen(w->toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(w->toplevel);
    }
    w->fullscreen = on;
    return 0;
}

static void wl_close_session(struct nb_session *s)
{
    struct nb_wl *w = s->priv;
    int i;

    if (!w) {
        return;
    }
    wl_set_grab(s, false);
    for (i = 0; i < NB_MAX_BUFS; i++) {
        wl_buf_destroy(w, i);
    }
    if (w->dpy) {
        wl_display_flush(w->dpy);
        wl_display_disconnect(w->dpy);
    }
    free(w);
    s->priv = NULL;
    free(s);
}

static int wl_open(struct nb_session *s, const struct nb_config *cfg)
{
    struct nb_wl *w = s->priv;
    int missing = 0;

    w->sess = s;
    w->surf_w = (int)cfg->win_w;
    w->surf_h = (int)cfg->win_h;
    w->pending = -1;
    w->current = -1;

    w->dpy = wl_display_connect(NULL);
    if (!w->dpy) {
        nb_err("wl_display_connect failed (WAYLAND_DISPLAY=%s)",
               getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        return -ENOENT;
    }
    w->reg = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(w->reg, &reg_listener, w);
    wl_display_roundtrip(w->dpy);   /* globals */
    wl_display_roundtrip(w->dpy);   /* the dmabuf format/modifier burst */

    if (!w->comp || !w->wm_base) {
        nb_err("the compositor is missing %s%s— there is no window to make",
               w->comp ? "" : "wl_compositor ",
               w->wm_base ? "" : "xdg_wm_base ");
        goto fail;
    }
    if (!w->seat) {
        /*
         * No seat means no input and, critically, no wl_keyboard.leave — so
         * focus loss is unobservable and grab must not be offered.  Not fatal:
         * a display with no input is still a display, and saying so beats
         * refusing to start with a message about a global nobody has heard of.
         */
        nb_err("this compositor advertises no wl_seat: there will be NO "
               "keyboard, NO pointer and NO grab. Display only.");
    }
    if (!w->dmabuf) {
        nb_err("this compositor does not implement zwp_linux_dmabuf_v1, so it "
               "cannot take the guest's buffer without a copy.  There is no "
               "fallback here on purpose: a copy path would need a GL context "
               "in the privileged process, which is what this design removes.");
        goto fail;
    }
    if (w->dmabuf_ver < 2) {
        nb_err("zwp_linux_dmabuf_v1 version %u has no create_immed; version 2 "
               "or later is required (universally available since 2016)",
               w->dmabuf_ver);
        goto fail;
    }
    if (w->formats.n == 0) {
        nb_err("the compositor advertised no dma-buf formats at all — nothing "
               "could be validated against, so every frame would be rejected");
        goto fail;
    }

    /* The window. */
    w->surf = wl_compositor_create_surface(w->comp);
    w->xdg_surf = xdg_wm_base_get_xdg_surface(w->wm_base, w->surf);
    xdg_surface_add_listener(w->xdg_surf, &xdg_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surf);
    xdg_toplevel_add_listener(w->toplevel, &top_listener, w);
    xdg_toplevel_set_title(w->toplevel, cfg->title);
    xdg_toplevel_set_app_id(w->toplevel, "nvkvm-display-broker");
    wl_surface_commit(w->surf);
    wl_display_roundtrip(w->dpy);

    s->width = (uint32_t)w->surf_w;
    s->height = (uint32_t)w->surf_h;
    s->caps = NVKVM_BROKER_CAP_FULLSCREEN | NVKVM_BROKER_CAP_DMABUF |
              NVKVM_BROKER_CAP_RELEASE;
    if (w->seat) {
        s->caps |= NVKVM_BROKER_CAP_KEYBOARD | NVKVM_BROKER_CAP_ABS_POINTER |
                   NVKVM_BROKER_CAP_FOCUS_EVENTS;
    }
    if (w->dmabuf_ver >= 3) {
        s->caps |= NVKVM_BROKER_CAP_MODIFIERS;
    } else {
        nb_log("zwp_linux_dmabuf_v1 is version 2: no explicit modifiers, so "
               "only implicitly-modified buffers will be accepted");
    }
    if (w->relptr_mgr && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_REL_POINTER;
    } else {
        missing++;
    }
    if (w->constraints && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_POINTER_LOCK;
    } else {
        missing++;
    }
    if (w->inhibit_mgr && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_TOTAL_GRAB;
    } else {
        missing++;
    }

    /* Say exactly what is missing, at startup, in one line. */
    if (missing) {
        snprintf(s->grab_caveat, sizeof(s->grab_caveat),
                 "compositor is missing: %s%s%s",
                 w->inhibit_mgr ? ""
                     : "keyboard-shortcuts-inhibit (Super and other compositor "
                       "shortcuts WILL still fire under grab); ",
                 w->constraints ? "" : "pointer-constraints (pointer NOT "
                                       "confined); ",
                 w->relptr_mgr ? "" : "relative-pointer (NO relative motion "
                                      "under grab); ");
    }
    nb_formats_log(&w->formats, "the compositor");
    return 0;

fail:
    if (w->dpy) {
        wl_display_disconnect(w->dpy);
        w->dpy = NULL;
    }
    return -ENOTSUP;
}

static const struct nb_session_ops wl_ops = {
    .name = "wayland",
    .open = wl_open,
    .close = wl_close_session,
    .pollfds = wl_pollfds,
    .dispatch = wl_dispatch_session,
    .set_grab = wl_set_grab,
    .set_fullscreen = wl_set_fullscreen,
    .format_ok = wl_format_ok,
    .attach = wl_attach,
    .commit = wl_commit,
    .resize = wl_resize,
};

struct nb_session *nb_session_wayland(const struct nb_config *cfg)
{
    struct nb_session *s = calloc(1, sizeof(*s));
    struct nb_wl *w = calloc(1, sizeof(*w));

    if (!s || !w) {
        free(s);
        free(w);
        return NULL;
    }
    s->ops = &wl_ops;
    s->priv = w;

    if (wl_ops.open(s, cfg) != 0) {
        free(w);
        free(s);
        return NULL;
    }
    return s;
}

#endif /* NB_HAVE_WAYLAND */
