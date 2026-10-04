/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_broker.c — nvkvm privileged display broker.
 *
 * WHY THIS EXISTS
 *
 * nvkvm's VMM (a patched QEMU) normally needs, on the host: an EGL context, a
 * DRM render node, and a GTK/SDL connection to the user's X11 or Wayland
 * session.  That is a large privilege surface to hand a process which has
 * repeatedly turned out to be reachable from a hostile guest kernel — this
 * tree's own audits found, among others, an arbitrary-address pin primitive
 * reachable from the guest.  An X11 socket inside a sandbox is close to no
 * sandbox at all: X11 has no inter-client isolation.
 *
 * This program moves exactly that privilege out of the VMM.  It owns the
 * window, the compositor connection and the input grab; the VMM keeps only a
 * unix socket.
 *
 * THE DIRECTION OF THE BUFFER IS THE POINT
 *
 * The frame originates in the GUEST and travels UP.  The guest compositor
 * allocates its scanout bo through the forwarded render node, so the real RM
 * object lives in the isolate; the stub PRIME-exports it and hands QEMU a
 * dma-buf fd over SCM_RIGHTS — that already happens today, every frame.  In
 * broker mode QEMU RELAYS THAT SAME FD onward to us instead of importing it,
 * and we wrap it in the display server's own buffer object.
 *
 * The consequence is the reason to do this at all: QEMU imports nothing and
 * touches no pixels, so in broker mode its display path needs no EGL, no GL,
 * no libnvidia-eglcore and no /dev/dri/renderD*.  One physical allocation is
 * seen as the guest's scanout bo, the isolate's RM object, a host dma-buf and
 * a wl_buffer (or a DRI3 pixmap).  Zero-copy holds end to end.
 *
 * WHAT THIS SIDE MUST ASSUME
 *
 * The VMM is hostile.  It sends us a descriptor and an fd; we validate both
 * against the real buffer before the compositor — which runs as the user,
 * OUTSIDE any sandbox — is allowed to read a byte of it.  Every rule is in
 * nb_validate_desc() below and in README.md §3.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include "nvkvm_uidmap.h"
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/vfs.h>

#include "nvkvm_broker.h"

/*
 * The filesystem magic of the anonymous inode every dma-buf lives on.  This is
 * how the privileged side answers "is this really a dma-buf?" — see
 * nb_fd_is_dmabuf().  In linux/magic.h since 4.19; spelled out because a
 * broker built against older headers must still make the check.
 */
#ifndef DMA_BUF_MAGIC
#define DMA_BUF_MAGIC 0x444d4142      /* "DMAB" */
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC   0x01021994
#endif

int nb_verbose;
int nb_trace_frames;

/* ── logging ─────────────────────────────────────────────────────────────── */

static void nb_vlog(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
    fputs(prefix, f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

void nb_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nb_vlog(stderr, "nvkvm-broker: ", fmt, ap);
    va_end(ap);
}

void nb_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nb_vlog(stderr, "nvkvm-broker: ERROR: ", fmt, ap);
    va_end(ap);
}

/* ── the policy core ─────────────────────────────────────────────────────── */

static bool bit_get(const unsigned char *m, unsigned b)
{
    return b < 768 && (m[b >> 3] & (1u << (b & 7))) != 0;
}
static void bit_set(unsigned char *m, unsigned b, bool v)
{
    if (b >= 768) {
        return;
    }
    if (v) {
        m[b >> 3] |= (unsigned char)(1u << (b & 7));
    } else {
        m[b >> 3] &= (unsigned char)~(1u << (b & 7));
    }
}

/* Forward: closing a client's fd is a filesystem call; see nb_fd_drop(). */
static void nb_fd_drop(int fd);

void nb_sink_init(struct nb_sink *s, struct nb_session *sess)
{
    memset(s, 0, sizeof(*s));
    s->sess = sess;
    s->client_fd = -1;
    s->rxfd = -1;
}

/*
 * One definition of "belongs to a client connection".  This deliberately
 * excludes host-window facts (`focused`, `pointer_in`, `fullscreen`) and the
 * backend objects themselves; everything a VMM supplied or caused is reset
 * here.  Before this helper existed each new clipboard flag was easy to miss,
 * and --persist let the next VMM inherit partial transfers and paste state.
 */
static void nb_client_state_reset(struct nb_sink *s)
{
    s->seq = 0;
    s->tx_head = s->tx_tail = 0;
    s->tx_partial = 0;
    s->rxlen = 0;
    if (s->rxfd >= 0) {
        nb_fd_drop(s->rxfd);
        s->rxfd = -1;
    }
    s->window_established = false;
    s->rate_ms = 0;
    s->rate_count = 0;
    s->rej_log_ms = 0;
    s->rej_log_count = 0;
    s->rej_log_hidden = 0;
    memset(s->clip_in, 0, sizeof(s->clip_in));
    s->clip_in_len = 0;
    s->clip_in_chunks = 0;
    s->clip_in_next_chunk = 0;
    s->clip_in_active = false;
    s->clip_in_bad = false;
    s->clip_rate_ms = 0;
    s->clip_rate_count = 0;
    s->clip_last_ms = 0;
    s->clip_told_off = false;
    s->clip_told_noclient = false;
    s->clip_told_noagent = false;
    s->clip_held_key = 0;
    s->clip_held_ctrl = false;
    s->clip_held_shift = false;
    s->clip_held_generation = 0;
    s->caps_seen = 0;
    s->ctrl_down = s->alt_down = s->shift_down = false;
    memset(s->key_down, 0, sizeof(s->key_down));
    memset(s->consumed, 0, sizeof(s->consumed));
    s->n_attach = s->n_commit = s->n_reject = 0;
    /*
     * THE GUEST'S CURSOR IS CONNECTION STATE.  A --persist broker must not
     * show the previous VM's pointer image over the next VM's picture, or over
     * the placeholder.  The image bytes are left in place (they are the next
     * SET's to overwrite, and `defined` is what every reader checks); `gen`
     * moves so no backend mistakes a later image for one it already rendered.
     *
     * Told to the backend NOW rather than through the pacing in
     * nb_sink_tick(): forgetting is not a flood risk, and a stale image
     * surviving the VM it belonged to for even one interval is the wrong
     * picture on screen.  BOTH copies are forgotten: the published one is
     * what the backend holds a pointer to, and the pending one must never be
     * published later on the next client's behalf.
     */
    s->cursor.defined = false;
    s->cursor.visible = false;
    s->cursor.gen++;
    nb_cursor_copy(&s->cursor_pub, &s->cursor);
    s->cursor_dirty = false;
    s->cursor_applied_ms = 0;
    s->n_cursor = s->n_cursor_reject = 0;
    if (s->sess && s->sess->ops->cursor) {
        s->sess->ops->cursor(s->sess, &s->cursor_pub);
    }
    /* The previous client's buffers provoked these: see fmt_refused. */
    memset(&s->fmt_refused, 0, sizeof s->fmt_refused);
    memset(&s->fmt_told, 0, sizeof s->fmt_told);
}

bool nb_sink_want_write(const struct nb_sink *s)
{
    return s->client_fd >= 0 && s->tx_head != s->tx_tail;
}

/* Forward: the clipboard helpers are defined with the rest of the clipboard
 * code, below the input path that uses them. */
static const struct nb_clip_trigger *
nb_clip_trigger(const struct nb_sink *s, unsigned code);
static uint64_t nb_now_ms(void);

static unsigned nb_tx_used(const struct nb_sink *s)
{
    return (s->tx_head + NB_TXRING - s->tx_tail) % NB_TXRING;
}

/*
 * Compact the queue by merging pointer motion, leaving everything else alone.
 *
 * The drop policy is not uniform and must not be:
 *
 *   - pointer motion is COALESCED.  Absolute is latest-wins (a position is a
 *     position; older ones are not information).  Relative is ACCUMULATED into
 *     one delta, which is exactly equivalent to delivering them separately.
 *     Either way a burst of motion collapses to one packet and no motion is
 *     lost — the pointer ends up in the same place.
 *   - key and button events are NEVER dropped.  They are low-rate, and losing
 *     a release without its press leaves a modifier latched down in the guest,
 *     which is far worse than a coarse mouse path.
 *   - FRAME is coalesced to at most one outstanding: it is a level, not an
 *     edge, and a backlog of stale pacing signals is only latency.
 *
 * One pass over a fixed-size ring.  No allocation, and the work is bounded by
 * NB_TXRING regardless of what any client does.
 */
static void nb_tx_coalesce(struct nb_sink *s)
{
    /* static: 12 KB does not belong on a stack frame, and the broker is
     * single-threaded so there is no re-entrancy to worry about. */
    static struct nvkvm_broker_pkt out[NB_TXRING];
    unsigned n = 0, cur = s->tx_tail;
    int abs_slot = -1, rel_slot = -1, frame_slot = -1;

    /* A partially written packet at the tail must survive untouched, or the
     * client sees the second half of one packet and the first half of another. */
    if (s->tx_partial) {
        out[n++] = s->tx[cur];
        cur = (cur + 1u) % NB_TXRING;
    }
    while (cur != s->tx_head) {
        struct nvkvm_broker_pkt *p = &s->tx[cur];

        cur = (cur + 1u) % NB_TXRING;
        if (p->type == NVKVM_BROKER_EV_ABS && abs_slot >= 0) {
            out[abs_slot].x = p->x;
            out[abs_slot].y = p->y;
            out[abs_slot].w0 = p->w0;
            out[abs_slot].w1 = p->w1;
            out[abs_slot].seq = p->seq;
            continue;
        }
        if (p->type == NVKVM_BROKER_EV_REL && rel_slot >= 0) {
            int64_t x = (int64_t)out[rel_slot].x + p->x;
            int64_t y = (int64_t)out[rel_slot].y + p->y;

            out[rel_slot].x = x > INT32_MAX ? INT32_MAX :
                              x < INT32_MIN ? INT32_MIN : (int32_t)x;
            out[rel_slot].y = y > INT32_MAX ? INT32_MAX :
                              y < INT32_MIN ? INT32_MIN : (int32_t)y;
            out[rel_slot].seq = p->seq;
            continue;
        }
        if (p->type == NVKVM_BROKER_EV_FRAME && frame_slot >= 0) {
            out[frame_slot].seq = p->seq;
            continue;
        }
        if (p->type == NVKVM_BROKER_EV_ABS) {
            abs_slot = (int)n;
        } else if (p->type == NVKVM_BROKER_EV_REL) {
            rel_slot = (int)n;
        } else if (p->type == NVKVM_BROKER_EV_FRAME) {
            frame_slot = (int)n;
        } else if (p->type == NVKVM_BROKER_EV_GRAB ||
                   p->type == NVKVM_BROKER_EV_FOCUS) {
            /* Motion either side of a state change is not the same motion.
             * Start fresh runs so nothing is merged across the boundary. */
            abs_slot = rel_slot = -1;
        }
        out[n++] = *p;
    }
    memcpy(s->tx, out, (size_t)n * sizeof(out[0]));
    s->tx_tail = 0;
    s->tx_head = n;
}

/*
 * Enqueue.  Never blocks, never allocates, never grows.
 *
 * Pressure is handled by coalescing motion (above) first, and only if that
 * does not free a slot — i.e. the backlog is genuinely all keystrokes, which
 * at human typing rates means the client has been unresponsive for minutes —
 * do we disconnect.  Dropping a key silently is not on the menu.
 */
static void nb_emit(struct nb_sink *s, int type, int x, int y,
                    uint32_t w0, uint32_t w1)
{
    unsigned next;

    if (s->client_fd < 0) {
        return;
    }
    /* Coalesce early, not just at the brink: a backlog that is allowed to grow
     * is latency the pointer never gets back. */
    if (nb_tx_used(s) > NB_TXRING / 4) {
        nb_tx_coalesce(s);
    }
    next = (s->tx_head + 1u) % NB_TXRING;
    if (next == s->tx_tail) {
        nb_sink_detach(s, "client stopped draining its socket");
        return;
    }
    s->tx[s->tx_head] = (struct nvkvm_broker_pkt){
        .type  = (uint16_t)type,
        .flags = (uint16_t)((s->grabbed ? NVKVM_BROKER_F_GRABBED : 0) |
                            (s->focused ? NVKVM_BROKER_F_FOCUSED : 0) |
                            (s->fullscreen ? NVKVM_BROKER_F_FULLSCREEN : 0)),
        .seq   = s->seq++,
        .x = x, .y = y, .w0 = w0, .w1 = w1,
    };
    s->tx_head = next;
}

int nb_sink_flush(struct nb_sink *s)
{
    while (s->client_fd >= 0 && s->tx_tail != s->tx_head) {
        const char *p = (const char *)&s->tx[s->tx_tail];
        size_t left = NVKVM_BROKER_PKT_SIZE - s->tx_partial;
        ssize_t n = send(s->client_fd, p + s->tx_partial, left,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;           /* try again when poll says writable */
            }
            return -errno;          /* EPIPE etc. — client is gone */
        }
        s->tx_partial += (size_t)n;
        if (s->tx_partial == NVKVM_BROKER_PKT_SIZE) {
            s->tx_partial = 0;
            s->tx_tail = (s->tx_tail + 1u) % NB_TXRING;
        }
    }
    return 0;
}

/* Blocking send used only during the handshake, when the socket buffer is
 * known-empty. */
static int nb_send_now(int sock, const struct nvkvm_broker_pkt *pkt)
{
    struct iovec iov = { .iov_base = (void *)pkt,
                         .iov_len  = NVKVM_BROKER_PKT_SIZE };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t n;

    do {
        n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return -errno;
    }
    /*
     * A unix SOCK_STREAM sendmsg of 24 bytes into an empty buffer is atomic in
     * practice, but "in practice" is not a guarantee and a short write here
     * would desynchronise the stream.  Treat it as fatal.
     */
    if (n != (ssize_t)NVKVM_BROKER_PKT_SIZE) {
        return -EPROTO;
    }
    return 0;
}

/* Handshake packets go out directly rather than through the ring, but they
 * still consume sequence numbers: the protocol says a gap means packets were
 * dropped, so the handshake must not create one. */
static struct nvkvm_broker_pkt nb_pkt(struct nb_sink *s, int type,
                                      int x, int y, uint32_t w0, uint32_t w1)
{
    struct nvkvm_broker_pkt p = {
        .type  = (uint16_t)type,
        .flags = (uint16_t)((s->grabbed ? NVKVM_BROKER_F_GRABBED : 0) |
                            (s->focused ? NVKVM_BROKER_F_FOCUSED : 0) |
                            (s->fullscreen ? NVKVM_BROKER_F_FULLSCREEN : 0)),
        .seq   = s->seq++,
        .x = x, .y = y, .w0 = w0, .w1 = w1,
    };
    return p;
}

int nb_sink_attach(struct nb_sink *s, int fd)
{
    struct nb_session *ss = s->sess;
    struct nvkvm_broker_pkt p;
    int r;

    nb_client_state_reset(s);
    s->client_generation++;
    /*
     * A new client never inherits a grab.  If the previous client died while
     * grabbed we would otherwise hand a fresh, unproven process a keyboard
     * that captures everything.
     */
    if (s->grabbed) {
        ss->ops->set_grab(ss, false);
        s->grabbed = false;
    }
    s->client_fd = fd;

    /* CAP_DEVICE is the CORE's promise, not a backend's: the EV_DEVICE below
     * is sent on every backend, carrying "unknown" where the backend could
     * not find out. */
    p = nb_pkt(s, NVKVM_BROKER_EV_HELLO, 0, 0,
               NVKVM_BROKER_PROTO_VERSION, ss->caps | NVKVM_BROKER_CAP_DEVICE);
    r = nb_send_now(fd, &p);
    if (r == 0) {
        p = nb_pkt(s, NVKVM_BROKER_EV_SURFACE,
                   (int)ss->width, (int)ss->height, ss->refresh_mhz, 0);
        r = nb_send_now(fd, &p);
    }
    if (r == 0) {
        p = nb_pkt(s, NVKVM_BROKER_EV_FOCUS, s->focused, 0, 0, 0);
        r = nb_send_now(fd, &p);
    }
    if (r == 0) {
        p = nb_pkt(s, NVKVM_BROKER_EV_GRAB, 0, 0, 0, 0);
        r = nb_send_now(fd, &p);
    }
    if (r == 0) {
        /* Free-running until the first commit: without this the client waits
         * for a pacing signal that only a commit can produce, and never
         * commits.  One FRAME primes the pump. */
        p = nb_pkt(s, NVKVM_BROKER_EV_FRAME, 0, 0, 0, 0);
        r = nb_send_now(fd, &p);
    }
    if (r == 0) {
        /*
         * AFTER the FRAME, deliberately: the five packets above are the
         * handshake every existing client was written against, byte for byte,
         * and a client that reads exactly five and then starts drawing still
         * does.  A relay that wants the device before its first frame reads
         * what is already buffered -- all six leave here in one burst.
         */
        p = nb_pkt(s, NVKVM_BROKER_EV_DEVICE, (int)ss->dev_flags, 0,
                   ss->dev_major, ss->dev_minor);
        r = nb_send_now(fd, &p);
    }
    if (r != 0) {
        nb_err("handshake failed: %s", strerror(-r));
        s->client_fd = -1;
        s->client_generation++;
        nb_client_state_reset(s);
        return r;
    }

    nb_log("client attached: window %ux%u, capabilities 0x%x, display device "
           "%s%u:%u", ss->width, ss->height, ss->caps | NVKVM_BROKER_CAP_DEVICE,
           (ss->dev_flags & NVKVM_BROKER_DEVICE_F_KNOWN)
               ? ((ss->dev_flags & NVKVM_BROKER_DEVICE_F_RENDER)
                      ? "render node " : "node ")
               : "unknown ",
           ss->dev_major, ss->dev_minor);
    nb_log("grab: CTRL+ALT+G toggles, CTRL+ALT+F fullscreen. %s",
           ss->grab_caveat[0] ? ss->grab_caveat
                              : "all keyboard input is captured under grab.");
    /* The client missed every edge that happened before it connected. */
    ss->sink = s;
    if (ss->ops->resync) {
        ss->ops->resync(ss);
    }
    return 0;
}

void nb_sink_detach(struct nb_sink *s, const char *why)
{
    if (s->client_fd < 0) {
        return;
    }
    if (s->sess->ops->client_detach) {
        s->sess->ops->client_detach(s->sess, s->client_generation);
    }
    close(s->client_fd);
    s->client_fd = -1;
    if (s->grabbed) {
        /* Never leave the host's keyboard grabbed because the VMM died. */
        s->sess->ops->set_grab(s->sess, false);
        s->grabbed = false;
    }
    /*
     * AUDIT B-2.  A dialog asking what to do about a VM is meaningless once
     * that VM is gone, and its "you already asked" memory belongs to the
     * connection, not to the window.
     */
    if (s->sess->ops->dismiss_dialog) {
        s->sess->ops->dismiss_dialog(s->sess);
    }
    nb_log("client detached: %s "
           "(%llu attach, %llu commit, %llu rejected; %llu cursor, "
           "%llu cursor refused)", why,
           (unsigned long long)s->n_attach,
           (unsigned long long)s->n_commit,
           (unsigned long long)s->n_reject,
           (unsigned long long)s->n_cursor,
           (unsigned long long)s->n_cursor_reject);
    s->client_pid = 0;
    s->client_generation++;
    nb_client_state_reset(s);
}

