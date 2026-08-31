/*
 * nested_pinned_sysmem.c -- second-stage isolation for the nested-nvkvm zeros.
 *
 * nested_vidmem_zeros.c established that at L2:
 *   - VIDMEM is fine (a kernel reads back what HtoD wrote, and what another
 *     kernel wrote),
 *   - managed memory is fine in both directions,
 *   - cuMemcpyDtoH produces zeros regardless of the source (VIDMEM *or*
 *     managed), and
 *   - cuMemcpyHtoD *from a pinned (cuMemHostAlloc) buffer* lands as zeros while
 *     the same copy from a pageable buffer lands correctly.
 *
 * Common object in every failure: host system memory that the GPU itself
 * touches -- libcuda's copy staging buffer, and cuMemHostAlloc buffers.  This
 * probe takes the copy engine and libcuda's staging heuristics out of the
 * picture entirely and has a KERNEL dereference the pinned buffer directly, in
 * each direction, so the GPU's own view of pinned sysmem is measured rather
 * than inferred.
 *
 * Build: cc -O2 -o nested_pinned_sysmem nested_pinned_sysmem.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int CUresult; typedef int CUdevice;
typedef void *CUcontext; typedef void *CUmodule; typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

static void *H;
static CUresult (*p_err)(CUresult, const char **);
static const char *en(int r){const char*n=NULL; if(p_err&&p_err(r,&n)==0&&n)return n; return "?";}
static void *sym2(const char *b){char x[128];snprintf(x,sizeof x,"%s_v2",b);void*s=dlsym(H,x);if(!s)s=dlsym(H,b);return s;}

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuCtxSync)(void);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemHostAlloc)(void **, size_t, unsigned);
static CUresult (*cuMemHostGetDevicePointer)(CUdeviceptr *, void *, unsigned);
static CUresult (*cuMemHostRegister)(void *, size_t, unsigned);
static CUresult (*cuMemHostUnregister)(void *);
static CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  void *, void **, void **);

static const char vec_add_ptx[] =
"//\n.version 7.5\n.target sm_60\n.address_size 64\n\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n    .param .u32 vec_add_param_3\n)\n{\n"
"    .reg .pred  %p<2>;\n    .reg .b32   %r<8>;\n    .reg .b64   %rd<11>;\n\n"
"    ld.param.u64    %rd1, [vec_add_param_0];\n"
"    ld.param.u64    %rd2, [vec_add_param_1];\n"
"    ld.param.u64    %rd3, [vec_add_param_2];\n"
"    ld.param.u32    %r2,  [vec_add_param_3];\n"
"    mov.u32         %r3, %ntid.x;\n    mov.u32         %r4, %ctaid.x;\n"
"    mov.u32         %r5, %tid.x;\n    mad.lo.s32      %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32     %p1, %r1, %r2;\n    @%p1 bra        $L__BB0_2;\n\n"
"    cvta.to.global.u64  %rd4, %rd1;\n    mul.wide.s32        %rd5, %r1, 4;\n"
"    add.s64             %rd6, %rd4, %rd5;\n"
"    cvta.to.global.u64  %rd7, %rd2;\n    add.s64             %rd8, %rd7, %rd5;\n"
"    ld.global.u32       %r6, [%rd8];\n    ld.global.u32       %r7, [%rd6];\n"
"    add.s32             %r6, %r7, %r6;\n"
"    cvta.to.global.u64  %rd9, %rd3;\n    add.s64             %rd10, %rd9, %rd5;\n"
"    st.global.u32       [%rd10], %r6;\n\n$L__BB0_2:\n    ret;\n}\n";

#define N     (1 << 14)              /* 16384 u32 = 64 KiB */
#define BYTES ((size_t)N * 4)

static CUfunction fn;
static CUdeviceptr mzero;

