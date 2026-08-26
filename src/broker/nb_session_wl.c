/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_session_wl.c — the Wayland backend.
 *
 * DISPLAY.  An ordinary xdg_toplevel window.  The guest's scanout dma-buf
 * arrives from the VMM as an fd; we wrap it in a wl_buffer through
 * zwp_linux_dmabuf_v1 and attach it to our wl_surface.  Nothing is imported
 * into a GL context, nothing is blitted, and this file contains no GL call:
 * the compositor takes the guest's own allocation as the surface's content.
 *
 * That is one blit fewer than QEMU's GTK/SDL path, which imports the same
 * dma-buf as a texture and glBlitFramebuffer's it into a window framebuffer it
 * owns.  It is also the only arrangement under which the frame can reach a
 * hardware plane at all: a compositor promotes a surface to DIRECT SCANOUT
 * when it is fullscreen, opaque, unoccluded and its format/modifier suits the
 * plane.  That cannot be requested — only qualified for — which is why
 * CTRL+ALT+F is worth having.
 *
 * FORMATS.  The set of acceptable (fourcc, modifier) pairs is whatever the
 * compositor advertised on zwp_linux_dmabuf_v1, and nothing else.  We bind at
 * version 3 deliberately: from version 4 the compositor is required NOT to
 * send `format`/`modifier` events (feedback objects replace them), and a
 * broker that binds 4 would end up with an empty advertised set and reject
 * every frame.
 *
 * INPUT.  Three optional protocols, each independently absent:
 *   zwp_keyboard_shortcuts_inhibit_manager_v1  keyboard, including the
 *                                              compositor's own bindings
 *   zwp_pointer_constraints_v1 (locked pointer) pointer lock under grab
 *   zwp_relative_pointer_manager_v1             relative deltas under grab
 * Missing ones are reported at startup, reflected in the capability bits and
 * spelled out in grab_caveat — a grab that quietly leaks Super to the
 * compositor is exactly the failure a user finds mid-game.
 *
 * FOCUS.  wl_keyboard.leave is the focus-out signal.  It is what makes the
 * grab safe to offer at all: without it the grab would be unescapable, and an
 * unescapable grab is a keylogger.
 */
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvkvm_broker.h"

#ifndef NB_HAVE_WAYLAND

struct nb_session *nb_session_wayland(const struct nb_config *cfg)
{
    (void)cfg;
    nb_err("the Wayland backend was not compiled in (wayland-client or "
           "wayland-protocols missing at build time)");
    return NULL;
}

#else /* NB_HAVE_WAYLAND */

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* Defined here rather than pulled from libdrm: with the DRM-lease design gone
 * this backend has no other reason to link libdrm, and one constant is not
 * worth a dependency. */
#define NB_DRM_FORMAT_MOD_INVALID  0x00ffffffffffffffULL

#define NB_TB_H     28
#define NB_TB_BTN   34          /* width of one button, from the right edge  */

/* The close-confirmation overlay. */
#define NB_DLG_W     560
#define NB_DLG_BTN_H  40
#define NB_DLG_GAP     8
#define NB_DLG_PAD    18
#define NB_DLG_N       4        /* acpi / force / display-only / cancel       */
#define NB_DLG_TITLE  26
#define NB_DLG_H  (NB_DLG_PAD * 2 + NB_DLG_TITLE + NB_DLG_GAP + \
                   NB_DLG_N * (NB_DLG_BTN_H + NB_DLG_GAP))

static const char *const nb_dlg_label[NB_DLG_N] = {
    "SHUT DOWN THE GUEST (ACPI)",
    "FORCE OFF THE VM",
    "CLOSE THE DISPLAY ONLY",
    "CANCEL",
};
#define NB_CUR_N     5          /* arrow + four resize cursors            */
/*
 * Px of grabbable edge OUTSIDE the window.  Invisible and outside the visible
 * bounds, so width costs nothing but the ease of hitting it -- and 8 was hard
 * to hit, which is the whole purpose of the thing.
 */
#define NB_BORDER    14
#define NB_TITLE_MAX ((size_t)48)
/* Cursor slots: 0 arrow, 1 horizontal, 2 vertical, 3 NW-SE, 4 NE-SW. */
#define NB_CUR_ARROW 0
#define NB_CUR_EW    1
#define NB_CUR_NS    2
#define NB_CUR_NWSE  3
#define NB_CUR_NESW  4

static const uint32_t nb_bd_edge[8] = {
    XDG_TOPLEVEL_RESIZE_EDGE_TOP,
    XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
    XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
    XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
    XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
    XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
    XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT,
    XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
};
static const int nb_bd_cursor[8] = {
    NB_CUR_NS, NB_CUR_NS, NB_CUR_EW, NB_CUR_EW,
    NB_CUR_NWSE, NB_CUR_NESW, NB_CUR_NESW, NB_CUR_NWSE,
};

struct nb_wl_buf {
    bool      valid;
    uint64_t  id;                   /* dma-buf inode                          */
    struct wl_buffer *buf;
    uint32_t  w, h, stride, offset, fourcc;
    uint64_t  modifier;
    uint64_t  used;                 /* LRU tick                               */
    struct nb_wl *owner;
    /*
     * HELD BY THE COMPOSITOR.  Set when the buffer is committed to the
     * surface, cleared by wl_buffer.release.  A Wayland client must not touch
     * a buffer between those two points -- and here the party that touches it
     * is the GUEST, several processes away, which never sees the release.
     * Tracking it is what makes "the guest overwrote a buffer the compositor
     * was still reading" an observable event instead of a theory.
     */
    bool      held;
    uint32_t  seq;                  /* client frame counter at last commit    */
    uint64_t  commits;
};

/* Distinct pairs whose importability we remember.  Two occur in practice:
 * the guest's own tiling and the LINEAR fallback. */
/* DRM_FORMAT_ARGB8888, spelled locally like the XR24 one. */
#define NB_FOURCC_AR24_LOCAL 0x34325241u

#define NB_PROVEN_SLOTS 4

struct nb_wl {
    struct wl_display    *dpy;
    struct wl_registry   *reg;
    struct wl_compositor *comp;
    struct wl_seat       *seat;
    struct wl_keyboard   *kbd;
    struct wl_pointer    *ptr;
    struct wl_surface    *surf;
    struct xdg_wm_base   *wm_base;
    struct xdg_surface   *xdg_surf;
    struct xdg_toplevel  *toplevel;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    uint32_t              dmabuf_ver;

    /*
     * PAIRS PROVEN BY IMPORT, not by advertisement.
     *
     * A compositor can advertise a (fourcc, modifier) its own driver then
     * refuses to bind.  MEASURED on GNOME/Mutter with NVIDIA 595.84: it
     * advertises XR24 + LINEAR and answers the import with
     *   [destroyed object]: error 7: failed to import supplied dmabufs:
     *   Could not bind the given EGLImage to a CoglTexture2D
     * -- which is a WAYLAND PROTOCOL ERROR, so the connection dies and the
     * broker exits.  Under a restart policy that is a spin: OBSERVED 15
     * restarts in a few minutes on the physical box.
     *
     * So the first buffer of an unproven pair goes through the ASYNCHRONOUS
     * zwp_linux_buffer_params_v1.create, which reports failure as a `failed`
     * EVENT instead of killing the connection.  Once a pair is proven, every
     * later buffer uses create_immed, which is why the roundtrip is paid once
     * per pair and never on the steady-state frame path -- the objection the
     * create_immed comment below is about.
     */
    struct {
        uint32_t fourcc;
        uint64_t mod;
        int      state;       /* 0 unproven, 1 importable, -1 refused */
        bool     probing;
    } proven[NB_PROVEN_SLOTS];
    uint32_t probe_fourcc;   /* the pair the outstanding probe is about */
    uint64_t probe_mod;
    /* A probe that failed and the client has not been told about yet. */
    bool     probe_bad_pending;
    uint32_t probe_bad_fourcc;
    uint64_t probe_bad_mod;
    /* Only ever used for the idle placeholder.  The guest path never touches
     * shm — its frames are dma-bufs and are never copied. */
    struct wl_shm        *shm;
    struct wl_buffer     *idle_buf;
    void                 *idle_px;
    size_t                idle_sz;
    int                   idle_w, idle_h;
    bool                  idle_wanted;   /* show it as soon as configured */
    bool                  idle_shown;

    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;
    struct zwp_keyboard_shortcuts_inhibitor_v1       *inhibitor;
    struct zwp_pointer_constraints_v1                *constraints;
    struct zwp_locked_pointer_v1                     *lock;
    struct zwp_relative_pointer_manager_v1           *relptr_mgr;
    struct zwp_relative_pointer_v1                   *relptr;

    /* Window chrome.  Without this the toplevel is undecorated on Mutter:
     * no title bar, no close button, no resize grips — and the --title we
     * were given renders nowhere. */
    struct zxdg_decoration_manager_v1     *deco_mgr;
    struct zxdg_toplevel_decoration_v1    *deco;

    /* Resize.  A surface's size is its buffer's size unless a viewport says
     * otherwise; wp_viewporter is what decouples "the window the user
     * dragged" from "the resolution the guest is scanning out". */
    struct wp_viewporter *viewporter;
    struct wp_viewport   *viewport;

    /*
     * wp_presentation is the ONLY way a Wayland client learns whether its
     * buffer reached a hardware plane: the `presented` event carries
     * KIND_ZERO_COPY, which is set exactly when the compositor scanned the
     * client's own buffer out instead of compositing a copy of it.  That is
     * the same question the X11 backend answers with Present: FLIP / COPY,
     * and until this was wired up the Wayland side could not answer it at all.
     */
    struct wp_presentation *presentation;
    int                     last_scanout;   /* -1 unknown, 0 copy, 1 zero-copy */

    /*
     * THE OUTPUT'S SCALE, IN 120THS -- and it is not cosmetic.
     *
     * A Wayland surface is measured in LOGICAL pixels.  On this GNOME the
     * output is 3840x2160 at scale 1.5, so a fullscreen surface measures
     * 2560x1440 -- and telling the guest to render 2560x1440 would produce a
     * buffer that does NOT cover the 3840x2160 output, which is exactly the
     * condition the compositor requires before it will scan it out directly.
     * The guest must be told the PHYSICAL size.
     *
     * 120 is the protocol's fixed-point base, and 120 (i.e. 1.0) is the right
     * default: a compositor that does not offer wp_fractional_scale_v1 is
     * telling us logical and physical are the same thing.
     */
    struct wp_fractional_scale_manager_v1 *frac_mgr;
    struct wp_fractional_scale_v1         *frac;
    uint32_t                               scale_120;

    /* Client-side title bar.  A SUBSURFACE ABOVE the content, at negative y,
     * so it never overlaps the guest's picture -- an occluded surface is not a
     * scanout candidate, and the whole point of this backend is that it can
     * be one. */
    struct wl_subcompositor *subcomp;
    struct wl_surface       *tb_surf;
    struct wl_subsurface    *tb_sub;
    /*
     * THE CLOSE CONFIRMATION.  A subsurface above the content, drawn with the
     * same shm + 5x7 font the title bar and placeholder already use -- no
     * toolkit, no new dependency, and the hit-testing is the same shape as the
     * title bar's three buttons.
     *
     * It does NOT decide anything about the VM.  Three of its four choices
     * send a message and let QEMU act; the fourth (close the display only) is
     * purely local to the broker and touches no guest.  The privileged process
     * still holds no VM policy -- it asks the human which message to send.
     */
    struct wl_surface       *dlg_surf;
    struct wl_subsurface    *dlg_sub;
    struct wl_buffer        *dlg_buf;
    void                    *dlg_px;
    size_t                   dlg_sz;
    bool                     dlg_open;
    /* We have already sent a close request and are still here.  The broker
     * cannot know WHY -- that is the VMM's and the guest's business -- but
     * "you already asked and the window is still open" is its own history and
     * is worth saying, because the QEMU log where the reason appears is not
     * something the person clicking the button can see. */
    bool                     close_asked;
    bool                     ptr_on_dlg;
    bool                     ptr_on_content;   /* over the guest's picture */
    int                      dlg_hover, dlg_press;
    int                      dlg_px_x, dlg_px_y;
    struct wl_buffer        *tb_buf;
    void                    *tb_px;
    size_t                   tb_sz;
    int                      tb_w;
    bool                     ptr_on_tb;
    bool                     tb_mapped;
    int                      tb_hover;      /* button under the pointer, -1 */
    int                      tb_press;      /* button held down, -1 for none */
    /* Our own pointer cursor, for when the pointer is on our chrome.  Drawn
     * here rather than loaded: libwayland-cursor would be a new dependency
     * and a theme lookup, for one arrow that is only ever seen over a 28px
     * bar. */
    struct wl_surface       *cur_surf[NB_CUR_N];
    /* Per-cursor hotspot, in SURFACE coordinates (so already magnified).  It
     * used to be a single hardcoded 6,5 for all four resize cursors while
     * cur_make() took the real ones and dropped them -- so the glyph was drawn
     * offset from the actual pointer, and for the 7px-wide bottom cursor 6 was
     * nearly off the image.  Aiming with a cursor that lies about where it
     * points is most of "the resize edge is hard to hit". */
    int                      cur_hx[NB_CUR_N], cur_hy[NB_CUR_N];
    struct wl_buffer        *cur_buf[NB_CUR_N];
    void                    *cur_px[NB_CUR_N];
    size_t                   cur_sz[NB_CUR_N];

    /*
     * RESIZE BORDERS.  A Wayland client has no window edges: the compositor
     * draws no frame and the surface stops exactly where the content does, so
     * there is nothing outside it to grab.  These eight surfaces ARE the
     * edges -- invisible (a single transparent pixel stretched by a viewport),
     * present only to take a pointer and turn a drag into
     * xdg_toplevel_resize().
     */
    /*
     * LETTERBOX BACKGROUND.  In aspect mode the viewport destination is
     * smaller than the window in one dimension, and Wayland has no background
     * colour -- something has to actually occupy the remainder or the desktop
     * shows through.  This is one opaque black pixel stretched by its own
     * viewport, placed BELOW the parent surface (the protocol allows the
     * parent as place_below's sibling), so the guest's frame stays exactly
     * where it was: on w->surf, attached directly, still a direct-scanout
     * candidate.  Mapped ONLY when there are bars to draw.
     */
    struct wl_surface    *bg_surf;
    struct wl_subsurface *bg_sub;
    struct wp_viewport   *bg_vp;
    struct wl_buffer     *bg_buf;
    void                 *bg_px;
    bool                  bg_mapped;
    int                   off_x, off_y;   /* content offset inside the window */

    /*
     * What bd_layout() last positioned the borders from.  They are children of
     * the CONTENT surface and offset by the letterbox, so they depend on
     * off_x/off_y -- which bg_layout() computes from inside wl_viewport_apply(),
     * on the per-frame path as well as the resize path.  Recording the inputs
     * lets bd_layout() be called from both and do nothing when nothing moved.
     */
    int                   bd_at_ox, bd_at_oy, bd_at_w, bd_at_h;
    bool                  bd_at_tb, bd_at_fs, bd_at_valid;
    struct wl_surface    *bd_surf[8];
    struct wl_subsurface *bd_sub[8];
    struct wp_viewport   *bd_vp[8];
    struct wl_buffer     *bd_buf;       /* one transparent pixel, shared */
    void                 *bd_px;
    int                   bd_hot;       /* border under the pointer, -1 none */
    int                   fit_w, fit_h; /* aspect-fit size when scaling      */
    int                   scale_mode;   /* 0 never, 1 always, 2 auto         */
    bool                     maximized;
    int                      tb_px_x, tb_px_y;   /* pointer, title-bar-local */
    uint32_t                 last_serial;
    char                     title[128];
    bool                     quit;

    /*
     * CLIPBOARD.  wl_data_device is core Wayland, so no extra protocol XML.
     *
     * The compositor only delivers a selection offer to the FOCUSED client,
     * which is not a limitation to work around -- it is the same rule the
     * clipboard policy states, enforced a layer down.  wlr-data-control would
     * read the selection while unfocused; GNOME does not offer it, and wanting
     * it would mean the design had drifted.
     */
    struct wl_data_device_manager *ddm;
    struct wl_data_device         *ddev;
    struct wl_data_offer          *offer;      /* current selection, if text */
    bool                           offer_text; /* it advertised our mime     */
    /* data_offer precedes the selection/enter event that says what the object
     * is for.  Keep that candidate separate so an ignored DnD offer cannot
     * replace the clipboard selection. */
    struct wl_data_offer          *pending_offer;
    bool                           pending_offer_text;
    struct wl_data_source         *source;     /* ours, when we own it       */
    char                          *src_text;   /* what we would send         */
    size_t                         src_len;
    int                            fetch_fd;   /* pipe being read, or -1     */
    uint64_t                       fetch_generation;
    uint64_t                       clip_notice_until;  /* title-bar notice   */
    /*
     * A guest copy is only PUT ON THE HOST CLIPBOARD while this window has
     * keyboard focus.  Wayland enforces half of that anyway -- setting a
     * selection needs a serial from a real input event -- but the policy is
     * the point: a background VM must not be able to reach across and replace
     * what you just copied somewhere else.
     *
     * Dropping it instead would be worse than it sounds: the guest believes
     * the copy succeeded, so nothing re-sends it, and the text only reappears
     * if the user copies something ELSE in the guest.  So hold the last one
     * and apply it on focus-in.
     */
    bool                           focused;
    char                          *clip_pending;
    size_t                         clip_pending_len;
    char                           fetch_buf[NVKVM_BROKER_CLIP_MAX_BYTES + 1];
    size_t                         fetch_len;

    struct nb_formats formats;

    /* The sink is not reachable from Wayland callbacks otherwise.  NULL until
     * the first dispatch(), so every callback must tolerate that. */
    struct nb_sink *sink;
    struct nb_session *sess;
    /* The pollfd main filled for us, so dispatch can tell whether the
     * compositor fd is actually readable.  Valid only between a
     * wl_pollfds() and the dispatch() that follows it. */
    struct pollfd *pfd;
    bool   flush_blocked;       /* the compositor socket would not take it all */

