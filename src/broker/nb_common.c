/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_common.c — backend selection, the fourcc table, and the advertised-format
 * set every backend fills and the validator consults.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvkvm_broker.h"

/*
 * Bytes per pixel, for the single-plane 32-bit formats the nvkvm guest head
 * can actually flip.  src/guest/nvkvm_kms.c's nvkvm_pipe_formats[] advertises
 * XRGB8888 and ARGB8888; the BGR twins are the same 4-byte layout and cost
 * nothing to carry.  This mirrors nvkvm_present_bpp() in
 * src/qemu/nvkvm_isolate_handlers.c on purpose — the same default-deny list,
 * one process further out.
 *
 * 0 means REJECT.  A format whose pitch cannot be computed is a format whose
 * bounds cannot be checked, and an unchecked bound here is an out-of-bounds
 * read in the compositor.
 */
#define NB_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

uint32_t nb_fourcc_bpp(uint32_t fourcc)
{
    switch (fourcc) {
    case NB_FOURCC('X', 'R', '2', '4'):   /* DRM_FORMAT_XRGB8888 */
    case NB_FOURCC('A', 'R', '2', '4'):   /* DRM_FORMAT_ARGB8888 */
    case NB_FOURCC('X', 'B', '2', '4'):   /* DRM_FORMAT_XBGR8888 */
    case NB_FOURCC('A', 'B', '2', '4'):   /* DRM_FORMAT_ABGR8888 */
        return 4;
    default:
        return 0;
    }
}

const char *nb_fourcc_name(uint32_t fourcc, char buf[8])
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        unsigned char ch = (unsigned char)((fourcc >> (i * 8)) & 0xff);

        buf[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    buf[4] = '\0';
    return buf;
}

/* ── the advertised (fourcc, modifier) set ───────────────────────────────── */

/*
 * HARDENING 3 lives here: a backend fills this from what the compositor or the
 * X server enumerated, and nb_validate_desc() consults it.  Nothing in the
 * broker carries a hardcoded modifier list — the modifiers nvkvm's guest GBM
 * produces (0x0300000000606014 and 0x0300000000e08014 are the two seen so far,
 * per src/guest/nvkvm_kms.c) are NVIDIA-specific and driver-version-specific,
 * so a hardcoded list would be wrong on the next driver.
 */
void nb_formats_add(struct nb_formats *f, uint32_t fourcc, uint64_t modifier)
{
    unsigned i;

    for (i = 0; i < f->n; i++) {
        if (f->e[i].fourcc == fourcc && f->e[i].modifier == modifier) {
            return;
        }
    }
    if (f->n >= NB_MAX_FORMATS) {
        /*
         * Fixed capacity, and the overflow is REMEMBERED rather than silently
         * dropped: a display that advertises more pairs than we can hold would
         * otherwise turn into "the format you need is mysteriously rejected".
         */
        f->overflowed = true;
        return;
    }
    f->e[f->n].fourcc = fourcc;
    f->e[f->n].modifier = modifier;
    f->n++;
}

bool nb_formats_has(const struct nb_formats *f, uint32_t fourcc, uint64_t mod)
{
    unsigned i;

    for (i = 0; i < f->n; i++) {
        if (f->e[i].fourcc == fourcc && f->e[i].modifier == mod) {
            return true;
        }
    }
    return false;
}

void nb_formats_log(const struct nb_formats *f, const char *what)
{
    unsigned i, shown = 0;

    nb_log("%s advertises %u (format, modifier) pair%s%s", what, f->n,
           f->n == 1 ? "" : "s",
           f->overflowed ? " (TRUNCATED — more than we can hold)" : "");
    if (!nb_verbose) {
        return;
    }
    for (i = 0; i < f->n && shown < 64; i++, shown++) {
        char fcc[8];

        nb_log("    %s  modifier 0x%016llx", nb_fourcc_name(f->e[i].fourcc, fcc),
               (unsigned long long)f->e[i].modifier);
    }
}

/* ── backend selection ───────────────────────────────────────────────────── */

struct nb_session *nb_session_open(const struct nb_config *cfg)
{
    struct nb_session *s;
    const char *want = cfg->backend ? cfg->backend : "auto";
    bool have_wl = getenv("WAYLAND_DISPLAY") != NULL;
    bool have_x11 = getenv("DISPLAY") != NULL;

    if (!strcmp(want, "x11")) {
        return nb_session_x11(cfg);
    }
    if (!strcmp(want, "wayland")) {
        return nb_session_wayland(cfg);
    }
    if (!strcmp(want, "test")) {
        /* Deliberately not reachable from "auto": a backend that drives no
         * display, and that relaxes the dma-buf check, must never be selected
         * by accident. */
        return nb_session_test(cfg);
    }
    if (strcmp(want, "auto") != 0) {
        nb_err("unknown --backend '%s' (auto|wayland|x11|test)", want);
        return NULL;
    }

    /*
     * Wayland first when it is there.  It is not a preference: on Wayland the
     * broker attaches the guest's buffer directly to its wl_surface, so the
     * compositor takes it as the surface's content — one blit fewer than
     * today's GTK/SDL path, and the case where the compositor can promote the
     * frame to a hardware plane (direct scanout) at all.
     */
    if (have_wl) {
        nb_log("auto: WAYLAND_DISPLAY is set — trying the Wayland backend");
        s = nb_session_wayland(cfg);
        if (s) {
            return s;
        }
        nb_log("auto: the Wayland backend did not come up; falling through");
    }
    if (have_x11) {
        nb_log("auto: DISPLAY is set — trying the X11 backend");
        s = nb_session_x11(cfg);
        if (s) {
            return s;
        }
        nb_log("auto: the X11 backend did not come up");
    }

    nb_err("no display backend could open a window.");
    nb_err("  The broker is a SESSION program: it draws on the user's desktop,");
    nb_err("  so it needs WAYLAND_DISPLAY or DISPLAY set and reachable.  It is");
    nb_err("  the VMM that is meant to be sandboxed away from the display");
    nb_err("  server — running the BROKER without one defeats the design.");
    return NULL;
}