static void nb_release_all(struct nb_sink *s)
{
    unsigned c;

    for (c = 0; c < 768; c++) {
        if (bit_get(s->key_down, c)) {
            bit_set(s->key_down, c, false);
            nb_emit(s, (c >= BTN_MISC && c < KEY_OK)
                        ? NVKVM_BROKER_EV_BTN : NVKVM_BROKER_EV_KEY,
                    (int)c, 0, 0, 0);
        }
    }
    s->ctrl_down = s->alt_down = s->shift_down = false;
}

static void nb_set_grab(struct nb_sink *s, bool on)
{
    if (s->grabbed == on) {
        return;
    }
    /*
     * A grab that cannot be dropped automatically on focus loss is a
     * keylogger, so on a session where focus loss is not observable we do not
     * offer grab at all.  Degrade visibly rather than silently.
     */
    if (on && !(s->sess->caps & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
        nb_err("grab refused: this session cannot report focus loss, so the "
               "grab could not be dropped automatically. %s",
               s->sess->grab_caveat);
        return;
    }
    if (s->sess->ops->set_grab(s->sess, on) != 0) {
        nb_err("the session refused to %s the grab; staying %s",
               on ? "take" : "drop", s->grabbed ? "grabbed" : "ungrabbed");
        return;
    }
    /*
     * Releasing everything across the transition is not tidiness.  Going in,
     * the modifiers of the CTRL+ALT+G chord itself would otherwise arrive in
     * the guest as stuck-down; coming out, whatever was held stays held in a
     * guest that can no longer see the key-up.
     */
    nb_release_all(s);
    s->grabbed = on;
    /*
     * AUDIT 2026-08-27 S-5.  nb_release_all() EMITS, and an emit into a ring
     * the client has stopped draining calls nb_sink_detach() from underneath
     * us.  That detach carries the rule "never leave the host's keyboard
     * grabbed because the VMM died" -- but it read s->grabbed before the line
     * above set it, so of all the transitions it protects, the one it missed
     * was the grab being taken.  The display-side grab was already held by
     * then, so the broker came out of here holding the user's keyboard with no
     * client to send it to.  A hostile client can arrange the moment: nothing
     * stops it filling the ring on demand (QUERY_FORMAT answers are not
     * coalesced) and then never reading.
     *
     * Re-check after the emit rather than reordering the assignment, so the
     * key-ups above still go out with the flags they were generated under.
     */
    if (on && s->client_fd < 0) {
        s->sess->ops->set_grab(s->sess, false);
        s->grabbed = false;
        nb_log("grab dropped: the client went away while it was being taken");
        return;
    }
    nb_emit(s, NVKVM_BROKER_EV_GRAB, on, 0, 0, 0);
    nb_log("grab %s", on ? "ON" : "off");
}

void nb_sink_key(struct nb_sink *s, unsigned code, bool down)
{
    const struct nb_clip_trigger *trigger;

    if (code >= 768) {
        return;
    }
    /* Modifier latch first: it must track reality even while unfocused, or
     * the first chord after a focus change is missed. */
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        s->ctrl_down = down;
    }
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) {
        s->alt_down = down;
    }
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        s->shift_down = down;
    }

    /* Hotkeys are the broker's, never the client's. */
    if (down && s->ctrl_down && s->alt_down &&
        (code == KEY_G || code == KEY_F)) {
        bit_set(s->consumed, code, true);
        /*
         * TURNING EITHER ON CANCELS A DIALOG.  Grabbing while a host dialog is
         * up, or going fullscreen over it, are contradictory states; the later
         * action wins rather than leaving the two to fight.  Done before the
         * state changes so the backend sees a consistent world.
         */
        if (s->sess->ops->dismiss_dialog &&
            ((code == KEY_G && !s->grabbed) ||
             (code == KEY_F && !s->fullscreen))) {
            s->sess->ops->dismiss_dialog(s->sess);
        }
        if (code == KEY_G) {
            nb_set_grab(s, !s->grabbed);
        } else {
            s->fullscreen = !s->fullscreen;
            if (s->sess->ops->set_fullscreen(s->sess, s->fullscreen) != 0) {
                s->fullscreen = !s->fullscreen;
            }
        }
        return;
    }
    /*
     * PASTE TRIGGER.  The key is NOT forwarded here.
     *
     * The guest's paste handler runs the moment it sees the keystroke, so
     * content that arrives afterwards finds stale or empty data.  Reading the
     * host selection on Wayland is asynchronous (an offer, a pipe, a read), so
     * the key is HELD and replayed by nb_sink_clip_finish() once the content
     * is queued -- or immediately, if nothing is going to be sent.
     */
    trigger = down && s->focused ? nb_clip_trigger(s, code) : NULL;
    if (trigger) {
        /* Auto-repeat while an asynchronous fetch is pending is still the same
         * physical keypress.  Consume it; never replace the in-flight chord. */
        bit_set(s->consumed, code, true);
        if (s->clip_held_key) {
            return;
        }
        s->clip_held_key = code;
        s->clip_held_ctrl = trigger->need_ctrl;
        s->clip_held_shift = trigger->need_shift;
        s->clip_held_generation = s->client_generation;
        nb_sink_paste_trigger(s);
        return;
    }
    if (bit_get(s->consumed, code)) {
        if (!down) {
            bit_set(s->consumed, code, false);
        }
        return;
    }

    if (!s->focused) {
        return;             /* spec: keyboard only while the window is active */
    }
    if (down == bit_get(s->key_down, code)) {
        return;             /* drop auto-repeat and duplicate edges */
    }
    bit_set(s->key_down, code, down);
    nb_emit(s, NVKVM_BROKER_EV_KEY, (int)code, down, 0, 0);
}

void nb_sink_btn(struct nb_sink *s, unsigned code, bool down)
{
    if (code >= 768 || !s->focused) {
        return;
    }
    /* Without a grab, a click outside the window is not ours to forward. */
    if (!s->grabbed && !s->pointer_in) {
        return;
    }
    if (down == bit_get(s->key_down, code)) {
        return;
    }
    bit_set(s->key_down, code, down);
    nb_emit(s, NVKVM_BROKER_EV_BTN, (int)code, down, 0, 0);
}

void nb_sink_abs(struct nb_sink *s, int x, int y, unsigned w, unsigned h)
{
    /* Under grab the client must never learn where the host pointer is. */
    if (s->grabbed || !s->focused || !s->pointer_in) {
        return;
    }
    nb_emit(s, NVKVM_BROKER_EV_ABS, x, y, w, h);
}

void nb_sink_rel(struct nb_sink *s, int dx, int dy)
{
    if (!s->grabbed || !s->focused) {
        return;
    }
    if (dx == 0 && dy == 0) {
        return;
    }
    nb_emit(s, NVKVM_BROKER_EV_REL, dx, dy, 0, 0);
}

void nb_sink_wheel(struct nb_sink *s, int v, int h)
{
    if (!s->focused || (!s->grabbed && !s->pointer_in)) {
        return;
    }
    nb_emit(s, NVKVM_BROKER_EV_WHEEL, v, h, 0, 0);
}

void nb_sink_focus(struct nb_sink *s, bool active)
{
    if (s->focused == active) {
        return;
    }
    s->focused = active;
    if (!active) {
        /*
         * SECURITY PROPERTY, not a convenience.  A grab that survives focus
         * loss is a keylogger: the host user switches to their password
         * manager and the guest keeps receiving the keystrokes.  Drop the
         * grab, then release everything, then tell the client.
         */
        if (s->grabbed) {
            s->sess->ops->set_grab(s->sess, false);
            s->grabbed = false;
            nb_log("grab dropped: the window lost focus");
        }
        nb_release_all(s);
        s->pointer_in = false;
    }
    nb_emit(s, NVKVM_BROKER_EV_FOCUS, active, 0, 0, 0);
    if (!active) {
        nb_emit(s, NVKVM_BROKER_EV_GRAB, 0, 0, 0, 0);
    }
}

void nb_sink_format_verdict(struct nb_sink *s, uint32_t fourcc, uint64_t mod,
                            bool usable)
{
    nb_emit(s, NVKVM_BROKER_EV_FORMAT, usable ? 1 : 0, (int)fourcc,
            (uint32_t)(mod & 0xffffffffu), (uint32_t)(mod >> 32));
}

/*
 * Tell the client x=0 for a pair, unless it has already been told on this
 * connection.  Every x=0 the broker volunteers goes through here.
 */
static void nb_format_tell_no(struct nb_sink *s, uint32_t fourcc,
                              uint64_t mod)
{
    if (nb_refused_has(&s->fmt_told, fourcc, mod)) {
        return;
    }
    nb_refused_add(&s->fmt_told, fourcc, mod);
    nb_sink_format_verdict(s, fourcc, mod, false);
}

void nb_sink_format_refused(struct nb_sink *s, uint32_t fourcc, uint64_t mod,
                            bool both_twins)
{
    /*
     * The other member of an alpha pair, whichever one was used: AR24's twin
     * is XR24 and XR24's is AR24.  nb_fourcc_opaque_twin() only answers the
     * alpha -> opaque direction, so the reverse is found by asking it about
     * each candidate.
     */
    static const uint32_t alpha[] = {
        0x34325241u,    /* AR24 */
        0x34324241u,    /* AB24 */
    };
    uint32_t twin = 0;
    unsigned i;

    nb_refused_add(&s->fmt_refused, fourcc, mod);
    nb_format_tell_no(s, fourcc, mod);
    if (!both_twins) {
        return;
    }
    twin = nb_fourcc_opaque_twin(fourcc);
    for (i = 0; !twin && i < sizeof alpha / sizeof alpha[0]; i++) {
        if (nb_fourcc_opaque_twin(alpha[i]) == fourcc) {
            twin = alpha[i];
        }
    }
    if (twin) {
        /*
         * AND SAY SO FOR THE TWIN TOO.  Remembering it without announcing it
         * is the bug this exists to prevent: a relay that was told x=1 for
         * the twin keeps sending it, the resolver refuses every frame, and
         * nothing on the wire says why -- a black window.
         */
        nb_refused_add(&s->fmt_refused, twin, mod);
        nb_format_tell_no(s, twin, mod);
    }
}

void nb_sink_pointer(struct nb_sink *s, bool inside)
{
    if (s->pointer_in == inside) {
        return;
    }
    s->pointer_in = inside;
    nb_emit(s, NVKVM_BROKER_EV_POINTER, inside, 0, 0, 0);
}

void nb_sink_surface(struct nb_sink *s, unsigned w, unsigned h,
                     unsigned refresh_mhz)
{
    if (w == 0 || h == 0) {
        return;
    }
    /* A refresh-only host mode change matters to a VMM's virtual vblank even
     * when the pixel dimensions stay fixed. Cache it for reconnects too. */
    if (s->sess->width == w && s->sess->height == h &&
        s->sess->refresh_mhz == refresh_mhz) {
        return;
    }
    s->sess->width = w;
    s->sess->height = h;
    s->sess->refresh_mhz = refresh_mhz;
    /* w0 carries the host's refresh in MILLIHERTZ, 0 when unknown.  It was
     * a reserved zero, so an older VMM reads it as 'unknown' -- which is
     * what it meant. */
    nb_emit(s, NVKVM_BROKER_EV_SURFACE, (int)w, (int)h, refresh_mhz, 0);
}

void nb_sink_frame(struct nb_sink *s)
{
    nb_emit(s, NVKVM_BROKER_EV_FRAME, 0, 0, 0, 0);
}

void nb_sink_release(struct nb_sink *s, uint64_t buf_id)
{
    nb_emit(s, NVKVM_BROKER_EV_RELEASE, 0, 0,
            (uint32_t)buf_id, (uint32_t)(buf_id >> 32));
}

/* ── clipboard ───────────────────────────────────────────────────────────── */

/* Does this key, with the modifiers currently latched, mean "paste"? */
static const struct nb_clip_trigger *
nb_clip_trigger(const struct nb_sink *s, unsigned code)
{
    const struct nb_config *cfg = s->cfg;
    int i;

    if (cfg->clip_mode == NB_CLIP_OFF && s->clip_told_off) {
        /* Still recognised once, so the first attempt can explain itself;
         * after that it is an ordinary key and belongs to the guest. */
        return NULL;
    }
    for (i = 0; i < cfg->n_clip_trigger; i++) {
        const struct nb_clip_trigger *t = &cfg->clip_trigger[i];

        if (t->code == code && t->need_ctrl == s->ctrl_down &&
            t->need_shift == s->shift_down) {
            return t;
        }
    }
    return NULL;
}

static bool nb_guest_modifier_down(const struct nb_sink *s,
                                   unsigned left, unsigned right)
{
    return bit_get(s->key_down, left) || bit_get(s->key_down, right);
}

static void nb_clip_key_edge(struct nb_sink *s, unsigned code, bool down)
{
    bit_set(s->key_down, code, down);
    nb_emit(s, NVKVM_BROKER_EV_KEY, (int)code, down, 0, 0);
}

/*
 * Finish the chord held while the host selection was fetched.  A physical
 * key-up may have arrived meanwhile, and its modifiers may have gone up too.
 * Replaying one balanced chord here means neither timing can leave V/Insert
 * stuck or turn CTRL+V into an unmodified V.
 */
void nb_sink_clip_finish(struct nb_sink *s, uint64_t generation, bool paste)
{
    unsigned code = s->clip_held_key;
    bool need_ctrl = s->clip_held_ctrl;
    bool need_shift = s->clip_held_shift;
    bool synth_ctrl, synth_shift;

    if (!code || generation != s->client_generation ||
        generation != s->clip_held_generation) {
        return;
    }
    s->clip_held_key = 0;
    s->clip_held_ctrl = false;
    s->clip_held_shift = false;
    s->clip_held_generation = 0;
    if (!paste) {
        nb_log("clipboard: host selection was not queued; paste key cancelled "
               "rather than pasting stale guest content");
        return;
    }
    if (!s->focused || s->client_fd < 0) {
        return;
    }

    synth_ctrl = need_ctrl &&
        !nb_guest_modifier_down(s, KEY_LEFTCTRL, KEY_RIGHTCTRL);
    synth_shift = need_shift &&
        !nb_guest_modifier_down(s, KEY_LEFTSHIFT, KEY_RIGHTSHIFT);
    if (synth_ctrl) {
        nb_clip_key_edge(s, KEY_LEFTCTRL, true);
    }
    if (synth_shift) {
        nb_clip_key_edge(s, KEY_LEFTSHIFT, true);
    }
    nb_clip_key_edge(s, code, true);
    nb_clip_key_edge(s, code, false);
    if (synth_shift) {
        nb_clip_key_edge(s, KEY_LEFTSHIFT, false);
    }
    if (synth_ctrl) {
        nb_clip_key_edge(s, KEY_LEFTCTRL, false);
    }
}

/*
 * Queue host clipboard text as fixed-size chunks, then nothing else until the
 * caller releases the held key -- so the guest has the content in hand before
 * it is told to paste.
 *
 * Refuses rather than truncates on ring pressure: half a paste is worse than
 * none, and a silent partial one is worse than either.
 */
bool nb_sink_send_clipboard(struct nb_sink *s, uint64_t generation,
                            const char *text, size_t len)
{
    /* A delayed ctrl+shift+V may need both modifiers synthesized around the
     * balanced key press after the physical keys were released: ctrl down,
     * shift down, key down/up, shift up, ctrl up.  Reserve all six slots before
     * queuing any clipboard chunk so ring pressure cannot split that chord or
     * disconnect the client halfway through it. */
    const unsigned replay_reserve = 6u;
    size_t off = 0;
    unsigned chunks;

    if (s->client_fd < 0 || generation != s->client_generation ||
        generation != s->clip_held_generation || !s->clip_held_key) {
        return false;
    }
    if (len > NVKVM_BROKER_CLIP_MAX_BYTES) {
        nb_log("clipboard: %zu bytes is over the %u-byte cap; not sending",
               len, NVKVM_BROKER_CLIP_MAX_BYTES);
        return false;
    }
    chunks = len == 0 ? 1u :
        (unsigned)((len + NVKVM_BROKER_CLIP_PKT_BYTES - 1u) /
                   NVKVM_BROKER_CLIP_PKT_BYTES);
    if (nb_tx_used(s) + chunks + replay_reserve >= NB_TXRING) {
        nb_log("clipboard: the client is not draining; dropping this paste "
               "rather than sending half of it");
        return false;
    }
    do {
        struct nvkvm_broker_clip_pkt cp;
        size_t n = len - off;

        if (n > NVKVM_BROKER_CLIP_PKT_BYTES) {
            n = NVKVM_BROKER_CLIP_PKT_BYTES;
        }
        memset(&cp, 0, sizeof(cp));
        cp.type  = NVKVM_BROKER_EV_CLIPBOARD;
        cp.flags = (uint16_t)((s->grabbed ? NVKVM_BROKER_F_GRABBED : 0) |
                              (s->focused ? NVKVM_BROKER_F_FOCUSED : 0) |
                              (s->fullscreen ? NVKVM_BROKER_F_FULLSCREEN : 0));
        cp.seq   = s->seq++;
        cp.info  = (uint8_t)n;
        if (off + n == len) {
            cp.info |= NVKVM_BROKER_CLIP_LAST;
        }
        memcpy(cp.data, text + off, n);
        memcpy(&s->tx[s->tx_head], &cp, sizeof(cp));
        s->tx_head = (s->tx_head + 1u) % NB_TXRING;
        off += n;
    } while (off < len);
    nb_log("clipboard: sent %zu bytes to the guest (%u chunks)", len, chunks);
    return true;
}

/* Clipboard traffic gets its own 1s window: it must not be able to consume the
 * general command budget, nor be starved by it. */
#define NB_CLIP_MAX_PER_SEC 4000u

static bool nb_clip_rate_exceeded(struct nb_sink *s)
{
    uint64_t now = nb_now_ms();

    if (now - s->clip_rate_ms >= 1000u) {
        s->clip_rate_ms = now;
        s->clip_rate_count = 0;
    }
    return ++s->clip_rate_count > NB_CLIP_MAX_PER_SEC;
}

/*
 * Strict UTF-8 validation.  Rejects overlong forms, surrogates and anything
 * past U+10FFFF -- the classic ways a decoder downstream is made to disagree
 * with the validator upstream.  Embedded NULs are rejected too: the host
 * clipboard is a C string from here on.
 */
static bool nb_utf8_ok(const char *p, unsigned len)
{
    unsigned i = 0;

    while (i < len) {
        unsigned char c = (unsigned char)p[i];
        unsigned need;
        uint32_t cp;

        if (c == 0) {
            return false;
        }
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xe0) == 0xc0) { need = 1; cp = c & 0x1fu; }
        else if ((c & 0xf0) == 0xe0) { need = 2; cp = c & 0x0fu; }
        else if ((c & 0xf8) == 0xf0) { need = 3; cp = c & 0x07u; }
        else { return false; }

        /* Continuation bytes i+1 .. i+need must all exist. */
        if (i + need >= len) {
            return false;                       /* truncated sequence */
        }
        for (unsigned k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)p[i + k];

            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3fu);
        }
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000)) {
            return false;                       /* overlong */
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;                       /* out of range / surrogate */
        }
        i += need + 1;
    }
    return true;
}

static const char *nb_clip_mode_name(int m)
{
    switch (m) {
    case NB_CLIP_OFF:     return "off";
    case NB_CLIP_G2H:     return "guest-to-host";
    case NB_CLIP_CONSENT: return "consent";
    default:              return "?";
    }
}