    struct nb_wl_buf bufs[NB_MAX_BUFS];
    uint64_t tick;
    int      pending;           /* index of the ATTACHed-but-not-COMMITted buf */
    int      current;           /* index of the buffer the surface holds       */
    bool     frame_inflight;
    uint64_t n_reuse_inflight;  /* commits of a buffer the compositor still
                                 * held -- see wl_commit()                     */

    /* THREE sizes, deliberately not one:
     *   buf_w/buf_h   the guest's scanout buffer — the resolution it chose
     *   win_w/win_h   what the compositor last configured for the window
     *   surf_w/surf_h what the surface currently measures, which is win_*
     *                 when a viewport is scaling and buf_* when it is not
     * Conflating them is what makes a user's window drag turn into a guest
     * mode switch, which is not what dragging a window means. */
    int    buf_w, buf_h;
    int    win_w, win_h;
    int    surf_w, surf_h;
    /* A FOURTH, and it is the only one EV_SURFACE may ever carry: the window's
     * CONTENT size -- win_* less the decorations we draw.  surf_* cannot serve
     * because, per the note above, it becomes buf_* whenever no viewport is
     * scaling: reporting it then tells the guest to render the size it just
     * rendered, and on a fullscreen window the two disagree, so they trade
     * places forever.  OBSERVED on a 4K display at scale 1.5: the guest flipped
     * between 1920x1080 and 3760x2118 about every two seconds, indefinitely.
     * (2160 - 42 is the 28px title bar at 1.5x, hence the odd number.) */
    int    cont_w, cont_h;
    /* The last hint actually sent, so an unchanged one is not resent. */
    unsigned hint_w, hint_h;
    /* The host output's refresh in MILLIHERTZ, learned from presentation
     * feedback.  0 until the first frame has been presented -- there is no
     * other source: the broker does not bind wl_output. */
    unsigned refresh_mhz;
    uint32_t deco_mode;
    bool   grabbed;
    bool   fullscreen;
    bool   configured;
};

/* Forward: the title bar lives further down, but wl_commit sizes it. */
static void tb_update(struct nb_wl *w, int width);
/* Forward: the cursor is built lazily, on first entry to our own chrome. */
static void cur_build(struct nb_wl *w);
/* Forward: hide or show the host cursor over the content, per what is on it. */
static void cur_apply(struct nb_wl *w, uint32_t serial);
/* Forward: the clipboard notice needs a clock and lives with the clipboard. */
static uint64_t nb_now_ms_wl(void);
/* Forward: the invisible resize borders, laid out on every size change. */
static void bd_build(struct nb_wl *w);
/* Forward: the letterbox bars, sized from the content rect wl_viewport_apply
 * just computed.  Defined next to the rest of the shm chrome. */
static void bg_layout(struct nb_wl *w, int dw, int dh);
/* Forward: EV_SURFACE reports PHYSICAL pixels; defined with the scale code. */
static void wl_report_surface(struct nb_wl *w, int lw, int lh);
static void bd_layout(struct nb_wl *w);
static int  bd_index(const struct nb_wl *w, const struct wl_surface *s);
/* Forward: presentation feedback is requested by wl_commit, defined below. */
extern const struct wp_presentation_feedback_listener nb_pres_listener;

/* ── buffers ─────────────────────────────────────────────────────────────── */

static void buf_release(void *data, struct wl_buffer *b)
{
    struct nb_wl_buf *slot = data;
    (void)b;

    /*
     * The compositor has finished reading this buffer.  Telling the client is
     * what lets it recycle without guessing; it is advisory (nvkvm's guest
     * cycles its own bos regardless), which is why a missed release only costs
     * the client an optimisation.
     */
    slot->held = false;
    if (slot->owner->sink) {
        nb_sink_release(slot->owner->sink, slot->id);
    }
}
static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void wl_buf_destroy(struct nb_wl *w, int i)
{
    if (!w->bufs[i].valid) {
        return;
    }
    if (w->bufs[i].buf) {
        wl_buffer_destroy(w->bufs[i].buf);
    }
    memset(&w->bufs[i], 0, sizeof(w->bufs[i]));
}

/* ── ops: format policy ──────────────────────────────────────────────────── */

static bool wl_format_ok(struct nb_session *s, uint32_t fourcc, uint64_t mod)
{
    struct nb_wl *w = s->priv;

    /*
     * HARDENING 3, verbatim: what the GPU advertises, not a list we wrote.
     * When the compositor could only tell us formats (dmabuf version 2, no
     * `modifier` event) the only thing it has committed to is the implicit
     * layout, so an explicit modifier is not something it agreed to accept.
     */
    if (!(s->caps & NVKVM_BROKER_CAP_MODIFIERS)) {
        return mod == NB_DRM_FORMAT_MOD_INVALID &&
               nb_formats_has(&w->formats, fourcc, NB_DRM_FORMAT_MOD_INVALID);
    }
    return nb_formats_has(&w->formats, fourcc, mod);
}

/* ── ops: attach / commit ────────────────────────────────────────────────── */

/* Slot for this pair, allocating one if there is room. */
static int wl_pair_slot(struct nb_wl *w, uint32_t fourcc, uint64_t mod,
                        bool create)
{
    int free_slot = -1;

    for (int i = 0; i < NB_PROVEN_SLOTS; i++) {
        if (w->proven[i].fourcc == fourcc && w->proven[i].mod == mod &&
            (w->proven[i].state || w->proven[i].probing)) {
            return i;
        }
        if (free_slot < 0 && !w->proven[i].state && !w->proven[i].probing) {
            free_slot = i;
        }
    }
    if (!create || free_slot < 0) {
        return -1;
    }
    w->proven[free_slot].fourcc = fourcc;
    w->proven[free_slot].mod    = mod;
    return free_slot;
}

/*
 * The probe answered.  `created` means this display really can import the pair;
 * `failed` means it advertised something it cannot bind, and the whole point of
 * probing is that we learn this WITHOUT the connection being torn down.
 *
 * The probe buffer itself is discarded either way.  Showing it would mean
 * threading a deferred attach through the commit path for one frame's benefit;
 * dropping it costs a single frame the first time a pair is seen.
 */
static void probe_created(void *data, struct zwp_linux_buffer_params_v1 *params,
                          struct wl_buffer *buf)
{
    struct nb_wl *w = data;
    int i = wl_pair_slot(w, w->probe_fourcc, w->probe_mod, false);

    if (i >= 0) {
        w->proven[i].state   = 1;
        w->proven[i].probing = false;
        nb_log("this display CAN import %.4s modifier 0x%016llx — proven by "
               "import, not by advertisement",
               (const char *)&w->proven[i].fourcc,
               (unsigned long long)w->proven[i].mod);
    }
    wl_buffer_destroy(buf);
    zwp_linux_buffer_params_v1_destroy(params);
}

static void probe_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    struct nb_wl *w = data;
    int i = wl_pair_slot(w, w->probe_fourcc, w->probe_mod, false);

    if (i >= 0) {
        w->proven[i].state   = -1;
        w->proven[i].probing = false;
        nb_err("this display ADVERTISED %.4s modifier 0x%016llx and then "
               "refused to import it.  Frames in that layout will be rejected "
               "rather than retried; the VMM is being told so it can send "
               "something else.",
               (const char *)&w->proven[i].fourcc,
               (unsigned long long)w->proven[i].mod);
        w->probe_bad_pending = true;
        w->probe_bad_fourcc  = w->proven[i].fourcc;
        w->probe_bad_mod     = w->proven[i].mod;
    }
    zwp_linux_buffer_params_v1_destroy(params);
}

static const struct zwp_linux_buffer_params_v1_listener probe_listener = {
    .created = probe_created,
    .failed  = probe_failed,
};

static int wl_attach(struct nb_session *s, const struct nb_buf_desc *d)
{
    struct nb_wl *w = s->priv;
    struct zwp_linux_buffer_params_v1 *params;
    struct wl_buffer *b;
    int i, victim = -1;
    uint64_t oldest = UINT64_MAX;

    w->tick++;

    /* Already imported?  Same reasoning as the import cache in
     * nvkvm_present_egl.c: the guest cycles a handful of scanout bos, and the
     * dma-buf inode is their stable identity across dup(2) and SCM_RIGHTS. */
    for (i = 0; i < NB_MAX_BUFS; i++) {
        struct nb_wl_buf *sl = &w->bufs[i];

        if (sl->valid && sl->id == d->id && sl->w == d->width &&
            sl->h == d->height && sl->stride == d->stride &&
            sl->offset == d->offset && sl->fourcc == d->fourcc &&
            sl->modifier == d->modifier) {
            sl->used = w->tick;
            sl->seq  = d->seq;
            w->pending = i;
            return 0;
        }
    }

    /* Pick a victim.  Never the buffer the surface is holding and never the
     * one already staged, or the compositor is left reading freed content. */
    for (i = 0; i < NB_MAX_BUFS; i++) {
        if (i == w->current || i == w->pending) {
            continue;
        }
        if (!w->bufs[i].valid) {
            victim = i;
            break;
        }
        if (w->bufs[i].used < oldest) {
            oldest = w->bufs[i].used;
            victim = i;
        }
    }
    if (victim < 0) {
        return -ENOSPC;         /* cannot happen with NB_MAX_BUFS >= 3 */
    }

    /*
     * TIER 3: SHARED MEMORY.  The VMM sends a memfd when the dma-buf tiers have
     * been refused.  wl_shm is a core Wayland global -- every compositor must
     * implement it -- so this cannot fail for want of import support, which is
     * exactly why it is the floor under a display that advertises a modifier it
     * will not bind.  It costs the compositor an upload per frame; that is the
     * price of universality and the reason the VMM tries it last.
     *
     * No modifier is involved and none was checked: shm is linear by
     * definition, and the geometry bounds nb_validate_desc() already enforced
     * are what keep the compositor from reading past the mapping.
     */
    if (d->is_shm) {
        struct wl_shm_pool *pool;
        struct wl_buffer *sb;
        size_t need = (size_t)d->stride * d->height + d->offset;

        if (!w->shm) {
            nb_err("ATTACH: this compositor offers no wl_shm, so there is no "
                   "buffer type left to try");
            return -EINVAL;
        }
        pool = wl_shm_create_pool(w->shm, d->fd, (int32_t)need);
        if (!pool) {
            return -EIO;
        }
        sb = wl_shm_pool_create_buffer(pool, (int32_t)d->offset,
                                       (int32_t)d->width, (int32_t)d->height,
                                       (int32_t)d->stride,
                                       d->fourcc == NB_FOURCC_AR24_LOCAL
                                           ? WL_SHM_FORMAT_ARGB8888
                                           : WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        if (!sb) {
            return -EIO;
        }
        {
            static bool told;

            if (!told) {
                told = true;
                nb_log("presenting through wl_shm: the display refused every "
                       "dma-buf layout offered, so frames are shared as plain "
                       "memory.  This works everywhere and costs the "
                       "compositor one upload per frame.");
            }
        }
        wl_buf_destroy(w, victim);
        w->bufs[victim] = (struct nb_wl_buf){
            .valid = true, .id = d->id, .buf = sb,
            .w = d->width, .h = d->height, .stride = d->stride,
            .offset = d->offset, .fourcc = d->fourcc, .modifier = d->modifier,
            .used = w->tick, .owner = w, .seq = d->seq,
        };
        wl_buffer_add_listener(sb, &buf_listener, &w->bufs[victim]);
        w->pending = victim;
        return 0;
    }

    /*
     * ADVERTISED IS NOT IMPORTABLE.  Gate on what this display has actually
     * proven it can bind; see the `proven` table's comment for the Mutter case
     * that makes this necessary.
     */
    {
        int ps = wl_pair_slot(w, d->fourcc, d->modifier, true);

        if (ps >= 0 && w->proven[ps].state == -1) {
            return -EINVAL;         /* known-refused: reject, do not retry */
        }
        if (ps >= 0 && w->proven[ps].probing) {
            return 0;               /* a probe is in flight; nothing to show */
        }
        if (ps >= 0 && w->proven[ps].state == 0) {
            /* First buffer of this pair: probe it asynchronously, so a refusal
             * arrives as an event rather than as a fatal protocol error. */
            struct zwp_linux_buffer_params_v1 *pp =
                zwp_linux_dmabuf_v1_create_params(w->dmabuf);

            if (!pp) {
                return -EIO;
            }
            zwp_linux_buffer_params_v1_add(pp, d->fd, 0, d->offset, d->stride,
                                           (uint32_t)(d->modifier >> 32),
                                           (uint32_t)(d->modifier & 0xffffffffu));
            zwp_linux_buffer_params_v1_add_listener(pp, &probe_listener, w);
            w->probe_fourcc      = d->fourcc;
            w->probe_mod         = d->modifier;
            w->proven[ps].probing = true;
            zwp_linux_buffer_params_v1_create(pp, (int32_t)d->width,
                                              (int32_t)d->height, d->fourcc,
                                              0 /* flags */);
            nb_log("probing whether this display can import %.4s modifier "
                   "0x%016llx (one frame is dropped to find out)",
                   (const char *)&d->fourcc,
                   (unsigned long long)d->modifier);
            w->pending = -1;
            return 0;
        }
    }

    params = zwp_linux_dmabuf_v1_create_params(w->dmabuf);
    if (!params) {
        return -EIO;
    }
    zwp_linux_buffer_params_v1_add(params, d->fd, 0, d->offset, d->stride,
                                   (uint32_t)(d->modifier >> 32),
                                   (uint32_t)(d->modifier & 0xffffffffu));
    /*
     * create_immed, not create: `create` answers asynchronously, and waiting
     * for the answer means a roundtrip to the compositor inside the path that
     * also carries input.  Input must never block on rendering — this project
     * shipped that bug once already.  create_immed reports failure as a
     * protocol error on the params object instead, which we surface as a lost
     * display rather than as a stall.
     */
    b = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)d->width,
                                                (int32_t)d->height, d->fourcc,
                                                0 /* flags */);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!b) {
        return -EIO;
    }

    wl_buf_destroy(w, victim);
    w->bufs[victim] = (struct nb_wl_buf){
        .valid = true, .id = d->id, .buf = b,
        .w = d->width, .h = d->height, .stride = d->stride,
        .offset = d->offset, .fourcc = d->fourcc, .modifier = d->modifier,
        .used = w->tick, .owner = w, .seq = d->seq,
    };
    wl_buffer_add_listener(b, &buf_listener, &w->bufs[victim]);
    w->pending = victim;
    return 0;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    struct nb_wl *w = data;
    (void)t;

    wl_callback_destroy(cb);
    w->frame_inflight = false;
    if (w->sink) {
        nb_sink_frame(w->sink);
    }
}

/*
 * THE ONE PLACE THE VIEWPORT DESTINATION IS DECIDED.
 *
 * It used to be decided in two -- once in wl_commit() for a new frame, once in
 * top_configure() for a resize -- and the two disagreed in exactly one case,
 * which is the case a user hits constantly:
 *
 *   top_configure() only touched the viewport when SCALING WAS WANTED
 *   (scale_mode 1, or scale_mode 2 while fullscreen).  Leaving fullscreen
 *   clears w->fullscreen, so the condition went false and the destination was
 *   left AT THE FULLSCREEN SIZE.  The frame stayed 3840x2160 while the window
 *   frame shrank, so the guest's desktop spilled across the whole screen with
 *   the host visible in a strip down the side.
 *
 * Entering and leaving fullscreen are the same operation with different
 * numbers, so they now run the same code.  A destination that equals the
 * buffer is UNSET rather than set 1:1, because a viewport scaling by exactly
 * one is still a viewport and a compositor deciding whether a surface may go
 * straight to a hardware plane is entitled to refuse anything carrying one.
 *
 * Returns the resulting surface size in out_w / out_h.
 */
static void wl_viewport_apply(struct nb_wl *w, int bw, int bh,
                              int *out_w, int *out_h)
{
    /*
     * A RESIZE NEVER REACHES THE GUEST.  All three modes are a viewport
     * destination and an offset: no buffer is reallocated, no round trip is
     * made, and the guest goes on believing it is the size it chose -- so
     * nothing inside it reflows because someone dragged a window edge.  The
     * one case where the guest IS told is fullscreen, and that is decided in
     * the relay off NVKVM_BROKER_F_FULLSCREEN, not here.
     */
    int dw, dh;

    if (bw <= 0 || bh <= 0) {
        bw = w->win_w > 0 ? w->win_w : 1;
        bh = w->win_h > 0 ? w->win_h : 1;
    }

    if (w->scale_mode == NB_SCALE_NONE || w->win_w <= 0 || w->win_h <= 0) {
        /*
         * 1:1.  The surface IS the buffer, whatever the window is.  The window
         * is still the user's -- we do not force it to the buffer size.
         *
         * THIS USED TO SNAP w->win_w/win_h BACK TO THE BUFFER, which is what
         * made dragging a window smaller do nothing: the compositor granted
         * the smaller size and we immediately restored the guest's resolution.
         * The resize request and its serial were never the problem.
         */
        dw = bw;
        dh = bh;
    } else if (w->scale_mode == NB_SCALE_STRETCH) {
        /* Fill it, aspect be damned.  No bars, ever. */
        dw = w->win_w;
        dh = w->win_h;
    } else {
        /*
         * ASPECT.  As large as fits, aspect kept, remainder blank.
         *
         * Both branches are computed from the SAME rounding so a window one
         * pixel off does not produce a bar on one side only, and -- the case
         * that decides direct scanout -- a window whose aspect MATCHES the
         * buffer's must come out exactly equal to the window, with no bars at
         * all.  Otherwise the surface fails to cover the output and the
         * compositor quietly declines to promote it, and fullscreen stays COPY
         * for a reason nothing logs.
         */
        long by_h = (long)w->win_h * bw / bh;   /* width if we fit by height */

        if (by_h <= (long)w->win_w) {
            dw = (int)by_h;
            dh = w->win_h;
        } else {
            dw = w->win_w;
            dh = (int)((long)w->win_w * bh / bw);
        }
        /* Snap away a rounding remainder of one: an exact-aspect window must
         * produce an exact cover, not a 1px bar that costs direct scanout. */
        if (dw > w->win_w - 2 && dw < w->win_w + 2) {
            dw = w->win_w;
        }
        if (dh > w->win_h - 2 && dh < w->win_h + 2) {
            dh = w->win_h;
        }
    }
    if (dw <= 0 || dh <= 0) {
        dw = bw;
        dh = bh;
    }
    w->fit_w = dw;
    w->fit_h = dh;

    if (w->viewport) {
        if (dw == bw && dh == bh) {
            wp_viewport_set_destination(w->viewport, -1, -1);
        } else {
            wp_viewport_set_destination(w->viewport, dw, dh);
        }
    }
    bg_layout(w, dw, dh);
    *out_w = dw;
    *out_h = dh;
}

