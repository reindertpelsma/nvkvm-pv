/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_placeholder.c — what the window shows when no VM is attached.
 *
 * WHY THIS EXISTS.  The broker's surface has no content of its own: the
 * client's first ATTACH+COMMIT is what maps it.  Before that the window is a
 * live xdg_toplevel with no buffer — it appears in the dock, and clicking it
 * shows nothing.  That is correct by the protocol and useless to a human:
 * "waiting for a VM" and "the broker crashed" look identical from the outside,
 * which is the first thing anyone running this hits.
 *
 * So the broker paints its own idle frame, once, at startup and again whenever
 * a client goes away.  Deliberately CPU-only into a plain shm buffer: this
 * file has no GL, no GBM and no dma-buf, because the whole point of the broker
 * is that it needs none of those to put a picture up.  The first real guest
 * frame replaces it and nothing here runs again until the guest is gone.
 *
 * The font is five-by-seven, stored as ASCII art rather than as hex columns.
 * It is thirty glyphs used twice; a hex table would be shorter to type and
 * impossible to check by eye, and a wrong bit in it would be a silent
 * cosmetic bug nobody would ever chase.
 */
#include <string.h>

#include "nvkvm_broker.h"

#define GW 5                    /* glyph width, px  */
#define GH 7                    /* glyph height, px */

/* Rows top to bottom, five columns each, concatenated. '#' is ink. */
static const char *nb_glyph(char c);
static const char *nb_glyph(char c)
{
    switch (c) {
    case 'A': return ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#";
    case 'B': return "####." "#...#" "#...#" "####." "#...#" "#...#" "####.";
    case 'C': return ".###." "#...#" "#...." "#...." "#...." "#...#" ".###.";
    case 'D': return "####." "#...#" "#...#" "#...#" "#...#" "#...#" "####.";
    case 'E': return "#####" "#...." "#...." "####." "#...." "#...." "#####";
    case 'F': return "#####" "#...." "#...." "####." "#...." "#...." "#....";
    case 'G': return ".###." "#...#" "#...." "#..##" "#...#" "#...#" ".###.";
    case 'H': return "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#";
    case 'I': return "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "#####";
    case 'J': return "....#" "....#" "....#" "....#" "#...#" "#...#" ".###.";
    case 'K': return "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#";
    case 'L': return "#...." "#...." "#...." "#...." "#...." "#...." "#####";
    case 'M': return "#...#" "##.##" "#.#.#" "#...#" "#...#" "#...#" "#...#";
    case 'N': return "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#" "#...#";
    case 'O': return ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###.";
    case 'P': return "####." "#...#" "#...#" "####." "#...." "#...." "#....";
    case 'Q': return ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#";
    case 'R': return "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#";
    case 'S': return ".####" "#...." "#...." ".###." "....#" "....#" "####.";
    case 'T': return "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#..";
    case 'U': return "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###.";
    case 'V': return "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#..";
    case 'W': return "#...#" "#...#" "#...#" "#...#" "#.#.#" "##.##" "#...#";
    case 'X': return "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#";
    case 'Y': return "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#..";
    case 'Z': return "#####" "....#" "...#." "..#.." ".#..." "#...." "#####";
    case '0': return ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###.";
    case '1': return "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###.";
    case '2': return ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####";
    case '3': return "#####" "...#." "..#.." "...#." "....#" "#...#" ".###.";
    case '4': return "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#.";
    case '5': return "#####" "#...." "####." "....#" "....#" "#...#" ".###.";
    case '6': return "..##." ".#..." "#...." "####." "#...#" "#...#" ".###.";
    case '7': return "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#...";
    case '8': return ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###.";
    case '9': return ".###." "#...#" "#...#" ".####" "....#" "...#." ".##..";
    case '-': return "....." "....." "....." "#####" "....." "....." ".....";
    case '.': return "....." "....." "....." "....." "....." ".##.." ".##..";
    case ':': return "....." ".##.." ".##.." "....." ".##.." ".##.." ".....";
    case '/': return "....#" "....#" "...#." "..#.." ".#..." "#...." "#....";
    case '_': return "....." "....." "....." "....." "....." "....." "#####";
    default:
        if (c >= 'a' && c <= 'z') {
            return nb_glyph((char)(c - 'a' + 'A'));
        }
        return NULL;            /* space, and anything we have no glyph for */
    }
}

static void put(uint32_t *px, unsigned w, unsigned h, unsigned stride_px,
                unsigned x, unsigned y, uint32_t argb)
{
    if (x < w && y < h) {
        px[(size_t)y * stride_px + x] = argb;
    }
}

/* Text width in pixels for a scale, including the one-column inter-glyph gap
 * but not a trailing one. */
