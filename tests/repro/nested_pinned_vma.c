/*
 * nested_pinned_vma.c -- third-stage isolation for the nested-nvkvm zeros.
 *
 * nested_pinned_sysmem.c showed that at L2 a cuMemHostAlloc buffer is NOT
 * shared between the guest CPU and the GPU: the CPU's stores are invisible to a
 * kernel reading the same pointer, and a kernel's stores are invisible to the
 * CPU (the buffer keeps its 0xEE fill).  Two disjoint sets of physical pages.
 *
 * nvkvm makes such a buffer shared by remap_pfn_range()ing a GPA out of its
 * mmap/sparse window over the VMA, so the guest CPU and the host (hence the
 * GPU) address the same memory.  A VMA that did that is VM_PFNMAP -- smaps
 * reports the `pf` VmFlag and Anonymous: 0 kB.  A VMA that fell back to
 * ordinary guest anon pages is not.
 *
 * So: dump the kernel's own description of the buffer's VMA, plus the physical
 * frame the guest CPU resolves it to (needs root for /proc/self/pagemap), at L1
 * and at L2, and compare.  This distinguishes "the window GPA is wrong" from
 * "no window GPA was installed at all" without touching the module.
 *
 * Build: cc -O2 -o nested_pinned_vma nested_pinned_vma.c -ldl
 * Run as root to get the PFN line.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

typedef int CUresult; typedef int CUdevice;
typedef void *CUcontext; typedef unsigned long long CUdeviceptr;
static void *H;
static void *sym2(const char *b){char x[128];snprintf(x,sizeof x,"%s_v2",b);void*s=dlsym(H,x);if(!s)s=dlsym(H,b);return s;}

/* print the /proc/self/smaps stanza containing `addr` */
static void dump_smaps(const char *label, unsigned long addr)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	if (!f) { printf("  (no smaps)\n"); return; }
	char line[512]; int in = 0;
	unsigned long lo = 0, hi = 0;
	printf("  --- smaps for %s (%#lx) ---\n", label, addr);
	while (fgets(line, sizeof line, f)) {
		if (line[0] >= '0' && line[0] <= '9') {
			if (sscanf(line, "%lx-%lx", &lo, &hi) == 2)
				in = (addr >= lo && addr < hi);
			if (in) printf("  %s", line);
		} else if (in) {
			if (!strncmp(line, "Anonymous:", 10) ||
			    !strncmp(line, "Rss:", 4) ||
			    !strncmp(line, "Shared_", 7) ||
			    !strncmp(line, "Private_", 8) ||
			    !strncmp(line, "VmFlags:", 8))
				printf("  %s", line);
			if (!strncmp(line, "VmFlags:", 8)) {
				printf("  => VM_PFNMAP(pf) %s\n",
				       strstr(line, " pf") ? "YES (window-backed)"
							    : "NO  (plain guest pages)");
				break;
			}
		}
	}
	fclose(f);
}

/* physical frame the guest CPU resolves `addr` to, via /proc/self/pagemap */
static void dump_pfn(const char *label, unsigned long addr)
{
	int fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) { printf("  (pagemap unreadable -- run as root)\n"); return; }
	long ps = sysconf(_SC_PAGESIZE);
	uint64_t e = 0;
	if (pread(fd, &e, 8, (addr / ps) * 8) == 8) {
		printf("  %s pagemap: present=%llu swapped=%llu pfn=0x%llx"
		       " -> guest phys 0x%llx\n", label,
		       (unsigned long long)((e >> 63) & 1),
		       (unsigned long long)((e >> 62) & 1),
		       (unsigned long long)(e & ((1ULL << 55) - 1)),
		       (unsigned long long)((e & ((1ULL << 55) - 1)) * ps));
	}
	close(fd);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	H = dlopen("libcuda.so.1", RTLD_NOW);
	if (!H) { printf("no libcuda\n"); return 2; }
	CUresult (*cuInit)(unsigned) = sym2("cuInit");
	CUresult (*cuDeviceGet)(CUdevice *, int) = sym2("cuDeviceGet");
	CUresult (*cuCtxCreate)(CUcontext *, unsigned, CUdevice) = sym2("cuCtxCreate");
	CUresult (*cuMemHostAlloc)(void **, size_t, unsigned) = sym2("cuMemHostAlloc");
	CUresult (*cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned) = sym2("cuMemAllocManaged");
	CUresult (*cuMemAlloc)(CUdeviceptr *, size_t) = sym2("cuMemAlloc");

	if (cuInit(0)) { printf("cuInit failed\n"); return 2; }
	CUdevice d = 0; cuDeviceGet(&d, 0);
	CUcontext c = 0;
	if (cuCtxCreate(&c, 0, d)) { printf("ctx failed\n"); return 2; }

	size_t B = 64 * 1024;
	void *P = NULL;
	CUresult r = cuMemHostAlloc(&P, B, 2);   /* DEVICEMAP */
	printf("cuMemHostAlloc rc=%d host=%p\n", r, P);
	if (r == 0) {
		memset(P, 0x5A, B);                    /* fault every page in */
		dump_smaps("cuMemHostAlloc buffer", (unsigned long)P);
		dump_pfn("cuMemHostAlloc buffer", (unsigned long)P);
	}

	CUdeviceptr M = 0;
	r = cuMemAllocManaged(&M, B, 1);
	printf("\ncuMemAllocManaged rc=%d ptr=0x%llx\n", r, (unsigned long long)M);
	if (r == 0) {
		memset((void *)M, 0x5A, B);
		dump_smaps("managed buffer", (unsigned long)M);
		dump_pfn("managed buffer", (unsigned long)M);
	}

	/* an ordinary malloc, for contrast: this one SHOULD be plain anon */
	void *A = aligned_alloc(4096, B);
	memset(A, 0x5A, B);
	printf("\nplain malloc %p\n", A);
	dump_smaps("plain malloc", (unsigned long)A);
	dump_pfn("plain malloc", (unsigned long)A);
	return 0;
}
