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
    uint64_t gen0 = cur.gen;

    nb_cursor_load(&cur, src, 2, 2, 12, 1, 0);
    CHECK(cur.defined && cur.visible, "a load defines and shows");
    CHECK(cur.gen != gen0, "a load moves gen");
    /*
     * "Never repeats within a process" is a property of the WIDTH: a 32-bit
     * counter wraps to a value a backend may still hold as "already rendered"
     * (a cursor change every 8 ms reaches 2^32 in about a year).  Start one
     * load short of 2^32 and require the next to be 2^32, not 0.
     */
    cur.gen = 0xffffffffull;
    nb_cursor_load(&cur, src, 2, 2, 12, 1, 0);
    CHECK(cur.gen == 0x100000000ull, "gen does not wrap at 32 bits "
          "(0x%llx)", (unsigned long long)cur.gen);
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
    /* Halved, hot 3 is a source column the scaler skips (it samples 0, 2,
     * 4...); the hot spot is the output pixel showing the next one, 4. */
    nb_cursor_scaled_geom(&cur, 960, 1920, 540, 1080, 512, &g);
    CHECK(g.w == 16 && g.h == 16 && g.hot_x == 2 && g.hot_y == 2,
          "a half-size frame halves it (hot %u,%u)", g.hot_x, g.hot_y);
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

/* The scaler's own sampling rule, restated: output index x shows source
 * floor(x * in / out). */
static uint32_t sample_of(uint32_t x, uint32_t in, uint32_t out)
{
    return (uint32_t)((uint64_t)x * in / out);
}

/*
 * THE HOT SPOT LANDS ON THE GUEST'S HOT PIXEL (finding 6, 2026-10-03).
 *
 * First the case the review measured: 32 px at 1.5x with hot spot 3.  An image
 * whose ONLY opaque pixel is the hot pixel is scaled exactly as a backend
 * scales it, and the pixel under the scaled hot spot must be that pixel.
 * floor(hot * out / in) -- the old rule -- put it on output 4, which shows
 * source 2: transparent.
 */
static void test_hot_pixel(void)
{
    static uint8_t src[32 * 4 * 32];
    static uint32_t out[NB_CURSOR_SCALED_MAX * NB_CURSOR_SCALED_MAX];
    struct nb_cursor_geom g;

    memset(src, 0, sizeof src);
    src[3 * 128 + 3 * 4 + 3] = 0xff;            /* (3,3): alpha 255 */
    nb_cursor_load(&cur, src, 32, 32, 128, 3, 3);
    nb_cursor_scaled_geom(&cur, 3, 2, 3, 2, NB_CURSOR_SCALED_MAX, &g);
    CHECK(g.w == 48 && g.h == 48, "1.5x of 32 is 48 (%ux%u)", g.w, g.h);
    CHECK(g.hot_x == 5 && g.hot_y == 5, "1.5x: hot 3,3 -> 5,5 (got %u,%u)",
          g.hot_x, g.hot_y);
    nb_cursor_scale(&cur, out, g.w, g.h);
    CHECK((out[(size_t)g.hot_y * g.w + g.hot_x] >> 24) == 0xff,
          "1.5x: the pixel under the hot spot is the guest's hot pixel");
}

/*
 * ...and then every scale a window can plausibly produce, every width up to
 * 64 and the protocol's largest, and every hot spot in each: the hot spot must
 * be EXACTLY the first output index whose sample reaches the hot pixel
 * (clamped to the last), and whenever the cursor is not shrunk that index must
 * sample the hot pixel itself -- checked on real scaled pixels, not just on
 * the arithmetic.
 */
