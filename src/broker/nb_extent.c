/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nb_extent.c — how far into its buffer a frame's description reaches, per
 * DRM format modifier.
 *
 * THE BUG THIS EXISTS FOR.  Until 2026-10-04 the ATTACH validator bounded
 * every frame by the LINEAR formula, offset + stride*height <= size, whatever
 * the modifier said.  A block-linear surface does not end there.  Its rows are
 * stored in BLOCKS -- a column of 2^h GOBs, each GOB 64 bytes by 8 rows -- and
 * a block is the unit of the layout, so a surface occupies whole blocks of
 * rows: 500 rows in 128-row blocks span 512 rows of memory.  The linear bound
 * therefore passed buffers whose last block row ends past the buffer.
 *
 * HOW IT PRESENTED.  kayfabe's box run on vast 54032077 (RTX 3060, host driver
 * and NVIDIA DDX 580.159.04, broker at badf2d7) had a DRI3 client import a
 * real 512x512 block-linear bo, modifier 0x0300000000606014 (16-GOB = 128-row
 * blocks), through the NVIDIA X server under ten malformed descriptions.
 * Three of them are exactly this: 512x510 at offset 4096 and 512x480 at offset
 * 65536 in the 1 MiB bo, and 512x500 in a 1 024 000-byte udmabuf.  Each fits
 * the linear bound TO THE BYTE and ends 4096, 65536 and 24576 bytes past the
 * buffer as block-linear.  Two more (pitch 2112, and offset 4, over 480 rows)
 * fit it with room to spare and still end past the bo as whole blocks, and
 * one (pitch 2052) is not a whole number of GOBs at all.  The X server
 * imported and presented every one with no X error and no Xid; whether the
 * GPU read past the end is not known
 * (kayfabe origin/v3-broker, traces/v3_display/broker_20261004/README.md
 * finding 1 and brkF3/dri3.log).  The X server checks nothing here, so the
 * broker has to.
 *
 * The answer, per modifier:
 *
 *   DRM_FORMAT_MOD_LINEAR (0)      offset + stride * height.
 *   DRM_FORMAT_MOD_INVALID         the same.  "Implicit" means the importer
 *                                  works the layout out; the NVIDIA DDX was
 *                                  MEASURED reading a block-linear bo handed
 *                                  in this way as linear (nb_session_x11.c,
 *                                  x11_attach()), and the broker has no view
 *                                  of an exporter's private layout metadata.
 *                                  The linear extent of the DECLARED stride is
 *                                  the only bound this process can compute.
 *   NVIDIA block-linear            offset + stride * height rounded UP to
 *                                  whole blocks, with stride a whole number of
 *                                  GOBs.  Decoded field by field below.
 *   anything else                  REFUSED.  A layout the broker cannot decode
 *                                  is an extent it cannot bound, and a bound
 *                                  it cannot compute is not a bound.
 *
 * Like nb_cursor.c this file is pure -- no fd, no socket, no log -- so every
 * field and boundary is driven directly by test/test_extent.c, linked against
 * this same object.  nb_validate_desc() in nvkvm_broker.c is the one caller
 * that bounds a frame, and nb_format_usable() asks nb_modifier_layout() too, so
 * that CMD_QUERY_FORMAT never says yes to a modifier every frame in which would
 * then be refused.
 */
#include "nvkvm_broker.h"

/* include/uapi/drm/drm_fourcc.h, spelled locally: this file links into a unit
 * test that must not need libdrm either. */
#define NB_MOD_LINEAR         0x0000000000000000ull  /* DRM_FORMAT_MOD_LINEAR  */
#define NB_MOD_INVALID        0x00ffffffffffffffull  /* DRM_FORMAT_MOD_INVALID */
#define NB_MOD_VENDOR_NONE    0x00u                  /* ..._MOD_VENDOR_NONE    */
#define NB_MOD_VENDOR_NVIDIA  0x03u                  /* ..._MOD_VENDOR_NVIDIA  */
#define NB_MOD_NV_TEGRA_TILED 0x0000000000000001ull  /* value half of
                                                      * DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED */

/*
 * The GOB.  64 bytes wide on every generation: NVKMS_BLOCK_LINEAR_GOB_WIDTH,
 * open-gpu-kernel-modules 580.159.04 src/nvidia-modeset/include/
 * nvkms-types.h:110-111.  8 rows tall from Fermi on (NVKMS_BLOCK_LINEAR_GOB_
 * HEIGHT, :113-114), 4 on G80-GT2XX -- which of the two is the modifier's own
 * field `g`, below.
 */
