/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_broker.h — internals of the privileged display broker.
 *
 * Two abstractions, and only two:
 *
 *   struct nb_session   a *session backend*: it owns one window on one display
 *                       stack (Wayland or X11), knows how to turn a dma-buf
 *                       into that stack's native buffer object and put it on
 *                       screen, and knows how to read input.  Backends are
 *                       deliberately dumb: they translate native events into
 *                       sink calls and do what set_grab() says.
 *
 *   struct nb_sink      the policy core: hotkeys, grab state, focus gating,
 *                       stuck-key release, command validation and packet
 *                       emission.  Every security-relevant rule lives here
 *                       ONCE so it can be audited once, rather than twice in
 *                       two backends that will drift.
 *
 * Nothing in the broker allocates on behalf of a client.  Every buffer is
 * fixed size and statically sized at compile time, and no size, count or index
 * is ever taken from the wire.
 */
#ifndef NVKVM_BROKER_H
#define NVKVM_BROKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <sys/types.h>

#include "nvkvm_broker_proto.h"

#define NB_MAX_SESSION_FDS   8
#define NB_MAX_ALLOW_IDS     16
/*
 * How many imported buffers a backend keeps alive at once.  A guest compositor
 * cycles a handful of scanout bos — measured at 3, which is why
 * src/qemu/nvkvm_present_egl.c carries an 8-entry import cache — so 8 covers
 * the real workload with headroom.  It is also the FD INTAKE BOUND: the client
 * cannot make the privileged process hold more than this many buffers, because
 * importing the ninth evicts and closes the least recently used.
 */
#define NB_MAX_BUFS          8
/* Outbound packet ring.  ~12 KB.  Overflow means the client is not draining
 * its socket, which is either a dead client or a hostile one trying to make
 * the privileged side buffer without bound: we disconnect instead of growing. */
#define NB_TXRING            512

/* ── logging ─────────────────────────────────────────────────────────────── */
void nb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nb_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int nb_verbose;

/* ── configuration (command line only; never anything a client sent) ─────── */
struct nb_config {
    const char *socket_path;
    const char *backend;        /* "auto" | "x11" | "wayland" | "test"        */
    const char *title;
    unsigned    win_w, win_h;   /* initial window size                        */
    const char *drop_user;      /* user to become after setup, or NULL        */
    uid_t  allow_uid[NB_MAX_ALLOW_IDS];
    int    n_allow_uid;
    gid_t  allow_gid[NB_MAX_ALLOW_IDS];
    int    n_allow_gid;
};

/* ── a validated buffer description ──────────────────────────────────────── */

/*
 * What reaches a backend's attach().  Every field here has already been
 * checked by nb_cmd_attach() in nvkvm_broker.c against the REAL fd:
 *
 *   - `fd` is a dma-buf (fstatfs f_type == DMA_BUF_MAGIC), not merely an fd;
 *   - width/height are non-zero and <= NVKVM_BROKER_MAX_DIM;
 *   - fourcc is one the backend's format_ok() said the GPU advertises,
 *     paired with this exact modifier;
 *   - stride >= width * bpp, and offset + stride*height <= the fd's real size
 *     measured with lseek(fd, 0, SEEK_END).
 *
 * A backend may therefore import without re-deriving any of that.  It must
 * still check its own import calls for failure, because the compositor or X
 * server can refuse for reasons the broker cannot see.
 */
struct nb_buf_desc {
    int      fd;                /* borrowed: attach() must not close it       */
    uint64_t id;                /* the dma-buf inode — stable, not reused
                                 * while the buffer is live, and derived by
                                 * the BROKER, never supplied by the client   */
    uint64_t size;              /* real extent, from lseek(SEEK_END)          */
    uint32_t width, height, stride, offset, fourcc;
    uint64_t modifier;
    uint32_t bpp;               /* bytes per pixel implied by fourcc          */
};

/* ── the policy core ─────────────────────────────────────────────────────── */
struct nb_session;

struct nb_sink {
    struct nb_session *sess;

