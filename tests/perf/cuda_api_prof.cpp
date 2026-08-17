#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime_api.h>
static inline uint64_t rd(){ return __builtin_ia32_rdtsc(); }
#define REAL(f) ({ static decltype(&f) p=0; if(!p) p=(decltype(&f))dlsym(RTLD_NEXT,#f); p; })
/* per (kind 0..3) x (size bucket: 0=<4K,1=<64K,2=<1M,3=>=1M) */
static uint64_t mc_cnt[4][4], mc_cyc[4][4], mc_bytes[4][4];
static uint64_t la_cnt,la_cyc, ss_cnt,ss_cyc;
static int bucket(size_t n){ return n<4096?0:n<65536?1:n<1048576?2:3; }
extern "C" {
cudaError_t cudaMemcpyAsync(void*d,const void*s,size_t n,enum cudaMemcpyKind k,cudaStream_t st){
  int kk=(int)k&3,b=bucket(n); uint64_t t=rd(); cudaError_t r=REAL(cudaMemcpyAsync)(d,s,n,k,st);
  uint64_t c=rd()-t; mc_cnt[kk][b]++; mc_cyc[kk][b]+=c; mc_bytes[kk][b]+=n; return r; }
cudaError_t cudaMemcpy(void*d,const void*s,size_t n,enum cudaMemcpyKind k){
  int kk=(int)k&3,b=bucket(n); uint64_t t=rd(); cudaError_t r=REAL(cudaMemcpy)(d,s,n,k);
  uint64_t c=rd()-t; mc_cnt[kk][b]++; mc_cyc[kk][b]+=c; mc_bytes[kk][b]+=n; return r; }
cudaError_t cudaLaunchKernel(const void*f,dim3 gd,dim3 bd,void**a,size_t s,cudaStream_t st){
  uint64_t t=rd(); cudaError_t r=REAL(cudaLaunchKernel)(f,gd,bd,a,s,st); la_cyc+=rd()-t; la_cnt++; return r; }
cudaError_t cudaStreamSynchronize(cudaStream_t st){
  uint64_t t=rd(); cudaError_t r=REAL(cudaStreamSynchronize)(st); ss_cyc+=rd()-t; ss_cnt++; return r; }
}
__attribute__((destructor)) static void dump(){
  const char*kn[4]={"H2H","H2D","D2H","D2D"}; const char*bn[4]={"<4K","<64K","<1M",">=1M"};
  fprintf(stderr,"==== memcpy by kind x size (cycles, same TSC) ====\n");
  for(int k=0;k<4;k++)for(int b=0;b<4;b++) if(mc_cnt[k][b])
    fprintf(stderr,"PROF memcpy %s %-5s calls=%-7lu cyc/call=%-10lu totcyc=%-13lu avgB=%lu\n",
      kn[k],bn[b],mc_cnt[k][b],mc_cyc[k][b]/mc_cnt[k][b],mc_cyc[k][b],mc_bytes[k][b]/mc_cnt[k][b]);
  fprintf(stderr,"PROF launch calls=%lu cyc/call=%lu\nPROF strsync calls=%lu cyc/call=%lu\n",
    la_cnt,la_cnt?la_cyc/la_cnt:0, ss_cnt,ss_cnt?ss_cyc/ss_cnt:0);
}
