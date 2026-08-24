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
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Keys the broker swallowed as part of a hotkey chord.  The release must be
 * swallowed too, and it must be swallowed even if the modifiers came up first
 * — otherwise the guest sees a key-up it never saw a key-down for, which some
 * guest input stacks latch on forever.
 */
static unsigned char nb_consumed[768 / 8];

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

void nb_sink_init(struct nb_sink *s, struct nb_session *sess)
{
    memset(s, 0, sizeof(*s));
    s->sess = sess;
    s->client_fd = -1;
    s->rxfd = -1;
}

bool nb_sink_want_write(const struct nb_sink *s)
{
    return s->client_fd >= 0 && s->tx_head != s->tx_tail;
}

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
            out[rel_slot].x += p->x;
            out[rel_slot].y += p->y;
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
                            (s->focused ? NVKVM_BROKER_F_FOCUSED : 0)),
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
                            (s->focused ? NVKVM_BROKER_F_FOCUSED : 0)),
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

    /* State that must not survive the previous client. */
    s->seq = 0;
    s->tx_head = s->tx_tail = 0;
    s->tx_partial = 0;
    s->rxlen = 0;
    if (s->rxfd >= 0) {
        close(s->rxfd);
        s->rxfd = -1;
    }
    s->n_attach = s->n_commit = s->n_reject = 0;
    memset(s->key_down, 0, sizeof(s->key_down));
    memset(nb_consumed, 0, sizeof(nb_consumed));
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

    p = nb_pkt(s, NVKVM_BROKER_EV_HELLO, 0, 0,
               NVKVM_BROKER_PROTO_VERSION, ss->caps);
    r = nb_send_now(fd, &p);
    if (r == 0) {
        p = nb_pkt(s, NVKVM_BROKER_EV_SURFACE,
                   (int)ss->width, (int)ss->height, 0, 0);
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
    if (r != 0) {
        nb_err("handshake failed: %s", strerror(-r));
        s->client_fd = -1;
        return r;
    }

    nb_log("client attached: window %ux%u, capabilities 0x%x",
           ss->width, ss->height, ss->caps);
    nb_log("grab: CTRL+ALT+G toggles, CTRL+ALT+F fullscreen. %s",
           ss->grab_caveat[0] ? ss->grab_caveat
                              : "all keyboard input is captured under grab.");
    return 0;
}

void nb_sink_detach(struct nb_sink *s, const char *why)
{
    if (s->client_fd < 0) {
        return;
    }
    close(s->client_fd);
    s->client_fd = -1;
    s->tx_head = s->tx_tail = 0;
    s->tx_partial = 0;
    s->rxlen = 0;
    if (s->rxfd >= 0) {
        close(s->rxfd);
        s->rxfd = -1;
    }
    if (s->grabbed) {
        /* Never leave the host's keyboard grabbed because the VMM died. */
        s->sess->ops->set_grab(s->sess, false);
        s->grabbed = false;
    }
    nb_log("client detached: %s "
           "(%llu attach, %llu commit, %llu rejected)", why,
           (unsigned long long)s->n_attach,
           (unsigned long long)s->n_commit,
           (unsigned long long)s->n_reject);
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
    s->ctrl_down = s->alt_down = false;
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
    nb_emit(s, NVKVM_BROKER_EV_GRAB, on, 0, 0, 0);
    nb_log("grab %s", on ? "ON" : "off");
}

