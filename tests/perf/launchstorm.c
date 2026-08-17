/* Launch-storm: separate pipelined submission (A) from per-launch round-trip (B). */
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHK(x) do{CUresult r=(x);if(r){const char*s=0;cuGetErrorName(r,&s);fprintf(stderr,"%s:%d %s=%d %s\n",__FILE__,__LINE__,#x,r,s?s:"");exit(1);}}while(0)
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/* empty kernel */
static const char* PTX =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry nop(){\n ret;\n}\n";
int main(){
  CHK(cuInit(0)); CUdevice d; CHK(cuDeviceGet(&d,0));
  CUcontext c; CHK(cuCtxCreate(&c,CU_CTX_SCHED_SPIN,d));
  CUmodule m; CHK(cuModuleLoadData(&m,PTX));
  CUfunction f; CHK(cuModuleGetFunction(&f,m,"nop"));
  /* warm */
  for(int i=0;i<100;i++) cuLaunchKernel(f,1,1,1,1,1,1,0,0,0,0);
  CHK(cuCtxSynchronize());

  int MA=200000;
  double t=now();
  for(int i=0;i<MA;i++) CHK(cuLaunchKernel(f,1,1,1,1,1,1,0,0,0,0));
  CHK(cuCtxSynchronize());
  t=now()-t;
  printf("A pipelined submit: %.0f launches/s  (%.2f us/launch)\n", MA/t, t/MA*1e6);

  int MB=20000;
  t=now();
  for(int i=0;i<MB;i++){ CHK(cuLaunchKernel(f,1,1,1,1,1,1,0,0,0,0)); CHK(cuCtxSynchronize()); }
  t=now()-t;
  printf("B launch+sync RT:   %.0f rt/s        (%.2f us/round-trip)\n", MB/t, t/MB*1e6);

  /* C: just sync repeatedly with no work (pure completion-poll/notify cost) */
  CHK(cuCtxSynchronize());
  int MC=50000;
  t=now();
  for(int i=0;i<MC;i++) CHK(cuCtxSynchronize());
  t=now()-t;
  printf("C empty sync:       %.0f sync/s       (%.2f us/sync)\n", MC/t, t/MC*1e6);
  printf("done\n"); return 0;
}