/*
 * A PASTE TRIGGER FIRED.  Everything that can go wrong here has a DIFFERENT
 * fix, so each says so distinctly and once.
 *
 * A restrictive default only works if the dead end teaches.  "Nothing happened"
 * sends people searching, and the setting they find first is `full`, because it
 * is the one that obviously works -- which is worse than either default.  So
 * every branch below names the next rung up the ladder, not the top of it.
 */
void nb_sink_paste_trigger(struct nb_sink *s)
{
    const struct nb_config *cfg = s->cfg;
    uint64_t generation = s->clip_held_generation;
    int r;

    if (cfg->clip_mode == NB_CLIP_OFF) {
        if (!s->clip_told_off) {
            s->clip_told_off = true;
            nb_log("clipboard is disabled, so this paste sent nothing. "
                   "--clipboard=consent allows host->guest paste on exactly "
                   "this trigger, and nothing else. (off | guest-to-host | "
                   "consent)");
        }
        nb_sink_clip_finish(s, generation, true);
        return;
    }
    if (cfg->clip_mode == NB_CLIP_G2H) {
        if (!s->clip_told_off) {
            s->clip_told_off = true;
            nb_log("clipboard mode is guest-to-host, which deliberately never "
                   "lets the guest read the host clipboard. "
                   "--clipboard=consent adds host->guest on this trigger.");
        }
        nb_sink_clip_finish(s, generation, true);
        return;
    }
    if (!nb_sink_has_client(s)) {
        if (!s->clip_told_noclient) {
            s->clip_told_noclient = true;
            nb_log("clipboard is enabled but no VM is connected, so there is "
                   "nothing to paste into.");
        }
        nb_sink_clip_finish(s, generation, false);
        return;
    }
    /*
     * DISTINCT FROM THE ABOVE ON PURPOSE.  "The mode is off" and "the guest
     * agent is missing" both look like nothing happening and have completely
     * different fixes; conflating them is how someone ends up at `full`
     * believing the mode was the problem.
     */
    if (!(s->caps_seen & NB_CLIENT_HAS_CLIPBOARD)) {
        if (!s->clip_told_noagent) {
            s->clip_told_noagent = true;
            nb_log("clipboard is %s, but the VM has not reported a clipboard "
                   "agent. The mode is not the problem: QEMU needs "
                   "-chardev qemu-vdagent and the guest needs spice-vdagent "
                   "running.", nb_clip_mode_name(cfg->clip_mode));
        }
        nb_sink_clip_finish(s, generation, true);
        return;
    }
    if (!s->sess->ops->fetch_clipboard) {
        nb_err("clipboard: backend '%s' cannot read the host selection",
               s->sess->ops->name);
        nb_sink_clip_finish(s, generation, false);
        return;
    }
    r = s->sess->ops->fetch_clipboard(s->sess, s, generation);
    if (r == -ENOENT) {
        nb_log("clipboard: the host selection is empty or is not text, so "
               "this paste uses the guest's existing clipboard");
        nb_sink_clip_finish(s, generation, true);
    } else if (r != 0) {
        nb_err("clipboard: could not read the host selection: %s",
               strerror(-r));
        nb_sink_clip_finish(s, generation, false);
    }
}

bool nb_sink_has_client(const struct nb_sink *s)
{
    return s && s->client_fd >= 0;
}

void nb_sink_force_ungrab(struct nb_sink *s)
{
    if (s && s->grabbed) {
        nb_log("dropping the grab: the broker is showing its own UI");
        nb_set_grab(s, false);
    }
}

void nb_sink_set_fullscreen(struct nb_sink *s, bool on)
{
    if (!s || s->fullscreen == on) {
        return;
    }
    s->fullscreen = on;
}

bool nb_sink_close_request(struct nb_sink *s, int action)
{
    if (!s || s->client_fd < 0) {
        return false;           /* nobody to tell; the caller quits instead */
    }
    nb_log("the user closed the display (%s); telling the VMM, which decides "
           "what that means for the guest",
           action == NVKVM_BROKER_CLOSE_FORCE ? "force off" : "ACPI powerdown");
    nb_emit(s, NVKVM_BROKER_EV_CLOSE, action, 0, 0, 0);
    nb_sink_flush(s);
    return true;
}

void nb_sink_bye(struct nb_sink *s, int reason)
{
    nb_emit(s, NVKVM_BROKER_EV_BYE, reason, 0, 0, 0);
    nb_sink_flush(s);
}

/* ── inbound commands: the privileged side's parser ──────────────────────── */

/*
 * HARDENING 4: is this fd really a dma-buf?
 *
 * Every dma-buf file lives on the kernel's anonymous `dmabuf` filesystem, so
 * fstatfs()'s f_type is an identity the caller cannot forge — unlike anything
 * derived from the fd's mode or from what the sender claimed.  Anything else
 * (a memfd, a socket, a pipe, an O_TMPFILE on the user's disk, /dev/mem) is a
 * protocol violation, not a buffer, and gets nowhere near an import.
 *
 * The TEST backend also takes tmpfs (memfd), because it has no import path at
 * all and exists precisely so the accept side of this validator can be
 * exercised without a GPU.  It is unreachable from --backend auto.
 *
 * ── AND NOTHING THAT ASKS THE FD'S FILESYSTEM GOES FIRST ──────────────────
 *
 * fstatfs() is not a neutral question.  It calls the superblock's ->statfs,
 * and on FUSE that is a FUSE_STATFS request to the daemon behind the mount
 * (fs/fuse/inode.c fuse_statfs(), whenever fuse_allow_current_process()
 * passes -- which it does for a broker running as the uid that mounted it,
 * the podman keep-id deployment src/broker/README.md describes).  The wait
 * for the answer is wait_event_interruptible then wait_event_killable
 * (fs/fuse/dev.c request_wait_answer()), and this process blocks its signals
 * for signalfd -- so a daemon that never answers parks the thread that holds
 * the keyboard grab until something sends SIGKILL.  fstat() and lseek() reach
 * FUSE the same way (GETATTR), and so does close() -- the one no check can
 * avoid, handled by nb_fd_drop() instead.  A client choosing the fd type must
 * not be choosing when the broker stalls, so every fd from a client is first
 * put through a check that never leaves the kernel's own code, and only an fd
 * that passes it is ever fstatfs()ed, fstat()ed or measured:
 *
 *   - "is it shmem?" -- fcntl(F_GET_SEALS).  do_fcntl() -> memfd_fcntl() ->
 *     memfd_file_seals_ptr() (mm/memfd.c): a pointer comparison on
 *     file->f_mapping->a_ops (shmem_aops) or f_op (hugetlbfs), else EINVAL,
 *     with no filesystem method called.  Measured: a memfd answers 0x0 or
 *     0x1, an fd to a /dev/shm file 0x1 (F_SEAL_SEAL, which mm/shmem.c sets
 *     on every inode memfd_create(MFD_ALLOW_SEALING) did not make), and
 *     /dev/null, a tmpfs FIFO and a file on disk EINVAL.  It also answers for
 *     hugetlbfs, which the fstatfs() that follows -- now on a filesystem whose
 *     ->statfs is kernel code -- rules out.
 *   - "is it a dma-buf?" -- /proc/self/fdinfo/N carrying an `exp_name:` line.
 *     fs/proc/fd.c seq_show() prints pos/flags/mnt_id/ino and the inode's
 *     lock list, then calls the file's own ->show_fdinfo, which a FUSE file
 *     does not have (fuse_dev_show_fdinfo is /dev/fuse's, not a mounted
 *     file's); only drivers/dma-buf/dma-buf.c dma_buf_show_fdinfo() prints
 *     `exp_name:`, and it arrived in Linux 5.3 together with the dmabuf
 *     filesystem DMA_BUF_MAGIC names, so no kernel this check already accepts
 *     lacks it.  NOT readlink("/proc/self/fd/N"): a dma-buf reads as
 *     "/dmabuf:<name>" there, but for any other file the text is a PATH, and
 *     paths are chosen by whoever mounts things -- not an identity the
 *     sender cannot shape.
 *
 * The one exception found in the kernel source: a Coda file mmapped from a
 * container file on tmpfs shares that file's mapping (fs/coda/file.c), so it
 * passes F_GET_SEALS and its fstatfs() is an upcall to Venus.  Coda is not
 * FS_USERNS_MOUNT -- only root can mount it -- and its upcalls time out.
 */
#define NB_FOURCC_XR24 0x34325258u
#define NB_FOURCC_AR24 0x34325241u

/*
 * AUDIT 2026-08-27 S-4.  A rejected frame is deliberately NOT a disconnect
 * (design §3: "a guest flipping nonsense must not be able to kill the display
 * of a VMM that is behaving"), which makes the rejection path the one thing a
 * hostile client can drive forever.  Every nb_err() is a vfprintf followed by
 * an fflush -- a blocking write on the thread that also services the display
 * server and the keyboard.  At the B-1b command ceiling that is ~20,000
 * fflush()ed lines a second, megabytes a second into the session's journal,
 * and on a log pipe that stops draining it is B-1's consequence reached
 * through a different door: the main loop stalls and the grab cannot be
 * released.
 *
 * So the reject path gets its own budget, and says how many it swallowed --
 * a silent throttle would hide the very flood it is there to bound.
 */
#define NB_REJECT_LOG_PER_SEC 10u

static bool nb_reject_log(struct nb_sink *s)
{
    uint64_t now = nb_now_ms();

    if (now - s->rej_log_ms >= 1000u) {
        unsigned hidden = s->rej_log_hidden;

        s->rej_log_ms     = now;
        s->rej_log_count  = 0;
        s->rej_log_hidden = 0;
        if (hidden) {
            nb_err("... and %u more frames rejected in that second, not "
                   "logged (the client is being refused faster than this can "
                   "be written down)", hidden);
        }
    }
    if (++s->rej_log_count > NB_REJECT_LOG_PER_SEC) {
        s->rej_log_hidden++;
        return false;
    }
    return true;
}

/*
 * Is `fd` shmem (a memfd, or any other tmpfs file), proved without asking the
 * fd's filesystem anything -- see HARDENING 4.  F_GET_SEALS first, and only
 * then fstatfs(): it narrows "shmem or hugetlbfs" to tmpfs, and by then the
 * ->statfs it calls is the kernel's own.  Returns the seals, or -1.
 */
static int nb_fd_shmem_seals(int fd)
{
    struct statfs sfs;
    int seals = fcntl(fd, F_GET_SEALS);

    if (seals < 0) {
        return -1;
    }
    if (fstatfs(fd, &sfs) < 0 || sfs.f_type != TMPFS_MAGIC) {
        return -1;
    }
    return seals;
}

/*
 * Does /proc/self/fdinfo/<fd> carry the line only a dma-buf's fdinfo has?
 * 1 yes, 0 no, -errno when it could not be read (no /proc): see HARDENING 4.
 * Bounded: 4 KiB is far past a dma-buf's six lines; a file whose lock list
 * pushes the line past it is refused, which only an fd's own sender can cause.
 */
static int nb_fd_fdinfo_is_dmabuf(int fd)
{
    char path[48];
    char buf[4096 + 1];
    size_t got = 0;
    int pfd;

    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    pfd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (pfd < 0) {
        return -errno;
    }
    while (got < sizeof buf - 1) {
        ssize_t n = read(pfd, buf + got, sizeof buf - 1 - got);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(pfd);
    buf[got] = '\0';
    /* Never the first line (pos: is), so a line start is always "\n". */
    return strstr(buf, "\nexp_name:\t") != NULL;
}

/* 1 a dma-buf (or, with allow_memfd, shmem), 0 neither, -errno when the
 * in-kernel proof could not be read -- the caller refuses that too. */
static int nb_fd_is_dmabuf(int fd, bool allow_memfd)
{
    struct statfs sfs;
    int r = nb_fd_fdinfo_is_dmabuf(fd);

    if (r < 0) {
        return r;
    }
    if (r > 0) {
        /* Only now is fstatfs() a question the kernel answers itself. */
        return fstatfs(fd, &sfs) == 0 && sfs.f_type == DMA_BUF_MAGIC;
    }
    return allow_memfd && nb_fd_shmem_seals(fd) >= 0;
}

/* ── closing a client's fd ───────────────────────────────────────────────── */

/*
 * CLOSING IS A FILESYSTEM CALL TOO.  close(2) runs the file's ->flush
 * (fs/open.c filp_flush()), and FUSE's is a FUSE_FLUSH request to the daemon
 * sent with args.force set (fs/fuse/file.c fuse_flush()) -- which makes
 * request_wait_answer() end in a plain, UNINTERRUPTIBLE wait_event() once the
 * interruptible phase passes, and with this process's signals blocked for
 * signalfd nothing interrupts that phase.  Unlike the fstatfs() HARDENING 4
 * moves out of the way, FLUSH is sent whatever the caller's credentials.  So
 * the moment the broker had refused a FUSE fd without asking it anything, the
 * close that followed handed the same daemon the same thread -- the one
 * holding the keyboard grab -- in D state, until the daemon answered or died.
 * Nothing about the fd can be checked first that removes this: the fd is in
 * our table from the moment recvmsg() returns, and every way out of the table
 * (close, dup2 over it, close_range, process exit) flushes.
 *
 * So a client's fd is closed HERE only when it is proved -- in-kernel, as in
 * HARDENING 4 -- to be a type whose file_operations have no ->flush: shmem and
 * hugetlbfs (F_GET_SEALS answers; shmem_file_operations,
 * hugetlbfs_file_operations and SysV shm's have none) or a dma-buf (dma_buf_fops
 * has none).  Every other fd -- the ones the validators refuse -- goes to a
 * reaper thread that closes it, and if a filesystem makes that close wait,
 * the reaper waits instead of the broker.  An honest client never reaches it:
 * it sends only dma-bufs and memfds.
 *
 * The thread is started lazily, on the first such fd, so a broker that never
 * meets one stays single-threaded -- and is always started after --drop-user,
 * because glibc's setuid() must interrupt every thread and cannot interrupt
 * one stuck in D state.  It blocks every signal: signalfd in the main thread
 * stays the only consumer.  The queue is a pipe of fd numbers; while a close
 * is stuck the numbers wait there and their fds stay open, bounded by
 * RLIMIT_NOFILE -- past which recvmsg() itself drops what it cannot install
 * (MSG_CTRUNC, a violation), so the cost of a stuck reaper is fds, never the
 * main loop.  If even the pipe is full the fd is HELD, deliberately leaked,
 * rather than closed here.
 */
static int nb_reap_wr = -1;
static unsigned long long nb_reap_held;

static void *nb_reaper(void *arg)
{
    int rd = (int)(intptr_t)arg;

    for (;;) {
        int fd;
        ssize_t n = read(rd, &fd, sizeof fd);

        if (n == (ssize_t)sizeof fd) {
            close(fd);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;          /* EOF or error: nothing more will ever be queued */
    }
    close(rd);
    return NULL;
}

static bool nb_reaper_start(void)
{
    static bool failed;
    pthread_attr_t attr;
    pthread_t tid;
    sigset_t all, old;
    int p[2], r;

    if (nb_reap_wr >= 0) {
        return true;
    }
    if (failed) {
        return false;
    }
    if (pipe2(p, O_CLOEXEC) < 0) {
        failed = true;
        return false;
    }
    /* Never block the main thread on the queue either. */
    if (fcntl(p[1], F_SETFL, O_NONBLOCK) < 0) {
        close(p[0]);
        close(p[1]);
        failed = true;
        return false;
    }
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64u * 1024u);
    r = pthread_create(&tid, &attr, nb_reaper, (void *)(intptr_t)p[0]);
    pthread_attr_destroy(&attr);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r != 0) {
        close(p[0]);
        close(p[1]);
        failed = true;
        return false;
    }
    nb_reap_wr = p[1];
    nb_log("a client sent an fd that is neither a dma-buf nor shared memory; "
           "fds like it are closed on a separate thread from now on, so a "
           "filesystem that makes close() wait cannot stall the broker");
    return true;
}

/* Does this fd's close() stay in the kernel's own code?  See above. */
static bool nb_fd_close_is_local(int fd)
{
    return fcntl(fd, F_GET_SEALS) >= 0 || nb_fd_fdinfo_is_dmabuf(fd) > 0;
}

static void nb_fd_drop(int fd)
{
    if (fd < 0) {
        return;
    }
    if (nb_fd_close_is_local(fd)) {
        close(fd);
        return;
    }
    if (nb_reaper_start()) {
        ssize_t n;

        do {
            n = write(nb_reap_wr, &fd, sizeof fd);
        } while (n < 0 && errno == EINTR);
        if (n == (ssize_t)sizeof fd) {
            return;
        }
    }
    if (nb_reap_held++ == 0) {
        nb_err("a client's fd could not be handed to the closing thread (it "
               "is stuck on an earlier close, or could not start); HOLDING it "
               "open rather than closing it here, where a filesystem could "
               "stall the broker.  Every further such fd is held too.");
    }
}

static void nb_violation(struct nb_sink *s, const char *why)
{
    nb_err("protocol violation from pid %d: %s", (int)s->client_pid, why);
    nb_sink_bye(s, NVKVM_BROKER_BYE_PROTOCOL);
    nb_sink_detach(s, "protocol violation");
}

/*
 * Is the pair usable at all: advertised by the display, AND not refused by it
 * since on this connection (nb_sink_format_refused()), AND a layout whose
 * extent the broker can bound (nb_modifier_layout()).  All three are checked
 * HERE, in the core, so that ATTACH and QUERY_FORMAT cannot disagree about any
 * of them on any backend.
 *
 * The third is not the display's question but it has to be asked here all the
 * same: a modifier the display advertises and nb_frame_extent() cannot decode
 * -- another vendor's tiling, or an NVIDIA field from a newer drm_fourcc.h --
 * would otherwise be answered "yes" and then have every frame refused by the
 * extent check, a black window with nothing on the wire to say why.  Asked
 * here, the VMM hears x=0 and falls back to LINEAR like any other "no".
 */
static bool nb_format_usable(struct nb_sink *s, uint32_t fourcc,
                             uint64_t modifier)
{
    struct nb_session *ss = s->sess;
    struct nb_layout layout;
    const char *why;

    if (nb_refused_has(&s->fmt_refused, fourcc, modifier)) {
        return false;
    }
    if (nb_modifier_layout(modifier, &layout, &why) != 0) {
        return false;
    }
    return ss->ops->format_ok(ss, fourcc, modifier);
}

/*
 * Resolve a requested (fourcc, modifier) to one the display actually
 * advertises, or refuse.
 *
 * ONE code path, used by ATTACH and by CMD_QUERY_FORMAT, so the answer the VMM
 * is given can never disagree with what the next frame does.  A query that says
 * "yes" followed by a rejected ATTACH would be worse than no query at all: the
 * VMM would settle on zero-copy and every frame would vanish.
 *
 * Returns true and writes the fourcc to USE (possibly the opaque twin) to
 * *use_fourcc; false means no description of this buffer is displayable.
 */
