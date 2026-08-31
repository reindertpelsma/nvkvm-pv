/*
 * nested_vidmem_zeros.c -- isolate WHERE the zeros come from when nvkvm runs
 * nested (L2: an nvkvm guest booted by nvkvm inside an nvkvm guest).
 *
 * Symptom being chased: at L2, cuMemAlloc + cuMemcpyHtoD + cuMemcpyDtoH
 * round-trips as all zeros with every ioctl returning success, while managed
 * memory + a real kernel verify byte-exact.
 *
 * "Reads back as zeros" has three mutually exclusive explanations and the plain
 * round-trip cannot tell them apart:
 *
 *   (A) the HtoD never lands  -- VIDMEM really is zero; the GPU would also see
 *       zeros if a kernel read the buffer.
 *   (B) the DtoH is what zeros -- VIDMEM holds the right bytes, a kernel sees
 *       them, but the copy back to the host produces zeros.
 *   (C) the host staging buffer is the broken object, not VIDMEM -- in which
 *       case copies to/from *pinned* host memory (no staging buffer) behave
 *       differently from copies to/from pageable host memory.
 *
 * The GPU's own view is read out through MANAGED memory, which is independently
 * known-good at L2 (cuda_managed_coherence passes there), using vec_add:
 * c[i] = a[i] + b[i].  With b a managed buffer of zeros and c a managed buffer,
 * vec_add(D, 0, M) copies the GPU's view of device buffer D into M, where the
 * CPU can read it without any cuMemcpy at all.
 *
 * Build: cc -O2 -o nested_vidmem_zeros nested_vidmem_zeros.c -ldl
 * Run on bare metal / L1 first -- every line must read OK there.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

static void *H;
static CUresult (*p_err)(CUresult, const char **);
static const char *en(int r)
{
	const char *n = NULL;
	if (p_err && p_err(r, &n) == 0 && n) return n;
	return "?";
}
static void *sym2(const char *b)
{
	char x[128];
	snprintf(x, sizeof x, "%s_v2", b);
	void *s = dlsym(H, x);
	if (!s) s = dlsym(H, b);
	return s;
}

static CUresult (*cuInit)(unsigned);
static CUresult (*cuDeviceGet)(CUdevice *, int);
static CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*cuCtxSync)(void);
static CUresult (*cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*cuMemFree)(CUdeviceptr);
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemHostAlloc)(void **, size_t, unsigned);
static CUresult (*cuMemFreeHost)(void *);
static CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*cuMemcpyDtoD)(CUdeviceptr, CUdeviceptr, size_t);
static CUresult (*cuMemsetD8)(CUdeviceptr, unsigned char, size_t);
static CUresult (*cuMemsetD32)(CUdeviceptr, unsigned, size_t);
static CUresult (*cuModuleLoadData)(CUmodule *, const void *);
static CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
static CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
				  unsigned, unsigned, unsigned, unsigned,
				  void *, void **, void **);
static CUresult (*cuPointerGetAttribute)(void *, int, CUdeviceptr);

static const char vec_add_ptx[] =
"//\n"
".version 7.5\n"
".target sm_60\n"
".address_size 64\n"
"\n"
".visible .entry vec_add(\n"
"    .param .u64 vec_add_param_0,\n"
"    .param .u64 vec_add_param_1,\n"
"    .param .u64 vec_add_param_2,\n"
"    .param .u32 vec_add_param_3\n"
")\n"
"{\n"
"    .reg .pred  %p<2>;\n"
"    .reg .b32   %r<8>;\n"
"    .reg .b64   %rd<11>;\n"
"\n"
"    ld.param.u64    %rd1, [vec_add_param_0];\n"
"    ld.param.u64    %rd2, [vec_add_param_1];\n"
"    ld.param.u64    %rd3, [vec_add_param_2];\n"
"    ld.param.u32    %r2,  [vec_add_param_3];\n"
"    mov.u32         %r3, %ntid.x;\n"
"    mov.u32         %r4, %ctaid.x;\n"
"    mov.u32         %r5, %tid.x;\n"
"    mad.lo.s32      %r1, %r3, %r4, %r5;\n"
"    setp.ge.s32     %p1, %r1, %r2;\n"
"    @%p1 bra        $L__BB0_2;\n"
"\n"
"    cvta.to.global.u64  %rd4, %rd1;\n"
"    mul.wide.s32        %rd5, %r1, 4;\n"
"    add.s64             %rd6, %rd4, %rd5;\n"
"    cvta.to.global.u64  %rd7, %rd2;\n"
"    add.s64             %rd8, %rd7, %rd5;\n"
"    ld.global.u32       %r6, [%rd8];\n"
"    ld.global.u32       %r7, [%rd6];\n"
"    add.s32             %r6, %r7, %r6;\n"
"    cvta.to.global.u64  %rd9, %rd3;\n"
"    add.s64             %rd10, %rd9, %rd5;\n"
"    st.global.u32       [%rd10], %r6;\n"
"\n"
"$L__BB0_2:\n"
"    ret;\n"
"}\n";

#define N        (1 << 16)          /* 65536 u32 = 256 KiB */
#define BYTES    ((size_t)N * 4)

