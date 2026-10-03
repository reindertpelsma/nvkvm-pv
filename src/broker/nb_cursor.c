/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_cursor.c — the pure half of CMD_CURSOR: classify a record, bound its
 * extent, copy its pixels, and scale them.
 *
 * Nothing here touches an fd, a socket or a display connection, and nothing
 * logs.  That is the point of the split: every rule a hostile VMM's cursor
 * record has to get past lives in functions test/test_cursor.c can drive
 * through each boundary, and through a few hundred thousand random records,
 * without a broker, a compositor or a GPU.  The fd half -- proving the fd is
 * shmem, measuring it, reading it -- is nb_cmd_cursor() in nvkvm_broker.c, and
 * it calls these for every decision it makes.
 *
 * Every size here is bounded by a compile-time constant before it is used in
 * arithmetic, and every product is formed in 64 bits, so no client value can
 * wrap a bound into a small number.
 */
#include <string.h>

#include "nvkvm_broker.h"

/* DRM_FORMAT_ARGB8888, spelled locally: this file links into a unit test
 * that should not need libdrm either. */
#define NB_CURSOR_FOURCC_AR24 0x34325241u

int nb_cursor_check(const struct nvkvm_broker_cursor_cmd *c, bool has_fd,
                    const char **why)
{
    /*
     * FRAMING FIRST, then content -- the same order the clipboard case learned
     * the hard way: a malformed record is a violation however the image inside
     * it would have been judged, so the verdict must not depend on which
     * content check happens to run first.
     */
    if (c->flags != 0) {
        *why = "CURSOR flags are not zero";
        return NB_CURSOR_VIOLATION;
    }
    if (c->reserved1 != 0) {
        *why = "CURSOR reserved1 is not zero";
        return NB_CURSOR_VIOLATION;
    }
    switch (c->op) {
    case NVKVM_BROKER_CURSOR_HIDE:
    case NVKVM_BROKER_CURSOR_SHOW:
        if (has_fd) {
            *why = "CURSOR HIDE/SHOW carried an fd";
            return NB_CURSOR_VIOLATION;
        }
        if (c->width || c->height || c->stride || c->offset || c->fourcc ||
            c->hot_x || c->hot_y) {
            *why = "CURSOR HIDE/SHOW carried image fields";
            return NB_CURSOR_VIOLATION;
        }
        *why = "";
        return NB_CURSOR_OK;
    case NVKVM_BROKER_CURSOR_SET:
        if (!has_fd) {
            *why = "CURSOR SET without an SCM_RIGHTS fd";
            return NB_CURSOR_VIOLATION;
        }
        break;
    default:
        *why = "CURSOR op is not SET, HIDE or SHOW";
        return NB_CURSOR_VIOLATION;
    }

    /*
     * CONTENT.  Everything from here on describes the guest's image, which the
     * VMM forwards rather than invents, so a bad value is a refused cursor and
     * not a disconnect -- the same rule as a rejected ATTACH.
     */
    if (c->fourcc != NB_CURSOR_FOURCC_AR24) {
        *why = "CURSOR SET fourcc is not ARGB8888 (the only cursor format)";
        return NB_CURSOR_REJECT;
    }
    if (c->width == 0 || c->height == 0 ||
        c->width > NVKVM_BROKER_CURSOR_MAX_DIM ||
        c->height > NVKVM_BROKER_CURSOR_MAX_DIM) {
        *why = "CURSOR SET size is outside 1..256";
        return NB_CURSOR_REJECT;
    }
    /* Checked against the SIZE, which is already known to be <= 256, so
     * neither comparison can be satisfied by a wrapped value. */
    if (c->hot_x >= c->width || c->hot_y >= c->height) {
        *why = "CURSOR SET hot spot is outside the image";
        return NB_CURSOR_REJECT;
    }
    if ((uint64_t)c->stride < (uint64_t)c->width * 4u ||
        c->stride > NVKVM_BROKER_CURSOR_MAX_STRIDE) {
        *why = "CURSOR SET stride is not between width*4 and 1024";
        return NB_CURSOR_REJECT;
    }
    *why = "";
    return NB_CURSOR_OK;
}

uint32_t nb_cursor_span(uint32_t w, uint32_t h, uint32_t stride)
{
    /*
     * The LAST row is only w*4 bytes long -- a cursor packed tightly at the
     * end of its memfd is legitimate, and demanding a full stride for it would
     * refuse an honest client for padding it never needed.  For a record
     * nb_cursor_check() accepted this is at most 1024*255 + 1024.
     */
    uint64_t span;

    if (w == 0 || h == 0) {
        return 0;
    }
    span = (uint64_t)stride * (uint64_t)(h - 1u) + (uint64_t)w * 4u;
    return span > UINT32_MAX ? UINT32_MAX : (uint32_t)span;
}

