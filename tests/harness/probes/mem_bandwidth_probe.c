/*
 * mem_bandwidth_probe.c -- H2D / D2D / D2H bandwidth triad via the CUDA
 * driver API, dlopen()'d exactly like tests/validate.sh's embedded probes so
 * this compiles with nothing but `cc -ldl -lm` -- no CUDA toolkit, no -dev
 * packages, matching this project's rule that a probe's only build
 * dependency is a working `cc`. Written for the medium tier of
 * tests/harness: quantitative "how fast", not just "does it run".
 *
 * Usage: mem_bandwidth_probe [size_mb] [iters]     (defaults: 64 MB, 8 iters)
 *
 * Prints one machine-parseable "KEY: VALUE GB/s" line per leg, plus a
 * `device:` line and a final correctness line. On any dlopen/API failure
 * the program exits nonzero and says why on stderr, prefixed distinctly for
 * "no GPU here at all" (starts with "no libcuda") vs a real API failure --
 * the caller (tests/harness's medium tier) greps stderr to tell "SKIP, no
 * GPU" apart from "FAIL, something is actually wrong", the same way
 * tests/validate.sh's checks do.
 *
 * Exit codes: 0 pass, 1 no libcuda/cuInit/cuDeviceGet/cuCtxCreate failed,
 *             2 alloc failed, 3 memcpy failed, 4 correctness check failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

#define CHK(call, code, msg) do { CUresult r = (call); if (r) { \
    fprintf(stderr, msg " failed: cuda error %d\n", r); return code; } } while (0)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    size_t sz = (argc > 1) ? (size_t)atol(argv[1]) << 20 : (size_t)64 << 20;
    int iters = (argc > 2) ? atoi(argv[2]) : 8;
    if (iters < 1) iters = 1;

    void *h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!h) h = dlopen("libcuda.so", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "no libcuda: %s\n", dlerror());
        return 1;
    }

    CUresult (*cuInit)(unsigned) = (CUresult (*)(unsigned))dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice *, int) = (CUresult (*)(CUdevice *, int))dlsym(h, "cuDeviceGet");
    CUresult (*cuDeviceGetName)(char *, int, CUdevice) =
        (CUresult (*)(char *, int, CUdevice))dlsym(h, "cuDeviceGetName");
    CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice) =
        (CUresult (*)(CUcontext *, unsigned, CUdevice))dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuMemAlloc)(CUdeviceptr *, size_t) = (CUresult (*)(CUdeviceptr *, size_t))dlsym(h, "cuMemAlloc_v2");
    CUresult (*cuMemFree)(CUdeviceptr) = (CUresult (*)(CUdeviceptr))dlsym(h, "cuMemFree_v2");
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t) =
        (CUresult (*)(CUdeviceptr, const void *, size_t))dlsym(h, "cuMemcpyHtoD_v2");
    CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t) =
        (CUresult (*)(void *, CUdeviceptr, size_t))dlsym(h, "cuMemcpyDtoH_v2");
    CUresult (*cuMemcpyDtoD)(CUdeviceptr, CUdeviceptr, size_t) =
        (CUresult (*)(CUdeviceptr, CUdeviceptr, size_t))dlsym(h, "cuMemcpyDtoD_v2");

    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuMemAlloc || !cuMemFree || !cuMemcpyHtoD || !cuMemcpyDtoH ||
        !cuMemcpyDtoD) {
        fprintf(stderr, "no libcuda: expected driver-API symbols missing\n");
        return 1;
    }

    CHK(cuInit(0), 1, "cuInit");
    CUdevice dev;
    CHK(cuDeviceGet(&dev, 0), 1, "cuDeviceGet");
    char name[256] = {0};
    if (cuDeviceGetName) cuDeviceGetName(name, sizeof name, dev);
    CUcontext ctx;
    CHK(cuCtxCreate(&ctx, 0, dev), 1, "cuCtxCreate");
    printf("device: %s\n", name);

    CUdeviceptr d1, d2;
    CHK(cuMemAlloc(&d1, sz), 2, "alloc d1");
    CHK(cuMemAlloc(&d2, sz), 2, "alloc d2");

    unsigned char *host = malloc(sz);
    unsigned char *readback = malloc(sz);
    if (!host || !readback) {
        fprintf(stderr, "host malloc(%zu) failed\n", sz);
        return 2;
    }
    for (size_t i = 0; i < sz; i++) host[i] = (unsigned char)(i * 2654435761u);

    /* warm each leg once so steady-state bandwidth is measured, not
     * first-touch / lazy-mapping cost */
    CHK(cuMemcpyHtoD(d1, host, sz), 3, "H2D warmup");
    CHK(cuMemcpyDtoD(d2, d1, sz), 3, "D2D warmup");
    CHK(cuMemcpyDtoH(readback, d2, sz), 3, "D2H warmup");

    double t;

    t = now();
    for (int i = 0; i < iters; i++) CHK(cuMemcpyHtoD(d1, host, sz), 3, "H2D");
    t = now() - t;
    printf("H2D: %.2f GB/s\n", (double)sz * iters / t / 1e9);

    t = now();
    for (int i = 0; i < iters; i++) CHK(cuMemcpyDtoD(d2, d1, sz), 3, "D2D");
    t = now() - t;
    printf("D2D: %.2f GB/s\n", (double)sz * iters / t / 1e9);

    t = now();
    for (int i = 0; i < iters; i++) CHK(cuMemcpyDtoH(readback, d2, sz), 3, "D2H");
    t = now() - t;
    printf("D2H: %.2f GB/s\n", (double)sz * iters / t / 1e9);

    /* correctness: H2D -> D2D -> D2H must round-trip the pattern byte-exact.
     * A bandwidth number from a corrupted copy is worse than no number --
     * same spirit as validate.sh's memcmp check. */
    size_t bad = 0;
    for (size_t i = 0; i < sz; i++)
        if (readback[i] != host[i]) bad++;
    if (bad) {
        fprintf(stderr, "correctness FAIL: %zu/%zu bytes mismatched after H2D->D2D->D2H\n", bad, sz);
        return 4;
    }
    printf("correctness: OK (%zu bytes verified)\n", sz);

    cuMemFree(d1);
    cuMemFree(d2);
    free(host);
    free(readback);
    return 0;
}
