/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * test_cursor.c — the CMD_CURSOR record's validator, extent arithmetic, pixel
 * copy and scaler, driven directly.  No socket, no display, no GPU.
 *
 * Linked against the SAME nb_cursor.c the broker links (see the Makefile), so
 * what passes here is the code that runs, not a copy of it.  `make
 * check-sanitize` builds this under ASAN+UBSAN too, which is what turns the
 * fuzz loop's out-of-range reads and writes into failures instead of luck.
 *
 * Three kinds of check:
 *   - every boundary of every rule, from both sides (the value that passes and
 *     the first one that does not);
 *   - arithmetic chosen to wrap 32 bits if anything were computed in 32 bits;
 *   - a deterministic random sweep asserting the invariants the backends rely
 *     on: an accepted record's extent is inside the protocol bounds, a scaled
 *     cursor fits a NB_CURSOR_SCALED_MAX slot, and its hot spot is inside it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nvkvm_broker.h"

#define AR24 0x34325241u
#define XR24 0x34325258u

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

static struct nvkvm_broker_cursor_cmd base_set(void)
{
    struct nvkvm_broker_cursor_cmd c;

    memset(&c, 0, sizeof c);
    c.type = NVKVM_BROKER_CMD_CURSOR;
    c.op = NVKVM_BROKER_CURSOR_SET;
    c.width = 32;
    c.height = 32;
    c.stride = 128;
    c.fourcc = AR24;
    c.hot_x = 3;
    c.hot_y = 4;
    return c;
}

static int verdict(const struct nvkvm_broker_cursor_cmd *c, bool fd)
{
    const char *why = NULL;
    int v = nb_cursor_check(c, fd, &why);

    /* Every verdict names its rule, and an OK names none. */
    CHECK(why != NULL, "nb_cursor_check left *why unset");
    if (why) {
        CHECK((v == NB_CURSOR_OK) == (why[0] == '\0'),
              "verdict %d with reason '%s'", v, why);
    }
    return v;
}

static void test_framing(void)
{
    struct nvkvm_broker_cursor_cmd c;
    uint32_t ops[] = { 0, 4, 5, 0x80000001u, 0xffffffffu };
    unsigned i, f;

    c = base_set();
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "the base SET is accepted");

    c = base_set(); c.flags = 1;
    CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "flags bit 0 (F_SHM)");
    c = base_set(); c.flags = 0x8000;
    CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "flags bit 15");
    c = base_set(); c.reserved1 = 1;
    CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "reserved1");
    c = base_set(); c.reserved1 = 0x80000000u;
    CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "reserved1 top bit");

    for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        c = base_set(); c.op = ops[i];
        CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "op %u", ops[i]);
        CHECK(verdict(&c, false) == NB_CURSOR_VIOLATION, "op %u, no fd",
              ops[i]);
    }

    c = base_set();
    CHECK(verdict(&c, false) == NB_CURSOR_VIOLATION, "SET without an fd");

    /* HIDE and SHOW: no fd, and every image field zero -- each one alone. */
    for (f = 0; f < 2; f++) {
        uint32_t op = f ? NVKVM_BROKER_CURSOR_SHOW : NVKVM_BROKER_CURSOR_HIDE;

        memset(&c, 0, sizeof c);
        c.type = NVKVM_BROKER_CMD_CURSOR;
        c.op = op;
        CHECK(verdict(&c, false) == NB_CURSOR_OK, "bare op %u", op);
        CHECK(verdict(&c, true) == NB_CURSOR_VIOLATION, "op %u with an fd",
              op);
        for (i = 0; i < 7; i++) {
            struct nvkvm_broker_cursor_cmd d = c;
            uint32_t *field[7] = { &d.width, &d.height, &d.stride, &d.offset,
                                   &d.fourcc, &d.hot_x, &d.hot_y };

            *field[i] = 1;
            CHECK(verdict(&d, false) == NB_CURSOR_VIOLATION,
                  "op %u with image field %u set", op, i);
        }
    }
}

