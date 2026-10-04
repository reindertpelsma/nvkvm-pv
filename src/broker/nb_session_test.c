/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_session_test.c — a session backend with no display behind it.
 *
 * WHY THIS EXISTS.  Everything interesting about this program that is not the
 * import itself — SO_PEERCRED validation, the command framing, the whole
 * ATTACH validator (geometry against the real fd, the fourcc/modifier gate,
 * the dma-buf check, the fd-intake bound), one-client-at-a-time, hotkey
 * interception, focus gating, stuck-key release, and the motion-coalescing
 * queue — is testable without a GPU, a monitor or a compositor.  Without this
 * backend none of it could be tested at all before reaching hardware, and it
 * is the half most likely to contain a bug.
 *
 * It IS NOT A DISPLAY.  Nothing is shown.  attach() records the descriptor and
 * prints it; commit() answers with the pacing and release events a real
 * backend would produce, so a client's full loop can be exercised.
 *
 * It is also the ONLY backend that accepts a memfd in place of a dma-buf, so
 * the ACCEPT side of the validator can be exercised on a machine with no GPU.
 * That relaxation is why it is unreachable from --backend auto and why it
 * prints a banner saying so.
 *
 * Input comes from stdin, one event per line, so the whole state machine is
 * scriptable from a shell:
 *
 *     k <linux-keycode> <0|1>     key up/down
 *     b <linux-btncode> <0|1>     button up/down
 *     a <x> <y>                   absolute motion
 *     r <dx> <dy>                 relative motion
 *     w <v> <h>                   wheel
 *     f <0|1>                     focus out/in
 *     p <0|1>                     pointer leave/enter
 *     c                           complete the current clipboard fetch
 *     s                           attempt a cancelled client's stale fetch
 *     d <major> <minor> <flags>   the display moved to another DRM device
 *                                 (flags = NVKVM_BROKER_DEVICE_F_*); a real
 *                                 backend learns this from the compositor
 *
 * The guest cursor is "shown" by logging exactly what a real backend would
 * put up -- size, hot spot, generation and a hash of the pixels, and the
 * monotonic time it was put up -- or that it would show none, which is what a
 * test needs to tell "applied" from "refused" from "coalesced away".  Like the
 * real backends it also RE-RENDERS on its own schedule -- every frame COMMIT,
 * which is when nb_session_wl.c re-runs gc_refresh() and nb_session_x11.c its
 * cursor policy -- from the pointer it was handed, logging only when what it
 * would show has changed.  That is what lets test/test_cursor.py measure the
 * pacing interval across commits, the path that once bypassed it.
 *
 * And like the X11 backend it can be REFUSED an import it advertised: the pair
 * (XR24 or AR24, NB_TEST_MOD_REFUSED) is advertised and every ATTACH of it is
 * refused the way DRI3 refuses one, through the same core call
 * (nb_sink_format_refused(), both alpha twins) -- so the wire behaviour of a
 * taken-back yes is testable without a GPU.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvkvm_broker.h"

#define NB_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NB_DRM_FORMAT_MOD_INVALID  0x00ffffffffffffffULL
/*
 * Advertised, and refused at import: see the header comment.  A REAL layout,
 * DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 0) -- the one-GOB
 * (8-row) block height of the modifier family the NVIDIA DDX advertises --
 * because since nb_extent.c the core refuses, before any backend sees it, a
 * modifier whose layout it cannot bound.  It was vendor NONE 0xc0ffee, which
 * now never reaches a backend at all.  One-GOB blocks keep test/test_cursor.py's
 * 64-row frames at 64 * stride bytes, and its stride of 256 is whole GOBs.
 */
#define NB_TEST_MOD_REFUSED        0x0300000000606010ULL
/*
 * Advertised, and never usable: block-linear with bit 5 set, which
 * drm_fourcc.h reserves (bits 8:5, a 3D block depth).  A display can
 * advertise a modifier from a newer header than the broker was built with;
 * nb_modifier_layout() refuses the field by name, CMD_QUERY_FORMAT answers it
 * x=0, and a frame in it is told x=0 -- test/test_cursor.py checks all three.
 */
#define NB_TEST_MOD_UNBOUNDED      0x0300000000606034ULL