#define NB_GOB_WIDTH          64u
#define NB_GOB_ROWS_FERMI     8u
#define NB_GOB_ROWS_G80       4u
/*
 * The tallest block: 2^5 = 32 GOBs.  drm_fourcc.h's DRM_FORMAT_MOD_NVIDIA_
 * 16BX2_BLOCK(v) comment: "GOBs are then stacked vertically by a power of 2
 * (1 to 32 GOBs) to form a block ... Valid values are: 0 == ONE_GOB ...
 * 5 == THIRTYTWO_GOBS", and that macro IS BLOCK_LINEAR_2D(0, 0, 0, 0, v).  So a
 * block spans at most 32 * 8 = 256 rows.
 */
#define NB_LOG2_BLOCK_MAX     5u

/*
 * DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h), as Linux 7.1's
 * include/uapi/drm/drm_fourcc.h defines it (the field table at :940-1010, the
 * macro at :1012):
 *
 *    3:0   h  log2(block height in GOBs)
 *    4     -  must be 1: block-linear
 *    8:5   -  reserved (3D block depth), must be zero
 *   11:9   -  reserved (array tile width), must be zero
 *   19:12  k  page kind
 *   21:20  g  GOB height and kind generation: 0 = 8 rows (Fermi-Volta),
 *             1 = 4 rows (G80-GT2XX), 2 = 8 rows (Turing+), 3 = reserved
 *   22     s  sector layout, low bit   } s = bit 22 | bits 27:26 << 1;
 *   27:26  s  sector layout, high bits } 0-3 defined, 4-7 reserved
 *   25:23  c  compression: 0 none, 1-2 ROP/3D, 3-4 CDE, 5-7 reserved
 *   55:28  -  reserved, must be zero
 *
 * Older copies of the header give s one bit and call 55:25 reserved; bits
 * 27:26 were added for GB20x's 8 and 16 bpp layouts.  Accepting them is the
 * newer header's reading and is correct under the older one's too, which
 * simply never produces them.
 *
 * k, s and c change how bytes are arranged INSIDE a GOB (swizzle, sector
 * remap, compression tags held outside the surface), never how many GOBs the
 * surface spans, so any DEFINED value of them leaves the extent alone; only
 * their reserved values are refused, because a reserved value is a layout
 * nobody has defined yet.
 */
#define NB_BL_H_MASK          0x000000000000000full
#define NB_BL_IS_BL           0x0000000000000010ull
#define NB_BL_DEPTH_MASK      0x00000000000001e0ull  /*  8:5  */
#define NB_BL_ARRAY_MASK      0x0000000000000e00ull  /* 11:9  */
#define NB_BL_G_SHIFT         20
#define NB_BL_C_SHIFT         23
#define NB_BL_S_HI_RESERVED   0x0000000008000000ull  /* bit 27: s >= 4 */
#define NB_BL_RESERVED_HI     0x00fffffff0000000ull  /* 55:28 */

int nb_modifier_layout(uint64_t modifier, struct nb_layout *l,
                       const char **why)
{
    uint64_t v = modifier & 0x00ffffffffffffffull;   /* the value half */
    unsigned vendor = (unsigned)(modifier >> 56);
    unsigned h, g, c;

    l->block_linear = false;
    l->gob_width = 1;
    l->block_rows = 1;
    *why = "";

    if (modifier == NB_MOD_LINEAR || modifier == NB_MOD_INVALID) {
        return 0;
    }
    if (vendor == NB_MOD_VENDOR_NONE) {
        *why = "vendor NONE defines only LINEAR (0) and INVALID (the implicit "
               "layout); no other layout exists to bound";
        return -1;
    }
    if (vendor != NB_MOD_VENDOR_NVIDIA) {
        *why = "not an NVIDIA modifier: the broker decodes only LINEAR, the "
               "implicit layout and NVIDIA block-linear, so it cannot bound "
               "what this layout reaches";
        return -1;
    }
    if (!(v & NB_BL_IS_BL)) {
        *why = (v == NB_MOD_NV_TEGRA_TILED)
             ? "NVIDIA TEGRA_TILED (16x16-byte tiles, Tegra 2-4) is not a "
               "layout the broker bounds"
             : "NVIDIA modifier with bit 4 clear names no block-linear layout "
               "(drm_fourcc.h defines only TEGRA_TILED there)";
        return -1;
    }
    /* Reserved fields first, highest bits first: a modifier from a newer
     * header is refused for the field it is newer in. */
    if (v & NB_BL_RESERVED_HI) {
        *why = "NVIDIA block-linear bits 55:28 (reserved) are not zero";
        return -1;
    }
    if (v & NB_BL_S_HI_RESERVED) {
        *why = "NVIDIA block-linear sector layout s (bit 22, bits 27:26) is "
               "4-7, reserved";
        return -1;
    }
    c = (unsigned)((v >> NB_BL_C_SHIFT) & 0x7u);
    if (c > 4) {
        *why = "NVIDIA block-linear compression c (bits 25:23) is 5-7, "
               "reserved";
        return -1;
    }
    g = (unsigned)((v >> NB_BL_G_SHIFT) & 0x3u);
    if (g == 3) {
        *why = "NVIDIA block-linear GOB generation g (bits 21:20) is 3, "
               "reserved";
        return -1;
    }
    if (v & NB_BL_ARRAY_MASK) {
        *why = "NVIDIA block-linear bits 11:9 (reserved: array tile width) are "
               "not zero";
        return -1;
    }
    if (v & NB_BL_DEPTH_MASK) {
        *why = "NVIDIA block-linear bits 8:5 (reserved: 3D block depth) are "
               "not zero";
        return -1;
    }
    h = (unsigned)(v & NB_BL_H_MASK);
    if (h > NB_LOG2_BLOCK_MAX) {
        *why = "NVIDIA block-linear block height h (bits 3:0) is above 5: a "
               "block is 1 to 32 GOBs";
        return -1;
    }
    l->block_linear = true;
    l->gob_width = NB_GOB_WIDTH;
    l->block_rows = (g == 1 ? NB_GOB_ROWS_G80 : NB_GOB_ROWS_FERMI) << h;
    return 0;
}

