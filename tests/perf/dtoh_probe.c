#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHK(x) do{CUresult r=(x); if(r){const char*s=0;cuGetErrorName(r,&s);fprintf(stderr,"%s=%d %s\n",#x,r,s?s:"");exit(1);}}while(0)
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(){
  CHK(cuInit(0)); CUdevice dev; CHK(cuDeviceGet(&dev,0));
  CUcontext ctx; CHK(cuCtxCreate(&ctx,0,dev));
  size_t sz=16<<20; CUdeviceptr d; CHK(cuMemAlloc(&d,sz));
  /* fill device with a known pattern via HtoD so we can verify DtoH */
  unsigned *src=malloc(sz); for(size_t i=0;i<sz/4;i++) src[i]=(unsigned)i*2654435761u;
  CHK(cuMemcpyHtoD(d,src,sz));
  void *fresh=malloc(sz);
  double t=now(); CHK(cuMemcpyDtoH(fresh,d,sz)); t=now()-t;
  printf("A cold DtoH:  %.3f GB/s (%.1f ms)\n",(double)sz/t/1e9,t*1e3);
  t=now(); for(int i=0;i<8;i++) CHK(cuMemcpyDtoH(fresh,d,sz)); t=(now()-t)/8;
  printf("B warm DtoH:  %.3f GB/s (%.1f ms)\n",(double)sz/t/1e9,t*1e3);
  /* correctness */
  size_t bad=0; for(size_t i=0;i<sz/4;i++) if(((unsigned*)fresh)[i]!=src[i]) bad++;
  printf("correctness: %s (%zu/%zu mismatched)\n", bad?"FAIL":"OK", bad, sz/4);
  /* HtoD bandwidth for comparison */
  t=now(); for(int i=0;i<8;i++) CHK(cuMemcpyHtoD(d,src,sz)); t=(now()-t)/8;
  printf("HtoD:         %.3f GB/s (%.1f ms)\n",(double)sz/t/1e9,t*1e3);
  printf("done\n"); return 0;
}