    int      client_fd;         /* -1 when nobody is connected                */
    pid_t    client_pid;        /* from SO_PEERCRED, for log lines only       */
    uint32_t seq;

    bool     focused;
    bool     pointer_in;
    bool     grabbed;
    bool     fullscreen;

    /* Modifier latch for the broker-owned hotkeys.  Tracked from the same key
     * stream the client sees, so the two cannot disagree about reality. */
    bool     ctrl_down, alt_down;

    /* Which keys the CLIENT currently believes are down.  On focus loss we
     * synthesise a release for each: a grab that ends with keys latched down
     * in the guest is both a bug and, for modifiers, a security surprise. */
    unsigned char key_down[768 / 8];

    struct nvkvm_broker_pkt tx[NB_TXRING];
    unsigned tx_head, tx_tail;  /* head==tail ⇒ empty                         */
    size_t   tx_partial;        /* bytes of tx[tx_tail] already written       */

    /* Inbound command accumulator.  EXACTLY one command wide: nothing here is
     * length-driven, so there is no buffer for a client to overrun. */
    uint8_t  rx[NVKVM_BROKER_CMD_SIZE];
    size_t   rxlen;
    int      rxfd;              /* fd banked from a partial ATTACH read       */

    /* Counters, for the one-line status the broker logs on detach. */
    uint64_t n_attach, n_commit, n_reject;
};

void nb_sink_init(struct nb_sink *s, struct nb_session *sess);
/* Attach the single client.  Performs the handshake.  <0 ⇒ nothing attached. */
int  nb_sink_attach(struct nb_sink *s, int fd);
void nb_sink_detach(struct nb_sink *s, const char *why);
/* Flush queued packets; returns <0 if the client must be dropped. */
int  nb_sink_flush(struct nb_sink *s);
bool nb_sink_want_write(const struct nb_sink *s);
/* Drain and act on whatever the client sent.  Disconnects on any violation. */
void nb_sink_readable(struct nb_sink *s);

/* Called by session backends.  `code` values are Linux evdev codes on every
 * backend — that is what makes the wire format backend-independent. */
void nb_sink_key(struct nb_sink *s, unsigned code, bool down);
void nb_sink_btn(struct nb_sink *s, unsigned code, bool down);
void nb_sink_abs(struct nb_sink *s, int x, int y, unsigned w, unsigned h);
void nb_sink_rel(struct nb_sink *s, int dx, int dy);
void nb_sink_wheel(struct nb_sink *s, int v, int h);
void nb_sink_focus(struct nb_sink *s, bool active);
void nb_sink_pointer(struct nb_sink *s, bool inside);
void nb_sink_surface(struct nb_sink *s, unsigned w, unsigned h);
void nb_sink_frame(struct nb_sink *s);
void nb_sink_release(struct nb_sink *s, uint64_t buf_id);
void nb_sink_bye(struct nb_sink *s, int reason);

/* ── session backends ────────────────────────────────────────────────────── */
struct nb_session_ops {
    const char *name;
    /* Create the window and the input source.  Fills the public fields below.
     * Returns 0, or -errno with a human explanation already logged. */
    int  (*open)(struct nb_session *s, const struct nb_config *cfg);
    void (*close)(struct nb_session *s);
    /* Fds to poll, WITH their events already set — a backend that has queued
     * output the display server has not taken yet asks for POLLOUT itself, so
     * flushing the display can never sit inside the input path.  Returns how
     * many were written to `out`, or <0. */
    int  (*pollfds)(struct nb_session *s, struct pollfd *out, int max);
    /* Drain whatever those fds have, pushing into `sink`.  Returns <0 on a
     * fatal session error (the display went away). */
    int  (*dispatch)(struct nb_session *s, struct nb_sink *sink);
    int  (*set_grab)(struct nb_session *s, bool on);
    int  (*set_fullscreen)(struct nb_session *s, bool on);

