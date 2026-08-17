/*
 * pinned_write_bench — isolate the per-frame "feed" cost (#101).
 *
 * cuMemAllocHost() pins host memory and (in nvkvm) maps it through the GPA
 * window — the same class of buffer ffmpeg's NVENC input surface lives in.
 * We time pure CPU memcpy INTO and OUT OF that pinned buffer vs a plain
 * malloc() buffer. If writes into the pinned/window buffer are dramatically
 * slower than malloc (and slower than reads from it), the per-frame NVENC
 * cost is the CPU write-through-window path — not virtqueue copying, not WB.
 *
 * Run on host and guest; compare. No KVM debugfs needed.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    size_t mb   = argc > 1 ? atol(argv[1]) : 8;     /* buffer size MB   */
    int    iter = argc > 2 ? atoi(argv[2]) : 200;   /* iterations       */
    size_t sz   = mb << 20;

    void *h = dlopen("libcuda.so.1", 2); if (!h) h = dlopen("libcuda.so", 2);
    if (!h) { fprintf(stderr, "no libcuda\n"); return 1; }
    CUresult (*cuInit)(unsigned)                            = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice*, int)                 = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext*, unsigned, CUdevice) = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuMemAllocHost)(void**, size_t)              = dlsym(h, "cuMemAllocHost_v2");

    if (cuInit(0)) { fprintf(stderr, "cuInit failed\n"); return 1; }
    CUdevice dev; if (cuDeviceGet(&dev, 0)) return 1;
    CUcontext ctx; if (cuCtxCreate(&ctx, 0, dev)) { fprintf(stderr, "ctx failed\n"); return 1; }

    void *pinned = NULL;
    if (cuMemAllocHost(&pinned, sz) || !pinned) { fprintf(stderr, "cuMemAllocHost failed\n"); return 2; }
    uint8_t *mal = malloc(sz);
    uint8_t *src = malloc(sz);
    memset(src, 0xA5, sz);
    memset(mal, 0, sz);
    memset(pinned, 0, sz);   /* fault both in first */

    double t, gb = (double)sz * iter / 1e9;
    volatile uint64_t sink = 0;

    /* WRITE into malloc (baseline) */
    t = now();
    for (int i = 0; i < iter; i++) memcpy(mal, src, sz);
    double w_mal = gb / (now() - t);

    /* WRITE into pinned (window-mapped) */
    t = now();
    for (int i = 0; i < iter; i++) memcpy(pinned, src, sz);
    double w_pin = gb / (now() - t);

    /* READ from malloc */
    t = now();
    for (int i = 0; i < iter; i++) { memcpy(src, mal, sz); }
    double r_mal = gb / (now() - t);

    /* READ from pinned */
    t = now();
    for (int i = 0; i < iter; i++) { memcpy(src, pinned, sz); }
    double r_pin = gb / (now() - t);
    (void)sink;

    printf("buf=%zuMB iter=%d\n", mb, iter);
    printf("  WRITE malloc  : %6.2f GB/s\n", w_mal);
    printf("  WRITE pinned  : %6.2f GB/s   (%.1fx slower than malloc)\n", w_pin, w_mal / w_pin);
    printf("  READ  malloc  : %6.2f GB/s\n", r_mal);
    printf("  READ  pinned  : %6.2f GB/s   (%.1fx slower than malloc)\n", r_pin, r_mal / r_pin);
    return 0;
}
