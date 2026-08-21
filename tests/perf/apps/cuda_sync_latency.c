/* cuda_sync_latency.c — CPU->GPU->CPU round-trip LATENCY on the compute path.
 *
 * The compute parity numbers in this repo are all THROUGHPUT (tokens/s,
 * GFLOP/s, GB/s), which hide per-submission latency completely.  This measures
 * the thing glmark2 actually stresses: submit a trivial amount of work and wait
 * for it, over and over.  It is the CUDA twin of gl_finishrate.c.
 *
 * Driver API only, via dlopen -- no CUDA toolkit, no nvcc, no headers needed;
 * it runs against the staged libcuda.so.1 on either side.
 *
 * Build: gcc -O2 cuda_sync_latency.c -o cuda_sync_latency -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
typedef int (*fn_i)(void);
typedef int (*fn_ii)(int);
typedef int (*fn_pi)(void*,int);
typedef int (*fn_ppi)(void*,unsigned,int);
typedef int (*fn_msd)(unsigned long long,unsigned,size_t);
typedef int (*fn_msda)(unsigned long long,unsigned,size_t,void*);
typedef int (*fn_alloc)(unsigned long long*,size_t);
int main(int argc,char**argv){
    int N=argc>1?atoi(argv[1]):20000;
    void*h=dlopen("libcuda.so.1",RTLD_NOW);
    if(!h){printf("CHECK cuda_sync FAIL no-libcuda: %s\n",dlerror());return 1;}
    fn_ii   cuInit          = (fn_ii)  dlsym(h,"cuInit");
    fn_pi   cuDeviceGet     = (fn_pi)  dlsym(h,"cuDeviceGet");
    fn_ppi  cuCtxCreate     = (fn_ppi) dlsym(h,"cuCtxCreate_v2");
    fn_alloc cuMemAlloc     = (fn_alloc)dlsym(h,"cuMemAlloc_v2");
    fn_msd  cuMemsetD32     = (fn_msd) dlsym(h,"cuMemsetD32_v2");
    fn_msda cuMemsetD32Async= (fn_msda)dlsym(h,"cuMemsetD32Async");
    fn_i    cuCtxSynchronize= (fn_i)   dlsym(h,"cuCtxSynchronize");
    if(!cuInit||!cuDeviceGet||!cuCtxCreate||!cuMemAlloc||!cuMemsetD32Async||!cuCtxSynchronize){
        printf("CHECK cuda_sync FAIL missing-symbols\n");return 1;}
    int r=cuInit(0); if(r){printf("CHECK cuda_sync FAIL cuInit=%d\n",r);return 1;}
    int dev=0; r=cuDeviceGet(&dev,0); if(r){printf("CHECK cuda_sync FAIL cuDeviceGet=%d\n",r);return 1;}
    void*ctx=0; r=cuCtxCreate(&ctx,0,dev); if(r){printf("CHECK cuda_sync FAIL cuCtxCreate=%d\n",r);return 1;}
    unsigned long long dptr=0; r=cuMemAlloc(&dptr,4096);
    if(r){printf("CHECK cuda_sync FAIL cuMemAlloc=%d\n",r);return 1;}

    /* warm up */
    for(int i=0;i<2000;i++){cuMemsetD32Async(dptr,i,1,0);cuCtxSynchronize();}

    double t0=now();
    for(int i=0;i<N;i++){cuMemsetD32Async(dptr,i,1,0);cuCtxSynchronize();}
    double dt=now()-t0;
    printf("METRIC cuda_sync_us %.2f\nMETRIC cuda_sync_per_s %.0f\n",dt/N*1e6,N/dt);

    /* sync with NOTHING queued: pure "is the channel idle" check */
    cuCtxSynchronize();
    t0=now();
    for(int i=0;i<N;i++) cuCtxSynchronize();
    dt=now()-t0;
    printf("METRIC cuda_syncidle_us %.2f\n",dt/N*1e6);
    printf("CHECK cuda_sync ok\n");
    return 0;
}