static int wl_commit(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_wl *w = s->priv;
    struct nb_wl_buf *sl;
    struct wl_callback *cb;
    int new_w, new_h;

    /*
     * A probe came back refused.  Told here rather than from the listener
     * because this is where the sink is in scope, and it is called often
     * enough that the news is never more than a frame late.
     */
    if (w->probe_bad_pending) {
        w->probe_bad_pending = false;
        nb_sink_format_verdict(sink, w->probe_bad_fourcc, w->probe_bad_mod,
                               false);
    }

    if (w->pending < 0) {
        return -ENOENT;         /* COMMIT with nothing attached; not fatal */
    }
    sl = &w->bufs[w->pending];
    if (!sl->valid) {
        w->pending = -1;
        return -ENOENT;
    }

    /*
     * THE FRAME-GLITCH DETECTOR.
     *
     * `held` means the compositor took this buffer and has not sent
     * wl_buffer.release for it.  Seeing it set here, for a buffer that is NOT
     * the one currently on the surface, means the guest has cycled its whole
     * scanout ring and come back to a buffer the compositor is still reading
     * -- so the guest has been rendering into it underneath the compositor.
     * That is exactly the "light, intermittent, stale-looking frame" a user
     * reports, and it is a real correctness bug rather than a cosmetic one.
     *
     * It is DETECTED and counted here, not fixed here: the broker cannot stop
     * the guest from drawing.  The fix is backpressure, and it has to reach
     * the guest -- see nb_sink_release() and the relay.
     */
    if (sl->held && w->current != w->pending) {
        w->n_reuse_inflight++;
        if (nb_trace_frames || w->n_reuse_inflight <= 8 ||
            (w->n_reuse_inflight % 256) == 0) {
            nb_log("frame: REUSE-IN-FLIGHT seq=%u buf=%llu slot=%d "
                   "(the compositor never released it; %llu so far)",
                   sl->seq, (unsigned long long)sl->id, w->pending,
                   (unsigned long long)w->n_reuse_inflight);
        }
    }
    if (nb_trace_frames) {
        int i, held = 0;

        for (i = 0; i < NB_MAX_BUFS; i++) {
            if (w->bufs[i].valid && w->bufs[i].held) {
                held++;
            }
        }
        nb_log("frame: commit seq=%u buf=%llu slot=%d prev-slot=%d held=%d",
               sl->seq, (unsigned long long)sl->id, w->pending, w->current,
               held);
    }

    /* A real frame supersedes the placeholder; drop its mapping rather than
     * hold a megabyte of shm for the life of the VM. */
    if (w->idle_wanted) {
        w->idle_wanted = false;
        w->idle_shown = false;
    }
    w->buf_w = (int)sl->w;
    w->buf_h = (int)sl->h;
    /*
     * With a viewport the surface measures the WINDOW, not the buffer, and the
     * compositor scales the guest's frame into it.  That is what makes the
     * window resizable without the guest re-moding: dragging a window is a
     * host gesture and has nothing to say about the resolution the guest chose
     * to scan out.  Without wp_viewporter the surface is the buffer, as
     * before, and the window is whatever size the guest picked.
     */
    if (w->viewport) {
        if (w->win_w <= 0 || w->win_h <= 0) {
            w->win_w = (int)sl->w;
            w->win_h = (int)sl->h;
            tb_update(w, w->win_w);
        }
        wl_surface_attach(w->surf, sl->buf, 0, 0);
        /*
         * SCALING POLICY, applied by wl_viewport_apply() so that a new frame
         * and a resize can never disagree about it:
         *
         *   windowed   1:1, pixel-exact.  Sharp, and the window snaps to the
         *              guest's own resolution.
         *   fullscreen scaled to fit, aspect preserved.
         *
         * --scale forces scaling always, --no-scale forces 1:1 always.
         *
         * THE REAL FIX IS NOT HERE.  It is for the window size to reach the
         * guest so it re-modes and renders at the right size in the first
         * place -- which is now wired: GraphicHwOps.ui_info on nvkvm's
         * QemuConsole carries EV_SURFACE down to the guest head, and the head
         * offers the host's size as its preferred mode.  This remains the
         * fallback for a guest that declines to re-mode.
         */
        wl_viewport_apply(w, (int)sl->w, (int)sl->h, &new_w, &new_h);
    } else {
        wl_surface_attach(w->surf, sl->buf, 0, 0);
        new_w = (int)sl->w;
        new_h = (int)sl->h;
    }
    wl_surface_damage_buffer(w->surf, 0, 0, (int32_t)sl->w, (int32_t)sl->h);
    /*
     * An opaque region is not cosmetic here: a compositor will not promote a
     * surface to a hardware plane while it might have to blend it, so
     * declaring opacity is one of the preconditions for direct scanout.  The
     * guest head only ever flips X8/A8 32-bit formats and composites its own
     * cursor into them, so the whole surface is opaque by construction.
     *
     * Only on a size change: the region is sticky surface state, so
     * creating and destroying a protocol object every frame is traffic
     * for nothing.
     */
    if (new_w != w->surf_w || new_h != w->surf_h) {
        struct wl_region *opaque = wl_compositor_create_region(w->comp);

        if (opaque) {
            wl_region_add(opaque, 0, 0, new_w, new_h);
            wl_surface_set_opaque_region(w->surf, opaque);
            wl_region_destroy(opaque);
        }
    }
    if (w->presentation) {
        struct wp_presentation_feedback *fb =
            wp_presentation_feedback(w->presentation, w->surf);

        if (fb) {
            wp_presentation_feedback_add_listener(fb, &nb_pres_listener, w);
        }
    }
    if (!w->frame_inflight) {
        cb = wl_surface_frame(w->surf);
        if (cb) {
            wl_callback_add_listener(cb, &frame_listener, w);
            w->frame_inflight = true;
        }
    }
    /*
     * The letterbox offsets the resize borders hang off are computed by
     * bg_layout() inside the wl_viewport_apply() above, so re-place them here
     * rather than only on the resize path -- otherwise the FIRST frame, the one
     * that introduces the letterbox, leaves them where xdg_configure put them.
     * Guarded on its inputs, so a steady stream of same-size frames pays only
     * the compare.
     */
    bd_layout(w);
    wl_surface_commit(w->surf);

    sl->held = true;
    sl->commits++;
    if (w->current < 0) {
        /* First guest frame after the placeholder: the guest draws the cursor
         * from here on, so ours has to go even though the pointer never left. */
        w->current = w->pending;
        cur_apply(w, w->last_serial);
    }
    w->current = w->pending;
    w->pending = -1;
    /*
     * RECORD WHAT WE PRESENTED.  DO NOT REPORT IT.
     *
     * EV_SURFACE is an INSTRUCTION -- "guest, render this size" -- and it must
     * only ever carry what the WINDOW can show.  What arrives here is the size
     * the guest actually sent, which is an OBSERVATION.  Sending it back told
     * the guest to render the size it had just rendered, and on a fullscreen
     * window the two disagree, so they traded places forever:
     *
     *   configure: the window can show 3760x2118  -> "render 3760x2118"
     *   commit:    the guest's buffer is 1920x1080 -> "render 1920x1080"
     *   configure: the window can still show 3760x2118 -> ...
     *
     * OBSERVED on a 4K display at scale 1.5: the guest flipped between
     * 1920x1080 and 3760x2118 about every two seconds, forever.  (2160 - 42 is
     * the 28px title bar at 1.5x, which is why the number looks arbitrary.)
     *
     * This is the same rule the WINDOW handler states one layer up -- a guest
     * re-mode does not move the user's window -- applied to the buffer layer:
     * what the guest did is not an instruction to the guest.
     */
    w->surf_w = new_w;
    w->surf_h = new_h;
    return 0;
}

/* ── the client-side title bar ───────────────────────────────────────────── */
/*
 * GNOME'S MUTTER DOES NOT ADVERTISE zxdg_decoration_manager_v1 AT ALL
 * (verified on GNOME Shell 50.1, 2026-08-24): server-side decorations are not
 * something it can be asked for, so a toplevel that draws none arrives bare --
 * no title bar, no close button, and the --title nobody renders.  On a
 * compositor that DOES offer the protocol we ask for server-side and use it;
 * this is the fallback for the one every desktop Linux user is running.
 *
 * Placed at y = -NB_TB_H, so the bar sits ABOVE the content rather than on top
 * of it.  That is not cosmetic: a subsurface overlapping the main surface
 * occludes it, and an occluded surface cannot be promoted to a hardware plane.
 * The whole reason this backend hands the guest's buffer over untouched is to
 * stay a scanout candidate, and decorations must not spend that.
 */
         /* drawn characters of --title in the bar */

/* Buttons, counted from the right: 0 = close, 1 = maximize, 2 = minimize.
 * Returns -1 for "not on a button", i.e. somewhere to drag the window by. */
static int tb_hit(const struct nb_wl *w, int x)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (x >= w->tb_w - (i + 1) * NB_TB_BTN &&
            x <  w->tb_w - i * NB_TB_BTN) {
            return i;
        }
    }
    return -1;
}

static void tb_drop(struct nb_wl *w)
{
    if (w->tb_buf) {
        wl_buffer_destroy(w->tb_buf);
        w->tb_buf = NULL;
    }
    if (w->tb_px) {
        munmap(w->tb_px, w->tb_sz);
        w->tb_px = NULL;
        w->tb_sz = 0;
    }
    w->tb_w = 0;
}

static void tb_paint(struct nb_wl *w, int width)
{
    uint32_t *px = w->tb_px;
    const uint32_t bg  = w->grabbed ? 0xff43301aU : 0xff1b2027U;
    const uint32_t fg  = 0xffe6edf3U;
    const uint32_t hot = 0xffffb454U;
    const char *txt;
    char composed[192];
    bool hot_txt;
    unsigned tw, scale = 2, x;

    if (!px) {
        return;
    }
    nb_placeholder_fill(px, (unsigned)width, NB_TB_H, (unsigned)width, bg);
    /* one-pixel bottom rule, so the bar reads as chrome and not as content */
    for (x = 0; x < (unsigned)width; x++) {
        px[(size_t)(NB_TB_H - 1) * width + x] = 0xff2b3947U;
    }

    /*
     * UNDER GRAB THE TITLE IS THE ONLY WAY OUT.  CTRL+ALT+G is the sole
     * release gesture by design -- every other key is going to the guest -- so
     * the bar says so, in the accent colour, for as long as the grab is held.
     */
    {
        /*
         * STATUS AND TITLE, not status INSTEAD OF title.  Losing the window
         * name is a real cost when several are open -- but a status line is
         * only worth having if it is legible, so when both do not fit the
         * status keeps the space and the title is what goes.
         */
        const char *status = NULL;

        if (w->clip_notice_until && nb_now_ms_wl() < w->clip_notice_until) {
            status = "THE VM CHANGED YOUR CLIPBOARD";
        } else {
            w->clip_notice_until = 0;
            if (w->grabbed) {
                status = "GRABBED - CTRL+ALT+G TO RELEASE";
            }
        }
        hot_txt = status != NULL;
        if (status && w->title[0]) {
            const unsigned avail = (unsigned)(width - 3 * NB_TB_BTN - 16);

            snprintf(composed, sizeof composed, "%s - %s", status, w->title);
            txt = nb_placeholder_text_w(composed, scale) <= avail
                      ? composed : status;
        } else {
            txt = status ? status : w->title;
        }
    }
    tw = nb_placeholder_text_w(txt, scale);
    while (scale > 1 && tw > (unsigned)(width - 3 * NB_TB_BTN - 16)) {
        scale--;
        tw = nb_placeholder_text_w(txt, scale);
    }
    nb_placeholder_text(px, (unsigned)width, NB_TB_H, (unsigned)width,
                        10, (NB_TB_H - 7 * scale) / 2, txt, scale,
                        hot_txt ? hot : fg);

    /*
     * Three buttons at the right, in the order every desktop puts them:
     * minimize, maximize, close.  Drawn as pixels rather than glyphs -- they
     * are not letters and the 5x7 font has no box-drawing in it.
     */
    {
        int b;

        for (b = 0; b < 3; b++) {
            int cx = width - (b + 1) * NB_TB_BTN + (NB_TB_BTN - 11) / 2;
            int cy = (NB_TB_H - 11) / 2;
            int k;

            if (cx < 0) {
                continue;
            }
            /* Hover feedback.  Without it there is no way to tell a button
             * from a decoration until you have already clicked it. */
            if (b == w->tb_press) {
                int hx0 = width - (b + 1) * NB_TB_BTN;
                int hy, hx;

                for (hy = 0; hy < NB_TB_H - 1; hy++) {
                    for (hx = hx0; hx < hx0 + NB_TB_BTN && hx < width; hx++) {
                        if (hx >= 0) {
                            px[(size_t)hy * width + hx] =
                                b == 0 ? 0xffe05a4aU : 0xff7d8c9bU;
                        }
                    }
                }
            } else if (b == w->tb_hover) {
                int hx0 = width - (b + 1) * NB_TB_BTN;
                int hy, hx;

                for (hy = 0; hy < NB_TB_H - 1; hy++) {
                    for (hx = hx0; hx < hx0 + NB_TB_BTN && hx < width; hx++) {
                        if (hx >= 0) {
                            px[(size_t)hy * width + hx] =
                                b == 0 ? 0xffc0392bU : 0xff53606eU;
                        }
                    }
                }
            }
            if (b == 0) {                       /* close: an X */
                for (k = 0; k < 11; k++) {
                    px[(size_t)(cy + k) * width + cx + k] = fg;
                    px[(size_t)(cy + k) * width + cx + 10 - k] = fg;
                }
            } else if (b == 1) {                /* maximize: a box */
                for (k = 0; k < 11; k++) {
                    px[(size_t)cy * width + cx + k] = fg;
                    px[(size_t)(cy + 10) * width + cx + k] = fg;
                    px[(size_t)(cy + k) * width + cx] = fg;
                    px[(size_t)(cy + k) * width + cx + 10] = fg;
                }
            } else {                            /* minimize: a rule */
                for (k = 0; k < 11; k++) {
                    px[(size_t)(cy + 9) * width + cx + k] = fg;
                    px[(size_t)(cy + 10) * width + cx + k] = fg;
                }
            }
        }
    }
}

