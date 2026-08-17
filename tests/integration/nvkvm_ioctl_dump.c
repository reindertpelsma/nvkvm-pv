/*
 * nvkvm_ioctl_dump.c — LD_PRELOAD that captures NVIDIA ioctl arg buffers
 * before AND after the call, dumping the bytes (using _IOC_SIZE for the
 * exact length, so we never read past the struct).
 *
 *   pre  ioctl[<seq>] fd=<n> cmd=0x.... size=N: aa bb cc dd ...
 *   post ioctl[<seq>] fd=<n> cmd=0x.... ret=<r> size=N: aa bb cc dd ...
 *
 * Only logs NVIDIA-family ioctls (_IOC_TYPE == 'F' or 'u' for UVM, 'P'
 * for nvidia-uvm-tools), so the noise from other ioctls is filtered out.
 *
 * Set NVKVM_DUMP=<path> to redirect output to a file instead of stderr.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>

static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static FILE *out;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long seq_ctr;

/* ── tracked nvidia mmap regions ────────────────────────────────────────────
 * Each mmap on a /dev/nvidia* fd is recorded so we can dump its current
 * contents after every ioctl — surfaces kernel-side writes that happen
 * outside the ioctl response path (e.g., RM_USER_SHARED_DATA pages).
 */
struct tracked_region {
	void *addr;
	size_t len;
	int   fd;
	int   prot;
	char  path[64];
	int   in_use;
};
#define MAX_TRACKED 64
static struct tracked_region tracked[MAX_TRACKED];
static pthread_mutex_t tracked_lock = PTHREAD_MUTEX_INITIALIZER;

static void resolve_fd_path(int fd, char *out_path, size_t cap)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(buf, out_path, cap - 1);
	if (n <= 0) { out_path[0] = 0; return; }
	out_path[n] = 0;
}

static void open_out(void)
{
	if (out) return;
	const char *p = getenv("NVKVM_DUMP");
	if (p && *p) {
		FILE *f = fopen(p, "w");
		if (f) { setvbuf(f, NULL, _IOLBF, 0); out = f; return; }
	}
	out = stderr;
}

static int is_nvidia_fd(int fd)
{
	char buf[64];
	char link[128];
	snprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(buf, link, sizeof(link)-1);
	if (n <= 0) return 0;
	link[n] = 0;
	return strstr(link, "nvidia") != NULL;
}

static void hexdump(const void *p, size_t n, char *out_buf, size_t out_cap)
{
	const unsigned char *b = p;
	size_t pos = 0;
	for (size_t i = 0; i < n && pos + 4 < out_cap; i++) {
		pos += snprintf(out_buf + pos, out_cap - pos,
				"%02x%s", b[i],
				((i+1) % 4 == 0 && i+1 != n) ? "_" : "");
	}
}

static void *mmap_common(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
	void *r = real_mmap(addr, length, prot, flags, fd, offset);
	open_out();
	if (r != MAP_FAILED && fd >= 0) {
		char path[64];
		resolve_fd_path(fd, path, sizeof(path));
		/* Only track small pages we can safely read.  Big regions
		 * (UVM, BAR mappings) often segfault on direct CPU access. */
		int trackable = strstr(path, "nvidia") &&
				(prot & PROT_READ) &&
				length <= 16 * 1024;
		if (trackable) {
			pthread_mutex_lock(&tracked_lock);
			for (int i = 0; i < MAX_TRACKED; i++) {
				if (!tracked[i].in_use) {
					tracked[i].in_use = 1;
					tracked[i].addr   = r;
					tracked[i].len    = length;
					tracked[i].fd     = fd;
					tracked[i].prot   = prot;
					strncpy(tracked[i].path, path, sizeof(tracked[i].path) - 1);
					break;
				}
			}
			pthread_mutex_unlock(&tracked_lock);
		}
		if (strstr(path, "nvidia")) {
			pthread_mutex_lock(&out_lock);
			fprintf(out, "mmap %s len=%zu prot=0x%x flags=0x%x off=0x%lx -> %p\n",
				path, length, prot, flags, (long)offset, r);
			pthread_mutex_unlock(&out_lock);
		}
	}
	return r;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap_common(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap_common(addr, length, prot, flags, fd, offset);
}

/* Dump first N bytes of every tracked nvidia mmap region. */
static void dump_tracked_after_seq(unsigned long seq)
{
	pthread_mutex_lock(&tracked_lock);
	for (int i = 0; i < MAX_TRACKED; i++) {
		if (!tracked[i].in_use) continue;
		/* Dump the WHOLE page (up to 4096 bytes) so we don't miss
		 * fields past the first 256. */
		size_t dn = tracked[i].len > 4096 ? 4096 : tracked[i].len;
		char hex[16384] = {0};
		hexdump(tracked[i].addr, dn, hex, sizeof(hex));
		fprintf(out, "      page[%lu] %s @%p (%zu of %zu): %s\n",
			seq, tracked[i].path, tracked[i].addr,
			dn, tracked[i].len, hex);
	}
	pthread_mutex_unlock(&tracked_lock);
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	open_out();

	unsigned int type = _IOC_TYPE(req);
	int log = (type == 'F' || type == 'u' || type == 0xc4 || type == 0xc5);

	unsigned long seq = 0;
	size_t sz = _IOC_SIZE(req);
	char buf_pre[1024]  = {0};
	char buf_post[1024] = {0};

	if (log && arg && sz > 0 && sz <= 256) {
		if (!is_nvidia_fd(fd)) log = 0;
	}

	if (log && arg && sz > 0 && sz <= 256) {
		pthread_mutex_lock(&out_lock);
		seq = ++seq_ctr;
		hexdump(arg, sz, buf_pre, sizeof(buf_pre));
		fprintf(out, "pre  ioctl[%lu] fd=%d cmd=0x%lx size=%zu: %s\n",
			seq, fd, req, sz, buf_pre);
		pthread_mutex_unlock(&out_lock);
	}

	int r = real_ioctl(fd, req, arg);

	if (log && arg && sz > 0 && sz <= 256) {
		pthread_mutex_lock(&out_lock);
		hexdump(arg, sz, buf_post, sizeof(buf_post));
		fprintf(out, "post ioctl[%lu] fd=%d cmd=0x%lx ret=%d size=%zu: %s\n",
			seq, fd, req, r, sz, buf_post);
		/* For RM_CONTROL (NV_ESC_RM_CONTROL = 0x2a), the inner param
		 * buffer is pointed to by NVOS54.params (offset 16, 8 bytes);
		 * NVOS54.params_size at offset 24 (4 bytes).  Dump up to 128
		 * bytes of that buffer so we can see what the kernel wrote. */
		if (_IOC_TYPE(req) == 'F' && _IOC_NR(req) == 0x2a && sz >= 32) {
			const uint8_t *b = arg;
			uintptr_t pp;  uint32_t ps;
			memcpy(&pp, b + 16, sizeof(pp));
			memcpy(&ps, b + 24, sizeof(ps));
			if (pp != 0 && ps > 0 && ps <= 256) {
				char aux[1024] = {0};
				hexdump((const void *)pp, ps, aux, sizeof(aux));
				fprintf(out, "      ctrl[%lu] inner-params (%u B): %s\n",
					seq, ps, aux);
			}
			/* Also dump current tracked mmap pages — surfaces any
			 * kernel-side writeback into RM_USER_SHARED_DATA / BAR0
			 * triggered by this ioctl. */
			dump_tracked_after_seq(seq);
		}
		pthread_mutex_unlock(&out_lock);
	}
	return r;
}
