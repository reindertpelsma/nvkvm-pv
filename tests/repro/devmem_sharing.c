/*
 * devmem_sharing.c -- measures where /dev/nvkvm-mem sharing stops.
 *
 * Owner-directed test for the dev-nvkvm-mem spike (branch spike/dev-nvkvm-mem):
 * sharing a devmem fd across mmap() calls -- same process, and across a
 * fork() -- is the primary purpose of the device, not an edge case. This
 * measures three things, in order:
 *
 *   PART 1 -- two mmap()s of the SAME fd, SAME process: does the second
 *             install succeed, and do both VAs see the same bytes?
 *   PART 2 -- fd inherited by a forked CHILD, which calls mmap() itself
 *             (a fresh VMA, not an inherited already-mapped one -- mmap()
 *             BEFORE fork() is deliberately not exercised here, see the
 *             comment at fork() below): does the child's install succeed,
 *             and do parent and child see each other's writes through
 *             their own, independent mappings?
 *   PART 3 -- GPU DMA against a mapping registered from EITHER process:
 *             cuMemHostRegister() the parent's mapping, then a kernel
 *             reads bytes the CHILD wrote through ITS OWN mapping, and a
 *             kernel write is read back by the CHILD through ITS OWN
 *             mapping. Same vec_add-shaped kernel as
 *             tests/repro/nested_pinned_sysmem.c and
 *             tests/repro/devmem_gpu_dma.c.
 *
 * Build: cc -O2 -o devmem_sharing devmem_sharing.c -ldl -lpthread
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
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>

/* Must match src/guest/nvkvm_devmem.c's NVKVM_MEM_IOC_SET_SIZE exactly:
 * _IOW('M', 1, __u64). */
#define NVKVM_MEM_IOC_MAGIC    'M'
#define NVKVM_MEM_IOC_SET_SIZE _IOW(NVKVM_MEM_IOC_MAGIC, 1, uint64_t)

#define WINLEN (2UL << 20)   /* NVKVM_DEVMEM_DEFAULT_SIZE */

/* ---- CUDA driver-API subset, lifted from nested_pinned_sysmem.c ---------- */
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
static CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*cuMemHostRegister)(void *, size_t, unsigned);
static CUresult (*cuMemHostUnregister)(void *);
static CUresult (*cuMemHostGetDevicePointer)(CUdeviceptr *, void *, unsigned);
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

#define N     (1 << 12)              /* 4096 u32 = 16 KiB per region */
#define BYTES ((size_t)N * 4)
/* Layout inside the shared 2 MiB window: parent writes at REGION_P, child
 * writes at REGION_C, well apart so the two never overlap. */
#define REGION_P_OFF   0
#define REGION_C_OFF   (256UL * 1024)

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
static uint32_t pat(uint32_t salt, int i){ return (salt ^ (uint32_t)(i * 2654435761u)) ^ 0xA5A5A5A5u; }