static void tb_update(struct nb_wl *w, int width)
{
    struct wl_shm_pool *pool;
    size_t stride, sz;
    void *px;
    int fd;

    if (!w->tb_surf || !w->shm) {
        return;
    }
    /*
     * NO CHROME IN FULLSCREEN.  Every other application hides its decorations
     * there and so must this one -- and it is not only cosmetic: fullscreen is
     * the state in which the content surface can reach a hardware plane, and a
     * mapped subsurface hanging off it is one more reason for a compositor to
     * decline.  Attaching a NULL buffer unmaps the bar outright.
     */
    if (w->fullscreen) {
        if (w->tb_mapped) {
            wl_surface_attach(w->tb_surf, NULL, 0, 0);
            wl_surface_commit(w->tb_surf);
            xdg_surface_set_window_geometry(w->xdg_surf,
                                            -w->off_x, -w->off_y,
                                            w->win_w > 0 ? w->win_w : width,
                                            w->win_h > 0 ? w->win_h : NB_TB_H);
            wl_surface_commit(w->surf);
            wl_display_flush(w->dpy);
            w->tb_mapped = false;
        }
        return;
    }
    if (width <= 3 * NB_TB_BTN + 20) {
        return;
    }
    if (width != w->tb_w) {
        stride = (size_t)width * 4;
        sz = stride * NB_TB_H;
        fd = memfd_create("nvkvm-broker-titlebar", MFD_CLOEXEC);
        if (fd < 0) {
            return;
        }
        if (ftruncate(fd, (off_t)sz) < 0) {
            close(fd);
            return;
        }
        px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (px == MAP_FAILED) {
            close(fd);
            return;
        }
        pool = wl_shm_create_pool(w->shm, fd, (int32_t)sz);
        close(fd);
        if (!pool) {
            munmap(px, sz);
            return;
        }
        tb_drop(w);
        w->tb_buf = wl_shm_pool_create_buffer(pool, 0, width, NB_TB_H,
                                              (int32_t)stride,
                                              WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        if (!w->tb_buf) {
            munmap(px, sz);
            return;
        }
        w->tb_px = px;
        w->tb_sz = sz;
        w->tb_w  = width;
    }
    tb_paint(w, w->tb_w);
    if (!w->tb_mapped) {
        nb_log("title bar: mapped, %dx%d at the top of the window",
               w->tb_w, NB_TB_H);
    }
    w->tb_mapped = true;
    /* Sit at the WINDOW's top-left, which is off_* up-left of the content
     * when aspect mode is drawing bars. */
    if (w->tb_sub) {
        wl_subsurface_set_position(w->tb_sub, -w->off_x,
                                   -w->off_y - NB_TB_H);
    }
    wl_surface_attach(w->tb_surf, w->tb_buf, 0, 0);
    wl_surface_damage_buffer(w->tb_surf, 0, 0, w->tb_w, NB_TB_H);
    wl_surface_commit(w->tb_surf);
    /* The window is the content PLUS the bar sitting above it, and PLUS any
     * letterbox bars around it. */
    xdg_surface_set_window_geometry(w->xdg_surf, -w->off_x,
                                    -w->off_y - NB_TB_H, w->tb_w,
                                    (w->win_h > 0 ? w->win_h : NB_TB_H)
                                    + NB_TB_H);
    wl_surface_commit(w->surf);
    wl_display_flush(w->dpy);
}

/* ── clipboard ───────────────────────────────────────────────────────────── */

#define NB_CLIP_MIME "text/plain;charset=utf-8"

static uint64_t nb_now_ms_wl(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void doffer_offer(void *d, struct wl_data_offer *o, const char *mime)
{
    struct nb_wl *w = d;

    if (w->offer == o && !strcmp(mime, NB_CLIP_MIME)) {
        w->offer_text = true;
    } else if (w->pending_offer == o && !strcmp(mime, NB_CLIP_MIME)) {
        w->pending_offer_text = true;
    }
}
static void doffer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) {}
static void doffer_action(void *d, struct wl_data_offer *o, uint32_t a) {}
static const struct wl_data_offer_listener doffer_listener = {
    .offer = doffer_offer,
    .source_actions = doffer_source_actions,
    .action = doffer_action,
};

static void ddev_data_offer(void *d, struct wl_data_device *dev,
                            struct wl_data_offer *o)
{
    struct nb_wl *w = d;

    /* This event does not say whether `o` is a selection or drag-and-drop
     * offer.  Association comes in selection()/enter(), so replacing the
     * current selection here confuses the two object lifetimes. */
    if (w->pending_offer && w->pending_offer != w->offer) {
        wl_data_offer_destroy(w->pending_offer);
    }
    w->pending_offer = o;
    w->pending_offer_text = false;
    wl_data_offer_add_listener(o, &doffer_listener, w);
}
static void ddev_selection(void *d, struct wl_data_device *dev,
                           struct wl_data_offer *o)
{
    struct nb_wl *w = d;

    if (!o) {
        if (w->offer) {
            wl_data_offer_destroy(w->offer);
        }
        w->offer = NULL;
        w->offer_text = false;
        return;
    }
    /* The offer we were told about in data_offer is now THE selection. */
    if (w->offer && w->offer != o) {
        wl_data_offer_destroy(w->offer);
    }
    w->offer = o;
    if (w->pending_offer == o) {
        w->offer_text = w->pending_offer_text;
        w->pending_offer = NULL;
        w->pending_offer_text = false;
    } else {
        /* The protocol promises data_offer first.  Fail this offer closed if a
         * compositor violates that ordering; it has no recorded MIME set. */
        w->offer_text = false;
    }
}
static void ddev_enter(void *d, struct wl_data_device *v, uint32_t s,
                       struct wl_surface *su, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *o)
{
    struct nb_wl *w = d;

    /* Drag-and-drop is deliberately unsupported.  Destroy its offer now that
     * the association is known, without disturbing the clipboard selection. */
    if (o && o == w->pending_offer) {
        wl_data_offer_destroy(o);
        w->pending_offer = NULL;
        w->pending_offer_text = false;
    }
}
static void ddev_leave(void *d, struct wl_data_device *v) {}
static void ddev_motion(void *d, struct wl_data_device *v, uint32_t t,
                        wl_fixed_t x, wl_fixed_t y) {}
static void ddev_drop(void *d, struct wl_data_device *v) {}
static const struct wl_data_device_listener ddev_listener = {
    .data_offer = ddev_data_offer,
    .enter = ddev_enter,
    .leave = ddev_leave,
    .motion = ddev_motion,
    .drop = ddev_drop,
    .selection = ddev_selection,
};

/* ── guest -> host: we become the selection owner ────────────────────────── */

static void dsrc_send(void *d, struct wl_data_source *src, const char *mime,
                      int fd)
{
    struct nb_wl *w = d;
    size_t off = 0;
    int flags;

    if (!w->src_text || strcmp(mime, NB_CLIP_MIME)) {
        close(fd);
        return;
    }
    /* The requester owns this fd.  It must not be allowed to stop the single
     * display/input loop by declining to read.  The selection is bounded and
     * normally fits in one pipe write; under pressure, fail this request
     * instead of blocking the user's keyboard. */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        nb_err("clipboard: cannot make a host requester's selection fd "
               "nonblocking: %s; refusing that request", strerror(errno));
        close(fd);
        return;
    }
    while (off < w->src_len) {
        ssize_t n = write(fd, w->src_text + off, w->src_len - off);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                nb_log("clipboard: a host requester stopped draining; "
                       "abandoning its nonblocking selection transfer");
            }
            break;
        }
        off += (size_t)n;
    }
    close(fd);
}
static void dsrc_cancelled(void *d, struct wl_data_source *src)
{
    struct nb_wl *w = d;

    /* Someone else took the selection.  Ours is dead; drop it so we do not
     * keep the guest's text alive longer than it is on the clipboard. */
    if (w->source == src) {
        wl_data_source_destroy(src);
        w->source = NULL;
        free(w->src_text);
        w->src_text = NULL;
        w->src_len = 0;
    }
}
static void dsrc_target(void *d, struct wl_data_source *s, const char *m) {}
static void dsrc_dnd_drop(void *d, struct wl_data_source *s) {}
static void dsrc_dnd_finished(void *d, struct wl_data_source *s) {}
static void dsrc_action(void *d, struct wl_data_source *s, uint32_t a) {}
static const struct wl_data_source_listener dsrc_listener = {
    .target = dsrc_target,
    .send = dsrc_send,
    .cancelled = dsrc_cancelled,
    .dnd_drop_performed = dsrc_dnd_drop,
    .dnd_finished = dsrc_dnd_finished,
    .action = dsrc_action,
};

static int wl_set_clipboard(struct nb_session *s, const char *text, size_t len);
static void wl_notify_clipboard(struct nb_session *s);

/* Put a held guest copy on the clipboard now that the window has focus. */
static void wl_clip_flush_pending(struct nb_wl *w)
{
    char *text;
    size_t len;

    if (!w->clip_pending || !w->focused) {
        return;
    }
    /* Detach first: wl_set_clipboard() is about to be re-entered and must not
     * see the pending copy again, or focus-in would loop. */
    text = w->clip_pending; len = w->clip_pending_len;
    w->clip_pending = NULL; w->clip_pending_len = 0;
    if (w->sess && wl_set_clipboard(w->sess, text, len) == 0) {
        nb_log("the VM's clipboard text is now on yours (it was held while "
               "the window was not focused)");
        wl_notify_clipboard(w->sess);
    }
    free(text);
}

static int wl_set_clipboard(struct nb_session *s, const char *text, size_t len)
{
    struct nb_wl *w = s->priv;
    char *copy;

    if (!w->ddm || !w->ddev || !w->seat) {
        return -ENOTSUP;
    }
    copy = malloc(len + 1);
    if (!copy) {
        return -ENOMEM;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';

    /*
     * NOT FOCUSED: hold it, do not apply it.  Wayland would refuse the
     * selection anyway (no input serial to justify it), so the alternative is
     * not "apply it regardless" -- it is "fail silently and lose the copy".
     * Only the most recent one is kept; a clipboard has room for one thing.
     */
    if (!w->focused) {
        free(w->clip_pending);
        w->clip_pending = copy;
        w->clip_pending_len = len;
        return 1;               /* HELD -- see the ops contract */
    }

    if (w->source) {
        wl_data_source_destroy(w->source);
        w->source = NULL;
    }
    free(w->src_text);
    w->src_text = copy;
    w->src_len = len;

    w->source = wl_data_device_manager_create_data_source(w->ddm);
    if (!w->source) {
        return -EIO;
    }
    wl_data_source_add_listener(w->source, &dsrc_listener, w);
    wl_data_source_offer(w->source, NB_CLIP_MIME);
    /* The serial must be one from a real input event; the compositor rejects
     * a fabricated one, which is the protocol enforcing "only a focused client
     * that has seen input may take the selection". */
    wl_data_device_set_selection(w->ddev, w->source, w->last_serial);
    wl_display_flush(w->dpy);
    return 0;
}

/* ── host -> guest: read the selection, asynchronously ───────────────────── */

static int wl_fetch_clipboard(struct nb_session *s, struct nb_sink *sink,
                              uint64_t generation)
{
    struct nb_wl *w = s->priv;
    int fds[2];

    if (w->fetch_fd >= 0) {
        return -EBUSY;              /* one at a time */
    }
    if (!w->offer || !w->offer_text) {
        return -ENOENT;             /* nothing, or nothing we accept */
    }
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        return -errno;
    }
    wl_data_offer_receive(w->offer, NB_CLIP_MIME, fds[1]);
    close(fds[1]);                  /* the compositor owns its end now */
    wl_display_flush(w->dpy);
    w->fetch_fd = fds[0];
    w->fetch_len = 0;
    w->fetch_generation = generation;
    return 0;
}

/*
 * Drain the selection pipe.  Returns true when the transfer is finished (for
 * any reason), so the caller can release the held paste keystroke exactly once.
 */
