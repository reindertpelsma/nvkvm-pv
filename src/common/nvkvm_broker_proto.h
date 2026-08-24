/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_broker_proto.h — the display-broker wire protocol (version 2).
 *
 * TWO DIRECTIONS, AND THEY ARE NOT SYMMETRIC.
 *
 * The broker runs as the user's desktop session and owns the window, the
 * keyboard grab and the compositor connection.  The VMM is sandboxed and is
 * treated as HOSTILE: this project has produced multiple guest→host bugs in
 * the QEMU-side VMM, including an arbitrary-address pin primitive reachable
 * from a guest kernel, and the whole point of the broker is that such a VMM
 * can be contained without giving up native zero-copy rendering.
 *
 *   broker → VMM   fixed-size EVENT packets (input, pacing, state).  The VMM
 *                  is the untrusted side here, so this direction carries no
 *                  fds, no lengths and nothing the broker has to be careful
 *                  about.
 *
 *   VMM → broker   fixed-size COMMAND records, one of which carries a single
 *                  dma-buf fd as SCM_RIGHTS ancillary data.  THIS is the
 *                  attack surface.  Everything in it is validated by the
 *                  privileged side against the real buffer and against what
 *                  the GPU actually advertises — see src/broker/README.md §3.
 *
 * Both records are fixed size, little-endian, and their sizes are compile-time
 * constants on both sides.  There is no length field anywhere and no
 * variable-length payload, so neither side ever parses a length an attacker
 * chose.  A short or oversized read is a framing error and MUST be treated as
 * a disconnect, never resynced: a resync is a parser, and a parser is the
 * thing this shape exists to avoid.
 *
 * The wire format is IDENTICAL on every backend.  A Wayland broker and an X11
 * broker are indistinguishable to the VMM — that is deliberate, so the VMM
 * cannot make a decision (or a bug) conditional on the host's display stack.
 */
#ifndef NVKVM_BROKER_PROTO_H
#define NVKVM_BROKER_PROTO_H

#include <stdint.h>

#define NVKVM_BROKER_PROTO_VERSION 2u

/* ── broker → VMM: events ────────────────────────────────────────────────── */

#define NVKVM_BROKER_PKT_SIZE      24u

/* Event types.  Values are frozen; append only. */
enum {
    NVKVM_BROKER_EV_HELLO     = 1,  /* w0=proto version, w1=capability bits.
                                     * Always the first packet on a connection */
    NVKVM_BROKER_EV_SURFACE   = 2,  /* x=width y=height of the broker's window.
                                     * Sent at attach and on every resize.     */
    NVKVM_BROKER_EV_FRAME     = 3,  /* the display is ready for another frame
                                     * (wl frame callback / PresentComplete).
                                     * This is the pacing signal; it is NOT a
                                     * request, and ignoring it only costs fps */
    NVKVM_BROKER_EV_RELEASE   = 4,  /* w0,w1 = low,high 32 bits of the buffer
                                     * id (its dma-buf inode).  The display is
                                     * no longer reading that buffer.          */
    NVKVM_BROKER_EV_KEY       = 5,  /* x=Linux evdev keycode (KEY_*), y=down   */
    NVKVM_BROKER_EV_BTN       = 6,  /* x=Linux evdev button (BTN_*),  y=down   */
    NVKVM_BROKER_EV_ABS       = 7,  /* x,y = position; w0,w1 = the range.
                                     * Sent ONLY when not grabbed and only
                                     * while the pointer is over the window.   */
    NVKVM_BROKER_EV_REL       = 8,  /* x=dx y=dy.  Sent ONLY when grabbed.     */
    NVKVM_BROKER_EV_WHEEL     = 9,  /* x=vertical detents, y=horizontal        */
    NVKVM_BROKER_EV_GRAB      = 10, /* x=1 grab on, 0 off.  The client should
                                     * switch pointing device on this edge.    */
    NVKVM_BROKER_EV_FOCUS     = 11, /* x=1 active, 0 inactive.  While 0 the
                                     * broker sends no KEY/BTN/ABS/REL at all  */
    NVKVM_BROKER_EV_POINTER   = 12, /* x=1 pointer over the window, 0 not      */
    NVKVM_BROKER_EV_BYE       = 13, /* broker is going away; x=reason code     */
    /*
     * EV_CLOSE — THE USER CLOSED THE DISPLAY.  The X button, or the
     * compositor's own close request (Alt+F4, a window-list close).
     *
     * It states a fact and requests nothing: the broker knows nothing about
     * VMs and has no business deciding what closing a window means for one.
     * The VMM applies its own policy -- an ACPI powerdown, a prompt, a
     * snapshot, or nothing at all.
     *
     * DELIBERATELY NOT A DIALOG IN THE BROKER.  The broker is the privileged
     * process holding the keyboard grab; putting dialog UI, hit-testing and
     * the state machine behind it in there buys attack surface for a decision
     * it cannot make anyway.
     *
     * The broker does NOT exit on sending this.  It exits when the client
     * disconnects (or keeps the window with --persist), so the VMM decides how
     * and when the display goes away.
     */
    NVKVM_BROKER_EV_CLOSE     = 14,
};

