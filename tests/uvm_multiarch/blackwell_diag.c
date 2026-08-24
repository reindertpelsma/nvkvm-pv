/*
 * blackwell_diag.c — why does a kernel launch over MANAGED memory fail on
 * Blackwell while validate.sh's managed coherence check passes?
 *
 * managed_ladder reports "kernel launch/sync" for every size on an RTX 5060 /
 * 580.95.05 open module, and prints no CUresult.  Three things could produce
 * that and they have completely different meanings:
 *
 *   1. the PTX.  managed_ladder and cuda_micro embed `.target sm_52`;
 *      validate.sh embeds `.target sm_60`.  If the driver's JIT refuses sm_52
 *      on GB206 this is a TEST ARTEFACT and says nothing about the branch.
 *   2. the memory.  If the same kernel runs on cuMemAlloc memory but not on
 *      cuMemAllocManaged memory, that is a real managed-memory failure on this
 *      architecture and it outranks every green result.
 *   3. the launch shape.  A grid size the older path never reached.
 *
 * So: hold everything else constant and vary exactly one thing at a time,
 * printing the CUresult and its string for each.
 *
 *   cc -O2 -o /tmp/bwdiag blackwell_diag.c -ldl && /tmp/bwdiag
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
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuDeviceGetAttribute)(int *, int, CUdevice);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  void *, void **, void **);
static CUresult (*cuCtxSynchronize)(void);
static CUresult (*cuGetErrorString)(CUresult, const char **);
static CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);

static void *H;
static void *sym2(const char *base)
{
	char b[128]; void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}

static const char *es(CUresult r)
{
	const char *s = "?";
	if (cuGetErrorString) cuGetErrorString(r, &s);
	return s ? s : "?";
}

#define BODY \
".visible .entry inc(.param .u64 p, .param .u32 n){\n" \
" .reg .pred %p<2>; .reg .b32 %r<8>; .reg .b64 %rd<5>;\n" \
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n" \
" cvta.to.global.u64 %rd2,%rd1;\n" \
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n" \
" mad.lo.s32 %r1,%r4,%r3,%r5;\n" \
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n" \
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3;\n" \
" ld.global.u32 %r6,[%rd4]; add.s32 %r7,%r6,1; st.global.u32 [%rd4],%r7;\n" \
"END: ret;\n}\n"

static const char ptx52[] = ".version 6.0\n.target sm_52\n.address_size 64\n" BODY;
static const char ptx60[] = ".version 6.0\n.target sm_60\n.address_size 64\n" BODY;
static const char ptx75[] = ".version 6.4\n.target sm_75\n.address_size 64\n" BODY;

#define CU_MEM_ATTACH_GLOBAL 1u

static int try_one(const char *ptxname, const char *ptx, int managed, size_t sz)
{
	CUmodule m = NULL; CUfunction f = NULL;
	CUdeviceptr p = 0;
	unsigned n = (unsigned)(sz / 4);
	unsigned grid = (n + 255) / 256;
	void *args[2];
	int rc, bad = 0;
	unsigned *u;

	rc = cuModuleLoadData(&m, ptx);
	if (rc) { printf("  %-6s %-8s %5zu MiB  cuModuleLoadData=%d (%s)\n",
			 ptxname, managed ? "managed" : "device", sz >> 20, rc, es(rc));
		  return 1; }
	rc = cuModuleGetFunction(&f, m, "inc");
	if (rc) { printf("  %-6s %-8s %5zu MiB  cuModuleGetFunction=%d (%s)\n",
			 ptxname, managed ? "managed" : "device", sz >> 20, rc, es(rc));
		  return 1; }

	rc = managed ? cuMemAllocManaged(&p, sz, CU_MEM_ATTACH_GLOBAL)
		     : cuMemAlloc(&p, sz);
	if (rc) { printf("  %-6s %-8s %5zu MiB  alloc=%d (%s)\n",
			 ptxname, managed ? "managed" : "device", sz >> 20, rc, es(rc));
		  return 1; }

	if (managed) {
		u = (unsigned *)(uintptr_t)p;
		for (unsigned k = 0; k < n; k += 977) u[k] = k;
	} else {
		unsigned *tmp = calloc(n, 4);
		for (unsigned k = 0; k < n; k += 977) tmp[k] = k;
		cuMemcpyHtoD(p, tmp, sz);
		free(tmp);
	}

	args[0] = &p; args[1] = &n;
	rc = cuLaunchKernel(f, grid, 1, 1, 256, 1, 1, 0, NULL, args, NULL);
	if (rc) { printf("  %-6s %-8s %5zu MiB  grid=%u LAUNCH=%d (%s)\n",
			 ptxname, managed ? "managed" : "device", sz >> 20, grid, rc, es(rc));
		  cuMemFree(p); return 1; }
	rc = cuCtxSynchronize();
	if (rc) { printf("  %-6s %-8s %5zu MiB  grid=%u SYNC=%d (%s)\n",
			 ptxname, managed ? "managed" : "device", sz >> 20, grid, rc, es(rc));
		  cuMemFree(p); return 1; }

	if (managed) {
		u = (unsigned *)(uintptr_t)p;
		for (unsigned k = 0; k < n; k += 977) if (u[k] != k + 1) bad++;
	} else {
		unsigned *tmp = calloc(n, 4);
		cuMemcpyDtoH(tmp, p, sz);
		for (unsigned k = 0; k < n; k += 977) if (tmp[k] != k + 1) bad++;
		free(tmp);
	}
	printf("  %-6s %-8s %5zu MiB  grid=%-8u OK, %d wrong\n",
	       ptxname, managed ? "managed" : "device", sz >> 20, grid, bad);
	cuMemFree(p);
	return bad ? 1 : 0;
}

int main(void)
{
	CUdevice dev; CUcontext ctx; int rc, cc_major = 0, cc_minor = 0;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
	cuInit = sym2("cuInit"); cuDeviceGet = sym2("cuDeviceGet");
	cuDeviceGetAttribute = sym2("cuDeviceGetAttribute");
	cuCtxCreate = sym2("cuCtxCreate"); cuMemAlloc = sym2("cuMemAlloc");
	cuMemAllocManaged = sym2("cuMemAllocManaged"); cuMemFree = sym2("cuMemFree");
	cuModuleLoadData = sym2("cuModuleLoadData");
	cuModuleGetFunction = sym2("cuModuleGetFunction");
	cuLaunchKernel = sym2("cuLaunchKernel");
	cuCtxSynchronize = sym2("cuCtxSynchronize");
	cuGetErrorString = sym2("cuGetErrorString");
	cuMemcpyDtoH = sym2("cuMemcpyDtoH"); cuMemcpyHtoD = sym2("cuMemcpyHtoD");

	if ((rc = cuInit(0)))             { printf("cuInit=%d\n", rc); return 1; }
	if ((rc = cuDeviceGet(&dev, 0)))  { printf("cuDeviceGet=%d\n", rc); return 1; }
	if ((rc = cuCtxCreate(&ctx, 0, dev))) { printf("cuCtxCreate=%d (%s)\n", rc, es(rc)); return 1; }
	if (cuDeviceGetAttribute) {
		cuDeviceGetAttribute(&cc_major, 75, dev);   /* COMPUTE_CAPABILITY_MAJOR */
		cuDeviceGetAttribute(&cc_minor, 76, dev);   /* COMPUTE_CAPABILITY_MINOR */
	}
	printf("device compute capability %d.%d\n", cc_major, cc_minor);

	printf("\n-- vary the PTX target, hold the memory kind (device) --\n");
	try_one("sm_52", ptx52, 0, 4u << 20);
	try_one("sm_60", ptx60, 0, 4u << 20);
	try_one("sm_75", ptx75, 0, 4u << 20);

	printf("\n-- vary the PTX target, hold the memory kind (MANAGED) --\n");
	try_one("sm_52", ptx52, 1, 4u << 20);
	try_one("sm_60", ptx60, 1, 4u << 20);
	try_one("sm_75", ptx75, 1, 4u << 20);

	printf("\n-- the ladder's own sizes, sm_52 PTX, managed --\n");
	try_one("sm_52", ptx52, 1,   4u << 20);
	try_one("sm_52", ptx52, 1,  16u << 20);
	try_one("sm_52", ptx52, 1,  64u << 20);
	try_one("sm_52", ptx52, 1, 256u << 20);

	printf("\n-- the ladder's own sizes, sm_60 PTX, managed --\n");
	try_one("sm_60", ptx60, 1,   4u << 20);
	try_one("sm_60", ptx60, 1,  16u << 20);
	try_one("sm_60", ptx60, 1,  64u << 20);
	try_one("sm_60", ptx60, 1, 256u << 20);
	return 0;
}
