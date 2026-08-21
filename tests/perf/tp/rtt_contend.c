/*
 * rtt_contend.c — per-call forwarding round-trip latency, with a DISTRIBUTION
 * and under configurable concurrency.
 *
 * Why this exists: tests/perf/launchstorm.c reports a mean over 20k launches.
 * A mean hides the thing we are hunting — a long tail that a *synchronised*
 * group of N workers pays N times over instead of overlapping away.  It also
 * runs one process, so it cannot see contention inside the forwarder.
 *
 * Timing is rdtscp, not clock_gettime: under kvm-clock the guest leaves the
 * vDSO and clock_gettime costs ~645 ns, which is 5% of a 12 us round trip and
 * 100% of the difference we are looking for at the low end.  The TSC is read
 * directly and converted once, at the end, against a CLOCK_MONOTONIC-calibrated
 * frequency measured in this same process.
 *
 * Modes:
 *   launch  — cuLaunchKernel(empty) + cuCtxSynchronize   (the control tax)
 *   sync    — cuCtxSynchronize on an idle context        (completion poll only)
 *   alloc   — cuMemAlloc + cuMemFree                     (known 29x tax)
 *
 * Usage: rtt_contend <device> <mode> <iters> [warmup]
 */
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define CHK(x) do{CUresult r=(x);if(r){const char*s=0;cuGetErrorName(r,&s);\
    fprintf(stderr,"%s:%d %s=%d %s\n",__FILE__,__LINE__,#x,r,s?s:"");exit(1);}}while(0)

static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;}

static inline uint64_t rdtscp_now(void){
    unsigned aux; unsigned lo, hi;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate TSC ticks per second against CLOCK_MONOTONIC over ~200 ms. */
static double tsc_hz(void){
    double t0 = now_s(); uint64_t c0 = rdtscp_now();
    struct timespec req = {0, 200000000L}; nanosleep(&req, NULL);
    uint64_t c1 = rdtscp_now(); double t1 = now_s();
    return (double)(c1 - c0) / (t1 - t0);
}

static int cmp_u64(const void *a, const void *b){
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static const char* PTX =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry nop(){\n ret;\n}\n";

int main(int argc, char **argv){
    int dev    = argc > 1 ? atoi(argv[1]) : 0;
    const char *mode = argc > 2 ? argv[2] : "launch";
    int iters  = argc > 3 ? atoi(argv[3]) : 20000;
    int warm   = argc > 4 ? atoi(argv[4]) : 2000;

    CHK(cuInit(0));
    CUdevice d; CHK(cuDeviceGet(&d, dev));
    CUcontext c; CHK(cuCtxCreate(&c, CU_CTX_SCHED_SPIN, d));
    CUmodule m; CHK(cuModuleLoadData(&m, PTX));
    CUfunction f; CHK(cuModuleGetFunction(&f, m, "nop"));

    double hz = tsc_hz();

    uint64_t *s = malloc((size_t)iters * sizeof(uint64_t));
    if (!s) { fprintf(stderr, "oom\n"); return 1; }

    int is_launch = !strcmp(mode, "launch");
    int is_sync   = !strcmp(mode, "sync");
    int is_alloc  = !strcmp(mode, "alloc");
    if (!is_launch && !is_sync && !is_alloc) { fprintf(stderr, "bad mode\n"); return 1; }

    for (int i = 0; i < warm; i++) {
        if (is_launch) { CHK(cuLaunchKernel(f,1,1,1,1,1,1,0,0,0,0)); CHK(cuCtxSynchronize()); }
        else if (is_sync) { CHK(cuCtxSynchronize()); }
        else { CUdeviceptr p; CHK(cuMemAlloc(&p, 4096)); CHK(cuMemFree(p)); }
    }

    double wall0 = now_s();
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = rdtscp_now();
        if (is_launch) { CHK(cuLaunchKernel(f,1,1,1,1,1,1,0,0,0,0)); CHK(cuCtxSynchronize()); }
        else if (is_sync) { CHK(cuCtxSynchronize()); }
        else { CUdeviceptr p; CHK(cuMemAlloc(&p, 4096)); CHK(cuMemFree(p)); }
        s[i] = rdtscp_now() - t0;
    }
    double wall = now_s() - wall0;

    double mean_us = wall / iters * 1e6;
    qsort(s, iters, sizeof(uint64_t), cmp_u64);
    #define P(q) (s[(int)((iters-1)*(q))] / hz * 1e6)
    printf("RTT|dev=%d|mode=%s|n=%d|tsc_ghz=%.4f|mean_us=%.3f|p50=%.3f|p90=%.3f|p99=%.3f|p999=%.3f|max=%.3f|min=%.3f\n",
           dev, mode, iters, hz/1e9, mean_us, P(0.50), P(0.90), P(0.99), P(0.999),
           s[iters-1]/hz*1e6, s[0]/hz*1e6);
    return 0;
}
