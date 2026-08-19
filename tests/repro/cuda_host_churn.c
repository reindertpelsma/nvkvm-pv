/* cuda_host_churn.c — does the map/free/realloc trigger corrupt CUDA too?
 *
 * The silent-corruption bug (docs/reference/correctness.md) was found through
 * OpenCL, but its cause is in a GENERIC guest path: nvkvm_cpu_page_migrate()
 * caches "already mapped?" on the guest virtual address alone.  Nothing about
 * that is OpenCL, so before claiming "CUDA is validated, OpenCL is broken" the
 * same trigger has to be run through CUDA.  This does that:
 *
 *   churn: allocate pinned host memory, WRITE to it, free it  (repeat)
 *   then:  allocate again -- likely landing on the same VA -- write a pattern,
 *          and have the GPU read it back via HtoD + DtoH.
 *
 * If the GPU sees stale or zero host memory the round-trip returns wrong data,
 * exactly as the OpenCL kernel read an all-zero input.
 *
 * dlopen's libcuda, so no CUDA toolkit is needed to build:
 *   cc -O2 -o cudachurn cuda_host_churn.c -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuMemHostAlloc)(void **, size_t, unsigned);
static CUresult (*cuMemFreeHost)(void *);
static CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*cuCtxSynchronize)(void);

#define SYM(n) do { *(void **)&n = dlsym(h, #n); \
	if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; } } while (0)

int main(int argc, char **argv)
{
	int churn = argc > 1 ? atoi(argv[1]) : 3;
	size_t n = (size_t)1 << (argc > 2 ? atoi(argv[2]) : 20);
	size_t bytes = n * sizeof(float);

	void *h = dlopen("libcuda.so.1", RTLD_NOW);
	if (!h) { fprintf(stderr, "no libcuda: %s\n", dlerror()); return 2; }
	SYM(cuInit); SYM(cuDeviceGet); SYM(cuCtxCreate);
	SYM(cuMemAlloc); SYM(cuMemFree); SYM(cuMemHostAlloc); SYM(cuMemFreeHost);
	SYM(cuMemcpyHtoD); SYM(cuMemcpyDtoH); SYM(cuCtxSynchronize);

	CUdevice dev; CUcontext ctx; CUresult r;
	if ((r = cuInit(0)))               { printf("cuInit rc=%d\n", r); return 2; }
	if ((r = cuDeviceGet(&dev, 0)))    { printf("cuDeviceGet rc=%d\n", r); return 2; }
	if ((r = cuCtxCreate(&ctx, 0, dev))) { printf("cuCtxCreate rc=%d\n", r); return 2; }

	/* Churn: pinned host buffers that are written and then freed, so a later
	 * allocation is handed the same virtual address. */
	for (int i = 0; i < churn; i++) {
		void *p = NULL; CUdeviceptr d = 0;
		if (cuMemHostAlloc(&p, bytes, 0) || cuMemAlloc(&d, bytes)) {
			printf("churn %d: alloc failed\n", i); return 2;
		}
		for (size_t j = 0; j < n; j++) ((float *)p)[j] = -999.0f;
		cuMemcpyHtoD(d, p, bytes);
		cuCtxSynchronize();
		cuMemFree(d);
		cuMemFreeHost(p);
	}
	printf("churned %d pinned host buffers\n", churn);

	void *host = NULL, *back = NULL; CUdeviceptr dptr = 0;
	if (cuMemHostAlloc(&host, bytes, 0) || cuMemAlloc(&dptr, bytes)) {
		printf("alloc failed\n"); return 2;
	}
	back = malloc(bytes);
	for (size_t j = 0; j < n; j++) ((float *)host)[j] = (float)(j % 1000) + 1.0f;
	printf("wrote pattern via %p\n", host);

	if ((r = cuMemcpyHtoD(dptr, host, bytes))) { printf("HtoD rc=%d\n", r); return 2; }
	cuCtxSynchronize();
	if ((r = cuMemcpyDtoH(back, dptr, bytes))) { printf("DtoH rc=%d\n", r); return 2; }
	cuCtxSynchronize();

	size_t bad = 0, first = 0; float got = 0;
	for (size_t j = 0; j < n; j++) {
		float want = (float)(j % 1000) + 1.0f;
		if (((float *)back)[j] != want) {
			if (!bad) { first = j; got = ((float *)back)[j]; }
			bad++;
		}
	}
	if (bad)
		printf("GPU round-trip: %zu/%zu WRONG, first idx %zu got %.1f want %.1f\n",
		       bad, n, first, got, (float)(first % 1000) + 1.0f);
	else
		printf("GPU round-trip: clean -- the GPU saw the CPU's writes\n");
	return bad ? 1 : 0;
}