static CUfunction fn;
static CUdeviceptr mzero;           /* managed, all zeros -- the "+0" operand */

/* run vec_add(a, mzero, out, N) -- i.e. copy the GPU's view of `a` into `out` */
static int gpu_copy(CUdeviceptr a, CUdeviceptr out)
{
	int n = N;
	void *args[] = { &a, &mzero, &out, &n };
	unsigned threads = 256, blocks = (N + threads - 1) / threads;
	CUresult r = cuLaunchKernel(fn, blocks, 1, 1, threads, 1, 1, 0, NULL,
				    args, NULL);
	if (r) return r;
	return cuCtxSync();
}

static uint32_t pat(int i) { return (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u; }

/* how many of the N u32s in `p` match pat(), and how many are zero */
static void tally(const uint32_t *p, long *good, long *zero, int *firstbad,
		  uint32_t *firstgot)
{
	long g = 0, z = 0; int fb = -1; uint32_t fg = 0;
	for (int i = 0; i < N; i++) {
		if (p[i] == pat(i)) g++;
		else if (fb < 0) { fb = i; fg = p[i]; }
		if (p[i] == 0) z++;
	}
	*good = g; *zero = z; *firstbad = fb; *firstgot = fg;
}

static void verdict(const char *label, const uint32_t *p)
{
	long good, zero; int fb; uint32_t fg;
	tally(p, &good, &zero, &fb, &fg);
	printf("  %-34s good=%6ld/%d  zero=%6ld/%d  %s",
	       label, good, N, zero, N,
	       good == N ? "OK" : (zero == N ? "ALL-ZERO" : "MIXED"));
	if (good != N) printf("  (first bad i=%d got=0x%08x want=0x%08x)",
			      fb, fg, fb < 0 ? 0 : pat(fb));
	printf("\n");
}

int main(void)
{
	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("no libcuda: %s\n", dlerror()); return 2; }
#define S(x) x = sym2(#x)
	S(cuInit); S(cuDeviceGet); S(cuCtxCreate); S(cuMemAlloc); S(cuMemFree);
	S(cuMemAllocManaged); S(cuMemHostAlloc); S(cuMemFreeHost);
	S(cuMemcpyHtoD); S(cuMemcpyDtoH); S(cuMemcpyDtoD); S(cuMemsetD8);
	S(cuMemsetD32); S(cuModuleLoadData); S(cuModuleGetFunction);
	S(cuLaunchKernel); S(cuPointerGetAttribute);
#undef S
	cuCtxSync = dlsym(H, "cuCtxSynchronize");
	p_err = dlsym(H, "cuGetErrorName");

	CUresult r = cuInit(0);
	if (r) { printf("cuInit rc=%d (%s)\n", r, en(r)); return 2; }
	CUdevice d = 0; cuDeviceGet(&d, 0);
	CUcontext c = 0;
	r = cuCtxCreate(&c, 0, d);
	printf("ctx rc=%d\n", r);
	if (r) return 2;

	CUmodule mod = NULL;
	r = cuModuleLoadData(&mod, vec_add_ptx);
	if (r) { printf("module rc=%d (%s)\n", r, en(r)); return 2; }
	r = cuModuleGetFunction(&fn, mod, "vec_add");
	if (r) { printf("getfn rc=%d (%s)\n", r, en(r)); return 2; }

	/* managed scratch: mzero (zeros), mout (kernel output), msrc (pattern) */
	CUdeviceptr mout = 0, msrc = 0;
	r  = cuMemAllocManaged(&mzero, BYTES, 1);
	r |= cuMemAllocManaged(&mout,  BYTES, 1);
	r |= cuMemAllocManaged(&msrc,  BYTES, 1);
	if (r) { printf("managed alloc rc=%d (%s)\n", r, en(r)); return 2; }
	memset((void *)mzero, 0, BYTES);
	for (int i = 0; i < N; i++) ((uint32_t *)msrc)[i] = pat(i);

	uint32_t *hp = malloc(BYTES);          /* pageable host */
	void *hpin = NULL;
	CUresult rpin = cuMemHostAlloc ? cuMemHostAlloc(&hpin, BYTES, 0) : 1;
	printf("cuMemHostAlloc rc=%d (%s) ptr=%p\n", rpin, en(rpin), hpin);

	CUdeviceptr D = 0;
	r = cuMemAlloc(&D, BYTES);
	if (r) { printf("cuMemAlloc rc=%d (%s)\n", r, en(r)); return 2; }
	printf("device buffer D = 0x%llx (%zu bytes)\n",
	       (unsigned long long)D, BYTES);

	/* sanity: managed round-trips through the kernel at all */
	printf("\n[0] control: managed -> kernel -> managed (known good at L2)\n");
	memset((void *)mout, 0xEE, BYTES);
	r = gpu_copy(msrc, mout);
	printf("  launch rc=%d (%s)\n", r, en(r));
	verdict("managed src, read by CPU", (uint32_t *)mout);

	/* ---- A: does HtoD land in VIDMEM as far as the GPU is concerned? ---- */
	printf("\n[1] HtoD(pageable) -> kernel reads D -> managed out\n");
	for (int i = 0; i < N; i++) hp[i] = pat(i);
	r = cuMemsetD8(D, 0x00, BYTES); cuCtxSync();
	CUresult rh = cuMemcpyHtoD(D, hp, BYTES);
	printf("  cuMemcpyHtoD rc=%d (%s)\n", rh, en(rh));
	memset((void *)mout, 0xEE, BYTES);
	r = gpu_copy(D, mout);
	printf("  launch rc=%d (%s)\n", r, en(r));
	verdict("GPU's view of D after HtoD", (uint32_t *)mout);

	printf("\n[2] same D, now read back with cuMemcpyDtoH(pageable)\n");
	memset(hp, 0xEE, BYTES);
	CUresult rd = cuMemcpyDtoH(hp, D, BYTES);
	printf("  cuMemcpyDtoH rc=%d (%s)\n", rd, en(rd));
	verdict("host buffer after DtoH", hp);

	if (rpin == 0) {
		printf("\n[3] HtoD from PINNED host memory -> kernel reads D\n");
		for (int i = 0; i < N; i++) ((uint32_t *)hpin)[i] = pat(i);
		cuMemsetD8(D, 0x00, BYTES); cuCtxSync();
		rh = cuMemcpyHtoD(D, hpin, BYTES);
		printf("  cuMemcpyHtoD rc=%d (%s)\n", rh, en(rh));
		memset((void *)mout, 0xEE, BYTES);
		r = gpu_copy(D, mout);
		printf("  launch rc=%d (%s)\n", r, en(r));
		verdict("GPU's view of D after pinned HtoD", (uint32_t *)mout);

		printf("\n[4] same D, read back with cuMemcpyDtoH into PINNED\n");
		memset(hpin, 0xEE, BYTES);
		rd = cuMemcpyDtoH(hpin, D, BYTES);
		printf("  cuMemcpyDtoH rc=%d (%s)\n", rd, en(rd));
		verdict("pinned host buffer after DtoH", (uint32_t *)hpin);
	}

	/* ---- B: GPU writes VIDMEM, host reads it back three ways ---------- */
	printf("\n[5] kernel writes pattern INTO D (managed src -> D)\n");
	cuMemsetD8(D, 0x00, BYTES); cuCtxSync();
	r = gpu_copy(msrc, D);
	printf("  launch rc=%d (%s)\n", r, en(r));
	memset((void *)mout, 0xEE, BYTES);
	r = gpu_copy(D, mout);
	printf("  reread by kernel rc=%d (%s)\n", r, en(r));
	verdict("GPU's view of D (kernel-written)", (uint32_t *)mout);
	memset(hp, 0xEE, BYTES);
	rd = cuMemcpyDtoH(hp, D, BYTES);
	printf("  cuMemcpyDtoH rc=%d (%s)\n", rd, en(rd));
	verdict("host after DtoH of kernel-written D", hp);

	/* ---- C: is cuMemcpy broken generally, or only for VIDMEM? --------- */
	printf("\n[6] cuMemcpyDtoH with a MANAGED source (no VIDMEM involved)\n");
	memset(hp, 0xEE, BYTES);
	rd = cuMemcpyDtoH(hp, msrc, BYTES);
	printf("  cuMemcpyDtoH rc=%d (%s)\n", rd, en(rd));
	verdict("host after DtoH from managed", hp);

	printf("\n[7] cuMemcpyHtoD into a MANAGED dst (no VIDMEM involved)\n");
	memset((void *)mout, 0, BYTES);
	for (int i = 0; i < N; i++) hp[i] = pat(i);
	rh = cuMemcpyHtoD(mout, hp, BYTES);
	printf("  cuMemcpyHtoD rc=%d (%s)\n", rh, en(rh));
	verdict("managed dst read by CPU", (uint32_t *)mout);

	/* ---- D: DtoD between two VIDMEM buffers, verified by kernel ------- */
	printf("\n[8] cuMemsetD32(D, pattern-ish) then kernel + DtoH\n");
	r = cuMemsetD32 ? cuMemsetD32(D, 0xDEADBEEF, N) : 1;
	cuCtxSync();
	printf("  cuMemsetD32 rc=%d (%s)\n", r, en(r));
	memset((void *)mout, 0xEE, BYTES);
	gpu_copy(D, mout);
	printf("  kernel sees D[0]=0x%08x D[1]=0x%08x D[%d]=0x%08x\n",
	       ((uint32_t *)mout)[0], ((uint32_t *)mout)[1], N - 1,
	       ((uint32_t *)mout)[N - 1]);
	memset(hp, 0xEE, BYTES);
	cuMemcpyDtoH(hp, D, BYTES);
	printf("  DtoH  sees D[0]=0x%08x D[1]=0x%08x D[%d]=0x%08x\n",
	       hp[0], hp[1], N - 1, hp[N - 1]);

	cuMemFree(D);
	return 0;
}