static bool nb_format_resolve(struct nb_sink *s, uint32_t fourcc,
                              uint64_t modifier, uint32_t *use_fourcc)
{
    if (nb_format_usable(s, fourcc, modifier)) {
        *use_fourcc = fourcc;
        return true;
    }
    /*
     * Try the OPAQUE TWIN of an alpha format.
     *
     * This is not a relaxation of HARDENING 3: the pair we fall back to must
     * ITSELF be advertised, so the compositor is still only ever handed
     * something it said it could import.  What changes is which of two
     * bit-identical descriptions of the same buffer it gets.
     *
     * A guest head flips AR24; the compositor advertises the modifier for XR24
     * only, because alpha goes through its blend path and block-linear is not
     * always wired up there.  The bytes are the same either way and a scanout
     * has no meaningful alpha, so describing it as opaque is both correct and
     * cheaper for the compositor -- it may skip blending altogether.
     */
    uint32_t twin = nb_fourcc_opaque_twin(fourcc);

    if (twin && nb_format_usable(s, twin, modifier)) {
        *use_fourcc = twin;
        return true;
    }
    return false;
}

/*
 * HARDENING 1, 2, 3: validate the descriptor against the REAL buffer.
 *
 * A-18 in docs/internal/audit-boundaries-2026-08-20.md is this exact bug found
 * one process further in: PRESENT geometry reached QEMU's allocator unchecked
 * because the handler validated *which handle* may be exported and let the
 * geometry ride alongside as opaque metadata.  Here the consumer is the
 * COMPOSITOR, which runs as the user and is outside the sandbox — so an
 * under-sized buffer described as a large one is an out-of-bounds read in an
 * unsandboxed process.
 *
 * REJECT, NEVER CLAMP.  A frame whose geometry does not describe its buffer is
 * not a frame that can be fixed up, and quietly showing a different rectangle
 * than the client asked for is its own bug.
 *
 * Returns 0 and fills `d`, or -EINVAL after logging exactly which rule failed.
 */
static int nb_validate_desc(struct nb_sink *s, const struct nvkvm_broker_cmd *c,
                            int fd, struct nb_buf_desc *d)
{
    struct nb_session *ss = s->sess;
    struct stat st;
    struct nb_layout layout;
    const char *why;
    off_t size;
    uint32_t bpp;
    uint32_t fourcc;
    uint64_t need;
    char fcc[8], fcc2[8];

    /*
     * The VMM DECLARES shared memory; it is not inferred from the fd.  A memfd
     * is a legitimate carrier for other things (the test client sends one every
     * frame), so sniffing st.f_type would misread honest clients as asking for
     * a tier they never requested.
     *
     * The declaration is then ENFORCED, and it decides WHICH identity the fd
     * has to prove.  AUDIT 2026-08-27 S-1: this used to be the other way round
     * -- one nb_fd_is_dmabuf(accept_memfd) gate for both tiers -- and because
     * the real backends had to set accept_memfd to make the shm tier work at
     * all, "the fd is proved to be a dma-buf" (design §3 rule 4) silently
     * stopped holding for ORDINARY frames too.  A memfd then reached
     * zwp_linux_buffer_params_v1_add() as if it were a dma-buf.  Two questions,
     * two gates, and neither one answers the other.
     */
    d->is_shm = (c->flags & NVKVM_BROKER_CMD_F_SHM) != 0;
    if (d->is_shm) {
        int seals;

        if (!ss->accept_shm) {
            if (nb_reject_log(s)) {
                nb_err("ATTACH: F_SHM was set but backend '%s' does not present "
                       "shared memory in this mode", ss->ops->name);
            }
            return -EINVAL;
        }
        /* Shmem, proved without asking the fd's filesystem: HARDENING 4. */
        seals = nb_fd_shmem_seals(fd);
        if (seals < 0) {
            if (nb_reject_log(s)) {
                nb_err("ATTACH: F_SHM was set but the fd is not a memfd");
            }
            return -EINVAL;
        }
        /*
         * F_SEAL_SHRINK, OR THE SIZE WE MEASURE IS A SIZE THAT CAN CHANGE.
         *
         * AUDIT 2026-08-27 S-2.  The extent below comes from lseek(SEEK_END)
         * at this instant; the fd stays the client's and it can ftruncate()
         * the backing store smaller a microsecond later.  What then touches a
         * page past the new end is not the client: it is the BROKER
         * (nb_session_x11.c mmaps and PutImages it) or the user's COMPOSITOR
         * (nb_session_wl.c hands it over as a wl_shm_pool), and a MAP_SHARED
         * tmpfs page past EOF is SIGBUS.  Neither has a handler.  The seal is
         * what makes the measurement mean something for longer than a line of
         * code; it is exactly why udmabuf demands it of the same memfd one
         * process out (src/qemu/nvkvm_udmabuf.c), so an honest VMM already
         * carries it.
         *
         * An fd to a plain tmpfs file (/dev/shm/...) is refused HERE, by the
         * seal, and not -- as this comment used to say -- by F_GET_SEALS
         * failing.  It does not fail: MEASURED, it answers 0x1, F_SEAL_SEAL,
         * which mm/shmem.c puts on every inode memfd_create(MFD_ALLOW_SEALING)
         * did not create.  F_SEAL_SEAL forbids adding F_SEAL_SHRINK (F_ADD_SEALS
         * answers EPERM), so such a file can never carry the seal required
         * here.  test/test_cursor.py sends one.
         */
        if (!(seals & F_SEAL_SHRINK)) {
            if (nb_reject_log(s)) {
                nb_err("ATTACH: an F_SHM buffer must be a memfd sealed with "
                       "F_SEAL_SHRINK; without it the size measured here can be "
                       "taken away under the reader (SIGBUS)");
            }
            return -EINVAL;
        }
    } else {
        int r = nb_fd_is_dmabuf(fd, ss->accept_memfd);

        if (r <= 0) {
            if (nb_reject_log(s)) {
                if (r < 0) {
                    nb_err("ATTACH: cannot read /proc/self/fdinfo (%s), so "
                           "the fd cannot be proved to be a dma-buf without "
                           "asking its filesystem -- which a hostile sender "
                           "can make block; refused", strerror(-r));
                } else {
                    nb_err("ATTACH: the fd is not a dma-buf");
                }
            }
            return -EINVAL;
        }
    }
    /* HARDENING 2: same bound as NVKVM_PRESENT_MAX_DIM in the VMM, restated
     * here because the VMM is the attacker in this model. */
    if (c->width == 0 || c->height == 0 ||
        c->width > NVKVM_BROKER_MAX_DIM || c->height > NVKVM_BROKER_MAX_DIM) {
        if (nb_reject_log(s)) {
            nb_err("ATTACH: %ux%u is out of range (1..%u)",
                   c->width, c->height, NVKVM_BROKER_MAX_DIM);
        }
        return -EINVAL;
    }
    /* HARDENING 3: not a hardcoded list — what the GPU advertises.  Shared
     * with CMD_QUERY_FORMAT so the promise and the frame cannot disagree.
     * Not applicable to shm, which carries no modifier. */
    /*
     * For shm the MODIFIER is not a question -- shared memory is linear by
     * definition and carries none -- but the FOURCC still is: the broker has to
     * map it to a WL_SHM_FORMAT, and a format nothing advertises is one it
     * cannot present.  So ask the same resolver the same way, about LINEAR.
     * Skipping the check entirely (which the first version of this did) also
     * silently disabled the NV12 rejection and the opaque-twin substitution for
     * every shm client.
     */
    fourcc = c->fourcc;
    /*
     * SHM IS A SEPARATE ADVERTISEMENT CHANNEL.  Wayland announces shm formats
     * with wl_shm.format events, not through zwp_linux_dmabuf_v1, so validating
     * a memfd against the dma-buf table is the wrong question -- and under
     * --present-mode=shm that table is deliberately empty, which would reject
     * every frame.
     *
     * What must be true is that we can name the format to wl_shm at all.  Only
     * the two 32-bit layouts the guest head flips are mapped, and requiring
     * that keeps the NV12-style rejection that the dma-buf path gets from its
     * own gate.  No modifier is involved: shm is linear by definition.
     */
    if (d->is_shm) {
        if (fourcc != NB_FOURCC_XR24 && fourcc != NB_FOURCC_AR24) {
            if (nb_reject_log(s)) {
                nb_err("ATTACH: fourcc %s cannot be presented as shared memory; "
                       "only XR24 and AR24 are mapped to wl_shm formats",
                       nb_fourcc_name(fourcc, fcc));
            }
            return -EINVAL;
        }
    } else if (!nb_format_resolve(s, c->fourcc, c->modifier, &fourcc)) {
        /*
         * AND SAY SO ON THE WIRE, for the pair the client SENT -- whatever
         * the reason, and before the log budget can return early.  A frame
         * dropped here is otherwise dropped in silence, and a relay that
         * holds x=1 for the pair (told so earlier, or never asked) keeps
         * sending it into a black window.  The backend's own x=0 does not
         * always cover it: it names the pair the backend IMPORTED, which for
         * an alpha format resolved through its opaque twin is not the pair
         * the client named -- on Wayland a refused probe of XR24 used to
         * leave a relay sending AR24 holding its yes forever.  Once per pair
         * per connection (fmt_told), so a refused stream costs one packet.
         */
        nb_format_tell_no(s, c->fourcc, c->modifier);
        if (!nb_reject_log(s)) {
            return -EINVAL;
        }
        if (nb_modifier_layout(c->modifier, &layout, &why) != 0) {
            /* Whatever the display says about it: name the field, because
             * "not advertised" would send the reader to the compositor. */
            nb_err("ATTACH: fourcc %s modifier 0x%016llx (%s): %s -- the "
                   "broker cannot bound what a display would read from this "
                   "buffer, so no frame in it is presented (the VMM was sent "
                   "EV_FORMAT x=0 for it)",
                   nb_fourcc_name(c->fourcc, fcc),
                   (unsigned long long)c->modifier,
                   nb_modifier_vendor(c->modifier), why);
            return -EINVAL;
        }
        if (nb_refused_has(&s->fmt_refused, c->fourcc, c->modifier)) {
            /* Advertised, and then refused on this connection: "not
             * advertised" would send the reader looking for the wrong fault. */
            nb_err("ATTACH: fourcc %s modifier 0x%016llx was refused by this "
                   "display earlier on this connection (the VMM was sent "
                   "EV_FORMAT x=0 for it); rejected without asking again",
                   nb_fourcc_name(c->fourcc, fcc),
                   (unsigned long long)c->modifier);
            return -EINVAL;
        }
        /*
         * Name the VENDOR.  A rejection that lists only the pair reads as
         * "one entry is missing"; when the vendor differs from everything
         * the display offers, NO pair can ever match and the answer is a
         * copy, not a different format.  Saying so here is the difference
         * between a one-line diagnosis and an evening of bisecting.
         */
        nb_err("ATTACH: fourcc %s modifier 0x%016llx (%s) is not "
               "advertised by this display.  If the compositor runs on a "
               "different GPU than the guest's, no modifier will ever "
               "match and a readback/copy path is required — the VMM can "
               "ask first with CMD_QUERY_FORMAT; run the broker with "
               "--verbose to see what it does advertise.",
               nb_fourcc_name(c->fourcc, fcc),
               (unsigned long long)c->modifier,
               nb_modifier_vendor(c->modifier));
        return -EINVAL;
    }
    if (fourcc != c->fourcc) {
        static bool told;

        if (!told) {
            told = true;
            nb_log("ATTACH: %s modifier 0x%016llx is not advertised, but its "
                   "opaque twin %s is — presenting as %s (same bytes; a "
                   "scanout has no alpha).  Logged once.",
                   nb_fourcc_name(c->fourcc, fcc),
                   (unsigned long long)c->modifier,
                   nb_fourcc_name(fourcc, fcc2), nb_fourcc_name(fourcc, fcc2));
        }
    }
    bpp = nb_fourcc_bpp(fourcc);
    if (bpp == 0) {
        if (nb_reject_log(s)) {
            nb_err("ATTACH: fourcc %s has no known bytes-per-pixel, so its extent "
                   "cannot be bounded", nb_fourcc_name(fourcc, fcc));
        }
        return -EINVAL;
    }
    /* A row must hold the pixels it claims to. */
    if ((uint64_t)c->stride < (uint64_t)c->width * bpp) {
        if (nb_reject_log(s)) {
            nb_err("ATTACH: stride %u < width %u * bpp %u",
                   c->stride, c->width, bpp);
        }
        return -EINVAL;
    }
    /*
     * AND IT MUST NOT BE ARBITRARILY WIDER THAN THEM EITHER.
     *
     * Only the lower bound existed, so stride was bounded from above solely by
     * the `need <= INT32_MAX` test further down -- which a 1-PIXEL-WIDE buffer
     * satisfies with a 256 KiB stride over 8192 rows.  The extent is then 2 GiB
     * of wl_shm_pool handed to the user's unsandboxed compositor for an image
     * containing 8192 pixels.  Nothing over-reads (the fd really is that big,
     * and the pool size is checked), but a compositor that copies the pool
     * rather than the image commits 2 GiB of host RAM on the VMM's say-so, and
     * a legitimate buffer at this backend's maximum 8192x8192 needs only
     * 268 MiB.
     *
     * The bound is relative to THIS buffer's own row rather than a global
     * constant, so it scales with what is actually being displayed.  Twice the
     * pixel row plus a page is far past any real padding: a tiled pitch is the
     * width rounded up to a tile (at most 128 pixels, so +512 bytes at 32bpp),
     * and hardware pitch alignment is 256 or 512 bytes.  Reject rather than
     * clamp, like every other bound here -- the message carries all four
     * numbers, so a false rejection is one log line to diagnose.
     */
    if ((uint64_t)c->stride > (uint64_t)c->width * bpp * 2 + 4096) {
        if (nb_reject_log(s)) {
            nb_err("ATTACH: stride %u is more than double the %llu-byte pixel "
                   "row of a %u-wide %u-bpp buffer plus a page of padding; "
                   "REJECTED (an over-wide row buys the client a huge shared "
                   "mapping for a tiny image)",
                   c->stride, (unsigned long long)((uint64_t)c->width * bpp),
                   c->width, bpp);
        }
        return -EINVAL;
    }
    /* HARDENING 1: measure the fd, do not believe the sender.  The in-tree
     * pattern is nvkvm_isolate_handlers.c:1332. */
    size = lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        if (nb_reject_log(s)) {
            nb_err("ATTACH: lseek(SEEK_END) on the dma-buf gave %lld — refusing a "
                   "buffer whose extent cannot be measured", (long long)size);
        }
        return -EINVAL;
    }
    /*
     * THE EXTENT, PER MODIFIER -- nb_frame_extent() in nb_extent.c, the one
     * function every frame's bound comes from.  For a linear frame it is
     * offset + stride*height, as it always was.  For NVIDIA block-linear the
     * height is rounded up to whole blocks and the stride must be whole
     * 64-byte GOBs: until 2026-10-04 this line applied the linear formula to
     * every modifier, and a box run (kayfabe, vast 54032077, NVIDIA DDX
     * 580.159.04) had the X server import and present five block-linear
     * descriptions that fitted it -- three to the byte -- and ended up to
     * 64 KiB past their buffers, and a sixth whose pitch was not whole GOBs.
     * The X server checks none of it.
     *
     * shm is linear by definition: its modifier field is not read, the same
     * as the format gate above, so an F_SHM frame is bounded as LINEAR.
     *
     * Then two bounds, in this order, inside the same call: the extent must
     * fit an int32_t (AUDIT 2026-08-27 S-3: wl_shm_create_pool takes one, and
     * lseek on a sparse 4 GiB memfd costs the client nothing -- a stride of
     * 0x40000 over 8192 rows once made the pool size land on INT32_MIN, a
     * protocol error fatal to the connection at a moment the client picks),
     * and it must fit the buffer.  Bounding stride and offset with them: each
     * is <= the extent when height >= 1.
     */
    switch (nb_frame_extent(d->is_shm ? 0 /* DRM_FORMAT_MOD_LINEAR */
                                      : c->modifier,
                            c->height, c->stride, c->offset, (uint64_t)size,
                            &need, &why)) {
    case NB_EXTENT_OK:
        break;
    case NB_EXTENT_LAYOUT:
        /* Unreachable through the format gate, which asks the same decoder;
         * kept so this call stands on its own. */
        if (nb_reject_log(s)) {
            nb_err("ATTACH: modifier 0x%016llx (%s): %s — REJECTED",
                   (unsigned long long)c->modifier,
                   nb_modifier_vendor(c->modifier), why);
        }
        return -EINVAL;
    case NB_EXTENT_PITCH:
        if (nb_reject_log(s)) {
            nb_err("ATTACH: %ux%u stride=%u modifier 0x%016llx: %s (%u is "
                   "not a multiple of 64) — REJECTED", c->width, c->height,
                   c->stride, (unsigned long long)c->modifier, why,
                   c->stride);
        }
        return -EINVAL;
    case NB_EXTENT_TOO_BIG:
        if (nb_reject_log(s)) {
            nb_err("ATTACH: %ux%u stride=%u offset=%u spans %llu bytes; the "
                   "largest buffer any display protocol here can be handed is "
                   "%d — REJECTED", c->width, c->height, c->stride, c->offset,
                   (unsigned long long)need, INT32_MAX);
        }
        return -EINVAL;
    default:    /* NB_EXTENT_SHORT */
        if (nb_reject_log(s)) {
            char blocks[160] = "";

            if (!d->is_shm &&
                nb_modifier_layout(c->modifier, &layout, &why) == 0 &&
                layout.block_linear) {
                snprintf(blocks, sizeof blocks,
                         "; block-linear modifier 0x%016llx stores rows in "
                         "whole %u-row blocks, so its %u rows occupy %llu",
                         (unsigned long long)c->modifier, layout.block_rows,
                         c->height,
                         (unsigned long long)((need - c->offset) / c->stride));
            }
            nb_err("ATTACH: %ux%u stride=%u offset=%u needs %llu bytes but the "
                   "dma-buf is %lld — REJECTED (the compositor would read out of "
                   "bounds)%s", c->width, c->height, c->stride, c->offset,
                   (unsigned long long)need, (long long)size, blocks);
        }
        return -EINVAL;
    }
    if (fstat(fd, &st) < 0) {
        nb_err("ATTACH: fstat: %s", strerror(errno));
        return -EINVAL;
    }

    d->fd       = fd;
    d->id       = (uint64_t)st.st_ino;
    d->size     = (uint64_t)size;
    d->width    = c->width;
    d->height   = c->height;
    d->stride   = c->stride;
    d->offset   = c->offset;
    d->fourcc   = fourcc;   /* possibly the opaque twin, see above */
    d->modifier = c->modifier;
    d->bpp      = bpp;
    d->seq      = c->seq;
    return 0;
}

/* ── CMD_CURSOR: the fd half ─────────────────────────────────────────────── */

/*
 * How often a CHANGED cursor reaches the backend: 125 Hz.  An animated guest
 * pointer (a busy spinner) changes at 10-60 Hz, so an honest client never sees
 * this; what it bounds is the display-server traffic a client can cause, at
 * <= 125 uploads of <= NB_CURSOR_SCALED_MAX^2 pixels a second whatever it
 * sends.  Latest wins, and the latest is always applied -- see nb_sink_tick().
 */
#define NB_CURSOR_MIN_MS 8u

int nb_sink_tick(struct nb_sink *s)
{
    uint64_t now, since;

    if (!s->cursor_dirty) {
        return -1;
    }
    now = nb_now_ms();
    since = now - s->cursor_applied_ms;
    if (s->cursor_applied_ms != 0 && since < NB_CURSOR_MIN_MS) {
        return (int)(NB_CURSOR_MIN_MS - since);
    }
    s->cursor_dirty = false;
    s->cursor_applied_ms = now;
    /*
     * PUBLISH.  This copy is the only way a SET reaches what the backend can
     * see -- the backend holds a pointer to cursor_pub and re-renders from it
     * whenever it likes, so the interval above bounds THAT too, not merely
     * how often ->cursor() is called.
     */
    nb_cursor_copy(&s->cursor_pub, &s->cursor);
    if (s->sess->ops->cursor) {
        s->sess->ops->cursor(s->sess, &s->cursor_pub);
    }
    return -1;
}