static long verify(const uint32_t *p, uint32_t salt)
{
	long good = 0;
	for (int i = 0; i < N; i++)
		if (p[i] == pat(salt, i)) good++;
	return good;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	/* ---- open + (exercise) SET_SIZE, the ftruncate(2)-equivalent ---- */
	int fd = open("/dev/nvkvm-mem", O_RDWR);
	if (fd < 0) { perror("open"); return 2; }
	uint64_t want_size = WINLEN;
	if (ioctl(fd, NVKVM_MEM_IOC_SET_SIZE, &want_size) != 0) {
		printf("NVKVM_MEM_IOC_SET_SIZE(%lu) failed: %s\n",
		       (unsigned long)want_size, strerror(errno));
		return 2;
	}
	printf("NVKVM_MEM_IOC_SET_SIZE(%lu) -- OK\n", (unsigned long)want_size);

	/* =================================================================
	 * PART 1 -- two mmap()s of the SAME fd, SAME process.
	 * ================================================================= */
	printf("\n=== PART 1: two mmap()s, same process ===\n");
	void *a1 = mmap(NULL, WINLEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (a1 == MAP_FAILED) { perror("mmap #1"); return 2; }
	printf("mmap #1 -> %p -- OK\n", a1);
	void *a2 = mmap(NULL, WINLEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (a2 == MAP_FAILED) {
		printf("mmap #2 FAILED: %s\n", strerror(errno));
		return 1;
	}
	printf("mmap #2 -> %p -- OK (%s)\n", a2, a1 == a2 ? "same VA as #1" : "different VA from #1");

	memset(a1, 0, 4096);
	memset(a2, 0, 4096);
	strcpy((char *)a1, "written-through-mapping-1");
	int same_bytes = strcmp((char *)a1, (char *)a2) == 0;
	printf("write via mapping #1, read via mapping #2: %s (\"%s\")\n",
	       same_bytes ? "IDENTICAL -- OK" : "DIFFERENT -- FAIL", (char *)a2);

	munmap(a1, WINLEN);
	munmap(a2, WINLEN);

	/* Re-map once more for parts 2/3 -- this is the PARENT's own first
	 * mmap() AFTER the fork below, so its VMA is fresh and independent of
	 * anything the child does; see the fork() comment. */

	/* =================================================================
	 * PART 2 -- fd shared with a forked child; PART 3 -- GPU DMA.
	 * ================================================================= */
	printf("\n=== PART 2: fd inherited across fork(), child mmap()s itself ===\n");

	int p2c[2], c2p[2];  /* parent->child, child->parent handshake pipes */
	if (pipe(p2c) || pipe(c2p)) { perror("pipe"); return 2; }

	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return 2; }

	if (pid == 0) {
		/* ---- CHILD ----
		 * Deliberately does NOT inherit an already-mapped VMA (the
		 * parent never mmap()'d before this fork() -- see PART 1's
		 * teardown above). The child's mm is its own (fork() gives a
		 * new mm_struct, hence nvkvm_session_get_or_create() gives it
		 * a NEW session/isolate the first time anything in the guest
		 * module touches it), so this mmap() exercises a genuinely
		 * different isolate than the parent's.
		 */
		close(p2c[1]); close(c2p[0]);
		void *ac = mmap(NULL, WINLEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (ac == MAP_FAILED) {
			dprintf(c2p[1], "CHILD mmap FAILED errno=%d (%s)\n", errno, strerror(errno));
			char c = 'F';
			write(c2p[1], &c, 1);
			_exit(1);
		}
		dprintf(c2p[1], "CHILD mmap -> %p -- OK\n", ac);

		/* Write the child's region, then tell the parent it's ready. */
		uint32_t *cregion = (uint32_t *)((char *)ac + REGION_C_OFF);
		for (int i = 0; i < N; i++) cregion[i] = pat(0xC0FFEE, i);
		char sig = 'R';
		write(c2p[1], &sig, 1);

		/* Wait for the parent to have written its own region. */
		char ack;
		if (read(p2c[0], &ack, 1) != 1) { _exit(2); }

		uint32_t *pregion = (uint32_t *)((char *)ac + REGION_P_OFF);
		long good = verify(pregion, 0xFEEDFACE);
		dprintf(c2p[1], "CHILD sees parent's region: %ld/%d correct %s\n",
			good, N, good == N ? "-- OK" : "-- FAIL");

		/* PART 3, child half: wait for the parent's GPU-write signal,
		 * then check the GPU wrote directly into the child's OWN
		 * mapping of the shared window. */
		char gsig;
		if (read(p2c[0], &gsig, 1) == 1 && gsig == 'G') {
			long ggood = verify(cregion, 0xC0FFEE + 1); /* GPU writes salt+1 */
			dprintf(c2p[1], "CHILD sees GPU's write (via CHILD's own mapping): %ld/%d correct %s\n",
				ggood, N, ggood == N ? "-- OK" : "-- FAIL");
		}

		char c = 'D';
		write(c2p[1], &c, 1);
		munmap(ac, WINLEN);
		_exit(0);
	}

	/* ---- PARENT ---- */
	close(p2c[0]); close(c2p[1]);

	void *ap = mmap(NULL, WINLEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ap == MAP_FAILED) {
		printf("PARENT mmap FAILED errno=%d (%s)\n", errno, strerror(errno));
		/* Drain child's pipe so it doesn't block forever, then reap it. */
		char buf[256]; ssize_t n;
		while ((n = read(c2p[0], buf, sizeof buf)) > 0) fwrite(buf, 1, n, stdout);
		int st; waitpid(pid, &st, 0);
		close(fd);
		return 1;
	}
	printf("PARENT mmap -> %p -- OK\n", ap);

	/* Relay the child's own status lines as they arrive. */
	char line[256]; ssize_t n;
	/* Read up through the child's "ready" signal 'R' (one byte, no
	 * newline) -- everything before it is diagnostic text ending in \n,
	 * so read byte-by-byte until we see a bare 'R' right after a line
	 * boundary is simplest to get wrong; instead just read the two
	 * dprintf lines (mmap status, none yet) then the 1-byte signal. */
	{
		/* mmap status line from the child (present in both success and
		 * failure cases -- read up to and including the trailing \n). */
		size_t off = 0;
		while (off < sizeof(line) - 1) {
			ssize_t r = read(c2p[0], line + off, 1);
			if (r != 1) break;
			off++;
			if (line[off - 1] == '\n') break;
		}
		line[off] = 0;
		fputs(line, stdout);
		if (strstr(line, "FAILED")) {
			char f;
			read(c2p[0], &f, 1); /* the 'F' signal byte */
			int st; waitpid(pid, &st, 0);
			close(fd);
			return 1;
		}
	}
	char rsig;
	if (read(c2p[0], &rsig, 1) != 1 || rsig != 'R') {
		printf("PARENT: did not get child's ready signal\n");
		int st; waitpid(pid, &st, 0);
		close(fd);
		return 1;
	}

	/* Now write the parent's region and let the child check it. */
	uint32_t *pregion = (uint32_t *)((char *)ap + REGION_P_OFF);
	for (int i = 0; i < N; i++) pregion[i] = pat(0xFEEDFACE, i);
	char ack = 'A';
	write(p2c[1], &ack, 1);

	/* Read the child's verdict on the parent's region. */
	{
		size_t off = 0;
		while (off < sizeof(line) - 1) {
			ssize_t r = read(c2p[0], line + off, 1);
			if (r != 1) break;
			off++;
			if (line[off - 1] == '\n') break;
		}
		line[off] = 0;
		fputs(line, stdout);
	}

	uint32_t *cregion = (uint32_t *)((char *)ap + REGION_C_OFF);
	long good_p = verify(cregion, 0xC0FFEE);
	printf("PARENT sees child's region: %ld/%d correct %s\n",
	       good_p, N, good_p == N ? "-- OK" : "-- FAIL");

	/* =================================================================
	 * PART 3 -- GPU DMA against a buffer registered from EITHER process.
	 * ================================================================= */
	printf("\n=== PART 3: GPU DMA on the shared window (registered from the PARENT) ===\n");
	H = dlopen("libcuda.so.1", RTLD_NOW);
	int gpu_ok = 0;
	if (!H) {
		printf("no libcuda -- skipping GPU part\n");
	} else {
#define S(x) x = sym2(#x)
		S(cuInit); S(cuDeviceGet); S(cuCtxCreate); S(cuMemAllocManaged);
		S(cuMemHostRegister); S(cuMemHostUnregister);
		S(cuMemHostGetDevicePointer); S(cuModuleLoadData);
		S(cuModuleGetFunction); S(cuLaunchKernel);
#undef S
		cuCtxSync = dlsym(H, "cuCtxSynchronize");
		p_err = dlsym(H, "cuGetErrorName");

		CUresult r = cuInit(0);
		CUdevice d = 0; if (!r) cuDeviceGet(&d, 0);
		CUcontext c = 0; if (!r) r = cuCtxCreate(&c, 0, d);
		CUmodule mod = NULL;
		if (!r) r = cuModuleLoadData(&mod, vec_add_ptx);
		if (!r) r = cuModuleGetFunction(&fn, mod, "vec_add");
		printf("cuda init/ctx/module rc=%d (%s)\n", r, en(r));

		CUdeviceptr mout = 0;
		if (!r) r = cuMemAllocManaged(&mzero, BYTES, 1);
		if (!r) r = cuMemAllocManaged(&mout, BYTES, 1);
		if (!r) memset((void *)mzero, 0, BYTES);

		CUresult rreg = cuMemHostRegister(ap, WINLEN, 2 /* DEVICEMAP */);
		printf("cuMemHostRegister(parent's shared mapping) rc=%d (%s)\n", rreg, en(rreg));

		if (!r && !rreg) {
			CUdeviceptr dwin = 0;
			CUresult rg = cuMemHostGetDevicePointer(&dwin, ap, 0);
			printf("cuMemHostGetDevicePointer rc=%d (%s)\n", rg, en(rg));
			if (!rg && dwin) {
				/* [R] kernel reads the CHILD's region (written by
				 * the OTHER process) through the PARENT's
				 * registration. */
				CUdeviceptr dchild = dwin + REGION_C_OFF;
				memset((void *)mout, 0xEE, BYTES);
				CUresult rl = gpu_copy(dchild, mout);
				long good = verify((uint32_t *)mout, 0xC0FFEE);
				printf("[R] kernel reads child's region via parent's registration: launch rc=%d, %ld/%d correct %s\n",
				       rl, good, N, good == N ? "-- OK" : "-- FAIL");

				/* [W] kernel writes into the PARENT's region
				 * (through the parent's registration); the CHILD
				 * then checks it via ITS OWN mapping (proves the
				 * GPU's write is visible through a DIFFERENT
				 * process's VA, not just back to the writer). */
				CUdeviceptr msrc = 0;
				if (!cuMemAllocManaged(&msrc, BYTES, 1)) {
					for (int i = 0; i < N; i++)
						((uint32_t *)msrc)[i] = pat(0xC0FFEE + 1, i);
					CUdeviceptr dparent = dwin + REGION_C_OFF; /* GPU writes into the CHILD's region this time */
					CUresult rw = gpu_copy(msrc, dparent);
					printf("[W] kernel writes into child's region via parent's registration: launch rc=%d (%s)\n",
					       rw, en(rw));
					gpu_ok = (rl == 0 && rw == 0 && good == N);
				}
			}
			cuMemHostUnregister(ap);
		}
	}

	/* Tell the child the GPU write landed; it checks its own mapping. */
	char gsig = 'G';
	write(p2c[1], &gsig, 1);
	{
		size_t off = 0;
		while (off < sizeof(line) - 1) {
			ssize_t r = read(c2p[0], line + off, 1);
			if (r != 1) break;
			off++;
			if (line[off - 1] == '\n') break;
		}
		line[off] = 0;
		fputs(line, stdout);
	}
	char done;
	read(c2p[0], &done, 1);

	int st = 0;
	waitpid(pid, &st, 0);
	munmap(ap, WINLEN);
	close(fd);

	printf("\nSUMMARY: part1_same_bytes=%s part2_parent_sees_child=%s part3_gpu=%s\n",
	       same_bytes ? "OK" : "FAIL",
	       good_p == N ? "OK" : "FAIL",
	       gpu_ok ? "OK" : "FAIL/SKIPPED");

	return (same_bytes && good_p == N) ? 0 : 1;
}
