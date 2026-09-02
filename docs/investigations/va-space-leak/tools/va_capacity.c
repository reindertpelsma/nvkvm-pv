/* va_capacity.c - measure remaining host GPU VA capacity.
 *
 * The leak exhausts RM's DMA VA space, not VRAM: nvidia-smi stays flat while
 * dmaAllocMapping_GM107 starts failing.  Every cuMemAlloc consumes a DMA
 * mapping (and therefore VA), so "how much can we still allocate" declines as
 * the leak grows -- a leading indicator, unlike the dmesg errors which only
 * appear once VA is already full.
 *
 * Two numbers:
 *   MAXCONTIG_MB  largest single cuMemAlloc that succeeds (binary search)
 *   TOTAL_MB      total allocatable in 128MB chunks (capped)
 *   NCHUNKS       how many chunks succeeded
 *
 * Driver API only, so no nvcc and no CUDA headers needed.
 * Build: gcc -O0 -o va_capacity va_capacity.c -lcuda
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

extern int cuInit(unsigned);
extern int cuDeviceGet(int *, int);
extern int cuDevicePrimaryCtxRetain(void **, int);
extern int cuCtxSetCurrent(void *);
extern int cuMemAlloc_v2(unsigned long long *, size_t);
extern int cuMemFree_v2(unsigned long long);
extern int cuMemGetInfo_v2(size_t *, size_t *);

#define MB (1024ULL * 1024ULL)
#define MAX_CHUNKS 512          /* 512 * 128MB = 64GB ceiling */

int main(void) {
    void *ctx; int dev; int rc;
    if ((rc = cuInit(0)) != 0)                      { printf("ERR cuInit=%d\n", rc); return 1; }
    if ((rc = cuDeviceGet(&dev, 0)) != 0)           { printf("ERR cuDeviceGet=%d\n", rc); return 1; }
    if ((rc = cuDevicePrimaryCtxRetain(&ctx, dev)) != 0) { printf("ERR ctxRetain=%d\n", rc); return 1; }
    if ((rc = cuCtxSetCurrent(ctx)) != 0)           { printf("ERR setCurrent=%d\n", rc); return 1; }

    size_t freeb = 0, totb = 0;
    cuMemGetInfo_v2(&freeb, &totb);

    /* 1. Largest single allocation that still succeeds. */
    unsigned long long lo = 0, hi = 16384, p;   /* MB */
    while (lo < hi) {
        unsigned long long mid = (lo + hi + 1) / 2;
        if (cuMemAlloc_v2(&p, (size_t)(mid * MB)) == 0) { cuMemFree_v2(p); lo = mid; }
        else hi = mid - 1;
    }
    unsigned long long maxcontig = lo;

    /* 2. Total allocatable in 128MB chunks. */
    static unsigned long long ptrs[MAX_CHUNKS];
    int n = 0;
    while (n < MAX_CHUNKS && cuMemAlloc_v2(&ptrs[n], (size_t)(128 * MB)) == 0) n++;
    unsigned long long total = (unsigned long long)n * 128;
    for (int i = 0; i < n; i++) cuMemFree_v2(ptrs[i]);

    printf("MAXCONTIG_MB=%llu TOTAL_MB=%llu NCHUNKS=%d CUDA_FREE_MB=%zu CUDA_TOTAL_MB=%zu\n",
           maxcontig, total, n, freeb / MB, totb / MB);
    return 0;
}