static void test_hot_sweep(void)
{
    static const uint32_t f[][2] = {
        {1, 1}, {3, 2}, {2, 1}, {5, 4}, {7, 5}, {4, 3}, {9, 4}, {3, 1},
        {5, 3}, {1, 2}, {2, 3}, {4, 5}, {1, 3}, {7, 3}, {11, 8}, {2560, 1920},
        {1920, 1280}, {3840, 2560}, {1366, 1024}, {1000, 999}, {999, 1000},
    };
    static uint8_t row[256 * 4];
    static uint32_t out[NB_CURSOR_SCALED_MAX * NB_CURSOR_SCALED_MAX];
    unsigned fi, bad = 0, exact = 0, pixel_bad = 0;
    uint32_t w, hot;

    for (fi = 0; fi < sizeof f / sizeof f[0]; fi++) {
        /* 1..64 one by one, then 128 and 256 (the protocol maximum). */
        for (w = 1; w <= 256; w = (w < 64) ? w + 1 : w * 2) {
            const uint32_t ww = w;

            for (hot = 0; hot < ww; hot++) {
                struct nb_cursor_geom g;
                uint32_t want, x;

                /* A ww x 1 image, opaque only at the hot pixel: the scaler is
                 * separable, so one row tests the x rule completely; the same
                 * call with the axes swapped tests y. */
                memset(row, 0, (size_t)ww * 4);
                row[hot * 4 + 3] = 0xff;
                nb_cursor_load(&cur, row, ww, 1, ww * 4, hot, 0);
                nb_cursor_scaled_geom(&cur, f[fi][0], f[fi][1], 1, 1,
                                      NB_CURSOR_SCALED_MAX, &g);
                want = (uint32_t)(((uint64_t)hot * g.w + ww - 1) / ww);
                if (want >= g.w) {
                    want = g.w - 1;
                }
                if (g.hot_x != want) {
                    bad++;
                }
                /* The first index at or past the hot pixel, by the scaler's
                 * own rule: no earlier index reaches it. */
                for (x = 0; x < g.hot_x; x++) {
                    if (sample_of(x, ww, g.w) >= hot) {
                        bad++;
                        break;
                    }
                }
                /* And, independently of the two arithmetic checks above, on
                 * the scaled pixels themselves. */
                if (g.w >= ww) {
                    exact++;
                    nb_cursor_scale(&cur, out, g.w, g.h);
                    if (sample_of(g.hot_x, ww, g.w) != hot ||
                        (out[g.hot_x] >> 24) != 0xff) {
                        pixel_bad++;
                    }
                }
                /* The y axis: the same image as a column. */
                nb_cursor_load(&cur, row, 1, ww, 4, 0, hot);
                nb_cursor_scaled_geom(&cur, 1, 1, f[fi][0], f[fi][1],
                                      NB_CURSOR_SCALED_MAX, &g);
                want = (uint32_t)(((uint64_t)hot * g.h + ww - 1) / ww);
                if (want >= g.h) {
                    want = g.h - 1;
                }
                if (g.hot_y != want) {
                    bad++;
                }
                if (g.h >= ww) {
                    nb_cursor_scale(&cur, out, g.w, g.h);
                    if ((out[(size_t)g.hot_y * g.w] >> 24) != 0xff) {
                        pixel_bad++;
                    }
                }
            }
        }
    }
    CHECK(exact > 10000, "the sweep reached the not-shrunk case (%u)", exact);
    CHECK(bad == 0, "%u hot spots off the first index that shows the hot "
          "pixel", bad);
    CHECK(pixel_bad == 0, "%u unshrunk cursors whose hot spot is not on the "
          "guest's hot pixel", pixel_bad);
}

/*
 * HiDPI (finding 7): the buffer a backend renders is the cursor in DEVICE
 * pixels.  A 32x32 guest cursor over a 3840-wide guest frame shown in a
 * 1920-logical window on a scale-2 output is 16 logical pixels -- and 32
 * device pixels, i.e. the guest's own, so nothing is lost to magnification.
 */
