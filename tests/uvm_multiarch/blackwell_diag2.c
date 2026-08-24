/*
 * blackwell_diag2.c — bisect the Blackwell `cuCtxSynchronize -> 709` failure.
 *
 * Established so far (RTX 5060 / 580.95.05 open module, guest 6.8.0-137):
 *   * managed_ladder fails at EVERY size with "kernel launch/sync";
 *   * a minimal driver-API reproducer fails identically on DEVICE memory, so it
 *     is not a managed-memory failure;
 *   * it reproduces with the BASELINE guest module (252bd44), so it is not this
 *     branch;
 *   * tests/validate.sh still passes 30/0/0 with kernels verified by value,
 *     cuda_micro runs all seven cases, and the CUDA runtime API (attach_verify,
 *     conjugateGradientUM) is correct.
 *
 * So two driver-API programs doing almost the same thing disagree.  cuda_micro
 * differs from managed_ladder in exactly two ways before its first big launch:
 * it launches an EMPTY kernel 50 times as a warm-up, and its kernel only
 * STORES where managed_ladder's loads-modifies-stores.  Vary those two things
 * and nothing else.
 *
 *   cc -O2 -o /tmp/bwdiag2 blackwell_diag2.c -ldl && /tmp/bwdiag2
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
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  void *, void **, void **);
static CUresult (*cuCtxSynchronize)(void);
static CUresult (*cuGetErrorString)(CUresult, const char **);

static void *H;
static void *sym2(const char *base)
{
	char b[128]; void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}
static const char *es(CUresult r)
{ const char *s = "?"; if (cuGetErrorString) cuGetErrorString(r, &s); return s ? s : "?"; }

/* inc: LOAD-modify-STORE (managed_ladder).  wr: STORE only (cuda_micro).
 * noop: empty (cuda_micro's warm-up). */
static const char ptx[] =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry inc(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<8>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra E1;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3;\n"
" ld.global.u32 %r6,[%rd4]; add.s32 %r7,%r6,1; st.global.u32 [%rd4],%r7;\n"
"E1: ret;\n}\n"
".visible .entry wr(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<6>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra E2;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3; st.global.u32 [%rd4],%r1;\n"
"E2: ret;\n}\n"
".visible .entry noop(){ ret; }\n";

static CUmodule mod;
static CUfunction f_inc, f_wr, f_noop;

static void one(const char *label, int warmup, CUfunction f, unsigned grid)
{
	CUdeviceptr p = 0;
	unsigned n = grid * 256;
	void *args[2];
	int rc;

	if (warmup) {
		int i;
		for (i = 0; i < 50; i++) {
			cuLaunchKernel(f_noop, 1, 1, 1, 1, 1, 1, 0, NULL, NULL, NULL);
			cuCtxSynchronize();
		}
	}
	if ((rc = cuMemAlloc(&p, (size_t)n * 4))) {
		printf("  %-38s alloc=%d (%s)\n", label, rc, es(rc)); return; }
	args[0] = &p; args[1] = &n;
	rc = cuLaunchKernel(f, grid, 1, 1, 256, 1, 1, 0, NULL, args, NULL);
	if (rc) { printf("  %-38s LAUNCH=%d (%s)\n", label, rc, es(rc)); cuMemFree(p); return; }
	rc = cuCtxSynchronize();
	printf("  %-38s sync=%d (%s)\n", label, rc, es(rc));
	cuMemFree(p);
}

int main(int argc, char **argv)
{
	CUdevice dev; CUcontext ctx; int rc;
	int mode = argc > 1 ? atoi(argv[1]) : 0;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen\n"); return 1; }
	cuInit = sym2("cuInit"); cuDeviceGet = sym2("cuDeviceGet");
	cuCtxCreate = sym2("cuCtxCreate"); cuMemAlloc = sym2("cuMemAlloc");
	cuMemFree = sym2("cuMemFree"); cuModuleLoadData = sym2("cuModuleLoadData");
	cuModuleGetFunction = sym2("cuModuleGetFunction");
	cuLaunchKernel = sym2("cuLaunchKernel");
	cuCtxSynchronize = sym2("cuCtxSynchronize");
	cuGetErrorString = sym2("cuGetErrorString");

	if ((rc = cuInit(0)))                 { printf("cuInit=%d\n", rc); return 1; }
	if ((rc = cuDeviceGet(&dev, 0)))      { printf("cuDeviceGet=%d\n", rc); return 1; }
	if ((rc = cuCtxCreate(&ctx, 0, dev))) { printf("cuCtxCreate=%d (%s)\n", rc, es(rc)); return 1; }
	/* WHEN does the context die?  Sync with no launch outstanding is a pure
	 * liveness probe: if it already fails here, nothing about the kernel, the
	 * grid or the memory can be the cause. */
	rc = cuCtxSynchronize();
	printf("  probe: sync right after cuCtxCreate  = %d (%s)\n", rc, es(rc));
	if ((rc = cuModuleLoadData(&mod, ptx))) { printf("cuModuleLoadData=%d (%s)\n", rc, es(rc)); return 1; }
	rc = cuCtxSynchronize();
	printf("  probe: sync right after PTX JIT      = %d (%s)\n", rc, es(rc));
	if ((rc = cuModuleGetFunction(&f_inc, mod, "inc")))   { printf("get inc=%d\n", rc); return 1; }
	if ((rc = cuModuleGetFunction(&f_wr, mod, "wr")))     { printf("get wr=%d\n", rc); return 1; }
	if ((rc = cuModuleGetFunction(&f_noop, mod, "noop"))) { printf("get noop=%d\n", rc); return 1; }

	/* One variable per process: a wedged context poisons everything after it,
	 * so each variant must be run in its own process.  mode selects which. */
	switch (mode) {
	case 0: one("first launch = inc,  grid 4096",  0, f_inc,  4096); break;
	case 1: one("first launch = wr,   grid 4096",  0, f_wr,   4096); break;
	case 2: one("noop x50 warm-up, then inc 4096", 1, f_inc,  4096); break;
	case 3: one("noop x50 warm-up, then wr 4096",  1, f_wr,   4096); break;
	case 4: one("first launch = noop, grid 1",     0, f_noop, 1);    break;
	case 5: one("first launch = inc,  grid 1",     0, f_inc,  1);    break;
	case 6: one("first launch = inc,  grid 64",    0, f_inc,  64);   break;
	default: printf("mode 0..6\n");
	}
	return 0;
}
