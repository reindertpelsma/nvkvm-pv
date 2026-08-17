// SPDX-License-Identifier: GPL-2.0
/*
 * pin_size_probe.c — measure the largest host-memory registration the CUDA
 * driver will accept, and how long each registration takes.
 *
 * This is the probe that characterises the nvkvm guest's pinned-host-buffer
 * cap.  It deliberately uses the CUDA *driver* API and calls both entry
 * points, because the two behave differently in the guest:
 *
 *   cuMemHostRegister — register memory the caller already owns (this is what
 *                       cudaHostRegister maps onto).  In the guest this is the
 *                       call that reaches NV_ESC_RM_ALLOC_MEMORY with
 *                       hClass = NV01_MEMORY_SYSTEM_OS_DESCRIPTOR and therefore
 *                       nvkvm_cpu_pages_migrate_range().
 *   cuMemHostAlloc    — allocate + register in one step.
 *
 * Every result is printed with the exact byte count and the driver's own
 * error name, so "it failed" is never reported without the number that failed
 * and the reason the driver gave.
 *
 *   gcc -O2 -o pin_size_probe pin_size_probe.c -ldl
 *   ./pin_size_probe                 # the standard size ladder
 *   ./pin_size_probe --bisect        # find the exact byte boundary too
 *
 * Output lines are machine-readable: "M <key> <value>".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuMemHostRegister)(void *, size_t, unsigned);
static CUresult (*p_cuMemHostUnregister)(void *);
static CUresult (*p_cuMemHostAlloc)(void **, size_t, unsigned);
static CUresult (*p_cuMemFreeHost)(void *);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);

static const char *errname(CUresult r)
{
	const char *s = NULL;
	if (p_cuGetErrorName && p_cuGetErrorName(r, &s) == 0 && s)
		return s;
	return "?";
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* MAP_PRIVATE (the default for malloc'd/mmap'd user memory) vs MAP_SHARED.
 * This is a control, not a knob: remap_pfn_range() refuses any sub-VMA remap
 * on a copy-on-write mapping (is_cow_mapping() == (VM_SHARED|VM_MAYWRITE) ==
 * VM_MAYWRITE), so the two map kinds exercise different kernel paths inside
 * nvkvm_cpu_pages_migrate_range().  Run both. */
static int map_share_flag = MAP_PRIVATE;

/* Returns 0 on success.  *ms gets the wall time of the register call.
 * The mapping is deliberately NOT unmapped on success: the guest module
 * dedupes migrations by start GVA and never forgets a range until the fd is
 * closed, so recycling a VA silently turns the next registration into a no-op
 * and makes results non-deterministic.  One registration per process. */
static CUresult try_register(size_t n, double *ms)
{
	CUresult r;
	double t0;
	/* Faulted in first so the registration cost we measure is registration,
	 * not demand paging. */
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
		       map_share_flag | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		*ms = 0;
		return -1000;
	}
	memset(p, 0x5a, n);

	t0 = now_ms();
	r = p_cuMemHostRegister(p, n, 0);
	*ms = now_ms() - t0;

	if (r == 0)
		p_cuMemHostUnregister(p);
	munmap(p, n);
	return r;
}

/* Count the VMAs in /proc/self/maps that overlap [lo, hi), and report how many
 * VMAs the process has in total.  This is the direct check on the claim that a
 * chunked migration fragments the caller's mapping: a whole-VMA vm_flags_set()
 * cannot split, so a correctly-migrated buffer must still be ONE VMA however
 * many chunks it took. */
static void report_vmas(unsigned long lo, unsigned long hi)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int total = 0, overlapping = 0;
	char first[512];
	first[0] = 0;
	if (!f) { printf("M vmas_overlapping ?\n"); return; }
	while (fgets(line, sizeof(line), f)) {
		unsigned long a, b;
		total++;
		if (sscanf(line, "%lx-%lx", &a, &b) != 2)
			continue;
		if (a < hi && b > lo) {
			overlapping++;
			if (!first[0]) {
				strncpy(first, line, sizeof(first) - 1);
				first[strcspn(first, "\n")] = 0;
			}
		}
	}
	fclose(f);
	printf("M vmas_overlapping_buffer %d\n", overlapping);
	printf("M vmas_total %d\n", total);
	printf("M vma_first %s\n", first);
}

/* Single registration, no unmap, no unregister — for --one. */
static void *keep_ptr;
static CUresult try_register_keep(size_t n, double *ms)
{
	CUresult r;
	double t0;
	void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
		       map_share_flag | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { *ms = 0; return -1000; }
	memset(p, 0x5a, n);
	keep_ptr = p;
	t0 = now_ms();
	r = p_cuMemHostRegister(p, n, 0);
	*ms = now_ms() - t0;
	return r;
}

/* Read back every byte of a successfully registered buffer and verify it still
 * holds the pattern written before registration.  Migration copies the data
 * into a host memfd and repoints the VMA at it; if a chunk were dropped,
 * mis-ordered or mis-offset, this is where it shows up. */
static int verify_pattern(void *p, size_t n)
{
	const unsigned char *b = p;
	size_t bad = 0, i;
	for (i = 0; i < n; i++)
		if (b[i] != 0x5a) { bad++; if (bad == 1) printf("M first_bad_offset %zu\n", i); }
	printf("M pattern_bytes_checked %zu\n", n);
	printf("M pattern_bytes_wrong %zu\n", bad);
	return bad == 0;
}

static CUresult try_hostalloc(size_t n, double *ms)
{
	void *p = NULL;
	double t0 = now_ms();
	CUresult r = p_cuMemHostAlloc(&p, n, 0);
	*ms = now_ms() - t0;
	if (r == 0 && p)
		p_cuMemFreeHost(p);
	return r;
}

