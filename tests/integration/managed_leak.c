/*
 * managed_leak.c — does a managed alloc/free loop give the host memory back?
 *
 * The managed-memory fallback backs each cudaMallocManaged with an RM sysmem
 * object, i.e. PINNED HOST PAGES that live in the host kernel and are invisible
 * to every guest-side accounting.  If a free ever fails to reach RM, the leak is
 * not a guest problem: it is host RAM that never comes back, and a loop of
 * managed allocations takes the whole machine down instead of failing.
 *
 * So the loop below is a bounded stress with a checkpoint, and the ONLY reliable
 * observation is on the host: run `free -m` on the host before and after.  This
 * program's job is to make the demand deterministic and to prove the guest side
 * stayed correct throughout.
 *
 *   cc -O2 -o /tmp/managed_leak managed_leak.c -ldl
 *   /tmp/managed_leak [iters] [mib]
 *
 * Exit 0 iff every iteration allocated, verified and freed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef unsigned long long CUdeviceptr;

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemFree)(CUdeviceptr);

static void *H;
static void *sym2(const char *base)
{
	char b[128]; void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 500;
	size_t mib = argc > 2 ? (size_t)atoi(argv[2]) : 4;
	size_t sz  = mib << 20;
	CUdevice dev; CUcontext ctx;
	int i;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
	cuInit            = sym2("cuInit");
	cuDeviceGet       = sym2("cuDeviceGet");
	cuCtxCreate       = sym2("cuCtxCreate");
	cuMemAllocManaged = sym2("cuMemAllocManaged");
	cuMemFree         = sym2("cuMemFree");
	if (cuInit(0) || cuDeviceGet(&dev, 0) || cuCtxCreate(&ctx, 0, dev)) {
		printf("FAIL cuda setup\n"); return 1;
	}

	printf("loop: %d x %zu MiB managed alloc+touch+free (%zu MiB cumulative)\n",
	       iters, mib, (size_t)iters * mib);
	for (i = 0; i < iters; i++) {
		CUdeviceptr p = 0;
		CUresult r = cuMemAllocManaged(&p, sz, 1u);
		volatile unsigned char *b;

		if (r || !p) {
			printf("FAIL iter %d cuMemAllocManaged rc=%d ptr=0x%llx\n",
			       i, r, (unsigned long long)p);
			return 1;
		}
		b = (volatile unsigned char *)(uintptr_t)p;
		b[0] = 0xa5; b[sz - 1] = 0x5a;
		if (b[0] != 0xa5 || b[sz - 1] != 0x5a) {
			printf("FAIL iter %d readback\n", i);
			return 1;
		}
		if ((r = cuMemFree(p)) != 0) {
			printf("FAIL iter %d cuMemFree rc=%d\n", i, r);
			return 1;
		}
		if (((i + 1) % 100) == 0) {
			printf("  %d/%d ok\n", i + 1, iters);
			fflush(stdout);
		}
	}
	printf("RESULT: PASS (%d iterations, %zu MiB cumulative demand)\n",
	       iters, (size_t)iters * mib);
	return 0;
}
