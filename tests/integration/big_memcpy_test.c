#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

int main(int argc, char **argv) {
    size_t sz_kb = argc > 1 ? atol(argv[1]) : 16;
    size_t sz = sz_kb * 1024;

    void *h = dlopen("libcuda.so.1", 2); if (!h) h = dlopen("libcuda.so", 2);
    if (!h) { fprintf(stderr, "no libcuda\n"); return 1; }
    CUresult (*cuInit)(unsigned)                          = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice*, int)               = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext*, unsigned, CUdevice) = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t)          = dlsym(h, "cuMemAlloc_v2");
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, size_t) = dlsym(h, "cuMemcpyHtoD_v2");
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, size_t)  = dlsym(h, "cuMemcpyDtoH_v2");

    int r = cuInit(0); if (r) { printf("init=%d\n", r); return 1; }
    CUdevice dev; r = cuDeviceGet(&dev, 0); if (r) { printf("dev=%d\n", r); return 1; }
    CUcontext ctx; r = cuCtxCreate(&ctx, 0, dev); if (r) { printf("ctx=%d\n", r); return 1; }

    uint8_t *h_src = malloc(sz);
    uint8_t *h_dst = calloc(1, sz);
    for (size_t i = 0; i < sz; i++) h_src[i] = (uint8_t)(i * 31 + 7);

    CUdeviceptr d_ptr;
    r = cuMemAlloc(&d_ptr, sz); if (r) { printf("alloc=%d\n", r); return 2; }
    r = cuMemcpyHtoD(d_ptr, h_src, sz); if (r) { printf("htod=%d\n", r); return 3; }
    r = cuMemcpyDtoH(h_dst, d_ptr, sz); if (r) { printf("dtoh=%d\n", r); return 3; }

    int bad = 0;
    for (size_t i = 0; i < sz; i++) {
        if (h_src[i] != h_dst[i]) {
            if (bad < 4) printf("  diff[%zu] src=%d dst=%d\n", i, h_src[i], h_dst[i]);
            bad++;
        }
    }
    if (bad) { printf("FAIL: %d/%zu mismatches in %zu bytes\n", bad, sz, sz); return 4; }
    printf("PASS: %zu bytes round-tripped exactly\n", sz);
    return 0;
}