static void nb_cursor_changed(struct nb_sink *s)
{
    s->cursor_dirty = true;
    /* At once if the interval allows -- the first change after a quiet spell
     * is the common case and should cost no latency.  Otherwise the main
     * loop's poll timeout brings us back exactly when it does. */
    (void)nb_sink_tick(s);
}

/*
 * Copy a SET's rows out of the client's fd.  Every field has already passed
 * nb_cursor_check(); this proves the FD, measures it, and reads it.
 *
 * pread, NEVER mmap.  The fd stays the client's, and it can ftruncate() a
 * memfd it did not seal the instant after we measure it.  A mapping read past
 * the new end is SIGBUS in the privileged process -- the shm tier needs
 * F_SEAL_SHRINK precisely because it maps (see nb_validate_desc()).  A read
 * past the new end is a SHORT READ, which is just a refused cursor.  So no
 * seal is demanded here: copying makes the measurement not matter for safety,
 * only for whether the image is complete.
 *
 * And it is the BROKER's copy the compositor sees, never the client's fd: the
 * image lands in s->cursor, is published to s->cursor_pub when the pacing
 * allows, and is scaled and re-uploaded by the backend from that, so nothing
 * the client does to its memfd afterwards reaches the display server.
 *
 * Returns 0, or -EINVAL after logging (throttled) which rule failed.  On
 * failure s->cursor is untouched: a refused SET leaves the previous cursor
 * up, as a refused ATTACH leaves the previous frame.
 */
static int nb_cursor_read(struct nb_sink *s,
                          const struct nvkvm_broker_cursor_cmd *c, int fd)
{
    /* static: 256 KiB does not belong on the stack, and the broker is
     * single-threaded.  Sized by the protocol's own bounds, which is what
     * nb_cursor_check() enforced on the record before we got here. */
    static uint8_t staging[NVKVM_BROKER_CURSOR_MAX_STRIDE *
                           NVKVM_BROKER_CURSOR_MAX_DIM];
    struct stat st;
    uint32_t span;
    size_t got = 0;

    memset(&st, 0, sizeof st);  /* logged below even when fstat fails */
    /*
     * SHMEM ONLY.  The read below happens on the thread that holds the
     * keyboard grab, so the fd must be one whose read cannot block on someone
     * else: a FUSE file, a FIFO, a socket, a tty or a device all could, and a
     * client choosing the fd type would be choosing when the broker stalls.
     * tmpfs pages are memory; a read of them returns.
     *
     * AND THE PROOF ITSELF MUST NOT BLOCK.  This used to call fstatfs()
     * first, which on a FUSE file is a request to the daemon behind it --
     * the very stall the rule exists to prevent, reached one syscall early.
     * nb_fd_shmem_seals() asks F_GET_SEALS first, answered from the file's
     * mapping without calling into any filesystem, and fstatfs()es only what
     * that proved is shmem (HARDENING 4 has the kernel-source detail).  Its
     * success is the shmem proof, not a seal demand: no particular seal is
     * required, because the rows are copied, never mapped.
     */
    if (nb_fd_shmem_seals(fd) < 0) {
        if (nb_reject_log(s)) {
            nb_err("CURSOR SET: the fd is not a memfd; refused (the previous "
                   "cursor stays)");
        }
        return -EINVAL;
    }
    span = nb_cursor_span(c->width, c->height, c->stride);
    /*
     * nb_cursor_check() has already bounded every term of the span to the
     * protocol's maxima, which is exactly what `staging` is sized from.  The
     * copy below writes `span` bytes into it, so that bound is restated HERE,
     * where the write is: a later change to the check must not be able to turn
     * this into an overflow of a static buffer in the privileged process.
     */
    if (span == 0 || span > sizeof staging) {
        nb_err("CURSOR SET: span %u outside 1..%zu -- the record check and "
               "this buffer disagree; refused", span, sizeof staging);
        return -EINVAL;
    }
    /*
     * fstat, not lseek(SEEK_END): the fd shares its open file description
     * with the client, and moving the client's file offset is a side effect
     * of validation nobody asked for.  A memfd reports its size here; a
     * dma-buf would not, which is why ATTACH measures differently.
     */
    if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
        !nb_cursor_fits(c->offset, span, (uint64_t)st.st_size)) {
        if (nb_reject_log(s)) {
            nb_err("CURSOR SET: %ux%u stride=%u offset=%u needs %llu bytes but "
                   "the memfd is %lld; refused (the previous cursor stays)",
                   c->width, c->height, c->stride, c->offset,
                   (unsigned long long)c->offset + span,
                   (long long)st.st_size);
        }
        return -EINVAL;
    }
    while (got < span) {
        ssize_t n = pread(fd, staging + got, span - got,
                          (off_t)c->offset + (off_t)got);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got != span) {
        if (nb_reject_log(s)) {
            nb_err("CURSOR SET: read %zu of %u bytes -- the memfd shrank or "
                   "failed under the read; refused (the previous cursor "
                   "stays)", got, span);
        }
        return -EINVAL;
    }
    nb_cursor_load(&s->cursor, staging, c->width, c->height, c->stride,
                   c->hot_x, c->hot_y);
    /* What it cost, so a flood of SETs yields the loop like a flood of
     * frames does (see NB_WORK_BUDGET_BYTES). */
    nb_sink_charge_work(s, (uint64_t)span +
                           (uint64_t)c->width * c->height * 4u);
    return 0;
}

static void nb_cmd_cursor(struct nb_sink *s, const struct nvkvm_broker_cmd *raw,
                          int fd)
{
    struct nb_session *ss = s->sess;
    struct nvkvm_broker_cursor_cmd c;
    const char *why = "";
    int verdict;

    /* A COPY, NOT A CAST -- the same strict-aliasing reason as CLIPBOARD.
     * Both types are NVKVM_BROKER_CMD_SIZE, asserted in the protocol header. */
    memcpy(&c, raw, sizeof c);

    /*
     * A broker that did not ADVERTISE the type treats it exactly as one that
     * does not KNOW it: as an unknown command.  The client was told in HELLO;
     * a client that sends it anyway is not reading HELLO, and the rule for
     * that is the same everywhere.
     */
    if (!ss->ops->cursor || !(ss->caps & NVKVM_BROKER_CAP_CURSOR)) {
        if (fd >= 0) {
            nb_fd_drop(fd);
        }
        nb_violation(s, "CURSOR on a session that did not advertise "
                        "CAP_CURSOR");
        return;
    }
    verdict = nb_cursor_check(&c, fd >= 0, &why);
    if (verdict == NB_CURSOR_VIOLATION) {
        if (fd >= 0) {
            nb_fd_drop(fd);
        }
        nb_violation(s, why);
        return;
    }
    s->n_cursor++;
    if (verdict == NB_CURSOR_REJECT) {
        if (fd >= 0) {
            nb_fd_drop(fd);
        }
        s->n_cursor_reject++;
        if (nb_reject_log(s)) {
            nb_err("%s (%ux%u stride=%u offset=%u hot %u,%u); refused (the "
                   "previous cursor stays)", why, c.width, c.height,
                   c.stride, c.offset, c.hot_x, c.hot_y);
        }
        return;
    }
    switch (c.op) {
    case NVKVM_BROKER_CURSOR_SET: {
        int r = nb_cursor_read(s, &c, fd);

        /* Copied or refused; either way we are done with it.  Copied means
         * proved shmem, whose close is the kernel's own; refused may be
         * anything, so it goes the way every unproven fd goes. */
        if (r == 0) {
            close(fd);
        } else {
            nb_fd_drop(fd);
        }
        if (r != 0) {
            s->n_cursor_reject++;
            return;
        }
        break;
    }
    case NVKVM_BROKER_CURSOR_HIDE:
        s->cursor.visible = false;
        break;
    case NVKVM_BROKER_CURSOR_SHOW:
        s->cursor.visible = true;
        break;
    default:
        /* nb_cursor_check() returned a violation for anything else. */
        return;
    }
    nb_cursor_changed(s);
}

void nb_sink_device_changed(struct nb_sink *s)
{
    struct nb_session *ss = s->sess;

    nb_log("the display reports a different device: %s%u:%u",
           (ss->dev_flags & NVKVM_BROKER_DEVICE_F_KNOWN)
               ? ((ss->dev_flags & NVKVM_BROKER_DEVICE_F_RENDER)
                      ? "render node " : "node ")
               : "unknown ",
           ss->dev_major, ss->dev_minor);
    nb_emit(s, NVKVM_BROKER_EV_DEVICE, (int)ss->dev_flags, 0,
            ss->dev_major, ss->dev_minor);
}

/*
 * Act on one fully received command.  `fd` is -1 unless the client attached
 * one; this function always takes ownership of it.
 *
 * Every failure path here is either "reject this frame" or "drop the client".
 * There is no partial recovery: a client that got the protocol wrong once has
 * no claim on the privileged side's patience, and continuing after a malformed
 * message is how a resync bug becomes a parser bug.
 */
static void nb_handle_cmd(struct nb_sink *s, const struct nvkvm_broker_cmd *c,
                          int fd)
{
    struct nb_session *ss = s->sess;
    struct nb_buf_desc d;
    int r;

    /*
     * FLAGS (was reserved0) ONLY, and that is not a shortcut.
     *
     * This check used to cover reserved1 as well, which was correct while
     * every command shared one layout.  CLIPBOARD does not: its payload runs
     * to the end of the record, so the generic `reserved1` at offset 36
     * ALIASES clipboard data bytes 23..26 -- and any chunk whose last four
     * payload bytes were non-zero was rejected as a malformed header.  Found
     * by the hostile-client harness, which sent 27 bytes of 'A'.
     *
     * flags is at the same offset and means the same thing in BOTH
     * layouts, so it stays here.  reserved1 belongs to the layouts that
     * actually have it, and is checked in their own cases -- the alternative
     * is a control keyed on a layout that is no longer universal, which is how
     * A-1 and R-1 happened.
     */
    if ((c->flags & ~(uint16_t)NVKVM_BROKER_CMD_F_ALL) != 0) {
        if (fd >= 0) {
            nb_fd_drop(fd);
        }
        nb_violation(s, "unknown bits set in the command flags");
        return;
    }

    switch (c->type) {
    case NVKVM_BROKER_CMD_ATTACH:
        if (c->reserved1 != 0) {
            if (fd >= 0) {
                nb_fd_drop(fd);
            }
            nb_violation(s, "ATTACH reserved1 is not zero");
            return;
        }
        if (fd < 0) {
            nb_violation(s, "ATTACH without an SCM_RIGHTS fd");
            return;
        }
        s->n_attach++;
        if (!(ss->caps & NVKVM_BROKER_CAP_DMABUF)) {
            nb_fd_drop(fd);
            s->n_reject++;
            nb_err("ATTACH: this session cannot accept dma-buf buffers");
            return;
        }
        if (nb_validate_desc(s, c, fd, &d) != 0) {
            nb_fd_drop(fd);
            s->n_reject++;
            /*
             * A rejected buffer is NOT a disconnect.  The descriptor comes
             * from the guest by way of the VMM, so a guest that flips
             * nonsense would otherwise be able to kill the display of a
             * VMM that is behaving correctly.  The frame is dropped; the
             * connection survives; the reason is on stderr.
             */
            return;
        }
        r = ss->ops->attach(ss, &d);
        close(fd);          /* the backend imported it or it failed; either
                             * way our copy is done — HARDENING 5, fd intake
                             * is bounded at one in flight by construction.
                             * A plain close: nb_validate_desc() proved it a
                             * dma-buf or shmem, neither of which has a
                             * ->flush to wait on (see nb_fd_drop()). */
        if (r != 0) {
            s->n_reject++;
            if (nb_reject_log(s)) {
                nb_err("ATTACH: the display refused the buffer: %s",
                       strerror(-r));
            }
        }
        return;

    case NVKVM_BROKER_CMD_QUERY_FORMAT: {
        /*
         * "Can you display this?"  Answered from the SAME resolver ATTACH
         * uses, so a yes here is binding.
         *
         * The VMM needs this because it cannot see the advertised set and a
         * rejected ATTACH is not reported back to it -- the frame is dropped
         * and the reason goes to our stderr.  Without the question, a
         * cross-vendor host is a black window with an explanation the VMM
         * never hears.
         *
         * No fd, no state, no side effects: a query cannot change what the
         * next frame does.  reserved1 is checked because this uses the
         * ordinary command layout, which has one.
         */
        uint32_t use = 0;
        bool ok;

        if (c->reserved1 != 0) {
            if (fd >= 0) {
                nb_fd_drop(fd);
            }
            nb_violation(s, "QUERY_FORMAT reserved1 is not zero");
            return;
        }
        if (fd >= 0) {
            nb_fd_drop(fd);          /* a query carries no buffer; drop any fd */
        }
        ok = nb_format_resolve(s, c->fourcc, c->modifier, &use);
        /* Asked once per mode change by an honest client -- and once per
         * command by a hostile one, so it shares the reject budget. */
        if (nb_reject_log(s)) {
            char fcc[8];
            struct nb_layout layout;
            const char *why = "";
            /* A "no" for a layout the broker cannot bound says so: it is the
             * one "no" no change on the display's side can turn into a yes. */
            bool unbounded =
                nb_modifier_layout(c->modifier, &layout, &why) != 0;

            nb_log("QUERY_FORMAT %s modifier 0x%016llx (%s) -> %s%s%s%s",
                   nb_fourcc_name(c->fourcc, fcc),
                   (unsigned long long)c->modifier,
                   nb_modifier_vendor(c->modifier),
                   ok ? "YES" : "NO",
                   (ok && use != c->fourcc) ? " (as its opaque twin)" : "",
                   unbounded ? ": " : "", unbounded ? why : "");
        }
        nb_emit(s, NVKVM_BROKER_EV_FORMAT, ok ? 1 : 0, (int)c->fourcc,
                (uint32_t)(c->modifier & 0xffffffffu),
                (uint32_t)(c->modifier >> 32));
        return;
    }

    case NVKVM_BROKER_CMD_COMMIT:
        if (c->reserved1 != 0) {
            if (fd >= 0) {
                nb_fd_drop(fd);
            }
            nb_violation(s, "COMMIT reserved1 is not zero");
            return;
        }
        if (fd >= 0) {
            nb_fd_drop(fd);
            nb_violation(s, "COMMIT carried an fd");
            return;
        }
        if (c->width || c->height || c->stride || c->offset || c->fourcc ||
            c->modifier) {
            nb_violation(s, "COMMIT carried descriptor fields");
            return;
        }
        s->n_commit++;
        r = ss->ops->commit(ss, s);
        if (r != 0 && r != -ENOENT) {
            nb_err("COMMIT failed: %s", strerror(-r));
        }
        return;

    case NVKVM_BROKER_CMD_CLIPBOARD: {
        /*
         * A COPY, NOT A CAST.  The record was read into a `struct
         * nvkvm_broker_cmd`, and reading that object back through a
         * `clip_cmd *` is a strict-aliasing violation however identical the two
         * layouts are — the compiler is entitled to assume the two types never
         * name the same storage and to reorder the loads accordingly.  Both are
         * exactly NVKVM_BROKER_CMD_SIZE (asserted in the protocol header), so
         * the memcpy is the same bytes with the aliasing question removed, and
         * at -O2 it is a register shuffle.  Not a theoretical worry worth
         * carrying in the one parser that runs on the privileged side.
         */
        struct nvkvm_broker_clip_cmd ccbuf;
        const struct nvkvm_broker_clip_cmd *cc = &ccbuf;
        unsigned n;

        memcpy(&ccbuf, c, sizeof ccbuf);

        if (fd >= 0) {
            nb_fd_drop(fd);
            nb_violation(s, "CLIPBOARD carried an fd");
            return;
        }
        if (cc->reserved1 != 0) {
            nb_violation(s, "CLIPBOARD reserved field is not zero");
            return;
        }
        /*
         * THE DIRECTION PEOPLE UNDERESTIMATE.  The obvious risk is the guest
         * READING your clipboard; the sharper one is the guest WRITING it --
         * you copy something in the guest, paste it into a host terminal, and
         * what lands is not what you copied.  Hence: only in a mode that
         * permits it, only while focused, rate-limited, capped, and SAID OUT
         * LOUD when it happens.
         */
        /*
         * STRUCTURE BEFORE POLICY.  A chunk claiming more bytes than it can
         * hold is malformed however the broker is configured -- checking it
         * after the mode and focus gates meant an unfocused client's malformed
         * chunk was silently dropped instead of being flagged, so the same lie
         * was a violation or not depending on where the pointer happened to
         * be.
         */
        n = NVKVM_BROKER_CLIP_NBYTES(cc->info);
        if (n > NVKVM_BROKER_CLIP_CMD_BYTES) {
            nb_violation(s, "CLIPBOARD chunk claims more bytes than it has");
            return;
        }
        /*
         * TRANSACTION FRAMING.  QEMU's nonblocking sender can abandon a
         * transfer after a prefix.  Chunk zero therefore starts a fresh
         * transaction and discards any abandoned prefix; every continuation
         * must then be monotonic.  Without this, the next transfer's LAST
         * committed a concatenation of two unrelated clipboards.
         */
        if (cc->chunk == 0) {
            s->clip_in_len = 0;
            s->clip_in_chunks = 0;
            s->clip_in_next_chunk = 0;
            s->clip_in_bad = false;
            s->clip_in_active = true;
        }
        if (!s->clip_in_active || cc->chunk != s->clip_in_next_chunk) {
            nb_violation(s, "CLIPBOARD chunks are not a monotonic transaction");
            return;
        }
        s->clip_in_next_chunk++;
        s->clip_in_chunks++;

        /* Policy rejection still advances framing and still reaches LAST.
         * Returning early here used to poison the following transaction; the
         * size-cap branch could wedge it permanently. */
        if (s->cfg->clip_mode == NB_CLIP_OFF) {
            if (!s->clip_told_off) {
                s->clip_told_off = true;
                nb_log("the VM offered clipboard content and clipboard is "
                       "off, so it was discarded. --clipboard=guest-to-host "
                       "or =consent would accept it.");
            }
            s->clip_in_bad = true;  /* a mode, not a protocol violation */
        }
        /*
         * NOT FOCUSED IS NOT A REASON TO DISCARD.  It used to be, silently:
         * the text was dropped here, before the backend ever saw it, with no
         * log and no title-bar notice -- so a copy in the guest simply did
         * nothing and nothing said why.  Worse, the guest believes the copy
         * succeeded, so nothing re-sends it; the text only reappears if the
         * user copies something ELSE.
         *
         * The backend holds it instead and applies it on focus-in, which keeps
         * the actual property this gate is for -- a background VM never
         * silently replaces what you copied somewhere else -- without losing
         * the copy.  See wl_set_clipboard().
         */
        if (nb_clip_rate_exceeded(s)) {
            /* Say so.  A silent rate-limit is indistinguishable from a bug,
             * and this one discards a copy the user did make. */
            nb_log("clipboard from the VM is arriving too fast; this one was "
                   "dropped");
            s->clip_in_bad = true;
        }
        /* The cap is a count we keep and a checked sum, never a sender-supplied
         * allocation size. */
        if (s->clip_in_chunks > NVKVM_BROKER_CLIP_MAX_CHUNKS_CMD ||
            n > NVKVM_BROKER_CLIP_MAX_BYTES - s->clip_in_len) {
            if (!s->clip_in_bad) {
                nb_log("clipboard from the VM exceeds the %u-byte cap; "
                       "discarding it", NVKVM_BROKER_CLIP_MAX_BYTES);
            }
            s->clip_in_bad = true;
        }
        if (!s->clip_in_bad) {
            memcpy(s->clip_in + s->clip_in_len, cc->data, n);
            s->clip_in_len += n;
        }
        if (cc->info & NVKVM_BROKER_CLIP_LAST) {
            unsigned len = s->clip_in_len;
            bool bad = s->clip_in_bad;

            s->clip_in_len = 0;
            s->clip_in_chunks = 0;
            s->clip_in_next_chunk = 0;
            s->clip_in_active = false;
            s->clip_in_bad = false;
            if (bad || len == 0) {
                return;
            }
            s->clip_in[len] = '\0';
            /* UTF-8 only.  A decoder in the privileged process is exactly what
             * "text only" exists to avoid, so this validates rather than
             * transcodes, and rejects anything it cannot vouch for. */
            if (!nb_utf8_ok(s->clip_in, len)) {
                nb_log("clipboard from the VM is not valid UTF-8; discarding");
                return;
            }
            if (!(ss->clipboard_caps & NB_SESSION_CLIP_G2H) ||
                !ss->ops->set_clipboard) {
                nb_err("clipboard: backend '%s' lost guest-to-host support; "
                       "discarding instead of pretending the copy worked",
                       ss->ops->name);
                return;
            }
            r = ss->ops->set_clipboard(ss, s->clip_in, len);
            if (r == 0) {
                /* VISIBLE.  A guest silently replacing the host clipboard is
                 * the attack; saying so is the control. */
                nb_log("the VM put %u bytes on YOUR clipboard", len);
                if (ss->ops->notify_clipboard) {
                    ss->ops->notify_clipboard(ss);
                }
            } else if (r > 0) {
                /* HELD, not applied: the window is not focused, so nothing has
                 * been written to the user's clipboard yet.  No notice here --
                 * the backend raises it when it applies the text on focus-in,
                 * which is the first moment a title bar is something anyone
                 * can read. */
                nb_log("the VM copied %u bytes; holding them until the window "
                       "is focused", len);
            } else {
                nb_err("clipboard: backend '%s' rejected the VM's text: %s",
                       ss->ops->name, strerror(-r));
            }
        }
        return;
    }

    case NVKVM_BROKER_CMD_CAPS:
        if (c->reserved1 != 0) {
            if (fd >= 0) {
                nb_fd_drop(fd);
            }
            nb_violation(s, "CAPS reserved1 is not zero");
            return;
        }
        if (fd >= 0) {
            nb_fd_drop(fd);
            nb_violation(s, "CAPS carried an fd");
            return;
        }
        if (c->height || c->stride || c->offset || c->fourcc || c->modifier) {
            nb_violation(s, "CAPS carried other fields");
            return;
        }
        s->caps_seen = c->width;
        nb_log("the VM reports: clipboard agent %s",
               (c->width & NVKVM_BROKER_CLIENT_CLIPBOARD)
                   ? "present"
                   : "not seen yet (it is announced the first time the guest "
                     "copies something)");
        return;

    case NVKVM_BROKER_CMD_WINDOW:
        if (c->reserved1 != 0) {
            if (fd >= 0) {
                nb_fd_drop(fd);
            }
            nb_violation(s, "WINDOW reserved1 is not zero");
            return;
        }
        if (fd >= 0) {
            nb_fd_drop(fd);
            nb_violation(s, "WINDOW carried an fd");
            return;
        }
        if (c->stride || c->offset || c->fourcc || c->modifier) {
            nb_violation(s, "WINDOW carried buffer fields");
            return;
        }
        if (c->width == 0 || c->height == 0 ||
            c->width > NVKVM_BROKER_MAX_DIM ||
            c->height > NVKVM_BROKER_MAX_DIM) {
            s->n_reject++;
            if (nb_reject_log(s)) {
                nb_err("WINDOW: %ux%u out of range — ignored",
                       c->width, c->height);
            }
            return;
        }
        /*
         * A GUEST RE-MODE DOES NOT MOVE THE USER'S WINDOW.
         *
         * WINDOW is the one message where the guest legitimately drives
         * geometry, and it is honoured EXACTLY ONCE: the first one sizes the
         * window to whatever resolution the guest booted at, which is the only
         * sensible initial size.  After that the window belongs to the user,
         * and someone opening the guest's display settings changes the SOURCE
         * the broker is scaling, not the window they arranged on their desk.
         *
         * That is the mirror of the rule in the other direction -- a host
         * resize never re-modes the guest -- and without it the two rules
         * fight: the guest re-modes, the window jumps, the window change is
         * reported back, and the geometry oscillates.
         *
         * Nothing else is needed to adopt the new size: the next COMMIT
         * carries a buffer with the new dimensions and the backend rescales it
         * into the window it already has, per the active --scale mode.
         */
        if (!s->window_established) {
            s->window_established = true;
            ss->ops->resize(ss, c->width, c->height);
        } else {
            nb_log("the guest changed resolution to %ux%u: keeping the window "
                   "and rescaling into it", c->width, c->height);
        }
        return;

    case NVKVM_BROKER_CMD_CURSOR:
        /* Its own layout and its own reserved/flags rules: see
         * nb_cursor_check().  Takes ownership of `fd`. */
        nb_cmd_cursor(s, c, fd);
        return;

    default:
        if (fd >= 0) {
            nb_fd_drop(fd);
        }
        nb_violation(s, "unknown command type");
        return;
    }
}