int main(int argc, char **argv)
{
	void *lib;
	CUdevice dev;
	CUcontext ctx;
	CUresult r;
	int do_bisect = 0;
	int skip_ladder = 0;
	size_t bisect_hi = 64ULL << 20;   /* default ceiling; --bisect-hi <MiB> */
	size_t one_bytes = 0;             /* --one <bytes>: single registration */
	double ms;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--bisect"))
			do_bisect = 1;
		else if (!strcmp(argv[i], "--no-ladder"))
			skip_ladder = 1;
		else if (!strcmp(argv[i], "--bisect-hi") && i + 1 < argc)
			bisect_hi = (size_t)strtoull(argv[++i], NULL, 0) << 20;
		else if (!strcmp(argv[i], "--one") && i + 1 < argc)
			one_bytes = (size_t)strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--shared"))
			map_share_flag = MAP_SHARED;
	}

	lib = dlopen("libcuda.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen libcuda.so.1: %s\n", dlerror());
		return 3;
	}
#define SYM(n) do { p_##n = dlsym(lib, #n); \
	if (!p_##n) { fprintf(stderr, "missing %s\n", #n); return 3; } } while (0)
	SYM(cuInit);
	SYM(cuDeviceGet);
	SYM(cuMemHostRegister);
	SYM(cuMemHostUnregister);
	SYM(cuMemHostAlloc);
	SYM(cuMemFreeHost);
	SYM(cuGetErrorName);
	p_cuCtxCreate = dlsym(lib, "cuCtxCreate_v2");
	if (!p_cuCtxCreate)
		p_cuCtxCreate = dlsym(lib, "cuCtxCreate");
	if (!p_cuCtxCreate) { fprintf(stderr, "missing cuCtxCreate\n"); return 3; }
#undef SYM

	if ((r = p_cuInit(0)) != 0) {
		fprintf(stderr, "cuInit rc=%d (%s)\n", r, errname(r));
		return 3;
	}
	if ((r = p_cuDeviceGet(&dev, 0)) != 0) {
		fprintf(stderr, "cuDeviceGet rc=%d (%s)\n", r, errname(r));
		return 3;
	}
	if ((r = p_cuCtxCreate(&ctx, 0, dev)) != 0) {
		fprintf(stderr, "cuCtxCreate rc=%d (%s)\n", r, errname(r));
		return 3;
	}

	if (one_bytes) {
		r = try_register_keep(one_bytes, &ms);
		printf("M one %s rc=%d %s bytes=%zu mib=%.4f ms=%.1f map=%s mbps=%.1f\n",
		       r == 0 ? "OK" : "FAIL", r, r == 0 ? "-" : errname(r),
		       one_bytes, (double)one_bytes / (1 << 20), ms,
		       map_share_flag == MAP_SHARED ? "SHARED" : "PRIVATE",
		       ms > 0 ? (double)one_bytes / (1 << 20) / (ms / 1e3) : 0.0);
		if (r == 0 && keep_ptr) {
			report_vmas((unsigned long)keep_ptr,
				    (unsigned long)keep_ptr + one_bytes);
			if (!verify_pattern(keep_ptr, one_bytes))
				return 2;
		}
		return r == 0 ? 0 : 1;
	}

	static const struct { const char *label; size_t n; } ladder[] = {
		{ "1MiB",     1ULL << 20 },
		{ "2MiB",     2ULL << 20 },
		{ "4MiB",     4ULL << 20 },
		{ "8MiB",     8ULL << 20 },
		{ "16MiB",   16ULL << 20 },
		{ "16MiB+1", (16ULL << 20) + 1 },
		{ "17MiB",   17ULL << 20 },
		{ "32MiB",   32ULL << 20 },
		{ "64MiB",   64ULL << 20 },
		{ "80MiB",   80ULL << 20 },
		{ "128MiB", 128ULL << 20 },
		{ "256MiB", 256ULL << 20 },
		{ "512MiB", 512ULL << 20 },
		{ "1GiB",     1ULL << 30 },
		{ "2GiB",     2ULL << 30 },
	};

	if (!skip_ladder) {
	printf("M probe cuMemHostRegister\n");
	for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		r = try_register(ladder[i].n, &ms);
		printf("M reg_%s %s rc=%d %s bytes=%zu ms=%.1f\n",
		       ladder[i].label, r == 0 ? "OK" : "FAIL", r,
		       r == 0 ? "-" : errname(r), ladder[i].n, ms);
		fflush(stdout);
	}

	printf("M probe cuMemHostAlloc\n");
	for (unsigned i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		r = try_hostalloc(ladder[i].n, &ms);
		printf("M alloc_%s %s rc=%d %s bytes=%zu ms=%.1f\n",
		       ladder[i].label, r == 0 ? "OK" : "FAIL", r,
		       r == 0 ? "-" : errname(r), ladder[i].n, ms);
		fflush(stdout);
	}
	}

	if (do_bisect) {
		/* Largest cuMemHostRegister that succeeds, to the byte. */
		size_t lo = 1ULL << 20, hi = bisect_hi;
		if (try_register(lo, &ms) != 0) {
			printf("M max_register_bytes 0\n");
		} else {
			while (lo < hi) {
				size_t mid = lo + (hi - lo + 1) / 2;
				if (try_register(mid, &ms) == 0)
					lo = mid;
				else
					hi = mid - 1;
			}
			printf("M max_register_bytes %zu\n", lo);
			printf("M max_register_mib %.4f\n",
			       (double)lo / (1 << 20));
		}
	}

	printf("M ok 1\n");
	return 0;
}
