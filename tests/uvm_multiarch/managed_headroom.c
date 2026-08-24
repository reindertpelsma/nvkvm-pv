/*
 * managed_headroom.c — how many CONCURRENTLY LIVE managed allocations can a VM
 * hold, and what runs out first?
 *
 * The managed-memory fallback costs, per live allocation:
 *   * one entry in QEMU's 16384-entry U-6 VA ownership table (NVKVM_UVM_VA_MAX,
 *     src/qemu/nvkvm_isolate_handlers.c) — VM-global, shared with every other
 *     external range in the VM, including the ~24 cudaMalloc already makes per
 *     context;
 *   * one RM sysmem object;
 *   * one /dev/nvidiactl handle AND one host fd, because RM arms its mmap
 *     context on the struct file named by NVOS33.fd and never clears it.
 *
 * The documented ceiling is the VA table.  Whether that is the ceiling you
 * actually hit is an empirical question — the host fd cost could hit
 * RLIMIT_NOFILE first, and which one bites is exactly what a multi-architecture
 * validation should report rather than assume.
 *
 * So: allocate the SMALLEST useful managed buffer over and over, holding every
 * one live, until something refuses.  Then report
 *   * how many were live at the refusal,
 *   * the CUresult of the refusal,
 *   * whether the CUDA context still works afterwards, and
 *   * whether freeing them all gives the headroom back (allocate again).
 *
 * A clean refusal plus a working context plus full recovery is a PASS: the
 * bound is documented and fail-closed.  A wedged context, a crash, or a
 * silently-wrong pointer is the finding.
 *
 *   cc -O2 -o /tmp/managed_headroom managed_headroom.c -ldl
 *   /tmp/managed_headroom [max_allocs] [kib_each]
 *
 * Exit 0 iff the ceiling was reached cleanly (or max_allocs was reached with no
 * refusal at all) AND the context survived AND the headroom came back.
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
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemGetInfo)(size_t *, size_t *);
static CUresult (*cuCtxSynchronize)(void);

/*
 * How many /dev/nvidia-uvm mappings does this process actually hold?
 *
 * This is the number that decides NVKVM_UVM_VA_MAX consumption, and it is NOT
 * necessarily the number of cuMemAllocManaged calls: libcuda is free to carve
 * several small managed allocations out of one UVM range, and only the RANGE
 * costs a table entry (one UVM_CREATE_EXTERNAL_RANGE per intercepted mmap).
 * Counting the VMAs measures the cost model directly instead of assuming it.
 */
static void uvm_vmas(const char *when, int nalloc, unsigned long *n_out,
		     unsigned long long *bytes_out)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	unsigned long n = 0;
	unsigned long long lo, hi, bytes = 0;

	*n_out = 0; *bytes_out = 0;
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		if (!strstr(line, "nvidia-uvm"))
			continue;
		if (sscanf(line, "%llx-%llx", &lo, &hi) == 2)
			bytes += hi - lo;
		n++;
	}
	fclose(f);
	*n_out = n; *bytes_out = bytes;
	printf("MAPS[%s]: %d managed allocations live -> %lu /dev/nvidia-uvm VMAs, "
	       "%llu MiB mapped\n", when, nalloc, n, bytes >> 20);
}

static void *H;
static void *sym2(const char *base)
{
	char b[128];
	void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}

#define CU_MEM_ATTACH_GLOBAL 1u