/*
 * Drain the client socket.  MSG_DONTWAIT throughout: this runs on the same
 * thread that services input, so it must never block for any reason, least of
 * all one a hostile client chose.
 *
 * Framing: exactly NVKVM_BROKER_CMD_SIZE bytes per command, accumulated in a
 * buffer of exactly that size.  The read length is `remaining`, so a client
 * cannot cause a write past the end of `s->rx` even in principle.
 */
/*
 * How many commands one readable() call will process before returning to the
 * event loop.  Far more than a well-behaved VMM sends per frame (an ATTACH, a
 * COMMIT, occasionally a WINDOW), and small enough that a hostile one cannot
 * hold the loop.  See B-1 in the audit note above nb_sink_readable().
 */
#define NB_RX_BUDGET 64

/*
 * AND HOW MUCH WORK, not just how many commands (audit 2026-08-29 S-1, S-2).
 *
 * NB_RX_BUDGET is a bound only if commands cost roughly the same.  On the X11
 * shm tier one COMMIT copies the whole frame — `stride != width*4` adds a
 * second full copy on top of the PutImage — so 64 of them at the 8192x8192
 * maximum is gigabytes of memcpy between two dispatches of the display queue.
 *
 * 64 MB is a shade under two 4K frames, so no honest client ever meets it: a
 * VMM sends one COMMIT per frame and waits for the pacing reply.  The charge is
 * taken AFTER the copy, deliberately — the first command of a wakeup always
 * runs however big it is, so a legitimate 8K frame still displays, and what is
 * bounded is how many more follow it before the loop lets poll() run again.
 *
 * The round-trip allowance is separate because its cost is not bytes but
 * LATENCY: an xcb_request_check() takes as long as the X server takes, and
 * takes forever while any other client holds an XGrabServer.  Four per wakeup
 * is far more than the three or four imports an honest VM performs in its whole
 * life, and it is the ceiling on how many times one wakeup can be made to wait
 * on the display server.  (It does not bound a SINGLE trip; nothing on this
 * path can, short of the asynchronous import the Wayland backend uses.)
 */
#define NB_WORK_BUDGET_BYTES (64u * 1024u * 1024u)
#define NB_WORK_ROUNDTRIPS   4u

void nb_sink_charge_work(struct nb_sink *s, uint64_t bytes)
{
    s->work_bytes += bytes;
}

bool nb_sink_take_roundtrip(struct nb_sink *s)
{
    if (s->work_trips >= NB_WORK_ROUNDTRIPS) {
        return false;
    }
    s->work_trips++;
    return true;
}

/* True once this wakeup has done as much work as it is allowed to. */
static bool nb_work_spent(const struct nb_sink *s)
{
    return s->work_bytes >= NB_WORK_BUDGET_BYTES;
}

/*
 * Sustained commands per second above which a client is hung up on (B-1b).
 *
 * A VMM sends an ATTACH, a COMMIT and occasionally a WINDOW per frame -- about
 * 500/s at 144 Hz.  This is forty times that, so no honest client can reach it,
 * and it is two orders of magnitude below the 1.8 million/s a flooding client
 * actually achieved on this hardware.  The budget above keeps the event loop
 * alive under a flood; this stops the flood being free.
 */
#define NB_RX_MAX_PER_SEC 20000u

static uint64_t nb_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* True when the client has exceeded the sustained command rate. */
static bool nb_rate_exceeded(struct nb_sink *s)
{
    uint64_t now = nb_now_ms();

    if (now - s->rate_ms >= 1000u) {
        s->rate_ms = now;
        s->rate_count = 0;
    }
    return ++s->rate_count > NB_RX_MAX_PER_SEC;
}

/*
 * AUDIT B-1.  This loop used to run until the socket drained, which a client
 * controls: it can keep data available indefinitely, and measured on hardware a
 * flood of valid COMMITs pinned the broker at 100% of a core with `syscall`
 * showing "running" and never ppoll, for as long as the flood lasted.
 *
 * That is not merely a busy loop.  The SAME poll() services the display server,
 * so while it is starved NO key is dispatched -- which includes CTRL+ALT+G, and
 * includes the focus-loss auto-ungrab.  A client that starts flooding while the
 * grab is on therefore leaves the user's keyboard captured with no way out,
 * which is the single failure mode this whole design treats as unacceptable.
 * The client never gains the grab (see the audit: no client-reachable path
 * turns one on) -- it makes an existing one unreleasable, which is as bad.
 *
 * The budget fixes it because a return with data still pending is not a loss:
 * poll() reports POLLIN again immediately and the next call resumes.  Progress
 * is preserved; monopoly is not.
 */
void nb_sink_readable(struct nb_sink *s)
{
    unsigned budget = NB_RX_BUDGET;

    /* A wakeup's allowances are its own; nothing carries over. */
    s->work_bytes = 0;
    s->work_trips = 0;

    for (;;) {
        union {
            char buf[CMSG_SPACE(sizeof(int) * 4)];
            struct cmsghdr align;
        } u;
        struct iovec iov = {
            .iov_base = s->rx + s->rxlen,
            .iov_len  = NVKVM_BROKER_CMD_SIZE - s->rxlen,
        };
        struct msghdr msg = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = u.buf, .msg_controllen = sizeof(u.buf),
        };
        struct cmsghdr *cm;
        ssize_t n;

        if (s->client_fd < 0) {
            return;
        }
        do {
            n = recvmsg(s->client_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        } while (n < 0 && errno == EINTR);

        if (n == 0) {
            nb_sink_detach(s, "closed the connection");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            nb_sink_detach(s, "recv error");
            return;
        }

        /*
         * HARDENING 5: bound fd intake.  EXACTLY ONE fd may accompany a
         * command, and only ATTACH may carry it.  Anything else — several fds
         * on one message, a second fd while one is already banked from a
         * partial read, or more ancillary data than our control buffer holds
         * (MSG_CTRUNC) — is a violation.  Every fd seen is closed first: the
         * privileged process must not still be holding descriptors a hostile
         * client sent when it decides to hang up on it.
         */
        {
            /* An fd may already be banked from a PARTIAL read of this same
             * command; a second one for the same command is still a second
             * one, so that counts too. */
            bool had_fd = (s->rxfd >= 0);
            unsigned nfd_total = 0;

            for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                size_t payload, nfd, k;
                int got;

                if (cm->cmsg_level != SOL_SOCKET ||
                    cm->cmsg_type != SCM_RIGHTS) {
                    continue;
                }
                payload = cm->cmsg_len - CMSG_LEN(0);
                nfd = payload / sizeof(int);
                for (k = 0; k < nfd; k++) {
                    memcpy(&got, CMSG_DATA(cm) + k * sizeof(int), sizeof(int));
                    nfd_total++;
                    if (s->rxfd < 0) {
                        s->rxfd = got;
                    } else {
                        nb_fd_drop(got);
                    }
                }
            }
            if (nfd_total > 1 || (nfd_total == 1 && had_fd) ||
                (msg.msg_flags & MSG_CTRUNC)) {
                nb_violation(s, "more than one fd on a single command");
                return;
            }
        }

        s->rxlen += (size_t)n;
        if (s->rxlen < NVKVM_BROKER_CMD_SIZE) {
            continue;           /* partial command; the fd stays banked */
        }
        {
            struct nvkvm_broker_cmd c;
            int fd = s->rxfd;

            memcpy(&c, s->rx, sizeof(c));
            s->rxlen = 0;
            s->rxfd = -1;
            if (nb_rate_exceeded(s)) {
                if (fd >= 0) {
                    nb_fd_drop(fd);
                }
                nb_violation(s, "command rate far beyond anything a display "
                                "needs; treating it as an attempt to burn the "
                                "broker's CPU");
                return;
            }
            nb_handle_cmd(s, &c, fd);
        }
        if (--budget == 0 || nb_work_spent(s)) {
            return;             /* let the display server have the loop */
        }
    }
}

/* ── listener ────────────────────────────────────────────────────────────── */

/*
 * SOCKET ACTIVATION, without linking libsystemd.
 *
 * The protocol is three environment variables and a convention, so the
 * dependency buys nothing: LISTEN_PID must be OUR pid (that is what stops a
 * parent's variables being inherited by a grandchild and misread), LISTEN_FDS
 * is a count, and the descriptors start at fd 3.
 *
 * Returns the fd, or -1 when we were not socket-activated.  The variables are
 * cleared either way so nothing downstream can misread them.
 */
#define NB_LISTEN_FDS_START 3

static int nb_listen_fds_take(void)
{
    const char *pid_s = getenv("LISTEN_PID");
    const char *fds_s = getenv("LISTEN_FDS");
    long pid, n;
    int fd = -1;

    if (pid_s && fds_s) {
        pid = strtol(pid_s, NULL, 10);
        n   = strtol(fds_s, NULL, 10);
        if (pid == (long)getpid() && n >= 1) {
            if (n > 1) {
                nb_log("socket activation passed %ld descriptors; using the "
                       "first and ignoring the rest", n);
            }
            fd = NB_LISTEN_FDS_START;
        } else if (pid != (long)getpid()) {
            nb_err("LISTEN_PID is %ld but we are %ld — not socket-activated, "
                   "ignoring inherited activation variables",
                   pid, (long)getpid());
        }
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return fd;
}

/*
 * Adopt a listening fd we did not create (socket activation, or --socket-fd).
 * It is already bound and listening, so we must NOT bind, chmod or unlink it --
 * and on shutdown must not remove a path we do not own.
 */
static int nb_adopt_fd(int fd)
{
    struct sockaddr_storage addr;
    struct stat st;
    int val = 0, type = 0;
    socklen_t len;

    if (fstat(fd, &st) < 0 || !S_ISSOCK(st.st_mode)) {
        nb_err("--socket-fd %d is not a socket", fd);
        return -EINVAL;
    }
    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0 ||
        type != SOCK_STREAM) {
        nb_err("--socket-fd %d is not a SOCK_STREAM socket", fd);
        return -EINVAL;
    }
    memset(&addr, 0, sizeof(addr));
    len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0 ||
        addr.ss_family != AF_UNIX) {
        nb_err("--socket-fd %d is not an AF_UNIX socket; network listeners "
               "can never authenticate with SO_PEERCRED", fd);
        return -EINVAL;
    }
    /* Refuse anything that is not already listening: adopting a connected or
     * unbound socket would fail later and much less clearly. */
    len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) < 0 || !val) {
        nb_err("--socket-fd %d is not a listening socket", fd);
        return -EINVAL;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        nb_err("--socket-fd %d: cannot set CLOEXEC/NONBLOCK: %s",
               fd, strerror(errno));
        return -errno;
    }
    nb_log("using an adopted AF_UNIX SOCK_STREAM listener (fd %d); its "
           "filesystem mode is external, so SO_PEERCRED remains mandatory",
           fd);
    return fd;
}

static int nb_listen(const char *path, const struct nb_config *cfg)
{
    struct sockaddr_un sa;
    struct stat st;
    mode_t old;
    int fd;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        nb_err("socket path too long (max %zu)", sizeof(sa.sun_path) - 1);
        return -ENAMETOOLONG;
    }
    /* Remove a stale socket, but ONLY if it really is a socket: a broker that
     * unlinks whatever it finds at its path is a foot-gun with root. */
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            nb_err("%s exists and is not a socket — refusing to touch it",
                   path);
            return -EEXIST;
        }
        unlink(path);
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        nb_err("socket: %s", strerror(errno));
        return -errno;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

    /* bind() applies the umask to the socket's mode, so set it here rather
     * than chmod()ing afterwards and leaving a window where it is 0777. */
    old = umask((mode_t)(~cfg->socket_mode & 0777));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = errno;
        umask(old);
        close(fd);
        nb_err("bind %s: %s", path, strerror(e));
        return -e;
    }
    umask(old);
    /* Belt and braces: umask can only clear bits, and a hostile umask cannot
     * ADD any, but an unusual filesystem could.  Assert the mode we want. */
    if (chmod(path, (mode_t)cfg->socket_mode) < 0) {
        nb_err("chmod %04o %s: %s", cfg->socket_mode, path, strerror(errno));
        close(fd);
        unlink(path);
        return -errno;
    }
    /*
     * The group has to be set for --socket-mode 0660 to mean anything: a mode
     * that grants the group access is useless while the group is the broker
     * user's own.  Done after chmod so a failure leaves the tighter mode.
     */
    if (cfg->socket_group) {
        struct group *gr = getgrnam(cfg->socket_group);

        if (!gr) {
            nb_err("--socket-group %s: no such group", cfg->socket_group);
            close(fd);
            unlink(path);
            return -EINVAL;
        }
        if (chown(path, (uid_t)-1, gr->gr_gid) < 0) {
            nb_err("chgrp %s %s: %s (you must own the socket and be a member "
                   "of that group, or be root)",
                   cfg->socket_group, path, strerror(errno));
            close(fd);
            unlink(path);
            return -errno;
        }
        nb_log("socket group is %s (gid %u)", cfg->socket_group,
               (unsigned)gr->gr_gid);
    }
    if (listen(fd, 4) < 0) {
        nb_err("listen: %s", strerror(errno));
        close(fd);
        unlink(path);
        return -errno;
    }
    return fd;
}

/*
 * HARDENING 7: SO_PEERCRED, checked BEFORE a single byte goes out.  The kernel
 * fills this in at connect() time from the peer's credentials, so it cannot be
 * forged by the peer, and unlike a filesystem permission it survives a socket
 * whose mode someone widened.
 */
static bool nb_peer_allowed(int fd, const struct nb_config *cfg,
                            struct ucred *out)
{
    socklen_t len = sizeof(*out);
    int i;