struct nb_test {
    /* Settable so the harness can exercise the refresh hint without a
     * compositor; 0 means 'unknown', which is what a backend with no
     * presentation feedback reports. */
    unsigned refresh_mhz;
    bool     grabbed;
    bool     fullscreen;
    char     line[256];
    size_t   len;
    struct nb_formats formats;
    /* the "pending" buffer, as a real backend would hold it */
    bool     have_pending;
    uint64_t pending_id;
    uint32_t pending_w, pending_h;
    unsigned resize_w, resize_h;    /* announced on the next dispatch */
    bool     fetch_pending;
    uint64_t fetch_generation;
    uint64_t stale_fetch_generation;
    /* The core's cursor state, as a real backend keeps the pointer to it. */
    const struct nb_cursor *cursor;
    /* What was last shown, so a re-render that changes nothing logs nothing
     * -- the comparison the real backends make before re-uploading. */
    bool     shown_valid;
    bool     shown_wanted;
    uint64_t shown_gen;
};

static uint64_t test_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/*
 * Log what a real backend would now show over the guest's picture.  The
 * decision is the SHARED one (nb_cursor_wanted) with this backend's own grab
 * state, which is exactly the call the Wayland and X11 backends make -- so the
 * selftest exercises the policy they run, not a copy of it.
 */
static void test_cursor_show(struct nb_test *t)
{
    const struct nb_cursor *c = t->cursor;
    bool want;

    if (!c) {
        return;
    }
    want = nb_cursor_wanted(c, t->grabbed);
    t->shown_valid = true;
    t->shown_wanted = want;
    t->shown_gen = c->gen;
    if (want) {
        nb_log("TEST cursor: shown %ux%u hot %u,%u gen %llu hash 0x%08x "
               "t_us %llu", c->w, c->h, c->hot_x, c->hot_y,
               (unsigned long long)c->gen, nb_cursor_hash(c),
               (unsigned long long)test_now_us());
    } else {
        nb_log("TEST cursor: none (%s)",
               !c->defined ? "no image" :
               !c->visible ? "hidden" : "grabbed");
    }
}

/*
 * A re-render on the backend's own schedule (here: every COMMIT).  Re-reads the
 * held pointer, exactly as gc_refresh() and x11_cursor_policy() do, and shows
 * something only if it differs from what is up -- so if the core ever hands a
 * backend a cursor the pacing has not released, this is where it shows.
 */
static void test_cursor_refresh(struct nb_test *t)
{
    const struct nb_cursor *c = t->cursor;

    if (!c) {
        return;
    }
    if (t->shown_valid && t->shown_wanted == nb_cursor_wanted(c, t->grabbed) &&
        (!t->shown_wanted || t->shown_gen == c->gen)) {
        return;
    }
    test_cursor_show(t);
}

static void test_cursor(struct nb_session *s, const struct nb_cursor *cur)
{
    struct nb_test *t = s->priv;

    t->cursor = cur;
    test_cursor_show(t);
}

static int test_pollfds(struct nb_session *s, struct pollfd *out, int max)
{
    (void)s;
    if (max < 1) {
        return 0;
    }
    out[0].fd = STDIN_FILENO;
    out[0].events = POLLIN;
    out[0].revents = 0;
    return 1;
}

