/* gpu_bench.c — host-vs-guest GPU overhead benchmark via the CUDA driver API.
 * Self-contained (dlopen libcuda + embedded PTX, JIT'd at runtime), so the SAME
 * binary runs on the host (direct -> driver) and the guest (-> nvkvm forwarder).
 *
 * Phase A  throughput  : large naive FP32 GEMM, timed -> GFLOP/s (compute/data
 *                        path; expected ~identical host vs guest).
 * Phase B  launch RTT  : flood of no-op kernels with per-launch sync -> us/launch
 *                        (control path; exposes per-submit forwarding latency).
 * Phase C  alloc RTT   : cuMemAlloc+cuMemFree pairs -> us/alloc (most ioctl-heavy;
 *                        where the forwarder tax is largest).
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
typedef struct CUmod_st *CUmodule;
typedef struct CUfn_st  *CUfunction;
typedef struct CUstream_st *CUstream;

#define CHK(call, msg) do { CUresult r=(call); if(r){fprintf(stderr, msg " failed: %d\n", r); return 1;} } while(0)

static const char ptx[] =
".version 7.5\n.target sm_60\n.address_size 64\n"
".visible .entry noop(){ ret; }\n"
".visible .entry matmul(.param .u64 p0,.param .u64 p1,.param .u64 p2,.param .u32 p3){\n"
" .reg .pred %p<5>; .reg .b32 %r<16>; .reg .f32 %f<6>; .reg .b64 %rd<16>;\n"
" ld.param.u64 %rd1,[p0]; ld.param.u64 %rd2,[p1]; ld.param.u64 %rd3,[p2]; ld.param.u32 %r1,[p3];\n"
" mov.u32 %r2,%ntid.y; mov.u32 %r3,%ctaid.y; mov.u32 %r4,%tid.y; mad.lo.s32 %r5,%r3,%r2,%r4;\n"
" mov.u32 %r6,%ntid.x; mov.u32 %r7,%ctaid.x; mov.u32 %r8,%tid.x; mad.lo.s32 %r9,%r7,%r6,%r8;\n"
" setp.ge.s32 %p1,%r5,%r1; setp.ge.s32 %p2,%r9,%r1; or.pred %p3,%p1,%p2; @%p3 bra $E;\n"
" mov.f32 %f1,0f00000000; mov.u32 %r10,0;\n"
"$L: setp.ge.s32 %p4,%r10,%r1; @%p4 bra $S;\n"
" mad.lo.s32 %r11,%r5,%r1,%r10; mul.wide.s32 %rd4,%r11,4; cvta.to.global.u64 %rd5,%rd1; add.s64 %rd6,%rd5,%rd4; ld.global.f32 %f2,[%rd6];\n"
" mad.lo.s32 %r12,%r10,%r1,%r9; mul.wide.s32 %rd7,%r12,4; cvta.to.global.u64 %rd8,%rd2; add.s64 %rd9,%rd8,%rd7; ld.global.f32 %f3,[%rd9];\n"
" fma.rn.f32 %f1,%f2,%f3,%f1; add.s32 %r10,%r10,1; bra.uni $L;\n"
"$S: mad.lo.s32 %r13,%r5,%r1,%r9; mul.wide.s32 %rd10,%r13,4; cvta.to.global.u64 %rd11,%rd3; add.s64 %rd12,%rd11,%rd10; st.global.f32 [%rd12],%f1;\n"
"$E: ret; }\n";

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

int main(int argc, char**argv){
    int N = argc>1?atoi(argv[1]):2048;     /* GEMM size */
    int GITER = argc>2?atoi(argv[2]):20;    /* GEMM timed iters */
    int LITER = argc>3?atoi(argv[3]):2000;  /* launch-RTT iters */
    int AITER = argc>4?atoi(argv[4]):2000;  /* alloc-RTT iters */

    void *h=dlopen("libcuda.so.1",2); if(!h)h=dlopen("libcuda.so",2);
    if(!h){fprintf(stderr,"no libcuda\n");return 1;}
    CUresult(*cuInit)(unsigned)=dlsym(h,"cuInit");
    CUresult(*cuDeviceGet)(CUdevice*,int)=dlsym(h,"cuDeviceGet");
    CUresult(*cuDeviceGetName)(char*,int,CUdevice)=dlsym(h,"cuDeviceGetName");
    CUresult(*cuCtxCreate)(CUcontext*,unsigned,CUdevice)=dlsym(h,"cuCtxCreate_v2");
    CUresult(*cuModuleLoadData)(CUmodule*,const void*)=dlsym(h,"cuModuleLoadData");
    CUresult(*cuModuleGetFunction)(CUfunction*,CUmodule,const char*)=dlsym(h,"cuModuleGetFunction");
    CUresult(*cuMemAlloc)(CUdeviceptr*,size_t)=dlsym(h,"cuMemAlloc_v2");
    CUresult(*cuMemFree)(CUdeviceptr)=dlsym(h,"cuMemFree_v2");
    CUresult(*cuMemcpyHtoD)(CUdeviceptr,const void*,size_t)=dlsym(h,"cuMemcpyHtoD_v2");
    CUresult(*cuLaunchKernel)(CUfunction,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,CUstream,void**,void**)=dlsym(h,"cuLaunchKernel");
    CUresult(*cuCtxSynchronize)(void)=dlsym(h,"cuCtxSynchronize");

    CHK(cuInit(0),"cuInit");
    CUdevice dev; CHK(cuDeviceGet(&dev,0),"cuDeviceGet");
    char name[256]={0}; if(cuDeviceGetName)cuDeviceGetName(name,sizeof name,dev);
    CUcontext ctx; CHK(cuCtxCreate(&ctx,0,dev),"cuCtxCreate");
    CUmodule mod; CUfunction mm,noop;
    CHK(cuModuleLoadData(&mod,ptx),"moduleLoad");
    CHK(cuModuleGetFunction(&mm,mod,"matmul"),"getFn matmul");
    CHK(cuModuleGetFunction(&noop,mod,"noop"),"getFn noop");
    printf("device: %s\n", name);

    /* ---- Phase A: GEMM throughput ---- */
    size_t bytes=(size_t)N*N*sizeof(float);
    float *A=malloc(bytes),*B=malloc(bytes);
    for(size_t i=0;i<(size_t)N*N;i++){A[i]=(float)(i%17)*0.1f;B[i]=(float)(i%13)*0.2f;}
    CUdeviceptr dA,dB,dC;
    CHK(cuMemAlloc(&dA,bytes),"allocA"); CHK(cuMemAlloc(&dB,bytes),"allocB"); CHK(cuMemAlloc(&dC,bytes),"allocC");
    CHK(cuMemcpyHtoD(dA,A,bytes),"HtoDA"); CHK(cuMemcpyHtoD(dB,B,bytes),"HtoDB");
    int n=N; void*args[]={&dA,&dB,&dC,&n};
    unsigned g=(N+31)/32;
    for(int i=0;i<3;i++){ cuLaunchKernel(mm,g,g,1,32,32,1,0,NULL,args,NULL);} cuCtxSynchronize(); /*warmup*/
    double t0=now();
    for(int i=0;i<GITER;i++) CHK(cuLaunchKernel(mm,g,g,1,32,32,1,0,NULL,args,NULL),"gemm");
    CHK(cuCtxSynchronize(),"gemm sync");
    double tA=now()-t0;
    double gflop=2.0*N*N*N*GITER/1e9;
    printf("A throughput : %dx%d GEMM x%d  %.3fs  %.1f GFLOP/s\n",N,N,GITER,tA,gflop/tA);

    /* ---- Phase B: launch round-trip latency (sync each) ---- */
    for(int i=0;i<50;i++){cuLaunchKernel(noop,1,1,1,1,1,1,0,NULL,NULL,NULL);cuCtxSynchronize();}/*warmup*/
    t0=now();
    for(int i=0;i<LITER;i++){ CHK(cuLaunchKernel(noop,1,1,1,1,1,1,0,NULL,NULL,NULL),"noop"); CHK(cuCtxSynchronize(),"noop sync"); }
    double tB=now()-t0;
    printf("B launch RTT : %d launches  %.3fs  %.2f us/launch  (%.0f launches/s)\n",LITER,tB,tB/LITER*1e6,LITER/tB);

    /* ---- Phase C: alloc round-trip latency ---- */
    for(int i=0;i<50;i++){CUdeviceptr p;cuMemAlloc(&p,4096);cuMemFree(p);}/*warmup*/
    t0=now();
    for(int i=0;i<AITER;i++){ CUdeviceptr p; CHK(cuMemAlloc(&p,65536),"alloc"); CHK(cuMemFree(p),"free"); }
    double tC=now()-t0;
    printf("C alloc RTT  : %d alloc+free  %.3fs  %.2f us/pair\n",AITER,tC,tC/AITER*1e6);
    return 0;
}
