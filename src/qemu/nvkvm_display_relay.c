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
#include "block/aio.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "ui/console.h"
#include "ui/input.h"

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
     * one CMD_WINDOW instead of a request per frame. */
    uint32_t    last_w, last_h;

    /* Partial packet accumulator.  Fixed size: a packet is always exactly
     * NVKVM_BROKER_PKT_SIZE bytes, so nothing here is length-driven. */
    uint8_t     rxbuf[NVKVM_BROKER_PKT_SIZE];
    size_t      rxlen;

    bool        grabbed;
    uint64_t    n_sent, n_dropped;
} NvkvmRelay;

static NvkvmRelay *nvkvm_relay;
static char *nvkvm_relay_sock_path;

bool nvkvm_display_relay_active(void)
{
    return nvkvm_relay && nvkvm_relay->sock >= 0;
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
    error_report("nvkvm-broker: %s; the display and input are gone "
                 "(%" PRIu64 " frames relayed, %" PRIu64 " dropped)",
                 why, r->n_sent, r->n_dropped);
}

bool nvkvm_display_relay_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                                uint32_t width, uint32_t height,
                                uint32_t stride, uint32_t fourcc,
                                uint64_t modifier)
{
    NvkvmRelay *r = nvkvm_relay;
    struct nvkvm_broker_cmd cmd;
    int rc;

    if (!r || r->sock < 0 || dmabuf_fd < 0) {
        return false;
    }

    qemu_mutex_lock(&r->lock);
    if (r->sock < 0) {
        qemu_mutex_unlock(&r->lock);
        return false;
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
    rc = relay_send(r, &cmd, dmabuf_fd);
    if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
        /*
         * The broker has not drained.  Dropping is correct: the guest has
         * produced faster than the display can consume, and the next flip
         * carries a newer buffer.  Counted, not silent.
         */
        r->n_dropped++;
        qemu_mutex_unlock(&r->lock);
        close(dmabuf_fd);
        return true;        /* we consumed the fd; do not fall through */
    }
    if (rc != 0) {
        relay_drop(r, rc == -EPROTO ? "short write on the broker socket"
                                    : "the broker socket failed");
        qemu_mutex_unlock(&r->lock);
        close(dmabuf_fd);
        return true;
    }
    /* The broker holds its own copy from SCM_RIGHTS now. */
    close(dmabuf_fd);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = NVKVM_BROKER_CMD_COMMIT;
    rc = relay_send(r, &cmd, -1);
    if (rc != 0 && rc != -EAGAIN && rc != -EWOULDBLOCK) {
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

    for (e = mice; e; e = e->next) {
        if (e->value->absolute != relative) {
            qemu_mouse_set((int)e->value->index, NULL);
            break;
        }
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
            return -errno;
        }
        got += (size_t)n;
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
    r->con = con;
    qemu_mutex_init(&r->lock);
    if (relay_connect(r, nvkvm_relay_sock_path, &err) < 0) {
        error_report_err(err);
        exit(1);
    }
    /*
     * NOTHING ELSE IS REGISTERED.  No DisplayChangeListener, because there is
     * nothing for one to do: the frame does not travel through QEMU's console
     * layer in broker mode — nvkvm_req_present() hands the fd straight to
     * nvkvm_display_relay_submit().  A DCL here would only invite the very
     * dpy_gl_scanout_dmabuf import this design exists to remove.
     */
    nvkvm_relay = r;
    qemu_set_fd_handler(r->sock, relay_readable, NULL, r);
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
