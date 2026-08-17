/*
 * sig_interrupt_test.c — #73 signal/interrupt delivery during forwarded ioctls.
 *
 * Queues many async matmul launches, then blocks in cuCtxSynchronize (a long
 * forwarded wait ioctl).  A harness signals this process mid-sync; with #73 the
 * guest's wait_for_completion is interruptible, so the process dies promptly
 * instead of hanging until the GPU drains.
 *
 * Prints "SYNC_ENTER" (flushed) immediately before the blocking sync and
 * "SYNC_DONE" only if the sync completes normally.  argv[1] = launch count.
 *
 * Exit codes: 0 sync completed (NOT signaled), 1 init, 2 alloc, 3 launch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfn_st  *CUfunction;
typedef struct CUstream_st *CUstream;

#define CHK(call, code, msg) do { CUresult r = (call); if (r) { fprintf(stderr, msg " failed: %d\n", r); return code; } } while (0)

static const char matmul_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry matmul(\n"
"    .param .u64 matmul_param_0,\n"
"    .param .u64 matmul_param_1,\n"
"    .param .u64 matmul_param_2,\n"
"    .param .u32 matmul_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<5>;\n"
"    .reg .b32   %r<16>;\n"
"    .reg .f32   %f<6>;\n"
"    .reg .b64   %rd<16>;\n"
"\n"
"    ld.param.u64    %rd1, [matmul_param_0];\n"
"    ld.param.u64    %rd2, [matmul_param_1];\n"
"    ld.param.u64    %rd3, [matmul_param_2];\n"
"    ld.param.u32    %r1,  [matmul_param_3];\n"
"    mov.u32         %r2, %ntid.y;\n"
"    mov.u32         %r3, %ctaid.y;\n"
"    mov.u32         %r4, %tid.y;\n"
"    mad.lo.s32      %r5, %r3, %r2, %r4;\n"   // row
"    mov.u32         %r6, %ntid.x;\n"
"    mov.u32         %r7, %ctaid.x;\n"
"    mov.u32         %r8, %tid.x;\n"
"    mad.lo.s32      %r9, %r7, %r6, %r8;\n"   // col
"    setp.ge.s32     %p1, %r5, %r1;\n"
"    setp.ge.s32     %p2, %r9, %r1;\n"
"    or.pred         %p3, %p1, %p2;\n"
"    @%p3 bra        $L__END;\n"
"\n"
"    mov.f32         %f1, 0f00000000;\n"
"    mov.u32         %r10, 0;\n"
"$L__LOOP:\n"
"    setp.ge.s32     %p4, %r10, %r1;\n"
"    @%p4 bra        $L__STORE;\n"
"    mad.lo.s32      %r11, %r5, %r1, %r10;\n"
"    mul.wide.s32    %rd4, %r11, 4;\n"
"    cvta.to.global.u64 %rd5, %rd1;\n"
"    add.s64         %rd6, %rd5, %rd4;\n"
"    ld.global.f32   %f2, [%rd6];\n"
"    mad.lo.s32      %r12, %r10, %r1, %r9;\n"
"    mul.wide.s32    %rd7, %r12, 4;\n"
"    cvta.to.global.u64 %rd8, %rd2;\n"
"    add.s64         %rd9, %rd8, %rd7;\n"
"    ld.global.f32   %f3, [%rd9];\n"
"    fma.rn.f32      %f1, %f2, %f3, %f1;\n"
"    add.s32         %r10, %r10, 1;\n"
"    bra.uni         $L__LOOP;\n"
"$L__STORE:\n"
"    mad.lo.s32      %r13, %r5, %r1, %r9;\n"
"    mul.wide.s32    %rd10, %r13, 4;\n"
"    cvta.to.global.u64 %rd11, %rd3;\n"
"    add.s64         %rd12, %rd11, %rd10;\n"
"    st.global.f32   [%rd12], %f1;\n"
"$L__END:\n"
"    ret;\n"
"}\n";

int main(int argc, char **argv) {
    int launches = (argc > 1) ? atoi(argv[1]) : 6000;
    void *h = dlopen("libcuda.so.1", 2);
    if (!h) h = dlopen("libcuda.so", 2);
    if (!h) { fprintf(stderr, "no libcuda\n"); return 1; }

    CUresult (*cuInit)(unsigned)                              = dlsym(h, "cuInit");
    CUresult (*cuDeviceGet)(CUdevice*, int)                   = dlsym(h, "cuDeviceGet");
    CUresult (*cuCtxCreate)(CUcontext*, unsigned, CUdevice)   = dlsym(h, "cuCtxCreate_v2");
    CUresult (*cuModuleLoadData)(CUmodule*, const void*)      = dlsym(h, "cuModuleLoadData");
    CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = dlsym(h, "cuModuleGetFunction");
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t)              = dlsym(h, "cuMemAlloc_v2");
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, size_t) = dlsym(h, "cuMemcpyHtoD_v2");
    CUresult (*cuLaunchKernel)(CUfunction,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,CUstream,void**,void**) = dlsym(h, "cuLaunchKernel");
    CUresult (*cuCtxSynchronize)(void)                        = dlsym(h, "cuCtxSynchronize");

    CHK(cuInit(0), 1, "cuInit");
    CUdevice dev; CHK(cuDeviceGet(&dev, 0), 1, "cuDeviceGet");
    CUcontext ctx; CHK(cuCtxCreate(&ctx, 0, dev), 1, "cuCtxCreate");
    CUmodule mod; CUfunction fn;
    CHK(cuModuleLoadData(&mod, matmul_ptx), 1, "cuModuleLoadData");
    CHK(cuModuleGetFunction(&fn, mod, "matmul"), 1, "cuModuleGetFunction");

    const int N = 1024;
    size_t bytes = (size_t)N * N * sizeof(float);
    float *A = malloc(bytes), *B = malloc(bytes);
    for (int i = 0; i < N*N; i++) { A[i] = (float)(i % 17) * 0.1f; B[i] = (float)(i % 13) * 0.2f; }
    CUdeviceptr dA, dB, dC;
    CHK(cuMemAlloc(&dA, bytes), 2, "alloc A");
    CHK(cuMemAlloc(&dB, bytes), 2, "alloc B");
    CHK(cuMemAlloc(&dC, bytes), 2, "alloc C");
    CHK(cuMemcpyHtoD(dA, A, bytes), 2, "HtoD A");
    CHK(cuMemcpyHtoD(dB, B, bytes), 2, "HtoD B");

    int n = N;
    void *args[] = { &dA, &dB, &dC, &n };
    for (int i = 0; i < launches; i++)
        CHK(cuLaunchKernel(fn, 32,32,1, 32,32,1, 0, NULL, args, NULL), 3, "launch");

    printf("SYNC_ENTER\n");
    fflush(stdout);
    CHK(cuCtxSynchronize(), 3, "sync");
    printf("SYNC_DONE\n");
    fflush(stdout);
    return 0;
}
