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

/*
 * --trace-frames.  Logs one line per COMMIT: the client's frame counter, the
 * dma-buf inode, the cache slot, and whether the compositor had released that
 * buffer before it was handed back.  This exists because an intermittent
 * "stale frame" is the one bug class you cannot fix by reading the code: the
 * only way to tell a reorder from a buffer overwritten in flight is to watch
 * buffer IDENTITY per frame, and identity is only knowable here.
 */
extern int nb_trace_frames;

/* ── configuration (command line only; never anything a client sent) ─────── */
/*
 * CLIPBOARD MODES, NAMED BY DIRECTION.
 *
 * "copy-only" and similar are ambiguous about which way copying goes, and on a
 * security setting a misread default is the whole problem.  The ladder is
 * ordered by how much crosses, and `consent` is the recommended rung.
 */
#define NB_CLIP_OFF      0  /* nothing crosses.  The DEFAULT.               */
#define NB_CLIP_G2H      1  /* guest may write the host clipboard; the host
                             * clipboard is never readable by the guest      */
#define NB_CLIP_CONSENT  2  /* G2H, plus host->guest ONLY on an explicit
                             * paste trigger.  RECOMMENDED.                  */

/* Keys that mean "paste" and so trigger a host->guest send. */
#define NB_CLIP_MAX_TRIGGERS 8
struct nb_clip_trigger {
    unsigned code;              /* evdev key code                           */
    bool     need_ctrl;
    bool     need_shift;
};

struct nb_config {
    const char *socket_path;
    const char *backend;        /* "auto" | "x11" | "wayland" | "test"        */
    const char *title;
    unsigned    win_w, win_h;   /* initial window size                        */
    const char *drop_user;      /* user to become after setup, or NULL        */
    /*
     * SOCKET ACCESS.  Until these existed the socket was created 0600 and
     * owned by the broker's user, so no other user could connect() at all --
     * which made --allow-group a flag that could never fire: SO_PEERCRED never
     * got the chance to say yes, because the filesystem had already said no.
     * The two controls are complementary and both apply: the mode decides who
     * may reach the socket, SO_PEERCRED decides who may be served.
     */
    unsigned    socket_mode;    /* octal; default 0600                        */
    const char *socket_group;   /* chgrp the socket to this group, or NULL    */
    bool        no_peercred;    /* opt OUT of the credential allow-list       */
    int         socket_fd;      /* pre-bound listening fd, or -1              */

    /*
     * Clipboard.  DEFAULT OFF, deliberately.  Clipboard already needs opting
     * in twice (QEMU's -chardev qemu-vdagent, and spice-vdagent in the guest),
     * so a third gate costs little -- and it buys the property that matters:
     * with `consent` as a default, what a system actually does would depend on
     * whether vdagent happens to be installed, so you could not tell your
     * clipboard posture by reading the broker's configuration.  With `off`,
     * the flag IS the answer.
     *
     * The cost of a restrictive default is that friction pushes people to the
     * least safe setting, so every dead end here names `consent` explicitly
     * rather than leaving the reader to find the flag list.
     */
    int         clip_mode;
    struct nb_clip_trigger clip_trigger[NB_CLIP_MAX_TRIGGERS];
    int         n_clip_trigger;
    uid_t  allow_uid[NB_MAX_ALLOW_IDS];
    int    n_allow_uid;
    gid_t  allow_gid[NB_MAX_ALLOW_IDS];
    int    n_allow_gid;
    /* Start the window fullscreen, rather than waiting for CTRL+ALT+F.
     * Direct scanout is only possible fullscreen, so a measurement of it
     * should not depend on somebody pressing a key. */
    bool fullscreen;
    /* Keep the window and wait for another client when one disconnects.  The
     * default is to exit: a broker window outliving its VM is a window showing
     * nothing, and the socket is the VMM's to reconnect on. */
    bool persist;
    /* 0 = never scale the guest frame, 1 = always scale it to the window,
     * 2 = auto: 1:1 windowed, scaled to fit in fullscreen.  Default 2. */
    /*
     * How the guest's frame is mapped into the window.  "fit" is deliberately
     * NOT one of the names: in video players it conventionally means preserve
     * aspect and letterbox, while the user meant fill -- so the word would be
     * read backwards by half the people who saw it.
     */
    int  scale_mode;
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
    uint32_t seq;               /* the client's own frame counter (cmd.seq).
                                 * ADVISORY — it comes from the untrusted VMM
                                 * and is used for nothing but log lines, so a
                                 * lie in it costs a confusing trace and
                                 * nothing else.                              */
};

