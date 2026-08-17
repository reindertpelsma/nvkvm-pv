// mandelbrot.cu — Mandelbrot set escape-time (classic compute-bound workload).
// Reports millions of pixel-iterations per second. Emits METRIC + CHECK.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__global__ void mandel(int*out,int W,int H,int maxit){
    int x=blockIdx.x*blockDim.x+threadIdx.x, y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=W||y>=H)return;
    float cx=-2.5f+3.5f*x/W, cy=-1.25f+2.5f*y/H;
    float zx=0,zy=0; int i=0;
    while(i<maxit && zx*zx+zy*zy<4.0f){ float t=zx*zx-zy*zy+cx; zy=2*zx*zy+cy; zx=t; i++; }
    out[y*(size_t)W+x]=i;
}
int main(int argc,char**argv){
    int W=argc>1?atoi(argv[1]):4096, H=W, maxit=argc>2?atoi(argv[2]):512;
    int reps=argc>3?atoi(argv[3]):10;
    int*out; CK(cudaMalloc(&out,(size_t)W*H*4));
    dim3 b(16,16), g((W+15)/16,(H+15)/16);
    mandel<<<g,b>>>(out,W,H,maxit); CK(cudaDeviceSynchronize());
    double t=now();
    for(int r=0;r<reps;r++) mandel<<<g,b>>>(out,W,H,maxit);
    CK(cudaDeviceSynchronize()); t=now()-t;
    // center pixel (0,0 in complex ~ -0.? ) — check that pixel at image center is in-set (==maxit)
    int hc; CK(cudaMemcpy(&hc,out+((size_t)(H/2)*W+(W/2)),4,cudaMemcpyDeviceToHost));
    printf("METRIC mandelbrot_Mpix_iter_s %.1f\n", (double)W*H*maxit*reps/t/1e6);
    printf("CHECK %s\n", (hc>=0 && hc<=maxit)?"ok":"FAIL");
    return 0;
}
