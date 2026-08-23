/*
 * uvm_va_probe.c — the measurement behind docs/internal/uvm-va-decoupling.md.
 *
 * Answers three questions about a `cuMemAllocManaged` pointer, on whatever GPU
 * and driver you run it on.  Driver API only, so it needs no nvcc:
 *
 *   1. Is the managed pointer the start of a /dev/nvidia-uvm VMA, and is that
 *      VMA's file offset equal to its address?  (uvm_mmap()'s pin, from
 *      userspace.)
 *   2. Is the GPU VA the same number as the CPU VA?
 *      (CU_POINTER_ATTRIBUTE_DEVICE_POINTER)
 *   3. Do concurrent processes pick the same address, or is it ASLR'd?
 *      Run several copies at once and compare.
 *
 * Build:  gcc -O0 -o uvm_va_probe tools/uvm_va_probe.c -lcuda
 * Run:    ./uvm_va_probe [tag] [hold_seconds]
 *         for i in 1 2 3 4; do ./uvm_va_probe P$i 3 & done; wait
 *
 * Measured on RTX 5090 / 580.95.03-open, 2026-08-24: VMA_START_EQ_PTR=YES,
 * GPU_VA_EQ_CPU_VA=YES, and twelve concurrent processes gave twelve distinct
 * addresses.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Declared here rather than including cuda.h: the driver API headers are not
 * always installed next to libcuda, and these four prototypes are stable. */
extern int cuInit(unsigned);
extern int cuDeviceGet(int *, int);
extern int cuDevicePrimaryCtxRetain(void **, int);
extern int cuCtxSetCurrent(void *);
extern int cuMemAllocManaged(unsigned long long *, size_t, unsigned);
extern int cuPointerGetAttribute(void *, int, unsigned long long);

#define CU_POINTER_ATTRIBUTE_DEVICE_POINTER 3
#define CU_MEM_ATTACH_GLOBAL                1

static void report_vma(unsigned long long p, const char *tag)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		unsigned long long lo, hi, off;
		if (sscanf(line, "%llx-%llx %*4s %llx", &lo, &hi, &off) != 3)
			continue;
		if (p < lo || p >= hi)
			continue;
		line[strcspn(line, "\n")] = 0;
		printf("%s MAPS  %s\n", tag, line);
		printf("%s VMA_START_EQ_PTR=%s  VMA_OFFSET_EQ_ADDR=%s\n", tag,
		       lo == p ? "YES" : "NO", off == lo ? "YES" : "NO");
		fclose(f);
		return;
	}
	printf("%s MAPS  ptr=0x%llx is in no VMA\n", tag, p);
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *tag  = argc > 1 ? argv[1] : "P";
	int         hold = argc > 2 ? atoi(argv[2]) : 0;
	size_t      n    = 4ull << 20;
	int dev, rc;
	void *ctx;
	unsigned long long p = 0, gpu = 0;

	if ((rc = cuInit(0)))                            { printf("%s cuInit=%d\n", tag, rc); return 1; }
	if ((rc = cuDeviceGet(&dev, 0)))                 { printf("%s cuDeviceGet=%d\n", tag, rc); return 1; }
	if ((rc = cuDevicePrimaryCtxRetain(&ctx, dev)))  { printf("%s ctxRetain=%d\n", tag, rc); return 1; }
	cuCtxSetCurrent(ctx);

	if ((rc = cuMemAllocManaged(&p, n, CU_MEM_ATTACH_GLOBAL))) {
		/* 1 == CUDA_ERROR_INVALID_VALUE is the signature of "no managed
		 * range exists", i.e. nothing mapped /dev/nvidia-uvm. */
		printf("%s cuMemAllocManaged=%d\n", tag, rc);
		return 1;
	}
	printf("%s MANAGED_PTR=0x%llx size=%zu pid=%d\n", tag, p, n, (int)getpid());
	report_vma(p, tag);

	if (!(rc = cuPointerGetAttribute(&gpu, CU_POINTER_ATTRIBUTE_DEVICE_POINTER, p)))
		printf("%s DEVICE_PTR=0x%llx GPU_VA_EQ_CPU_VA=%s\n", tag, gpu,
		       gpu == p ? "YES" : "NO");
	else
		printf("%s cuPointerGetAttribute=%d\n", tag, rc);

	memset((void *)(uintptr_t)p, 0xab, 4096);
	printf("%s CPU_TOUCH_OK first=0x%02x\n", tag,
	       *(unsigned char *)(uintptr_t)p);

	if (hold) { fflush(stdout); sleep(hold); }
	return 0;
}