static void test_content(void)
{
    struct nvkvm_broker_cursor_cmd c;

    c = base_set(); c.fourcc = XR24;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "XR24 cursor");
    c = base_set(); c.fourcc = 0;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "fourcc 0");

    /* Size: 1..256 on both axes. */
    c = base_set(); c.width = 0;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "width 0");
    c = base_set(); c.height = 0;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "height 0");
    c = base_set(); c.width = 1; c.height = 1; c.stride = 4;
    c.hot_x = 0; c.hot_y = 0;
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "1x1");
    c = base_set(); c.width = 256; c.height = 256; c.stride = 1024;
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "256x256");
    c = base_set(); c.width = 257; c.stride = 1028;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "width 257");
    c = base_set(); c.height = 257;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "height 257");
    /* A width whose *4 wraps 32 bits to something small. */
    c = base_set(); c.width = 0x40000001u; c.stride = 4;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "width that wraps *4");
    c = base_set(); c.width = 0xffffffffu;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "width UINT32_MAX");

    /* Hot spot: strictly inside. */
    c = base_set(); c.hot_x = 31; c.hot_y = 31;
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "hot spot at the last pixel");
    c = base_set(); c.hot_x = 32;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "hot_x == width");
    c = base_set(); c.hot_y = 32;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "hot_y == height");
    c = base_set(); c.hot_x = 0xffffffffu;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "hot_x UINT32_MAX");

    /* Stride: width*4 .. 1024. */
    c = base_set(); c.stride = 127;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "stride < width*4");
    c = base_set(); c.stride = 0;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "stride 0");
    c = base_set(); c.stride = 1024;
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "a 32-wide image in a 256 pitch");
    c = base_set(); c.stride = 1025;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "stride 1025");
    c = base_set(); c.stride = 0xffffffffu;
    CHECK(verdict(&c, true) == NB_CURSOR_REJECT, "stride UINT32_MAX");

    /* Offset is NOT the record's to judge -- it needs the fd's size. */
    c = base_set(); c.offset = 0xffffffffu;
    CHECK(verdict(&c, true) == NB_CURSOR_OK, "offset is checked by extent");
}

static void test_extent(void)
{
    CHECK(nb_cursor_span(32, 32, 128) == 128u * 31u + 128u, "32x32 span");
    CHECK(nb_cursor_span(1, 1, 4) == 4, "1x1 span");
    CHECK(nb_cursor_span(2, 2, 16) == 24,
          "the last row needs only its pixels, not a whole stride");
    CHECK(nb_cursor_span(256, 256, 1024) == 262144u, "largest span");
    CHECK(nb_cursor_span(0, 5, 4) == 0 && nb_cursor_span(5, 0, 20) == 0,
          "an empty image has no span");

    CHECK(nb_cursor_fits(0, 4096, 4096), "exact fit");
    CHECK(!nb_cursor_fits(1, 4096, 4096), "one byte over");
    CHECK(!nb_cursor_fits(0, 4096, 0), "an empty fd");
    /* In 32 bits 0xffffffff + 262144 wraps to 262143, which "fits" a 256 KiB
     * memfd.  It must not. */
    CHECK(!nb_cursor_fits(0xffffffffu, 262144u, 262144u),
          "offset + span must not wrap");
    CHECK(nb_cursor_fits(0xffffffffu, 1u, 0x100000000ull),
          "the exact 64-bit sum fits");
    CHECK(!nb_cursor_fits(0xffffffffu, 2u, 0x100000000ull),
          "and one past it does not");
}

static struct nb_cursor cur;     /* 256 KiB: static, not on the stack */

static void test_load(void)
{
    /* 2x2, stride 12 (one pixel of padding per row, filled with junk). */
    static const uint8_t src[] = {
        /* row 0: B G R A             padding  */
        0x01, 0x02, 0x03, 0xff,  0xff, 0x10, 0xff, 0x80,  0xde, 0xad, 0xbe, 0xef,
        /* row 1 */
        0x00, 0x00, 0x00, 0x00,  0x40, 0x40, 0x40, 0x40,
    };
    uint32_t gen0 = cur.gen;

    nb_cursor_load(&cur, src, 2, 2, 12, 1, 0);
    CHECK(cur.defined && cur.visible, "a load defines and shows");
    CHECK(cur.gen != gen0, "a load moves gen");
    CHECK(cur.w == 2 && cur.h == 2 && cur.hot_x == 1 && cur.hot_y == 0,
          "geometry copied");
    CHECK(cur.px[0] == 0xff030201u, "opaque pixel unchanged: 0x%08x",
          cur.px[0]);
    /* R and B exceed alpha 0x80: clamped to it.  G (0x10) does not. */
    CHECK(cur.px[1] == 0x80801080u, "colour clamped to alpha: 0x%08x",
          cur.px[1]);
    CHECK(cur.px[2] == 0x00000000u, "transparent pixel");
    CHECK(cur.px[3] == 0x40404040u, "exactly-premultiplied pixel");
    /* Packed: the padding never reached the image. */
    {
        unsigned i;
        bool junk = false;

        for (i = 0; i < 4; i++) {
            junk |= cur.px[i] == 0xefbeadde;
        }
        CHECK(!junk, "stride padding is not image");
    }
}

