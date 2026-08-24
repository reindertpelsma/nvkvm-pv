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
#include "block/aio.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "sysemu/runstate.h"   /* qemu_system_powerdown_request.  QEMU 9.2 path;
                                 * renamed to system/ in 10.0.               */
#include "ui/console.h"
#include "ui/input.h"
#include "ui/clipboard.h"

#include "nvkvm_display_relay.h"
#include "nvkvm_inc/nvkvm_broker_proto.h"

/* Always-visible, like the rest of the present path: these lines are how a
 * user finds out why their screen is black. */
#define RELAY_LOG(fmt, ...) \
    fprintf(stderr, "nvkvm-broker: " fmt "\n", ##__VA_ARGS__)

typedef struct NvkvmRelay {
    int         sock;           /* the broker connection, or -1               */
    QemuMutex   lock;           /* serialises sends; the virtio worker sends  */
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
    unsigned    retry_ms;
    bool        retry_logged;   /* first failure is loud, the rest are silent */
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
    char        clip_in[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    unsigned    clip_in_len;
    unsigned    clip_in_chunks;
    bool        clip_in_bad;

    int         last_fd;
    uint32_t    last_bw, last_bh, last_stride, last_fourcc;
    uint64_t    last_modifier;

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
} NvkvmRelay;

static NvkvmRelay *nvkvm_relay;
static char *nvkvm_relay_sock_path;

bool nvkvm_display_relay_active(void)
{
    /* enabled, not connected: see the `enabled` comment.  While the broker is
     * away we still consume the frame (and keep the newest) rather than let it
     * fall through to the GL import path. */
    return nvkvm_relay && nvkvm_relay->enabled;
}

/* ── sending ─────────────────────────────────────────────────────────────── */

/*
 * One command, optionally carrying one fd.  MSG_DONTWAIT throughout: this can
 * be called from a virtio worker thread, and a display that has stopped
 * draining must cost a dropped frame, never a blocked vCPU.
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

static void relay_arm_retry_bh(void *opaque);
static void relay_retry(void *opaque);
static int  relay_connect(NvkvmRelay *r, const char *path, Error **errp);
static void relay_readable(void *opaque);
static void relay_replay(NvkvmRelay *r);
static void relay_send_caps(NvkvmRelay *r);
static void relay_clip_recv(NvkvmRelay *r, const struct nvkvm_broker_pkt *p);

static void relay_close_bh(void *opaque)
{
    int fd = (int)(intptr_t)opaque;

    qemu_set_fd_handler(fd, NULL, NULL, NULL);
    close(fd);
}

static void relay_drop(NvkvmRelay *r, const char *why)
{
    int fd;

    if (r->sock < 0) {
        return;
    }
    fd = r->sock;
    r->sock = -1;
    /* A NEW broker has been told nothing, so nothing may be elided later. */
    r->last_w = r->last_h = 0;
    /*
     * The fd is in the main loop's iohandler set, and relay_drop() is
     * reachable from a virtio WORKER thread (a failed send).  Unregistering
     * and closing a polled fd out from under the main loop is a
     * use-after-close, so the teardown is bounced to the main loop.  Setting
     * r->sock = -1 first is what makes every other path stop using it
     * immediately, so the delay costs nothing.
     */
    aio_bh_schedule_oneshot(qemu_get_aio_context(), relay_close_bh,
                            (void *)(intptr_t)fd);
    error_report("nvkvm-broker: %s; the display and input are gone for now "
                 "(%" PRIu64 " frames relayed, %" PRIu64 " dropped, "
                 "%" PRIu64 " attached without a commit). "
                 "The VM keeps running; reconnecting in the background.",
                 why, r->n_sent, r->n_dropped, r->n_uncommitted);
    /*
     * RESTARTING A DISPLAY PROCESS MUST NOT BE A VM-AFFECTING EVENT.  Arm the
     * retry from the main loop -- relay_drop() is reachable from a virtio
     * worker thread and QEMUTimer is not thread-safe.
     */
    r->retry_ms = RELAY_RETRY_MIN_MS;
    r->retry_logged = false;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), relay_arm_retry_bh, r);
}

bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier)
{
    NvkvmRelay *r = nvkvm_relay;
    struct nvkvm_broker_cmd cmd;
    uint32_t cmd_seq = 0;
    int rc;

    if (!r || r->sock < 0 || dmabuf_fd < 0) {
        return false;
    }

    qemu_mutex_lock(&r->lock);

    /*
     * RETAIN THE NEWEST, DROP THE REST.  This runs whether or not the socket is
     * up: while the broker is away the frames still arrive (the guest does not
     * know), and keeping the latest is what lets a reconnect paint immediately.
     * Replacing rather than queueing is deliberate -- a backlog of stale frames
     * delivered on reconnect is worse than a brief blank, and unbounded.
     */
    if (r->last_fd >= 0) {
        close(r->last_fd);
    }
    r->last_fd       = dmabuf_fd;
    r->last_bw       = width;
    r->last_bh       = height;
    r->last_stride   = stride;
    r->last_fourcc   = fourcc;
    r->last_modifier = modifier;

    if (r->sock < 0) {
        /* Disconnected: consumed and remembered, nothing to send.  Returning
         * true keeps the caller from falling through to the GL import path. */
        qemu_mutex_unlock(&r->lock);
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
        qemu_mutex_unlock(&r->lock);
        return true;        /* we consumed the fd; do not fall through */
    }
    if (rc != 0) {
        relay_drop(r, rc == -EPROTO ? "short write on the broker socket"
                                    : "the broker socket failed");
        qemu_mutex_unlock(&r->lock);
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
    qemu_mutex_unlock(&r->lock);
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
        /* Linux evdev keycode -> QKeyCode, the same table ui/input-linux.c
         * uses, so the broker's backend-independent wire format costs one
         * array lookup here. */
        unsigned code = (unsigned)p->x;

        if (code < qemu_input_map_linux_to_qcode_len) {
            QKeyCode q = qemu_input_map_linux_to_qcode[code];

            if (q != Q_KEY_CODE_UNMAPPED) {
                qemu_input_event_send_key_qcode(con, q, p->y != 0);
            }
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
         * FULLSCREEN ONLY, and that is a design rule rather than an
         * optimisation.  A windowed resize must not reach the guest at all --
         * the host scales the buffer it already has, and the guest goes on
         * believing it is the same size, so nothing inside it reflows because
         * someone dragged a window edge.  Fullscreen is the opposite case:
         * propagating the size is exactly what makes the guest render at the
         * output's resolution, which is 1:1 pixels and the precondition for
         * the compositor scanning its buffer out directly.
         *
         * `delay` is true: entering fullscreen can produce more than one
         * configure and only the last is worth a guest mode switch.  QEMU's
         * own timer coalescing is the right tool and is already there.
         */
        if (con && p->x > 0 && p->y > 0 &&
            (p->flags & NVKVM_BROKER_F_FULLSCREEN) &&
            dpy_ui_info_supported(con)) {
            QemuUIInfo info = *dpy_get_ui_info(con);

            info.width  = (uint32_t)p->x;
            info.height = (uint32_t)p->y;
            dpy_set_ui_info(con, &info, true);
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

    for (;;) {
        ssize_t n;

        if (r->sock < 0) {
            return;
        }
        n = recv(r->sock, r->rxbuf + r->rxlen,
                 NVKVM_BROKER_PKT_SIZE - r->rxlen, MSG_DONTWAIT);
        if (n == 0) {
            relay_drop(r, "the display broker closed the connection");
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            relay_drop(r, "read error on the broker socket");
            return;
        }
        r->rxlen += (size_t)n;
        if (r->rxlen == NVKVM_BROKER_PKT_SIZE) {
            struct nvkvm_broker_pkt pkt;

            memcpy(&pkt, r->rxbuf, sizeof(pkt));
            r->rxlen = 0;
            relay_handle(r, &pkt);
        }
    }
}

/* ── connection ──────────────────────────────────────────────────────────── */

static int relay_recv_blocking(int sock, struct nvkvm_broker_pkt *pkt)
{
    size_t got = 0;
    /*
     * BOUNDED.  This runs on the main loop -- at startup, and now on every
     * reconnect attempt -- so a broker that accepts the connection and then
     * says nothing must not wedge QEMU.  Two seconds is far longer than a
     * local unix-socket handshake and short enough that nobody watches it.
     */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (got < NVKVM_BROKER_PKT_SIZE) {
        ssize_t n = recv(sock, (char *)pkt + got,
                         NVKVM_BROKER_PKT_SIZE - got, 0);

        if (n == 0) {
            return -EPIPE;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;         /* EAGAIN here is the 2s timeout above */
        }
        got += (size_t)n;
    }
    {   /* Back to blocking for the rest of the connection's life. */
        struct timeval off = { .tv_sec = 0, .tv_usec = 0 };

        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &off, sizeof(off));
    }
    return 0;
}

static int relay_connect(NvkvmRelay *r, const char *path, Error **errp)
{
    struct sockaddr_un sa;
    struct nvkvm_broker_pkt pkt;
    int fd, rc;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        error_setg(errp, "nvkvm-broker: socket path too long");
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "nvkvm-broker: socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        error_setg_errno(errp, errno,
                         "nvkvm-broker: cannot connect to %s. Is the display "
                         "broker running, and is the socket bind-mounted into "
                         "this container?", path);
        close(fd);
        return -1;
    }

    /* HELLO is unconditionally the first packet, sent immediately on accept.
     * If it does not arrive, SO_PEERCRED rejected us. */
    rc = relay_recv_blocking(fd, &pkt);
    if (rc || pkt.type != NVKVM_BROKER_EV_HELLO) {
        error_setg(errp, "nvkvm-broker: no HELLO from %s (%s). The broker "
                   "validates SO_PEERCRED before it sends anything, so this "
                   "is usually 'this uid is not on its allow list'.",
                   path, rc ? strerror(-rc) : "wrong packet type");
        close(fd);
        return -1;
    }
    if (pkt.w0 != NVKVM_BROKER_PROTO_VERSION) {
        error_setg(errp, "nvkvm-broker: protocol version %u, expected %u",
                   pkt.w0, NVKVM_BROKER_PROTO_VERSION);
        close(fd);
        return -1;
    }
    r->caps = pkt.w1;
    RELAY_LOG("connected to %s, capabilities 0x%x", path, pkt.w1);
    if (!(pkt.w1 & NVKVM_BROKER_CAP_DMABUF)) {
        error_setg(errp, "nvkvm-broker: the broker reports it cannot accept "
                   "dma-buf buffers, so nothing would ever be displayed");
        close(fd);
        return -1;
    }
    if (!(pkt.w1 & NVKVM_BROKER_CAP_FOCUS_EVENTS)) {
        RELAY_LOG("NOTE: the broker cannot observe focus loss on this "
                  "session, so it will not offer a keyboard grab. Absolute "
                  "pointer and keyboard still work while the window is "
                  "active.");
    }
    /* Non-blocking from here on, in both directions: the send side runs on a
     * virtio worker thread and must never stall a vCPU, and the receive side
     * runs on the main loop and must never stall the VM. */
    if (!g_unix_set_fd_nonblocking(fd, true, NULL)) {
        error_setg(errp, "nvkvm-broker: cannot set O_NONBLOCK");
        close(fd);
        return -1;
    }
    r->sock = fd;
    return 0;
}

