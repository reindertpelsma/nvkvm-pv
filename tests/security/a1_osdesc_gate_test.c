/*
 * a1_osdesc_gate_test.c — adversarial probe for the A-1 gate, and a check that
 * the legitimate path it protects still works.
 *
 * Runs INSIDE the guest.  A-1: NV_ESC_RM_ALLOC_MEMORY (nr 0x27) with
 * hClass == NV01_MEMORY_SYSTEM_OS_DESCRIPTOR (0x71) hands `pMemory` to
 * pin_user_pages() on the CALLING TASK — the stub.  Unvalidated, that pins an
 * arbitrary range of a host process's address space, which nr 0x4e can then map
 * back into the guest.
 *
 * Two halves, and BOTH must hold:
 *
 *   NEGATIVE  an OS descriptor over an address the host never installed in this
 *             isolate must be refused.
 *   POSITIVE  cuMemHostRegister — the real U-14 consumer, what cudaHostRegister
 *             calls — must still succeed.  A gate that only passes the negative
 *             half has broken host registration and is a regression, not a fix.
 *
 * The refusal is RM-shaped: the ioctl SUCCEEDS and the status field carries the
 * error, exactly as the alloc-class gate does, because failing the ioctl
 * outright is a far more severe signal than anything the real driver produces.
 * So this checks `status`, not the return value.
 *
 *   gcc -O0 -o /tmp/a1_osdesc_gate_test a1_osdesc_gate_test.c -ldl && /tmp/a1_osdesc_gate_test
 *
 * READ THIS BEFORE TRUSTING A PASS.  Run from guest userspace against the STOCK
 * module, the negative probes are refused by the GUEST MODULE, not by the host
 * gate: the module tries to migrate the range first (nvkvm_ioctl.c:409-440) and
 * fails on an address the guest does not own, so the ioctl never leaves the
 * guest.  The probes pass, and would keep passing with the host gate removed.
 *
 * The threat model is a malicious guest KERNEL, which simply would not migrate.
 * To exercise the host gate, build a module that forwards the raw ioctl:
 *
 *   cp -r /mnt/nvkvm/src /tmp/bp/src && cd /tmp/bp/src/guest
 *   L=$(grep -n '^#include' nvkvm_ioctl.c | tail -1 | cut -d: -f1)
 *   sed -i "${L}a #define nvkvm_cpu_pages_migrate_range(a,b,c,d) 0" nvkvm_ioctl.c
 *   make KDIR=/lib/modules/$(uname -r)/build
 *   sudo rmmod nvkvm_guest && sudo insmod ./nvkvm-guest.ko
 *
 * Then the probe reaches the host and MUST come back status=0x1e
 * (NV_ERR_INVALID_ADDRESS) with "DENY OS_DESCRIPTOR ... (A-1)" in QEMU's log.
 * Measured on an RTX 3060 / 575.51.03: exactly that, and cuMemHostRegister
 * still succeeds on the stock module.  Restore the stock module afterwards.
 *
 * Exit: 0 = gate holds and registration still works, 1 = otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define NV01_ROOT                        0x00000000u
#define NV01_MEMORY_SYSTEM_OS_DESCRIPTOR 0x00000071u
#define NV_ERR_INVALID_ADDRESS           0x1eu

/* NVOS21 (RM_ALLOC) prefix: hRoot@0 hObjectParent@4 hObjectNew@8 hClass@12 */
struct nvos21 {
	uint32_t h_root, h_object_parent, h_object_new, h_class;
	uint64_t p_alloc_parms;
	uint32_t status, pad;
};

/* nv_ioctl_nvos02_parameters_with_fd — src/abi/nvgpu.h */
struct nvos02 {
	uint32_t h_root, h_object_parent, h_object_new, h_class;
	uint32_t flags, reserved;
	uint64_t p_memory;
	uint64_t limit;
	uint32_t status, pad1;
	int32_t  fd;
	uint32_t pad0;
};

#define NV_ESC_RM_ALLOC        _IOWR('F', 0x2b, struct nvos21)
#define NV_ESC_RM_ALLOC_MEMORY _IOWR('F', 0x27, struct nvos02)

static int failures;

static void expect_refused(const char *what, int rc, uint32_t status)
{
	if (rc == 0 && status == 0) {
		printf("  FAIL %-38s ACCEPTED (status=0)\n", what);
		failures++;
	} else {
		printf("  ok   %-38s refused (rc=%d status=0x%x%s)\n", what, rc,
		       status,
		       status == NV_ERR_INVALID_ADDRESS ? " INVALID_ADDRESS" : "");
	}
}