void nb_sink_key(struct nb_sink *s, unsigned code, bool down)
{
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

    /* Hotkeys are the broker's, never the client's. */
    if (down && s->ctrl_down && s->alt_down &&
        (code == KEY_G || code == KEY_F)) {
        bit_set(nb_consumed, code, true);
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
    if (bit_get(nb_consumed, code)) {
        if (!down) {
            bit_set(nb_consumed, code, false);
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

void nb_sink_pointer(struct nb_sink *s, bool inside)
{
    if (s->pointer_in == inside) {
        return;
    }
    s->pointer_in = inside;
    nb_emit(s, NVKVM_BROKER_EV_POINTER, inside, 0, 0, 0);
}

void nb_sink_surface(struct nb_sink *s, unsigned w, unsigned h)
{
    if (w == 0 || h == 0) {
        return;
    }
    if (s->sess->width == w && s->sess->height == h) {
        return;
    }
    s->sess->width = w;
    s->sess->height = h;
    nb_emit(s, NVKVM_BROKER_EV_SURFACE, (int)w, (int)h, 0, 0);
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
 */
static bool nb_fd_is_dmabuf(int fd, bool allow_memfd)
{
    struct statfs sfs;

    if (fstatfs(fd, &sfs) < 0) {
        return false;
    }
    if (sfs.f_type == DMA_BUF_MAGIC) {
        return true;
    }
    if (allow_memfd && sfs.f_type == TMPFS_MAGIC) {
        return true;
    }
    return false;
}

static void nb_violation(struct nb_sink *s, const char *why)
{
    nb_err("protocol violation from pid %d: %s", (int)s->client_pid, why);
    nb_sink_bye(s, NVKVM_BROKER_BYE_PROTOCOL);
    nb_sink_detach(s, "protocol violation");
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
    off_t size;
    uint32_t bpp;
    uint64_t need;
    char fcc[8];

    if (!nb_fd_is_dmabuf(fd, ss->accept_memfd)) {
        nb_err("ATTACH: the fd is not a dma-buf");
        return -EINVAL;
    }
    /* HARDENING 2: same bound as NVKVM_PRESENT_MAX_DIM in the VMM, restated
     * here because the VMM is the attacker in this model. */
    if (c->width == 0 || c->height == 0 ||
        c->width > NVKVM_BROKER_MAX_DIM || c->height > NVKVM_BROKER_MAX_DIM) {
        nb_err("ATTACH: %ux%u is out of range (1..%u)",
               c->width, c->height, NVKVM_BROKER_MAX_DIM);
        return -EINVAL;
    }
    /* HARDENING 3: not a hardcoded list — what the GPU advertises. */
    if (!ss->ops->format_ok(ss, c->fourcc, c->modifier)) {
        nb_err("ATTACH: fourcc %s modifier 0x%016llx is not advertised by "
               "this display", nb_fourcc_name(c->fourcc, fcc),
               (unsigned long long)c->modifier);
        return -EINVAL;
    }
    bpp = nb_fourcc_bpp(c->fourcc);
    if (bpp == 0) {
        nb_err("ATTACH: fourcc %s has no known bytes-per-pixel, so its extent "
               "cannot be bounded", nb_fourcc_name(c->fourcc, fcc));
        return -EINVAL;
    }
    /* A row must hold the pixels it claims to. */
    if ((uint64_t)c->stride < (uint64_t)c->width * bpp) {
        nb_err("ATTACH: stride %u < width %u * bpp %u",
               c->stride, c->width, bpp);
        return -EINVAL;
    }
    /* HARDENING 1: measure the fd, do not believe the sender.  The in-tree
     * pattern is nvkvm_isolate_handlers.c:1332. */
    size = lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        nb_err("ATTACH: lseek(SEEK_END) on the dma-buf gave %lld — refusing a "
               "buffer whose extent cannot be measured", (long long)size);
        return -EINVAL;
    }
    /*
     * 64-bit throughout: stride and height are both uint32, so their product
     * cannot wrap a uint64, and adding a uint32 offset to it cannot either.
     * That is what makes this test overflow-safe without a separate check.
     */
    need = (uint64_t)c->stride * c->height + (uint64_t)c->offset;
    if (need > (uint64_t)size) {
        nb_err("ATTACH: %ux%u stride=%u offset=%u needs %llu bytes but the "
               "dma-buf is %lld — REJECTED (the compositor would read out of "
               "bounds)", c->width, c->height, c->stride, c->offset,
               (unsigned long long)need, (long long)size);
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
    d->fourcc   = c->fourcc;
    d->modifier = c->modifier;
    d->bpp      = bpp;
    return 0;
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

    if (c->reserved0 != 0 || c->reserved1 != 0) {
        if (fd >= 0) {
            close(fd);
        }
        nb_violation(s, "reserved fields are not zero");
        return;
    }

    switch (c->type) {
    case NVKVM_BROKER_CMD_ATTACH:
        if (fd < 0) {
            nb_violation(s, "ATTACH without an SCM_RIGHTS fd");
            return;
        }
        s->n_attach++;
        if (!(ss->caps & NVKVM_BROKER_CAP_DMABUF)) {
            close(fd);
            s->n_reject++;
            nb_err("ATTACH: this session cannot accept dma-buf buffers");
            return;
        }
        if (nb_validate_desc(s, c, fd, &d) != 0) {
            close(fd);
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
                             * is bounded at one in flight by construction */
        if (r != 0) {
            s->n_reject++;
            nb_err("ATTACH: the display refused the buffer: %s", strerror(-r));
        }
        return;

    case NVKVM_BROKER_CMD_COMMIT:
        if (fd >= 0) {
            close(fd);
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

    case NVKVM_BROKER_CMD_WINDOW:
        if (fd >= 0) {
            close(fd);
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
            nb_err("WINDOW: %ux%u out of range — ignored", c->width, c->height);
            return;
        }
        ss->ops->resize(ss, c->width, c->height);
        return;

    default:
        if (fd >= 0) {
            close(fd);
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
void nb_sink_readable(struct nb_sink *s)
{
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
                        close(got);
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
            nb_handle_cmd(s, &c, fd);
        }
    }
}

/* ── listener ────────────────────────────────────────────────────────────── */

static int nb_listen(const char *path)
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
    old = umask(0177);
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
    if (chmod(path, 0600) < 0) {
        nb_err("chmod 0600 %s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        return -errno;
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
    struct passwd *pw;

    /* Do this unconditionally: it costs nothing and it closes ptrace-attach
     * and core-dump reads of a process holding the keyboard grab. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    if (geteuid() != 0) {
        return 0;
    }
    if (!user) {
        nb_log("WARNING: running as root and --drop-user was not given. "
               "The broker will stay root. Pass --drop-user <name>.");
        return 0;
    }
    pw = getpwnam(user);
    if (!pw) {
        nb_err("--drop-user %s: no such user", user);
        return -ENOENT;
    }
    if (pw->pw_uid == 0) {
        nb_err("--drop-user %s resolves to uid 0 — refusing", user);
        return -EINVAL;
    }
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 ||
        setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
        nb_err("dropping to %s: %s", user, strerror(errno));
        return -errno;
    }
    /* setuid(2) is not allowed to fail silently here; prove it took. */
    if (setuid(0) == 0 || geteuid() != pw->pw_uid) {
        nb_err("privilege drop did not take — refusing to continue");
        _exit(1);
    }
    nb_log("dropped privileges to %s (uid %u, gid %u), no capabilities held",
           user, (unsigned)pw->pw_uid, (unsigned)pw->pw_gid);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fputs(
"usage: nvkvm-display-broker --socket PATH [options]\n"
"\n"
"  --socket PATH        unix socket to listen on (created mode 0600)\n"
"  --backend NAME       auto (default) | wayland | x11 | test\n"
"  --size WxH           initial window size (default 1920x1080)\n"
"  --title TEXT         window title\n"
"  --allow-user NAME    additional user allowed to connect (repeatable)\n"
"  --allow-group NAME   additional group allowed to connect (repeatable)\n"
"  --drop-user NAME     become this user after the window is up\n"
"  --verbose\n"
"\n"
"The broker owns the window, the compositor connection and the input grab.\n"
"The VMM keeps only this socket: it relays the guest's scanout dma-buf fd\n"
"here and receives input.  It needs no EGL, no GL and no display server.\n"
"By default root and the invoking user may connect.\n", stderr);
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
    sigset_t mask;

    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = "auto";
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
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { nb_err("unknown argument: %s", a); usage(); return 2; }
#undef NEEDVAL
    }
    if (!cfg.socket_path) {
        usage();
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
    sess = nb_session_open(&cfg);
    if (!sess) {
        return 1;               /* the backend already said why */
    }
    nb_announce(sess);
    if (!(sess->caps & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
        sess->caps &= ~(NVKVM_BROKER_CAP_REL_POINTER |
                        NVKVM_BROKER_CAP_POINTER_LOCK |
                        NVKVM_BROKER_CAP_TOTAL_GRAB);
    }

    /* 2. bind the socket */
    listen_fd = nb_listen(cfg.socket_path);
    if (listen_fd < 0) {
        sess->ops->close(sess);
        return 1;
    }
    nb_log("listening on %s (mode 0600)", cfg.socket_path);
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

    for (;;) {
        int n = 0, nsess, r;

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

        if (poll(pfd, (nfds_t)(n + nsess), -1) < 0) {
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
    }

    if (sink.client_fd >= 0) {
        nb_sink_bye(&sink, NVKVM_BROKER_BYE_SHUTDOWN);
        nb_sink_detach(&sink, "broker shutting down");
    }
    rc = 0;
out:
    close(listen_fd);
    unlink(cfg.socket_path);
    sess->ops->close(sess);
    close(sigfd);
    return rc;
}
