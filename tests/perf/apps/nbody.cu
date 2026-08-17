// nbody.cu — classic all-pairs N-body gravitation (compute-bound).
// Reports GFLOP/s (counting ~20 flop per pairwise interaction).
// Emits: METRIC nbody_GFLOPs <v> ; CHECK <ok|FAIL>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__global__ void bodyforce(const float4*p,float4*v,float dt,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=n)return;
    float fx=0,fy=0,fz=0; float4 pi=p[i];
    for(int j=0;j<n;j++){
        float dx=p[j].x-pi.x, dy=p[j].y-pi.y, dz=p[j].z-pi.z;
        float d2=dx*dx+dy*dy+dz*dz+1e-3f;
        float inv=rsqrtf(d2); float inv3=inv*inv*inv;
        fx+=dx*inv3; fy+=dy*inv3; fz+=dz*inv3;
    }
    v[i].x+=dt*fx; v[i].y+=dt*fy; v[i].z+=dt*fz;
}
int main(int argc,char**argv){
    int n = argc>1?atoi(argv[1]):30720;
    int steps = argc>2?atoi(argv[2]):10;
    float4 *p,*v; CK(cudaMalloc(&p,n*sizeof(float4)));CK(cudaMalloc(&v,n*sizeof(float4)));
    float4 *hp=(float4*)malloc(n*sizeof(float4));
    for(int i=0;i<n;i++){hp[i].x=(float)(i%1000)*0.01f;hp[i].y=(float)(i%777)*0.02f;hp[i].z=(float)(i%555)*0.03f;hp[i].w=1;}
    CK(cudaMemcpy(p,hp,n*sizeof(float4),cudaMemcpyHostToDevice));
    CK(cudaMemset(v,0,n*sizeof(float4)));
    int bs=256, gs=(n+bs-1)/bs;
    bodyforce<<<gs,bs>>>(p,v,0.01f,n); CK(cudaDeviceSynchronize()); // warm
    double t=now();
    for(int s=0;s<steps;s++) bodyforce<<<gs,bs>>>(p,v,0.01f,n);
    CK(cudaDeviceSynchronize()); t=now()-t;
    double gflop = 20.0*(double)n*n*steps/1e9;
    float4 hv; CK(cudaMemcpy(&hv,v,sizeof(float4),cudaMemcpyDeviceToHost));
    printf("METRIC nbody_GFLOPs %.1f\n", gflop/t);
    printf("CHECK %s\n", isfinite(hv.x)?"ok":"FAIL");
    return 0;
}
