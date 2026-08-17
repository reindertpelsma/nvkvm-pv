// stream_triad.cu — BabelStream-style memory-bandwidth benchmark (triad).
// a[i] = b[i] + s*c[i]  (2 reads + 1 write).  Reports sustained GB/s.
// Emits: METRIC stream_triad_GBs <v> ; CHECK <ok|FAIL>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__global__ void triad(float*a,const float*b,const float*c,float s,size_t n){
    size_t i=blockIdx.x*(size_t)blockDim.x+threadIdx.x;
    if(i<n) a[i]=b[i]+s*c[i];
}
int main(int argc,char**argv){
    size_t n = (argc>1?atoll(argv[1]):(64UL<<20)); // 64M floats/array = 256MB
    int it = argc>2?atoi(argv[2]):50;
    float *a,*b,*c; CK(cudaMalloc(&a,n*4));CK(cudaMalloc(&b,n*4));CK(cudaMalloc(&c,n*4));
    CK(cudaMemset(b,0,n*4));CK(cudaMemset(c,0,n*4));
    // init b=1,c=2 via a quick kernel-free memset trick: fill on host then copy.
    float *hb=(float*)malloc(n*4); for(size_t i=0;i<n;i++)hb[i]=1.0f; CK(cudaMemcpy(b,hb,n*4,cudaMemcpyHostToDevice));
    for(size_t i=0;i<n;i++)hb[i]=2.0f; CK(cudaMemcpy(c,hb,n*4,cudaMemcpyHostToDevice));
    int bs=256, gs=(n+bs-1)/bs;
    triad<<<gs,bs>>>(a,b,c,3.0f,n); CK(cudaDeviceSynchronize()); // warm
    double t=now();
    for(int k=0;k<it;k++) triad<<<gs,bs>>>(a,b,c,3.0f,n);
    CK(cudaDeviceSynchronize());
    t=now()-t;
    double gbs = 3.0*n*4.0*it/t/1e9;
    // correctness: a = 1 + 3*2 = 7
    float ha; CK(cudaMemcpy(&ha,a,4,cudaMemcpyDeviceToHost));
    printf("METRIC stream_triad_GBs %.1f\n", gbs);
    printf("CHECK %s\n", (ha==7.0f)?"ok":"FAIL");
    return 0;
}
