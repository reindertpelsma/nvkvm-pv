/* Why is model-load HtoD slow? Separate: fresh-vs-reused source, anon-vs-file-backed. */
#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#define CHK(x) do{CUresult r=(x);if(r){const char*s=0;cuGetErrorName(r,&s);fprintf(stderr,"%s=%d %s\n",#x,r,s?s:"");exit(1);}}while(0)
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(){
  CHK(cuInit(0)); CUdevice dv; CHK(cuDeviceGet(&dv,0));
  CUcontext c; CHK(cuCtxCreate(&c,0,dv));
  size_t sz=32UL<<20; int N=8; CUdeviceptr d; CHK(cuMemAlloc(&d,sz));

  /* 1) anon, REUSE same source (warm) */
  void*a=malloc(sz); memset(a,1,sz);
  CHK(cuMemcpyHtoD(d,a,sz)); /* warm */
  double t=now(); for(int i=0;i<N;i++) CHK(cuMemcpyHtoD(d,a,sz)); t=now()-t;
  printf("1 anon REUSE:        %.2f GB/s\n",(double)sz*N/t/1e9);

  /* 2) anon, FRESH source each iter (cold every time, like distinct tensors) */
  t=now();
  for(int i=0;i<N;i++){ void*p=malloc(sz); memset(p,1,sz); CHK(cuMemcpyHtoD(d,p,sz)); free(p); }
  t=now()-t;
  printf("2 anon FRESH/iter:   %.2f GB/s\n",(double)sz*N/t/1e9);

  /* 3) file-backed mmap, FRESH map each iter (like llama.cpp gguf mmap upload) */
  const char*fn="/tmp/htod_dat"; int fd=open(fn,O_RDWR|O_CREAT,0644);
  if(ftruncate(fd,sz)!=0){perror("ftrunc");}
  { void*z=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); memset(z,2,sz); msync(z,sz,MS_SYNC); munmap(z,sz); }
  t=now();
  for(int i=0;i<N;i++){
    void*p=mmap(0,sz,PROT_READ,MAP_PRIVATE,fd,0);
    CHK(cuMemcpyHtoD(d,p,sz)); munmap(p,sz);
  }
  t=now()-t;
  printf("3 file mmap FRESH:   %.2f GB/s\n",(double)sz*N/t/1e9);
  close(fd); unlink(fn);
  printf("done\n"); return 0;
}
