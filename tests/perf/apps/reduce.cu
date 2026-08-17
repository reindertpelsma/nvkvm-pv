// reduce.cu — large parallel sum reduction (memory-bound throughput).
// Reports effective GB/s. Emits METRIC + CHECK (sum of N ones == N).
#include <cstdio>
#include <cstdlib>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__global__ void reduce(const float*in,double*out,size_t n){
    __shared__ double s[256];
    size_t i=blockIdx.x*(size_t)blockDim.x*8+threadIdx.x;
    double sum=0;
    for(int k=0;k<8;k++){ size_t j=i+(size_t)k*blockDim.x; if(j<n) sum+=in[j]; }
    s[threadIdx.x]=sum; __syncthreads();
    for(int o=blockDim.x/2;o>0;o>>=1){ if(threadIdx.x<o) s[threadIdx.x]+=s[threadIdx.x+o]; __syncthreads(); }
    if(threadIdx.x==0) atomicAdd(out,s[0]);
}
int main(int argc,char**argv){
    size_t n=argc>1?atoll(argv[1]):(128UL<<20); int it=argc>2?atoi(argv[2]):50;
    float*in; double*out; CK(cudaMalloc(&in,n*4)); CK(cudaMalloc(&out,8));
    float*h=(float*)malloc(n*4); for(size_t i=0;i<n;i++)h[i]=1.0f; CK(cudaMemcpy(in,h,n*4,cudaMemcpyHostToDevice));
    int bs=256; size_t gs=(n+(size_t)bs*8-1)/((size_t)bs*8);
    CK(cudaMemset(out,0,8)); reduce<<<gs,bs>>>(in,out,n); CK(cudaDeviceSynchronize());
    double t=now();
    for(int k=0;k<it;k++){ CK(cudaMemset(out,0,8)); reduce<<<gs,bs>>>(in,out,n); }
    CK(cudaDeviceSynchronize()); t=now()-t;
    double ho; CK(cudaMemcpy(&ho,out,8,cudaMemcpyDeviceToHost));
    printf("METRIC reduce_GBs %.1f\n", (double)n*4*it/t/1e9);
    printf("CHECK %s\n", (ho==(double)n)?"ok":"FAIL");
    return 0;
}