static void test_geom(void)
{
    struct nb_cursor_geom g;
    static const uint8_t white[256 * 1024];

    nb_cursor_load(&cur, white, 32, 32, 128, 3, 4);

    nb_cursor_scaled_geom(&cur, 1, 1, 1, 1, 512, &g);
    CHECK(g.w == 32 && g.h == 32 && g.hot_x == 3 && g.hot_y == 4, "1:1");
    nb_cursor_scaled_geom(&cur, 0, 0, 7, 0, 512, &g);
    CHECK(g.w == 32 && g.h == 32, "a zero factor means 1:1");
    nb_cursor_scaled_geom(&cur, 3840, 1920, 2160, 1080, 512, &g);
    CHECK(g.w == 64 && g.h == 64 && g.hot_x == 6 && g.hot_y == 8,
          "a 2x-scaled frame doubles the cursor and its hot spot");
    nb_cursor_scaled_geom(&cur, 960, 1920, 540, 1080, 512, &g);
    CHECK(g.w == 16 && g.h == 16 && g.hot_x == 1 && g.hot_y == 2,
          "a half-size frame halves it");
    nb_cursor_scaled_geom(&cur, 3, 1, 1, 1, 512, &g);
    CHECK(g.w == 96 && g.h == 32, "stretch scales the axes separately");
    nb_cursor_scaled_geom(&cur, 1, 1000, 1, 1000, 512, &g);
    CHECK(g.w == 1 && g.h == 1 && g.hot_x == 0 && g.hot_y == 0,
          "never below one pixel, hot spot still inside");
    nb_cursor_scaled_geom(&cur, 0xffffffffu, 1, 0xffffffffu, 1, 512, &g);
    CHECK(g.w == 512 && g.h == 512 && g.hot_x < 512 && g.hot_y < 512,
          "an absurd factor is clamped, not wrapped");
    nb_cursor_scaled_geom(&cur, 1, 1, 1, 1, 0, &g);
    CHECK(g.w == 1 && g.h == 1, "max 0 is treated as 1");

    nb_cursor_load(&cur, white, 256, 64, 1024, 255, 63);
    nb_cursor_scaled_geom(&cur, 4, 1, 4, 1, 512, &g);
    CHECK(g.w == 512 && g.h == 128, "clamping keeps the aspect (%ux%u)",
          g.w, g.h);
    CHECK(g.hot_x < g.w && g.hot_y < g.h, "clamped hot spot inside");
}

static void test_scale_pixels(void)
{
    /* A 2x2 checker scaled 2x must be four 2x2 blocks, nearest-neighbour. */
    static const uint8_t src[] = {
        0x00, 0x00, 0x00, 0xff,  0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,  0x00, 0x00, 0x00, 0xff,
    };
    uint32_t out[4 * 4 + 1];
    unsigned x, y;
    bool ok = true;

    nb_cursor_load(&cur, src, 2, 2, 8, 0, 0);
    out[16] = 0x5a5a5a5au;          /* canary past the end */
    nb_cursor_scale(&cur, out, 4, 4);
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            uint32_t want = ((x / 2) ^ (y / 2)) ? 0xffffffffu : 0xff000000u;

            ok &= out[y * 4 + x] == want;
        }
    }
    CHECK(ok, "nearest-neighbour 2x");
    CHECK(out[16] == 0x5a5a5a5au, "scale wrote past dw*dh");
}

static void test_wanted(void)
{
    struct nb_cursor *c = &cur;

    CHECK(!nb_cursor_wanted(NULL, false), "no cursor state");
    c->defined = true; c->visible = true;
    CHECK(nb_cursor_wanted(c, false), "defined, visible, ungrabbed");
    CHECK(!nb_cursor_wanted(c, true), "the grab hides it");
    c->visible = false;
    CHECK(!nb_cursor_wanted(c, false), "HIDE hides it");
    c->visible = true; c->defined = false;
    CHECK(!nb_cursor_wanted(c, false), "SHOW without an image shows none");
}

/* Deterministic: a failure here reproduces on every run and every machine. */
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* Mostly values near the boundaries, sometimes anything at all. */
static uint32_t edgy(uint32_t bound)
{
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return bound;
    case 2: return bound + 1;
    case 3: return bound ? bound - 1 : 0;
    case 4: return rnd() % (bound + 2);
    default: return rnd() | (rnd() << 16);
    }
}