/* ── clipboard ───────────────────────────────────────────────────────────── */

/*
 * Tell the broker what we can do.  Re-sent on every (re)connect, because a
 * restarted broker knows nothing -- the same rule as the geometry replay.
 *
 * The clipboard bit is what lets the broker tell "clipboard is off" apart from
 * "there is no guest agent"; without it both look like nothing happening and
 * the user is left to guess which knob to turn.
 */
static void relay_send_caps(NvkvmRelay *r)
{
    struct nvkvm_broker_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type  = NVKVM_BROKER_CMD_CAPS;
    cmd.width = r->clip_registered ? NVKVM_BROKER_CLIENT_CLIPBOARD : 0;
    (void)relay_send(r, &cmd, -1);
}

/* ── clipboard ───────────────────────────────────────────────────────────── */

/* Guest -> host: chunk the guest's clipboard text out to the broker. */
static void relay_clip_send(NvkvmRelay *r, const char *text, size_t len)
{
    size_t off = 0;
    uint32_t chunk = 0;

    if (r->sock < 0 || len == 0) {
        return;
    }
    if (len > NVKVM_BROKER_CLIP_MAX_BYTES) {
        RELAY_LOG("clipboard: the guest offered %zu bytes, over the %u cap; "
                  "not forwarding it", len, NVKVM_BROKER_CLIP_MAX_BYTES);
        return;
    }
    do {
        struct nvkvm_broker_clip_cmd cc;
        size_t n = len - off;

        if (n > NVKVM_BROKER_CLIP_CMD_BYTES) {
            n = NVKVM_BROKER_CLIP_CMD_BYTES;
        }
        memset(&cc, 0, sizeof(cc));
        cc.type  = NVKVM_BROKER_CMD_CLIPBOARD;
        cc.chunk = chunk++;
        cc.info  = (uint8_t)n;
        if (off + n == len) {
            cc.info |= NVKVM_BROKER_CLIP_LAST;
        }
        memcpy(cc.data, text + off, n);
        if (relay_send(r, (const struct nvkvm_broker_cmd *)&cc, -1) != 0) {
            RELAY_LOG("clipboard: the broker socket would not take the "
                      "guest's clipboard; dropped");
            return;
        }
        off += n;
    } while (off < len);
}

