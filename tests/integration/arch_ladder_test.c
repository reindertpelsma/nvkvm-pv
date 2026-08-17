/* SPDX-License-Identifier: Apache-2.0 */
/*
 * arch_ladder_test.c — the bring-up "success ladder" for a NEW GPU architecture
 * or a NEW driver-ABI profile, in one binary, with every return code printed.
 *
 * WHY THIS EXISTS.  The pre-existing cuinit_test.c / cumemalloc_test.c between
 * them cover most of the ladder, but they leave two rungs unmeasured:
 *   - cuDeviceGetName is never called, so "the guest sees *a* device" was never
 *     distinguished from "the guest sees the RIGHT device".
 *   - cumemalloc_test.c copies 1024 ints (4 KiB) and never calls cuMemsetD8.
 *     4 KiB fits in a single page, so it cannot catch a multi-page DMA/mapping
 *     bug, and a device-side write path (memset) is never exercised at all.
 * Both gaps return "pass" whether or not the untested path works, which is the
 * exact failure mode this codebase keeps getting bitten by.
 *
 * Build in the guest (link against the FULL path — /usr/local/nvidia-guest/lib
 * holds only symlinks, the real .so lives in /usr/lib/x86_64-linux-gnu):
 *   gcc -O0 -g -o arch_ladder_test arch_ladder_test.c -ldl
 *
 * Exit codes: 0 = whole ladder passed; otherwise the number of the rung that
 * failed (4 = a rung-4 CUDA-init call failed, 5 = a rung-5 memory op failed).
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

static void *H;
static CUresult (*p_cuGetErrorString)(CUresult, const char **);

/* Print every call's rc explicitly — a silent success and a skipped call must
 * never look the same in the log. */
static int chk(const char *what, CUresult rc)
{
	const char *msg = NULL;
	if (rc && p_cuGetErrorString)
		p_cuGetErrorString(rc, &msg);
	printf("  %-28s rc=%d%s%s\n", what, rc,
	       rc ? "  <- FAILED: " : "", rc ? (msg ? msg : "?") : "");
	fflush(stdout);
	return rc;
}

#define SYM(name) dlsym(H, name)