    /* Is this (fourcc, modifier) pair one the GPU actually advertises?  NOT a
     * hardcoded list: each backend answers from the set the compositor or X
     * server enumerated at startup. */
    bool (*format_ok)(struct nb_session *s, uint32_t fourcc, uint64_t mod);
    /* Import an already-validated dma-buf and make it the pending content.
     * Does NOT take ownership of desc->fd.  Returns 0 or -errno. */
    int  (*attach)(struct nb_session *s, const struct nb_buf_desc *desc);
    /* Put the pending content on screen.  Returns 0 or -errno. */
    int  (*commit)(struct nb_session *s, struct nb_sink *sink);
    /* Ask for a window of this size.  Advisory; may be ignored. */
    int  (*resize)(struct nb_session *s, unsigned w, unsigned h);

    /*
     * Show the "no VM attached" placeholder.  Called once before the first
     * client can connect and again whenever one goes away, so that an idle
     * broker is visibly idle rather than an unmapped window.  Optional: a
     * backend that leaves this NULL simply shows nothing until a real frame
     * arrives, which is what every backend did before.
     */
    int  (*show_idle)(struct nb_session *s);
};

struct nb_session {
    const struct nb_session_ops *ops;
    void    *priv;

    uint32_t width, height;     /* current window size, in pixels             */
    uint32_t caps;              /* NVKVM_BROKER_CAP_*                         */
    /*
     * Backends whose import path cannot tell a dma-buf from any other fd are
     * not a thing — but the TEST backend has no import path at all and must be
     * able to exercise the accept side of the validator without a GPU.  It is
     * the only backend that sets this, it is never reachable from
     * --backend auto, and it prints a banner saying it drives nothing.
     */
    bool     accept_memfd;
    /* One line, printed at startup and again at connect, naming exactly what
     * this stack cannot capture.  Empty string means "everything". */
    char     grab_caveat[256];
};

/* Backend constructors.  Each returns NULL (after logging) when its stack is
 * not present or not usable; nb_session_open() tries them in order. */
struct nb_session *nb_session_x11(const struct nb_config *cfg);
struct nb_session *nb_session_wayland(const struct nb_config *cfg);
/* No display behind it; see nb_session_test.c.  Never selected by
 * --backend auto — it must be asked for by name. */
struct nb_session *nb_session_test(const struct nb_config *cfg);

/* Pick and open a backend per cfg->backend. */
struct nb_session *nb_session_open(const struct nb_config *cfg);

/* ── shared helpers ──────────────────────────────────────────────────────── */

/* Bytes per pixel for the single-plane 32-bit formats the nvkvm head can
 * flip, or 0 for anything else.  0 means "reject": a format whose pitch we
 * cannot compute is a format whose bounds we cannot check. */
uint32_t nb_fourcc_bpp(uint32_t fourcc);
const char *nb_fourcc_name(uint32_t fourcc, char buf[8]);

/*
 * A backend's advertised (fourcc, modifier) set.  Filled from what the display
 * server enumerated; consulted by the generic format_ok helper.  Fixed
 * capacity, because the number of entries a compositor sends is not something
 * the broker should let determine an allocation.
 */
#define NB_MAX_FORMATS 256
struct nb_formats {
    struct { uint32_t fourcc; uint64_t modifier; } e[NB_MAX_FORMATS];
    unsigned n;
    bool     overflowed;
};
void nb_formats_add(struct nb_formats *f, uint32_t fourcc, uint64_t modifier);
bool nb_formats_has(const struct nb_formats *f, uint32_t fourcc, uint64_t mod);
void nb_formats_log(const struct nb_formats *f, const char *what);

/*
 * Paint the idle placeholder into a 32-bit XRGB/ARGB buffer (nb_placeholder.c).
 * `stride_px` is in pixels.  CPU only — no GL, no dma-buf, deliberately.
 */
void nb_placeholder_paint(uint32_t *px, unsigned w, unsigned h,
                          unsigned stride_px, const char *line1,
                          const char *line2);

#endif /* NVKVM_BROKER_H */