/*
 * EV_CLOSE.x — WHICH close the user asked for.  The broker asks the human and
 * reports the answer; what each one MEANS is still entirely the VMM's, which
 * is what keeps VM policy out of the privileged process.
 */
#define NVKVM_BROKER_CLOSE_POWERDOWN 0  /* graceful: the guest OS decides     */
#define NVKVM_BROKER_CLOSE_FORCE     1  /* stop the machine now               */

/*
 * Capability bits reported in HELLO (w1).  These describe what the backend on
 * THIS session can actually do, so a client — and a human reading the broker's
 * startup log — learns the truth before a grab is attempted, not during.
 *
 * A partial grab is acceptable.  A partial grab announced as a total one is
 * not: the user would believe their keystrokes are going to the guest when
 * some of them are still reaching the host.
 */
#define NVKVM_BROKER_CAP_KEYBOARD     (1u << 0) /* keyboard events at all      */
#define NVKVM_BROKER_CAP_ABS_POINTER  (1u << 1) /* absolute motion available   */
#define NVKVM_BROKER_CAP_REL_POINTER  (1u << 2) /* true relative motion        */
#define NVKVM_BROKER_CAP_POINTER_LOCK (1u << 3) /* pointer confined under grab */
#define NVKVM_BROKER_CAP_TOTAL_GRAB   (1u << 4) /* compositor/WM shortcuts are
                                                 * inhibited too.  Clear means
                                                 * SOME shortcuts still reach
                                                 * the host — see the log line */
#define NVKVM_BROKER_CAP_FOCUS_EVENTS (1u << 5) /* focus loss is observable —
                                                 * REQUIRED for grab to be
                                                 * offered at all              */
#define NVKVM_BROKER_CAP_FULLSCREEN   (1u << 6) /* CTRL+ALT+F does something   */
#define NVKVM_BROKER_CAP_DMABUF       (1u << 7) /* the session can accept
                                                 * dma-buf buffers.  Clear ⇒
                                                 * ATTACH will always fail     */
#define NVKVM_BROKER_CAP_MODIFIERS    (1u << 8) /* explicit format modifiers
                                                 * are negotiated.  Clear ⇒
                                                 * only DRM_FORMAT_MOD_INVALID
                                                 * (implicit) is accepted      */
#define NVKVM_BROKER_CAP_RELEASE      (1u << 9) /* EV_RELEASE is real, not
                                                 * synthesised                 */

/* BYE reason codes. */
enum {
    NVKVM_BROKER_BYE_SHUTDOWN     = 0,
    NVKVM_BROKER_BYE_DISPLAY_LOST = 1,  /* the compositor/X server went away  */
    NVKVM_BROKER_BYE_PROTOCOL     = 2,  /* the client violated the protocol   */
};

