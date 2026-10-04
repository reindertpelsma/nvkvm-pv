/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * test_extent.c — the frame-extent rule, per DRM format modifier, driven
 * directly.  No socket, no display, no GPU.
 *
 * Linked against the SAME nb_extent.c the broker links (see the Makefile), so
 * what passes here is the code that runs.  `make check-sanitize` builds it
 * under ASAN+UBSAN as well.
 *
 * Four kinds of check:
 *   - the decoder, field by field: every block height 0..5 under every GOB
 *     generation, every reserved bit and reserved value refused BY NAME, the
 *     modifiers real NVIDIA drivers produce accepted, every other vendor
 *     refused;
 *   - the extent, with the rows NOT a multiple of the block, and an offset;
 *   - every boundary from both sides: extent == size and size - 1, extent ==
 *     INT32_MAX and INT32_MAX + 1, a pitch one byte off whole GOBs;
 *   - inputs chosen so that a 32-bit product, or a 32-bit rounding, would
 *     wrap to a small number -- the result must be the exact 64-bit extent.
 *   And the three descriptions the NVIDIA X server presented on the box run of
 *   2026-10-04 (kayfabe traces/v3_display/broker_20261004/brkF3/dri3.log):
 *   each must now be refused, and its unmodified twin accepted.
 *
 * Every expected extent is written out as a number or recomputed here by a
 * DIFFERENT method (a per-block loop) from the one nb_extent.c uses, so a
 * mutation of the formula cannot also move the expectation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nvkvm_broker.h"

static unsigned n_checks, n_fail;

