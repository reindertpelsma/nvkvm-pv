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
    /*
     * EV_CLIPBOARD — one fixed-size chunk of host clipboard text on its way to
     * the guest.  See "Clipboard framing" below for why it is chunked rather
     * than length-prefixed.
     */
    NVKVM_BROKER_EV_CLIPBOARD = 15,

    /*
     * Answer to NVKVM_BROKER_CMD_QUERY_FORMAT.  The question is echoed back so
     * the client needs no outstanding-query state and cannot mismatch a reply
     * to a request:
     *     x  = 1 the display can show it, 0 it cannot
     *     y  = the fourcc that was asked about
     *     w0 = modifier, low 32 bits
     *     w1 = modifier, high 32 bits
     *
     * x=1 is the same judgement ATTACH would make, taken through the same code
     * path -- including the opaque-twin substitution -- so an accepted answer
     * cannot be followed by a rejected frame.
     */
    NVKVM_BROKER_EV_FORMAT    = 16,
};

/* ── Clipboard framing ───────────────────────────────────────────────────── *
 *
 * Clipboard content is the first VARIABLE-LENGTH thing this protocol carries,
 * and docs/internal/audit-broker-client-2026-08-25.md names "no length field
 * anywhere to lie about" as the reason framing cannot be overrun by
 * construction.  Adding a length prefix would hand that property back.
 *
 * So it is not length-prefixed.  Every message stays EXACTLY the same size as
 * every other message of its direction, and content is split into fixed chunks
 * with an explicit end marker.  The reader still reads a constant number of
 * bytes and never consults a client-supplied number to decide how much to read.
 *
 * `info` is not a length field in that sense: it says how much of an
 * ALREADY-READ, fixed-size array is meaningful.  It is bounds-checked against
 * the array's compile-time size, and a lie in it cannot move the read cursor,
 * cannot allocate, and cannot reach past the struct.  The bound that matters --
 * total size -- is enforced as a CHUNK COUNT the receiver keeps itself, never
 * as a number the sender supplies.
 *
 * TEXT ONLY, UTF-8.  No images, no file lists, no arbitrary MIME: every one of
 * those is a decoder in the privileged process.
 */

/* Meaningful bytes in this chunk, and the end-of-transfer marker, packed into
 * one byte so `flags` can go on mirroring grab/focus state like every other
 * packet -- an invariant worth more than the byte it costs. */
#define NVKVM_BROKER_CLIP_NBYTES(info)  ((info) & 0x1fu)
#define NVKVM_BROKER_CLIP_LAST          0x20u

/* Payload bytes per message, each direction.  Chosen so the containing struct
 * is exactly the size that direction already uses. */
#define NVKVM_BROKER_CLIP_PKT_BYTES  15u   /* broker -> VMM, inside 24 */
#define NVKVM_BROKER_CLIP_CMD_BYTES  27u   /* VMM -> broker, inside 40 */

/*
 * The hard cap on one clipboard transfer, both directions.  A paste is text a
 * person selected; 7 KiB is far more than an ordinary paste and, unlike the
 * old 16 KiB claim, it provably fits the broker's fixed 512-packet outbound
 * ring with its four-packet control reserve.  Enforced as a chunk count on the
 * receiving side of each direction.
 */
#define NVKVM_BROKER_CLIP_MAX_BYTES  7168u
#define NVKVM_BROKER_CLIP_MAX_CHUNKS_PKT \
    ((NVKVM_BROKER_CLIP_MAX_BYTES + NVKVM_BROKER_CLIP_PKT_BYTES - 1u) / \
     NVKVM_BROKER_CLIP_PKT_BYTES)
#define NVKVM_BROKER_CLIP_MAX_CHUNKS_CMD \
    ((NVKVM_BROKER_CLIP_MAX_BYTES + NVKVM_BROKER_CLIP_CMD_BYTES - 1u) / \
     NVKVM_BROKER_CLIP_CMD_BYTES)

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
/*
 * The window is fullscreen.  Mirrored like the others, and load-bearing on
 * EV_SURFACE: it is what separates the two kinds of size change.
 *
 *   windowed resize  -> the host scales the buffer it already has.  The guest
 *                       is NOT told and MUST NOT re-mode: applications inside
 *                       would reflow, games would reinitialise swapchains, and
 *                       X clients would get a ConfigureNotify storm, all
 *                       because someone dragged a window edge.
 *   fullscreen       -> the size IS propagated, so the guest re-modes to the
 *                       output's resolution.  That is when you want it: 1:1
 *                       pixels, no scaling, and a surface that covers the
 *                       output, which is the compositor's condition for
 *                       promoting it to direct scanout.  It is also a
 *                       deliberate user action where a brief re-mode is
 *                       expected -- games do this routinely.
 */
#define NVKVM_BROKER_F_FULLSCREEN (1u << 2)

/*
 * EV_CLIPBOARD laid over the standard event packet.  Same 24 bytes, same
 * `type`/`flags`/`seq` in the same places -- so a receiver that does not know
 * this type still parses the header correctly and skips a whole, well-formed
 * message.  Ordering is the stream's; `seq` is the usual monotonic counter, so
 * a gap is already detectable.
 */
