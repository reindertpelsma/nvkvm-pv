// conv2d.cu — 2D image convolution with a KxK kernel (image-processing path).
// Reports GFLOP/s. Emits METRIC + CHECK.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
#define K 5
__constant__ float kern[K*K];
__global__ void conv(const float*in,float*out,int W,int H){
    int x=blockIdx.x*blockDim.x+threadIdx.x, y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=W||y>=H)return;
    float acc=0;
    for(int ky=0;ky<K;ky++)for(int kx=0;kx<K;kx++){
        int ix=min(max(x+kx-K/2,0),W-1), iy=min(max(y+ky-K/2,0),H-1);
        acc+=in[iy*(size_t)W+ix]*kern[ky*K+kx];
    }
    out[y*(size_t)W+x]=acc;
}
int main(int argc,char**argv){
    int W=argc>1?atoi(argv[1]):8192, H=W, reps=argc>2?atoi(argv[2]):20;
    float*in,*out; size_t b=(size_t)W*H*4; CK(cudaMalloc(&in,b));CK(cudaMalloc(&out,b));
    float*h=(float*)malloc(b); for(size_t i=0;i<(size_t)W*H;i++)h[i]=(float)(i%256)/256.0f;
    CK(cudaMemcpy(in,h,b,cudaMemcpyHostToDevice));
    float hk[K*K]; for(int i=0;i<K*K;i++)hk[i]=1.0f/(K*K); CK(cudaMemcpyToSymbol(kern,hk,sizeof hk));
    dim3 bl(16,16), g((W+15)/16,(H+15)/16);
    conv<<<g,bl>>>(in,out,W,H); CK(cudaDeviceSynchronize());
    double t=now();
    for(int r=0;r<reps;r++) conv<<<g,bl>>>(in,out,W,H);
    CK(cudaDeviceSynchronize()); t=now()-t;
    float ho; CK(cudaMemcpy(&ho,out+(size_t)W*H/2,4,cudaMemcpyDeviceToHost));
    printf("METRIC conv2d_GFLOPs %.1f\n", 2.0*K*K*(double)W*H*reps/t/1e9);
    printf("CHECK %s\n", isfinite(ho)?"ok":"FAIL");
    return 0;
}
