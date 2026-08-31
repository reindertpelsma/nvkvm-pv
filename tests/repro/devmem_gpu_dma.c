/*
 * devmem_gpu_dma.c -- TASK 2 measurement for the /dev/nvkvm-mem spike
 * (branch spike/dev-nvkvm-mem): does a GPU actually see /dev/nvkvm-mem
 * memory once cuMemHostRegister() is allowed to register it (TASK 1's
 * pass-through in nvkvm_ioctl.c's NV_ESC_RM_ALLOC_MEMORY handling)?
 *
 * Structure lifted directly from tests/repro/nested_pinned_sysmem.c: same
 * dlopen'd driver-API subset, same inline vec_add PTX kernel, same
 * pattern/verdict helpers.  The only new ingredient is the buffer itself:
 * instead of cuMemHostAlloc()/malloc(), it is a /dev/nvkvm-mem mmap().
 *
 * Measures, in order:
 *   1. open("/dev/nvkvm-mem") + mmap() -- must succeed (already proven).
 *   2. cuMemHostRegister() on that mapping -- THE question TASK 1 exists to
 *      answer.  Before pass-through this fails inside
 *      nvkvm_cpu_pages_migrate_range()'s VM_PFNMAP refusal.
 *   3. [R] CPU writes a pattern into the devmem window -> a kernel reads it
 *      and copies it into a managed output buffer -> CPU checks the output.
 *   4. [W] a kernel writes the pattern directly into the devmem window ->
 *      CPU reads it back.
 *   5. cuMemcpyHtoD from a pageable host buffer into a device buffer,
 *      verified by a kernel, THEN cuMemcpyDtoH from that device buffer back
 *      into the devmem window, verified by the CPU -- a byte-exact
 *      round trip through the devmem window in both copy directions.
 *
 * Build: cc -O2 -o devmem_gpu_dma devmem_gpu_dma.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

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
static CUresult (*cuMemHostRegister)(void *, size_t, unsigned);
static CUresult (*cuMemHostUnregister)(void *);
static CUresult (*cuMemHostGetDevicePointer)(CUdeviceptr *, void *, unsigned);
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

/* /dev/nvkvm-mem's window is a fixed 2 MiB (NVKVM_DEVMEM_SIZE, see
 * src/guest/nvkvm_devmem.c) -- stay comfortably inside it. */
#define N     (1 << 14)              /* 16384 u32 = 64 KiB */
#define BYTES ((size_t)N * 4)

static CUfunction fn;
static CUdeviceptr mzero;

/* out = a + zero, i.e. "copy a into out", run as a kernel so the GPU's own
 * view is what gets checked rather than anything libcuda's copy engine or
 * staging heuristics might paper over. */
