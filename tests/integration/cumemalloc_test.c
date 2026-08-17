/*
 * cumemalloc_test.c — verify cuMemAlloc + cuMemcpyHtoD + cuMemcpyDtoH.
 *
 * Returns 0 = pass, 1 = setup/init failure, 2 = alloc failed,
 *         3 = HtoD/DtoH copy failed, 4 = data mismatch.
 *
 * Uses dlopen on libcuda so the test binary itself doesn't link CUDA.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

int main(void) {
	void *h = dlopen("libcuda.so.1", RTLD_NOW);
	if (!h) h = dlopen("libcuda.so", RTLD_NOW);
	if (!h) { fprintf(stderr, "dlopen libcuda failed: %s\n", dlerror()); return 1; }

	CUresult (*cuInit)(unsigned)                                  = dlsym(h, "cuInit");
	CUresult (*cuDeviceGet)(CUdevice *, int)                      = dlsym(h, "cuDeviceGet");
	CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice)      = dlsym(h, "cuCtxCreate_v2");
	CUresult (*cuMemAlloc)(CUdeviceptr *, size_t)                 = dlsym(h, "cuMemAlloc_v2");
	CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t)   = dlsym(h, "cuMemcpyHtoD_v2");
	CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t)         = dlsym(h, "cuMemcpyDtoH_v2");
	CUresult (*cuMemFree)(CUdeviceptr)                            = dlsym(h, "cuMemFree_v2");
	CUresult (*cuCtxDestroy)(CUcontext)                           = dlsym(h, "cuCtxDestroy_v2");

	if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuMemAlloc ||
	    !cuMemcpyHtoD || !cuMemcpyDtoH || !cuMemFree || !cuCtxDestroy) {
		fprintf(stderr, "missing libcuda symbol\n");
		return 1;
	}

	CUresult r = cuInit(0);
	if (r) { fprintf(stderr, "cuInit failed: %d\n", r); return 1; }

	CUdevice dev = 0;
	r = cuDeviceGet(&dev, 0);
	if (r) { fprintf(stderr, "cuDeviceGet failed: %d\n", r); return 1; }

	CUcontext ctx = NULL;
	r = cuCtxCreate(&ctx, 0, dev);
	if (r) { fprintf(stderr, "cuCtxCreate failed: %d\n", r); return 1; }

	const size_t N = 1024;
	CUdeviceptr dptr = 0;
	r = cuMemAlloc(&dptr, N * sizeof(int));
	if (r) { fprintf(stderr, "cuMemAlloc failed: %d\n", r); return 2; }
	printf("cuMemAlloc OK: dptr=0x%lx size=%zu\n", (unsigned long)dptr, N * sizeof(int));

	int *src = malloc(N * sizeof(int));
	int *dst = malloc(N * sizeof(int));
	for (size_t i = 0; i < N; i++) src[i] = (int)(i * 17 + 5);
	memset(dst, 0, N * sizeof(int));

	r = cuMemcpyHtoD(dptr, src, N * sizeof(int));
	if (r) { fprintf(stderr, "cuMemcpyHtoD failed: %d\n", r); return 3; }
	printf("cuMemcpyHtoD OK\n");

	r = cuMemcpyDtoH(dst, dptr, N * sizeof(int));
	if (r) { fprintf(stderr, "cuMemcpyDtoH failed: %d\n", r); return 3; }
	printf("cuMemcpyDtoH OK\n");

	int mismatch = 0;
	for (size_t i = 0; i < N; i++)
		if (src[i] != dst[i]) { mismatch++; if (mismatch < 5)
			fprintf(stderr, "  mismatch[%zu] src=%d dst=%d\n",
				i, src[i], dst[i]); }
	if (mismatch) {
		fprintf(stderr, "DATA MISMATCH: %d / %zu\n", mismatch, N);
		return 4;
	}
	printf("PASS: %zu ints round-tripped through GPU memory\n", N);

	free(src); free(dst);
	cuMemFree(dptr);
	cuCtxDestroy(ctx);
	return 0;
}