#define CHECK(cond, ...) do {                                           \
        n_checks++;                                                     \
        if (!(cond)) {                                                  \
            n_fail++;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

#define MOD_LINEAR   0x0000000000000000ull
#define MOD_INVALID  0x00ffffffffffffffull

/*
 * DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h), transcribed from
 * Linux 7.1 include/uapi/drm/drm_fourcc.h:1012 -- including s's two high bits
 * at 27:26 -- so the test builds modifiers the way a driver does, not the way
 * nb_extent.c takes them apart.
 */
static uint64_t bl2d(unsigned c, unsigned s, unsigned g, unsigned k,
                     unsigned h)
{
    return (0x03ull << 56) |
           (0x10ull |
            ((uint64_t)h & 0xf) |
            (((uint64_t)k & 0xff) << 12) |
            (((uint64_t)g & 0x3) << 20) |
            (((uint64_t)s & 0x1) << 22) |
            (((uint64_t)s & 0x6) << 25) |
            (((uint64_t)c & 0x7) << 23));
}

/* The modifier the NVIDIA DDX and kayfabe's GPU copy use (Turing+ kinds,
 * generic kind 0x06, 16-GOB blocks), and src/guest/nvkvm_kms.c's other one
 * (compression type 1, kind 0x08). */
#define MOD_NV_BL16   0x0300000000606014ull
#define MOD_NV_BL16C  0x0300000000e08014ull

static int layout_of(uint64_t m, struct nb_layout *l, const char **why)
{
    int r;

    *why = NULL;
    r = nb_modifier_layout(m, l, why);
    /* Every refusal names its rule, and an acceptance names none. */
    CHECK(*why != NULL, "nb_modifier_layout left *why unset for 0x%016llx",
          (unsigned long long)m);
    if (*why) {
        CHECK((r == 0) == ((*why)[0] == '\0'),
              "0x%016llx: verdict %d with reason '%s'",
              (unsigned long long)m, r, *why);
    }
    return r;
}

/* Refused, and the reason contains `field` -- the field's name or range. */
static void refused_by(uint64_t m, const char *field, const char *what)
{
    struct nb_layout l;
    const char *why;
    int r = layout_of(m, &l, &why);

    CHECK(r != 0, "%s (0x%016llx) is refused", what, (unsigned long long)m);
    CHECK(r == 0 || (why && strstr(why, field) != NULL),
          "%s (0x%016llx): the reason '%s' names '%s'", what,
          (unsigned long long)m, why ? why : "(null)", field);
}

static int extent(uint64_t m, uint32_t h, uint32_t stride, uint32_t offset,
                  uint64_t size, uint64_t *need)
{
    const char *why = NULL;
    int v;

    *need = 0xdeadbeefdeadbeefull;
    v = nb_frame_extent(m, h, stride, offset, size, need, &why);
    CHECK(why != NULL, "nb_frame_extent left *why unset");
    if (why) {
        CHECK((v == NB_EXTENT_OK) == (why[0] == '\0'),
              "extent verdict %d with reason '%s'", v, why);
    }
    return v;
}

/*
 * The extent recomputed by WALKING the blocks: one block row at a time, each
 * pitch/64 blocks of 64 * block_rows bytes, until the rows are covered.  This
 * is the layout's definition (a surface is a grid of whole blocks) rather than
 * nb_extent.c's closed form.  Only for small heights.
 */
static uint64_t walk_extent(uint32_t height, uint32_t stride, uint32_t offset,
                            uint32_t block_rows)
{
    uint64_t bytes = offset;
    uint32_t covered = 0;

    while (covered < height) {
        bytes += (uint64_t)(stride / 64u) * 64u * block_rows;
        covered += block_rows;
    }
    return bytes;
}

static void test_decode_linear_and_implicit(void)
{
    struct nb_layout l;
    const char *why;

    CHECK(layout_of(MOD_LINEAR, &l, &why) == 0 && !l.block_linear &&
          l.gob_width == 1 && l.block_rows == 1, "LINEAR is linear");
    CHECK(layout_of(MOD_INVALID, &l, &why) == 0 && !l.block_linear &&
          l.gob_width == 1 && l.block_rows == 1,
          "INVALID (the implicit layout) is bounded as linear");
}

static void test_decode_block_heights(void)
{
    /* g = 0 and 2: 8-row GOBs; g = 1: 4-row GOBs (drm_fourcc.h 21:20). */
    static const struct { unsigned g, gob_rows; } gen[] = {
        { 0, 8 }, { 1, 4 }, { 2, 8 },
    };
    struct nb_layout l;
    const char *why;
    unsigned gi, h;

    for (gi = 0; gi < 3; gi++) {
        for (h = 0; h <= 5; h++) {
            uint64_t m = bl2d(0, 1, gen[gi].g, 0x06, h);

            CHECK(layout_of(m, &l, &why) == 0,
                  "g=%u h=%u is accepted (%s)", gen[gi].g, h, why);
            CHECK(l.block_linear && l.gob_width == 64,
                  "g=%u h=%u: block-linear, 64-byte GOBs", gen[gi].g, h);
            CHECK(l.block_rows == gen[gi].gob_rows * (1u << h),
                  "g=%u h=%u: %u-row blocks, got %u", gen[gi].g, h,
                  gen[gi].gob_rows * (1u << h), l.block_rows);
        }
        for (h = 6; h <= 15; h++) {
            refused_by(bl2d(0, 1, gen[gi].g, 0x06, h), "3:0",
                       "a block taller than 32 GOBs");
        }
    }
    /* The tallest block: 32 Fermi+ GOBs = 256 rows. */
    CHECK(layout_of(bl2d(0, 1, 2, 0x06, 5), &l, &why) == 0 &&
          l.block_rows == 256, "h=5: 256 rows");
}

static void test_decode_real_modifiers(void)
{
    struct nb_layout l;
    const char *why;
    unsigned k, c, s, v;

    CHECK(bl2d(0, 1, 2, 0x06, 4) == MOD_NV_BL16,
          "the transcribed macro reproduces 0x0300000000606014");
    CHECK(bl2d(1, 1, 2, 0x08, 4) == MOD_NV_BL16C,
          "and 0x0300000000e08014");
    CHECK(layout_of(MOD_NV_BL16, &l, &why) == 0 && l.block_rows == 128,
          "0x0300000000606014: 128-row blocks");
    CHECK(layout_of(MOD_NV_BL16C, &l, &why) == 0 && l.block_rows == 128,
          "0x0300000000e08014 (compressed): 128-row blocks");
    CHECK(layout_of(0x0300000000606010ull, &l, &why) == 0 &&
          l.block_rows == 8, "0x0300000000606010 (the box's bh0): 8 rows");
    /* DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(v) = BLOCK_LINEAR_2D(0,0,0,0,v),
     * the legacy Tegra K1+ spelling with page kind 0. */
    for (v = 0; v <= 5; v++) {
        CHECK(layout_of(0x0300000000000010ull | v, &l, &why) == 0 &&
              l.block_rows == (8u << v), "16BX2_BLOCK(%u)", v);
    }
    /* Every page kind: k arranges bits inside a GOB, never the GOB count. */
    for (k = 0; k < 256; k++) {
        CHECK(layout_of(bl2d(0, 1, 2, k, 4), &l, &why) == 0 &&
              l.block_rows == 128, "page kind 0x%02x", k);
    }
    /* Every DEFINED compression type and sector layout. */
    for (c = 0; c <= 4; c++) {
        CHECK(layout_of(bl2d(c, 1, 2, 0x06, 4), &l, &why) == 0,
              "compression c=%u is defined", c);
    }
    for (s = 0; s <= 3; s++) {
        CHECK(layout_of(bl2d(0, s, 2, 0x06, 4), &l, &why) == 0,
              "sector layout s=%u is defined", s);
    }
}

static void test_decode_reserved_fields(void)
{
    unsigned b;

    for (b = 5; b <= 8; b++) {
        refused_by(MOD_NV_BL16 | (1ull << b), "8:5", "a 3D block depth bit");
    }
    for (b = 9; b <= 11; b++) {
        refused_by(MOD_NV_BL16 | (1ull << b), "11:9", "an array-stride bit");
    }
    refused_by(bl2d(0, 1, 3, 0x06, 4), "21:20", "GOB generation g=3");
    for (b = 5; b <= 7; b++) {
        refused_by(bl2d(b, 1, 2, 0x06, 4), "25:23", "a reserved compression");
    }
    for (b = 4; b <= 7; b++) {
        refused_by(bl2d(0, b, 2, 0x06, 4), "27:26", "a reserved sector layout");
    }
    for (b = 28; b <= 55; b++) {
        refused_by(MOD_NV_BL16 | (1ull << b), "55:28", "a reserved high bit");
    }
}

static void test_decode_other_layouts(void)
{
    unsigned vendor;

    refused_by(0x0300000000000001ull, "TEGRA_TILED", "NVIDIA TEGRA_TILED");
    refused_by(0x0300000000000000ull, "bit 4", "NVIDIA value 0");
    refused_by(0x0300000000606004ull, "bit 4",
               "NVIDIA with every field but bit 4");
    /* Vendor NONE has LINEAR and INVALID and nothing else -- including the
     * test backend's old refused-pair value. */
    refused_by(0x0000000000c0ffeeull, "NONE", "vendor NONE 0xc0ffee");
    refused_by(0x0000000000000001ull, "NONE", "vendor NONE 1");
    refused_by(0x00fffffffffffffeull, "NONE", "INVALID - 1");
    /* Every other vendor, with the NVIDIA block-linear value half (so only
     * the vendor byte can be the reason), and Intel's real Y-tiling. */
    for (vendor = 1; vendor < 256; vendor++) {
        if (vendor == 0x03) {
            continue;
        }
        refused_by(((uint64_t)vendor << 56) | 0x0000000000606014ull,
                   "not an NVIDIA modifier", "a non-NVIDIA vendor");
    }
    refused_by(0x0100000000000002ull, "not an NVIDIA modifier",
               "I915_FORMAT_MOD_Y_TILED");
}

static void test_extent_linear(void)
{
    uint64_t need;

    /* The existing rule, unchanged: offset + stride*height, the whole stride
     * of the last row included. */
    CHECK(extent(MOD_LINEAR, 240, 1280, 0, 1280 * 240, &need) ==
          NB_EXTENT_OK && need == 307200, "320x240 XR24 in exactly its size");
    CHECK(extent(MOD_LINEAR, 240, 1280, 0, 1280 * 240 - 1, &need) ==
          NB_EXTENT_SHORT && need == 307200, "one byte short");
    CHECK(extent(MOD_LINEAR, 240, 1280, 100, 307300, &need) ==
          NB_EXTENT_OK && need == 307300, "with an offset, exactly");
    CHECK(extent(MOD_LINEAR, 240, 1280, 100, 307299, &need) ==
          NB_EXTENT_SHORT, "with an offset, one byte short");
    /* Linear has no GOB: any pitch. */
    CHECK(extent(MOD_LINEAR, 7, 2052, 0, 2052 * 7, &need) == NB_EXTENT_OK,
          "a linear pitch need not be whole GOBs");
    CHECK(extent(MOD_INVALID, 7, 2052, 0, 2052 * 7, &need) == NB_EXTENT_OK &&
          need == 2052u * 7u, "nor an implicit one");
    CHECK(extent(MOD_INVALID, 500, 2048, 0, 1024000, &need) == NB_EXTENT_OK,
          "implicit 512x500 in 1 024 000 bytes: bounded as linear");
}

static void test_extent_block_linear(void)
{
    static const unsigned heights[] = {
        1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
        255, 256, 257, 480, 500, 510, 511, 512, 695, 768, 900, 1080, 2160,
    };
    struct nb_layout l;
    const char *why;
    unsigned gi, h, hi;
    uint64_t need, want;

    /* Every block height under every GOB generation, at every height above:
     * exact fit accepted, one byte less refused, and the extent equal to the
     * block walk. */
    for (gi = 0; gi < 3; gi++) {
        for (h = 0; h <= 5; h++) {
            uint64_t m = bl2d(0, 1, gi, 0x06, h);

            if (layout_of(m, &l, &why) != 0) {
                CHECK(0, "g=%u h=%u did not decode", gi, h);
                continue;
            }
            for (hi = 0; hi < sizeof heights / sizeof heights[0]; hi++) {
                uint32_t rows = heights[hi];

                want = walk_extent(rows, 4096, 0, l.block_rows);
                CHECK(extent(m, rows, 4096, 0, want, &need) == NB_EXTENT_OK &&
                      need == want, "g=%u h=%u %u rows: extent %llu, got %llu",
                      gi, h, rows, (unsigned long long)want,
                      (unsigned long long)need);
                CHECK(extent(m, rows, 4096, 0, want - 1, &need) ==
                      NB_EXTENT_SHORT, "g=%u h=%u %u rows: one byte short",
                      gi, h, rows);
                want = walk_extent(rows, 4096, 4096, l.block_rows);
                CHECK(extent(m, rows, 4096, 4096, want, &need) ==
                      NB_EXTENT_OK && need == want,
                      "g=%u h=%u %u rows at offset 4096", gi, h, rows);
                CHECK(extent(m, rows, 4096, 4096, want - 1, &need) ==
                      NB_EXTENT_SHORT,
                      "g=%u h=%u %u rows at offset 4096, one byte short",
                      gi, h, rows);
            }
        }
    }

    /* Spelled-out values, for the arithmetic a walk could share a bug with. */
    CHECK(extent(MOD_NV_BL16, 1, 2048, 0, 1u << 20, &need) == NB_EXTENT_OK &&
          need == 2048u * 128u, "one row of 128-row blocks occupies 128");
    CHECK(extent(MOD_NV_BL16, 129, 2048, 0, 1u << 20, &need) ==
          NB_EXTENT_OK && need == 2048u * 256u, "129 rows occupy 256");
    CHECK(extent(MOD_NV_BL16, 695, 4096, 0, 4096u * 768u, &need) ==
          NB_EXTENT_OK && need == 4096u * 768u,
          "kayfabe's 1024x695 guest: 768 rows, 3 MiB, exactly");
    CHECK(extent(MOD_NV_BL16, 900, 6400, 0, 6400u * 1024u, &need) ==
          NB_EXTENT_OK && need == 6400u * 1024u,
          "the 1600x900 guest scanout: 1024 rows");
    CHECK(extent(MOD_NV_BL16, 900, 6400, 0, 6400u * 900u, &need) ==
          NB_EXTENT_SHORT, "and NOT in its 900 linear rows");
    CHECK(extent(bl2d(0, 1, 1, 0x06, 0), 5, 256, 0, 256u * 8u, &need) ==
          NB_EXTENT_OK && need == 256u * 8u,
          "G80 4-row GOBs, h=0: 5 rows occupy 8");
    CHECK(extent(bl2d(0, 1, 1, 0x06, 0), 4, 256, 0, 256u * 4u, &need) ==
          NB_EXTENT_OK && need == 256u * 4u, "and 4 rows occupy 4");
}

static void test_extent_pitch(void)
{
    uint64_t need;
    uint32_t pitch;

    for (pitch = 2048 - 64; pitch <= 2048 + 64; pitch++) {
        int v = extent(MOD_NV_BL16, 512, pitch, 0, 1u << 24, &need);

        if (pitch % 64 == 0) {
            CHECK(v == NB_EXTENT_OK, "pitch %u is whole GOBs", pitch);
        } else {
            CHECK(v == NB_EXTENT_PITCH,
                  "pitch %u is not whole GOBs: refused, got %d", pitch, v);
        }
    }
    /* The pitch is refused before the extent is formed. */
    CHECK(extent(MOD_NV_BL16, 480, 2052, 0, 1u << 20, &need) ==
          NB_EXTENT_PITCH && need == 0, "a PITCH verdict computes no extent");
}

static void test_extent_box_variants(void)
{
    /* brkF3/dri3.log: the source bo, 512x512 stride 2048, 1 048 576 bytes,
     * modifier 0x0300000000606014; each line is decl=WxH/stride/offset/mod. */
    uint64_t need;

    CHECK(extent(MOD_NV_BL16, 512, 2048, 0, 1048576, &need) == NB_EXTENT_OK &&
          need == 1048576, "own: 512x512 fills the bo exactly");
    CHECK(extent(MOD_NV_BL16, 510, 2048, 4096, 1048576, &need) ==
          NB_EXTENT_SHORT && need == 1052672,
          "tail4k: 510 rows at 4096 end 4096 bytes past the bo");
    CHECK(extent(MOD_LINEAR, 510, 2048, 4096, 1048576, &need) ==
          NB_EXTENT_OK && need == 1048576,
          "which the linear bound passed to the byte");
    CHECK(extent(MOD_NV_BL16, 480, 2048, 65536, 1048576, &need) ==
          NB_EXTENT_SHORT && need == 1114112,
          "tail64k: 480 rows at 65536 end 65536 bytes past the bo");
    CHECK(extent(MOD_LINEAR, 480, 2048, 65536, 1048576, &need) ==
          NB_EXTENT_OK && need == 1048576, "linear: to the byte");
    CHECK(extent(MOD_NV_BL16, 500, 2048, 0, 1024000, &need) ==
          NB_EXTENT_SHORT && need == 1048576,
          "udmabuf_short: 500 rows in 1 024 000 bytes");
    CHECK(extent(MOD_LINEAR, 500, 2048, 0, 1024000, &need) ==
          NB_EXTENT_OK && need == 1024000, "linear: to the byte");
    CHECK(extent(0x0300000000606010ull, 512, 2048, 0, 1048576, &need) ==
          NB_EXTENT_OK, "bh0: one-GOB blocks, 512 rows, in bounds");
    /* The other three malformed descriptions the X server took.  pitch+4 is
     * not whole GOBs; pitch+64 and offset+4 each fit the linear bound and
     * end past the bo as whole blocks (480 rows occupy 512). */
    CHECK(extent(MOD_NV_BL16, 480, 2052, 0, 1048576, &need) ==
          NB_EXTENT_PITCH, "pitch+4: 2052 is not whole GOBs");
    CHECK(extent(MOD_NV_BL16, 480, 2112, 0, 1048576, &need) ==
          NB_EXTENT_SHORT && need == 2112u * 512u,
          "pitch+64: 2112 * 512 = 1 081 344 > 1 MiB");
    CHECK(extent(MOD_LINEAR, 480, 2112, 0, 1048576, &need) ==
          NB_EXTENT_OK && need == 2112u * 480u, "linear: 1 013 760 fit");
    CHECK(extent(MOD_NV_BL16, 480, 2048, 4, 1048576, &need) ==
          NB_EXTENT_SHORT && need == 2048u * 512u + 4u,
          "offset+4: 512 rows + 4 bytes");
    CHECK(extent(MOD_LINEAR, 480, 2048, 4, 1048576, &need) ==
          NB_EXTENT_OK, "linear: fit");
    /* And the ones that describe the same 1 MiB in-bounds -- a different
     * kind, a different block height, a full-size udmabuf.  Wrong layouts
     * show garbage; they do not read out of bounds, and the broker cannot
     * tell a wrong in-bounds layout from a right one. */
    CHECK(extent(MOD_NV_BL16C, 512, 2048, 0, 1048576, &need) ==
          NB_EXTENT_OK, "kind (0x...e08014): in bounds");
}

static void test_extent_protocol_bound(void)
{
    uint64_t need;

    /* 65536 * 32767 + 65535 = INT32_MAX exactly. */
    CHECK(extent(MOD_LINEAR, 32767, 65536, 65535, UINT64_MAX, &need) ==
          NB_EXTENT_OK && need == 2147483647ull, "extent == INT32_MAX");
    CHECK(extent(MOD_LINEAR, 32767, 65536, 65536, UINT64_MAX, &need) ==
          NB_EXTENT_TOO_BIG && need == 2147483648ull,
          "extent == INT32_MAX + 1");
    /* The same boundary reached only through the block rounding: 32640 rows
     * of 128-row blocks round to 32768, so 65536 * 32768 = 2^31. */
    CHECK(extent(MOD_NV_BL16, 32640, 65536, 0, UINT64_MAX, &need) ==
          NB_EXTENT_OK && need == 65536ull * 32640ull,
          "32640 rows are whole blocks: 65536 * 32640");
    CHECK(extent(MOD_NV_BL16, 32641, 65536, 0, UINT64_MAX, &need) ==
          NB_EXTENT_TOO_BIG && need == 2147483648ull,
          "one row more rounds up to 2^31: too big");
    CHECK(extent(MOD_LINEAR, 32641, 65536, 0, UINT64_MAX, &need) ==
          NB_EXTENT_OK, "which as linear is not");
    /* TOO_BIG wins over SHORT: the protocol bound is checked first. */
    CHECK(extent(MOD_LINEAR, 32768, 65536, 0, 4096, &need) ==
          NB_EXTENT_TOO_BIG, "too big AND short: too big");
}

static void test_extent_overflow(void)
{
    uint64_t need;

    /*
     * Values that WRAP if anything is computed in 32 bits.  65536 * 65536 is
     * 2^32 (0 in 32 bits); UINT32_MAX rounded up to 256 rows is 2^32 (0 in 32
     * bits, which would make the extent the bare offset).  Every one must
     * come back as the exact 64-bit extent, and too big.
     */
    CHECK(extent(MOD_LINEAR, 65536, 65536, 0, UINT64_MAX, &need) ==
          NB_EXTENT_TOO_BIG && need == 4294967296ull,
          "65536 * 65536 = 2^32, not 0");
    CHECK(extent(MOD_NV_BL16, 65536, 65536, 0, UINT64_MAX, &need) ==
          NB_EXTENT_TOO_BIG && need == 4294967296ull,
          "block-linear 65536 * 65536 = 2^32");
    CHECK(extent(bl2d(0, 1, 2, 0x06, 5), UINT32_MAX, 64, 0, UINT64_MAX,
                 &need) == NB_EXTENT_TOO_BIG && need == 64ull << 32,
          "UINT32_MAX rows in 256-row blocks round to 2^32 rows, not 0");
    CHECK(extent(bl2d(0, 1, 2, 0x06, 5), UINT32_MAX - 254, 64, 0, UINT64_MAX,
                 &need) == NB_EXTENT_TOO_BIG && need == 64ull << 32,
          "0xffffff01 rows round up to 2^32 too");
    CHECK(extent(bl2d(0, 1, 2, 0x06, 5), UINT32_MAX - 255, 64, 0, UINT64_MAX,
                 &need) == NB_EXTENT_TOO_BIG &&
          need == 64ull * 0xffffff00ull,
          "0xffffff00 rows are already whole blocks: not rounded");
    /* The largest inputs: the proof that nothing wraps is that these are the
     * exact values, at most 2^64 - 1. */
    CHECK(extent(MOD_LINEAR, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT64_MAX,
                 &need) == NB_EXTENT_TOO_BIG &&
          need == 0xffffffff00000000ull,
          "linear maximum: (2^32-1)^2 + 2^32-1 = 0xffffffff00000000");
    CHECK(extent(bl2d(0, 1, 2, 0x06, 5), UINT32_MAX, 0xffffffc0u, UINT32_MAX,
                 UINT64_MAX, &need) == NB_EXTENT_TOO_BIG &&
          need == 0xffffffc0ffffffffull,
          "block-linear maximum: 0xffffffc0 * 2^32 + 0xffffffff");
    CHECK(extent(MOD_LINEAR, 1, 0, UINT32_MAX, UINT64_MAX, &need) ==
          NB_EXTENT_TOO_BIG && need == UINT32_MAX, "an offset alone");
}

static void test_extent_layout(void)
{
    uint64_t need;

    CHECK(extent(0x0100000000000002ull, 64, 256, 0, UINT64_MAX, &need) ==
          NB_EXTENT_LAYOUT && need == 0, "an Intel modifier: no extent");
    CHECK(extent(MOD_NV_BL16 | (1ull << 5), 64, 256, 0, UINT64_MAX, &need) ==
          NB_EXTENT_LAYOUT && need == 0, "a reserved NVIDIA field: no extent");
}

/*
 * A deterministic sweep over random descriptions: the verdict and the extent
 * must agree with the block walk and with the bounds, for every decodable
 * modifier built from random fields.
 */
static void test_sweep(void)
{
    uint64_t x = 0x9e3779b97f4a7c15ull;
    unsigned i;

    for (i = 0; i < 200000; i++) {
        struct nb_layout l;
        const char *why;
        uint64_t m, need, want, size;
        uint32_t height, stride, offset;
        int v;

        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        m = (x & 1) ? MOD_LINEAR
                    : bl2d((unsigned)(x >> 8) % 5, (unsigned)(x >> 12) % 4,
                           (unsigned)(x >> 16) % 3, (unsigned)(x >> 20),
                           (unsigned)(x >> 28) % 6);
        height = 1 + (uint32_t)((x >> 32) % 2200);
        stride = 64 * (1 + (uint32_t)((x >> 44) % 200)) +
                 (((x >> 2) & 7) == 0 ? (uint32_t)((x >> 52) % 64) : 0);
        offset = ((x >> 3) & 3) == 0 ? (uint32_t)((x >> 40) % 70000) : 0;
        if (layout_of(m, &l, &why) != 0) {
            CHECK(0, "sweep: a constructed modifier did not decode");
            continue;
        }
        want = walk_extent(height, stride, offset, l.block_rows);
        if (!l.block_linear) {
            want = (uint64_t)stride * height + offset;
        }
        size = want - (x >> 5) % 3 + (x >> 6) % 3;   /* want-2 .. want+2 */
        v = extent(m, height, stride, offset, size, &need);
        if (l.block_linear && stride % 64 != 0) {
            CHECK(v == NB_EXTENT_PITCH, "sweep %u: unaligned pitch", i);
        } else if (want > 2147483647ull) {
            CHECK(v == NB_EXTENT_TOO_BIG && need == want, "sweep %u: big", i);
        } else if (want > size) {
            CHECK(v == NB_EXTENT_SHORT && need == want, "sweep %u: short", i);
        } else {
            CHECK(v == NB_EXTENT_OK && need == want,
                  "sweep %u: m=0x%016llx %u rows stride %u offset %u: want "
                  "%llu got %llu (v=%d)", i, (unsigned long long)m, height,
                  stride, offset, (unsigned long long)want,
                  (unsigned long long)need, v);
        }
    }
}

int main(void)
{
    test_decode_linear_and_implicit();
    test_decode_block_heights();
    test_decode_real_modifiers();
    test_decode_reserved_fields();
    test_decode_other_layouts();
    test_extent_linear();
    test_extent_block_linear();
    test_extent_pitch();
    test_extent_box_variants();
    test_extent_protocol_bound();
    test_extent_overflow();
    test_extent_layout();
    test_sweep();

    if (n_fail) {
        fprintf(stderr, "test_extent: %u of %u checks FAILED\n", n_fail,
                n_checks);
        return 1;
    }
    printf("test_extent: all %u checks passed\n", n_checks);
    return 0;
}
