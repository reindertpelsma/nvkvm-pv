/*
 * gpu_holder.c — guest-side victim for the Phase-4 cross-process PoC.
 *
 * Creates a CUDA context + allocates GPU memory, then sleeps, holding the RM
 * objects (and thus the nvkvm stub + its TYPE_ALL-shared handles) alive so a
 * separate host process (poc_cross_proc_dup) can attempt to dup them.
 *
 * Build (in guest): gcc -O0 -o /tmp/holder gpu_holder.c -ldl
 * Run:   LD_LIBRARY_PATH=/usr/local/nvidia-guest/lib /tmp/holder 120
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
#define CHK(c,m) do{CUresult r=(c); if(r){fprintf(stderr,m" failed: %d\n",r); return 1;}}while(0)

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 120;
	void *h = dlopen("libcuda.so.1", 2);
	if (!h) h = dlopen("libcuda.so", 2);
	if (!h) { fprintf(stderr, "no libcuda\n"); return 1; }
	CUresult (*cuInit)(unsigned)                            = dlsym(h, "cuInit");
	CUresult (*cuDeviceGet)(CUdevice*, int)                 = dlsym(h, "cuDeviceGet");
	CUresult (*cuCtxCreate)(CUcontext*, unsigned, CUdevice) = dlsym(h, "cuCtxCreate_v2");
	CUresult (*cuMemAlloc)(CUdeviceptr*, size_t)            = dlsym(h, "cuMemAlloc_v2");

	CHK(cuInit(0), "cuInit");
	CUdevice dev; CHK(cuDeviceGet(&dev, 0), "cuDeviceGet");
	CUcontext ctx; CHK(cuCtxCreate(&ctx, 0, dev), "cuCtxCreate");
	CUdeviceptr p; CHK(cuMemAlloc(&p, 16 * 1024 * 1024), "cuMemAlloc");
	fprintf(stderr, "holder: ctx + 16MB allocated, holding for %ds (pid %d)\n",
		secs, getpid());
	fflush(stderr);
	sleep(secs);
	return 0;
}