static bool wl_fetch_pump(struct nb_wl *w, struct nb_sink *sink)
{
    for (;;) {
        ssize_t n = read(w->fetch_fd, w->fetch_buf + w->fetch_len,
                         sizeof(w->fetch_buf) - w->fetch_len);

        if (n > 0) {
            w->fetch_len += (size_t)n;
            if (w->fetch_len > NVKVM_BROKER_CLIP_MAX_BYTES) {
                nb_log("clipboard: host selection is larger than the %u-byte "
                       "cap; not pasting it",
                       NVKVM_BROKER_CLIP_MAX_BYTES);
                nb_sink_clip_finish(sink, w->fetch_generation, false);
                break;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;           /* more later */
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            nb_log("clipboard: reading the host selection failed: %s",
                   strerror(errno));
        }
        /* n == 0: EOF, the whole selection is here. */
        if (n == 0 && w->fetch_len > 0) {
            bool sent = nb_sink_send_clipboard(sink, w->fetch_generation,
                                               w->fetch_buf, w->fetch_len);

            nb_sink_clip_finish(sink, w->fetch_generation, sent);
        } else {
            nb_sink_clip_finish(sink, w->fetch_generation, false);
        }
        break;
    }
    close(w->fetch_fd);
    w->fetch_fd = -1;
    w->fetch_len = 0;
    w->fetch_generation = 0;
    return true;
}

static void wl_client_detach(struct nb_session *s, uint64_t generation)
{
    struct nb_wl *w = s->priv;

    if (w->fetch_fd >= 0 && w->fetch_generation == generation) {
        close(w->fetch_fd);
        w->fetch_fd = -1;
        w->fetch_len = 0;
        w->fetch_generation = 0;
    }
    /* The warning/title state described the departed VM, not the persistent
     * host window. */
    w->clip_notice_until = 0;
    tb_update(w, w->tb_w > 0 ? w->tb_w : w->win_w);
}

/* ── the output's fractional scale ───────────────────────────────────────── */
/*
 * EV_SURFACE carries what the GUEST should render, which is physical pixels --
 * see the scale_120 comment.  Everything else in this file works in logical
 * pixels, so the conversion lives here and nowhere else.
 */
static void wl_report_surface(struct nb_wl *w, int lw, int lh)
{
    unsigned s = w->scale_120 ? w->scale_120 : 120;

    if (!w->sink || lw <= 0 || lh <= 0) {
        return;
    }
    /*
     * Sync the fullscreen flag HERE, on the packet the client actually keys
     * its mode switch off, rather than only on the transition.  Fullscreen can
     * be true before any transition is observed -- `--fullscreen` sets it at
     * startup, so top_configure's was_fs check never fires and the flag stayed
     * false forever, which silently disabled the whole ui_info path.  The
     * setter is idempotent, so calling it on every report costs a compare.
     */
    nb_sink_set_fullscreen(w->sink, w->fullscreen);

    /*
     * THE ONE PLACE A HINT IS EMITTED, and it is reached only from host events
     * -- a geometry change or a scale change.  Nothing a guest does gets here.
     */
    unsigned pw, ph;

    switch (nb_res_mode) {
    case NB_RES_NONE:
        return;                 /* suggest nothing at all */
    case NB_RES_FIXED:
        pw = nb_res_w;
        ph = nb_res_h;
        break;
    default:
        pw = (unsigned)((long)lw * s / 120);
        ph = (unsigned)((long)lh * s / 120);
        break;
    }

    /*
     * ROUND THE WIDTH DOWN TO A MULTIPLE OF 8.
     *
     * The guest synthesises the mode with drm_cvt_mode(), which applies
     * CVT_H_GRANULARITY = 8, so a width that is not a multiple of 8 cannot be
     * honoured exactly.  Exact is what matters: a compositor promotes a surface
     * to a hardware plane only when it covers the output, so missing by four
     * pixels silently costs direct scanout.  Down, never up, so the suggestion
     * always fits the window we measured.
     */
    pw &= ~7u;
    if (pw == 0 || ph == 0) {
        return;
    }

    /*
     * Idempotent.  A configure that did not change the content size, or a
     * fixed mode re-reporting the same numbers, is not news -- and a hint the
     * guest has already acted on is a re-mode it does not need.
     */
    if (pw == w->hint_w && ph == w->hint_h) {
        return;
    }
    w->hint_w = pw;
    w->hint_h = ph;
    /*
     * What we LEARNED is w->refresh_mhz; what we SAY is this.  Kept apart so
     * the log still shows the real host rate even when an operator has pinned
     * a different one.
     */
    {
        unsigned say = w->refresh_mhz;

        if (nb_hz_mode == NB_HZ_NONE) {
            say = 0;
        } else if (nb_hz_mode == NB_HZ_FIXED) {
            say = nb_hz_fixed;
        }
        nb_sink_surface(w->sink, pw, ph, say);
    }
}


static void frac_scale(void *d, struct wp_fractional_scale_v1 *f,
                       uint32_t scale)
{
    struct nb_wl *w = d;

    (void)f;
    if (scale == 0 || scale == w->scale_120) {
        return;
    }
    w->scale_120 = scale;
    nb_log("output scale is %u.%03u — the guest is told PHYSICAL pixels, so a "
           "fullscreen guest buffer can cover the output",
           scale / 120, (scale % 120) * 1000 / 120);
    /* Re-report at the new scale: the size in logical pixels has not changed,
     * but what the guest should render has.  cont_*, not surf_* -- see the
     * field's comment for why reporting surf_* oscillates. */
    wl_report_surface(w, w->cont_w, w->cont_h);
}
static const struct wp_fractional_scale_v1_listener frac_listener = {
    .preferred_scale = frac_scale,
};

/*
 * The host cursor over the CONTENT surface, hidden or shown by what is
 * actually on it.  Called on pointer entry and again whenever the content
 * changes underneath a pointer that is already inside -- the placeholder
 * appearing or the guest's first frame arriving sends no enter event, so
 * without this the cursor keeps whatever state it had.
 */
static void cur_apply(struct nb_wl *w, uint32_t serial)
{
    if (!w->ptr || !w->ptr_on_content) {
        return;
    }
    cur_build(w);
    if (w->dlg_open) {
        /*
         * THE DIALOG IS HOST UI AND ALWAYS HAS A POINTER, exactly like the
         * title bar.  It can be up WHILE a guest buffer is on the surface, so
         * it has to be tested before the guest-content case below -- otherwise
         * the user is asked a question with four buttons and given no cursor
         * to answer it with.
         */
        wl_pointer_set_cursor(w->ptr, serial, w->cur_surf[NB_CUR_ARROW], 0, 0);
    } else if (w->current >= 0) {
        /* Guest content: it composites its own cursor, so ours must go. */
        wl_pointer_set_cursor(w->ptr, serial, NULL, 0, 0);
    } else {
        /* The placeholder.  Nothing is drawing a cursor into it. */
        wl_pointer_set_cursor(w->ptr, serial, w->cur_surf[NB_CUR_ARROW], 0, 0);
    }
}

/* ── the letterbox background ────────────────────────────────────────────── */
static void bg_build(struct nb_wl *w)
{
    struct wl_shm_pool *pool;
    uint32_t *px;
    int fd;

    if (w->bg_surf || !w->shm || !w->subcomp || !w->comp || !w->viewporter) {
        return;
    }
    fd = memfd_create("nvkvm-broker-bg", MFD_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (ftruncate(fd, 4) < 0) {
        close(fd);
        return;
    }
    px = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    *px = 0xff000000u;          /* opaque black: the conventional bar colour */
    w->bg_px = px;
    pool = wl_shm_create_pool(w->shm, fd, 4);
    if (pool) {
        w->bg_buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
                                              WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    if (!w->bg_buf) {
        return;
    }
    w->bg_surf = wl_compositor_create_surface(w->comp);
    if (!w->bg_surf) {
        return;
    }
    w->bg_sub = wl_subcompositor_get_subsurface(w->subcomp, w->bg_surf,
                                                w->surf);
    if (!w->bg_sub) {
        return;
    }
    /* Below the parent, so the guest's frame stays the topmost thing. */
    wl_subsurface_place_below(w->bg_sub, w->surf);
    wl_subsurface_set_desync(w->bg_sub);
    w->bg_vp = wp_viewporter_get_viewport(w->viewporter, w->bg_surf);
}

/*
 * Show or hide the bars.  `dw`/`dh` is the content rectangle just computed;
 * anything the window has left over is bar.  Called from the same place the
 * viewport destination is set, so the two can never disagree.
 */
static void bg_layout(struct nb_wl *w, int dw, int dh)
{
    bool want = w->win_w > dw || w->win_h > dh;

    w->off_x = want && w->win_w > dw ? (w->win_w - dw) / 2 : 0;
    w->off_y = want && w->win_h > dh ? (w->win_h - dh) / 2 : 0;

    if (want) {
        bg_build(w);
    }
    if (!w->bg_surf || !w->bg_vp) {
        w->off_x = w->off_y = 0;
        return;
    }
    if (!want) {
        if (w->bg_mapped) {
            wl_surface_attach(w->bg_surf, NULL, 0, 0);
            wl_surface_commit(w->bg_surf);
            w->bg_mapped = false;
        }
        return;
    }
    /* The background sits at the window origin; the content (the parent
     * surface) is inset by off_*, so the background starts that far up-left
     * of it. */
    wl_subsurface_set_position(w->bg_sub, -w->off_x, -w->off_y);
    wp_viewport_set_destination(w->bg_vp, w->win_w, w->win_h);
    wl_surface_attach(w->bg_surf, w->bg_buf, 0, 0);
    wl_surface_damage_buffer(w->bg_surf, 0, 0, 1, 1);
    wl_surface_commit(w->bg_surf);
    w->bg_mapped = true;
}

/* ── the close confirmation ──────────────────────────────────────────────── */
/*
 * WHY THIS IS IN THE PRIVILEGED PROCESS AT ALL, stated plainly because the
 * original design said it should not be.
 *
 * The objection to a dialog in the broker was that the broker holds the
 * keyboard grab and knows nothing about VMs, so it must not DECIDE what
 * closing a window does to one.  It still does not.  Each choice below either
 * selects which fixed-size message to send -- QEMU decides what a powerdown or
 * a force-off means -- or does something purely local to the broker's own
 * window.  What is new is drawing and hit-testing, and the title bar's three
 * buttons already do both, so this is not a new class of code in here.
 *
 * The alternative, a helper process spawned to draw it, buys isolation for the
 * drawing but costs a process, an IPC and a way for the dialog to disagree
 * with the window it belongs to.  Not worth it for four rectangles.
 */
static int dlg_hit(const struct nb_wl *w, int x, int y)
{
    int i;

    if (x < NB_DLG_PAD || x >= NB_DLG_W - NB_DLG_PAD) {
        return -1;
    }
    for (i = 0; i < NB_DLG_N; i++) {
        int top = NB_DLG_PAD + NB_DLG_TITLE + NB_DLG_GAP +
                  i * (NB_DLG_BTN_H + NB_DLG_GAP);

        if (y >= top && y < top + NB_DLG_BTN_H) {
            return i;
        }
    }
    return -1;
}

static void dlg_paint(struct nb_wl *w)
{
    uint32_t *px = w->dlg_px;
    unsigned stride_px = NB_DLG_W;
    int i;

    if (!px) {
        return;
    }
    nb_placeholder_fill(px, NB_DLG_W, NB_DLG_H, stride_px, 0xff1c1c22u);
    /* A border, so it reads as a thing on top rather than a hole in the guest. */
    for (i = 0; i < NB_DLG_W; i++) {
        px[i] = 0xff5a5a6eu;
        px[(NB_DLG_H - 1) * stride_px + i] = 0xff5a5a6eu;
    }
    for (i = 0; i < NB_DLG_H; i++) {
        px[i * stride_px] = 0xff5a5a6eu;
        px[i * stride_px + NB_DLG_W - 1] = 0xff5a5a6eu;
    }
    nb_placeholder_text(px, NB_DLG_W, NB_DLG_H, stride_px, NB_DLG_PAD,
                        NB_DLG_PAD,
                        w->close_asked ? "STILL RUNNING - ASK AGAIN?"
                                       : "CLOSE THE DISPLAY?",
                        2, w->close_asked ? 0xffffd080u : 0xffffffffu);

    for (i = 0; i < NB_DLG_N; i++) {
        int top = NB_DLG_PAD + NB_DLG_TITLE + NB_DLG_GAP +
                  i * (NB_DLG_BTN_H + NB_DLG_GAP);
        int bw = NB_DLG_W - 2 * NB_DLG_PAD;
        uint32_t bg;
        unsigned tw;
        int x, y;

        bg = (i == w->dlg_press) ? 0xff4a4a64u
           : (i == w->dlg_hover) ? 0xff3c3c50u
                                 : 0xff2e2e3au;
        for (y = top; y < top + NB_DLG_BTN_H; y++) {
            for (x = NB_DLG_PAD; x < NB_DLG_PAD + bw; x++) {
                px[y * (int)stride_px + x] = bg;
            }
        }
        tw = nb_placeholder_text_w(nb_dlg_label[i], 2);
        nb_placeholder_text(px, NB_DLG_W, NB_DLG_H, stride_px,
                            (unsigned)(NB_DLG_PAD + (bw - (int)tw) / 2),
                            (unsigned)(top + (NB_DLG_BTN_H - 14) / 2),
                            nb_dlg_label[i], 2,
                            i == 1 ? 0xffff9090u : 0xffe8e8f0u);
    }
}

static void dlg_hide(struct nb_wl *w)
{
    if (!w->dlg_open) {
        return;
    }
    w->dlg_open = false;
    w->dlg_hover = w->dlg_press = -1;
    w->ptr_on_dlg = false;
    cur_apply(w, w->last_serial);   /* back to whatever the content wants */
    if (w->dlg_surf) {
        wl_surface_attach(w->dlg_surf, NULL, 0, 0);
        wl_surface_commit(w->dlg_surf);
        wl_surface_commit(w->surf);
        wl_display_flush(w->dpy);
    }
    nb_log("close: dismissed");
}

static void dlg_commit(struct nb_wl *w)
{
    if (!w->dlg_surf || !w->dlg_buf) {
        return;
    }
    dlg_paint(w);
    /* Centre it on the window as the compositor last configured it. */
    if (w->dlg_sub) {
        int cw = w->win_w > 0 ? w->win_w : NB_DLG_W;
        int ch = w->win_h > 0 ? w->win_h : NB_DLG_H;

        wl_subsurface_set_position(w->dlg_sub, (cw - NB_DLG_W) / 2,
                                   (ch - NB_DLG_H) / 2);
    }
    wl_surface_attach(w->dlg_surf, w->dlg_buf, 0, 0);
    wl_surface_damage_buffer(w->dlg_surf, 0, 0, NB_DLG_W, NB_DLG_H);
    wl_surface_commit(w->dlg_surf);
    wl_surface_commit(w->surf);
    wl_display_flush(w->dpy);
}

static void dlg_show(struct nb_wl *w)
{
    struct wl_shm_pool *pool;
    size_t stride, sz;
    int fd;

    if (w->dlg_open) {
        return;
    }
    /*
     * NOTHING TO ASK ABOUT WITH NO VM.  Over the placeholder there is no guest
     * to shut down and no client to defer the policy to, so the four-way
     * question is meaningless -- X just closes the display, which is what it
     * has always meant when nobody is connected.
     */
    if (!nb_sink_has_client(w->sink)) {
        nb_log("close: no VM is connected, so there is nothing to ask about");
        w->quit = true;
        return;
    }
    if (!w->shm || !w->subcomp || !w->comp) {
        w->quit = true;         /* no way to ask; the old behaviour */
        return;
    }
    /*
     * DROP THE GRAB BEFORE SHOWING HOST UI.  A dialog the user cannot click
     * because the pointer is locked to the guest, in front of a guest that is
     * swallowing the keyboard, is exactly the state a grab is hardest to get
     * out of.  Done before the dialog maps, not after.
     */
    nb_sink_force_ungrab(w->sink);
    if (!w->dlg_surf) {
        w->dlg_surf = wl_compositor_create_surface(w->comp);
        if (!w->dlg_surf) {
            w->quit = true;
            return;
        }
        w->dlg_sub = wl_subcompositor_get_subsurface(w->subcomp, w->dlg_surf,
                                                     w->surf);
        if (!w->dlg_sub) {
            w->quit = true;
            return;
        }
        wl_subsurface_place_above(w->dlg_sub, w->surf);
        wl_subsurface_set_desync(w->dlg_sub);
    }
    if (!w->dlg_buf) {
        stride = (size_t)NB_DLG_W * 4;
        sz = stride * NB_DLG_H;
        fd = memfd_create("nvkvm-broker-close", MFD_CLOEXEC);
        if (fd < 0) {
            w->quit = true;
            return;
        }
        if (ftruncate(fd, (off_t)sz) < 0) {
            close(fd);
            w->quit = true;
            return;
        }
        w->dlg_px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (w->dlg_px == MAP_FAILED) {
            w->dlg_px = NULL;
            close(fd);
            w->quit = true;
            return;
        }
        w->dlg_sz = sz;
        pool = wl_shm_create_pool(w->shm, fd, (int32_t)sz);
        if (pool) {
            w->dlg_buf = wl_shm_pool_create_buffer(pool, 0, NB_DLG_W,
                                                   NB_DLG_H, (int32_t)stride,
                                                   WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
        }
        close(fd);
        if (!w->dlg_buf) {
            w->quit = true;
            return;
        }
    }
    w->dlg_open = true;
    w->dlg_hover = w->dlg_press = -1;
    cur_apply(w, w->last_serial);   /* give the pointer back over the content */
    nb_log("close: asking what to do (ACPI / force off / display only / cancel)");
    dlg_commit(w);
}

/* One of the four choices was taken. */
static void dlg_choose(struct nb_wl *w, int choice)
{
    switch (choice) {
    case 0:
        nb_log("close: ACPI powerdown requested%s",
               w->close_asked ? " (again; the guest has not acted on the last "
                                "one -- FORCE OFF is the way out)" : "");
        if (!nb_sink_close_request(w->sink, NVKVM_BROKER_CLOSE_POWERDOWN)) {
            w->quit = true;
        } else {
            w->close_asked = true;
        }
        break;
    case 1:
        nb_log("close: force off requested");
        if (!nb_sink_close_request(w->sink, NVKVM_BROKER_CLOSE_FORCE)) {
            w->quit = true;
        }
        break;
    case 2:
        /* Purely local: the VM is not told and keeps running.  The socket
         * closes, and a broker started again on the same path reattaches. */
        nb_log("close: closing the display only; the VM keeps running");
        w->quit = true;
        break;
    default:
        break;
    }
    dlg_hide(w);
}

/* ── the idle placeholder ────────────────────────────────────────────────── */
/*
 * The surface has no content until the client's first ATTACH+COMMIT, so an
 * idle broker is an unmapped window: it sits in the dock and shows nothing
 * when clicked.  "Waiting for a VM" and "crashed" then look identical, which
 * is the first thing a user meets.  We paint our own frame instead.
 *
 * shm, not dma-buf, on purpose: this is the one thing the broker draws itself,
 * it must work before any GPU buffer exists, and wl_shm is the only allocator
 * every compositor is required to have.  It costs one page-aligned mapping
 * that is freed the moment a guest frame replaces it.
 */
static void wl_idle_drop(struct nb_wl *w)
{
    if (w->idle_buf) {
        wl_buffer_destroy(w->idle_buf);
        w->idle_buf = NULL;
    }
    if (w->idle_px) {
        munmap(w->idle_px, w->idle_sz);
        w->idle_px = NULL;
        w->idle_sz = 0;
    }
    w->idle_w = w->idle_h = 0;
}

static int wl_idle_make(struct nb_wl *w, int wd, int ht)
{
    struct wl_shm_pool *pool;
    size_t stride = (size_t)wd * 4;
    size_t sz = stride * (size_t)ht;
    void *px;
    int fd;

    if (!w->shm || wd <= 0 || ht <= 0) {
        return -ENOTSUP;
    }
    fd = memfd_create("nvkvm-broker-idle", MFD_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    if (ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return -errno;
    }
    px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return -errno;
    }
    pool = wl_shm_create_pool(w->shm, fd, (int32_t)sz);
    close(fd);
    if (!pool) {
        munmap(px, sz);
        return -EIO;
    }
    /*
     * Destroy the previous buffer only AFTER the new one exists, and rely on
     * request ordering for the swap: the compositor processes attach+commit
     * before it sees the destroy, so the old buffer is never yanked out from
     * under a frame it is still scanning out.
     */
    wl_idle_drop(w);
    w->idle_buf = wl_shm_pool_create_buffer(pool, 0, wd, ht, (int32_t)stride,
                                            WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    if (!w->idle_buf) {
        munmap(px, sz);
        return -EIO;
    }
    w->idle_px = px;
    w->idle_sz = sz;
    w->idle_w  = wd;
    w->idle_h  = ht;
    nb_placeholder_paint(px, (unsigned)wd, (unsigned)ht, (unsigned)wd,
                         "NVKVM DISPLAY BROKER", "WAITING FOR A VM");
    return 0;
}

/* ops->dismiss_dialog: the broker took a state that contradicts the dialog. */
/*
 * ops->notify_clipboard: the guest just replaced the host clipboard.
 *
 * VISIBILITY IS THE CONTROL for this direction.  A guest that can silently
 * swap what you are about to paste into a host terminal is the actual attack;
 * a log line is good for forensics but nobody is reading stderr, so it also
 * goes where the user is already looking.
 */
static void wl_notify_clipboard(struct nb_session *s)
{
    struct nb_wl *w = s->priv;

    w->clip_notice_until = nb_now_ms_wl() + 4000u;
    tb_update(w, w->tb_w > 0 ? w->tb_w : w->win_w);
    wl_display_flush(w->dpy);
}

/*
 * Clock-driven work.  Only the title-bar notice qualifies today: it expires on
 * a deadline, not on an event, and the main loop otherwise sleeps in poll()
 * forever -- which is why a "transient" notice used to sit there until an
 * unrelated event happened to redraw the bar.
 */
static int wl_tick(struct nb_session *s)
{
    struct nb_wl *w = s->priv;
    uint64_t now;

    if (!w->clip_notice_until) {
        return -1;
    }
    now = nb_now_ms_wl();
    if (now >= w->clip_notice_until) {
        w->clip_notice_until = 0;
        tb_update(w, w->tb_w > 0 ? w->tb_w : w->win_w);
        wl_display_flush(w->dpy);
        return -1;
    }
    return (int)(w->clip_notice_until - now);
}

static void wl_dismiss_dialog(struct nb_session *s)
{
    struct nb_wl *w = s->priv;

    /* Also clears the "you already asked" memory: it is per-connection, and
     * the two callers are a client going away and the user choosing a
     * contradictory mode -- after either, the next close starts fresh. */
    w->close_asked = false;
    dlg_hide(w);
}

static int wl_show_idle(struct nb_session *s)
{
    struct nb_wl *w = s->priv;
    int wd = w->win_w > 0 ? w->win_w : (w->surf_w > 0 ? w->surf_w : 1280);
    int ht = w->win_h > 0 ? w->win_h : (w->surf_h > 0 ? w->surf_h : 800);
    int r;

    w->idle_wanted = true;
    if (!w->shm) {
        /* Said once: a compositor with no wl_shm is legal but startling. */
        if (!w->idle_shown) {
            w->idle_shown = true;
            nb_log("no wl_shm: the window stays blank until the VM's first "
                   "frame");
        }
        return -ENOTSUP;
    }
    if (!w->configured) {
        return 0;               /* xdg_configure will call us back */
    }
    if (!w->idle_buf || w->idle_w != wd || w->idle_h != ht) {
        r = wl_idle_make(w, wd, ht);
        if (r < 0) {
            nb_err("could not make the placeholder buffer: %s", strerror(-r));
            return r;
        }
    }
    wl_surface_attach(w->surf, w->idle_buf, 0, 0);
    if (w->viewport) {
        wp_viewport_set_destination(w->viewport, wd, ht);
    }
    wl_surface_damage_buffer(w->surf, 0, 0, wd, ht);
    wl_surface_commit(w->surf);
    wl_display_flush(w->dpy);
    w->current = -1;
    cur_apply(w, w->last_serial);   /* placeholder now: give the cursor back */
    if (!w->idle_shown) {
        nb_log("no client yet: showing the placeholder (%dx%d)", wd, ht);
        w->idle_shown = true;
    }
    return 0;
}

static int wl_resize(struct nb_session *s, unsigned wd, unsigned ht)
{
    struct nb_wl *w = s->priv;

    /*
     * On Wayland a surface's size IS its buffer's size, so there is nothing to
     * request: the next ATTACH at the new geometry resizes the window, and
     * EV_SURFACE then reports what actually happened.  Recorded so the window
     * has a sensible size before the first frame arrives.
     */
    w->surf_w = (int)wd;
    w->surf_h = (int)ht;
    return 0;
}

/* ── seat ────────────────────────────────────────────────────────────────── */

static void kbd_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd,
                       uint32_t sz)
{
    (void)d; (void)k; (void)f; (void)sz;
    /* We forward evdev keycodes, not symbols, so the keymap is the guest's
     * business.  Closing it is required by the protocol. */
    close(fd);
}
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys)
{
    struct nb_wl *w = d;
    (void)k; (void)s; (void)keys;
    /*
     * KEYBOARD SERIALS COUNT TOO.  wl_data_device.set_selection needs a serial
     * from a real input event, and only ptr_enter/ptr_button used to record
     * one -- so a user working from the keyboard claimed the selection with
     * whatever stale pointer serial was lying around, and the compositor
     * silently ignored it.  That is why a guest copy "sometimes" did not
     * reach the host clipboard, and why it failed most reliably right after a
     * paste: Ctrl+V and Ctrl+C are both keyboard events, so neither refreshed
     * the serial, and the one still on file had already been spent.
     */
    w->last_serial = serial;
    w->focused = true;
    if (w->sink) {
        nb_sink_focus(w->sink, true);
    }
    wl_clip_flush_pending(w);
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s)
{
    struct nb_wl *w = d;
    (void)k; (void)serial; (void)s;
    w->focused = false;
    /* The property that stops the grab being a keylogger. */
    if (w->sink) {
        nb_sink_focus(w->sink, false);
    }
}
static void kbd_key(void *d, struct wl_keyboard *k, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    struct nb_wl *w = d;
    (void)k; (void)time;

    w->last_serial = serial;    /* see kbd_enter: set_selection needs a fresh one */

    /*
     * MODAL.  While the confirmation is up, no key reaches the guest --
     * including the hotkeys, which nb_sink_key() would otherwise interpret.
     * A dialog the guest can type through is not a dialog.
     */
    if (w->dlg_open) {
        if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
            return;
        }
        switch (key) {
        case KEY_ESC:
            dlg_hide(w);
            break;
        case KEY_ENTER:
        case KEY_KPENTER:
            dlg_choose(w, 0);   /* the graceful one is the default */
            break;
        default:
            break;
        }
        return;
    }
    /* wl_keyboard.key carries the evdev code directly — no translation. */
    if (w->sink) {
        nb_sink_key(w->sink, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    }
}
static void kbd_mods(void *d, struct wl_keyboard *k, uint32_t serial,
                     uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)grp;
    /* Ignored on purpose: the sink derives modifier state from the same key
     * stream the client sees, so the two cannot disagree. */
}
static void kbd_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dly)
{
    (void)d; (void)k; (void)r; (void)dly;
}
static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave,
    .key = kbd_key, .modifiers = kbd_mods, .repeat_info = kbd_repeat,
};

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    struct nb_wl *w = d;
    (void)x; (void)y;

    w->last_serial = serial;
    /*
     * The title bar is a surface of ours too, and the pointer entering IT is
     * not the pointer entering the guest.  Leave the host cursor alone there
     * (the user is reaching for the close button and needs to see it) and
     * forward nothing to the guest.
     */
    cur_build(w);
    w->bd_hot = bd_index(w, s);
    if (w->bd_hot >= 0) {
        w->ptr_on_content = false;
        {
            const int c = nb_bd_cursor[w->bd_hot];

            wl_pointer_set_cursor(p, serial, w->cur_surf[c],
                                  w->cur_hx[c], w->cur_hy[c]);
        }
        return;
    }
    if (s && s == w->dlg_surf) {
        w->ptr_on_dlg = true;
        w->ptr_on_tb = false;
        w->ptr_on_content = false;
        wl_pointer_set_cursor(p, serial, w->cur_surf[NB_CUR_ARROW], 0, 0);
        return;
    }
    w->ptr_on_dlg = false;
    if (s && s == w->tb_surf) {
        w->ptr_on_tb = true;
        w->ptr_on_content = false;
        wl_pointer_set_cursor(p, serial, w->cur_surf[NB_CUR_ARROW], 0, 0);
        return;
    }
    w->ptr_on_tb = false;
    /*
     * HIDE THE HOST CURSOR over the guest's picture -- but ONLY over the
     * guest's picture.  The guest composites its own cursor into the scanout it
     * hands us (see wl_commit), so leaving the host one visible draws two, a
     * few pixels apart, and the one that responds is not the one the user is
     * looking at.
     *
     * THE PLACEHOLDER IS NOT THE GUEST.  Nothing is compositing a cursor into
     * it, so hiding the host one there just leaves the window with no pointer
     * at all -- which is what happens before a VM connects, exactly when a
     * person is most likely to be moving the mouse around wondering whether
     * the thing is alive.
     *
     * A NULL surface is the protocol's "no cursor", and its scope is right by
     * construction: set_cursor applies while the pointer is over OUR surface,
     * and the title bar is a different surface of ours that sets its own, so
     * the arrow returns on its own at the window chrome.
     */
    w->ptr_on_content = true;
    cur_apply(w, serial);
    if (w->sink) {
        nb_sink_pointer(w->sink, true);
    }
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s)
{
    struct nb_wl *w = d;

    w->ptr_on_content = false;

    if (bd_index(w, s) >= 0) {
        w->bd_hot = -1;
        return;
    }
    if (s && s == w->tb_surf) {
        w->ptr_on_tb = false;
        if (w->tb_hover != -1) {
            w->tb_hover = -1;
            tb_update(w, w->tb_w);
        }
        return;
    }
    (void)p; (void)serial; (void)s;
    if (w->sink) {
        nb_sink_pointer(w->sink, false);
    }
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t,
                       wl_fixed_t x, wl_fixed_t y)
{
    struct nb_wl *w = d;
    (void)p; (void)t;
    if (w->bd_hot >= 0) {
        return;                 /* chrome, not guest input */
    }
    if (w->ptr_on_dlg) {
        int was = w->dlg_hover;

        w->dlg_px_x = wl_fixed_to_int(x);
        w->dlg_px_y = wl_fixed_to_int(y);
        w->dlg_hover = dlg_hit(w, w->dlg_px_x, w->dlg_px_y);
        if (w->dlg_hover != was) {
            dlg_commit(w);
        }
        return;                 /* the overlay, not guest input */
    }
    if (w->ptr_on_tb) {
        int was = w->tb_hover;

        w->tb_px_x = wl_fixed_to_int(x);
        w->tb_px_y = wl_fixed_to_int(y);
        w->tb_hover = w->tb_w > 0 ? tb_hit(w, w->tb_px_x) : -1;
        if (w->tb_hover != was) {
            tb_update(w, w->tb_w);
        }
        return;                 /* chrome, not guest input */
    }
    if (w->sink) {
        nb_sink_abs(w->sink, wl_fixed_to_int(x), wl_fixed_to_int(y),
                    (unsigned)w->surf_w, (unsigned)w->surf_h);
    }
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t t, uint32_t button, uint32_t state)
{
    struct nb_wl *w = d;

    w->last_serial = serial;
    if (w->ptr_on_dlg) {
        /* PRESS ARMS, RELEASE ACTS, same rule as the title bar: none of these
         * four is an action anyone should trigger by a mis-click. */
        int hit = dlg_hit(w, w->dlg_px_x, w->dlg_px_y);

        if (state) {
            w->dlg_press = hit;
            dlg_commit(w);
            return;
        }
        if (w->dlg_press >= 0 && hit == w->dlg_press) {
            int choice = w->dlg_press;

            w->dlg_press = -1;
            dlg_choose(w, choice);
            return;
        }
        w->dlg_press = -1;
        dlg_commit(w);
        return;
    }
    if (w->dlg_open) {
        /*
         * A CLICK OUTSIDE CANCELS.  Standard modal behaviour, and it is what
         * stops the dialog ever being a trap: whatever else is confusing,
         * clicking away from it always works.  The click is consumed, never
         * forwarded -- it was aimed at host UI, not at the guest.
         */
        if (state) {
            dlg_hide(w);
        }
        return;
    }
    if (w->bd_hot >= 0) {
        if (state && w->toplevel && w->seat) {
            /* Logged with the serial because a compositor SILENTLY IGNORES a
             * resize carrying a stale one -- no error, no event, nothing.
             * Without this line "I never asked" and "I asked and was ignored"
             * look identical from out here. */
            nb_log("resize: edge %d requested with serial %u (window %dx%d)",
                   nb_bd_edge[w->bd_hot], serial, w->win_w, w->win_h);
            xdg_toplevel_resize(w->toplevel, w->seat, serial,
                                nb_bd_edge[w->bd_hot]);
            wl_display_flush(w->dpy);
        }
        return;
    }
    /*
     * A click on the title bar is a window-management gesture, never guest
     * input: the close box quits, anything else asks the compositor to move
     * the window (xdg_toplevel_move is the only way a Wayland client can --
     * it has no idea where its window is).
     */
    if (w->ptr_on_tb) {
        /*
         * PRESS ARMS, RELEASE ACTS -- and only if the release is over the
         * same button.  Acting on press means a mis-click cannot be taken
         * back, and "close" is not an action anyone should be able to trigger
         * by accident on a window holding a running VM.  Dragging is the
         * exception: a window move has to begin on press or there is nothing
         * to drag.
         */
        int hit = w->tb_w > 0 ? tb_hit(w, w->tb_px_x) : -1;

        if (state) {
            w->tb_press = hit;
            if (hit < 0 && w->toplevel && w->seat) {
                xdg_toplevel_move(w->toplevel, w->seat, serial);
            }
            tb_update(w, w->tb_w);
            wl_display_flush(w->dpy);
            return;
        }
        if (w->tb_press >= 0 && hit == w->tb_press) {
            switch (hit) {
            case 0:
                /*
                 * Report it and keep running.  The VMM owns the policy -- it
                 * is the only party that knows there is a guest here and what
                 * closing its display should do to it.  If nobody is connected
                 * there is no policy to defer to, so we simply quit.
                 */
                nb_log("title bar: close");
                dlg_show(w);
                break;
            case 1:
                if (w->maximized) {
                    xdg_toplevel_unset_maximized(w->toplevel);
                } else {
                    xdg_toplevel_set_maximized(w->toplevel);
                }
                break;
            case 2:
                xdg_toplevel_set_minimized(w->toplevel);
                break;
            default:
                break;
            }
        }
        w->tb_press = -1;
        tb_update(w, w->tb_w);
        wl_display_flush(w->dpy);
        return;
    }
    (void)p; (void)serial; (void)t;
    /* wl_pointer.button carries the evdev BTN_* code directly. */
    if (w->sink) {
        nb_sink_btn(w->sink, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis,
                     wl_fixed_t value)
{
    struct nb_wl *w = d;
    int v = wl_fixed_to_int(value);
    (void)p; (void)t;

    if (!w->sink) {
        return;
    }
    /* Wayland axis is "down is positive"; evdev REL_WHEEL is "up is positive". */
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        nb_sink_wheel(w->sink, v > 0 ? -1 : (v < 0 ? 1 : 0), 0);
    } else {
        nb_sink_wheel(w->sink, 0, v > 0 ? 1 : (v < 0 ? -1 : 0));
    }
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_src(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
                          uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_src, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_disc,
};

static void relptr_motion(void *d, struct zwp_relative_pointer_v1 *r,
                          uint32_t th, uint32_t tl,
                          wl_fixed_t dx, wl_fixed_t dy,
                          wl_fixed_t udx, wl_fixed_t udy)
{
    struct nb_wl *w = d;
    (void)r; (void)th; (void)tl; (void)dx; (void)dy;
    /* Unaccelerated deltas: a guest applies its own acceleration. */
    if (w->sink) {
        nb_sink_rel(w->sink, wl_fixed_to_int(udx), wl_fixed_to_int(udy));
    }
}
static const struct zwp_relative_pointer_v1_listener relptr_listener = {
    .relative_motion = relptr_motion,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    struct nb_wl *w = d;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->kbd) {
        w->kbd = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(w->kbd, &kbd_listener, w);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->ptr) {
        w->ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(w->ptr, &ptr_listener, w);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps, .name = seat_name,
};

/* ── zwp_linux_dmabuf_v1: the advertised format set ──────────────────────── */

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t fmt)
{
    struct nb_wl *w = d;
    (void)z;
    /* Version 1/2 only ever say "this format, implicit layout". */
    nb_formats_add(&w->formats, fmt, NB_DRM_FORMAT_MOD_INVALID);
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z,
                            uint32_t fmt, uint32_t hi, uint32_t lo)
{
    struct nb_wl *w = d;
    (void)z;
    nb_formats_add(&w->formats, fmt, ((uint64_t)hi << 32) | lo);
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format, .modifier = dmabuf_modifier,
};

/* ── xdg-shell ───────────────────────────────────────────────────────────── */

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
    (void)d;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct nb_wl *w = d;

    xdg_surface_ack_configure(s, serial);
    w->configured = true;
    /* Do NOT commit a buffer here: the first real content arrives as an
     * ATTACH+COMMIT from the client.  An empty commit is enough to make the
     * surface exist. */
    tb_update(w, w->win_w > 0 ? w->win_w : w->surf_w);
    bd_layout(w);
    if (w->idle_wanted && w->current < 0) {
        wl_show_idle(w->sess);
    } else if (w->current < 0) {
        wl_surface_commit(w->surf);
    }
}
static const struct xdg_surface_listener xdg_listener = {
    .configure = xdg_configure,
};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t wd,
                          int32_t ht, struct wl_array *states)
{
    struct nb_wl *w = d;
    uint32_t *st;
    bool was_fs = w->fullscreen;
    (void)t;

    /*
     * BELIEVE THE COMPOSITOR, not our own request.  Fullscreen and maximized
     * can both be entered without us asking (a compositor keybinding, a
     * double-click on the bar we do not draw), and the title bar has to
     * disappear in fullscreen however it was reached.
     */
    w->fullscreen = false;
    w->maximized = false;
    if (states) {
        wl_array_for_each(st, states) {
            if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) {
                w->fullscreen = true;
            } else if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) {
                w->maximized = true;
            }
        }
    }
    if (was_fs != w->fullscreen) {
        nb_log("window is %s", w->fullscreen ? "FULLSCREEN (chrome hidden)"
                                             : "windowed");
        /* Report what actually happened, not what we asked for: the client
         * keys a guest mode switch off this flag. */
        if (w->sink) {
            nb_sink_set_fullscreen(w->sink, w->fullscreen);
        }
        tb_update(w, w->win_w > 0 ? w->win_w : w->surf_w);
    }
    if (wd <= 0 || ht <= 0) {
        return;                 /* "pick your own size" */
    }
    /*
     * xdg_toplevel.configure reports the size of the WINDOW GEOMETRY, and our
     * geometry is the content PLUS the title bar (set_window_geometry with
     * y = -NB_TB_H, height = win_h + NB_TB_H).  So the content is what is
     * left after taking the bar back off.
     *
     * Feeding `ht` straight into win_h -- which is what this did first --
     * makes the window grow by NB_TB_H on every configure: we ask for
     * ht + NB_TB_H, the compositor obliges and tells us, we ask for
     * ht + 2*NB_TB_H, and dragging the width alone was enough to run the
     * height off the screen.  Observed 2026-08-24.
     */
    if (w->tb_mapped) {
        ht -= NB_TB_H;
    }
    if (ht <= 0) {
        ht = 1;
    }
    /*
     * A configure that changes nothing is worth skipping -- but ONLY if the
     * fullscreen state did not change too.  Leaving fullscreen back onto a
     * window that already happened to be this size still has to re-run the
     * geometry below, because the scaling policy is keyed on `fullscreen`, not
     * on the size.  Returning here on that edge is how the stale-viewport bug
     * would survive the fix for it.
     */
    if (wd == w->win_w && ht == w->win_h && was_fs == w->fullscreen) {
        return;
    }
    w->win_w = wd;
    w->win_h = ht;
    /*
     * The window changed -- dragged, maximised, or fullscreened and back.  All
     * of those are the same operation with different numbers, so they run the
     * same code as a new frame does: wl_viewport_apply() decides the
     * destination, and it is called UNCONDITIONALLY rather than only when
     * scaling is wanted, because "scaling is no longer wanted" is precisely
     * the transition that has to reset the destination.
     *
     * It is NOT a request for the guest to change resolution -- the guest owns
     * that, and conflating the two would make every window drag a mode switch.
     * The guest learns the window size through ui_info instead, and decides.
     */
    if (w->viewport && w->current >= 0) {
        int dw, dh;

        wl_viewport_apply(w, w->buf_w, w->buf_h, &dw, &dh);
        wl_surface_damage_buffer(w->surf, 0, 0, w->buf_w, w->buf_h);
        wl_surface_commit(w->surf);
        wd = dw;
        ht = dh;
    }
    /* AFTER the viewport, because wl_viewport_apply() may snap win_* back to
     * the buffer size in the 1:1 case and set_window_geometry has to agree
     * with the surface we just committed. */
    tb_update(w, w->win_w);
    bd_layout(w);
    wl_display_flush(w->dpy);
    if (w->current < 0 && w->idle_wanted) {
        wl_show_idle(w->sess);  /* repaint the placeholder at the new size */
        return;
    }
    if (wd != w->cont_w || ht != w->cont_h) {
        w->cont_w = wd;
        w->cont_h = ht;
        w->surf_w = wd;
        w->surf_h = ht;
        if (w->sink) {
            wl_report_surface(w, wd, ht);
        }
    }
}
/*
 * xdg_toplevel.close -- the compositor asking us to go away (Alt+F4, a close
 * from a window list, a session ending).  Same meaning as our own X button and
 * therefore the same path: this used to be a no-op, so those gestures did
 * nothing at all.
 */