static void test_finish_fetch(struct nb_test *t, struct nb_sink *sink,
                              unsigned requested_len)
{
    static char text[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    static bool initialized;
    size_t len = requested_len ? requested_len : 19u;
    bool sent;

    if (!t->fetch_pending) {
        return;
    }
    if (!initialized) {
        memset(text, 'x', sizeof(text));
        initialized = true;
    }
    if (len > sizeof(text)) {
        len = sizeof(text);
    }
    t->fetch_pending = false;
    sent = nb_sink_send_clipboard(sink, t->fetch_generation, text, len);
    nb_sink_clip_finish(sink, t->fetch_generation, sent);
    t->fetch_generation = 0;
}

static void test_finish_stale_fetch(struct nb_test *t, struct nb_sink *sink)
{
    static const char text[] = "stale-host-clipboard";
    uint64_t generation = t->stale_fetch_generation;
    bool sent;

    if (!generation) {
        return;
    }
    t->stale_fetch_generation = 0;
    sent = nb_sink_send_clipboard(sink, generation, text, sizeof(text) - 1u);
    nb_sink_clip_finish(sink, generation, sent);
}

static void test_line(struct nb_session *s, struct nb_sink *sink,
                      const char *line)
{
    int a = 0, b = 0, f = 0;
    char c = 0;

    if (sscanf(line, " %c %d %d %d", &c, &a, &b, &f) < 1) {
        return;
    }
    switch (c) {
    case 'k': nb_sink_key(sink, (unsigned)a, b != 0); break;
    case 'b': nb_sink_btn(sink, (unsigned)a, b != 0); break;
    case 'a': nb_sink_abs(sink, a, b, s->width, s->height); break;
    case 'r': nb_sink_rel(sink, a, b); break;
    case 'w': nb_sink_wheel(sink, a, b); break;
    case 'f': nb_sink_focus(sink, a != 0); break;
    case 'p': nb_sink_pointer(sink, a != 0); break;
    case 'c': test_finish_fetch(s->priv, sink, (unsigned)a); break;
    case 's': test_finish_stale_fetch(s->priv, sink); break;
    case 'm': /* host surface hint: width height refresh_mhz (0 = unknown) */
        if (a > 0 && b > 0 && f >= 0) {
            struct nb_test *t = s->priv;
            t->refresh_mhz = (unsigned)f;
            nb_sink_surface(sink, (unsigned)a, (unsigned)b, t->refresh_mhz);
        }
        break;
    case 'd':
        s->dev_flags = (uint32_t)f;
        s->dev_major = (uint32_t)a;
        s->dev_minor = (uint32_t)b;
        nb_sink_device_changed(sink);
        break;
    default: break;
    }
}

static int test_dispatch(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_test *t = s->priv;
    char buf[512];
    ssize_t n;

    if (t->resize_w) {
        nb_sink_surface(sink, t->resize_w, t->resize_h, t->refresh_mhz);
        t->resize_w = t->resize_h = 0;
    }
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        ssize_t i;

        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                t->line[t->len] = '\0';
                test_line(s, sink, t->line);
                t->len = 0;
            } else if (t->len + 1 < sizeof(t->line)) {
                t->line[t->len++] = buf[i];
            } else {
                t->len = 0;     /* overlong line: drop it, do not grow */
            }
        }
    }
    if (n == 0) {
        return -EPIPE;          /* stdin closed: end the test run */
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return -errno;
    }
    return 0;
}

static bool test_format_ok(struct nb_session *s, uint32_t fourcc, uint64_t mod)
{
    struct nb_test *t = s->priv;

    return nb_formats_has(&t->formats, fourcc, mod);
}

static int test_attach(struct nb_session *s, const struct nb_buf_desc *d)
{
    struct nb_test *t = s->priv;
    char fcc[8];

    if (!d->is_shm && d->modifier == NB_TEST_MOD_REFUSED) {
        /* What x11_attach() does when DRI3 answers with an X error. */
        nb_log("TEST attach: REFUSED %s mod=0x%016llx, as DRI3 refuses an "
               "import it advertised", nb_fourcc_name(d->fourcc, fcc),
               (unsigned long long)d->modifier);
        if (s->sink) {
            nb_sink_format_refused(s->sink, d->fourcc, d->modifier, true);
        }
        return -EINVAL;
    }

    t->have_pending = true;
    t->pending_id = d->id;
    t->pending_w = d->width;
    t->pending_h = d->height;
    nb_log("TEST attach: id=%llu %ux%u stride=%u offset=%u %s mod=0x%016llx "
           "size=%llu", (unsigned long long)d->id, d->width, d->height,
           d->stride, d->offset, nb_fourcc_name(d->fourcc, fcc),
           (unsigned long long)d->modifier, (unsigned long long)d->size);
    return 0;
}

static int test_commit(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_test *t = s->priv;

    if (!t->have_pending) {
        return -ENOENT;
    }
    nb_log("TEST commit: id=%llu %ux%u", (unsigned long long)t->pending_id,
           t->pending_w, t->pending_h);
    test_cursor_refresh(t);
    nb_sink_surface(sink, t->pending_w, t->pending_h, t->refresh_mhz);
    /* A real backend answers with these; produce them so a client's pacing
     * and recycling paths are exercised end to end. */
    nb_sink_release(sink, t->pending_id);
    nb_sink_frame(sink);
    t->have_pending = false;
    return 0;
}

static int test_resize(struct nb_session *s, unsigned w, unsigned h)
{
    struct nb_test *t = s->priv;

    t->resize_w = w;
    t->resize_h = h;
    return 0;
}

static int test_set_grab(struct nb_session *s, bool on)
{
    struct nb_test *t = s->priv;

    t->grabbed = on;
    /* The grab hides the guest's cursor and its end restores it; say which,
     * so the selftest can see the transition the real backends make. */
    test_cursor_show(t);
    return 0;
}

static int test_set_fullscreen(struct nb_session *s, bool on)
{
    struct nb_test *t = s->priv;

    t->fullscreen = on;
    return 0;
}

static int test_set_clipboard(struct nb_session *s, const char *text, size_t len)
{
    nb_log("TEST clipboard from guest: %zu bytes", len);
    return 0;
}