bool nb_cursor_fits(uint32_t offset, uint32_t span, uint64_t size)
{
    /* Two uint32 operands summed in 64 bits cannot wrap. */
    return (uint64_t)offset + (uint64_t)span <= size;
}

void nb_cursor_load(struct nb_cursor *dst, const uint8_t *src, uint32_t w,
                    uint32_t h, uint32_t stride, uint32_t hot_x,
                    uint32_t hot_y)
{
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *out = dst->px + (size_t)y * w;

        for (x = 0; x < w; x++) {
            /*
             * Bytes, not a uint32 load: B,G,R,A in memory is the DEFINITION
             * of DRM's little-endian ARGB8888, and assembling it explicitly
             * keeps that true on any host and needs no alignment from the
             * client's offset or stride.
             */
            uint32_t b = row[x * 4u + 0], g = row[x * 4u + 1];
            uint32_t r = row[x * 4u + 2], a = row[x * 4u + 3];

            /*
             * PREMULTIPLIED means no colour channel exceeds alpha.  Clamp
             * rather than refuse: an image that breaks it is the sender's
             * colour bug, and handing it on would make what the compositor
             * does with an out-of-range premultiplied value -- additive
             * blending, in practice -- a property of our input validation.
             */
            if (r > a) { r = a; }
            if (g > a) { g = a; }
            if (b > a) { b = a; }
            out[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    dst->w = w;
    dst->h = h;
    dst->hot_x = hot_x;
    dst->hot_y = hot_y;
    dst->defined = true;
    dst->visible = true;
    dst->gen++;
}

void nb_cursor_scaled_geom(const struct nb_cursor *c, uint32_t num_x,
                           uint32_t den_x, uint32_t num_y, uint32_t den_y,
                           uint32_t max, struct nb_cursor_geom *out)
{
    uint64_t w = c->w ? c->w : 1u, h = c->h ? c->h : 1u;
    uint64_t dw, dh;

    if (num_x == 0 || den_x == 0) {
        num_x = den_x = 1;
    }
    if (num_y == 0 || den_y == 0) {
        num_y = den_y = 1;
    }
    if (max == 0) {
        max = 1;
    }
    /* w <= 256 and num <= 2^32-1: the product is < 2^41. */
    dw = (w * num_x + den_x / 2u) / den_x;
    dh = (h * num_y + den_y / 2u) / den_y;
    if (dw < 1) { dw = 1; }
    if (dh < 1) { dh = 1; }
    /*
     * Past the bound, shrink BOTH edges by the same factor, so a clamped
     * cursor is a smaller cursor rather than a distorted one.
     */
    if (dw > max || dh > max) {
        if (dw >= dh) {
            dh = dh * max / dw;
            dw = max;
        } else {
            dw = dw * max / dh;
            dh = max;
        }
        if (dw < 1) { dw = 1; }
        if (dh < 1) { dh = 1; }
    }
    out->w = (uint32_t)dw;
    out->h = (uint32_t)dh;
    /*
     * hot < w  ⇒  hot*dw/w < dw, so the scaled hot spot is inside the scaled
     * image by construction -- and the min() below makes that true even for a
     * nb_cursor nobody loaded through nb_cursor_load().
     */
    out->hot_x = (uint32_t)((uint64_t)c->hot_x * dw / w);
    out->hot_y = (uint32_t)((uint64_t)c->hot_y * dh / h);
    if (out->hot_x >= out->w) { out->hot_x = out->w - 1u; }
    if (out->hot_y >= out->h) { out->hot_y = out->h - 1u; }
}

void nb_cursor_scale(const struct nb_cursor *c, uint32_t *dst, uint32_t dw,
                     uint32_t dh)
{
    uint32_t x, y;

    if (!c->defined || c->w == 0 || c->h == 0 || dw == 0 || dh == 0) {
        return;
    }
    for (y = 0; y < dh; y++) {
        /* y < dh  ⇒  y*h/dh < h: every source index is in the image. */
        uint32_t sy = (uint32_t)((uint64_t)y * c->h / dh);
        const uint32_t *srow = c->px + (size_t)sy * c->w;
        uint32_t *drow = dst + (size_t)y * dw;

        for (x = 0; x < dw; x++) {
            drow[x] = srow[(uint64_t)x * c->w / dw];
        }
    }
}

bool nb_cursor_wanted(const struct nb_cursor *c, bool grabbed)
{
    return c && c->defined && c->visible && !grabbed;
}

uint32_t nb_cursor_hash(const struct nb_cursor *c)
{
    uint32_t h = 2166136261u;
    size_t i, n;

    if (!c->defined) {
        return 0;
    }
    n = (size_t)c->w * c->h;
    for (i = 0; i < n; i++) {
        uint32_t v = c->px[i];
        int k;

        for (k = 0; k < 4; k++) {
            h ^= (v >> (8 * k)) & 0xffu;
            h *= 16777619u;
        }
    }
    return h;
}
