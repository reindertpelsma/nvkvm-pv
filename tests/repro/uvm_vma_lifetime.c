/*
 * uvm_vma_lifetime.c -- managed-fallback VMA split/fork lifetime regression.
 *
 * This is deliberately an nvkvm-guest stress test, not a host differential:
 * nvkvm's managed fallback has a GPA quarantine whose finite limits are part
 * of the bug being exercised.  A child punches a page out of an inherited
 * managed VMA, leaving two child VMAs plus the parent's complete VMA alive.
 * The parent then releases more than 64 extents and more than 64 MiB, forcing
 * churn past both quarantine limits before either process checks the original
 * mapping again.
 *
 * Before the VMA reference fix, the child's first partial munmap released the
 * shared host/GPA backing.  The surviving mappings then named freed backing;
 * closing either one also dereferenced the freed mmap-region object.  A pass
 * proves the CPU mappings keep their contents through the split/fork/churn
 * sequence.  It does not replace the ordinary GPU coherence test.
 *
 *   cc -O2 -Wall -Wextra -o uvm_vma_lifetime uvm_vma_lifetime.c -ldl
 *   ./uvm_vma_lifetime
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int CUresult;
typedef uintptr_t CUdeviceptr;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;

#define CU_MEM_ATTACH_GLOBAL 0x1u
#define LIVE_BYTES (2u * 1024u * 1024u)
#define CHURN_BYTES (2u * 1024u * 1024u)
#define CHURN_EXTENTS 80u

static CUresult (*p_cuInit)(unsigned);
static CUresult (*p_cuDeviceGet)(CUdevice *, int);
static CUresult (*p_cuCtxCreate)(CUcontext *, unsigned, CUdevice);
static CUresult (*p_cuMemAllocManaged)(CUdeviceptr *, size_t, unsigned);
static CUresult (*p_cuMemFree)(CUdeviceptr);
static CUresult (*p_cuGetErrorName)(CUresult, const char **);
static void *cuda;

static void *sym2(const char *base)
{
	char name[128];
	void *sym;

	snprintf(name, sizeof(name), "%s_v2", base);
	sym = dlsym(cuda, name);
	return sym ? sym : dlsym(cuda, base);
}

static const char *errname(CUresult ret)
{
	const char *name = NULL;

	if (p_cuGetErrorName && !p_cuGetErrorName(ret, &name) && name)
		return name;
	return "?";
}

static int one_byte(int fd, char value)
{
	return write(fd, &value, 1) == 1 ? 0 : -1;
}

static int wait_byte(int fd)
{
	char value;

	return read(fd, &value, 1) == 1 ? 0 : -1;
}

static unsigned char pattern(size_t off)
{
	return (unsigned char)((off * 131u + 17u) & 0xffu);
}

static int check_span(const unsigned char *p, size_t begin, size_t end,
		      const char *who)
{
	size_t off;

	for (off = begin; off < end; off += 4093u) {
		if (p[off] != pattern(off)) {
			fprintf(stderr,
				"FAIL %s: byte %zu changed (got 0x%02x want 0x%02x)\n",
				who, off, p[off], pattern(off));
			return -1;
		}
	}
	if (end > begin && p[end - 1] != pattern(end - 1)) {
		fprintf(stderr,
			"FAIL %s: byte %zu changed (got 0x%02x want 0x%02x)\n",
			who, end - 1, p[end - 1], pattern(end - 1));
		return -1;
	}
	return 0;
}

int main(void)
{
	CUdeviceptr live = 0;
	CUcontext ctx = NULL;
	CUdevice dev = 0;
	CUresult ret;
	unsigned char *p;
	long page_size;
	int ready[2], done[2], status;
	pid_t child;
	unsigned i;

	cuda = dlopen("libcuda.so.1", RTLD_NOW);
	if (!cuda)
		cuda = dlopen("libcuda.so", RTLD_NOW);
	if (!cuda) {
		fprintf(stderr, "FAIL libcuda: %s\n", dlerror());
		return 1;
	}
	p_cuInit = dlsym(cuda, "cuInit");
	p_cuDeviceGet = dlsym(cuda, "cuDeviceGet");
	p_cuCtxCreate = sym2("cuCtxCreate");
	p_cuMemAllocManaged = dlsym(cuda, "cuMemAllocManaged");
	p_cuMemFree = sym2("cuMemFree");
	p_cuGetErrorName = dlsym(cuda, "cuGetErrorName");
	if (!p_cuInit || !p_cuDeviceGet || !p_cuCtxCreate ||
	    !p_cuMemAllocManaged || !p_cuMemFree) {
		fprintf(stderr, "FAIL required CUDA driver symbol missing\n");
		return 1;
	}

	ret = p_cuInit(0);
	if (!ret)
		ret = p_cuDeviceGet(&dev, 0);
	if (!ret)
		ret = p_cuCtxCreate(&ctx, 0, dev);
	if (!ret)
		ret = p_cuMemAllocManaged(&live, LIVE_BYTES, CU_MEM_ATTACH_GLOBAL);
	if (ret) {
		fprintf(stderr, "FAIL CUDA setup rc=%d (%s)\n", ret, errname(ret));
		return 1;
	}

	p = (unsigned char *)(uintptr_t)live;
	for (i = 0; i < LIVE_BYTES; i++)
		p[i] = pattern(i);
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || LIVE_BYTES < (size_t)page_size * 3u ||
	    pipe(ready) || pipe(done)) {
		perror("FAIL setup");
		return 1;
	}

	child = fork();
	if (child < 0) {
		perror("FAIL fork");
		return 1;
	}
	if (!child) {
		close(ready[0]);
		close(done[1]);
		/* A middle-page hole forces a split and leaves two inherited VMAs. */
		if (munmap(p + page_size, (size_t)page_size)) {
			perror("FAIL child partial munmap");
			_exit(2);
		}
		if (one_byte(ready[1], 'R') || wait_byte(done[0]))
			_exit(3);
		if (check_span(p, 0, (size_t)page_size, "child prefix") ||
		    check_span(p, (size_t)page_size * 2u, LIVE_BYTES,
			       "child suffix"))
			_exit(4);
		_exit(0);
	}

	close(ready[1]);
	close(done[0]);
	if (wait_byte(ready[0])) {
		fprintf(stderr, "FAIL child did not complete partial munmap\n");
		return 1;
	}
	if (check_span(p, 0, LIVE_BYTES, "parent before churn"))
		return 1;

	for (i = 0; i < CHURN_EXTENTS; i++) {
		CUdeviceptr churn = 0;

		ret = p_cuMemAllocManaged(&churn, CHURN_BYTES,
					  CU_MEM_ATTACH_GLOBAL);
		if (ret) {
			fprintf(stderr, "FAIL churn alloc %u rc=%d (%s)\n",
				i, ret, errname(ret));
			return 1;
		}
		memset((void *)(uintptr_t)churn, (int)i, CHURN_BYTES);
		ret = p_cuMemFree(churn);
		if (ret) {
			fprintf(stderr, "FAIL churn free %u rc=%d (%s)\n",
				i, ret, errname(ret));
			return 1;
		}
	}

	if (check_span(p, 0, LIVE_BYTES, "parent after churn") ||
	    one_byte(done[1], 'D'))
		return 1;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		fprintf(stderr, "FAIL child status=0x%x\n", status);
		return 1;
	}
	ret = p_cuMemFree(live);
	if (ret) {
		fprintf(stderr, "FAIL live free rc=%d (%s)\n", ret, errname(ret));
		return 1;
	}
	printf("PASS split/fork VMA survived %u extents / %u MiB of GPA churn\n",
	       CHURN_EXTENTS,
	       (unsigned)(CHURN_EXTENTS * CHURN_BYTES / (1024u * 1024u)));
	return 0;
}