static int gpu_copy(CUdeviceptr a, CUdeviceptr out)
{
	int n = N; void *args[] = { &a, &mzero, &out, &n };
	unsigned t = 256, b = (N + t - 1) / t;
	CUresult r = cuLaunchKernel(fn, b, 1, 1, t, 1, 1, 0, NULL, args, NULL);
	if (r) return r;
	return cuCtxSync();
}
static uint32_t pat(int i){ return (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u; }

static long verdict(const char *label, const uint32_t *p)
{
	long good = 0, zero = 0, ee = 0; int fb = -1; uint32_t fg = 0;
	for (int i = 0; i < N; i++) {
		if (p[i] == pat(i)) good++;
		else if (fb < 0) { fb = i; fg = p[i]; }
		if (p[i] == 0) zero++;
		if (p[i] == 0xEEEEEEEEu) ee++;
	}
	printf("  %-42s good=%5ld/%d zero=%5ld untouched(0xEE)=%5ld  %s",
	       label, good, N, zero, ee,
	       good == N ? "OK" : zero == N ? "ALL-ZERO"
	       : ee == N ? "UNTOUCHED" : "MIXED");
	if (good != N && fb >= 0)
		printf("  (i=%d got=0x%08x want=0x%08x)", fb, fg, pat(fb));
	printf("\n");
	return good;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	/* ---- (a) open + mmap /dev/nvkvm-mem ------------------------------ */
	int devfd = open("/dev/nvkvm-mem", O_RDWR);
	if (devfd < 0) { perror("open /dev/nvkvm-mem"); return 2; }
	size_t winlen = 2UL << 20;   /* NVKVM_DEVMEM_SIZE */
	void *win = mmap(NULL, winlen, PROT_READ | PROT_WRITE, MAP_SHARED, devfd, 0);
	if (win == MAP_FAILED) { perror("mmap /dev/nvkvm-mem"); return 2; }
	printf("(a) /dev/nvkvm-mem: open fd=%d mmap=%p len=0x%zx -- OK\n",
	       devfd, win, winlen);
	/* touch it with the CPU up front -- prove this process's own view is
	 * sane before CUDA gets anywhere near it. */
	memset(win, 0xEE, winlen);

	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("no libcuda\n"); return 2; }
#define S(x) x = sym2(#x)
	S(cuInit); S(cuDeviceGet); S(cuCtxCreate); S(cuMemAlloc);
	S(cuMemAllocManaged); S(cuMemHostRegister); S(cuMemHostUnregister);
	S(cuMemHostGetDevicePointer); S(cuMemcpyHtoD); S(cuMemcpyDtoH);
	S(cuMemsetD8); S(cuModuleLoadData); S(cuModuleGetFunction);
	S(cuLaunchKernel);
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

	CUdeviceptr mout = 0;
	CUresult rm = cuMemAllocManaged(&mzero, BYTES, 1);
	rm |= cuMemAllocManaged(&mout, BYTES, 1);
	printf("managed alloc rc=%d (%s) mzero=0x%llx mout=0x%llx\n",
	       rm, en(rm), (unsigned long long)mzero, (unsigned long long)mout);
	if (rm || !mzero || !mout) return 2;
	memset((void *)mzero, 0, BYTES);

	/* ---- (b) cuMemHostRegister on the devmem mapping ----------------- */
	printf("\n(b) cuMemHostRegister(/dev/nvkvm-mem window, DEVICEMAP)\n");
	/* flag 2 = CU_MEMHOSTREGISTER_DEVICEMAP */
	r = cuMemHostRegister(win, BYTES, 2);
	printf("  cuMemHostRegister rc=%d (%s)\n", r, en(r));
	if (r) {
		printf("  -- pass-through did NOT make this registrable. Stopping here.\n");
		return 3;
	}
	CUdeviceptr dwin = 0;
	CUresult rg = cuMemHostGetDevicePointer(&dwin, win, 0);
	printf("  cuMemHostGetDevicePointer rc=%d (%s) dptr=0x%llx%s\n",
	       rg, en(rg), (unsigned long long)dwin,
	       (rg == 0 && dwin == (CUdeviceptr)(uintptr_t)win)
	       ? " (== host VA, unified)" : "");
	if (rg || !dwin) { printf("  -- no device pointer, stopping.\n"); return 3; }

	/* ---- (c) kernel READS the devmem window (host writes, GPU reads) - */
	printf("\n(c-R) CPU writes /dev/nvkvm-mem window -> KERNEL reads it\n");
	for (int i = 0; i < N; i++) ((uint32_t *)win)[i] = pat(i);
	memset((void *)mout, 0xEE, BYTES);
	r = gpu_copy(dwin, mout);
	printf("  launch rc=%d (%s)\n", r, en(r));
	long good_r = verdict("kernel's view of /dev/nvkvm-mem window", (uint32_t *)mout);

	/* ---- (c) kernel WRITES the devmem window (GPU writes, host reads) */
	printf("\n(c-W) KERNEL writes /dev/nvkvm-mem window -> CPU reads it\n");
	CUdeviceptr msrc = 0;
	if (cuMemAllocManaged(&msrc, BYTES, 1)) { printf("  msrc alloc failed\n"); return 2; }
	for (int i = 0; i < N; i++) ((uint32_t *)msrc)[i] = pat(i);
	memset(win, 0xEE, BYTES);
	r = gpu_copy(msrc, dwin);
	printf("  launch rc=%d (%s)\n", r, en(r));
	long good_w = verdict("CPU's view of /dev/nvkvm-mem after kernel wrote it",
			      (uint32_t *)win);

	/* ---- (d) cuMemcpyHtoD / cuMemcpyDtoH round trip through the window */
	printf("\n(d) cuMemcpyHtoD(pageable->device, verified by kernel) then "
	       "cuMemcpyDtoH(device->/dev/nvkvm-mem window)\n");
	CUdeviceptr D = 0;
	if (cuMemAlloc(&D, BYTES)) { printf("  device alloc failed\n"); return 2; }
	cuMemsetD8(D, 0, BYTES); cuCtxSync();
	uint32_t *hp = malloc(BYTES);
	for (int i = 0; i < N; i++) hp[i] = pat(i);
	CUresult rh = cuMemcpyHtoD(D, hp, BYTES);
	memset((void *)mout, 0xEE, BYTES);
	gpu_copy(D, mout);
	long good_htod = 0;
	for (int i = 0; i < N; i++) if (((uint32_t *)mout)[i] == pat(i)) good_htod++;
	printf("  HtoD(pageable->device) rc=%d -> kernel sees %ld/%d correct%s\n",
	       rh, good_htod, N, good_htod == N ? "  OK" : "  BAD");

	memset(win, 0xEE, BYTES);
	CUresult rd = cuMemcpyDtoH(win, D, BYTES);
	long good_dtoh = 0;
	for (int i = 0; i < N; i++) if (((uint32_t *)win)[i] == pat(i)) good_dtoh++;
	printf("  DtoH(device->/dev/nvkvm-mem window) rc=%d -> CPU sees %ld/%d correct%s\n",
	       rd, good_dtoh, N, good_dtoh == N ? "  OK" : "  BAD");

	cuMemHostUnregister(win);
	printf("\nSUMMARY: register=OK read=%s write=%s HtoD=%s DtoH=%s\n",
	       good_r == N ? "OK" : "FAIL", good_w == N ? "OK" : "FAIL",
	       good_htod == N ? "OK" : "FAIL", good_dtoh == N ? "OK" : "FAIL");

	munmap(win, winlen);
	close(devfd);
	return (good_r == N && good_w == N && good_htod == N && good_dtoh == N) ? 0 : 1;
}
