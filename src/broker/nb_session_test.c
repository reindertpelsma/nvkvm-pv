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
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvkvm_broker.h"

#define NB_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NB_DRM_FORMAT_MOD_INVALID  0x00ffffffffffffffULL

struct nb_test {
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
};

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
    int a = 0, b = 0;
    char c = 0;

    if (sscanf(line, " %c %d %d", &c, &a, &b) < 1) {
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
    default: break;
    }
}

static int test_dispatch(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_test *t = s->priv;
    char buf[512];
    ssize_t n;

    if (t->resize_w) {
        nb_sink_surface(sink, t->resize_w, t->resize_h);
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
    nb_sink_surface(sink, t->pending_w, t->pending_h);
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
     * accepted, everything else — including ARGB8888, which a real backend
     * would take — is rejected.
     */
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'), 0);
    nb_formats_add(&t->formats, NB_FOURCC('X', 'R', '2', '4'),
                   NB_DRM_FORMAT_MOD_INVALID);

    s->accept_memfd = true;
    s->clipboard_caps = NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G;
    s->caps = NVKVM_BROKER_CAP_KEYBOARD | NVKVM_BROKER_CAP_ABS_POINTER |
              NVKVM_BROKER_CAP_REL_POINTER | NVKVM_BROKER_CAP_POINTER_LOCK |
              NVKVM_BROKER_CAP_TOTAL_GRAB | NVKVM_BROKER_CAP_FOCUS_EVENTS |
              NVKVM_BROKER_CAP_FULLSCREEN | NVKVM_BROKER_CAP_DMABUF |
              NVKVM_BROKER_CAP_MODIFIERS | NVKVM_BROKER_CAP_RELEASE;
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
