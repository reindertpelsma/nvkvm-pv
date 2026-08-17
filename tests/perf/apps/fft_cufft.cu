// fft_cufft.cu — cuFFT batched 1D C2C FFT throughput (signal processing).
// Reports GFLOP/s (5*N*log2(N)*batch model). Emits METRIC + CHECK. Link: -lcufft
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cufft.h>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
    int N=argc>1?atoi(argv[1]):4096; int batch=argc>2?atoi(argv[2]):4096; int it=argc>3?atoi(argv[3]):20;
    size_t tot=(size_t)N*batch;
    cufftComplex*d; CK(cudaMalloc(&d,tot*sizeof(cufftComplex)));
    cufftComplex*h=(cufftComplex*)malloc(tot*sizeof(cufftComplex));
    for(size_t i=0;i<tot;i++){h[i].x=sinf(i*0.001f);h[i].y=0;}
    CK(cudaMemcpy(d,h,tot*sizeof(cufftComplex),cudaMemcpyHostToDevice));
    cufftHandle plan; if(cufftPlan1d(&plan,N,CUFFT_C2C,batch)!=CUFFT_SUCCESS){printf("cufftPlan fail\n");return 1;}
    // --- correctness: fresh buffer, forward then inverse must recover input*N ---
    cufftComplex*chk; CK(cudaMalloc(&chk,tot*sizeof(cufftComplex)));
    CK(cudaMemcpy(chk,h,tot*sizeof(cufftComplex),cudaMemcpyHostToDevice));
    cufftExecC2C(plan,chk,chk,CUFFT_FORWARD); cufftExecC2C(plan,chk,chk,CUFFT_INVERSE);
    CK(cudaDeviceSynchronize());
    cufftComplex rt; CK(cudaMemcpy(&rt,chk+1,sizeof(cufftComplex),cudaMemcpyDeviceToHost));
    float want=h[1].x*N;          // cuFFT inverse is unnormalized -> scaled by N
    int ok = isfinite(rt.x) && fabsf(rt.x-want) < 1e-2f*fabsf(want)+1e-2f;
    // --- throughput: repeated in-place forward transforms (real GPU work) ---
    cufftExecC2C(plan,d,d,CUFFT_FORWARD); CK(cudaDeviceSynchronize());
    double t=now();
    for(int k=0;k<it;k++) cufftExecC2C(plan,d,d,CUFFT_FORWARD);
    CK(cudaDeviceSynchronize()); t=now()-t;
    double gflop=5.0*N*log2((double)N)*batch*it/1e9;
    printf("METRIC fft_cufft_GFLOPs %.1f\n", gflop/t);
    printf("CHECK %s\n", ok?"ok":"FAIL");
    return 0;
}
