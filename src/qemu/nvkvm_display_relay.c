/*
 * nvkvm_display_relay.c — QEMU display backend "nvkvm-broker".
 *
 *   qemu-system-x86_64 ... -display nvkvm-broker,socket=/run/nvkvm/display.sock
 *
 * WHAT THIS REPLACES
 *
 * Every other way nvkvm can put a guest frame on a screen ends in a host
 * window that QEMU owns: `-display gtk,gl=on` or `-display sdl`, which make
 * QEMU a client of the user's X11 or Wayland session, plus a private EGL
 * context on the NVIDIA render node.  In broker mode NONE of that runs:
 *
 *   - no GTK, no SDL, no X11 or Wayland client library, and no display-server
 *     socket needs to exist inside QEMU's container;
 *   - no egl_init(), no EGLImage import, no readback, no import cache;
 *   - no window, so no window-system input loop.
 *
 * WHAT IT DOES INSTEAD — AND WHY IT IS SO SMALL
 *
 * nvkvm_req_present() already RECEIVES a host dma-buf fd from the isolate's
 * stub over SCM_RIGHTS, every frame: the guest compositor allocated its
 * scanout bo through the forwarded render node, so the real RM object lives in
 * the isolate and the stub PRIME-exports it.  Today QEMU stashes that fd and
 * the consumer hands it to the UI (dpy_gl_scanout_dmabuf) or imports and reads
 * it back.
 *
 * In broker mode the substitution is at exactly that point: RELAY THE SAME FD
 * ONWARD over SCM_RIGHTS instead of consuming it.  QEMU forwards a descriptor
 * it was handed and touches no pixels.  Zero-copy holds end to end — one
 * physical allocation is the guest's scanout bo, the isolate's RM object, a
 * host dma-buf, and (in the broker) a wl_buffer or an X pixmap.
 *
 * THE ASYMMETRY OF THE SOCKET
 *
 * The broker is privileged relative to us: it holds the display-server
 * connection and the input grab, it runs as the user, and it treats this
 * process as hostile.  Everything this file sends is validated over there
 * (geometry against the real dma-buf size, fourcc/modifier against what the
 * GPU advertises, and that the fd is a dma-buf at all).  Nothing here should
 * be written as if those checks were ours to skip — see src/broker/README.md.
 *
 * In the other direction the broker is the TRUSTED side of this socket, so the
 * event handling below is written for robustness, not defence.  It still
 * treats a short or oversized read as fatal rather than resyncing.
 *
 * COMPILE GATE.  CONFIG_LINUX only — deliberately NOT CONFIG_OPENGL.  See
 * nvkvm_display_relay.h for why.
 */
#include "qemu/osdep.h"

#ifdef CONFIG_LINUX

#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input-event-codes.h>

#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "system/runstate.h"    /* qemu_system_powerdown_request.  This was
                                 * sysemu/runstate.h up to QEMU 9.2; upstream
                                 * renamed include/sysemu/ to include/system/
                                 * in 10.0 and include/sysemu/ no longer
                                 * exists at all in the pinned 11.1.1.      */
#include "ui/console.h"
#include "ui/input.h"
#include "ui/clipboard.h"

#include "nvkvm_display_relay.h"
#include "nvkvm_inc/nvkvm_broker_proto.h"

/* Always-visible, like the rest of the present path: these lines are how a
 * user finds out why their screen is black. */
