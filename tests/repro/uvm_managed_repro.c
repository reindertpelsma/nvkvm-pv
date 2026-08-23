/*
 * uvm_managed_repro.c -- minimal cuMemAllocManaged reproducer.
 *
 * Standalone: dlopen()s libcuda.so.1, hand-rolls the few driver-API types it
 * needs, so it builds with nothing but `cc`.  Runs identically on the host and
 * inside an nvkvm guest, which is the whole point -- the same binary, the same
 * output format, so "host works / guest fails" is a diff and not a story.
 *
 *   cc -O2 -o uvm_managed_repro uvm_managed_repro.c -ldl
 *   ./uvm_managed_repro
 *
 * Exit 0 iff managed memory allocated, was written by the CPU, read+written by
 * a real GPU kernel, and read back by the CPU with every element correct.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfn_st  *CUfunction;
typedef struct CUstream_st *CUstream;

#define CU_MEM_ATTACH_GLOBAL 0x1u

static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuDeviceGetName)(char *, int, CUdevice);
static CUresult (*p_cuDeviceGetAttribute)(int *, int, CUdevice);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*p_cuMemFree)(CUdeviceptr);
static CUresult (*p_cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*p_cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*p_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                    unsigned, unsigned, unsigned, unsigned,
                                    CUstream, void **, void **);
static CUresult (*p_cuCtxSynchronize)(void);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);

static void *H;
static void *sym2(const char *base) {
    char b[128]; void *s;
    snprintf(b, sizeof b, "%s_v2", base);
    s = dlsym(H, b);
    if (!s) s = dlsym(H, base);
    return s;
}
static const char *en(CUresult r) {
    const char *n = NULL;
    if (p_cuGetErrorName && p_cuGetErrorName(r, &n) == 0 && n) return n;
    return "?";
}

/* c[i] = a[i] + b[i] -- same kernel tests/integration/vector_add_test.c uses. */
static const char vec_add_ptx[] =
".version 7.5\n.target sm_60\n.address_size 64\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n"
"    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n"
"    .param .u32 vec_add_param_3\n"
")\n{\n"
"    .reg .pred %p<2>;\n    .reg .b32 %r<9>;\n    .reg .b64 %rd<11>;\n"
"    ld.param.u64 %rd1, [vec_add_param_0];\n"
"    ld.param.u64 %rd2, [vec_add_param_1];\n"
"    ld.param.u64 %rd3, [vec_add_param_2];\n"
"    ld.param.u32 %r2,  [vec_add_param_3];\n"
"    mov.u32 %r3, %ntid.x;\n    mov.u32 %r4, %ctaid.x;\n    mov.u32 %r5, %tid.x;\n"
"    mad.lo.s32 %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32 %p1, %r1, %r2;\n"
"    @%p1 bra $L__BB0_2;\n"
"    cvta.to.global.u64 %rd4, %rd1;\n"
"    mul.wide.s32 %rd5, %r1, 4;\n"
"    add.s64 %rd6, %rd4, %rd5;\n"
"    cvta.to.global.u64 %rd7, %rd2;\n"
"    add.s64 %rd8, %rd7, %rd5;\n"
"    ld.global.u32 %r6, [%rd8];\n"
"    ld.global.u32 %r7, [%rd6];\n"
"    add.s32 %r8, %r7, %r6;\n"
"    cvta.to.global.u64 %rd9, %rd3;\n"
"    add.s64 %rd10, %rd9, %rd5;\n"
"    st.global.u32 [%rd10], %r8;\n"
"$L__BB0_2:\n    ret;\n}\n";

#define N (1u << 20)          /* 1 Mi ints = 4 MiB per buffer */

