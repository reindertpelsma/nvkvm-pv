/*
 * vector_add_test.c — minimal cuLaunchKernel test.
 *
 * Loads a tiny precompiled CUDA module that does C[i] = A[i] + B[i] in
 * parallel.  PTX is sm_60 baseline so it runs on any GPU we test on.
 *
 * Exit codes: 0 pass, 1 init, 2 mem, 3 launch, 4 verify.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfn_st  *CUfunction;
typedef struct CUstream_st *CUstream;

#define CHK(call, code, msg) do { CUresult r = (call); if (r) { fprintf(stderr, msg " failed: %d\n", r); return code; } } while (0)

/*
 * NVRTC-compiled PTX for:
 *   extern "C" __global__ void vec_add(const int *a, const int *b, int *c, int n)
 *   {
 *       int i = blockIdx.x * blockDim.x + threadIdx.x;
 *       if (i < n) c[i] = a[i] + b[i];
 *   }
 * Built for sm_60 with nvcc 12.x; portable across all our test targets.
 */
static const char vec_add_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n"
"    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n"
"    .param .u32 vec_add_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<2>;\n"
"    .reg .b32   %r<8>;\n"
"    .reg .b64   %rd<11>;\n"
"\n"
"    ld.param.u64    %rd1, [vec_add_param_0];\n"
"    ld.param.u64    %rd2, [vec_add_param_1];\n"
"    ld.param.u64    %rd3, [vec_add_param_2];\n"
"    ld.param.u32    %r2,  [vec_add_param_3];\n"
"    mov.u32         %r3, %ntid.x;\n"
"    mov.u32         %r4, %ctaid.x;\n"
"    mov.u32         %r5, %tid.x;\n"
"    mad.lo.s32      %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32     %p1, %r1, %r2;\n"
"    @%p1 bra        $L__BB0_2;\n"
"\n"
"    cvta.to.global.u64  %rd4, %rd1;\n"
"    mul.wide.s32        %rd5, %r1, 4;\n"
"    add.s64             %rd6, %rd4, %rd5;\n"
"    cvta.to.global.u64  %rd7, %rd2;\n"
"    add.s64             %rd8, %rd7, %rd5;\n"
"    ld.global.u32       %r6, [%rd8];\n"
"    ld.global.u32       %r7, [%rd6];\n"
"    add.s32             %r6, %r7, %r6;\n"
"    cvta.to.global.u64  %rd9, %rd3;\n"
"    add.s64             %rd10, %rd9, %rd5;\n"
"    st.global.u32       [%rd10], %r6;\n"
"\n"
"$L__BB0_2:\n"
"    ret;\n"
"}\n";

int main(void) {
    void *h = dlopen("libcuda.so.1", 2);
    if (!h) h = dlopen("libcuda.so", 2);
    if (!h) { fprintf(stderr, "dlopen libcuda failed\n"); return 1; }

    CUresult (*cuInit)(unsigned)                                  = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice *, int)                      = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice)      = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuModuleLoadData)(CUmodule *, const void *)        = dlsym(h, "cuModuleLoadData");
    CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *) = dlsym(h, "cuModuleGetFunction");
    CUresult (*cuMemAlloc)(CUdeviceptr *, size_t)                 = dlsym(h, "cuMemAlloc_v2");
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t)   = dlsym(h, "cuMemcpyHtoD_v2");
    CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t)         = dlsym(h, "cuMemcpyDtoH_v2");
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                               unsigned, unsigned, unsigned,
                               unsigned, CUstream, void **, void **)
                                                                  = dlsym(h, "cuLaunchKernel");
    CUresult (*cuCtxSynchronize)(void)                            = dlsym(h, "cuCtxSynchronize");

    CHK(cuInit(0), 1, "cuInit");
    CUdevice dev;
    CHK(cuDeviceGet(&dev, 0), 1, "cuDeviceGet");
    CUcontext ctx;
    CHK(cuCtxCreate(&ctx, 0, dev), 1, "cuCtxCreate");

    CUmodule  mod;
    CUfunction fn;
    CHK(cuModuleLoadData(&mod, vec_add_ptx), 1, "cuModuleLoadData");
    CHK(cuModuleGetFunction(&fn, mod, "vec_add"), 1, "cuModuleGetFunction");

    const int N = 1024;
    int *a_h = malloc(N * sizeof(int));
    int *b_h = malloc(N * sizeof(int));
    int *c_h = malloc(N * sizeof(int));
    for (int i = 0; i < N; i++) { a_h[i] = i; b_h[i] = 2*i; c_h[i] = 0; }

    CUdeviceptr a_d, b_d, c_d;
    CHK(cuMemAlloc(&a_d, N*sizeof(int)), 2, "cuMemAlloc a");
    CHK(cuMemAlloc(&b_d, N*sizeof(int)), 2, "cuMemAlloc b");
    CHK(cuMemAlloc(&c_d, N*sizeof(int)), 2, "cuMemAlloc c");

    CHK(cuMemcpyHtoD(a_d, a_h, N*sizeof(int)), 2, "HtoD a");
    CHK(cuMemcpyHtoD(b_d, b_h, N*sizeof(int)), 2, "HtoD b");

    int n = N;
    void *args[] = { &a_d, &b_d, &c_d, &n };
    CHK(cuLaunchKernel(fn, 4,1,1, 256,1,1, 0, NULL, args, NULL), 3, "cuLaunchKernel");
    CHK(cuCtxSynchronize(), 3, "cuCtxSynchronize");

    CHK(cuMemcpyDtoH(c_h, c_d, N*sizeof(int)), 3, "DtoH c");

    int bad = 0;
    for (int i = 0; i < N; i++) {
        if (c_h[i] != a_h[i] + b_h[i]) {
            if (bad < 4)
                fprintf(stderr, "  c[%d]=%d expected %d\n", i, c_h[i], a_h[i]+b_h[i]);
            bad++;
        }
    }
    if (bad) {
        fprintf(stderr, "VERIFY FAILED: %d/%d mismatches\n", bad, N);
        return 4;
    }
    printf("PASS: %d-element vec_add ran on GPU through nvkvm forwarder\n", N);
    return 0;
}