static int test_fetch_clipboard(struct nb_session *s, struct nb_sink *sink,
                                uint64_t generation)
{
    struct nb_test *t = s->priv;

    if (t->fetch_pending) {
        return -EBUSY;
    }
    t->fetch_pending = true;
    t->fetch_generation = generation;
    return 0;
}

static void test_client_detach(struct nb_session *s, uint64_t generation)
{
    struct nb_test *t = s->priv;

    if (t->fetch_pending && t->fetch_generation == generation) {
        /* Keep only the generation so `s` can model a completion callback
         * that was already queued when cancellation closed the real fd. */
        t->stale_fetch_generation = generation;
        t->fetch_pending = false;
        t->fetch_generation = 0;
    }
}

static void test_close(struct nb_session *s)
{
    free(s->priv);
    free(s);
}

static int test_open(struct nb_session *s, const struct nb_config *cfg)
{
    struct nb_test *t = s->priv;

    s->width = cfg->win_w;
    s->height = cfg->win_h;
    /*
     * A deliberately SMALL advertised set, so the selftest can prove both
     * halves of the format gate: XRGB8888 linear and XRGB8888 implicit are
     * accepted, everything else — including ARGB8888 in either layout, which
     * a real backend would take — is rejected.  The only other pairs are the
     * refused-at-import ones below and the one the core cannot bound, which
     * no frame ever gets through.
     */
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'), 0);
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'),
                   NB_DRM_FORMAT_MOD_INVALID);
    /* Advertised for both alpha twins, as DRI3 advertises a modifier, and
     * refused at import: see the header comment. */
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'),
                   NB_TEST_MOD_REFUSED);
    nb_formats_add(&t->formats, NB_FOURCC('A', 'R', '2', '4'),
                   NB_TEST_MOD_REFUSED);
    /* Advertised, and a layout the core cannot bound: see its definition. */
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'),
                   NB_TEST_MOD_UNBOUNDED);

    s->accept_memfd = true;
    s->accept_shm   = true;
    s->clipboard_caps = NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G;
    s->caps = NVKVM_BROKER_CAP_KEYBOARD | NVKVM_BROKER_CAP_ABS_POINTER |
              NVKVM_BROKER_CAP_REL_POINTER | NVKVM_BROKER_CAP_POINTER_LOCK |
              NVKVM_BROKER_CAP_TOTAL_GRAB | NVKVM_BROKER_CAP_FOCUS_EVENTS |
              NVKVM_BROKER_CAP_FULLSCREEN | NVKVM_BROKER_CAP_DMABUF |
              NVKVM_BROKER_CAP_MODIFIERS | NVKVM_BROKER_CAP_RELEASE |
              NVKVM_BROKER_CAP_CURSOR;
    /* No display, so no device: "unknown" until a `d` line says otherwise,
     * which is a valid EV_DEVICE answer in its own right. */
    snprintf(s->grab_caveat, sizeof(s->grab_caveat),
             "THIS IS THE TEST BACKEND: nothing is displayed and no real input "
             "is captured");
    nb_log("TEST BACKEND: no display is being driven, no input is being "
           "grabbed, and a memfd is accepted where a real backend demands a "
           "dma-buf. Events come from stdin.");
    nb_formats_log(&t->formats, "the test backend");
    return 0;
}

static const struct nb_session_ops test_ops = {
    .name = "test",
    .open = test_open,
    .close = test_close,
    .pollfds = test_pollfds,
    .dispatch = test_dispatch,
    .set_grab = test_set_grab,
    .set_fullscreen = test_set_fullscreen,
    .format_ok = test_format_ok,
    .attach = test_attach,
    .commit = test_commit,
    .resize = test_resize,
    .client_detach = test_client_detach,
    .set_clipboard = test_set_clipboard,
    .fetch_clipboard = test_fetch_clipboard,
    .cursor = test_cursor,
};

struct nb_session *nb_session_test(const struct nb_config *cfg)
{
    struct nb_session *s = calloc(1, sizeof(*s));
    struct nb_test *t = calloc(1, sizeof(*t));

    if (!s || !t) {
        free(s);
        free(t);
        return NULL;
    }
    s->ops = &test_ops;
    s->priv = t;
    if (test_ops.open(s, cfg) != 0) {
        free(t);
        free(s);
        return NULL;
    }
    /* stdin must not block the loop. */
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0) {
        nb_err("O_NONBLOCK on stdin: %s", strerror(errno));
    }
    return s;
}