static void top_close(void *d, struct xdg_toplevel *t)
{
    struct nb_wl *w = d;

    (void)t;
    nb_log("the compositor asked the window to close");
    dlg_show(w);
}
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure, .close = top_close,
};

/* ── our own pointer cursor ──────────────────────────────────────────────── */
/*
 * A Wayland client owns the cursor image while the pointer is over its
 * surfaces, and "owns" includes the case where it sets none: the cursor simply
 * keeps whatever the last client set.  So hiding it over the guest's picture
 * (which composites its own) means we MUST put it back over our own title bar,
 * or it stays invisible -- including when the pointer arrives on the bar
 * directly from outside the window, which never touches the content surface at
 * all.
 */


static void cur_make(struct nb_wl *w, int slot, const char *const *art,
                     int aw, int ah, int hx, int hy, int mag);

static void cur_build(struct nb_wl *w)
{
    /* Plain arrow: 1 = black outline, 2 = white fill, space = transparent. */
    static const char *const arrow[] = {
        "1           ", "11          ", "121         ", "1221        ",
        "12221       ", "122221      ", "1222221     ", "12222221    ",
        "122222221   ", "1222222221  ", "12222222221 ", "122222111111",
        "1221221     ", "121 1221    ", "11  1221    ", "1    1221   ",
        "     1221   ", "      121   ", "      11    ",
    };
    /* Double-headed arrows for the resize edges. */
    static const char *const ew[] = {
        "   1     1   ", "  11     11  ", " 121     121 ",
        "12222222222221", "121     121 ", "  11     11  ",
        "   1     1   ",
    };
    static const char *const ns[] = {
        "   1   ", "  111  ", " 12121 ", "1211121", "  121  ",
        "  121  ", "  121  ", "  121  ", "1211121", " 12121 ",
        "  111  ", "   1   ",
    };
    static const char *const nwse[] = {
        "1111111    ", "1222211    ", "122211     ", "1221221    ",
        "121 1221   ", "11   1221  ", "1     1221 ", "       122211",
        "        11221", "         1221", "     1111111",
    };
    static const char *const nesw[] = {
        "    1111111", "    1122221", "     112221", "    1221221",
        "   1221 121", "  1221   11", " 1221     1", "112211      ",
        "12211       ", "1221        ", "1111111     ",
    };

    if (w->cur_buf[NB_CUR_ARROW]) {
        return;
    }
    cur_make(w, NB_CUR_ARROW, arrow, 12, 19, 0, 0, 1);
    cur_make(w, NB_CUR_EW,    ew,   14,  7, 7, 3, 2);
    cur_make(w, NB_CUR_NS,    ns,    7, 12, 3, 6, 2);
    cur_make(w, NB_CUR_NWSE,  nwse, 13, 11, 6, 5, 2);
    cur_make(w, NB_CUR_NESW,  nesw, 11, 11, 5, 5, 2);
}

/*
 * One cursor image, drawn rather than loaded.  libwayland-cursor plus an
 * XCURSOR theme lookup would be a new dependency and a filesystem search, for
 * five small bitmaps that are only ever seen over this broker's own chrome.
 */