struct nvkvm_broker_pkt {
    uint16_t type;      /* NVKVM_BROKER_EV_*                                  */
    uint16_t flags;     /* NVKVM_BROKER_F_*                                   */
    uint32_t seq;       /* monotonic per connection; a gap means packets were
                         * dropped, which the broker never does silently for
                         * keys or buttons — it disconnects instead            */
    int32_t  x;
    int32_t  y;
    uint32_t w0;
    uint32_t w1;
};

/* Mirrored on every packet so a client can never disagree with the broker
 * about grab state, whatever it did with the GRAB event. */
#define NVKVM_BROKER_F_GRABBED  (1u << 0)
#define NVKVM_BROKER_F_FOCUSED  (1u << 1)

/* ── VMM → broker: commands ──────────────────────────────────────────────── */

#define NVKVM_BROKER_CMD_SIZE      40u

enum {
    /*
     * ATTACH — SCM_RIGHTS carries EXACTLY ONE fd, which must be a dma-buf.
     * The descriptor fields describe it.  The broker validates all of them
     * against the fd's real size before importing anything (README §3).
     * The fd is consumed: the broker imports it and closes its copy.
     */
    NVKVM_BROKER_CMD_ATTACH = 1,
    /*
     * COMMIT — present the most recently ATTACHed buffer.  Carries no fd and
     * no descriptor; every descriptor field must be zero.  Split from ATTACH
     * because a compositor distinguishes "the content changed" from "the
     * frame is finished", and committing a half-drawn buffer is visible.
     */
    NVKVM_BROKER_CMD_COMMIT = 2,
    /*
     * WINDOW — the guest changed resolution; ask for a window this size.
     * width/height only.  It is a REQUEST: the broker clamps it to its own
     * limits and the window manager may ignore it entirely.  The size that
     * actually took effect comes back as EV_SURFACE.
     */
    NVKVM_BROKER_CMD_WINDOW = 3,
};

/*
 * Explicitly laid out so every field is naturally aligned and the struct is
 * exactly 40 bytes with no compiler padding on any ABI either side may be
 * built for.  Checked by the assertion below, on both sides.
 */
struct nvkvm_broker_cmd {
    uint16_t type;      /* NVKVM_BROKER_CMD_*                       offset  0 */
    uint16_t reserved0; /* must be 0                                        2 */
    uint32_t width;     /*                                                  4 */
    uint32_t height;    /*                                                  8 */
    uint32_t stride;    /* bytes per row of plane 0                        12 */
    uint32_t offset;    /* byte offset of plane 0 within the dma-buf       16 */
    uint32_t fourcc;    /* DRM_FORMAT_*                                    20 */
    uint64_t modifier;  /* DRM_FORMAT_MOD_*                                24 */
    uint32_t seq;       /* client-side counter; advisory, logged only      32 */
    uint32_t reserved1; /* must be 0                                       36 */
};

/*
 * Single-plane only, on purpose.  The nvkvm guest head advertises XRGB8888 and
 * ARGB8888 (src/guest/nvkvm_kms.c nvkvm_pipe_formats[]) — both single-plane —
 * so multi-plane support would be untested code on the privileged side of the
 * boundary.  A multi-plane format is rejected as an unadvertised fourcc.
 */

/*
 * Largest window/buffer edge the broker will accept, in pixels.
 *
 * This is the same 8192 as NVKVM_PRESENT_MAX_DIM in
 * src/qemu/nvkvm_isolate_handlers.c, one process further out.  That bound
 * exists because finding S-3 caught guest-controlled geometry landing in the
 * VMM unexamined; the broker must not assume the VMM still enforces it,
 * because in this threat model the VMM is the attacker.
 */
#define NVKVM_BROKER_MAX_DIM   8192u

/* Both sides must agree byte for byte; a mismatch is a silent desync. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct nvkvm_broker_pkt) == NVKVM_BROKER_PKT_SIZE,
               "nvkvm_broker_pkt must be exactly 24 bytes");
_Static_assert(sizeof(struct nvkvm_broker_cmd) == NVKVM_BROKER_CMD_SIZE,
               "nvkvm_broker_cmd must be exactly 40 bytes");
#endif

#endif /* NVKVM_BROKER_PROTO_H */
