// memcpy2d.cu — isolate cudaMemcpy2D (pitched 2D copy), the call NVENC's frame
// upload spins in inside the guest.  Emits METRIC/CHECK like the other apps.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#define CK(x) do{cudaError_t e=(x); if(e){printf("CUDA %s:%d %s\n",__FILE__,__LINE__,cudaGetErrorString(e));return 1;}}while(0)
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
    int W = argc>1?atoi(argv[1]):1280;      // bytes per row
    int H = argc>2?atoi(argv[2]):720;
    int it= argc>3?atoi(argv[3]):50;
    void *d; size_t pitch;
    printf("[m2d] cudaMallocPitch %dx%d\n", W, H); fflush(stdout);
    CK(cudaMallocPitch(&d,&pitch,W,H));
    printf("[m2d] pitch=%zu\n", pitch); fflush(stdout);
    unsigned char *h=(unsigned char*)malloc((size_t)W*H);
    for(size_t i=0;i<(size_t)W*H;i++) h[i]=(unsigned char)(i*7+3);
    printf("[m2d] first HtoD 2D...\n"); fflush(stdout);
    CK(cudaMemcpy2D(d,pitch,h,W,W,H,cudaMemcpyHostToDevice));
    CK(cudaDeviceSynchronize());
    printf("[m2d] first HtoD 2D done\n"); fflush(stdout);
    double t=now();
    for(int k=0;k<it;k++) CK(cudaMemcpy2D(d,pitch,h,W,W,H,cudaMemcpyHostToDevice));
    CK(cudaDeviceSynchronize());
    t=now()-t;
    unsigned char *b=(unsigned char*)malloc((size_t)W*H); memset(b,0,(size_t)W*H);
    CK(cudaMemcpy2D(b,W,d,pitch,W,H,cudaMemcpyDeviceToHost));
    int ok = memcmp(h,b,(size_t)W*H)==0;
    printf("METRIC memcpy2d_GBs %.2f\n", (double)W*H*it/t/1e9);
    printf("CHECK memcpy2d_GBs %s\n", ok?"ok":"FAIL");
    return 0;
}
