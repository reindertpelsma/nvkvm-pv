/*
 * syscall_prof.c — LD_PRELOAD syscall-latency profiler (no ptrace).
 *
 * Wraps the syscalls CUDA actually leans on (ioctl/mmap/munmap/poll/ppoll/
 * read/write) and times each with vDSO clock_gettime (~20 ns, near-zero
 * overhead — the calls we measure are µs-ms).  ioctls are bucketed by the
 * NVIDIA escape decode (type<<8|nr) so we see WHICH control is slow.  At exit
 * it dumps, sorted by total time:
 *   - per-syscall-class totals + the top ioctl NRs (count/total/avg/max)
 *   - total in-syscall time vs wall time  → "is the bottleneck syscalls at all?"
 *
 * Build:  gcc -shared -fPIC -O2 syscall_prof.c -o syscall_prof.so -ldl
 * Use:    LD_PRELOAD=./syscall_prof.so <workload>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static inline uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

typedef struct { _Atomic uint64_t cnt, tot, max; } slot;

/* ioctl buckets: [class][nr].  class: 0='F' frontend, 1=type0 (UVM),
 * 2='m' NVKMS, 3='d' DRM, 4=other. */
static slot g_ioctl[5][256];
static slot g_mmap, g_munmap, g_poll, g_ppoll, g_read, g_write, g_ioctl_all;
static _Atomic uint64_t g_total_sys_ns;
static uint64_t g_t0;

static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static int (*real_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);

__attribute__((constructor)) static void prof_init(void)
{
	real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
	real_mmap   = dlsym(RTLD_NEXT, "mmap");
	real_munmap = dlsym(RTLD_NEXT, "munmap");
	real_poll   = dlsym(RTLD_NEXT, "poll");
	real_ppoll  = dlsym(RTLD_NEXT, "ppoll");
	real_read   = dlsym(RTLD_NEXT, "read");
	real_write  = dlsym(RTLD_NEXT, "write");
	g_t0 = now_ns();
}

static inline void rec(slot *s, uint64_t d)
{
	atomic_fetch_add_explicit(&s->cnt, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s->tot, d, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_total_sys_ns, d, memory_order_relaxed);
	uint64_t m = atomic_load_explicit(&s->max, memory_order_relaxed);
	while (d > m && !atomic_compare_exchange_weak_explicit(
			&s->max, &m, d, memory_order_relaxed, memory_order_relaxed))
		;
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap; va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	uint64_t a = now_ns();
	int r = real_ioctl(fd, req, arg);
	uint64_t d = now_ns() - a;
	rec(&g_ioctl_all, d);
	unsigned type = (req >> 8) & 0xff, nr = req & 0xff;
	int cls = type == 'F' ? 0 : type == 0 ? 1 : type == 'm' ? 2 : type == 'd' ? 3 : 4;
	/* g_ioctl rec without double-counting total: subtract the extra add. */
	atomic_fetch_add_explicit(&g_ioctl[cls][nr].cnt, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_ioctl[cls][nr].tot, d, memory_order_relaxed);
	{ uint64_t m = atomic_load_explicit(&g_ioctl[cls][nr].max, memory_order_relaxed);
	  while (d > m && !atomic_compare_exchange_weak_explicit(
			&g_ioctl[cls][nr].max, &m, d, memory_order_relaxed, memory_order_relaxed)); }
	return r;
}

void *mmap(void *a, size_t l, int p, int f, int fd, off_t o)
{
	uint64_t s = now_ns(); void *r = real_mmap(a, l, p, f, fd, o);
	rec(&g_mmap, now_ns() - s); return r;
}
int munmap(void *a, size_t l)
{
	uint64_t s = now_ns(); int r = real_munmap(a, l);
	rec(&g_munmap, now_ns() - s); return r;
}
int poll(struct pollfd *f, nfds_t n, int t)
{
	uint64_t s = now_ns(); int r = real_poll(f, n, t);
	rec(&g_poll, now_ns() - s); return r;
}
int ppoll(struct pollfd *f, nfds_t n, const struct timespec *t, const sigset_t *m)
{
	uint64_t s = now_ns(); int r = real_ppoll(f, n, t, m);
	rec(&g_ppoll, now_ns() - s); return r;
}
ssize_t read(int fd, void *b, size_t n)
{
	uint64_t s = now_ns(); ssize_t r = real_read(fd, b, n);
	rec(&g_read, now_ns() - s); return r;
}
ssize_t write(int fd, const void *b, size_t n)
{
	uint64_t s = now_ns(); ssize_t r = real_write(fd, b, n);
	rec(&g_write, now_ns() - s); return r;
}

static void pr(const char *name, slot *s)
{
	uint64_t c = atomic_load(&s->cnt), t = atomic_load(&s->tot), m = atomic_load(&s->max);
	if (!c) return;
	fprintf(stderr, "  %-22s cnt=%-7llu tot=%8.2f ms  avg=%7.2f us  max=%8.2f us\n",
		name, (unsigned long long)c, t / 1e6, (double)t / c / 1e3, m / 1e3);
}

__attribute__((destructor)) static void prof_dump(void)
{
	uint64_t wall = now_ns() - g_t0;
	uint64_t sys  = atomic_load(&g_total_sys_ns);
	fprintf(stderr, "\n==== syscall_prof ====\n");
	fprintf(stderr, "wall=%.1f ms  in-syscall(wrapped)=%.1f ms (%.1f%% of wall, summed across threads)\n",
		wall / 1e6, sys / 1e6, 100.0 * sys / wall);
	pr("ioctl(all)", &g_ioctl_all);
	pr("mmap", &g_mmap); pr("munmap", &g_munmap);
	pr("poll", &g_poll); pr("ppoll", &g_ppoll);
	pr("read", &g_read); pr("write", &g_write);
	fprintf(stderr, "  -- ioctl by NVIDIA escape (class:nr) --\n");
	const char *cn[5] = { "F", "uvm", "nvkms", "drm", "oth" };
	/* simple top-N by total */
	for (int top = 0; top < 16; top++) {
		int bc = -1, bn = -1; uint64_t bt = 0;
		for (int c = 0; c < 5; c++) for (int n = 0; n < 256; n++) {
			uint64_t t = atomic_load(&g_ioctl[c][n].tot);
			if (t > bt) { bt = t; bc = c; bn = n; }
		}
		if (bc < 0) break;
		slot *s = &g_ioctl[bc][bn];
		uint64_t cc = atomic_load(&s->cnt), mm = atomic_load(&s->max);
		fprintf(stderr, "  %-5s nr=0x%02x  cnt=%-7llu tot=%8.2f ms  avg=%7.2f us  max=%8.2f us\n",
			cn[bc], bn, (unsigned long long)cc, bt / 1e6,
			(double)bt / cc / 1e3, mm / 1e3);
		atomic_store(&s->tot, 0); /* consume so next iter finds the next */
	}
	fprintf(stderr, "======================\n");
}