int main(void)
{
	int ctl = open("/dev/nvidiactl", O_RDWR);
	uint32_t hclient = 0;

	if (ctl < 0) {
		printf("SKIP: /dev/nvidiactl: %s\n", strerror(errno));
		return 0;
	}

	/* A real client first, so the probe is refused by A-1 rather than by
	 * H-3's hClient-ownership gate — otherwise this test would pass for the
	 * wrong reason and keep passing if A-1 were removed. */
	{
		struct nvos21 a;
		memset(&a, 0, sizeof a);
		a.h_class = NV01_ROOT;
		if (ioctl(ctl, NV_ESC_RM_ALLOC, &a) == 0 && a.status == 0)
			hclient = a.h_object_new;
		printf("  ..   root client                          hClient=0x%x status=0x%x\n",
		       hclient, a.status);
	}
	if (!hclient) {
		printf("SKIP: could not allocate a root client; A-1 probe would be\n"
		       "      refused by H-3 instead and would prove nothing.\n");
		close(ctl);
		return 0;
	}

	/* ── NEGATIVE: descriptors over ranges the host never installed ── */
	{
		struct nvos02 m;
		/* Squarely where a host process's libraries and heap live. */
		static const uint64_t probes[] = {
			0x7f0000000000ULL,   /* stub .so / heap territory */
			0x000055a000000000ULL, /* PIE text territory       */
			0x1000ULL,           /* low, obviously never ours  */
		};
		static const char *names[] = {
			"A-1 OS_DESCRIPTOR(host libs)",
			"A-1 OS_DESCRIPTOR(host text)",
			"A-1 OS_DESCRIPTOR(low addr)",
		};
		for (unsigned i = 0; i < 3; i++) {
			memset(&m, 0, sizeof m);
			m.h_root          = hclient;
			m.h_object_parent = hclient;
			m.h_object_new    = 0xa1000001u + i;
			m.h_class         = NV01_MEMORY_SYSTEM_OS_DESCRIPTOR;
			m.p_memory        = probes[i];
			m.limit           = 0xfffULL;   /* size - 1 */
			m.fd              = -1;
			expect_refused(names[i],
				       ioctl(ctl, NV_ESC_RM_ALLOC_MEMORY, &m),
				       m.status);
		}

		/* A range this process legitimately owns is still not one the
		 * HOST installed in the isolate — the gate is about the stub's
		 * address space, not the guest's. */
		void *mine = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mine != MAP_FAILED) {
			memset(&m, 0, sizeof m);
			m.h_root          = hclient;
			m.h_object_parent = hclient;
			m.h_object_new    = 0xa1000010u;
			m.h_class         = NV01_MEMORY_SYSTEM_OS_DESCRIPTOR;
			m.p_memory        = (uint64_t)(uintptr_t)mine;
			m.limit           = 0xfffULL;
			m.fd              = -1;
			expect_refused("A-1 OS_DESCRIPTOR(guest-own anon)",
				       ioctl(ctl, NV_ESC_RM_ALLOC_MEMORY, &m),
				       m.status);
			munmap(mine, 0x1000);
		}
	}
	close(ctl);

	/* ── POSITIVE: the feature A-1 must not break ──────────────────── */
	{
		void *lib = dlopen("libcuda.so.1", RTLD_NOW);
		int (*cuInit)(unsigned) = NULL;
		int (*cuDeviceGet)(int *, int) = NULL;
		int (*cuCtxRetain)(void **, int) = NULL;
		int (*cuCtxSetCurrent)(void *) = NULL;
		int (*cuMemHostRegister)(void *, size_t, unsigned) = NULL;
		int (*cuMemHostUnregister)(void *) = NULL;

		if (!lib) {
			printf("  SKIP registration probe: libcuda: %s\n", dlerror());
		} else {
			*(void **)&cuInit             = dlsym(lib, "cuInit");
			*(void **)&cuDeviceGet        = dlsym(lib, "cuDeviceGet");
			*(void **)&cuCtxRetain        = dlsym(lib, "cuDevicePrimaryCtxRetain");
			*(void **)&cuCtxSetCurrent    = dlsym(lib, "cuCtxSetCurrent");
			*(void **)&cuMemHostRegister  = dlsym(lib, "cuMemHostRegister");
			*(void **)&cuMemHostUnregister= dlsym(lib, "cuMemHostUnregister");
		}
		if (cuInit && cuMemHostRegister && cuInit(0) == 0) {
			int dev = 0; void *ctx = NULL;
			cuDeviceGet(&dev, 0);
			cuCtxRetain(&ctx, dev);
			cuCtxSetCurrent(ctx);
			/*
			 * SIZES MATTER HERE, and 2 MiB alone does not test it.
			 * The migration installs the range in 2 MiB chunks, one
			 * table entry per chunk, so a registration above one
			 * chunk spans several entries.  A gate that demands
			 * containment in a single entry passes at 2 MiB and
			 * refuses everything larger -- which is precisely the
			 * regression a 2 MiB-only probe missed once already.
			 * 16 MiB is 8 chunks; 96 MiB is 48 and is past 64 MiB.
			 */
			static const size_t sizes[] = {
				2u << 20, 16u << 20, 96u << 20,
			};
			for (unsigned i = 0; i < 3; i++) {
				size_t sz = sizes[i];
				char label[64];
				void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (p == MAP_FAILED) continue;
				memset(p, 0xa5, sz);
				snprintf(label, sizeof label,
					 "U-14 cuMemHostRegister %zu MiB",
					 sz >> 20);
				int rc = cuMemHostRegister(p, sz, 0x01 /* PORTABLE */);
				if (rc == 0) {
					printf("  ok   %-38s registered (rc=0, ~%zu chunks)\n",
					       label, (sz + (2u<<20) - 1) / (2u<<20));
					if (cuMemHostUnregister) cuMemHostUnregister(p);
				} else {
					printf("  FAIL %-38s rc=%d — A-1 broke host registration\n",
					       label, rc);
					failures++;
				}
				munmap(p, sz);
			}
		} else {
			printf("  SKIP registration probe: CUDA unavailable\n");
		}
	}

	printf(failures ? "A1_GATE_TEST FAIL (%d)\n" : "A1_GATE_TEST PASS (%d accepted)\n",
	       failures);
	return failures ? 1 : 0;
}