int main(int argc, char **argv)
{
	int max_allocs = argc > 1 ? atoi(argv[1]) : 20000;
	size_t kib     = argc > 2 ? (size_t)atoi(argv[2]) : 64;
	size_t sz      = kib << 10;
	CUdevice dev;
	CUcontext ctx;
	CUdeviceptr *p;
	int n = 0, rc, i, ceiling_rc = 0, ok = 1;
	size_t freeb = 0, totb = 0;
	unsigned long vmas0 = 0, vmas1 = 0;
	unsigned long long vbytes0 = 0, vbytes1 = 0;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
	cuInit            = sym2("cuInit");
	cuDeviceGet       = sym2("cuDeviceGet");
	cuCtxCreate       = sym2("cuCtxCreate");
	cuMemAllocManaged = sym2("cuMemAllocManaged");
	cuMemFree         = sym2("cuMemFree");
	cuMemAlloc        = sym2("cuMemAlloc");
	cuMemGetInfo      = sym2("cuMemGetInfo");
	cuCtxSynchronize  = sym2("cuCtxSynchronize");
	if (!cuInit || !cuMemAllocManaged || !cuMemFree) {
		printf("FAIL: libcuda does not export the driver API we need\n");
		return 1;
	}

	if ((rc = cuInit(0)) != 0)                { printf("FAIL cuInit=%d\n", rc); return 1; }
	if ((rc = cuDeviceGet(&dev, 0)) != 0)     { printf("FAIL cuDeviceGet=%d\n", rc); return 1; }
	if ((rc = cuCtxCreate(&ctx, 0, dev)) != 0){ printf("FAIL cuCtxCreate=%d\n", rc); return 1; }

	p = calloc((size_t)max_allocs, sizeof *p);
	if (!p) { printf("FAIL calloc\n"); return 1; }

	printf("headroom: holding %zu KiB managed allocations live, up to %d of them\n",
	       kib, max_allocs);

	uvm_vmas("before", 0, &vmas0, &vbytes0);

	for (n = 0; n < max_allocs; n++) {
		rc = cuMemAllocManaged(&p[n], sz, CU_MEM_ATTACH_GLOBAL);
		if (rc != 0) { ceiling_rc = rc; break; }
		/* Touch it from the CPU so the mapping is real, not just reserved. */
		((volatile unsigned char *)(uintptr_t)p[n])[0] = (unsigned char)n;
		((volatile unsigned char *)(uintptr_t)p[n])[sz - 1] = (unsigned char)~n;
		if ((n + 1) % 1024 == 0) {
			unsigned long v; unsigned long long vb;
			uvm_vmas("live", n + 1, &v, &vb);
		}
	}
	uvm_vmas("peak", n, &vmas1, &vbytes1);
	/* THE COST MODEL, measured rather than assumed. */
	printf("COST-MODEL: %d live managed allocations of %zu KiB cost %ld "
	       "/dev/nvidia-uvm VMAs (%.4f VMAs per allocation); "
	       "NVKVM_UVM_VA_MAX is 16384\n",
	       n, kib, (long)(vmas1 - vmas0),
	       n ? (double)(vmas1 - vmas0) / (double)n : 0.0);

	if (ceiling_rc)
		printf("CEILING: refused at %d live allocations, CUresult=%d\n", n, ceiling_rc);
	else
		printf("CEILING: not reached — %d live allocations all succeeded "
		       "(raise max_allocs to find it)\n", n);

	/* Does anything else still work?  A ceiling that wedges the context is a
	 * different and much worse result than a ceiling that refuses cleanly. */
	{
		CUdeviceptr d = 0;
		int rc2 = cuMemAlloc ? cuMemAlloc(&d, 1 << 20) : -1;
		int rc3 = cuMemGetInfo ? cuMemGetInfo(&freeb, &totb) : -1;
		printf("CONTEXT-AFTER-CEILING: cuMemAlloc=%d cuMemGetInfo=%d "
		       "(vram free %zu MiB of %zu MiB)\n",
		       rc2, rc3, freeb >> 20, totb >> 20);
		if (rc2 != 0 || rc3 != 0) ok = 0;
		if (rc2 == 0 && cuMemFree) cuMemFree(d);
	}

	/* Give it all back, then prove the headroom actually returned. */
	for (i = 0; i < n; i++) {
		rc = cuMemFree(p[i]);
		if (rc != 0) { printf("FAIL cuMemFree[%d]=%d\n", i, rc); ok = 0; break; }
	}
	printf("RELEASED: %d allocations freed\n", n);

	{
		int again = 0;
		int want = n > 64 ? 64 : (n ? n : 1);
		for (i = 0; i < want; i++) {
			CUdeviceptr q;
			if (cuMemAllocManaged(&q, sz, CU_MEM_ATTACH_GLOBAL) != 0) break;
			((volatile unsigned char *)(uintptr_t)q)[0] = 0xa5;
			p[again++] = q;
		}
		for (i = 0; i < again; i++) cuMemFree(p[i]);
		printf("RECOVERY: re-allocated %d of %d after the release\n", again, want);
		if (again != want) ok = 0;
	}

	printf("RESULT: %s\n", ok ? "CLEAN" : "PROBLEM");
	return ok ? 0 : 1;
}
