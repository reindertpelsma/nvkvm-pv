/*
 * cuda_micro.c — per-subsystem CUDA driver-API microbenchmarks.
 *
 * Each subtest isolates ONE behaviour so we can compare host vs guest and see
 * exactly which forwarded path carries the overhead (instead of inferring from
 * a tangled LLM decode).  Same binary runs host and guest; dlopen libcuda.
 *
 *   1 rm_control   : cuMemGetInfo loop            — RM_CONTROL ioctl rate
 *   2 alloc        : cuMemAlloc+cuMemFree loop    — RM alloc/free/map path
 *   3 bandwidth    : cuMemcpyHtoD/DtoH large      — bulk DMA (direct memory)
 *   4 launch_sync  : noop kernel + cuCtxSynchronize— doorbell + fence/completion
 *   5 uvm_alloc    : cuMemAllocManaged + touch     — UVM alloc path
 *   6 uvm_migrate  : GPU-write then CPU-write loop — UVM CPU<->GPU migration thrash
 *
 * Build: gcc -O2 -o cuda_micro cuda_micro.c -ldl
 * Use:   ./cuda_micro [rm_iters alloc_iters bw_mb launch_iters uvm_iters mig_iters]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef void *CUstream;
typedef unsigned long long CUdeviceptr;

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuMemGetInfo)(size_t *, size_t *);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  CUstream, void **, void **);
static CUresult (*cuCtxSynchronize)(void);
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
typedef void *CUevent;
static CUresult (*cuEventCreate)(CUevent *, unsigned);
static CUresult (*cuEventRecord)(CUevent, CUstream);
static CUresult (*cuEventSynchronize)(CUevent);
static CUresult (*cuMemHostAlloc)(void **, size_t, unsigned);
static CUresult (*cuMemFreeHost)(void *);

#define CHK(call) do { CUresult _r=(call); if(_r){fprintf(stderr,#call" failed: %d\n",_r); return 1;} } while(0)
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

/* buf[i]=i over a grid; faults the managed range onto the GPU. */
static const char ptx_wr[] =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry wr(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<6>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3; st.global.u32 [%rd4],%r1;\n"
"END: ret;\n}\n"
".visible .entry noop(){ ret; }\n";

int main(int argc, char **argv)
{
	int rm_iters     = argc>1?atoi(argv[1]):20000;
	int alloc_iters  = argc>2?atoi(argv[2]):5000;
	int bw_mb        = argc>3?atoi(argv[3]):64;
	int launch_iters = argc>4?atoi(argv[4]):5000;
	int uvm_iters    = argc>5?atoi(argv[5]):2000;
	int mig_iters    = argc>6?atoi(argv[6]):500;
	int poll_iters   = argc>7?atoi(argv[7]):5000;

	setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: survive a crash mid-suite */
	void *h = dlopen("libcuda.so.1", RTLD_NOW);
	if(!h){ fprintf(stderr,"no libcuda\n"); return 1; }
#define SYM(n) *(void**)(&n)=dlsym(h,#n"_v2"); if(!n) *(void**)(&n)=dlsym(h,#n)
	SYM(cuInit); SYM(cuDeviceGet); SYM(cuCtxCreate); SYM(cuMemGetInfo);
	SYM(cuMemAlloc); SYM(cuMemFree); SYM(cuMemcpyHtoD); SYM(cuMemcpyDtoH);
	SYM(cuMemAllocManaged);
	*(void**)(&cuModuleLoadData)=dlsym(h,"cuModuleLoadData");
	*(void**)(&cuModuleGetFunction)=dlsym(h,"cuModuleGetFunction");
	*(void**)(&cuLaunchKernel)=dlsym(h,"cuLaunchKernel");
	*(void**)(&cuCtxSynchronize)=dlsym(h,"cuCtxSynchronize");
	*(void**)(&cuEventCreate)=dlsym(h,"cuEventCreate");
	*(void**)(&cuEventRecord)=dlsym(h,"cuEventRecord");
	*(void**)(&cuEventSynchronize)=dlsym(h,"cuEventSynchronize");
	*(void**)(&cuMemHostAlloc)=dlsym(h,"cuMemHostAlloc");
	*(void**)(&cuMemFreeHost)=dlsym(h,"cuMemFreeHost");

	CUdevice dev; CUcontext ctx;
	CHK(cuInit(0)); CHK(cuDeviceGet(&dev,0)); CHK(cuCtxCreate(&ctx,0,dev));
	CUmodule mod; CUfunction noop, wr;
	CHK(cuModuleLoadData(&mod, ptx_wr));
	CHK(cuModuleGetFunction(&noop, mod, "noop"));
	CHK(cuModuleGetFunction(&wr, mod, "wr"));
	double t;

	/* ---- 1: RM control ioctl rate ---- */
	{ size_t fr,to; for(int i=0;i<100;i++) cuMemGetInfo(&fr,&to);
	  t=now(); for(int i=0;i<rm_iters;i++) cuMemGetInfo(&fr,&to); t=now()-t;
	  printf("1 rm_control  : %d cuMemGetInfo   %.2f us/call\n", rm_iters, t/rm_iters*1e6); }

	/* ---- 2: alloc/free ---- */
	{ for(int i=0;i<50;i++){CUdeviceptr p; cuMemAlloc(&p,65536); cuMemFree(p);}
	  t=now(); for(int i=0;i<alloc_iters;i++){CUdeviceptr p; CHK(cuMemAlloc(&p,65536)); CHK(cuMemFree(p));} t=now()-t;
	  printf("2 alloc       : %d alloc+free     %.2f us/pair\n", alloc_iters, t/alloc_iters*1e6); }

	/* ---- 3: bulk DMA bandwidth + DtoH correctness ---- */
	{ size_t sz=(size_t)bw_mb<<20; CUdeviceptr d; unsigned *hb=malloc(sz), *vb=malloc(sz);
	  CHK(cuMemAlloc(&d,sz));
	  for(size_t i=0;i<sz/4;i++) hb[i]=(unsigned)(i*2654435761u);  /* pattern */
	  cuMemcpyHtoD(d,hb,sz); /* warm */
	  int M=8; t=now(); for(int i=0;i<M;i++) CHK(cuMemcpyHtoD(d,hb,sz)); t=now()-t;
	  printf("3 bandwidth   : HtoD %d MB x%d     %.1f GB/s", bw_mb, M, (double)sz*M/t/1e9);
	  memset(vb,0,sz);
	  t=now(); for(int i=0;i<M;i++) CHK(cuMemcpyDtoH(vb,d,sz)); t=now()-t;
	  printf("   DtoH %.1f GB/s", (double)sz*M/t/1e9);
	  size_t bad=0; for(size_t i=0;i<sz/4;i++) if(vb[i]!=hb[i]) bad++;
	  printf("   DtoH correctness: %s (%zu/%zu mismatched)\n", bad?"FAIL":"OK", bad, sz/4);
	  /* 3b: PINNED host buffer — copy-engine path, no UVM pageable per-page validate */
	  if(cuMemHostAlloc){
	    void *pin=NULL; CUresult pr=cuMemHostAlloc(&pin,sz,0);
	    if(pr==0 && pin){
	      cuMemcpyDtoH(pin,d,sz); /* warm */
	      t=now(); for(int i=0;i<M;i++) CHK(cuMemcpyDtoH(pin,d,sz)); t=now()-t;
	      printf("3b pinned DtoH: %.1f GB/s (pinned host buf, copy-engine path)\n", (double)sz*M/t/1e9);
	      cuMemFreeHost(pin);
	    } else printf("3b pinned DtoH: cuMemHostAlloc failed (err=%d)\n", pr);
	  } else printf("3b pinned DtoH: cuMemHostAlloc symbol missing\n");
	  cuMemFree(d); free(hb); free(vb); }

	/* ---- 4: launch + sync (doorbell + completion fence) ---- */
	{ for(int i=0;i<50;i++){cuLaunchKernel(noop,1,1,1,1,1,1,0,0,0,0); cuCtxSynchronize();}
	  t=now(); for(int i=0;i<launch_iters;i++){ CHK(cuLaunchKernel(noop,1,1,1,1,1,1,0,0,0,0)); CHK(cuCtxSynchronize()); } t=now()-t;
	  printf("4 launch_sync : %d launch+sync    %.2f us/launch\n", launch_iters, t/launch_iters*1e6); }

	/* ---- 5: UVM managed alloc + CPU touch ---- */
	int uvm_ok = 0;
	if(cuMemAllocManaged){
	  size_t sz=1<<20; CUdeviceptr probe=0;
	  CUresult mr = cuMemAllocManaged(&probe, sz, 1);
	  if(mr || !probe){
	    printf("5 uvm_alloc   : cuMemAllocManaged FAILED (err=%d ptr=0x%llx) — skipping UVM\n",
		   mr, (unsigned long long)probe);
	  } else {
	    cuMemFree(probe);
	    uvm_ok = 1;
	    for(int i=0;i<20;i++){CUdeviceptr p=0; if(cuMemAllocManaged(&p,sz,1)||!p)break; memset((void*)(uintptr_t)p,1,4096); cuMemFree(p);}
	    t=now(); for(int i=0;i<uvm_iters;i++){CUdeviceptr p=0; CHK(cuMemAllocManaged(&p,sz,1)); if(!p){fprintf(stderr,"null managed ptr\n");return 1;} memset((void*)(uintptr_t)p,1,4096); CHK(cuMemFree(p));} t=now()-t;
	    printf("5 uvm_alloc   : %d managed+touch   %.2f us/op\n", uvm_iters, t/uvm_iters*1e6);
	  }
	} else printf("5 uvm_alloc   : (cuMemAllocManaged unavailable)\n");

	/* ---- 6: UVM migration thrash (GPU-write then CPU-write each iter) ---- */
	if(uvm_ok){
	  size_t sz=4<<20; unsigned n=sz/4; CUdeviceptr p=0;
	  CHK(cuMemAllocManaged(&p,sz,1)); if(!p){fprintf(stderr,"null managed ptr\n");return 1;}
	  unsigned blk=256, grid=(n+blk-1)/blk; void *args[]={&p,&n};
	  for(int i=0;i<5;i++){ cuLaunchKernel(wr,grid,1,1,blk,1,1,0,0,args,0); cuCtxSynchronize(); memset((void*)(uintptr_t)p,2,sz);}
	  t=now();
	  for(int i=0;i<mig_iters;i++){
		CHK(cuLaunchKernel(wr,grid,1,1,blk,1,1,0,0,args,0)); /* pages -> GPU */
		CHK(cuCtxSynchronize());
		memset((void*)(uintptr_t)p,2,sz);                    /* pages -> CPU */
	  }
	  t=now()-t;
	  printf("6 uvm_migrate : %d GPU<->CPU cyc   %.2f us/cycle (%d MB each way)\n",
		 mig_iters, t/mig_iters*1e6, (int)(sz>>20));
	  cuMemFree(p);
	} else printf("6 uvm_migrate : (cuMemAllocManaged unavailable)\n");

	/* ---- 7: poll-heavy — blocking-sync event wait (poll on the event fd) ----
	 * CU_EVENT_BLOCKING_SYNC makes cuEventSynchronize block the thread on the
	 * OS event (poll/ppoll) instead of spinning, isolating the completion-wait
	 * relay path that dominated DtoH (~92ms/poll). */
	if(cuEventCreate && cuEventRecord && cuEventSynchronize){
	  CUevent ev;
	  CHK(cuEventCreate(&ev, 0x1 /*CU_EVENT_BLOCKING_SYNC*/));
	  for(int i=0;i<50;i++){ cuLaunchKernel(noop,1,1,1,1,1,1,0,0,0,0); cuEventRecord(ev,0); cuEventSynchronize(ev); }
	  t=now();
	  for(int i=0;i<poll_iters;i++){
		CHK(cuLaunchKernel(noop,1,1,1,1,1,1,0,0,0,0));
		CHK(cuEventRecord(ev,0));
		CHK(cuEventSynchronize(ev));   /* blocks via poll() on the event fd */
	  }
	  t=now()-t;
	  printf("7 poll_sync   : %d blk-event sync %.2f us/sync\n", poll_iters, t/poll_iters*1e6);
	} else printf("7 poll_sync   : (cuEvent* unavailable)\n");

	return 0;
}