    memset(out, 0, sizeof(*out));
    /*
     * --no-peercred.  Documented opt-out for the case the credential check
     * cannot answer: a peer behind a proxy, or a userns where the uid we see
     * is not the uid that matters.  It hands the whole decision to the
     * socket's filesystem permissions, which is why the two are validated
     * together at startup and the wide-open combination is refused.
     */
    if (cfg->no_peercred) {
        (void)getsockopt(fd, SOL_SOCKET, SO_PEERCRED, out, &len);
        return true;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, out, &len) < 0 ||
        len != sizeof(*out)) {
        nb_err("SO_PEERCRED failed: %s", strerror(errno));
        return false;
    }
    for (i = 0; i < cfg->n_allow_uid; i++) {
        if (out->uid == cfg->allow_uid[i]) {
            return true;
        }
    }
    for (i = 0; i < cfg->n_allow_gid; i++) {
        if (out->gid == cfg->allow_gid[i]) {
            return true;
        }
    }
    return false;
}

/* ── privilege drop ──────────────────────────────────────────────────────── */

/*
 * Everything the broker needs is held as an OPEN FD by the time this runs —
 * the display-server connection, the input devices behind it, and the bound
 * listening socket — which is what lets this be so blunt.  The broker retains
 * NO capabilities: setuid(2) away from root clears the permitted and effective
 * sets, we do not use SECBIT_KEEP_CAPS, and nothing here wants one back.
 *
 * Note the broker is normally NOT started as root at all: it is a session
 * program, started by the user whose desktop it draws on.  --drop-user exists
 * for the systemd-unit case.
 */
static int nb_drop_privs(const char *user)
{
    struct passwd *pw = NULL;
    uid_t uid = 0;
    gid_t gid = 0;
    bool have_target = false;
    char label[64];

    if (!user) {
        if (geteuid() == 0) {
            nb_log("WARNING: running as root and --drop-user was not given. "
                   "The broker will stay root. Pass --drop-user <name|uid>.");
        }
        goto harden;
    }

    /*
     * NAME or NUMERIC.  A container has no passwd entry for a uid picked at
     * random, and picking at random is the point: the broker ends up as a uid
     * that owns nothing on the system, rather than as the desktop user whose
     * files, keys and autostart directory it would otherwise still reach.
     */
    /*
     * "auto": pick a uid nothing owns, from INSIDE this namespace's uid map.
     *
     * Done here rather than in the launcher because only this process can read
     * /proc/self/uid_map, and a uid outside it fails setuid() with EINVAL --
     * which is what a hardcoded high offset does under `dockerd --userns-remap`
     * or sysbox-runc, where the container typically maps just 65536 uids.
     *
     * Chosen from the TOP half of the usable range: real accounts, system users
     * and /etc/subuid allocations all live low, so the top collides with least.
     * Deliberately not 65534 -- `nobody` is shared by half the sandboxed daemons
     * on a desktop, and while PR_SET_DUMPABLE(0) below stops a same-uid process
     * ptracing us for the display fd, it does not stop one signalling us dead.
     */
    if (!strcmp(user, "auto")) {
        struct nvkvm_uidmap map;
        uint32_t span, pick;
        unsigned int seed = 0;
        FILE *rnd = fopen("/dev/urandom", "re");

        nvkvm_uidmap_get(&map);
        if (rnd) {
            if (fread(&seed, sizeof(seed), 1, rnd) != 1) {
                seed = (unsigned int)getpid();
            }
            fclose(rnd);
        } else {
            seed = (unsigned int)getpid();
        }
        /* Top half, but never uid 0 and never below the map's floor. */
        uint32_t lo = map.lo + (map.hi - map.lo) / 2;

        if (lo == 0) {
            lo = 1;
        }
        span = map.hi > lo ? map.hi - lo : 0;
        pick = span ? lo + (seed % span) : lo;
        if (pick == 0 || pick == geteuid()) {
            pick = lo ? lo : 1;
        }
        uid = (uid_t)pick;
        gid = (gid_t)pick;
        have_target = true;
        snprintf(label, sizeof(label), "uid %u gid %u (auto, map %u..%u)",
                 (unsigned)uid, (unsigned)gid, map.lo, map.hi);
        goto do_drop;
    }

    if (user[0] >= '0' && user[0] <= '9') {
        char *end = NULL;
        unsigned long want = strtoul(user, &end, 10);

        uid = (uid_t)want;
        gid = (gid_t)want;
        if (end && *end == ':') {
            gid = (gid_t)strtoul(end + 1, &end, 10);
        }
        if (!end || *end != '\0' || want == 0) {
            nb_err("--drop-user %s: expected a name, or UID[:GID] with UID != 0",
                   user);
            return -EINVAL;
        }
        have_target = true;
        snprintf(label, sizeof(label), "uid %u gid %u",
                 (unsigned)uid, (unsigned)gid);
    } else {
        pw = getpwnam(user);
        if (!pw) {
            nb_err("--drop-user %s: no such user", user);
            return -ENOENT;
        }
        if (pw->pw_uid == 0) {
            nb_err("--drop-user %s resolves to uid 0 — refusing", user);
            return -EINVAL;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
        have_target = true;
        snprintf(label, sizeof(label), "%s (uid %u, gid %u)", user,
                 (unsigned)uid, (unsigned)gid);
    }

    if (!have_target) {
        goto harden;
    }

do_drop:

    /*
     * Everything the broker needs is already an OPEN FD by the time this runs:
     * the display-server connection, the input devices behind it, and the bound
     * listening socket.  That is what lets the drop be this blunt -- nothing
     * after this point needs to open anything by name.
     *
     * It also means the drop works from a NON-root start, which is the
     * container case: the entrypoint hands us CAP_SETUID/CAP_SETGID and nothing
     * else, we spend them here, and clear them below.  Dropping later than
     * startup is deliberate; dropping earlier would mean connecting to the
     * compositor as a uid that cannot reach its socket.
     */
    if (pw ? initgroups(pw->pw_name, gid) < 0 : setgroups(0, NULL) < 0) {
        nb_err("dropping to %s: clearing groups: %s", label, strerror(errno));
        return -errno;
    }
    if (setgid(gid) < 0 || setuid(uid) < 0) {
        nb_err("dropping to %s: %s%s", label, strerror(errno),
               errno == EPERM
                   ? " (no CAP_SETUID: start the broker as root, or grant it "
                     "ambient CAP_SETUID/CAP_SETGID so it can drop after the "
                     "window is up)"
                   : "");
        return -errno;
    }

    /*
     * SPEND THE CAPABILITY, THEN THROW IT AWAY.
     *
     * A root->nonroot setuid clears the permitted set for free, but a
     * nonroot->nonroot one does NOT: ambient CAP_SETUID survives it, and a
     * broker that can still setuid(0) has not dropped anything.  Clear the
     * ambient set and then the whole capability set explicitly, before the
     * check below -- otherwise the check would pass while the capability is
     * still held.
     */
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
    {
        struct __user_cap_header_struct hdr = {
            .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0
        };
        struct __user_cap_data_struct data[2] = { { 0, 0, 0 }, { 0, 0, 0 } };

        if (syscall(SYS_capset, &hdr, data) < 0 && errno != EPERM) {
            nb_err("clearing capabilities after the drop: %s", strerror(errno));
            return -errno;
        }
    }

    /* Not allowed to fail silently: prove it took, rather than announce it. */
    if (setuid(0) == 0 || geteuid() != uid) {
        nb_err("privilege drop did not take — refusing to continue");
        _exit(1);
    }
    nb_log("dropped privileges to %s, no capabilities held", label);

harden:
    /* These are advertised security properties, not best-effort tuning.  A
     * failure must be visible and must stop before the broker accepts a peer. */
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) < 0) {
        int e = errno;

        nb_err("PR_SET_DUMPABLE failed: %s; refusing to serve clients",
               strerror(e));
        return -e;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        int e = errno;

        nb_err("PR_SET_NO_NEW_PRIVS failed: %s; refusing to serve clients",
               strerror(e));
        return -e;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fputs(
"usage: nvkvm-display-broker --socket PATH [options]\n"
"\n"
"  --socket PATH        unix socket to listen on (mode from --socket-mode)\n"
"  --socket-mode OCTAL  socket permissions (default 0600).  0660 with\n"
"                       --socket-group is what lets another user connect\n"
"  --socket-group NAME  group to own the socket; without it a group-\n"
"                       readable mode grants nobody anything new\n"
"  --no-peercred        serve any peer the socket permissions admit, instead\n"
"                       of checking SO_PEERCRED against the allow list.\n"
"                       Refused for adopted fds and world-accessible modes\n"
"  --socket-fd N        use an already-bound listening fd instead of\n"
"                       creating one.  LISTEN_FDS socket activation is also\n"
"                       honoured, and takes precedence over --socket\n"
"  --backend NAME       auto (default) | wayland | x11 | test\n"
"  --size WxH           initial window size (default 1920x1080)\n"
"  --title TEXT         window title\n"
"  --allow-user NAME    additional user allowed to connect (repeatable)\n"
"  --allow-group NAME   additional PRIMARY gid allowed to connect; SO_PEERCRED\n"
"                       does not report a peer's supplementary groups\n"
"  --drop-user WHO      become this user after the window is up: a name, a\n"
"                       numeric UID[:GID], or `auto` to pick a uid nothing\n"
"                       owns from inside this namespace's uid map\n"
"  --fullscreen         start fullscreen (CTRL+ALT+F toggles)\n"
"  --scale MODE         aspect (default) keeps the guest's aspect ratio and\n"
"                       fills the remainder with black; stretch fills the\n"
"                       window and distorts; none is 1:1, no scaling\n"
"  --persist            keep the window when the VMM disconnects and wait\n"
"                       for another (default: exit with it)\n"
"  --clipboard MODE     off (default) | guest-to-host | consent\n"
"                         off            nothing crosses\n"
"                         guest-to-host  the guest may write YOUR clipboard;\n"
"                                        it can never read it\n"
"                         consent        the above, plus host->guest on an\n"
"                                        explicit paste key.  RECOMMENDED\n"
"                       Automatic/full sync is deliberately not implemented.\n"
"                       Text only, UTF-8, 7 KiB (7168 bytes) max, rate limited\n"
"                       both ways.  Needs QEMU -chardev qemu-vdagent and\n"
"                       spice-vdagent in the guest as well\n"
"  --clipboard-trigger LIST  keys that mean paste, replacing the default\n"
"                       list (ctrl+v,ctrl+shift+v,shift+insert).  A paste\n"
"                       chosen from a MENU cannot be caught this way\n"
"  --trace-frames       log one line per commit: frame counter, dma-buf\n"
"                       inode, cache slot, buffers the compositor still holds\n"
"  --refresh WHAT       what refresh to tell the guest: auto (default, from\n"
"                       the compositor's presentation feedback), none (say\n"
"                       nothing; the guest keeps its kms_hz default), or a\n"
"                       rate in Hz.  The guest paces its virtual vblank to\n"
"                       this, so it is not cosmetic.\n"
"  --resolution WHAT    what size to SUGGEST to the guest.  A suggestion only:\n"
"                       the guest may ignore it, and whatever frame arrives is\n"
"                       displayed at whatever size it arrives.\n"
"                         auto   (default) the window's content size, sent at\n"
"                                startup and whenever the window geometry\n"
"                                changes -- never in reply to a guest frame\n"
"                         none   suggest nothing at all; the guest picks its\n"
"                                own size and the broker scales it\n"
"                         WxH    always suggest this size\n"
"                       Use --scale to choose how a mismatch is fitted.\n"
"  --present-mode=MODE  which presentation tier to offer.  The tiers are a\n"
"                       fallback ladder, cheapest first:\n"
"                         auto    (default) the guest's own modifier if the\n"
"                                 display takes it, else a LINEAR dma-buf, else\n"
"                                 shared memory\n"
"                         native  zero-copy or nothing\n"
"                         linear  advertise only LINEAR, forcing the VMM to\n"
"                                 detile.  Reproduces a cross-vendor host on a\n"
"                                 single-GPU one.  Costs one GPU transfer\n"
"                         shm     no dma-buf at all: plain shared memory, which\n"
"                                 no compositor can refuse.  Slowest, universal\n"
"  --linear-only        deprecated alias for --present-mode=linear\n"
"  --verbose\n"
"\n"
"The broker owns the window, the compositor connection and the input grab.\n"
"The VMM keeps only this socket: it relays the guest's scanout dma-buf fd\n"
"here and receives input.  It needs no EGL, no GL and no display server.\n"
"By default root and the invoking user may connect, and only they can reach\n"
"the socket at all.  To let a DIFFERENT user's VMM connect you need both\n"
"gates opened: --socket-mode 0660 --socket-group vmm  (the filesystem) and\n"
"--allow-group vmm  (the credential check).\n"
"\n"
"A host resize never changes the guest's resolution -- it is scaled here.\n"
"Going fullscreen does tell the guest, so it can render at the output's own\n"
"resolution, which is what lets the compositor scan its buffer out directly.\n", stderr);
}

/*
 * --clipboard-trigger "ctrl+v,ctrl+shift+v,shift+insert"
 *
 * Replaces the default list wholesale rather than adding to it, so what fires
 * is exactly what is written and there is no invisible inherited entry.
 */
static int nb_parse_triggers(struct nb_config *c, const char *spec)
{
    char buf[256];
    char *save = NULL, *tok;

    if (strlen(spec) >= sizeof(buf)) {
        nb_err("--clipboard-trigger is too long");
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", spec);
    c->n_clip_trigger = 0;

    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        struct nb_clip_trigger t = { 0, false, false };
        char *plus = NULL, *part, *psave = NULL;

        for (part = strtok_r(tok, "+", &psave); part;
             part = strtok_r(NULL, "+", &psave)) {
            (void)plus;
            if (!strcasecmp(part, "ctrl"))       { t.need_ctrl = true; }
            else if (!strcasecmp(part, "shift")) { t.need_shift = true; }
            else if (!strcasecmp(part, "v"))     { t.code = KEY_V; }
            else if (!strcasecmp(part, "insert")) { t.code = KEY_INSERT; }
            else {
                nb_err("--clipboard-trigger: '%s' is not a key this "
                       "understands (ctrl, shift, v, insert)", part);
                return -1;
            }
        }
        if (t.code == 0) {
            nb_err("--clipboard-trigger: '%s' names modifiers but no key",
                   tok);
            return -1;
        }
        if (c->n_clip_trigger >= NB_CLIP_MAX_TRIGGERS) {
            nb_err("--clipboard-trigger: at most %d triggers",
                   NB_CLIP_MAX_TRIGGERS);
            return -1;
        }
        c->clip_trigger[c->n_clip_trigger++] = t;
    }
    if (c->n_clip_trigger == 0) {
        nb_err("--clipboard-trigger: empty list (use --clipboard=off to "
               "disable instead)");
        return -1;
    }
    return 0;
}

static int add_user(struct nb_config *c, const char *name)
{
    struct passwd *pw = getpwnam(name);

    if (!pw) {
        nb_err("--allow-user %s: no such user", name);
        return -1;
    }
    if (c->n_allow_uid >= NB_MAX_ALLOW_IDS) {
        nb_err("too many --allow-user");
        return -1;
    }
    c->allow_uid[c->n_allow_uid++] = pw->pw_uid;
    return 0;
}

static int add_group(struct nb_config *c, const char *name)
{
    struct group *gr = getgrnam(name);

    if (!gr) {
        nb_err("--allow-group %s: no such group", name);
        return -1;
    }
    if (c->n_allow_gid >= NB_MAX_ALLOW_IDS) {
        nb_err("too many --allow-group");
        return -1;
    }
    c->allow_gid[c->n_allow_gid++] = gr->gr_gid;
    return 0;
}

/*
 * HARDENING, announced.  This is the "detect and announce grab capability at
 * startup, in one plain line" requirement: a partial grab is fine, a partial
 * grab claimed as total is not.
 */