/* The guest's clipboard changed (vdagent -> ui/clipboard.c -> us). */
static void relay_clip_notify(Notifier *notifier, void *data)
{
    NvkvmRelay *r = container_of(notifier, NvkvmRelay, clip_peer.notifier);
    QemuClipboardNotify *notify = data;
    QemuClipboardInfo *info;

    if (!r || notify->type != QEMU_CLIPBOARD_UPDATE_INFO) {
        return;
    }
    info = notify->info;
    if (!info || info->owner == &r->clip_peer) {
        return;                 /* our own update coming back around */
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
    qemu_mutex_lock(&r->lock);
    relay_clip_send(r, (const char *)info->types[QEMU_CLIPBOARD_TYPE_TEXT].data,
                    info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
    qemu_mutex_unlock(&r->lock);
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

static void relay_arm_retry_bh(void *opaque)
{
    NvkvmRelay *r = opaque;

    if (!r->retry) {
        r->retry = timer_new_ms(QEMU_CLOCK_REALTIME, relay_retry, r);
    }
    timer_mod(r->retry, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + r->retry_ms);
}

/*
 * RE-ESTABLISH EVERYTHING THE BROKER CANNOT KNOW.
 *
 * A restarted broker has an empty import cache, a blank window and no idea
 * what resolution the guest is running.  Nothing may be elided on the grounds
 * that it was already sent -- it was sent to a different process.
 *
 * Called with r->lock held.
 */
static void relay_replay(NvkvmRelay *r)
{
    struct nvkvm_broker_cmd cmd;

    if (r->sock < 0 || r->last_fd < 0) {
        return;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = NVKVM_BROKER_CMD_WINDOW;
    cmd.width  = r->last_bw;
    cmd.height = r->last_bh;
    if (relay_send(r, &cmd, -1) == 0) {
        r->last_w = r->last_bw;
        r->last_h = r->last_bh;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = NVKVM_BROKER_CMD_ATTACH;
    cmd.width    = r->last_bw;
    cmd.height   = r->last_bh;
    cmd.stride   = r->last_stride;
    cmd.offset   = 0;
    cmd.fourcc   = r->last_fourcc;
    cmd.modifier = r->last_modifier;
    cmd.seq      = (uint32_t)r->n_sent;
    if (relay_send(r, &cmd, r->last_fd) != 0) {
        RELAY_LOG("reconnect: could not re-attach the last frame");
        return;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = NVKVM_BROKER_CMD_COMMIT;
    if (relay_send(r, &cmd, -1) != 0) {
        RELAY_LOG("reconnect: could not commit the last frame");
        return;
    }
    RELAY_LOG("reconnect: re-sent geometry %ux%u and the last frame, so the "
              "new window is correct without waiting for the guest to flip",
              r->last_bw, r->last_bh);
}

static void relay_retry(void *opaque)
{
    NvkvmRelay *r = opaque;
    Error *err = NULL;

    qemu_mutex_lock(&r->lock);
    if (r->sock >= 0) {                 /* raced with something else */
        qemu_mutex_unlock(&r->lock);
        return;
    }
    if (relay_connect(r, nvkvm_relay_sock_path, &err) < 0) {
        /*
         * LOUD ONCE, THEN SILENT.  A broker that is down for a minute would
         * otherwise produce a line every retry, which buries the one line that
         * said what happened.
         */
        if (!r->retry_logged) {
            r->retry_logged = true;
            warn_report_err(err);
            RELAY_LOG("retrying in the background (up to every %ums); "
                      "the VM is unaffected", RELAY_RETRY_MAX_MS);
        } else {
            error_free(err);
        }
        r->retry_ms = r->retry_ms * 2 > RELAY_RETRY_MAX_MS
                          ? RELAY_RETRY_MAX_MS : r->retry_ms * 2;
        qemu_mutex_unlock(&r->lock);
        relay_arm_retry_bh(r);
        return;
    }
    qemu_set_fd_handler(r->sock, relay_readable, NULL, r);
    relay_send_caps(r);
    r->n_reconnects++;
    r->retry_ms = RELAY_RETRY_MIN_MS;
    r->retry_logged = false;
    RELAY_LOG("reconnected to the display broker (reconnect #%" PRIu64 ")",
              r->n_reconnects);
    relay_replay(r);
    qemu_mutex_unlock(&r->lock);
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
    r->last_fd = -1;
    r->enabled = true;
    r->con = con;
    r->retry_ms = RELAY_RETRY_MIN_MS;
    qemu_mutex_init(&r->lock);
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
    if (relay_connect(r, nvkvm_relay_sock_path, &err) < 0) {
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
        relay_arm_retry_bh(r);
        RELAY_LOG("broker mode active: this QEMU holds no display-server "
                  "connection and imports nothing.");
        return;
    }
    /*
     * NOTHING ELSE IS REGISTERED.  No DisplayChangeListener, because there is
     * nothing for one to do: the frame does not travel through QEMU's console
     * layer in broker mode — nvkvm_req_present() hands the fd straight to
     * nvkvm_display_relay_submit().  A DCL here would only invite the very
     * dpy_gl_scanout_dmabuf import this design exists to remove.
     */
    qemu_set_fd_handler(r->sock, relay_readable, NULL, r);
    relay_send_caps(r);
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

#else /* !CONFIG_LINUX */

#include "nvkvm_display_relay.h"

bool nvkvm_display_relay_active(void)
{
    return false;
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