static void test_fuzz(void)
{
    static uint8_t src[NVKVM_BROKER_CURSOR_MAX_STRIDE *
                       NVKVM_BROKER_CURSOR_MAX_DIM];
    static uint32_t slot[NB_CURSOR_SCALED_MAX * NB_CURSOR_SCALED_MAX + 1];
    unsigned i, accepted = 0, bad = 0;

    for (i = 0; i < sizeof src; i++) {
        src[i] = (uint8_t)rnd();
    }
    for (i = 0; i < 300000; i++) {
        struct nvkvm_broker_cursor_cmd c;
        const char *why;
        int v;

        memset(&c, 0, sizeof c);
        c.type = NVKVM_BROKER_CMD_CURSOR;
        c.op = (rnd() % 8) ? NVKVM_BROKER_CURSOR_SET : edgy(3);
        c.flags = (rnd() % 16) ? 0 : (uint16_t)rnd();
        c.reserved1 = (rnd() % 16) ? 0 : rnd();
        c.fourcc = (rnd() % 8) ? AR24 : rnd();
        c.width = edgy(NVKVM_BROKER_CURSOR_MAX_DIM);
        c.height = edgy(NVKVM_BROKER_CURSOR_MAX_DIM);
        c.stride = (rnd() % 2) ? c.width * 4u : edgy(NVKVM_BROKER_CURSOR_MAX_STRIDE);
        c.hot_x = edgy(c.width);
        c.hot_y = edgy(c.height);
        c.offset = edgy(4096);
        v = nb_cursor_check(&c, true, &why);
        if (v != NB_CURSOR_OK || c.op != NVKVM_BROKER_CURSOR_SET) {
            continue;
        }
        accepted++;
        /* What every caller of an accepted SET relies on. */
        if (c.width < 1 || c.width > NVKVM_BROKER_CURSOR_MAX_DIM ||
            c.height < 1 || c.height > NVKVM_BROKER_CURSOR_MAX_DIM ||
            c.hot_x >= c.width || c.hot_y >= c.height ||
            c.stride < c.width * 4u ||
            c.stride > NVKVM_BROKER_CURSOR_MAX_STRIDE ||
            nb_cursor_span(c.width, c.height, c.stride) > sizeof src ||
            nb_cursor_span(c.width, c.height, c.stride) < c.width * 4u) {
            bad++;
            continue;
        }
        /* Exercise the copy and the scaler on a subset: under ASAN an
         * out-of-range index in either is a hard failure. */
        if (i % 16 == 0) {
            struct nb_cursor_geom g;
            uint32_t nx = edgy(8192), dx = edgy(8192);
            uint32_t ny = edgy(8192), dy = edgy(8192);

            nb_cursor_load(&cur, src, c.width, c.height, c.stride, c.hot_x,
                           c.hot_y);
            nb_cursor_scaled_geom(&cur, nx, dx, ny, dy, NB_CURSOR_SCALED_MAX,
                                  &g);
            if (g.w < 1 || g.w > NB_CURSOR_SCALED_MAX || g.h < 1 ||
                g.h > NB_CURSOR_SCALED_MAX || g.hot_x >= g.w ||
                g.hot_y >= g.h) {
                bad++;
                continue;
            }
            slot[(size_t)g.w * g.h] = 0xa5a5a5a5u;
            nb_cursor_scale(&cur, slot, g.w, g.h);
            if (slot[(size_t)g.w * g.h] != 0xa5a5a5a5u) {
                bad++;
            }
            /* Premultiplied after the copy, whatever the source held. */
            {
                size_t k, n = (size_t)cur.w * cur.h;

                for (k = 0; k < n; k++) {
                    uint32_t p = cur.px[k], a = p >> 24;

                    if (((p >> 16) & 0xff) > a || ((p >> 8) & 0xff) > a ||
                        (p & 0xff) > a) {
                        bad++;
                        break;
                    }
                }
            }
        }
    }
    CHECK(accepted > 1000, "the sweep reached the accept path (%u)", accepted);
    CHECK(bad == 0, "%u accepted records broke an invariant", bad);
}

int main(void)
{
    test_framing();
    test_content();
    test_extent();
    test_load();
    test_geom();
    test_scale_pixels();
    test_wanted();
    test_fuzz();
    printf("test_cursor: %u checks, %u failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