static void nb_announce(const struct nb_session *sess)
{
    nb_log("backend '%s': window %ux%u", sess->ops->name,
           sess->width, sess->height);
    nb_log("grab capabilities: keyboard=%d abs=%d rel=%d lock=%d "
           "shortcuts-inhibited=%d focus-events=%d fullscreen=%d",
           !!(sess->caps & NVKVM_BROKER_CAP_KEYBOARD),
           !!(sess->caps & NVKVM_BROKER_CAP_ABS_POINTER),
           !!(sess->caps & NVKVM_BROKER_CAP_REL_POINTER),
           !!(sess->caps & NVKVM_BROKER_CAP_POINTER_LOCK),
           !!(sess->caps & NVKVM_BROKER_CAP_TOTAL_GRAB),
           !!(sess->caps & NVKVM_BROKER_CAP_FOCUS_EVENTS),
           !!(sess->caps & NVKVM_BROKER_CAP_FULLSCREEN));
    if (!(sess->caps & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
        nb_log("GRAB IS NOT OFFERED: this session cannot report focus loss, "
               "and a grab that cannot be dropped on focus loss is a "
               "keylogger. CTRL+ALT+G will refuse.");
    } else if (sess->grab_caveat[0]) {
        nb_log("GRAB IS PARTIAL: %s", sess->grab_caveat);
    } else {
        nb_log("GRAB IS TOTAL: under CTRL+ALT+G every key reaches the guest "
               "and the pointer is locked to the window.");
    }
}

static int nb_validate_clipboard_mode(const struct nb_config *cfg,
                                      const struct nb_session *sess)
{
    const char *available;
    uint32_t need = 0;

    if (cfg->clip_mode == NB_CLIP_G2H) {
        need = NB_SESSION_CLIP_G2H;
    } else if (cfg->clip_mode == NB_CLIP_CONSENT) {
        need = NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G;
    }
    if ((sess->clipboard_caps & need) == need) {
        return 0;
    }
    switch (sess->clipboard_caps &
            (NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G)) {
    case NB_SESSION_CLIP_G2H:
        available = "guest-to-host only";
        break;
    case NB_SESSION_CLIP_H2G:
        available = "host-to-guest only";
        break;
    case NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G:
        available = "guest-to-host and host-to-guest";
        break;
    default:
        available = "no clipboard";
        break;
    }
    nb_err("clipboard mode '%s' is unsupported by backend '%s' (%s). "
           "Refusing instead of swallowing paste keys or clipboard data.",
           nb_clip_mode_name(cfg->clip_mode), sess->ops->name,
           available);
    return -ENOTSUP;
}

int main(int argc, char **argv)
{
    struct nb_config cfg;
    struct nb_session *sess;
    static struct nb_sink sink;     /* ~12 KB of ring; not a stack object */
    /* signalfd + listener + at most one client, then the session's own fds.
     * Getting this count wrong is a stack overflow in a process holding the
     * keyboard grab, so it is spelled out rather than left as "2 +". */
    struct pollfd pfd[3 + NB_MAX_SESSION_FDS];
    int listen_fd, sigfd, i, rc = 1;
    /* We only unlink a path we created ourselves: an adopted or activated
     * socket belongs to whoever handed it over. */
    bool socket_is_ours = true;
    bool had_client = false;
    /* Sticky within one loop iteration; see the single exit check at the
     * bottom of the loop for why the check cannot live where the detach
     * is noticed. */
    bool client_lost = false;
    sigset_t mask;

    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = "auto";
    cfg.scale_mode = NB_SCALE_ASPECT;   /* preserve aspect, black bars */
    cfg.socket_mode = 0600;
    cfg.socket_fd = -1;
    cfg.clip_mode = NB_CLIP_OFF;
    /* CTRL+V, CTRL+SHIFT+V and SHIFT+INSERT: the three chords that mean paste
     * to most people.  Replaceable wholesale with --clipboard-trigger. */
    cfg.clip_trigger[0] = (struct nb_clip_trigger){ KEY_V, true, false };
    cfg.clip_trigger[1] = (struct nb_clip_trigger){ KEY_V, true, true };
    cfg.clip_trigger[2] = (struct nb_clip_trigger){ KEY_INSERT, false, true };
    cfg.n_clip_trigger = 3;
    cfg.title = "nvkvm";
    cfg.win_w = 1920;
    cfg.win_h = 1080;
    /* Default allowlist: root, and whoever started the broker. */
    cfg.allow_uid[cfg.n_allow_uid++] = 0;
    if (getuid() != 0) {
        cfg.allow_uid[cfg.n_allow_uid++] = getuid();
    } else if (getenv("SUDO_UID")) {
        cfg.allow_uid[cfg.n_allow_uid++] =
            (uid_t)strtoul(getenv("SUDO_UID"), NULL, 10);
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

#define NEEDVAL() do { if (!v) { usage(); return 2; } i++; } while (0)
        if (!strcmp(a, "--socket"))          { NEEDVAL(); cfg.socket_path = v; }
        else if (!strcmp(a, "--backend"))    { NEEDVAL(); cfg.backend = v; }
        else if (!strcmp(a, "--title"))      { NEEDVAL(); cfg.title = v; }
        else if (!strcmp(a, "--size")) { NEEDVAL();
            unsigned w = 0, h = 0;
            if (sscanf(v, "%ux%u", &w, &h) != 2 || w == 0 || h == 0 ||
                w > NVKVM_BROKER_MAX_DIM || h > NVKVM_BROKER_MAX_DIM) {
                nb_err("--size must be WxH within 1..%u", NVKVM_BROKER_MAX_DIM);
                return 2;
            }
            cfg.win_w = w; cfg.win_h = h; }
        else if (!strcmp(a, "--drop-user"))  { NEEDVAL(); cfg.drop_user = v; }
        else if (!strcmp(a, "--allow-user")) { NEEDVAL();
            if (add_user(&cfg, v)) { return 2; } }
        else if (!strcmp(a, "--allow-group")) { NEEDVAL();
            if (add_group(&cfg, v)) { return 2; } }
        else if (!strcmp(a, "--verbose"))    { nb_verbose = 1; }
        else if (!strncmp(a, "--resolution=", 13) || !strcmp(a, "--resolution")) {
            unsigned rw, rh;

            const char *arg;

            if (a[12] == '=') { arg = a + 13; } else { NEEDVAL(); arg = v; }
            if (!strcmp(arg, "auto")) {
                nb_res_mode = NB_RES_AUTO;
            } else if (!strcmp(arg, "none") || !strcmp(arg, "guest")) {
                /* `guest` was the first spelling; kept so anything already
                 * written keeps working.  `none` is what it does. */
                nb_res_mode = NB_RES_NONE;
            } else if (sscanf(arg, "%ux%u", &rw, &rh) == 2 && rw && rh &&
                       rw <= NVKVM_BROKER_MAX_DIM && rh <= NVKVM_BROKER_MAX_DIM) {
                nb_res_mode = NB_RES_FIXED;
                nb_res_w = rw;
                nb_res_h = rh;
            } else {
                nb_err("--resolution must be auto, none, or WxH within 1..%u",
                       NVKVM_BROKER_MAX_DIM);
                return 2;
            }
        }
        else if (!strncmp(a, "--refresh=", 10) || !strcmp(a, "--refresh")) {
            const char *arg;
            double hz;
            char *end = NULL;

            if (a[9] == '=') { arg = a + 10; } else { NEEDVAL(); arg = v; }
            if (!strcmp(arg, "auto")) {
                nb_hz_mode = NB_HZ_AUTO;
            } else if (!strcmp(arg, "none")) {
                nb_hz_mode = NB_HZ_NONE;
            } else {
                hz = strtod(arg, &end);
                if (!end || *end || !(hz >= 1.0 && hz <= 1000.0)) {
                    nb_err("--refresh must be auto, none, or a rate in Hz "
                           "between 1 and 1000");
                    return 2;
                }
                nb_hz_mode  = NB_HZ_FIXED;
                nb_hz_fixed = (unsigned)(hz * 1000.0 + 0.5);
            }
        }
        else if (!strcmp(a, "--linear-only")) { nb_linear_only = 1; }
        else if (!strncmp(a, "--present-mode=", 15)) {
            const char *mode = a + 15;
            if      (!strcmp(mode, "auto"))   { nb_tier = NB_TIER_AUTO;   }
            else if (!strcmp(mode, "native")) { nb_tier = NB_TIER_NATIVE; }
            else if (!strcmp(mode, "linear")) { nb_tier = NB_TIER_LINEAR; }
            else if (!strcmp(mode, "shm"))    { nb_tier = NB_TIER_SHM;    }
            else {
                nb_err("--present-mode must be auto, native, linear or shm");
                return 2;
            }
        }
        else if (!strcmp(a, "--trace-frames")) { nb_trace_frames = 1; }
        else if (!strcmp(a, "--fullscreen")) { cfg.fullscreen = true; }
        else if (!strcmp(a, "--persist"))    { cfg.persist = true; }
        else if (!strcmp(a, "--socket-mode")) { NEEDVAL();
            char *end = NULL;
            unsigned long m = strtoul(v, &end, 8);

            if (!end || *end || m > 0777) {
                nb_err("--socket-mode must be an octal mode like 0660 "
                       "(got '%s')", v);
                return 2;
            }
            cfg.socket_mode = (unsigned)m; }
        else if (!strcmp(a, "--socket-group")) { NEEDVAL();
            cfg.socket_group = v; }
        else if (!strcmp(a, "--no-peercred")) { cfg.no_peercred = true; }
        else if (!strcmp(a, "--clipboard")) { NEEDVAL();
            if (!strcmp(v, "off"))           { cfg.clip_mode = NB_CLIP_OFF; }
            else if (!strcmp(v, "guest-to-host")) { cfg.clip_mode = NB_CLIP_G2H; }
            else if (!strcmp(v, "consent"))  { cfg.clip_mode = NB_CLIP_CONSENT; }
            else if (!strcmp(v, "full")) {
                nb_err("--clipboard=full is not implemented: automatic host "
                       "clipboard reads were advertised without distinct "
                       "behavior. Use 'consent' for explicit paste only.");
                return 2;
            }
            else {
                nb_err("--clipboard must be off, guest-to-host or consent "
                       "(got '%s'). `consent` is the recommended one.", v);
                return 2;
            } }
        else if (!strcmp(a, "--clipboard-trigger")) { NEEDVAL();
            if (nb_parse_triggers(&cfg, v) != 0) {
                return 2;
            } }
        else if (!strcmp(a, "--socket-fd")) { NEEDVAL();
            char *end = NULL;
            long n = strtol(v, &end, 10);

            if (!end || *end || n < 0 || n > 1024) {
                nb_err("--socket-fd must be a descriptor number (got '%s')", v);
                return 2;
            }
            cfg.socket_fd = (int)n; }
        else if (!strcmp(a, "--scale")) { NEEDVAL();
            if (!strcmp(v, "stretch"))     { cfg.scale_mode = NB_SCALE_STRETCH; }
            else if (!strcmp(v, "aspect")) { cfg.scale_mode = NB_SCALE_ASPECT; }
            else if (!strcmp(v, "none"))   { cfg.scale_mode = NB_SCALE_NONE; }
            else {
                nb_err("--scale must be stretch, aspect or none (not '%s')", v);
                return 2;
            } }
        /* The old boolean pair, kept working because it is in scripts. */
        else if (!strcmp(a, "--no-scale")) { cfg.scale_mode = NB_SCALE_NONE; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { nb_err("unknown argument: %s", a); usage(); return 2; }
#undef NEEDVAL
    }
    /* --socket is no longer the only way to get a listening socket; the check
     * that matters happens once activation has had its say, below. */
    if (!cfg.socket_path && cfg.socket_fd < 0 && !getenv("LISTEN_FDS")) {
        usage();
        return 2;
    }

    /* Resolve activation before evaluating authentication.  Configured mode
     * bits say nothing about an adopted socket, which is exactly how a TCP
     * listener once reached HELLO under --no-peercred while the log claimed
     * it was protected by the default 0600. */
    if (cfg.socket_fd < 0) {
        cfg.socket_fd = nb_listen_fds_take();
        if (cfg.socket_fd >= 0) {
            nb_log("socket-activated: LISTEN_FDS gave us fd %d", cfg.socket_fd);
        }
    }
    socket_is_ours = cfg.socket_fd < 0;
    if (!socket_is_ours && cfg.no_peercred) {
        nb_err("--no-peercred is refused for --socket-fd/socket activation: "
               "the broker cannot prove an adopted listener's pathname, "
               "owner and mode. Keep SO_PEERCRED enabled.");
        return 2;
    }

    /* Block the signals we want to see through a fd, so the main loop is one
     * poll() with no async-signal-safety questions anywhere. */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sigfd < 0) {
        nb_err("signalfd: %s", strerror(errno));
        return 1;
    }

    /* 1. put a window on the user's desktop and start reading input */
    /*
     * THE TWO GATES ARE VALIDATED TOGETHER, because each is only safe while
     * the other holds.  A socket any local user can reach is fine while
     * SO_PEERCRED decides who is served; an unchecked peer is fine while the
     * filesystem keeps everyone else out.  BOTH open at once means any user on
     * the machine can drive the process holding your keyboard, so it is
     * refused rather than warned about.
     */
    if (socket_is_ours && cfg.no_peercred && (cfg.socket_mode & 0006)) {
        nb_err("--no-peercred with --socket-mode %04o would let ANY local user "
               "connect to the process that owns your display and can grab "
               "your keyboard. Refusing. Use a group (--socket-mode 0660 "
               "--socket-group ...) or keep the credential check.",
               cfg.socket_mode);
        return 2;
    }
    if (socket_is_ours && cfg.no_peercred) {
        nb_log("WARNING: --no-peercred: any peer the socket's permissions let "
               "through is served. The socket is mode %04o%s%s.",
               cfg.socket_mode, cfg.socket_group ? ", group " : "",
               cfg.socket_group ? cfg.socket_group : "");
    }
    if (socket_is_ours && (cfg.socket_mode & 0006)) {
        nb_log("NOTE: socket mode %04o is world-accessible; only SO_PEERCRED "
               "is keeping other users out.", cfg.socket_mode);
    }
    if (socket_is_ours && cfg.socket_group && !(cfg.socket_mode & 0060)) {
        nb_log("NOTE: --socket-group %s has no effect at mode %04o — the "
               "group has no access. Did you mean --socket-mode 0660?",
               cfg.socket_group, cfg.socket_mode);
    }

    sess = nb_session_open(&cfg);
    if (!sess) {
        return 1;               /* the backend already said why */
    }
    if (nb_validate_clipboard_mode(&cfg, sess) != 0) {
        sess->ops->close(sess);
        return 2;
    }
    nb_announce(sess);
    if (!(sess->caps & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
        sess->caps &= ~(NVKVM_BROKER_CAP_REL_POINTER |
                        NVKVM_BROKER_CAP_POINTER_LOCK |
                        NVKVM_BROKER_CAP_TOTAL_GRAB);
    }

    /* 2. bind the socket */
    /*
     * Socket activation wins over --socket, because a manager that handed us a
     * listening fd has already decided the path, mode and ownership; binding
     * our own would quietly ignore all three.
     */
    if (cfg.socket_fd >= 0) {
        listen_fd = nb_adopt_fd(cfg.socket_fd);
        socket_is_ours = false;
    } else if (!cfg.socket_path) {
        nb_err("--socket PATH is required (or pass --socket-fd, or use socket "
               "activation)");
        return 2;
    } else {
        listen_fd = nb_listen(cfg.socket_path, &cfg);
    }
    if (listen_fd < 0) {
        sess->ops->close(sess);
        return 1;
    }
    if (socket_is_ours) {
        nb_log("listening on %s (mode %04o%s%s)", cfg.socket_path,
               cfg.socket_mode, cfg.socket_group ? ", group " : "",
               cfg.socket_group ? cfg.socket_group : "");
    }
    if (cfg.no_peercred) {
        nb_log("  peer credential check DISABLED (--no-peercred)");
    }
    for (i = 0; i < cfg.n_allow_uid; i++) {
        nb_log("  allowed uid %u", (unsigned)cfg.allow_uid[i]);
    }
    for (i = 0; i < cfg.n_allow_gid; i++) {
        nb_log("  allowed gid %u", (unsigned)cfg.allow_gid[i]);
    }

    /* 3. and now give up everything we no longer need */
    if (nb_drop_privs(cfg.drop_user) != 0) {
        goto out;
    }

    nb_sink_init(&sink, sess);
    sink.cfg = &cfg;   /* clipboard policy is config, read at every decision */

    /* Put something on the screen NOW.  Until a client attaches the surface
     * has no content, and a contentless window is indistinguishable from a
     * dead broker — which is the first thing a user meets. */
    if (sess->ops->show_idle) {
        sess->ops->show_idle(sess);
    }

    for (;;) {
        int n = 0, nsess, r, tmo, ctmo;

        /*
         * Core clock work FIRST, before the backend builds its poll set:
         * whatever it hands the backend (a paced cursor update) is then
         * flushed to the display server by that backend's pollfds() in this
         * same iteration, instead of sitting queued until something else
         * wakes the loop.
         */
        ctmo = nb_sink_tick(&sink);

        pfd[n].fd = sigfd;      pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        pfd[n].fd = listen_fd;  pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        if (sink.client_fd >= 0) {
            pfd[n].fd = sink.client_fd;
            pfd[n].events = POLLIN | (nb_sink_want_write(&sink) ? POLLOUT : 0);
            pfd[n].revents = 0;
            n++;
        }
        nsess = sess->ops->pollfds(sess, pfd + n, NB_MAX_SESSION_FDS);
        if (nsess < 0) {
            nb_err("session lost its event source");
            break;
        }

        /* -1 unless a backend has clock-driven work pending; see ops->tick. */
        tmo = sess->ops->tick ? sess->ops->tick(sess) : -1;
        if (ctmo >= 0 && (tmo < 0 || ctmo < tmo)) {
            tmo = ctmo;
        }

        if (poll(pfd, (nfds_t)(n + nsess), tmo) < 0) {
            if (errno == EINTR) {
                continue;
            }
            nb_err("poll: %s", strerror(errno));
            break;
        }

        if (pfd[0].revents) {
            nb_log("signal received; shutting down");
            break;
        }

        /* Client first, so a death is noticed before we queue more for it. */
        if (sink.client_fd >= 0) {
            struct pollfd *cp = &pfd[2];

            if (cp->revents & POLLIN) {
                nb_sink_readable(&sink);
            }
            if (sink.client_fd >= 0 && (cp->revents & (POLLHUP | POLLERR))) {
                nb_sink_detach(&sink, "hung up");
            }
            if (sink.client_fd >= 0 && (cp->revents & POLLOUT)) {
                if (nb_sink_flush(&sink) < 0) {
                    nb_sink_detach(&sink, "write failed");
                }
            }
            /*
             * The VMM went away.  Recorded, not acted on here: the exit
             * itself happens at the bottom of the loop, because this is not
             * the only place a client can be lost.  See there.
             */
            if (sink.client_fd < 0) {
                client_lost = true;
            }
        }

        if (pfd[1].revents & POLLIN) {
            struct ucred cred;
            int cfd = accept4(listen_fd, NULL, NULL,
                              SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd >= 0) {
                if (!nb_peer_allowed(cfd, &cfg, &cred)) {
                    nb_err("rejected connection from uid %u gid %u pid %d",
                           (unsigned)cred.uid, (unsigned)cred.gid, cred.pid);
                    close(cfd);
                } else if (sink.client_fd >= 0) {
                    /* One client at a time.  Refusing (rather than displacing)
                     * means no allowed-uid process can steal the display out
                     * from under a running VM; the incumbent's death is
                     * detected reliably above, so this cannot wedge. */
                    nb_err("refusing second client (uid %u pid %d): the window "
                           "is already handed to pid %d",
                           (unsigned)cred.uid, cred.pid, (int)sink.client_pid);
                    close(cfd);
                } else {
                    nb_log("accepted uid %u gid %u pid %d",
                           (unsigned)cred.uid, (unsigned)cred.gid, cred.pid);
                    sink.client_pid = cred.pid;
                    if (nb_sink_attach(&sink, cfd) != 0) {
                        close(cfd);
                    }
                }
            }
        }

        r = sess->ops->dispatch(sess, &sink);
        if (r < 0) {
            nb_err("session dispatch failed: %s", strerror(-r));
            /* Say goodbye ONCE: detach here so the shutdown path below does
             * not send a second BYE with a different reason code. */
            nb_sink_bye(&sink, NVKVM_BROKER_BYE_DISPLAY_LOST);
            nb_sink_detach(&sink, "the display went away");
            break;
        }
        if (sink.client_fd >= 0 && nb_sink_flush(&sink) < 0) {
            nb_sink_detach(&sink, "write failed");
        }

        /*
         * THE ONE EXIT CHECK, AND IT HAS TO BE HERE.
         *
         * It used to sit inside the `sink.client_fd >= 0` block above, which
         * means it was only ever evaluated for a client that was still
         * attached when that block ran.  A client lost LATER in the same
         * iteration therefore never reached it, and on the next iteration the
         * block is skipped entirely (client_fd is already < 0) -- so the check
         * was never reached again either.  The broker then polls forever with
         * no client and no window content, which is exactly the state
         * --persist exists to opt INTO.
         *
         * Two paths do that today, both after the block above:
         *   - nb_emit() -> nb_sink_detach("client stopped draining its
         *     socket"), reached from inside sess->ops->dispatch(); and
         *   - the post-dispatch nb_sink_flush() failure a few lines up.
         *
         * client_lost is sticky for the iteration rather than re-derived from
         * client_fd, so a replacement client accepted in the same poll cycle
         * cannot swallow the loss -- which preserves exactly what the old
         * early break did, since it ran before accept().
         */
        if (had_client && sink.client_fd < 0) {
            client_lost = true;
        }
        had_client = (sink.client_fd >= 0);
        if (client_lost) {
            client_lost = false;
            if (!cfg.persist) {
                /* A broker window that outlives its VM shows nothing and still
                 * holds a window, a compositor connection and the hotkeys.
                 * --persist keeps it and waits for another client, which is
                 * what makes a VMM restart -- or a broker told to close so
                 * QEMU can shut the guest down gracefully -- survivable
                 * without restarting both. */
                nb_log("the client is gone; exiting (use --persist to keep "
                       "the window and wait for another)");
                break;
            }
            /* The VM is gone: say so, rather than leaving its last frame
             * frozen on screen looking like a hung guest. */
            if (sess->ops->show_idle) {
                sess->ops->show_idle(sess);
            }
        }
    }

    if (sink.client_fd >= 0) {
        nb_sink_bye(&sink, NVKVM_BROKER_BYE_SHUTDOWN);
        nb_sink_detach(&sink, "broker shutting down");
    }
    rc = 0;
out:
    close(listen_fd);
    /* Never unlink a socket we did not create: with activation the manager
     * owns the path and will hand it to the next start. */
    if (socket_is_ours && cfg.socket_path) {
        unlink(cfg.socket_path);
    }
    sess->ops->close(sess);
    close(sigfd);
    return rc;
}