int main(void)
{
	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("dlopen libcuda.so.1 failed: %s\n", dlerror()); return 4; }

	CUresult (*cuInit)(unsigned)                                = SYM("cuInit");
	CUresult (*cuDriverGetVersion)(int *)                       = SYM("cuDriverGetVersion");
	CUresult (*cuDeviceGetCount)(int *)                         = SYM("cuDeviceGetCount");
	CUresult (*cuDeviceGet)(CUdevice *, int)                    = SYM("cuDeviceGet");
	CUresult (*cuDeviceGetName)(char *, int, CUdevice)          = SYM("cuDeviceGetName");
	CUresult (*cuDeviceComputeCapability)(int *, int *, CUdevice) = SYM("cuDeviceComputeCapability");
	CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice)    = SYM("cuCtxCreate_v2");
	CUresult (*cuMemAlloc)(CUdeviceptr *, size_t)               = SYM("cuMemAlloc_v2");
	CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t) = SYM("cuMemcpyHtoD_v2");
	CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t)       = SYM("cuMemcpyDtoH_v2");
	CUresult (*cuMemsetD8)(CUdeviceptr, unsigned char, size_t)  = SYM("cuMemsetD8_v2");
	CUresult (*cuCtxSynchronize)(void)                          = SYM("cuCtxSynchronize");
	CUresult (*cuMemFree)(CUdeviceptr)                          = SYM("cuMemFree_v2");
	CUresult (*cuCtxDestroy)(CUcontext)                         = SYM("cuCtxDestroy_v2");
	p_cuGetErrorString = SYM("cuGetErrorString");

	if (!cuInit || !cuDeviceGetCount || !cuDeviceGet || !cuDeviceGetName ||
	    !cuCtxCreate || !cuMemAlloc || !cuMemcpyHtoD || !cuMemcpyDtoH ||
	    !cuMemsetD8 || !cuMemFree || !cuCtxDestroy) {
		printf("dlsym failed for a required libcuda symbol\n");
		return 4;
	}

	/* ── Rung 4: cuInit / cuDeviceGetCount / cuDeviceGetName / cuCtxCreate ── */
	printf("== RUNG 4: CUDA init ==\n");
	if (chk("cuInit(0)", cuInit(0))) return 4;

	int ver = -1;
	if (cuDriverGetVersion && chk("cuDriverGetVersion", cuDriverGetVersion(&ver))) return 4;
	printf("  driver version reported     : %d\n", ver);

	int count = -1;
	if (chk("cuDeviceGetCount", cuDeviceGetCount(&count))) return 4;
	printf("  device count                : %d\n", count);
	if (count <= 0) { printf("FAIL rung 4: zero devices\n"); return 4; }

	CUdevice dev = 0;
	if (chk("cuDeviceGet(0)", cuDeviceGet(&dev, 0))) return 4;

	char name[256] = {0};
	if (chk("cuDeviceGetName", cuDeviceGetName(name, sizeof name, dev))) return 4;
	printf("  device name                 : \"%s\"\n", name);
	if (name[0] == '\0') { printf("FAIL rung 4: empty device name\n"); return 4; }

	if (cuDeviceComputeCapability) {
		int maj = -1, min = -1;
		if (!cuDeviceComputeCapability(&maj, &min, dev))
			printf("  compute capability          : %d.%d\n", maj, min);
	}

	CUcontext ctx = NULL;
	if (chk("cuCtxCreate", cuCtxCreate(&ctx, 0, dev))) return 4;
	printf("RUNG 4 PASS\n\n");

	/* ── Rung 5: 8 MiB alloc + HtoD/DtoH roundtrip + cuMemsetD8 ───────────── */
	printf("== RUNG 5: device memory (8 MiB) ==\n");
	const size_t N = 8u * 1024u * 1024u;          /* 8 MiB, 2048 pages */
	CUdeviceptr dptr = 0;
	if (chk("cuMemAlloc(8 MiB)", cuMemAlloc(&dptr, N))) return 5;
	printf("  dptr                        : 0x%llx\n", (unsigned long long)dptr);

	unsigned char *src = malloc(N), *dst = malloc(N);
	if (!src || !dst) { printf("host malloc failed\n"); return 5; }
	/* Non-trivial, position-dependent pattern: an off-by-page copy bug that a
	 * constant fill would hide shows up as a mismatch here. */
	for (size_t i = 0; i < N; i++)
		src[i] = (unsigned char)((i * 31u + (i >> 12) * 7u + 11u) & 0xff);
	memset(dst, 0xAA, N);

	if (chk("cuMemcpyHtoD(8 MiB)", cuMemcpyHtoD(dptr, src, N))) return 5;
	if (chk("cuMemcpyDtoH(8 MiB)", cuMemcpyDtoH(dst, dptr, N))) return 5;

	size_t bad = 0, first_bad = (size_t)-1;
	for (size_t i = 0; i < N; i++)
		if (src[i] != dst[i]) { if (!bad) first_bad = i; bad++; }
	if (bad) {
		printf("FAIL rung 5: %zu/%zu bytes differ, first at %zu (src=0x%02x dst=0x%02x)\n",
		       bad, N, first_bad, src[first_bad], dst[first_bad]);
		return 5;
	}
	printf("  roundtrip                   : byte-exact over all %zu bytes\n", N);

	/* cuMemsetD8 — a DEVICE-side write. Verify by reading it back; a memset
	 * that silently no-ops would otherwise pass as "rc=0". */
	if (chk("cuMemsetD8(0x5C)", cuMemsetD8(dptr, 0x5C, N))) return 5;
	if (cuCtxSynchronize && chk("cuCtxSynchronize", cuCtxSynchronize())) return 5;
	memset(dst, 0, N);
	if (chk("cuMemcpyDtoH(after memset)", cuMemcpyDtoH(dst, dptr, N))) return 5;

	bad = 0; first_bad = (size_t)-1;
	for (size_t i = 0; i < N; i++)
		if (dst[i] != 0x5C) { if (!bad) first_bad = i; bad++; }
	if (bad) {
		printf("FAIL rung 5: memset verify %zu/%zu bytes wrong, first at %zu (0x%02x)\n",
		       bad, N, first_bad, dst[first_bad]);
		return 5;
	}
	printf("  cuMemsetD8 verified         : all %zu bytes == 0x5C\n", N);

	chk("cuMemFree", cuMemFree(dptr));
	chk("cuCtxDestroy", cuCtxDestroy(ctx));
	free(src); free(dst);

	printf("RUNG 5 PASS\n\nLADDER COMPLETE: rungs 4 and 5 passed on \"%s\"\n", name);
	return 0;
}