static unsigned text_w(const char *s, unsigned scale)
{
    size_t n = strlen(s);

    return n ? (unsigned)(n * (GW + 1) * scale - scale) : 0;
}

static void draw_text(uint32_t *px, unsigned w, unsigned h, unsigned stride_px,
                      unsigned x0, unsigned y0, const char *s, unsigned scale,
                      uint32_t argb)
{
    unsigned pen = x0;

    for (; *s; s++) {
        const char *g = nb_glyph(*s);

        if (g) {
            unsigned gx, gy, sx, sy;

            for (gy = 0; gy < GH; gy++) {
                for (gx = 0; gx < GW; gx++) {
                    if (g[gy * GW + gx] != '#') {
                        continue;
                    }
                    for (sy = 0; sy < scale; sy++) {
                        for (sx = 0; sx < scale; sx++) {
                            put(px, w, h, stride_px,
                                pen + gx * scale + sx,
                                y0 + gy * scale + sy, argb);
                        }
                    }
                }
            }
        }
        pen += (GW + 1) * scale;
    }
}

/*
 * Paint the idle frame into a 32-bit XRGB/ARGB buffer.  `stride_px` is the
 * row stride in PIXELS, not bytes, because every caller has a byte pitch that
 * is a multiple of four and converting once here is one place to get it wrong
 * instead of two.
 */
void nb_placeholder_paint(uint32_t *px, unsigned w, unsigned h,
                          unsigned stride_px, const char *line1,
                          const char *line2)
{
    const uint32_t bg    = 0xff11151aU;
    const uint32_t frame = 0xff2b3947U;
    const uint32_t ink1  = 0xffe6edf3U;
    const uint32_t ink2  = 0xff8b98a5U;
    unsigned x, y, s1 = 1, s2 = 1, tw, th, y1;

    if (!px || !w || !h) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            px[(size_t)y * stride_px + x] = bg;
        }
    }
    /* A two-pixel frame, so an all-black window and a painted one are
     * distinguishable even if the text does not fit. */
    for (x = 0; x < w; x++) {
        put(px, w, h, stride_px, x, 0, frame);
        put(px, w, h, stride_px, x, 1, frame);
        put(px, w, h, stride_px, x, h - 1, frame);
        put(px, w, h, stride_px, x, h - 2, frame);
    }
    for (y = 0; y < h; y++) {
        put(px, w, h, stride_px, 0, y, frame);
        put(px, w, h, stride_px, 1, y, frame);
        put(px, w, h, stride_px, w - 1, y, frame);
        put(px, w, h, stride_px, w - 2, y, frame);
    }

    /* Largest integer scale at which the longer line still uses at most 70%
     * of the width.  Integer only: a scaled bitmap font with a fractional
     * step looks broken, and this is a fallback screen, not typography. */
    if (line1 && *line1) {
        while (s1 < 12 && text_w(line1, s1 + 1) < w * 7 / 10) {
            s1++;
        }
    }
    s2 = s1 > 1 ? s1 - 1 : 1;
    if (line2 && *line2) {
        while (s2 > 1 && text_w(line2, s2) >= w * 7 / 10) {
            s2--;
        }
    }

    th = GH * s1 + (line2 && *line2 ? GH * s2 + 6 * s2 : 0);
    y1 = h > th ? (h - th) / 2 : 0;

    if (line1 && *line1) {
        tw = text_w(line1, s1);
        draw_text(px, w, h, stride_px, w > tw ? (w - tw) / 2 : 0, y1,
                  line1, s1, ink1);
    }
    if (line2 && *line2) {
        tw = text_w(line2, s2);
        draw_text(px, w, h, stride_px, w > tw ? (w - tw) / 2 : 0,
                  y1 + GH * s1 + 6 * s2, line2, s2, ink2);
    }
}


/* ── the same font, for anyone else who needs one line of text ──────────── */
/*
 * The title bar (nb_session_wl.c) needs exactly what this file already has: a
 * fill, a text width and a text blit, with no font library anywhere near the
 * broker.  Exported rather than duplicated.
 */
unsigned nb_placeholder_text_w(const char *s, unsigned scale)
{
    return text_w(s, scale);
}

void nb_placeholder_fill(uint32_t *px, unsigned w, unsigned h,
                         unsigned stride_px, uint32_t argb)
{
    unsigned x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            px[(size_t)y * stride_px + x] = argb;
        }
    }
}

void nb_placeholder_text(uint32_t *px, unsigned w, unsigned h,
                         unsigned stride_px, unsigned x, unsigned y,
                         const char *s, unsigned scale, uint32_t argb)
{
    draw_text(px, w, h, stride_px, x, y, s, scale, argb);
}