#define RELAY_LOG(fmt, ...) \
    fprintf(stderr, "nvkvm-broker: " fmt "\n", ##__VA_ARGS__)

/* Distinct (fourcc, modifier) pairs whose verdict we remember per connection. */
#define RELAY_FMT_SLOTS 4

#define RELAY_CLIP_MAX_CMDS \
    ((NVKVM_BROKER_CLIP_MAX_BYTES + NVKVM_BROKER_CLIP_CMD_BYTES - 1u) / \
     NVKVM_BROKER_CLIP_CMD_BYTES)

typedef enum RelayConnState {
    RELAY_CONN_DOWN,
    RELAY_CONN_CONNECTING,
    RELAY_CONN_HELLO,
    RELAY_CONN_SYNC_CAPS,
    RELAY_CONN_SYNC_WINDOW,
    RELAY_CONN_SYNC_ATTACH,
    RELAY_CONN_SYNC_COMMIT,
    RELAY_CONN_ACTIVE,
} RelayConnState;

typedef struct NvkvmRelay {
    int         sock;           /* the broker connection, or -1               */
    RelayConnState conn_state;  /* owned by the main loop under the BQL       */
    QemuConsole *con;           /* where input is injected                    */
    uint32_t    caps;           /* from HELLO                                 */

    /* Last geometry relayed, so a guest resolution change can be turned into
     * one CMD_WINDOW instead of a request per frame.  Reset to 0 on a
     * disconnect: a NEW broker has never been told, and eliding the WINDOW
     * because "we already sent it" would be telling the wrong process. */
    uint32_t    last_w, last_h;

    /*
     * BROKER MODE IS ON EVEN WHILE THE SOCKET IS DOWN.
     *
     * `sock < 0` means "not connected right now"; `enabled` means "this QEMU
     * was started with -display nvkvm-broker".  The present path keys off
     * enabled, NOT off sock -- otherwise every frame produced while the broker
     * is restarting falls through to nvkvm_present_submit(), which imports the
     * dma-buf into an EGL context to show it on a display that does not exist.
     * Observed doing exactly that: after killing the broker the log filled with
     * "nvkvm present: import fd=..." at the guest's full frame rate.
     */
    bool        enabled;

    /* Reconnect.  A display process restarting must never be a VM-affecting
     * event, so a lost socket is a transient, not the end. */
    QEMUTimer  *retry;
    QEMUTimer  *handshake_deadline;
    unsigned    retry_ms;
    bool        retry_logged;   /* first failure is loud, the rest are silent */
    bool        attempt_is_retry;
    uint64_t    n_reconnects;

    /*
     * THE LAST FRAME, RETAINED.
     *
     * A reconnected broker's import cache is empty and its window is blank, and
     * nvkvm's present path is GUEST-driven -- frames arrive because the guest
     * flipped, not because anyone asked.  On an idle desktop that can be a very
     * long time, so a reconnect with nothing to show reads as "reconnect does
     * not work".  Holding one dma-buf fd costs one pinned scanout buffer and
     * makes the new window correct immediately.
     */
    /*
     * CLIPBOARD.  The broker becomes an ordinary QEMU clipboard peer -- the
     * same mechanism GTK and SDL already use, with -chardev qemu-vdagent and
     * stock spice-vdagent as the guest side.  Nothing is invented here beyond
     * the two broker messages, and NOTHING is added to the virtio GPU
     * transport: clipboard and GPU forwarding are unrelated concerns with
     * different lifetimes, and coupling them would tie a text channel to the
     * lifecycle of a display.
     */
    QemuClipboardPeer clip_peer;
    bool        clip_registered;
    bool        clip_agent_seen;   /* another peer has published a clipboard */
    char        clip_in[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    unsigned    clip_in_len;
    unsigned    clip_in_chunks;
    bool        clip_in_bad;
    struct nvkvm_broker_clip_cmd clip_out[RELAY_CLIP_MAX_CMDS];
    size_t      clip_out_count;
    size_t      clip_out_next;

    int         last_fd;
    uint32_t    last_bw, last_bh, last_stride, last_fourcc;
    uint64_t    last_modifier;

    /*
     * CAN THE DISPLAY SHOW WHAT THE GUEST RENDERS?
     *
     * On a cross-vendor host it cannot, at any price: the guest's tiling is its
     * GPU's own, and the compositor's GPU has no way to read it.  We cannot see
     * the broker's advertised set and a rejected ATTACH is not reported back,
     * so we ASK -- once per (fourcc, modifier), not per frame.
     *
     * Connection-generation state: cleared on every disconnect, because a
     * reconnected broker may be a different display entirely.  Carrying a
     * verdict across a reconnect is the RR-07 class of bug.
     */
    /*
     * A TABLE, not one slot.  Two pairs are in play at once on a
     * cross-vendor host: what the guest renders (its GPU's tiling) and what
     * the readback produces (XR24 + LINEAR).  With a single slot the second
     * submit evicted the first, so the next guest frame read "unknown", went
     * zero-copy, was rejected, re-queried, and the mode oscillated
     * CANNOT -> CAN -> CANNOT forever.  OBSERVED on the physical box: 4
     * CANNOT and 5 CAN lines for one guest session.
     *
     * Four entries is deliberate headroom over the two that occur; a fifth
     * distinct pair in one connection would mean the guest is changing format
     * repeatedly, which is worth the re-query.
     */
    struct {
        uint32_t fourcc;
        uint64_t mod;
        bool     used;
        int      verdict;       /* -1 asked, awaiting reply; 0 no; 1 yes */
    } fmt[RELAY_FMT_SLOTS];

    /* Partial packet accumulator.  Fixed size: a packet is always exactly
     * NVKVM_BROKER_PKT_SIZE bytes, so nothing here is length-driven. */
    uint8_t     rxbuf[NVKVM_BROKER_PKT_SIZE];
    size_t      rxlen;

    bool        grabbed;
    /* An ACPI powerdown we asked for and the guest has not acted on.  Kept so
     * a second close is answered with something rather than with silence --
     * see the EV_CLOSE case. */
    bool        powerdown_pending;
    time_t      powerdown_at;
    unsigned    powerdown_asks;
    uint64_t    n_sent, n_dropped;
    /* ATTACHed but the COMMIT could not be written.  A distinct failure from
     * n_dropped -- see the comment at the counter's only increment. */
    uint64_t    n_uncommitted;
    /*
     * PRODUCED: every frame the guest handed us, counted at the ingress before
     * any decision is taken about it.  n_sent is what actually reached the
     * broker, so (n_produced - n_sent) is the loss THIS layer is responsible
     * for, and n_dropped + n_uncommitted should account for all of it.
     *
     * Without this the relay could only report its own drops, which cannot
     * distinguish "the guest never sent a frame" from "we threw it away" --
     * exactly the ambiguity a last-frame-drop report turns on.
     */
    uint64_t    n_produced;
    uint64_t    n_stats_at;     /* n_produced when we last logged */
} NvkvmRelay;

static NvkvmRelay *nvkvm_relay;
static char *nvkvm_relay_sock_path;

/*
 * NVKVM_RELAY_FRAME_STATS=N -- log the ingress/egress frame counters every N
 * frames the guest produces.  0 (the default) disables it entirely, so the
 * per-frame cost when unset is one compare against a static.
 *
 * Off by default because this logs at the guest's flip rate: on a busy desktop
 * N=600 is about one line every ten seconds, and N=1 would itself perturb the
 * timing it is measuring.
 */
static uint64_t nvkvm_relay_frame_stats;

static void nvkvm_relay_stats_init(void)
{
    static bool done;
    const char *e;

    if (done) {
        return;
    }
    done = true;
    e = getenv("NVKVM_RELAY_FRAME_STATS");
    if (e && *e) {
        nvkvm_relay_frame_stats = strtoull(e, NULL, 10);
    }
}

bool nvkvm_display_relay_active(void)
{
    /* enabled, not connected: see the `enabled` comment.  While the broker is
     * away we still consume the frame (and keep the newest) rather than let it
     * fall through to the GL import path. */
    return nvkvm_relay && nvkvm_relay->enabled;
}

/* ── sending ─────────────────────────────────────────────────────────────── */

/*
 * One command, optionally carrying one fd.  MSG_DONTWAIT throughout: PRESENT
 * runs inline in the BQL-held virtqueue callback, so a display that has stopped
 * draining must cost a dropped frame, never a stalled VM.
 *
 * Connection and descriptor ownership is deliberately BQL/main-loop-only.
 * Moving PRESENT to a worker therefore requires an explicit handoff to the
 * main loop; it must not make this function a multi-threaded socket owner.
 *
 * Returns 0, -EAGAIN when the socket is full, or another -errno (fatal).
 */
static int relay_send(NvkvmRelay *r, const struct nvkvm_broker_cmd *cmd, int fd)
{
    struct iovec iov = { .iov_base = (void *)cmd,
                         .iov_len  = NVKVM_BROKER_CMD_SIZE };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t n;

    if (fd >= 0) {
        struct cmsghdr *c;

        memset(&u, 0, sizeof(u));
        msg.msg_control = u.buf;
        msg.msg_controllen = sizeof(u.buf);
        c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    do {
        n = sendmsg(r->sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return -errno;
    }
    /*
     * A 40-byte sendmsg into a unix SOCK_STREAM with room is atomic in
     * practice, but a short write would desynchronise the fd from its command
     * on the privileged side — which is exactly the confusion the broker's
     * fixed-size framing exists to prevent.  Fatal, never resynced.
     */
    if (n != (ssize_t)NVKVM_BROKER_CMD_SIZE) {
        return -EPROTO;
    }
    return 0;
}

/*
 * Reconnect backoff.  Starts short so a broker restart is nearly invisible,
 * caps low enough that a person restarting one by hand does not wait for it,
 * and is a backoff rather than a poll so a broker that is down for an hour
 * costs nothing measurable.
 */
#define RELAY_RETRY_MIN_MS   200u
#define RELAY_RETRY_MAX_MS  5000u

static void relay_retry(void *opaque);
static void relay_readable(void *opaque);
static void relay_forget_format_verdict(NvkvmRelay *r);
static void relay_writable(void *opaque);
static void relay_handshake_timeout(void *opaque);
static int  relay_start_connect(NvkvmRelay *r, const char *path, Error **errp);
static void relay_sync_flush(NvkvmRelay *r);
static int  relay_send_caps(NvkvmRelay *r);
static void relay_clip_recv(NvkvmRelay *r, const struct nvkvm_broker_pkt *p);

/* NVKVM_RELAY_STATE_HELPERS_BEGIN -- extracted by test_relay_state. */
/*
 * Everything below is knowledge held about ONE broker connection.  A stream
 * disconnect is both the packet-framing boundary and the clipboard transaction
 * boundary, so none of it may survive into a new broker process.  The retained
 * frame is deliberately absent: it belongs to the VM and is what the next
 * connection must replay.
 */
static void relay_connection_state_reset(NvkvmRelay *r)
{
    r->caps = 0;
    r->last_w = 0;
    r->last_h = 0;
    r->rxlen = 0;
    r->clip_in_len = 0;
    r->clip_in_chunks = 0;
    r->clip_in_bad = false;
    r->clip_out_count = 0;
    r->clip_out_next = 0;
}

/*
 * Consume and retain a frame whenever broker mode is enabled, independently
 * of whether a broker is connected at this instant.  False leaves ownership
 * with the caller; true transfers ownership of dmabuf_fd to r.  Like every
 * connection-state operation, this runs under the BQL; the relay has no
 * worker-side socket owner.
 */
static bool relay_frame_retain(NvkvmRelay *r, int dmabuf_fd,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t fourcc,
                               uint64_t modifier)
{
    if (!r || !r->enabled || dmabuf_fd < 0) {
        return false;
    }
    if (r->last_fd >= 0) {
        close(r->last_fd);
    }
    r->last_fd       = dmabuf_fd;
    r->last_bw       = width;
    r->last_bh       = height;
    r->last_stride   = stride;
    r->last_fourcc   = fourcc;
    r->last_modifier = modifier;
    return true;
}
/* NVKVM_RELAY_STATE_HELPERS_END */

/* NVKVM_RELAY_FD_OWNERSHIP_BEGIN -- extracted by test_relay_wiring. */
static void relay_set_fd_handlers(NvkvmRelay *r, bool writable)
{
    assert(bql_locked());
    assert(r->sock >= 0);
    qemu_set_fd_handler(r->sock, relay_readable,
                        writable ? relay_writable : NULL, r);
}

/*
 * The main loop owns registration and closure.  Removing the handler before
 * close means there is no deferred raw descriptor that can be reused between
 * scheduling and teardown.  aio_set_fd_handler(), which backs this API, is
 * explicitly safe when called while the handler list is being walked.
 */
static void relay_close_connection(NvkvmRelay *r)
{
    int fd;

    assert(bql_locked());
    if (r->handshake_deadline) {
        timer_del(r->handshake_deadline);
    }
    if (r->sock < 0) {
        r->conn_state = RELAY_CONN_DOWN;
        relay_connection_state_reset(r);
        return;
    }
    fd = r->sock;
    r->sock = -1;
    r->conn_state = RELAY_CONN_DOWN;
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
    relay_connection_state_reset(r);
}
/* NVKVM_RELAY_FD_OWNERSHIP_END */

static void relay_schedule_retry(NvkvmRelay *r)
{
    assert(bql_locked());
    timer_mod(r->retry,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + r->retry_ms);
}

/*
 * Ask the broker whether it can display this (fourcc, modifier).
 *
 * Sent once per distinct pair.  While the answer is outstanding the caller
 * keeps submitting zero-copy: that is the right optimism, because the pair the
 * guest flips is displayable on every same-vendor host, and being wrong costs
 * one round trip's worth of dropped frames rather than a wrong permanent mode.
 */
static void relay_query_format(NvkvmRelay *r, uint32_t fourcc, uint64_t mod)
{
    struct nvkvm_broker_cmd cmd;
    unsigned i, slot = RELAY_FMT_SLOTS;

    for (i = 0; i < RELAY_FMT_SLOTS; i++) {
        if (r->fmt[i].used && r->fmt[i].fourcc == fourcc &&
            r->fmt[i].mod == mod) {
            return;                     /* already asked about this pair */
        }
        if (!r->fmt[i].used && slot == RELAY_FMT_SLOTS) {
            slot = i;
        }
    }
    if (slot == RELAY_FMT_SLOTS) {
        slot = 0;                       /* full: recycle the oldest */
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = NVKVM_BROKER_CMD_QUERY_FORMAT;
    cmd.fourcc   = fourcc;
    cmd.modifier = mod;

    r->fmt[slot].fourcc  = fourcc;
    r->fmt[slot].mod     = mod;
    r->fmt[slot].used    = true;
    r->fmt[slot].verdict = -1;

    if (relay_send(r, &cmd, -1) != 0) {
        /* Not fatal and not retried here: freeing the slot makes the next
         * frame ask again, which is the whole recovery needed. */
        r->fmt[slot].used = false;
    }
}

static void relay_drop(NvkvmRelay *r, const char *why)
{
    assert(bql_locked());
    if (r->sock < 0) {
        return;
    }
    relay_close_connection(r);
    relay_forget_format_verdict(r);
    error_report("nvkvm-broker: %s; the display and input are gone for now "
                 "(%" PRIu64 " frames relayed, %" PRIu64 " dropped, "
                 "%" PRIu64 " attached without a commit). "
                 "The VM keeps running; reconnecting in the background.",
                 why, r->n_sent, r->n_dropped, r->n_uncommitted);
    r->retry_ms = RELAY_RETRY_MIN_MS;
    r->retry_logged = false;
    relay_schedule_retry(r);
}

/*
 * Connection-generation reset for the format verdict.  A reconnected broker may
 * be a completely different display -- a different compositor, a different GPU
 * -- so an answer from the previous connection says nothing about this one.
 * Carrying it across is the RR-07 class of bug (reconnect inheriting partial
 * protocol state), and here it would silently pin the wrong present path.
 */
static void relay_forget_format_verdict(NvkvmRelay *r)
{
    memset(r->fmt, 0, sizeof(r->fmt));
}

static void relay_attempt_failed(NvkvmRelay *r, const char *why)
{
    assert(bql_locked());
    relay_close_connection(r);
    relay_forget_format_verdict(r);
    if (!r->retry_logged) {
        error_report("nvkvm-broker: connection attempt failed: %s", why);
        RELAY_LOG("retrying in the background (up to every %ums); the VM "
                  "continues running", RELAY_RETRY_MAX_MS);
        r->retry_logged = true;
    }
    r->retry_ms = r->retry_ms * 2 > RELAY_RETRY_MAX_MS
                      ? RELAY_RETRY_MAX_MS : r->retry_ms * 2;
    relay_schedule_retry(r);
}

/*
 * -1 not answered yet, 0 the display cannot show this pair, 1 it can.
 *
 * The caller uses 0 to switch to the readback path.  While the answer is
 * outstanding it should keep going zero-copy: on every same-vendor host the
 * answer is yes, and being briefly wrong costs a few dropped frames instead of
 * a permanently wrong mode.
 */
int nvkvm_display_relay_format_verdict(uint32_t fourcc, uint64_t modifier)
{
    NvkvmRelay *r = nvkvm_relay;
    unsigned i;

    if (!r) {
        return -1;
    }
    for (i = 0; i < RELAY_FMT_SLOTS; i++) {
        if (r->fmt[i].used && r->fmt[i].fourcc == fourcc &&
            r->fmt[i].mod == modifier) {
            return r->fmt[i].verdict;
        }
    }
    return -1;
}

bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier)
{
    return nvkvm_display_relay_submit_flags(nv, dmabuf_fd, width, height,
                                            stride, fourcc, modifier, false);
}

/*
 * `shm` declares that the fd is a memfd to be shown through wl_shm rather than
 * imported as a dma-buf.  Declared, not inferred: the broker enforces that the
 * fd really is a memfd, but it must not have to GUESS what the VMM meant.
 */
bool nvkvm_display_relay_submit_flags(struct VirtIONvgpu *nv, int dmabuf_fd,
                                      uint32_t width, uint32_t height,
                                      uint32_t stride, uint32_t fourcc,
                                      uint64_t modifier, bool shm)
{
    NvkvmRelay *r = nvkvm_relay;
    struct nvkvm_broker_cmd cmd;
    uint32_t cmd_seq = 0;
    int rc;

    if (!r) {
        return false;
    }
    /* See the ownership contract in nvkvm_display_relay.h.  This assertion is
     * load-bearing: a future worker offload must marshal the whole operation to
     * the main loop instead of creating a second socket owner. */
    assert(bql_locked());

    /*
     * Count it as PRODUCED before anything can reject it, so the ingress total
     * is unconditional.  Every early return below is a frame the guest made
     * and the host never showed.
     */
    r->n_produced++;
    if (nvkvm_relay_frame_stats &&
        r->n_produced - r->n_stats_at >= nvkvm_relay_frame_stats) {
        r->n_stats_at = r->n_produced;
        info_report("nvkvm-relay frames: produced=%" PRIu64 " forwarded=%"
                    PRIu64 " dropped=%" PRIu64 " uncommitted=%" PRIu64
                    " lost=%" PRIu64,
                    r->n_produced, r->n_sent, r->n_dropped, r->n_uncommitted,
                    r->n_produced - r->n_sent);
    }

    /*
     * Ask -- once per pair -- whether the display can show this at all.  Done
     * here rather than at mode-set because this is where the actual (fourcc,
     * modifier) the guest flips first becomes known; a mode-set only tells us
     * the geometry.  The helper returns immediately once the pair has been
     * asked about, so this is not per-frame work.
     */
    if (r->sock >= 0) {
        relay_query_format(r, fourcc, modifier);
    }

    /*
     * RETAIN THE NEWEST, DROP THE REST.  This runs whether or not the socket is
     * up: while the broker is away the frames still arrive (the guest does not
     * know), and keeping the latest is what lets a reconnect paint immediately.
     * Replacing rather than queueing is deliberate -- a backlog of stale frames
     * delivered on reconnect is worse than a brief blank, and unbounded.
     */
    if (!relay_frame_retain(r, dmabuf_fd, width, height, stride, fourcc,
                            modifier)) {
        return false;
    }

    if (r->conn_state == RELAY_CONN_SYNC_COMMIT) {
        /* ATTACH for the previous retained frame is already on the stream.
         * Closing and replaying is the only way to avoid committing that stale
         * prefix while claiming the newest frame was retained. */
        relay_attempt_failed(r,
            "a newer frame replaced one attached during connection replay");
        return true;
    }
    if (r->conn_state == RELAY_CONN_SYNC_ATTACH ||
        r->conn_state == RELAY_CONN_SYNC_CAPS) {
        /* ATTACH/CAPS has not been accepted yet (otherwise the state already
         * advanced), so replay the newly retained metadata and frame before
         * finishing the handshake. */
        r->conn_state = RELAY_CONN_SYNC_WINDOW;
        relay_sync_flush(r);
        return true;
    }
    if (r->conn_state != RELAY_CONN_ACTIVE) {
        /* Down or still handshaking: consumed and remembered, nothing to send.
         * Returning true keeps the caller from falling through to GL import. */
        return true;
    }

    /*
     * Guest resolution change → ask for a window this size.  Advisory: the
     * broker clamps it and the window manager may ignore it, and the size that
     * actually took effect comes back as EV_SURFACE.  Sent before the buffer
     * so the window is already the right shape when the frame lands.
     */
    if (width != r->last_w || height != r->last_h) {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = NVKVM_BROKER_CMD_WINDOW;
        cmd.width = width;
        cmd.height = height;
        if (relay_send(r, &cmd, -1) == 0) {
            r->last_w = width;
            r->last_h = height;
            RELAY_LOG("guest resolution is now %ux%u", width, height);
        }
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = NVKVM_BROKER_CMD_ATTACH;
    cmd.flags = shm ? NVKVM_BROKER_CMD_F_SHM : 0;
    cmd.width = width;
    cmd.height = height;
    cmd.stride = stride;
    cmd.offset = 0;         /* the stub exports plane 0 at offset 0 */
    cmd.fourcc = fourcc;
    cmd.modifier = modifier;
    cmd.seq = (uint32_t)r->n_sent;
    cmd_seq = cmd.seq;
    rc = relay_send(r, &cmd, dmabuf_fd);
    if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
        /*
         * The broker has not drained.  Dropping is correct: the guest has
         * produced faster than the display can consume, and the next flip
         * carries a newer buffer.  Counted, not silent.
         */
        r->n_dropped++;
        return true;        /* we consumed the fd; do not fall through */
    }
    if (rc != 0) {
        relay_drop(r, rc == -EPROTO ? "short write on the broker socket"
                                    : "the broker socket failed");
        return true;
    }
    /*
     * NOT CLOSED HERE ANY MORE.  The broker has its own copy from SCM_RIGHTS,
     * but this process keeps the fd too: it is r->last_fd, the frame a
     * reconnecting broker will be shown.  It is closed when the next frame
     * replaces it, or at teardown.
     */

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = NVKVM_BROKER_CMD_COMMIT;
    rc = relay_send(r, &cmd, -1);
    if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
        /*
         * ATTACH went out and its COMMIT did not.  This is NOT the same as
         * dropping the frame before ATTACH: over there the broker never heard
         * about the buffer, whereas here it has imported it, staged it as
         * `pending`, and will go on showing the PREVIOUS frame until the next
         * COMMIT arrives.  So the screen holds a stale frame for one interval
         * and the buffer is pinned out of the broker's LRU meanwhile.
         *
         * It self-corrects on the next flip, which is why this is a counter
         * and not a disconnect -- but it was previously swallowed entirely,
         * counted nowhere and logged nowhere, which made exactly this
         * "occasional stale frame" invisible to anyone looking for it.
         */
        r->n_uncommitted++;
        if (r->n_uncommitted <= 4 || (r->n_uncommitted % 256) == 0) {
            RELAY_LOG("the broker did not drain: frame %u attached but not "
                      "committed, so the display holds the previous frame "
                      "(%" PRIu64 " so far)",
                      cmd_seq, r->n_uncommitted);
        }
    } else if (rc != 0) {
        relay_drop(r, "the broker socket failed on commit");
    }
    r->n_sent++;
    return true;
}

/* ── receiving: input and pacing ─────────────────────────────────────────── */

static InputButton relay_btn(unsigned code)
{
    switch (code) {
    case BTN_LEFT:   return INPUT_BUTTON_LEFT;
    case BTN_RIGHT:  return INPUT_BUTTON_RIGHT;
    case BTN_MIDDLE: return INPUT_BUTTON_MIDDLE;
    case BTN_SIDE:   return INPUT_BUTTON_SIDE;
    case BTN_EXTRA:  return INPUT_BUTTON_EXTRA;
    default:         return INPUT_BUTTON__MAX;
    }
}

/*
 * On grab, put a RELATIVE pointing device in front; on ungrab, the absolute
 * one.  Same mechanism, and the same caveat, as patch
 * 0007-gtk-grab-switches-the-guest-pointing-device: QEMU keeps one current
 * mouse handler and a tablet only ever reports absolute positions, so without
 * this a guest doing pointer-lock reads nothing under grab.
 *
 * *** THIS IS NOT KNOWN TO WORK.  The equivalent switch was performed by hand
 * *** on real hardware for the GTK path and mouse-look in the guest still did
 * *** not move; the remaining suspects are guest-side.  See patch 0007's
 * *** commit message before trusting it.
 */
static void relay_set_relative(bool relative)
{
    MouseInfoList *mice = qmp_query_mice(NULL), *e;
    MouseInfo *pick = NULL;
    bool found = false;

    /*
     * PICK DETERMINISTICALLY, AND PREFER VIRTIO.
     *
     * Taking the first device with the right absolute-ness is not stable:
     * qemu_mouse_set() moves the chosen handler to the head of the list
     * (ui/input.c), so the NEXT query sees a different order and the next
     * grab picks a different device.  A guest with virtio-tablet,
     * virtio-mouse, an emulated PS/2 mouse and vmmouse has two candidates in
     * each direction, and the grab lands on a different one each time.
     *
     * Observed on hardware 2026-08-24: grab worked roughly every other
     * attempt, the failures being the ones that selected "QEMU PS/2 Mouse" or
     * "vmmouse" -- devices the guest's desktop is not reading -- so the
     * pointer simply froze until the user ungrabbed and tried again.  It read
     * as a flaky grab and was really a coin flip over four devices.
     *
     * The virtio devices are the ones the guest driver actually uses, and
     * they are the ones this harness attaches on purpose, so prefer them by
     * name and fall back to the first match only if there is no virtio
     * candidate at all.
     */
    for (e = mice; e; e = e->next) {
        if (e->value->absolute == relative) {
            continue;                   /* wrong kind */
        }
        if (!pick) {
            pick = e->value;            /* fallback: first of the right kind */
        }
        if (e->value->name && strstr(e->value->name, "Virtio")) {
            pick = e->value;
            break;
        }
    }
    if (pick) {
        qemu_mouse_set((int)pick->index, NULL);
        RELAY_LOG("pointing device -> #%d %s (%s)",
                  (int)pick->index, pick->name,
                  relative ? "relative" : "absolute");
        found = true;
    }
    /*
     * SAY SO when there is nothing to switch to.  A guest configured with only
     * virtio-tablet (the default: see VM_RELATIVE_MOUSE in
     * scripts/run_test_vm.sh) has no relative device at all, so a grab
     * suppresses absolute motion in the broker and the relative motion it
     * sends instead lands on nothing -- the pointer simply stops until the
     * user ungrabs.  Observed on hardware 2026-08-24.  It looks like a broken
     * grab and is really a missing device, and the difference was invisible
     * because this function failed silently.
     */
    if (!found) {
        RELAY_LOG("NO %s pointing device exists -- the guest was started "
                  "without one, so %s.  Add -device virtio-mouse-pci "
                  "(VM_RELATIVE_MOUSE=1 in scripts/run_test_vm.sh) if you "
                  "want mouse-look under grab.",
                  relative ? "relative" : "absolute",
                  relative ? "pointer motion goes nowhere while grabbed"
                           : "the pointer cannot be put back on ungrab");
    }
    qapi_free_MouseInfoList(mice);
}

static void relay_handle(NvkvmRelay *r, const struct nvkvm_broker_pkt *p)
{
    QemuConsole *con = r->con;

    switch (p->type) {
    case NVKVM_BROKER_EV_KEY: {
        /*
         * The broker's wire format is a Linux evdev keycode, and since QEMU
         * 11.1 that is also what a QemuInputEvent carries natively -- the
         * QKeyCode-based sender (qemu_input_event_send_key_qcode) is gone and
         * qemu_input_event_send_key_linux() takes the evdev code directly.  So
         * the conversion this used to do is no longer needed on the way IN;
         * the map lookup is kept purely as the validity filter it always
         * doubled as, so a key QEMU has no mapping for is still dropped rather
         * than forwarded as a code the guest cannot interpret.
         */
        unsigned code = (unsigned)p->x;

        if (code < qemu_input_map_linux_to_qcode_len &&
            qemu_input_map_linux_to_qcode[code] != Q_KEY_CODE_UNMAPPED) {
            qemu_input_event_send_key_linux(con, code, p->y != 0);
        }
        break;
    }
    case NVKVM_BROKER_EV_BTN: {
        InputButton btn = relay_btn((unsigned)p->x);

        if (btn != INPUT_BUTTON__MAX) {
            qemu_input_queue_btn(con, btn, p->y != 0);
            qemu_input_event_sync();
        }
        break;
    }
    case NVKVM_BROKER_EV_ABS:
        if (p->w0 && p->w1) {
            qemu_input_queue_abs(con, INPUT_AXIS_X, p->x, 0, (int)p->w0);
            qemu_input_queue_abs(con, INPUT_AXIS_Y, p->y, 0, (int)p->w1);
            qemu_input_event_sync();
        }
        break;
    case NVKVM_BROKER_EV_REL:
        qemu_input_queue_rel(con, INPUT_AXIS_X, p->x);
        qemu_input_queue_rel(con, INPUT_AXIS_Y, p->y);
        qemu_input_event_sync();
        break;
    case NVKVM_BROKER_EV_WHEEL:
        if (p->x) {
            InputButton bt = p->x > 0 ? INPUT_BUTTON_WHEEL_UP
                                      : INPUT_BUTTON_WHEEL_DOWN;
            qemu_input_queue_btn(con, bt, true);
            qemu_input_event_sync();
            qemu_input_queue_btn(con, bt, false);
            qemu_input_event_sync();
        }
        break;
    case NVKVM_BROKER_EV_GRAB:
        r->grabbed = (p->x != 0);
        RELAY_LOG("grab %s", r->grabbed ? "ON" : "off");
        relay_set_relative(r->grabbed);
        break;
    case NVKVM_BROKER_EV_FOCUS:
        /* The broker already released every held key before telling us, so
         * there is nothing to clean up here.  Logged because a user chasing
         * "my keyboard stopped working" wants to see this line. */
        RELAY_LOG("window %s", p->x ? "active" : "inactive (input suspended)");
        break;
    case NVKVM_BROKER_EV_SURFACE:
        RELAY_LOG("broker window is now %dx%d", p->x, p->y);
        /*
         * FORWARD EVERY HINT.  The broker decides what to suggest; this side
         * carries it.
         *
         * This used to fire only when the fullscreen flag was set, on the rule
         * that "a windowed resize must not reach the guest at all -- the host
         * scales the buffer it already has".  Two things were wrong with that.
         *
         * It stranded the guest on EXIT: leaving fullscreen clears the flag, so
         * the hint that would have put the guest back to the windowed size was
         * the one hint guaranteed to be discarded.  OBSERVED: the window
         * returned to 2192x1237 while the guest stayed at the monitor's
         * resolution, with no way back short of a re-mode from inside.
         *
         * And it put the policy in the wrong process.  Whether a windowed
         * resize should re-mode the guest is exactly what --resolution answers,
         * and the broker is where that flag lives; a second, hidden rule here
         * could only disagree with it.  `--resolution none` is how someone asks
         * for the old behaviour, and it now means it everywhere.
         *
         * `delay` stays true, and matters more now than it did: dragging a
         * window edge produces a configure per motion event, and QEMU's timer
         * coalescing is what turns that into one mode switch when the drag
         * settles rather than a hundred during it.
         */
        /* 11.1: dpy_ui_info_supported/dpy_get_ui_info/dpy_set_ui_info were
         * renamed qemu_console_ui_info_supported/_get_ui_info/_set_ui_info,
         * and _get_ui_info now returns a CONST pointer -- the copy below was
         * already a copy, so that costs nothing. */
        if (con && p->x > 0 && p->y > 0 && qemu_console_ui_info_supported(con)) {
            QemuUIInfo info = *qemu_console_get_ui_info(con);

            info.width  = (uint32_t)p->x;
            info.height = (uint32_t)p->y;
            /*
             * w0 is the host output's refresh in millihertz, which is also
             * QemuUIInfo's unit, so it passes straight through.  0 means the
             * broker does not know -- an X11 session, or a Wayland one before
             * its first presentation feedback -- and then the field is left
             * alone rather than zeroed, so the guest keeps whatever it had.
             */
            if (p->w0 > 0) {
                info.refresh_rate = p->w0;
            }
            qemu_console_set_ui_info(con, &info, true);
        }
        break;
    case NVKVM_BROKER_EV_FRAME:
    case NVKVM_BROKER_EV_RELEASE:
        /*
         * Pacing and buffer-release.  Nothing to do here: nvkvm's present path
         * is GUEST-driven — a frame reaches us because the guest flipped it,
         * not because QEMU asked for one — and the guest recycles its own
         * scanout bos.  They are carried on the wire so a future consumer
         * (a paced capture path, or an fd cache keyed on the released buffer)
         * has them without a protocol change.
         */
        break;
    case NVKVM_BROKER_EV_FORMAT: {
        /*
         * The question is echoed back, so the reply is matched to its slot by
         * (fourcc, modifier) rather than by arrival order.  A reply for a pair
         * we are no longer tracking is discarded, not latched: the guest can
         * change format between query and answer, and adopting a stale verdict
         * would pin the wrong present path.
         */
        uint64_t mod = (uint64_t)p->w0 | ((uint64_t)p->w1 << 32);
        uint32_t fourcc = (uint32_t)p->y;
        unsigned i;

        for (i = 0; i < RELAY_FMT_SLOTS; i++) {
            if (r->fmt[i].used && r->fmt[i].fourcc == fourcc &&
                r->fmt[i].mod == mod) {
                break;
            }
        }
        if (i == RELAY_FMT_SLOTS) {
            RELAY_LOG("stale EV_FORMAT for fourcc 0x%x modifier 0x%llx, "
                      "ignored", (unsigned)fourcc, (unsigned long long)mod);
            break;
        }
        if (r->fmt[i].verdict != -1) {
            break;                      /* already answered; nothing new */
        }
        r->fmt[i].verdict = p->x ? 1 : 0;
        info_report("nvkvm-broker: the display %s show fourcc 0x%08x "
                    "modifier 0x%016llx%s",
                    r->fmt[i].verdict ? "CAN" : "CANNOT",
                    (unsigned)fourcc, (unsigned long long)mod,
                    r->fmt[i].verdict ? "" :
                    " — frames must be read back through the guest's GPU into "
                    "a LINEAR buffer the display can import");
        break;
    }

    case NVKVM_BROKER_EV_POINTER:
    case NVKVM_BROKER_EV_HELLO:
        break;
    case NVKVM_BROKER_EV_CLIPBOARD:
        relay_clip_recv(r, p);
        break;
    case NVKVM_BROKER_EV_CLOSE:
        /*
         * The user closed the display.  THE POLICY IS OURS, and this is the
         * only place in the system that has any business having one: the
         * broker knows there is a window, we are the only party that knows
         * there is a guest behind it.
         *
         * An ACPI powerdown, i.e. the same thing as pressing the power button
         * on the case -- the guest's own OS decides whether to shut down, ask
         * the user, or ignore it.  Not a `quit`: closing a window must not
         * destroy a running machine's state without the guest getting a say.
         * A guest that ignores ACPI keeps running with no display, and the
         * broker is still there to reconnect to.
         */
        if (p->x == NVKVM_BROKER_CLOSE_FORCE) {
            /*
             * The user explicitly chose to stop the machine, having been
             * offered the graceful option and declined it.  SHUTDOWN_CAUSE_
             * HOST_UI is exactly what a UI close button reports elsewhere in
             * QEMU, so this behaves like every other front-end's close.
             */
            RELAY_LOG("the user chose to force the VM off%s",
                      r->powerdown_pending
                          ? " after the guest ignored a powerdown" : "");
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        } else {
            /*
             * The same thing as the power button on the case: the guest's own
             * OS decides whether to shut down, prompt, or ignore it.
             *
             * AND IT MAY IGNORE IT -- hung, or a modal dialog blocking
             * shutdown.  Then the window just sits there, and a user clicking
             * close again gets no sign that anything happened at all, which is
             * a dead end in the most finished-looking part of the UI.  So a
             * repeat ask is answered explicitly, and it is answered HERE
             * because escalation is VM policy and the broker must not grow
             * any: all it did was tell us the user asked again.
             *
             * It does NOT force on the second click.  Destroying a running
             * machine on a double click is a worse failure than the one being
             * fixed, and the user already has an explicit "force off" in front
             * of them -- they need to be told it is the way out, not to have
             * it happen to them.
             */
            time_t now = time(NULL);

            r->powerdown_asks++;
            if (r->powerdown_pending) {
                RELAY_LOG("the guest has NOT responded to the powerdown "
                          "requested %llds ago (asked %u times). It may be "
                          "hung, or showing a dialog that blocks shutdown. "
                          "Choose FORCE OFF THE VM to stop it anyway.",
                          (long long)(now - r->powerdown_at),
                          r->powerdown_asks);
            } else {
                r->powerdown_pending = true;
                r->powerdown_at = now;
                RELAY_LOG("the user closed the display: requesting an ACPI "
                          "powerdown (the guest decides what to do with it)");
            }
            qemu_system_powerdown_request();
        }
        break;
    case NVKVM_BROKER_EV_BYE:
        RELAY_LOG("the broker is going away (reason %d)", p->x);
        break;
    default:
        /* An unknown type from a NEWER broker.  Packets are fixed size, so
         * skipping one is exact and costs nothing. */
        break;
    }
}

static void relay_readable(void *opaque)
{
    NvkvmRelay *r = opaque;

    assert(bql_locked());
    if (r->conn_state == RELAY_CONN_CONNECTING) {
        relay_writable(r);
        if (r->conn_state == RELAY_CONN_CONNECTING || r->sock < 0) {
            return;
        }
    }
    for (;;) {
        ssize_t n;

        if (r->sock < 0) {
            return;
        }
        n = recv(r->sock, r->rxbuf + r->rxlen,
                 NVKVM_BROKER_PKT_SIZE - r->rxlen, MSG_DONTWAIT);
        if (n == 0) {
            if (r->conn_state == RELAY_CONN_ACTIVE) {
                relay_drop(r, "the display broker closed the connection");
            } else {
                relay_attempt_failed(r,
                    "the display broker closed during the handshake");
            }
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (r->conn_state == RELAY_CONN_ACTIVE) {
                relay_drop(r, "read error on the broker socket");
            } else {
                relay_attempt_failed(r,
                    "read error during the broker handshake");
            }
            return;
        }
        r->rxlen += (size_t)n;
        if (r->rxlen == NVKVM_BROKER_PKT_SIZE) {
            struct nvkvm_broker_pkt pkt;

            memcpy(&pkt, r->rxbuf, sizeof(pkt));
            r->rxlen = 0;
            if (r->conn_state == RELAY_CONN_HELLO) {
                if (pkt.type != NVKVM_BROKER_EV_HELLO) {
                    relay_attempt_failed(r,
                        "the first broker packet was not HELLO");
                    return;
                }
                if (pkt.w0 != NVKVM_BROKER_PROTO_VERSION) {
                    relay_attempt_failed(r,
                        "the broker reported an incompatible protocol version");
                    return;
                }
                if (!(pkt.w1 & NVKVM_BROKER_CAP_DMABUF)) {
                    relay_attempt_failed(r,
                        "the broker cannot accept dma-buf buffers");
                    return;
                }
                r->caps = pkt.w1;
                RELAY_LOG("connected to %s, capabilities 0x%x",
                          nvkvm_relay_sock_path, pkt.w1);
                if (!(pkt.w1 & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
                    RELAY_LOG("NOTE: the broker cannot observe focus loss on "
                              "this session, so it will not offer a keyboard "
                              "grab. Absolute pointer and keyboard still work "
                              "while the window is active.");
                }
                r->conn_state = RELAY_CONN_SYNC_WINDOW;
                relay_sync_flush(r);
                if (r->sock < 0) {
                    return;
                }
            } else {
                relay_handle(r, &pkt);
            }
        }
    }
}

/* ── connection ──────────────────────────────────────────────────────────── */

/* NVKVM_RELAY_CONNECT_WIRING_BEGIN -- extracted by test_relay_wiring. */
static int relay_start_connect(NvkvmRelay *r, const char *path, Error **errp)
{
    struct sockaddr_un sa;
    bool connecting = false;
    int fd;

    assert(bql_locked());
    assert(r->sock < 0 && r->conn_state == RELAY_CONN_DOWN);
    if (strlen(path) >= sizeof(sa.sun_path)) {
        error_setg(errp, "nvkvm-broker: socket path too long");
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "nvkvm-broker: socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN) {
            connecting = true;
        } else {
            error_setg_errno(errp, errno,
                             "nvkvm-broker: cannot connect to %s. Is the "
                             "display broker running, and is the socket "
                             "bind-mounted into this container?", path);
            close(fd);
            return -1;
        }
    }

    /*
     * No connect(), HELLO read, CAPS send, or replay operation may wait under
     * the BQL.  Readiness advances this state machine; the timer bounds a peer
     * that accepts and then stops at any handshake phase.
     */
    relay_connection_state_reset(r);
    r->sock = fd;
    r->conn_state = connecting ? RELAY_CONN_CONNECTING : RELAY_CONN_HELLO;
    qemu_set_fd_handler(fd,
                        connecting ? relay_writable : relay_readable,
                        connecting ? relay_writable : NULL, r);
    timer_mod(r->handshake_deadline,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    return 0;
}
/* NVKVM_RELAY_CONNECT_WIRING_END */

/* ── clipboard ───────────────────────────────────────────────────────────── */

/*
 * Tell the broker what we can do.  Re-sent on every (re)connect, because a
 * restarted broker knows nothing -- the same rule as the geometry replay.
 *
 * The clipboard bit is what lets the broker tell "clipboard is off" apart from
 * "there is no guest agent"; without it both look like nothing happening and
 * the user is left to guess which knob to turn.
 */
static int relay_send_caps(NvkvmRelay *r)
{
    struct nvkvm_broker_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type  = NVKVM_BROKER_CMD_CAPS;
    /*
     * The bit means A GUEST AGENT EXISTS, not "we registered a peer" -- we
     * always register one, so reporting that would make the flag always true
     * and destroy the distinction it exists for ("clipboard is off" vs "there
     * is no vdagent").  It is only set once another peer has actually
     * published a clipboard, which is the first moment vdagent proves it is
     * there; CAPS is re-sent at that point.
     */
    cmd.width = r->clip_agent_seen ? NVKVM_BROKER_CLIENT_CLIPBOARD : 0;
    return relay_send(r, &cmd, -1);
}

/* NVKVM_RELAY_CLIP_BATCH_BEGIN -- extracted by test_relay_clip. */
/*
 * Build one complete clipboard transaction in memory before writing any of
 * it.  Separate nonblocking sends can accept a prefix and then return EAGAIN;
 * abandoning that prefix makes the next transfer's LAST commit concatenated
 * text.  The bounded batch and its cursor stay live until POLLOUT drains the
 * remainder, and no newer clipboard transaction begins in between.
 */
static size_t relay_clip_batch_build(struct nvkvm_broker_clip_cmd *out,
                                     size_t out_count,
                                     const char *text, size_t len)
{
    size_t ncmds, off = 0;
    uint32_t chunk = 0;

    if (!out || !text || !len || len > NVKVM_BROKER_CLIP_MAX_BYTES) {
        return 0;
    }
    ncmds = (len + NVKVM_BROKER_CLIP_CMD_BYTES - 1) /
            NVKVM_BROKER_CLIP_CMD_BYTES;
    if (out_count < ncmds) {
        return 0;
    }

    memset(out, 0, ncmds * sizeof(*out));
    while (off < len) {
        struct nvkvm_broker_clip_cmd *cc = &out[chunk];
        size_t n = len - off;

        if (n > NVKVM_BROKER_CLIP_CMD_BYTES) {
            n = NVKVM_BROKER_CLIP_CMD_BYTES;
        }
        cc->type = NVKVM_BROKER_CMD_CLIPBOARD;
        cc->chunk = chunk++;
        cc->info = (uint8_t)n;
        if (off + n == len) {
            cc->info |= NVKVM_BROKER_CLIP_LAST;
        }
        memcpy(cc->data, text + off, n);
        off += n;
    }
    return ncmds;
}

typedef int (*RelayClipSendOne)(void *opaque,
                                const struct nvkvm_broker_clip_cmd *cmd);

/*
 * Drain whole 40-byte commands from an already-built transaction.  EAGAIN
 * leaves `next` at the first unsent chunk, so POLLOUT resumes the SAME
 * transaction; a later clipboard update cannot append or interleave chunks.
 */
static int relay_clip_batch_flush(const struct nvkvm_broker_clip_cmd *cmds,
                                  size_t ncmds, size_t *next,
                                  RelayClipSendOne send_one, void *opaque)
{
    int rc;

    if (!cmds || !next || !send_one || *next > ncmds) {
        return -EINVAL;
    }
    while (*next < ncmds) {
        rc = send_one(opaque, &cmds[*next]);
        if (rc != 0) {
            return rc;
        }
        (*next)++;
    }
    return 0;
}
/* NVKVM_RELAY_CLIP_BATCH_END */

/* ── clipboard ───────────────────────────────────────────────────────────── */

/* NVKVM_RELAY_CLIP_WIRING_BEGIN -- extracted by test_relay_wiring. */
static int relay_clip_send_one(void *opaque,
                               const struct nvkvm_broker_clip_cmd *cmd)
{
    NvkvmRelay *r = opaque;

    return relay_send(r, (const struct nvkvm_broker_cmd *)cmd, -1);
}

static void relay_clip_flush(NvkvmRelay *r)
{
    int rc;

    assert(bql_locked());
    if (r->conn_state != RELAY_CONN_ACTIVE) {
        return;
    }
    if (r->clip_out_next >= r->clip_out_count) {
        r->clip_out_count = 0;
        r->clip_out_next = 0;
        relay_set_fd_handlers(r, false);
        return;
    }
    rc = relay_clip_batch_flush(r->clip_out, r->clip_out_count,
                                &r->clip_out_next, relay_clip_send_one, r);
    if (rc == 0) {
        r->clip_out_count = 0;
        r->clip_out_next = 0;
        relay_set_fd_handlers(r, false);
    } else if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
        relay_set_fd_handlers(r, true);
    } else {
        relay_drop(r, rc == -EPROTO
                          ? "short write in a clipboard command"
                          : "the broker socket failed during clipboard send");
    }
}
/* NVKVM_RELAY_CLIP_WIRING_END */

/* Guest -> host: queue one complete transaction, then drain it in order. */
static void relay_clip_send(NvkvmRelay *r, const char *text, size_t len)
{
    size_t built;

    if (r->conn_state != RELAY_CONN_ACTIVE || len == 0) {
        return;
    }
    if (len > NVKVM_BROKER_CLIP_MAX_BYTES) {
        RELAY_LOG("clipboard: the guest offered %zu bytes, over the %u cap; "
                  "not forwarding it", len, NVKVM_BROKER_CLIP_MAX_BYTES);
        return;
    }
    if (r->clip_out_next < r->clip_out_count) {
        /* The bounded older transaction already owns the stream.  Beginning
         * another one would interleave chunk indices and poison both. */
        RELAY_LOG("clipboard: a previous guest clipboard transaction is "
                  "still queued; dropped the newer update before it began");
        return;
    }
    built = relay_clip_batch_build(r->clip_out, RELAY_CLIP_MAX_CMDS,
                                   text, len);
    if (built == 0) {
        RELAY_LOG("clipboard: internal transaction build failed");
        return;
    }
    r->clip_out_count = built;
    r->clip_out_next = 0;
    relay_clip_flush(r);
}

/* NVKVM_RELAY_WRITABLE_WIRING_BEGIN -- extracted by test_relay_wiring. */
static void relay_writable(void *opaque)
{
    NvkvmRelay *r = opaque;
    int err = 0;
    socklen_t errlen = sizeof(err);

    assert(bql_locked());
    if (r->sock < 0) {
        return;
    }
    if (r->conn_state == RELAY_CONN_CONNECTING) {
        if (getsockopt(r->sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            err = errno;
        }
        if (err != 0) {
            relay_attempt_failed(r, strerror(err));
            return;
        }
        r->conn_state = RELAY_CONN_HELLO;
        relay_set_fd_handlers(r, false);
        /* HELLO may already be queued; consume it without another poll turn. */
        relay_readable(r);
        return;
    }
    if (r->conn_state >= RELAY_CONN_SYNC_CAPS &&
        r->conn_state <= RELAY_CONN_SYNC_COMMIT) {
        relay_sync_flush(r);
        return;
    }
    if (r->conn_state == RELAY_CONN_ACTIVE) {
        relay_clip_flush(r);
    }
}
/* NVKVM_RELAY_WRITABLE_WIRING_END */

/* The guest's clipboard changed (vdagent -> ui/clipboard.c -> us). */
static void relay_clip_notify(Notifier *notifier, void *data)
{
    NvkvmRelay *r = container_of(notifier, NvkvmRelay, clip_peer.notifier);
    QemuClipboardNotify *notify = data;
    QemuClipboardInfo *info;

    if (!r || notify->type != QEMU_CLIPBOARD_UPDATE_INFO) {
        return;
    }
    assert(bql_locked());
    info = notify->info;
    if (!info || info->owner == &r->clip_peer) {
        return;                 /* our own update coming back around */
    }
    /*
     * Another peer published something, so a guest agent demonstrably exists.
     * Tell the broker once, so it can stop blaming the mode.
     */
    if (!r->clip_agent_seen) {
        int rc;

        r->clip_agent_seen = true;
        rc = r->conn_state == RELAY_CONN_ACTIVE ? relay_send_caps(r) : 0;
        if (rc != 0) {
            /* Reconnect re-sends CAPS from clip_agent_seen, so dropping is a
             * bounded retry rather than losing the capability transition. */
            relay_drop(r, "the broker socket would not take updated CAPS");
        }
    }
    if (info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        return;                 /* PRIMARY is not what a paste key means */
    }
    if (!info->types[QEMU_CLIPBOARD_TYPE_TEXT].available) {
        return;                 /* text only, deliberately */
    }
    if (!info->types[QEMU_CLIPBOARD_TYPE_TEXT].data) {
        /* Announced but not fetched yet; ask, and we are called again. */
        qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
        return;
    }
    relay_clip_send(r, (const char *)info->types[QEMU_CLIPBOARD_TYPE_TEXT].data,
                    info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
}

/* Someone in QEMU wants the data behind a clipboard we announced.  We always
 * hand over the whole text at announce time, so there is nothing to fetch. */
static void relay_clip_request(QemuClipboardInfo *info, QemuClipboardType type)
{
    (void)info; (void)type;
}

/* Host -> guest: a chunk arrived from the broker. */
static void relay_clip_recv(NvkvmRelay *r, const struct nvkvm_broker_pkt *p)
{
    const struct nvkvm_broker_clip_pkt *cp =
        (const struct nvkvm_broker_clip_pkt *)p;
    unsigned n = NVKVM_BROKER_CLIP_NBYTES(cp->info);

    /* The broker is the trusted side of this socket, but "trusted" is not
     * "unbounded": a bug over there must not become memory corruption here. */
    if (n > NVKVM_BROKER_CLIP_PKT_BYTES) {
        r->clip_in_bad = true;
        n = 0;
    }
    if (++r->clip_in_chunks > NVKVM_BROKER_CLIP_MAX_CHUNKS_PKT ||
        r->clip_in_len + n > NVKVM_BROKER_CLIP_MAX_BYTES) {
        r->clip_in_bad = true;
    }
    if (!r->clip_in_bad && n) {
        memcpy(r->clip_in + r->clip_in_len, cp->data, n);
        r->clip_in_len += n;
    }
    if (!(cp->info & NVKVM_BROKER_CLIP_LAST)) {
        return;
    }
    if (!r->clip_in_bad && r->clip_in_len && r->clip_registered) {
        QemuClipboardInfo *info =
            qemu_clipboard_info_new(&r->clip_peer,
                                    QEMU_CLIPBOARD_SELECTION_CLIPBOARD);

        qemu_clipboard_set_data(&r->clip_peer, info, QEMU_CLIPBOARD_TYPE_TEXT,
                                r->clip_in_len, r->clip_in, true);
        qemu_clipboard_info_unref(info);
        RELAY_LOG("clipboard: %u bytes from the host are now the guest's "
                  "clipboard", r->clip_in_len);
    }
    r->clip_in_len = 0;
    r->clip_in_chunks = 0;
    r->clip_in_bad = false;
}

/* ── reconnect ───────────────────────────────────────────────────────────── */

static void relay_activate(NvkvmRelay *r)
{
    assert(bql_locked());
    assert(r->sock >= 0);
    r->conn_state = RELAY_CONN_ACTIVE;
    timer_del(r->handshake_deadline);
    relay_set_fd_handlers(r, false);
    if (r->attempt_is_retry) {
        r->n_reconnects++;
        RELAY_LOG("reconnected to the display broker (reconnect #%" PRIu64 ")",
                  r->n_reconnects);
    }
    r->retry_ms = RELAY_RETRY_MIN_MS;
    r->retry_logged = false;
}

/*
 * RE-ESTABLISH EVERYTHING THE BROKER CANNOT KNOW.
 *
 * A restarted broker has an empty import cache, a blank window and no idea
 * what resolution the guest is running.  Nothing may be elided on the grounds
 * that it was already sent -- it was sent to a different process.
 *
 * Every send is nonblocking.  EAGAIN leaves the state at the first unsent
 * command and enables POLLOUT; the same connection generation resumes there.
 * CAPS is last, so a clipboard-agent notification that arrives during replay
 * is reflected without needing a second capability transaction.
 */
static void relay_sync_flush(NvkvmRelay *r)
{
    struct nvkvm_broker_cmd cmd;
    int rc;

    assert(bql_locked());
    while (r->sock >= 0) {
        memset(&cmd, 0, sizeof(cmd));
        switch (r->conn_state) {
        case RELAY_CONN_SYNC_WINDOW:
            if (r->last_fd < 0) {
                r->conn_state = RELAY_CONN_SYNC_CAPS;
                continue;
            }
            cmd.type = NVKVM_BROKER_CMD_WINDOW;
            cmd.width = r->last_bw;
            cmd.height = r->last_bh;
            rc = relay_send(r, &cmd, -1);
            if (rc == 0) {
                r->last_w = r->last_bw;
                r->last_h = r->last_bh;
                r->conn_state = RELAY_CONN_SYNC_ATTACH;
                continue;
            }
            break;
        case RELAY_CONN_SYNC_ATTACH:
            if (r->last_fd < 0) {
                r->conn_state = RELAY_CONN_SYNC_CAPS;
                continue;
            }
            cmd.type = NVKVM_BROKER_CMD_ATTACH;
            cmd.width = r->last_bw;
            cmd.height = r->last_bh;
            cmd.stride = r->last_stride;
            cmd.offset = 0;
            cmd.fourcc = r->last_fourcc;
            cmd.modifier = r->last_modifier;
            cmd.seq = (uint32_t)r->n_sent;
            rc = relay_send(r, &cmd, r->last_fd);
            if (rc == 0) {
                r->conn_state = RELAY_CONN_SYNC_COMMIT;
                continue;
            }
            break;
        case RELAY_CONN_SYNC_COMMIT:
            cmd.type = NVKVM_BROKER_CMD_COMMIT;
            rc = relay_send(r, &cmd, -1);
            if (rc == 0) {
                RELAY_LOG("reconnect: re-sent geometry %ux%u and the last "
                          "frame", r->last_bw, r->last_bh);
                r->conn_state = RELAY_CONN_SYNC_CAPS;
                continue;
            }
            break;
        case RELAY_CONN_SYNC_CAPS:
            rc = relay_send_caps(r);
            if (rc == 0) {
                relay_activate(r);
                return;
            }
            break;
        default:
            return;
        }
        if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
            relay_set_fd_handlers(r, true);
            return;
        }
        relay_attempt_failed(r,
            rc == -EPROTO ? "short write during connection state replay"
                          : "socket failure during connection state replay");
        return;
    }
}

static void relay_handshake_timeout(void *opaque)
{
    NvkvmRelay *r = opaque;

    assert(bql_locked());
    if (r->conn_state != RELAY_CONN_DOWN &&
        r->conn_state != RELAY_CONN_ACTIVE) {
        relay_attempt_failed(r,
            "the nonblocking connect/HELLO/state replay deadline expired");
    }
}

static void relay_retry(void *opaque)
{
    NvkvmRelay *r = opaque;
    Error *err = NULL;

    assert(bql_locked());
    if (r->conn_state != RELAY_CONN_DOWN) {
        return;
    }
    r->attempt_is_retry = true;
    if (relay_start_connect(r, nvkvm_relay_sock_path, &err) < 0) {
        /*
         * LOUD ONCE, THEN SILENT.  A broker that is down for a minute would
         * otherwise produce a line every retry, which buries the one line that
         * said what happened.
         */
        if (!r->retry_logged) {
            r->retry_logged = true;
            warn_report_err(err);
            RELAY_LOG("retrying in the background (up to every %ums); "
                      "the VM continues running", RELAY_RETRY_MAX_MS);
        } else {
            error_free(err);
        }
        r->retry_ms = r->retry_ms * 2 > RELAY_RETRY_MAX_MS
                          ? RELAY_RETRY_MAX_MS : r->retry_ms * 2;
        relay_schedule_retry(r);
        return;
    }
}

/* ── QemuDisplay registration ────────────────────────────────────────────── */

static void nvkvm_relay_early_init(DisplayOptions *opts)
{
    /*
     * Stash the path only.  The connection itself is made in init(), after the
     * machine exists, so a broker that is not running fails with one clear
     * message instead of a half-built machine.
     */
    g_free(nvkvm_relay_sock_path);
    nvkvm_relay_sock_path = g_strdup(opts->u.nvkvm_broker.socket);
}

static void nvkvm_relay_init(DisplayState *ds, DisplayOptions *opts)
{
    NvkvmRelay *r;
    QemuConsole *con = NULL;
    Error *err = NULL;
    int idx;

    nvkvm_relay_stats_init();

    for (idx = 0;; idx++) {
        QemuConsole *c = qemu_console_lookup_by_index(idx);

        if (!c) {
            break;
        }
        if (qemu_console_is_graphic(c)) {
            con = c;    /* last graphic console wins: the nvkvm head is
                         * registered after the emulated VGA */
        }
    }

    r = g_new0(NvkvmRelay, 1);
    r->sock = -1;
    r->conn_state = RELAY_CONN_DOWN;
    r->last_fd = -1;
    r->enabled = true;
    r->con = con;
    r->retry_ms = RELAY_RETRY_MIN_MS;
    r->retry = timer_new_ms(QEMU_CLOCK_REALTIME, relay_retry, r);
    r->handshake_deadline = timer_new_ms(QEMU_CLOCK_REALTIME,
                                         relay_handshake_timeout, r);
    /*
     * Register as an ordinary clipboard peer.  If no vdagent chardev exists
     * this simply never fires, which is the honest outcome -- and the broker
     * is told so through CAPS rather than being left to infer it from silence.
     */
    r->clip_peer.name = "nvkvm-broker";
    r->clip_peer.notifier.notify = relay_clip_notify;
    r->clip_peer.request = relay_clip_request;
    qemu_clipboard_peer_register(&r->clip_peer);
    r->clip_registered = true;
    nvkvm_relay = r;
    r->attempt_is_retry = false;
    if (relay_start_connect(r, nvkvm_relay_sock_path, &err) < 0) {
        /*
         * NOT FATAL ANY MORE.  This used to exit(1), which makes the display
         * process a startup dependency of the VM -- wrong on its own terms, and
         * wrong in the case it is most likely to be hit: a compose file, where
         * container start order is not guaranteed and the broker may simply not
         * be up yet.  The machine boots; the window appears when it appears.
         */
        warn_report_err(err);
        RELAY_LOG("starting without a display; retrying in the background. "
                  "The VM boots regardless -- this is not a startup "
                  "dependency.");
        r->retry_logged = true;
        relay_schedule_retry(r);
    }
    /*
     * NOTHING ELSE IS REGISTERED.  No DisplayChangeListener, because there is
     * nothing for one to do: the frame does not travel through QEMU's console
     * layer in broker mode — nvkvm_req_present() hands the fd straight to
     * nvkvm_display_relay_submit().  A DCL here would only invite the very
     * dpy_gl_scanout_dmabuf import this design exists to remove.
     */
    RELAY_LOG("broker mode active: this QEMU holds no display-server "
              "connection and imports nothing.");
}

static QemuDisplay qemu_display_nvkvm_broker = {
    .type       = DISPLAY_TYPE_NVKVM_BROKER,
    .early_init = nvkvm_relay_early_init,
    .init       = nvkvm_relay_init,
};

static void register_nvkvm_broker(void)
{
    qemu_display_register(&qemu_display_nvkvm_broker);
}

type_init(register_nvkvm_broker);

/*
 * Connection generation.  Bumped every time a RECONNECT completes, so a caller
 * that cached anything about the previous broker can notice the display it was
 * talking to is gone.  The relay already forgets its own format verdict on
 * disconnect (relay_forget_format_verdict); this lets the present path do the
 * same for the decision it latched.
 */
uint64_t nvkvm_display_relay_generation(void)
{
    NvkvmRelay *r = nvkvm_relay;

    return r ? r->n_reconnects : 0;
}

#else /* !CONFIG_LINUX */

#include "nvkvm_display_relay.h"

bool nvkvm_display_relay_active(void)
{
    return false;
}

uint64_t nvkvm_display_relay_generation(void)
{
    return 0;
}

bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier)
{
    (void)nv; (void)dmabuf_fd; (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier;
    return false;
}

#endif /* CONFIG_LINUX */
