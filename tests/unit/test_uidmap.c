/*
 * The uid window must land inside THIS namespace's uid map.
 *
 * Under `dockerd --userns-remap` or sysbox-runc a container is given a mapped
 * range, commonly 65536 uids.  setuid() to a uid outside it fails EINVAL -- not
 * a permission problem, an unmapped uid -- so a hardcoded window at 500000
 * breaks on exactly the deployments that enabled user namespaces.
 */
#include "nvkvm_uidmap.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, what) do {                                              \
    if (cond) { printf("  ok   %s\n", (what)); }                            \
    else      { printf("  FAIL %s\n", (what)); fails++; }                   \
} while (0)

int main(void)
{
    struct nvkvm_uidmap live;
    struct nvkvm_uidmap id    = { 0, 0x7ffffffeu, true };
    struct nvkvm_uidmap ns64k = { 0, 65535, false };
    struct nvkvm_uidmap tiny  = { 0, 100, false };
    struct nvkvm_uidmap tight = { 0, 4096, false };
    uint32_t base;

    puts("uid map discovery");
    nvkvm_uidmap_get(&live);
    printf("  live: lo=%u hi=%u identity=%d\n", live.lo, live.hi, live.identity);
    CHECK(live.hi <= 0x7ffffffeu,
          "never reports a uid at or above 2^31 (poorly-tested territory)");
    CHECK(live.hi >= live.lo, "the reported range is not inverted");

    puts("an ordinary host");
    CHECK(nvkvm_uidmap_fits(&id, 500000, 4096),
          "the historic 500000 window fits when there is no namespace");

    puts("a 65536-uid user namespace");
    CHECK(!nvkvm_uidmap_fits(&ns64k, 500000, 4096),
          "the historic window does NOT fit -- this is the bug being fixed");
    base = nvkvm_uidmap_place(&ns64k, 4096);
    printf("  placed base=%u\n", base);
    CHECK(base == 65536 - 4096, "a window is placed at the top of the map");
    CHECK(nvkvm_uidmap_fits(&ns64k, base, 4096),
          "and the placed window fits by the same test that rejected the old one");

    puts("degenerate maps");
    CHECK(nvkvm_uidmap_place(&tiny, 4096) == 0,
          "a map too small reports failure rather than a bad uid");
    CHECK(!nvkvm_uidmap_fits(&tiny, 1, 4096), "and nothing fits in it");
    CHECK(nvkvm_uidmap_place(&tight, 4096) >= 1,
          "an exactly-sized map never hands back uid 0");
    CHECK(!nvkvm_uidmap_fits(&tight, 0, 4096), "uid 0 is never a valid base");
    CHECK(nvkvm_uidmap_place(&id, 0) == 0, "a zero-width window is refused");

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "test_uidmap passed", fails);
    return fails ? 1 : 0;
}