static int gpu_copy(CUdeviceptr a, CUdeviceptr out)
{
	int n = N; void *args[] = { &a, &mzero, &out, &n };
	unsigned t = 256, b = (N + t - 1) / t;
	CUresult r = cuLaunchKernel(fn, b, 1, 1, t, 1, 1, 0, NULL, args, NULL);
	if (r) return r;
	return cuCtxSync();
}
static uint32_t pat(int i){ return (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u; }

static void verdict(const char *label, const uint32_t *p)
{
	long good = 0, zero = 0, ee = 0; int fb = -1; uint32_t fg = 0;
	for (int i = 0; i < N; i++) {
		if (p[i] == pat(i)) good++;
		else if (fb < 0) { fb = i; fg = p[i]; }
		if (p[i] == 0) zero++;
		if (p[i] == 0xEEEEEEEEu) ee++;
	}
	printf("  %-38s good=%5ld/%d zero=%5ld untouched(0xEE)=%5ld  %s",
	       label, good, N, zero, ee,
	       good == N ? "OK" : zero == N ? "ALL-ZERO"
	       : ee == N ? "UNTOUCHED" : "MIXED");
	if (good != N && fb >= 0)
		printf("  (i=%d got=0x%08x want=0x%08x)", fb, fg, pat(fb));
	printf("\n");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("no libcuda\n"); return 2; }
#define S(x) x = sym2(#x)
	S(cuInit); S(cuDeviceGet); S(cuCtxCreate); S(cuMemAlloc);
	S(cuMemAllocManaged); S(cuMemHostAlloc); S(cuMemHostGetDevicePointer);
	S(cuMemHostRegister); S(cuMemHostUnregister); S(cuMemcpyHtoD);
	S(cuMemcpyDtoH); S(cuMemsetD8); S(cuModuleLoadData);
	S(cuModuleGetFunction); S(cuLaunchKernel);
#undef S
	cuCtxSync = dlsym(H, "cuCtxSynchronize");
	p_err = dlsym(H, "cuGetErrorName");

	if (cuInit(0)) { printf("cuInit failed\n"); return 2; }
	CUdevice d = 0; cuDeviceGet(&d, 0);
	CUcontext c = 0;
	CUresult r = cuCtxCreate(&c, 0, d);
	printf("ctx rc=%d\n", r); if (r) return 2;
	CUmodule mod = NULL;
	if ((r = cuModuleLoadData(&mod, vec_add_ptx))) { printf("mod rc=%d\n", r); return 2; }
	if ((r = cuModuleGetFunction(&fn, mod, "vec_add"))) { printf("fn rc=%d\n", r); return 2; }

	CUdeviceptr mout = 0, msrc = 0;
	CUresult rm = cuMemAllocManaged(&mzero, BYTES, 1);
	rm |= cuMemAllocManaged(&mout,  BYTES, 1);
	rm |= cuMemAllocManaged(&msrc,  BYTES, 1);
	printf("managed alloc rc=%d (%s) mzero=0x%llx mout=0x%llx msrc=0x%llx\n",
	       rm, en(rm), (unsigned long long)mzero, (unsigned long long)mout,
	       (unsigned long long)msrc);
	if (rm || !mzero || !mout || !msrc) return 2;
	memset((void *)mzero, 0, BYTES);
	for (int i = 0; i < N; i++) ((uint32_t *)msrc)[i] = pat(i);

	/* ---- pinned host buffer, dereferenced BY THE KERNEL -------------- */
	void *P = NULL;
	/* flag 2 = CU_MEMHOSTALLOC_DEVICEMAP */
	r = cuMemHostAlloc(&P, BYTES, 2);
	printf("cuMemHostAlloc(DEVICEMAP) rc=%d (%s) host=%p\n", r, en(r), P);
	if (r == 0) {
		CUdeviceptr dp = 0;
		CUresult rg = cuMemHostGetDevicePointer(&dp, P, 0);
		printf("cuMemHostGetDevicePointer rc=%d (%s) dptr=0x%llx%s\n",
		       rg, en(rg), (unsigned long long)dp,
		       (rg == 0 && dp == (CUdeviceptr)(uintptr_t)P)
		       ? " (== host VA, unified)" : "");
		if (rg == 0) {
			printf("\n[P1] CPU writes pinned -> KERNEL reads it "
			       "(GPU's view of pinned sysmem)\n");
			for (int i = 0; i < N; i++) ((uint32_t *)P)[i] = pat(i);
			memset((void *)mout, 0xEE, BYTES);
			r = gpu_copy(dp, mout);
			printf("  launch rc=%d (%s)\n", r, en(r));
			verdict("kernel's view of pinned buffer", (uint32_t *)mout);

			printf("\n[P2] KERNEL writes pinned -> CPU reads it\n");
			memset(P, 0xEE, BYTES);
			r = gpu_copy(msrc, dp);
			printf("  launch rc=%d (%s)\n", r, en(r));
			verdict("CPU's view after kernel wrote it", (uint32_t *)P);
		}
	}

	/* ---- scale: does HtoD from pageable still work at 8 MiB? --------- */
	printf("\n[P5] HtoD(pageable) at several sizes, verified BY KERNEL\n");
	size_t szs[] = { 4096, 65536, BYTES };
	for (unsigned k = 0; k < sizeof szs / sizeof *szs; k++) {
		size_t nb = szs[k]; int nn = (int)(nb / 4);
		CUdeviceptr D = 0;
		if (cuMemAlloc(&D, BYTES)) continue;
		cuMemsetD8(D, 0, BYTES); cuCtxSync();
		uint32_t *hp = malloc(BYTES);
		for (int i = 0; i < N; i++) hp[i] = pat(i);
		CUresult rh = cuMemcpyHtoD(D, hp, nb);
		memset((void *)mout, 0xEE, BYTES);
		gpu_copy(D, mout);
		long good = 0;
		for (int i = 0; i < nn; i++) if (((uint32_t *)mout)[i] == pat(i)) good++;
		printf("  HtoD %7zu B rc=%d -> kernel sees %ld/%d correct%s\n",
		       nb, rh, good, nn, good == nn ? "  OK" : "  BAD");
		free(hp);
	}
	/* NOTE: cuMemHostRegister + kernel deref segfaults *inside libcuda*
	 * at L1 as well as L2, so it discriminates nothing.  It is kept, but
	 * last, so its crash cannot cost us the measurements above. */
	/* ---- pageable host buffer registered with cuMemHostRegister ------ */
	if (cuMemHostRegister) {
		void *Q = aligned_alloc(4096, BYTES);
		/* flag 2 = CU_MEMHOSTREGISTER_DEVICEMAP */
		r = cuMemHostRegister(Q, BYTES, 2);
		printf("\ncuMemHostRegister rc=%d (%s) host=%p\n", r, en(r), Q);
		if (r == 0) {
			CUdeviceptr dq = 0;
			CUresult rq = cuMemHostGetDevicePointer(&dq, Q, 0);
			printf("  cuMemHostGetDevicePointer rc=%d (%s) dq=0x%llx\n",
			       rq, en(rq), (unsigned long long)dq);
			if (rq == 0 && dq) {
				printf("[P3] CPU writes registered -> KERNEL reads it\n");
				for (int i = 0; i < N; i++) ((uint32_t *)Q)[i] = pat(i);
				memset((void *)mout, 0xEE, BYTES);
				r = gpu_copy(dq, mout);
				printf("  launch rc=%d (%s)\n", r, en(r));
				verdict("kernel's view of registered buffer",
					(uint32_t *)mout);

				printf("[P4] KERNEL writes registered -> CPU reads it\n");
				memset(Q, 0xEE, BYTES);
				r = gpu_copy(msrc, dq);
				printf("  launch rc=%d (%s)\n", r, en(r));
				verdict("CPU's view after kernel wrote it",
					(uint32_t *)Q);
			}
			cuMemHostUnregister(Q);
		}
	}

	return 0;
}