/* ── the policy core ─────────────────────────────────────────────────────── */
struct nb_session;

struct nb_sink {
    struct nb_session *sess;

    int      client_fd;         /* -1 when nobody is connected                */
    pid_t    client_pid;        /* from SO_PEERCRED, for log lines only       */
    uint64_t client_generation; /* changes across every attach/detach; async
                                 * backend work must match before delivery    */
    uint32_t seq;

    /*
     * The window has been sized once from the guest's own resolution.  After
     * that a guest re-mode changes the SOURCE, not the window -- see the
     * WINDOW case in nb_handle_cmd().
     */
    bool     window_established;
    /* Command-rate limiter (audit B-1b).  A legitimate VMM sends about three
     * commands per frame; anything orders of magnitude above that is a client
     * trying to burn the broker's CPU, not one trying to display. */
    uint64_t rate_ms;           /* start of the current 1s window          */
    unsigned rate_count;

    /*
     * CLIPBOARD, guest -> host.  Reassembled here rather than in a backend so
     * the bounds live next to every other client-input bound.  `n` is a chunk
     * count we keep ourselves; nothing the client sends decides how much we
     * store.
     */
    char     clip_in[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    unsigned clip_in_len;
    unsigned clip_in_chunks;
    uint32_t clip_in_next_chunk;
    bool     clip_in_active;
    bool     clip_in_bad;       /* this transfer already failed; drain it   */
    uint64_t clip_rate_ms;      /* separate 1s window for clipboard         */
    unsigned clip_rate_count;
    uint64_t clip_last_ms;      /* for the "guest changed your clipboard"
                                 * notice, so a burst says it once          */
    /* Each dead end is said ONCE per connection: a line per keystroke is
     * noise, and noise is why nobody reads the line that mattered. */
    bool     clip_told_off;
    bool     clip_told_noclient;
    bool     clip_told_noagent;
    unsigned clip_held_key;     /* paste key held until content is queued   */
    bool     clip_held_ctrl;
    bool     clip_held_shift;
    uint64_t clip_held_generation;
    unsigned caps_seen;         /* what the client told us it can do        */
    const struct nb_config *cfg;
    bool     focused;
    bool     pointer_in;
    bool     grabbed;
    bool     fullscreen;

    /* Modifier latch for the broker-owned hotkeys.  Tracked from the same key
     * stream the client sees, so the two cannot disagree about reality. */
    bool     ctrl_down, alt_down;
    /* Latched like ctrl/alt: a paste chord needs it, and it must track reality
     * even while unfocused, or the first chord after a focus change is missed. */
    bool     shift_down;

    /* Which keys the CLIENT currently believes are down.  On focus loss we
     * synthesise a release for each: a grab that ends with keys latched down
     * in the guest is both a bug and, for modifiers, a security surprise. */
    unsigned char key_down[768 / 8];
    unsigned char consumed[768 / 8]; /* broker-owned chord key-ups           */

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
/* The user closed the display.  Reports it to the client and returns; the
 * client decides what that means for the VM.  Returns false when nobody is
 * connected, in which case the caller should just quit -- there is no policy
 * to defer to. */
bool nb_sink_close_request(struct nb_sink *s, int action);
/* The BACKEND reporting what the display server actually did.  Fullscreen can
 * be entered and left without our hotkey -- a compositor binding, --fullscreen
 * at startup -- and the F_FULLSCREEN flag on every packet has to be the truth,
 * not our last request, because the client keys a guest mode switch off it. */
void nb_sink_set_fullscreen(struct nb_sink *s, bool on);
/* Is a VMM actually connected?  A backend asking "is there anything here to
 * shut down" -- with no client there is no policy to defer to. */
bool nb_sink_has_client(const struct nb_sink *s);
/* Drop the grab because the broker is about to show host UI.  A host dialog
 * while the guest holds the keyboard is incoherent, and it is the state a
 * stuck grab is least recoverable from. */
void nb_sink_force_ungrab(struct nb_sink *s);
/* Host clipboard text on its way to the guest, already size-checked.  The
 * generation captured when the fetch began must still name the current
 * connection and held paste operation.  Returns false for stale completion or
 * ring pressure, so neither can disclose or paste the wrong content. */
bool nb_sink_send_clipboard(struct nb_sink *s, uint64_t generation,
                            const char *text, size_t len);
/* A backend telling the core a paste trigger fired.  The core owns the policy:
 * which mode we are in, whether anyone is connected, and what to say when the
 * answer is no. */
void nb_sink_paste_trigger(struct nb_sink *s);
/* Complete an asynchronous host-selection fetch.  `generation` is the value
 * captured when it began; stale completion from an old persistent client is
 * ignored.  When `paste` is true a coherent, balanced paste chord is replayed
 * after the clipboard packets.  False cancels it visibly without pasting stale
 * guest content. */
void nb_sink_clip_finish(struct nb_sink *s, uint64_t generation, bool paste);
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

    /* Cancel connection-scoped asynchronous work before the sink generation
     * changes.  Optional on backends with no such work. */
    void (*client_detach)(struct nb_session *s, uint64_t generation);

    /*
     * Show the "no VM attached" placeholder.  Called once before the first
     * client can connect and again whenever one goes away, so that an idle
     * broker is visibly idle rather than an unmapped window.  Optional: a
     * backend that leaves this NULL simply shows nothing until a real frame
     * arrives, which is what every backend did before.
     */
    int  (*show_idle)(struct nb_session *s);
    /*
     * Cancel any modal UI the backend is showing.  OPTIONAL -- may be NULL,
     * and is on the X11 backend, which draws no dialog.  Called when the
     * broker takes a state that contradicts one (grab on, fullscreen on):
     * the later action wins, rather than leaving two modes fighting.
     */
    void (*dismiss_dialog)(struct nb_session *s);
    /* Clipboard.  OPTIONAL -- NULL on a backend that has no selection, in
     * which case clipboard is simply unavailable rather than half-working. */
    int  (*set_clipboard)(struct nb_session *s, const char *text, size_t len);
    void (*notify_clipboard)(struct nb_session *s);
    /* Begin an asynchronous read of the host selection.  The backend captures
     * `generation`, queues content with nb_sink_send_clipboard(), then calls
     * nb_sink_clip_finish() with the same generation. */
    int  (*fetch_clipboard)(struct nb_session *s, struct nb_sink *sink,
                            uint64_t generation);
};

#define NB_SESSION_CLIP_G2H (1u << 0)
#define NB_SESSION_CLIP_H2G (1u << 1)

struct nb_session {
    const struct nb_session_ops *ops;
    void    *priv;