int nb_frame_extent(uint64_t modifier, uint32_t height, uint32_t stride,
                    uint32_t offset, uint64_t size, uint64_t *need,
                    const char **why)
{
    struct nb_layout l;
    uint64_t rows;

    *need = 0;
    if (nb_modifier_layout(modifier, &l, why) != 0) {
        return NB_EXTENT_LAYOUT;
    }
    /*
     * A BLOCK-LINEAR PITCH IS A WHOLE NUMBER OF GOBs.  NVIDIA's own KMS import
     * refuses anything else for an explicit block-linear layout
     * (src/nvidia-modeset/kapi/src/nvkms-kapi.c:2302-2305, GetSurfaceParams():
     * "Invalid block-linear pitch alignment", then pitch >> 6), because the
     * hardware takes the pitch in GOBs.  The NVIDIA X server took 2052 and
     * 2112 on the box without a word; which way it rounded is unknown, and an
     * extent computed for one rounding is wrong for the other.
     */
    if (stride % l.gob_width != 0) {
        *why = "a block-linear pitch must be a whole number of 64-byte GOBs";
        return NB_EXTENT_PITCH;
    }
    /*
     * WHOLE BLOCKS OF ROWS -- the same bound the upstream kernel applies to
     * this very modifier before it scans a buffer out: Linux 7.1
     * drivers/gpu/drm/nouveau/nouveau_display.c:226-253,
     * nouveau_check_bl_size(), refuses a block-linear framebuffer unless
     * offset + ceil(pitch / 64) * ceil(height / (GOB rows * GOBs per block))
     * * GOBs per block * GOB bytes <= the bo's size (geometry in
     * dispnv50/tile.h).  And it is how a block-linear surface is ALLOCATED:
     * NVIDIA's own (open-gpu-kernel-modules 580.159.04 nvkms-headsurface.c:
     * 91-111, GetLog2GobsPerBlock()) aligns the pitch to the GOB width and the
     * height to GOB rows << log2 h and allocates pitch * lines, and kayfabe's
     * GPU copy does the same (its VramGeom.extent: "whole block rows").  So an
     * honest buffer already holds this much, and one that does not is a
     * surface whose last block row lies outside its buffer.
     *
     * 64 BITS, AND NOTHING CAN WRAP.  block_rows is a power of two <= 256, so
     * it divides 2^32 and a uint32 height rounds up to at most 2^32 rows; the
     * product with a uint32 stride is at most (2^32 - 1) * 2^32, and adding a
     * uint32 offset reaches at most 2^64 - 1.  The rounding is done in 64 bits
     * too: in 32, a height near UINT32_MAX would round up to 0.
     */
    rows = ((uint64_t)height + l.block_rows - 1u) / l.block_rows *
           l.block_rows;
    *need = (uint64_t)stride * rows + (uint64_t)offset;
    /*
     * AND IT MUST FIT THE PROTOCOLS WE HAND IT TO.  AUDIT 2026-08-27 S-3:
     * wl_shm_create_pool takes an int32_t, and a stride of 0x40000 over 8192
     * rows once made the pool size land on INT32_MIN -- a wl_shm protocol
     * error, fatal to the connection, at a moment the client picks.  Bounded
     * here, before the size, so "reject, never clamp" holds in the one place
     * every backend inherits it from.
     */
    if (*need > (uint64_t)INT32_MAX) {
        *why = "the extent is larger than any display protocol here accepts";
        return NB_EXTENT_TOO_BIG;
    }
    if (*need > size) {
        *why = "the extent ends past the end of the buffer";
        return NB_EXTENT_SHORT;
    }
    *why = "";
    return NB_EXTENT_OK;
}
