// sgemm_cublas.cu — real cuBLAS SGEMM throughput (the library everyone uses).
// Reports TFLOP/s. Emits METRIC + CHECK. Link: -lcublas
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cublas_v2.h>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
#define BK(x) do{cublasStatus_t s=(x); if(s){printf("cuBLAS %s:%d st=%d\n",__FILE__,__LINE__,s);return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
    int N=argc>1?atoi(argv[1]):4096; int it=argc>2?atoi(argv[2]):30;
    size_t b=(size_t)N*N*4; float*A,*B,*C;
    CK(cudaMalloc(&A,b));CK(cudaMalloc(&B,b));CK(cudaMalloc(&C,b));
    float*h=(float*)malloc(b); for(size_t i=0;i<(size_t)N*N;i++)h[i]=(float)((i%17)*0.1);
    CK(cudaMemcpy(A,h,b,cudaMemcpyHostToDevice)); CK(cudaMemcpy(B,h,b,cudaMemcpyHostToDevice));
    cublasHandle_t hd; BK(cublasCreate(&hd));
    float al=1,be=0;
    BK(cublasSgemm(hd,CUBLAS_OP_N,CUBLAS_OP_N,N,N,N,&al,A,N,B,N,&be,C,N)); CK(cudaDeviceSynchronize());
    double t=now();
    for(int k=0;k<it;k++) BK(cublasSgemm(hd,CUBLAS_OP_N,CUBLAS_OP_N,N,N,N,&al,A,N,B,N,&be,C,N));
    CK(cudaDeviceSynchronize()); t=now()-t;
    float hc; CK(cudaMemcpy(&hc,C,4,cudaMemcpyDeviceToHost));
    printf("METRIC sgemm_cublas_TFLOPs %.2f\n", 2.0*N*N*N*it/t/1e12);
    printf("CHECK %s\n", isfinite(hc)?"ok":"FAIL");
    return 0;
}