struct nvkvm_broker_clip_pkt {
    uint16_t type;      /* NVKVM_BROKER_EV_CLIPBOARD                        */
    uint16_t flags;     /* mirrored state, exactly as on every other packet */
    uint32_t seq;       /* monotonic per connection                         */
    uint8_t  info;      /* NVKVM_BROKER_CLIP_NBYTES / _LAST                 */
    uint8_t  data[NVKVM_BROKER_CLIP_PKT_BYTES];
};

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
    /*
     * CLIPBOARD — one fixed-size chunk of GUEST clipboard text.  Carries no
     * fd.  Accepted only in a mode that permits guest->host, only while the
     * window is focused, rate-limited, and capped by chunk count.
     *
     * The guest WRITING the host clipboard is the direction people
     * underestimate: you copy in the guest, paste into a host terminal, and
     * the guest planted something else.  That is why it is visible when it
     * happens rather than silent.
     */
    NVKVM_BROKER_CMD_CLIPBOARD = 4,
    /*
     * CAPS — what the VMM can do, sent once after connecting.  `width` carries
     * NVKVM_BROKER_CLIENT_* bits; every other field must be zero.
     *
     * It exists so "clipboard is enabled but nothing happens" can be told
     * apart from "the guest has no clipboard agent".  Those have completely
     * different fixes, and conflating them is how someone ends up turning the
     * mode up to `full` believing the mode was the problem.
     */
    NVKVM_BROKER_CMD_CAPS = 5,

    /*
     * "Can you display a buffer of this (fourcc, modifier)?"  Answered with
     * NVKVM_BROKER_EV_FORMAT.  Uses the ordinary command record: `fourcc` and
     * `modifier` are the question; every other field is ignored.
     *
     * WHY THIS EXISTS.  The guest renders in its GPU's native tiling, and on a
     * CROSS-VENDOR host -- guest on NVIDIA, compositor scanning out on an
     * Intel/AMD iGPU -- the compositor cannot import that at any price.
     * MEASURED on a hybrid laptop: Mutter advertised 28 (format, modifier)
     * pairs, every one of them Intel, LINEAR or INVALID, while the guest
     * presented NVIDIA block-linear 0x0300000000606014.  Every ATTACH was
     * refused and the window stayed black.
     *
     * The VMM cannot work that out for itself: the advertised set lives in the
     * broker, and an ATTACH rejection is not reported back (it drops the frame
     * and logs).  So it must be able to ASK, once, at mode-set -- and on "no"
     * fall back to reading the frame back through the guest's own GPU into a
     * LINEAR buffer that any compositor can take.
     *
     * Asked once per mode change, never per frame.
     */
    NVKVM_BROKER_CMD_QUERY_FORMAT = 6,
};

/*
 * ATTACH flags.
 *
 * F_SHM says the fd is a memfd to be presented from shared memory rather than
 * imported as a dma-buf.  How that happens is the backend's business: Wayland
 * wraps it in a wl_shm_pool, X11 pushes it with core-protocol PutImage.  The
 * point of the rung is that NEITHER can refuse it.
 * DECLARED rather than sniffed from the fd type: a memfd is a legitimate
 * carrier for other things -- the broker's own test client sends one for every
 * frame -- so inferring intent from st.f_type misclassifies honest clients.
 * The broker still CHECKS that the fd really is a memfd when this is set; the
 * flag states intent, the check enforces it.
 *
 * The field was `reserved0`, always zero, so an older client reads as "not
 * shm", which is what it meant.
 */
#define NVKVM_BROKER_CMD_F_SHM (1u << 0)
#define NVKVM_BROKER_CMD_F_ALL NVKVM_BROKER_CMD_F_SHM

/* NVKVM_BROKER_CMD_CAPS `width` bits. */
#define NVKVM_BROKER_CLIENT_CLIPBOARD (1u << 0)

/*
 * Explicitly laid out so every field is naturally aligned and the struct is
 * exactly 40 bytes with no compiler padding on any ABI either side may be
 * built for.  Checked by the assertion below, on both sides.
 */
struct nvkvm_broker_cmd {
    uint16_t type;      /* NVKVM_BROKER_CMD_*                       offset  0 */
    uint16_t flags;     /* NVKVM_BROKER_CMD_F_*; 0 for every older client   2 */
    uint32_t width;     /*                                                  4 */
    uint32_t height;    /*                                                  8 */
    uint32_t stride;    /* bytes per row of plane 0                        12 */
    uint32_t offset;    /* byte offset of plane 0 within the dma-buf       16 */
    uint32_t fourcc;    /* DRM_FORMAT_*                                    20 */
    uint64_t modifier;  /* DRM_FORMAT_MOD_*                                24 */
    uint32_t seq;       /* client-side counter; advisory, logged only      32 */
    uint32_t reserved1; /* must be 0                                       36 */
};

/* CMD_CLIPBOARD laid over the standard command record: same 40 bytes, same
 * `type` and `reserved0` in the same places. */
struct nvkvm_broker_clip_cmd {
    uint16_t type;      /* NVKVM_BROKER_CMD_CLIPBOARD */
    uint16_t flags;     /* must be 0 on this layout   */
    uint32_t chunk;     /* 0-based; bounded by the receiver's own count */
    uint32_t reserved1; /* must be 0                  */
    uint8_t  info;      /* NVKVM_BROKER_CLIP_NBYTES / _LAST */
    uint8_t  data[NVKVM_BROKER_CLIP_CMD_BYTES];
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
/* The clipboard overlays MUST be the same size as what they overlay, or the
 * "every message is fixed size" property is gone and the reader desynchronises
 * on the first clipboard chunk. */
_Static_assert(sizeof(struct nvkvm_broker_clip_pkt) == NVKVM_BROKER_PKT_SIZE,
               "clip_pkt must be exactly the event packet size");
_Static_assert(sizeof(struct nvkvm_broker_clip_cmd) == NVKVM_BROKER_CMD_SIZE,
               "clip_cmd must be exactly the command size");
_Static_assert(NVKVM_BROKER_CLIP_PKT_BYTES <= 0x1fu &&
               NVKVM_BROKER_CLIP_CMD_BYTES <= 0x1fu,
               "chunk payload must fit the 5-bit nbytes field");
#endif

#endif /* NVKVM_BROKER_PROTO_H */
