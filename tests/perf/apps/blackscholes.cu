// blackscholes.cu — European option pricing (quantitative finance on GPU).
// Reports millions of options priced per second. Emits METRIC + CHECK.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
__device__ float cnd(float d){
    const float A1=0.31938153f,A2=-0.356563782f,A3=1.781477937f,A4=-1.821255978f,A5=1.330274429f;
    float k=1.0f/(1.0f+0.2316419f*fabsf(d));
    float c=1.0f/sqrtf(2.0f*3.14159265f)*expf(-0.5f*d*d)*(k*(A1+k*(A2+k*(A3+k*(A4+k*A5)))));
    return d>0?1.0f-c:c;
}
__global__ void bs(float*call,const float*S,const float*X,const float*T,float r,float v,int n){
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=n)return;
    float s=S[i],x=X[i],t=T[i];
    float d1=(logf(s/x)+(r+0.5f*v*v)*t)/(v*sqrtf(t));
    float d2=d1-v*sqrtf(t);
    call[i]=s*cnd(d1)-x*expf(-r*t)*cnd(d2);
}
int main(int argc,char**argv){
    int n=argc>1?atoi(argv[1]):(8<<20); int it=argc>2?atoi(argv[2]):50;
    float *S,*X,*T,*C; size_t b=n*4;
    CK(cudaMalloc(&S,b));CK(cudaMalloc(&X,b));CK(cudaMalloc(&T,b));CK(cudaMalloc(&C,b));
    float*h=(float*)malloc(b);
    for(int i=0;i<n;i++)h[i]=5+ (i%100); CK(cudaMemcpy(S,h,b,cudaMemcpyHostToDevice));
    for(int i=0;i<n;i++)h[i]=10+(i%90);  CK(cudaMemcpy(X,h,b,cudaMemcpyHostToDevice));
    for(int i=0;i<n;i++)h[i]=0.25f+(i%10)*0.1f; CK(cudaMemcpy(T,h,b,cudaMemcpyHostToDevice));
    int bs_=256, gs=(n+bs_-1)/bs_;
    bs<<<gs,bs_>>>(C,S,X,T,0.02f,0.30f,n); CK(cudaDeviceSynchronize());
    double t=now();
    for(int k=0;k<it;k++) bs<<<gs,bs_>>>(C,S,X,T,0.02f,0.30f,n);
    CK(cudaDeviceSynchronize()); t=now()-t;
    float hc; CK(cudaMemcpy(&hc,C,4,cudaMemcpyDeviceToHost));
    printf("METRIC blackscholes_Mopts %.1f\n", (double)n*it/t/1e6);
    printf("CHECK %s\n", isfinite(hc)?"ok":"FAIL");
    return 0;
}