static void cur_make(struct nb_wl *w, int slot, const char *const *art,
                     int aw, int ah, int hx, int hy, int mag)
{
    /*
     * `mag` is nearest-neighbour magnification, per cursor on purpose.  The
     * resize art is a 7x12-ish bitmap that reads as a speck next to a normal
     * ~24px theme cursor -- the bottom-edge one worst, being the smallest --
     * so those go 2x.  The plain arrow is already 12x19 and looked oversized
     * doubled, so it stays 1x.  Either way: no theme lookup, no new
     * dependency.
     */
    if (mag < 1) { mag = 1; }
    struct wl_shm_pool *pool;
    const int bwd = aw * mag, bht = ah * mag;
    size_t stride = (size_t)bwd * 4, sz = stride * (size_t)bht;
    uint32_t *px;
    int fd, x, y;

    if (!w->shm || !w->comp || slot < 0 || slot >= NB_CUR_N) {
        return;
    }
    fd = memfd_create("nvkvm-broker-cursor", MFD_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (ftruncate(fd, (off_t)sz) < 0) {
        close(fd);
        return;
    }
    px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    for (y = 0; y < bht; y++) {
        const char *row = art[y / mag];
        int len = (int)strlen(row);

        for (x = 0; x < bwd; x++) {
            int sx = x / mag;
            char c = sx < len ? row[sx] : ' ';

            px[(size_t)y * bwd + x] = c == '1' ? 0xff000000U
                                    : c == '2' ? 0xffffffffU
                                    : 0x00000000U;
        }
    }
    pool = wl_shm_create_pool(w->shm, fd, (int32_t)sz);
    close(fd);
    if (!pool) {
        munmap(px, sz);
        return;
    }
    w->cur_hx[slot] = hx * mag;
    w->cur_hy[slot] = hy * mag;
    w->cur_buf[slot] = wl_shm_pool_create_buffer(pool, 0, bwd, bht,
                                                 (int32_t)stride,
                                                 WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!w->cur_buf[slot]) {
        munmap(px, sz);
        return;
    }
    w->cur_px[slot] = px;
    w->cur_sz[slot] = sz;
    w->cur_surf[slot] = wl_compositor_create_surface(w->comp);
    if (w->cur_surf[slot]) {
        wl_surface_attach(w->cur_surf[slot], w->cur_buf[slot], 0, 0);
        wl_surface_damage_buffer(w->cur_surf[slot], 0, 0, aw, ah);
        wl_surface_commit(w->cur_surf[slot]);
    }
}

/* ── resize borders ──────────────────────────────────────────────────────── */
/*
 * Eight invisible subsurfaces around the window -- four edges, four corners.
 * They exist ONLY to receive a pointer: each is a single fully transparent
 * pixel stretched to its rectangle by a viewport, with an empty opaque region
 * (so it never occludes anything, and in particular never disqualifies the
 * content surface from direct scanout) and a full input region.
 *
 * A press on one becomes xdg_toplevel_resize(), which is the only way a
 * Wayland client can be resized by a drag: it does not know where its window
 * is on screen, so it hands the gesture to the compositor along with the edge
 * and the compositor does the rest.
 */


static void bd_geometry(const struct nb_wl *w, int i, int *x, int *y,
                        int *bw, int *bh)
{
    /*
     * THESE ARE CHILDREN OF THE CONTENT SURFACE, so their positions are in
     * CONTENT-local coordinates -- and the content is not the window.  When
     * --scale aspect is drawing letterbox bars the content sits off_x/off_y
     * in from the window's edge, so a border placed at content x=0 lands
     * off_x inside the window instead of on its edge.
     *
     * OBSERVED: the resize edges were wrong ONLY on the axis that had
     * whitespace, and by exactly the bar's width.  The title bar already
     * compensates the same way (-off_x, -off_y - NB_TB_H); the borders did
     * not.
     */
    const int tb   = w->tb_mapped ? NB_TB_H : 0;
    const int left = -w->off_x;                 /* window's left edge   */
    const int top  = -w->off_y - tb;            /* window's top, incl. the bar */
    int ww = w->win_w > 0 ? w->win_w : 1;
    int wh = (w->win_h > 0 ? w->win_h : 1) + tb;
    const int b = NB_BORDER;

    switch (i) {
    case 0: *x = left;     *y = top - b;  *bw = ww; *bh = b;  break; /* top    */
    case 1: *x = left;     *y = top + wh; *bw = ww; *bh = b;  break; /* bottom */
    case 2: *x = left - b; *y = top;      *bw = b;  *bh = wh; break; /* left   */
    case 3: *x = left + ww; *y = top;     *bw = b;  *bh = wh; break; /* right  */
    case 4: *x = left - b; *y = top - b;  *bw = b;  *bh = b;  break;
    case 5: *x = left + ww; *y = top - b; *bw = b;  *bh = b;  break;
    case 6: *x = left - b; *y = top + wh; *bw = b;  *bh = b;  break;
    default:*x = left + ww; *y = top + wh; *bw = b; *bh = b;  break;
    }
}

static void bd_layout(struct nb_wl *w)
{
    int i, x, y, bw, bh;

    if (!w->bd_buf) {
        return;
    }
    /*
     * Idempotence, so the per-frame caller costs six int compares.  Without
     * that caller the borders were placed once at xdg_configure -- when off_x
     * and off_y are still 0 because no frame has arrived to letterbox -- and
     * nothing moved them until the first resize.  That is why the grab areas
     * started out displaced and silently corrected themselves after one
     * maximise.
     */
    if (w->bd_at_valid
        && w->bd_at_ox == w->off_x && w->bd_at_oy == w->off_y
        && w->bd_at_w  == w->win_w && w->bd_at_h  == w->win_h
        && w->bd_at_tb == w->tb_mapped && w->bd_at_fs == w->fullscreen) {
        return;
    }
    w->bd_at_ox = w->off_x; w->bd_at_oy = w->off_y;
    w->bd_at_w  = w->win_w; w->bd_at_h  = w->win_h;
    w->bd_at_tb = w->tb_mapped; w->bd_at_fs = w->fullscreen;
    w->bd_at_valid = true;
    /*
     * UNMAP IN FULLSCREEN, like the title bar.  There is nothing to resize a
     * fullscreen window by, and -- the reason that matters here -- fullscreen
     * is the state in which the content surface can reach a hardware plane.
     * Eight extra mapped subsurfaces hanging off it are eight more reasons
     * for a compositor to decline, for chrome nobody can use.
     */
    if (w->fullscreen) {
        for (i = 0; i < 8; i++) {
            if (w->bd_surf[i]) {
                wl_surface_attach(w->bd_surf[i], NULL, 0, 0);
                wl_surface_commit(w->bd_surf[i]);
            }
        }
        wl_surface_commit(w->surf);
        wl_display_flush(w->dpy);
        return;
    }
    for (i = 0; i < 8; i++) {
        if (!w->bd_surf[i]) {
            continue;
        }
        bd_geometry(w, i, &x, &y, &bw, &bh);
        if (bw <= 0 || bh <= 0) {
            continue;
        }
        wl_subsurface_set_position(w->bd_sub[i], x, y);
        if (w->bd_vp[i]) {
            wp_viewport_set_destination(w->bd_vp[i], bw, bh);
        }
        wl_surface_attach(w->bd_surf[i], w->bd_buf, 0, 0);
        wl_surface_damage_buffer(w->bd_surf[i], 0, 0, 1, 1);
        wl_surface_commit(w->bd_surf[i]);
    }
    wl_surface_commit(w->surf);
    wl_display_flush(w->dpy);
}

static void bd_build(struct nb_wl *w)
{
    struct wl_shm_pool *pool;
    uint32_t *px;
    int fd, i;

    if (w->bd_buf || !w->shm || !w->subcomp || !w->viewporter) {
        return;
    }
    fd = memfd_create("nvkvm-broker-border", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 4) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    px = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return;
    }
    *px = 0x00000000U;                  /* fully transparent */
    pool = wl_shm_create_pool(w->shm, fd, 4);
    close(fd);
    if (!pool) {
        munmap(px, 4);
        return;
    }
    w->bd_buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!w->bd_buf) {
        munmap(px, 4);
        return;
    }
    w->bd_px = px;
    for (i = 0; i < 8; i++) {
        struct wl_region *none;

        w->bd_surf[i] = wl_compositor_create_surface(w->comp);
        if (!w->bd_surf[i]) {
            continue;
        }
        w->bd_sub[i] = wl_subcompositor_get_subsurface(w->subcomp,
                                                       w->bd_surf[i], w->surf);
        w->bd_vp[i] = wp_viewporter_get_viewport(w->viewporter, w->bd_surf[i]);
        if (w->bd_sub[i]) {
            wl_subsurface_place_below(w->bd_sub[i], w->surf);
            wl_subsurface_set_desync(w->bd_sub[i]);
        }
        none = wl_compositor_create_region(w->comp);
        if (none) {
            wl_surface_set_opaque_region(w->bd_surf[i], none);
            wl_region_destroy(none);
        }
    }
    nb_log("resize borders: %d px of grabbable edge around the window",
           NB_BORDER);
}

static int bd_index(const struct nb_wl *w, const struct wl_surface *s)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (w->bd_surf[i] == s) {
            return i;
        }
    }
    return -1;
}

/* ── presentation feedback: FLIP or COPY ─────────────────────────────────── */
static void pres_discarded(void *d, struct wp_presentation_feedback *f)
{
    (void)d;
    wp_presentation_feedback_destroy(f);
}
static void pres_sync_output(void *d, struct wp_presentation_feedback *f,
                             struct wl_output *o)
{ (void)d; (void)f; (void)o; }
static void pres_presented(void *d, struct wp_presentation_feedback *f,
                           uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                           uint32_t tv_nsec, uint32_t refresh,
                           uint32_t sh, uint32_t sl, uint32_t flags)
{
    struct nb_wl *w = d;
    int zc = (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) ? 1 : 0;

    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec; (void)sh; (void)sl;
    wp_presentation_feedback_destroy(f);

    /*
     * THE HOST'S REFRESH, which the guest otherwise cannot know.
     *
     * Its virtual head is pinned by the kms_hz module parameter (default 60)
     * while the host may be running at 144, so the guest renders at a rate
     * nothing asked for and neither side is aware.  `refresh` here is the
     * output's nominal frame interval in NANOSECONDS; QemuUIInfo.refresh_rate
     * and our EV_SURFACE both use MILLIHERTZ, so 16666666ns -> 60000.
     *
     * Compared at whole-Hz granularity before re-hinting: the value is nominal
     * rather than measured and should be stable, but a hint per frame would be
     * a re-mode per frame, and that is the shape of bug this file has already
     * had twice.
     */
    if (refresh > 0) {
        unsigned mhz = (unsigned)(1000000000000ULL / refresh);

        if (nb_hz_mode == NB_HZ_AUTO && mhz / 1000 != w->refresh_mhz / 1000) {
            unsigned was = w->refresh_mhz;

            w->refresh_mhz = mhz;
            nb_log("host output refresh is %u.%03u Hz — telling the guest, so "
                   "it need not stay at its built-in default",
                   mhz / 1000, mhz % 1000);
            if (was && w->sink) {
                /* Not the first learn: geometry is settled, so re-hint now. */
                w->hint_w = 0;          /* force the idempotence check open */
                wl_report_surface(w, w->cont_w, w->cont_h);
            }
        }
    }
    if (zc != w->last_scanout) {
        w->last_scanout = zc;
        /*
         * Deliberately worded like the X11 backend's line, because it is the
         * same fact: FLIP means the guest's own buffer is on a hardware plane
         * and nothing copied it.
         */
        nb_log("Present: %s%s  (refresh %u.%03u ms)",
               zc ? "FLIP" : "COPY",
               zc ? " - the guest's buffer is scanned out directly"
                  : " - the compositor is compositing it",
               refresh / 1000000, (refresh % 1000000) / 1000);
    }
}
const struct wp_presentation_feedback_listener nb_pres_listener = {
    .sync_output = pres_sync_output,
    .presented   = pres_presented,
    .discarded   = pres_discarded,
};

/* ── decorations ─────────────────────────────────────────────────────────── */
/*
 * A Wayland toplevel has no chrome unless somebody draws it.  With neither
 * client-side decorations nor a server-side request the window arrives bare:
 * no title bar, no close button, nothing to drag a corner by — and the title
 * we set is never rendered anywhere, so --title looks broken.  We ask the
 * compositor to decorate, and we BELIEVE ITS ANSWER rather than the request:
 * a compositor is allowed to reply client-side, and then a chrome-less window
 * is what the user gets and they should be told why.
 */
static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *dec,
                           uint32_t mode)
{
    struct nb_wl *w = d;
    (void)dec;

    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        nb_log("window decorations: server-side (title bar, close, resize)");
    } else {
        nb_log("window decorations: the compositor insists on CLIENT-side, "
               "and this broker draws none — expect a bare window with no "
               "title bar and no resize grips.");
    }
    w->deco_mode = mode;
}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};

/* ── registry ────────────────────────────────────────────────────────────── */

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    struct nb_wl *w = data;

#define BIND(I, V) wl_registry_bind(r, name, &I, (ver < (V)) ? ver : (V))
    if (!strcmp(iface, wl_compositor_interface.name)) {
        w->comp = BIND(wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_seat_interface.name) && !w->seat) {
        w->seat = BIND(wl_seat_interface, 5);
        wl_seat_add_listener(w->seat, &seat_listener, w);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        w->deco_mgr = BIND(zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_subcompositor_interface.name)) {
        w->subcomp = BIND(wl_subcompositor_interface, 1);
    } else if (!strcmp(iface, wp_presentation_interface.name)) {
        w->presentation = BIND(wp_presentation_interface, 1);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        w->frac_mgr = BIND(wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        w->ddm = BIND(wl_data_device_manager_interface, 3);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        w->viewporter = BIND(wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wl_shm_interface.name) && !w->shm) {
        w->shm = BIND(wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        w->wm_base = BIND(xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wm_base, &wm_listener, w);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) &&
               !w->dmabuf) {
        /*
         * Cap at 3 ON PURPOSE.  From version 4 the compositor MUST NOT send
         * the `format`/`modifier` events — the per-surface feedback object
         * replaces them — so binding 4 would leave the advertised set empty
         * and every ATTACH would be rejected as an unadvertised format.
         */
        w->dmabuf_ver = ver < 3 ? ver : 3;
        w->dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface,
                                     w->dmabuf_ver);
        zwp_linux_dmabuf_v1_add_listener(w->dmabuf, &dmabuf_listener, w);
    } else if (!strcmp(iface,
               zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
        w->inhibit_mgr =
            BIND(zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
        w->constraints = BIND(zwp_pointer_constraints_v1_interface, 1);
    } else if (!strcmp(iface,
               zwp_relative_pointer_manager_v1_interface.name)) {
        w->relptr_mgr = BIND(zwp_relative_pointer_manager_v1_interface, 1);
    }
#undef BIND
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_remove,
};

/* ── ops: event loop ─────────────────────────────────────────────────────── */

static int wl_pollfds(struct nb_session *s, struct pollfd *out, int max)
{
    struct nb_wl *w = s->priv;
    int r;

    if (max < 1) {
        return 0;
    }
    /*
     * Flush before the poll, non-blocking.  wl_display_flush() returns -1 with
     * EAGAIN when the compositor socket is full; we then ask poll() for
     * POLLOUT and try again, rather than looping on write.  This is the rule
     * "input must never block on rendering": the same thread carries both, so
     * a blocking flush to a wedged compositor would stall the keyboard.
     */
    r = wl_display_flush(w->dpy);
    w->flush_blocked = (r < 0 && errno == EAGAIN);

    out[0].fd = wl_display_get_fd(w->dpy);
    out[0].events = POLLIN | (w->flush_blocked ? POLLOUT : 0);
    out[0].revents = 0;
    w->pfd = out;
    if (w->fetch_fd >= 0 && max >= 2) {
        /* The selection pipe.  Polled rather than read inline: the transfer is
         * the compositor's to pace, and blocking on it here would stall input
         * for as long as the other client takes to write. */
        out[1].fd = w->fetch_fd;
        out[1].events = POLLIN;
        out[1].revents = 0;
        return 2;
    }
    return 1;
}

/*
 * The main loop calls dispatch() on every wakeup, not only when the compositor
 * fd is readable — the client socket or a signal get us here too.
 * wl_display_read_events() BLOCKS, so it may only be called when poll actually
 * said POLLIN; otherwise the broker would stall on the compositor and stop
 * forwarding input, which is exactly the coupling this design forbids.
 */
static int wl_dispatch_session(struct nb_session *s, struct nb_sink *sink)
{
    struct nb_wl *w = s->priv;
    bool readable = w->pfd && (w->pfd->revents & (POLLIN | POLLHUP | POLLERR));

    w->sink = sink;

    /*
     * CLIPBOARD, both halves, before the Wayland dispatch below -- the pipe is
     * a separate fd and its readiness has nothing to do with the compositor's.
     */
    if (w->fetch_fd >= 0 && w->pfd && (w->pfd[1].revents & (POLLIN | POLLHUP))) {
        (void)wl_fetch_pump(w, sink);
    }

    if (w->pfd && (w->pfd->revents & POLLOUT)) {
        wl_display_flush(w->dpy);
    }
    if (wl_display_prepare_read(w->dpy) != 0) {
        /* Another queue already has events buffered; drain those first. */
        if (wl_display_dispatch_pending(w->dpy) < 0) {
            return -EIO;
        }
        return 0;
    }
    if (!readable) {
        wl_display_cancel_read(w->dpy);
        if (wl_display_dispatch_pending(w->dpy) < 0) {
            return -EIO;
        }
        return 0;
    }
    if (wl_display_read_events(w->dpy) < 0) {
        return -EIO;
    }
    if (wl_display_dispatch_pending(w->dpy) < 0) {
        return -EIO;
    }
    if (w->quit) {
        /* The close box.  Same shape as the display going away: main() says
         * goodbye to the client and tears the session down. */
        return -ECONNRESET;
    }
    return 0;
}

/*
 * The compositor answers a grab request with events, not with a return code,
 * and nothing here used to listen for them.  A capability that advertises
 * true and then silently declines is exactly the failure this project keeps
 * finding, so both objects are now listened to and every transition is
 * logged: what was requested, and what the compositor actually did with it.
 */
static void inhibitor_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i)
{
    (void)d; (void)i;
    nb_log("grab: shortcuts inhibitor ACTIVE — compositor bindings "
           "(Super, Alt+Tab, ...) now reach the guest");
}
static void inhibitor_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i)
{
    (void)d; (void)i;
    nb_log("grab: shortcuts inhibitor INACTIVE — the compositor keeps its own "
           "bindings; they will NOT reach the guest");
}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    .active = inhibitor_active, .inactive = inhibitor_inactive,
};

