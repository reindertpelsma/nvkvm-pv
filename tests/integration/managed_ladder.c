/*
 * managed_ladder.c — size ladder + oversubscription behaviour for
 * cudaMallocManaged under the managed-memory fallback.
 *
 * The fallback backs a "managed" range with pinned host memory published to the
 * GPU as an external range (docs/internal/uvm-va-decoupling.md §2e), so two
 * things need checking that a functional smoke test does not cover:
 *
 *   LADDER  — every size from 4 MiB to 1 GiB allocates, is written by the CPU,
 *             transformed by a kernel, read back by the CPU element-for-element,
 *             and freed.  Sizes are the ones that stress the boundaries: the
 *             1 GiB step is exactly the guest module's forwarded-mmap ceiling
 *             (nvkvm_mmap.c, `vma_len > SZ_1G`).
 *
 *   OVERSUB — a request far larger than host RAM must FAIL CLEANLY: a nonzero
 *             CUresult, a NULL pointer, and — the part that matters — a CUDA
 *             context that still works afterwards.  Real managed memory would
 *             oversubscribe; this backing cannot, so the only acceptable
 *             behaviour is an honest error rather than a wedged context.
 *
 * dlopen()s libcuda and hand-rolls the driver-API types, exactly like
 * tests/integration/cuda_micro.c, so it builds with nothing but a C compiler.
 *
 *   cc -O2 -o /tmp/managed_ladder managed_ladder.c -ldl && /tmp/managed_ladder
 *
 * Exit 0 iff every ladder step verified AND oversubscription failed cleanly.
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
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuMemGetInfo)(size_t *, size_t *);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  void *, void **, void **);
static CUresult (*cuCtxSynchronize)(void);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);

/* p[i] += 1 over a grid — reads AND writes the range, so a mapping that is
 * write-only or read-only in one direction cannot pass. */
static const char ptx[] =
".version 6.0\n.target sm_52\n.address_size 64\n"
".visible .entry inc(.param .u64 p, .param .u32 n){\n"
" .reg .pred %p<2>; .reg .b32 %r<8>; .reg .b64 %rd<5>;\n"
" ld.param.u64 %rd1,[p]; ld.param.u32 %r2,[n];\n"
" cvta.to.global.u64 %rd2,%rd1;\n"
" mov.u32 %r3,%ntid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%tid.x;\n"
" mad.lo.s32 %r1,%r4,%r3,%r5;\n"
" setp.ge.s32 %p1,%r1,%r2; @%p1 bra END;\n"
" mul.wide.s32 %rd3,%r1,4; add.s64 %rd4,%rd2,%rd3;\n"
" ld.global.u32 %r6,[%rd4]; add.s32 %r7,%r6,1; st.global.u32 [%rd4],%r7;\n"
"END: ret;\n}\n";

static void *H;
static void *sym2(const char *base)
{
	char b[128]; void *s;
	snprintf(b, sizeof b, "%s_v2", base);
	s = dlsym(H, b);
	return s ? s : dlsym(H, base);
}