    uint32_t width, height;     /* current window size, in pixels             */
    uint32_t caps;              /* NVKVM_BROKER_CAP_*                         */
    uint32_t clipboard_caps;    /* NB_SESSION_CLIP_* actually implemented     */
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
/* The opaque twin of an alpha format (AR24->XR24), or 0 if there is none.
 * Same byte layout; only the alpha channel's meaning differs. */
uint32_t nb_fourcc_opaque_twin(uint32_t fourcc);
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
/* What a client reports it supports.  Today only the clipboard bit exists;
 * a VMM with no clipboard agent must not be told to prepare a paste. */
#define NB_CLIENT_HAS_CLIPBOARD (1u << 0)

/* nb_config.scale_mode */
#define NB_SCALE_NONE    0      /* 1:1, centred, no scaling at all           */
#define NB_SCALE_STRETCH 1      /* fill the window, ignore aspect, distort   */
#define NB_SCALE_ASPECT  2      /* as large as fits, aspect kept, black bars */

/* The same 5x7 font, for the client-side title bar.  Uppercase-only. */
unsigned nb_placeholder_text_w(const char *s, unsigned scale);
void nb_placeholder_fill(uint32_t *px, unsigned w, unsigned h,
                         unsigned stride_px, uint32_t argb);
void nb_placeholder_text(uint32_t *px, unsigned w, unsigned h,
                         unsigned stride_px, unsigned x, unsigned y,
                         const char *s, unsigned scale, uint32_t argb);

#endif /* NVKVM_BROKER_H */