static void lockptr_locked(void *d, struct zwp_locked_pointer_v1 *l)
{
    (void)d; (void)l;
    nb_log("grab: pointer LOCKED to the window");
}
static void lockptr_unlocked(void *d, struct zwp_locked_pointer_v1 *l)
{
    (void)d; (void)l;
    nb_log("grab: pointer lock RELEASED by the compositor");
}
static const struct zwp_locked_pointer_v1_listener lockptr_listener = {
    .locked = lockptr_locked, .unlocked = lockptr_unlocked,
};

static int wl_set_grab(struct nb_session *s, bool on)
{
    struct nb_wl *w = s->priv;

    if (w->grabbed == on) {
        return 0;
    }
    nb_log("grab %s: requesting%s%s%s", on ? "ON" : "off",
           w->inhibit_mgr ? " shortcuts-inhibit" : " (no shortcuts-inhibit)",
           w->constraints  ? " pointer-lock"     : " (no pointer-lock)",
           w->relptr_mgr   ? " relative-pointer" : " (no relative-pointer)");
    if (on) {
        if (!w->ptr) {
            nb_log("grab: no wl_pointer on this seat — neither the pointer "
                   "lock nor relative motion can be requested at all");
        }
        if (w->inhibit_mgr && !w->inhibitor) {
            w->inhibitor =
                zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                    w->inhibit_mgr, w->surf, w->seat);
            if (w->inhibitor) {
                zwp_keyboard_shortcuts_inhibitor_v1_add_listener(
                    w->inhibitor, &inhibitor_listener, w);
            }
        }
        if (w->constraints && w->ptr && !w->lock) {
            w->lock = zwp_pointer_constraints_v1_lock_pointer(
                w->constraints, w->surf, w->ptr, NULL,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            if (w->lock) {
                zwp_locked_pointer_v1_add_listener(w->lock, &lockptr_listener,
                                                   w);
            }
        }
        if (w->relptr_mgr && w->ptr && !w->relptr) {
            w->relptr = zwp_relative_pointer_manager_v1_get_relative_pointer(
                w->relptr_mgr, w->ptr);
            zwp_relative_pointer_v1_add_listener(w->relptr,
                                                 &relptr_listener, w);
        }
    } else {
        if (w->inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(w->inhibitor);
            w->inhibitor = NULL;
        }
        if (w->lock) {
            zwp_locked_pointer_v1_destroy(w->lock);
            w->lock = NULL;
        }
        if (w->relptr) {
            zwp_relative_pointer_v1_destroy(w->relptr);
            w->relptr = NULL;
        }
    }
    w->grabbed = on;
    /* The bar is the only thing on screen that can say how to get out. */
    tb_update(w, w->tb_w > 0 ? w->tb_w : w->win_w);
    /*
     * Flush NOW.  Everything above is a queued request, and the only other
     * flush in this backend is the POLLOUT path, which does not run until the
     * compositor socket has something to say.  Without this the grab is issued
     * on the wire whenever the next frame happens to flush — which on an idle
     * guest is "eventually", and looks exactly like a grab that does nothing.
     */
    wl_display_flush(w->dpy);
    return 0;
}

static int wl_set_fullscreen(struct nb_session *s, bool on)
{
    struct nb_wl *w = s->priv;

    if (!w->toplevel) {
        return -ENOTSUP;
    }
    /*
     * Fullscreen is where the frame can actually reach a hardware plane: a
     * compositor promotes an opaque, unoccluded, correctly formatted surface
     * covering an output to DIRECT SCANOUT.  It cannot be requested, only
     * qualified for — this is the request for the precondition.
     */
    if (on) {
        xdg_toplevel_set_fullscreen(w->toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(w->toplevel);
    }
    w->fullscreen = on;
    return 0;
}

static void wl_close_session(struct nb_session *s)
{
    struct nb_wl *w = s->priv;
    int i;

    if (!w) {
        return;
    }
    wl_set_grab(s, false);
    if (w->fetch_fd >= 0) {
        close(w->fetch_fd);
    }
    if (w->pending_offer && w->pending_offer != w->offer) {
        wl_data_offer_destroy(w->pending_offer);
    }
    if (w->offer) {
        wl_data_offer_destroy(w->offer);
    }
    if (w->source) {
        wl_data_source_destroy(w->source);
    }
    free(w->src_text);
    free(w->clip_pending);
    w->clip_pending = NULL;
    for (i = 0; i < NB_MAX_BUFS; i++) {
        wl_buf_destroy(w, i);
    }
    wl_idle_drop(w);
    tb_drop(w);
    for (i = 0; i < NB_CUR_N; i++) {
        if (w->cur_buf[i]) {
            wl_buffer_destroy(w->cur_buf[i]);
        }
        if (w->cur_px[i]) {
            munmap(w->cur_px[i], w->cur_sz[i]);
        }
        if (w->cur_surf[i]) {
            wl_surface_destroy(w->cur_surf[i]);
        }
    }
    for (i = 0; i < 8; i++) {
        if (w->bd_vp[i]) {
            wp_viewport_destroy(w->bd_vp[i]);
        }
        if (w->bd_sub[i]) {
            wl_subsurface_destroy(w->bd_sub[i]);
        }
        if (w->bd_surf[i]) {
            wl_surface_destroy(w->bd_surf[i]);
        }
    }
    if (w->bd_buf) {
        wl_buffer_destroy(w->bd_buf);
        w->bd_buf = NULL;
    }
    if (w->bd_px) {
        munmap(w->bd_px, 4);
        w->bd_px = NULL;
    }
    if (w->tb_sub) {
        wl_subsurface_destroy(w->tb_sub);
        w->tb_sub = NULL;
    }
    if (w->tb_surf) {
        wl_surface_destroy(w->tb_surf);
        w->tb_surf = NULL;
    }
    if (w->viewport) {
        wp_viewport_destroy(w->viewport);
        w->viewport = NULL;
    }
    if (w->deco) {
        zxdg_toplevel_decoration_v1_destroy(w->deco);
        w->deco = NULL;
    }
    if (w->dpy) {
        wl_display_flush(w->dpy);
        wl_display_disconnect(w->dpy);
    }
    free(w);
    s->priv = NULL;
    free(s);
}

static int wl_open(struct nb_session *s, const struct nb_config *cfg)
{
    struct nb_wl *w = s->priv;
    int missing = 0;

    w->sess = s;
    w->surf_w = (int)cfg->win_w;
    w->surf_h = (int)cfg->win_h;
    w->pending = -1;
    w->current = -1;
    w->last_scanout = -1;
    /* 0 is a valid button index, so "none" has to be spelled -1 explicitly:
     * a calloc'd struct would otherwise open with the close button drawn as
     * hovered AND held. */
    w->tb_hover = -1;
    w->tb_press = -1;
    w->bd_hot = -1;
    w->scale_mode = cfg->scale_mode;

    w->dpy = wl_display_connect(NULL);
    if (!w->dpy) {
        nb_err("wl_display_connect failed (WAYLAND_DISPLAY=%s)",
               getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        return -ENOENT;
    }
    w->reg = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(w->reg, &reg_listener, w);
    wl_display_roundtrip(w->dpy);   /* globals */
    wl_display_roundtrip(w->dpy);   /* the dmabuf format/modifier burst */
    /*
     * The data device needs BOTH the seat and the manager, and registry order
     * is not guaranteed -- so it is created here, after the globals are in,
     * rather than in whichever handler happened to run second.
     */
    if (w->ddm && w->seat) {
        w->ddev = wl_data_device_manager_get_data_device(w->ddm, w->seat);
        if (w->ddev) {
            wl_data_device_add_listener(w->ddev, &ddev_listener, w);
        }
    }
    if (!w->ddev) {
        nb_log("no wl_data_device: clipboard is unavailable on this "
               "compositor, whatever --clipboard says");
    }
    if (w->ddev) {
        s->clipboard_caps = NB_SESSION_CLIP_G2H | NB_SESSION_CLIP_H2G;
    }

    if (!w->comp || !w->wm_base) {
        nb_err("the compositor is missing %s%s— there is no window to make",
               w->comp ? "" : "wl_compositor ",
               w->wm_base ? "" : "xdg_wm_base ");
        goto fail;
    }
    if (!w->seat) {
        /*
         * No seat means no input and, critically, no wl_keyboard.leave — so
         * focus loss is unobservable and grab must not be offered.  Not fatal:
         * a display with no input is still a display, and saying so beats
         * refusing to start with a message about a global nobody has heard of.
         */
        nb_err("this compositor advertises no wl_seat: there will be NO "
               "keyboard, NO pointer and NO grab. Display only.");
    }
    if (!w->dmabuf) {
        nb_err("this compositor does not implement zwp_linux_dmabuf_v1, so it "
               "cannot take the guest's buffer without a copy.  There is no "
               "fallback here on purpose: a copy path would need a GL context "
               "in the privileged process, which is what this design removes.");
        goto fail;
    }
    if (w->dmabuf_ver < 2) {
        nb_err("zwp_linux_dmabuf_v1 version %u has no create_immed; version 2 "
               "or later is required (universally available since 2016)",
               w->dmabuf_ver);
        goto fail;
    }
    if (w->formats.n == 0) {
        nb_err("the compositor advertised no dma-buf formats at all — nothing "
               "could be validated against, so every frame would be rejected");
        goto fail;
    }

    /* The window. */
    w->scale_120 = 120;         /* until the compositor says otherwise */
    w->fetch_fd = -1;
    w->surf = wl_compositor_create_surface(w->comp);
    if (w->surf && w->frac_mgr) {
        w->frac = wp_fractional_scale_manager_v1_get_fractional_scale(
                      w->frac_mgr, w->surf);
        if (w->frac) {
            wp_fractional_scale_v1_add_listener(w->frac, &frac_listener, w);
        }
    }
    w->xdg_surf = xdg_wm_base_get_xdg_surface(w->wm_base, w->surf);
    xdg_surface_add_listener(w->xdg_surf, &xdg_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surf);
    xdg_toplevel_add_listener(w->toplevel, &top_listener, w);
    xdg_toplevel_set_title(w->toplevel, cfg->title);
    xdg_toplevel_set_app_id(w->toplevel, "nvkvm-display-broker");
    if (w->deco_mgr) {
        w->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
            w->deco_mgr, w->toplevel);
        if (w->deco) {
            zxdg_toplevel_decoration_v1_add_listener(w->deco, &deco_listener,
                                                     w);
            zxdg_toplevel_decoration_v1_set_mode(
                w->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    } else {
        nb_log("no zxdg_decoration_manager_v1: the window will have no title "
               "bar, no close button and no resize grips, and --title will "
               "render nowhere.  That is the compositor's choice, not a bug "
               "here.");
    }
    /*
     * --title is the base title, and it is the user's to choose.  Clamped to
     * NB_TITLE_MAX drawn characters: the bar has three buttons on the right
     * and a fixed-width font, and a title long enough to run under them is
     * worse than a truncated one.
     */
    {
        const char *t = cfg->title && *cfg->title ? cfg->title : "nvkvm";

        if (strlen(t) > NB_TITLE_MAX) {
            snprintf(w->title, sizeof(w->title), "%.*s...",
                     (int)NB_TITLE_MAX - 3, t);
            nb_log("--title truncated to %zu characters: \"%s\"",
                   NB_TITLE_MAX, w->title);
        } else {
            snprintf(w->title, sizeof(w->title), "%s", t);
        }
    }
    /*
     * Draw our own title bar when -- and only when -- the compositor will not.
     * Mutter does not implement zxdg_decoration_manager_v1 (GNOME Shell 50.1,
     * verified 2026-08-24), so on GNOME this is the only way the window gets a
     * title, buttons, or anything to drag it by.
     */
    if (!w->deco_mgr && w->subcomp && w->shm) {
        w->tb_surf = wl_compositor_create_surface(w->comp);
        if (w->tb_surf) {
            struct wl_region *op = wl_compositor_create_region(w->comp);

            w->tb_sub = wl_subcompositor_get_subsurface(w->subcomp,
                                                        w->tb_surf, w->surf);
            if (w->tb_sub) {
                /*
                 * ABOVE the content, at negative y, NOT overlapping it.  The
                 * guest's picture stays whole -- the window is simply
                 * NB_TB_H taller than the guest's resolution, which is what
                 * a title bar is -- and, because nothing occludes the content
                 * surface, it remains a direct-scanout candidate even while
                 * windowed.  An overlay bar would forfeit that for chrome.
                 */
                wl_subsurface_set_position(w->tb_sub, 0, -NB_TB_H);
                wl_subsurface_place_above(w->tb_sub, w->surf);
                wl_subsurface_set_desync(w->tb_sub);
                nb_log("drawing a client-side title bar (%d px): this "
                       "compositor offers no server-side decorations",
                       NB_TB_H);
            }
            if (op) {
                wl_region_add(op, 0, 0, 1 << 20, NB_TB_H);
                wl_surface_set_opaque_region(w->tb_surf, op);
                wl_region_destroy(op);
            }
        }
    }
    bd_build(w);
    if (cfg->fullscreen) {
        /*
         * Asking at startup, before the first configure, is the only way to
         * get a fullscreen window without a keystroke -- which is what makes
         * the direct-scanout question measurable from a script instead of
         * from somebody's fingers.
         */
        xdg_toplevel_set_fullscreen(w->toplevel, NULL);
        w->fullscreen = true;
        nb_log("starting fullscreen (--fullscreen)");
    }
    if (w->viewporter) {
        w->viewport = wp_viewporter_get_viewport(w->viewporter, w->surf);
    } else {
        nb_log("no wp_viewporter: the window is fixed at the guest's own "
               "resolution and cannot be resized independently of it");
    }
    w->win_w = w->surf_w;
    w->win_h = w->surf_h;
    wl_surface_commit(w->surf);
    wl_display_roundtrip(w->dpy);   /* configure, and the decoration mode */

    s->width = (uint32_t)w->surf_w;
    s->height = (uint32_t)w->surf_h;
    s->caps = NVKVM_BROKER_CAP_FULLSCREEN | NVKVM_BROKER_CAP_DMABUF |
              NVKVM_BROKER_CAP_RELEASE;
    if (w->seat) {
        s->caps |= NVKVM_BROKER_CAP_KEYBOARD | NVKVM_BROKER_CAP_ABS_POINTER |
                   NVKVM_BROKER_CAP_FOCUS_EVENTS;
    }
    if (w->dmabuf_ver >= 3) {
        s->caps |= NVKVM_BROKER_CAP_MODIFIERS;
    } else {
        nb_log("zwp_linux_dmabuf_v1 is version 2: no explicit modifiers, so "
               "only implicitly-modified buffers will be accepted");
    }
    if (w->relptr_mgr && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_REL_POINTER;
    } else {
        missing++;
    }
    if (w->constraints && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_POINTER_LOCK;
    } else {
        missing++;
    }
    if (w->inhibit_mgr && w->seat) {
        s->caps |= NVKVM_BROKER_CAP_TOTAL_GRAB;
    } else {
        missing++;
    }

    /* Say exactly what is missing, at startup, in one line. */
    if (missing) {
        snprintf(s->grab_caveat, sizeof(s->grab_caveat),
                 "compositor is missing: %s%s%s",
                 w->inhibit_mgr ? ""
                     : "keyboard-shortcuts-inhibit (Super and other compositor "
                       "shortcuts WILL still fire under grab); ",
                 w->constraints ? "" : "pointer-constraints (pointer NOT "
                                       "confined); ",
                 w->relptr_mgr ? "" : "relative-pointer (NO relative motion "
                                      "under grab); ");
    }
    nb_formats_log(&w->formats, "the compositor");

    /*
     * ACCEPT SHARED MEMORY TOO.
     *
     * wl_shm is a core Wayland global that every compositor must implement, so
     * a shm buffer is the one thing a display CANNOT refuse.  That makes it the
     * universal last tier for the VMM, after a display has advertised a
     * modifier it then would not import -- MEASURED on GNOME/Mutter with
     * NVIDIA 595.84, which advertises XR24+LINEAR through
     * eglQueryDmaBufModifiersEXT and answers the import with "Could not bind
     * the given EGLImage to a CoglTexture2D".
     *
     * It costs the compositor an upload per frame, which is why it is the LAST
     * tier and not the first.  The VMM only reaches for it when the dma-buf
     * tiers have been refused.
     */
    s->accept_memfd = true;
    return 0;

fail:
    if (w->dpy) {
        wl_display_disconnect(w->dpy);
        w->dpy = NULL;
    }
    return -ENOTSUP;
}

static const struct nb_session_ops wl_ops = {
    .name = "wayland",
    .open = wl_open,
    .close = wl_close_session,
    .pollfds = wl_pollfds,
    .dispatch = wl_dispatch_session,
    .set_grab = wl_set_grab,
    .set_fullscreen = wl_set_fullscreen,
    .format_ok = wl_format_ok,
    .attach = wl_attach,
    .commit = wl_commit,
    .resize = wl_resize,
    .client_detach = wl_client_detach,
    .show_idle = wl_show_idle,
    .dismiss_dialog = wl_dismiss_dialog,
    .set_clipboard = wl_set_clipboard,
    .tick = wl_tick,
    .notify_clipboard = wl_notify_clipboard,
    .fetch_clipboard = wl_fetch_clipboard,
};

struct nb_session *nb_session_wayland(const struct nb_config *cfg)
{
    struct nb_session *s = calloc(1, sizeof(*s));
    struct nb_wl *w = calloc(1, sizeof(*w));

    if (!s || !w) {
        free(s);
        free(w);
        return NULL;
    }
    s->ops = &wl_ops;
    s->priv = w;

    if (wl_ops.open(s, cfg) != 0) {
        free(w);
        free(s);
        return NULL;
    }
    return s;
}

#endif /* NB_HAVE_WAYLAND */