int main(void)
{
	static const size_t sizes[] = {
		4ull   << 20, 16ull  << 20, 64ull  << 20, 256ull << 20,
		512ull << 20, 1024ull << 20,
	};
	CUdevice dev; CUcontext ctx; CUmodule mod; CUfunction inc;
	int failures = 0;

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("FAIL dlopen libcuda: %s\n", dlerror()); return 1; }
	cuInit              = sym2("cuInit");
	cuDeviceGet         = sym2("cuDeviceGet");
	cuCtxCreate         = sym2("cuCtxCreate");
	cuMemAllocManaged   = sym2("cuMemAllocManaged");
	cuMemFree           = sym2("cuMemFree");
	cuMemGetInfo        = sym2("cuMemGetInfo");
	cuModuleLoadData    = sym2("cuModuleLoadData");
	cuModuleGetFunction = sym2("cuModuleGetFunction");
	cuLaunchKernel      = sym2("cuLaunchKernel");
	cuCtxSynchronize    = sym2("cuCtxSynchronize");
	cuMemAlloc          = sym2("cuMemAlloc");
	if (!cuInit || !cuMemAllocManaged) { printf("FAIL missing symbols\n"); return 1; }

	if (cuInit(0) || cuDeviceGet(&dev, 0) || cuCtxCreate(&ctx, 0, dev)) {
		printf("FAIL cuda setup\n"); return 1;
	}
	if (cuModuleLoadData(&mod, ptx) || cuModuleGetFunction(&inc, mod, "inc")) {
		printf("FAIL ptx load\n"); return 1;
	}

	for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		size_t sz = sizes[i];
		unsigned n = (unsigned)(sz / 4);
		CUdeviceptr p = 0;
		CUresult r = cuMemAllocManaged(&p, sz, 1u /* ATTACH_GLOBAL */);
		unsigned *u;
		unsigned blk = 256, grid = (n + blk - 1) / blk;
		void *args[2];
		size_t bad = 0, step;

		if (r || !p) {
			printf("FAIL ladder %4zu MiB  cuMemAllocManaged rc=%d ptr=0x%llx\n",
			       sz >> 20, r, (unsigned long long)p);
			failures++;
			continue;
		}
		u = (unsigned *)(uintptr_t)p;
		/* Stride the verification: at 1 GiB a full pass is 268M elements
		 * and the point is coverage of the mapping, not of the ALU. */
		step = (n > (4u << 20)) ? 977 : 1;
		for (size_t k = 0; k < n; k += step) u[k] = (unsigned)(k ^ 0x5a5a5a5au);

		args[0] = &p; args[1] = &n;
		if (cuLaunchKernel(inc, grid, 1, 1, blk, 1, 1, 0, NULL, args, NULL) ||
		    cuCtxSynchronize()) {
			printf("FAIL ladder %4zu MiB  kernel launch/sync\n", sz >> 20);
			failures++; cuMemFree(p); continue;
		}
		for (size_t k = 0; k < n; k += step)
			if (u[k] != (unsigned)((k ^ 0x5a5a5a5au) + 1)) bad++;

		printf("%s ladder %4zu MiB  ptr=0x%012llx checked=%zu mismatched=%zu\n",
		       bad ? "FAIL" : "ok  ", sz >> 20, (unsigned long long)p,
		       (n + step - 1) / step, bad);
		if (bad) failures++;
		if (cuMemFree(p)) {
			printf("FAIL ladder %4zu MiB  cuMemFree\n", sz >> 20);
			failures++;
		}
	}

	/* ── oversubscription: must fail cleanly, context still usable ──── */
	{
		size_t huge = 512ull << 30;      /* 512 GiB: past any host RAM */
		CUdeviceptr p = 0;
		CUresult r = cuMemAllocManaged(&p, huge, 1u);
		CUdeviceptr probe = 0;
		CUresult pr;
		int alive;

		if (r == 0 && p) {
			printf("FAIL oversub  %zu GiB ACCEPTED (ptr=0x%llx) — this "
			       "backing cannot oversubscribe, so success is a lie\n",
			       huge >> 30, (unsigned long long)p);
			cuMemFree(p);
			failures++;
		} else {
			printf("ok   oversub  %zu GiB refused rc=%d ptr=0x%llx\n",
			       huge >> 30, r, (unsigned long long)p);
		}

		/* The part that actually matters: is the context still healthy? */
		pr = cuMemAlloc(&probe, 1 << 20);
		alive = (pr == 0 && probe);
		if (alive) {
			CUdeviceptr m = 0;
			CUresult mr = cuMemAllocManaged(&m, 4ull << 20, 1u);
			unsigned *u; unsigned n = (4u << 20) / 4;
			unsigned blk = 256, grid = (n + blk - 1) / blk;
			void *args[2]; size_t bad = 0;

			if (mr || !m) {
				printf("FAIL post-oversub  managed alloc rc=%d\n", mr);
				failures++;
			} else {
				u = (unsigned *)(uintptr_t)m;
				for (unsigned k = 0; k < n; k++) u[k] = k;
				args[0] = &m; args[1] = &n;
				if (cuLaunchKernel(inc, grid, 1, 1, blk, 1, 1, 0,
						   NULL, args, NULL) ||
				    cuCtxSynchronize()) {
					printf("FAIL post-oversub  kernel\n");
					failures++;
				} else {
					for (unsigned k = 0; k < n; k++)
						if (u[k] != k + 1) bad++;
					printf("%s post-oversub  4 MiB managed alloc + kernel, mismatched=%zu\n",
					       bad ? "FAIL" : "ok  ", bad);
					if (bad) failures++;
				}
				cuMemFree(m);
			}
			cuMemFree(probe);
		} else {
			printf("FAIL post-oversub  context is dead (cuMemAlloc rc=%d)\n", pr);
			failures++;
		}
	}

	printf("RESULT: %s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