int main(void) {
    H = dlopen("libcuda.so.1", RTLD_NOW);
    if (!H) H = dlopen("libcuda.so", RTLD_NOW);
    if (!H) { printf("FAIL libcuda: %s\n", dlerror()); return 1; }

    /* Only the handful of entry points that really have a _v2 ABI get sym2();
     * everything else is a plain dlsym.  libcuda 13 exports a
     * cuCtxSynchronize_v2 with DIFFERENT semantics -- resolving it by the
     * _v2-first rule makes every sync return 709 CONTEXT_IS_DESTROYED, which
     * looks exactly like a GPU fault and is not one. */
    p_cuInit              = dlsym(H, "cuInit");
    p_cuDeviceGet         = dlsym(H, "cuDeviceGet");
    p_cuDeviceGetName     = dlsym(H, "cuDeviceGetName");
    p_cuDeviceGetAttribute= dlsym(H, "cuDeviceGetAttribute");
    p_cuGetErrorName      = dlsym(H, "cuGetErrorName");
    p_cuModuleLoadData    = dlsym(H, "cuModuleLoadData");
    p_cuModuleGetFunction = dlsym(H, "cuModuleGetFunction");
    p_cuLaunchKernel      = dlsym(H, "cuLaunchKernel");
    p_cuCtxSynchronize    = dlsym(H, "cuCtxSynchronize");
    p_cuMemAllocManaged   = dlsym(H, "cuMemAllocManaged");
    p_cuCtxCreate         = sym2("cuCtxCreate");
    p_cuMemFree           = sym2("cuMemFree");
    if (!p_cuMemAllocManaged) { printf("FAIL cuMemAllocManaged not exported\n"); return 1; }

    CUresult r = p_cuInit(0);
    if (r) { printf("FAIL cuInit rc=%d (%s)\n", r, en(r)); return 1; }
    CUdevice dev = 0;
    r = p_cuDeviceGet(&dev, 0);
    if (r) { printf("FAIL cuDeviceGet rc=%d (%s)\n", r, en(r)); return 1; }
    char nm[256] = {0}; p_cuDeviceGetName(nm, sizeof nm, dev);

    /* 83 = CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY */
    int managed_cap = -1;
    if (p_cuDeviceGetAttribute) p_cuDeviceGetAttribute(&managed_cap, 83, dev);
    printf("device: %s  MANAGED_MEMORY attr=%d\n", nm, managed_cap);

    CUcontext ctx = NULL;
    r = p_cuCtxCreate(&ctx, 0, dev);
    if (r) { printf("FAIL cuCtxCreate rc=%d (%s)\n", r, en(r)); return 1; }

    CUdeviceptr a = 0, b = 0, c = 0;
    size_t nbytes = (size_t)N * 4;
    r = p_cuMemAllocManaged(&a, nbytes, CU_MEM_ATTACH_GLOBAL);
    if (r) { printf("FAIL cuMemAllocManaged(%zu) rc=%d (%s)\n", nbytes, r, en(r)); return 1; }
    r = p_cuMemAllocManaged(&b, nbytes, CU_MEM_ATTACH_GLOBAL);
    if (r) { printf("FAIL cuMemAllocManaged b rc=%d (%s)\n", r, en(r)); return 1; }
    r = p_cuMemAllocManaged(&c, nbytes, CU_MEM_ATTACH_GLOBAL);
    if (r) { printf("FAIL cuMemAllocManaged c rc=%d (%s)\n", r, en(r)); return 1; }
    printf("PASS cuMemAllocManaged 3x%zu bytes: a=0x%llx b=0x%llx c=0x%llx\n",
           nbytes, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);

    /* --- CPU writes the managed pages directly (no cuMemcpy) --- */
    int32_t *ha = (int32_t *)a, *hb = (int32_t *)b, *hc = (int32_t *)c;
    for (unsigned i = 0; i < N; i++) { ha[i] = (int32_t)i; hb[i] = (int32_t)(2 * i + 1); hc[i] = -1; }
    printf("PASS host wrote %u elements into managed pages\n", N);

    CUmodule mod = NULL; CUfunction fn = NULL;
    r = p_cuModuleLoadData(&mod, vec_add_ptx);
    if (r) { printf("FAIL cuModuleLoadData rc=%d (%s)\n", r, en(r)); return 1; }
    r = p_cuModuleGetFunction(&fn, mod, "vec_add");
    if (r) { printf("FAIL cuModuleGetFunction rc=%d (%s)\n", r, en(r)); return 1; }

    int n = (int)N;
    void *args[4] = { &a, &b, &c, &n };
    r = p_cuLaunchKernel(fn, (N + 255) / 256, 1, 1, 256, 1, 1, 0, NULL, args, NULL);
    if (r) { printf("FAIL cuLaunchKernel rc=%d (%s)\n", r, en(r)); return 1; }
    r = p_cuCtxSynchronize();
    if (r) { printf("FAIL cuCtxSynchronize rc=%d (%s)\n", r, en(r)); return 1; }

    /* --- CPU reads back what the GPU wrote, every element --- */
    size_t bad = 0; unsigned firstbad = 0; int32_t got = 0, want = 0;
    for (unsigned i = 0; i < N; i++) {
        int32_t w = (int32_t)i + (int32_t)(2 * i + 1);
        if (hc[i] != w) { if (!bad) { firstbad = i; got = hc[i]; want = w; } bad++; }
    }
    if (bad) {
        printf("FAIL managed readback: %zu/%u elements wrong, first at %u (got %d want %d)\n",
               bad, N, firstbad, got, want);
        return 1;
    }
    printf("PASS managed round trip: %u/%u elements correct (host->device->host)\n", N, N);
    p_cuMemFree(a); p_cuMemFree(b); p_cuMemFree(c);
    printf("ALL OK\n");
    return 0;
}