static void test_device_geom(void)
{
    static const uint8_t white[256 * 1024];
    struct nb_cursor_geom g, b;
    unsigned i, bad = 0;

    memset(&b, 0, sizeof b);
    nb_cursor_load(&cur, white, 32, 32, 128, 3, 4);
    nb_cursor_scaled_geom(&cur, 1920, 3840, 1080, 2160, 512, &g);
    nb_cursor_device_geom(&cur, 1920, 3840, 1080, 2160, 240, 512, &b);
    CHECK(g.w == 16 && g.h == 16, "logical: 16x16 (%ux%u)", g.w, g.h);
    CHECK(b.w == 32 && b.h == 32 && b.hot_x == 3 && b.hot_y == 4,
          "scale 2: the buffer is the guest's own 32x32, hot 3,4 "
          "(%ux%u hot %u,%u)", b.w, b.h, b.hot_x, b.hot_y);
    nb_cursor_device_geom(&cur, 1920, 3840, 1080, 2160, 180, 512, &b);
    CHECK(b.w == 24 && b.h == 24, "scale 1.5: 24x24 (%ux%u)", b.w, b.h);
    nb_cursor_device_geom(&cur, 1920, 3840, 1080, 2160, 120, 512, &b);
    CHECK(b.w == g.w && b.h == g.h && b.hot_x == g.hot_x &&
          b.hot_y == g.hot_y, "scale 1: the buffer is the logical cursor");
    nb_cursor_device_geom(&cur, 1920, 3840, 1080, 2160, 0, 512, &b);
    CHECK(b.w == g.w && b.h == g.h, "scale unknown (0): 1:1");
    nb_cursor_device_geom(&cur, 1920, 3840, 1080, 2160, 60, 512, &b);
    CHECK(b.w == g.w && b.h == g.h, "scale below 1: never fewer pixels "
          "than logical");
    nb_cursor_device_geom(&cur, 1, 1, 1, 1, 0xffffffffu, 512, &b);
    CHECK(b.w == 512 && b.h == 512 && b.hot_x < 512 && b.hot_y < 512,
          "an absurd scale is clamped (16x of 32 = 512)");
    nb_cursor_device_geom(&cur, 0xffffffffu, 1, 0xffffffffu, 1, 0xffffffffu,
                          512, &b);
    CHECK(b.w == 512 && b.h == 512 && b.hot_x < 512 && b.hot_y < 512,
          "absurd factor AND scale: clamped, not wrapped");
    /* Whatever the inputs, the buffer fits a slot and its hot spot is in it. */
    for (i = 0; i < 20000; i++) {
        uint32_t sw = 1 + (i * 2654435761u) % 256u;
        uint32_t sh = 1 + (i * 40503u) % 256u;

        nb_cursor_load(&cur, white, sw, sh, sw * 4, (i * 7u) % sw,
                       (i * 13u) % sh);
        nb_cursor_device_geom(&cur, 1 + (i * 977u) % 8192u,
                              1 + (i * 331u) % 8192u, 1 + (i * 619u) % 8192u,
                              1 + (i * 113u) % 8192u, (i * 37u) % 4000u,
                              NB_CURSOR_SCALED_MAX, &b);
        if (b.w < 1 || b.w > NB_CURSOR_SCALED_MAX || b.h < 1 ||
            b.h > NB_CURSOR_SCALED_MAX || b.hot_x >= b.w || b.hot_y >= b.h) {
            bad++;
        }
    }
    CHECK(bad == 0, "%u device geometries outside a slot", bad);
}

/*
 * PUBLISHING (finding 3) is a copy: the published cursor must be the image as
 * it was, unaffected by the next load into the pending one -- which is the
 * whole point of having two.
 */
static void test_copy(void)
{
    static struct nb_cursor pub;
    static const uint8_t red[] = { 0, 0, 0xff, 0xff,  0, 0, 0xff, 0xff };
    static const uint8_t blue[] = { 0xff, 0, 0, 0xff,  0xff, 0, 0, 0xff };

    nb_cursor_load(&cur, red, 2, 1, 8, 1, 0);
    nb_cursor_copy(&pub, &cur);
    CHECK(pub.defined && pub.visible && pub.gen == cur.gen && pub.w == 2 &&
          pub.h == 1 && pub.hot_x == 1 && pub.px[0] == 0xffff0000u &&
          pub.px[1] == 0xffff0000u, "a copy is the image, field for field");
    nb_cursor_load(&cur, blue, 2, 1, 8, 0, 0);
    CHECK(pub.px[0] == 0xffff0000u && pub.hot_x == 1 && pub.gen != cur.gen,
          "a later load does not reach the published copy");
    cur.defined = false;
    cur.gen++;
    nb_cursor_copy(&pub, &cur);
    CHECK(!pub.defined && pub.gen == cur.gen, "forgetting is published too");
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
    test_hot_pixel();
    test_hot_sweep();
    test_device_geom();
    test_copy();
    test_wanted();
    test_fuzz();
    printf("test_cursor: %u checks, %u failed\n", n_checks, n_fail);
    return n_fail ? 1 : 0;
}
