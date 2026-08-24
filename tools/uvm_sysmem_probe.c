/*
 * uvm_sysmem_probe.c — does the RM half of the managed-memory fallback work?
 *
 * The fallback backs a "managed" range with an ordinary RM sysmem object:
 *
 *     NV_ESC_RM_ALLOC(hClass = NV01_MEMORY_SYSTEM)     -> hMemory
 *     NV_ESC_RM_MAP_MEMORY(hClient, hDevice, hMemory)  -> pLinearAddress
 *     mmap(/dev/nvidiactl, offset = pLinearAddress)    -> CPU pages
 *
 * Everything after that (CREATE_EXTERNAL_RANGE / MAP_EXTERNAL_ALLOCATION) is
 * already measured; see docs/internal/uvm-va-decoupling.md §6b-6c.  What was
 * NOT measured is this half — specifically which NV_MEMORY_ALLOCATION_PARAMS
 * a caller has to send for class 0x3e to be accepted, which is the one thing
 * the guest module cannot discover at runtime.  So this probe SWEEPS the
 * candidate parameter sets and prints RM's verdict for each, then proves the
 * winner end to end by writing through the CPU mapping and reading it back.
 *
 * Run on bare metal (or in a VM with the GPU passed through) as root:
 *     cc -O2 -I../src -o uvm_sysmem_probe uvm_sysmem_probe.c && ./uvm_sysmem_probe
 *
 * Prints one line per candidate and a final RESULT: line.  Nothing here is
 * nvkvm-specific — it is the driver's own ABI, taken from src/abi/nvgpu.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "abi/nvgpu.h"

#define NV_IOCTL_MAGIC 'F'
#define NVESC(nr, type) _IOWR(NV_IOCTL_MAGIC, (nr), type)

/* nv_ioctl_rm_api_version_t — the version handshake every fd wants first. */
struct rm_api_version {
	uint32_t cmd;
	uint32_t reply;
	char     version_string[64];
};
#define NV_RM_API_VERSION_CMD_STRICT 0
#define NV_RM_API_VERSION_CMD_RELAXED '1'
#define NV_RM_API_VERSION_CMD_OVERRIDE '2'

static int ctl_fd = -1;

static const char *st(uint32_t s)
{
	switch (s) {
	case 0x0:  return "NV_OK";
	case 0x1e: return "INVALID_ADDRESS";
	case 0x1f: return "INVALID_ARGUMENT";
	case 0x2b: return "INVALID_OBJECT_PARENT";
	case 0x1a: return "INSUFFICIENT_RESOURCES";
	case 0x56: return "NOT_SUPPORTED";
	case 0x57: return "OBJECT_NOT_FOUND";
	case 0x66: return "INVALID_CLASS";
	default:   return "?";
	}
}

/* NV_ESC_RM_ALLOC with the NVOS64 form (what libcuda sends today). */
static uint32_t rm_alloc(uint32_t h_root, uint32_t h_parent, uint32_t *h_new,
			 uint32_t h_class, void *params, uint32_t params_size)
{
	struct nvos64_parameters p;
	memset(&p, 0, sizeof p);
	p.h_root          = h_root;
	p.h_object_parent = h_parent;
	p.h_object_new    = *h_new;
	p.h_class         = h_class;
	p.p_alloc_parms   = (uintptr_t)params;
	p.alloc_parms_size = params_size;
	if (ioctl(ctl_fd, NVESC(NV_ESC_RM_ALLOC, struct nvos64_parameters), &p) < 0) {
		fprintf(stderr, "  ioctl RM_ALLOC class=0x%x: %s\n", h_class,
			strerror(errno));
		return 0xffffffffu;
	}
	*h_new = p.h_object_new;
	return p.status;
}

/* The older NVOS21 form, for comparison. */
static uint32_t rm_alloc21(uint32_t h_root, uint32_t h_parent, uint32_t *h_new,
			   uint32_t h_class, void *params)
{
	struct nvos21_parameters p;
	memset(&p, 0, sizeof p);
	p.h_root          = h_root;
	p.h_object_parent = h_parent;
	p.h_object_new    = *h_new;
	p.h_class         = h_class;
	p.p_alloc_parms   = (uintptr_t)params;
	if (ioctl(ctl_fd, NVESC(NV_ESC_RM_ALLOC, struct nvos21_parameters), &p) < 0) {
		fprintf(stderr, "  ioctl RM_ALLOC(21) class=0x%x: %s\n", h_class,
			strerror(errno));
		return 0xffffffffu;
	}
	*h_new = p.h_object_new;
	return p.status;
}

static uint32_t rm_free(uint32_t h_root, uint32_t h_parent, uint32_t h_old)
{
	struct nvos00_parameters p;
	memset(&p, 0, sizeof p);
	p.h_root = h_root;
	p.h_object_parent = h_parent;
	p.h_object_old = h_old;
	if (ioctl(ctl_fd, NVESC(NV_ESC_RM_FREE, struct nvos00_parameters), &p) < 0)
		return 0xffffffffu;
	return p.status;
}

/* One candidate NV_MEMORY_ALLOCATION_PARAMS shape. */
struct cand {
	const char *name;
	uint32_t owner;
	uint32_t type;
	uint32_t flags;
	uint32_t attr;
	uint32_t attr2;
	uint64_t alignment;
};

/*
 * NVOS32 bitfields, spelled out rather than DRF-macroed so this file needs no
 * NVIDIA headers.  Cross-check against ogkm src/common/sdk/nvidia/inc/nvos.h.
 *   NVOS32_ATTR_LOCATION            26:25   _PCI = 1  (system memory)
 *   NVOS32_ATTR_PHYSICALITY         28:27   _NONCONTIGUOUS = 1
 *   NVOS32_ATTR_COHERENCY           31:29   _CACHED = 3, _WRITE_BACK = 5
 *   NVOS32_ATTR2_GPU_CACHEABLE       1:0    _NO = 2
 * The sweep exists because these are the values in question: if a candidate is
 * wrong RM says so in `status` and we move on.
 */
#define ATTR_LOC_PCI          (1u << 25)
#define ATTR_LOC_ANY          (3u << 25)
#define ATTR_PHYS_NONCONTIG   (1u << 27)
#define ATTR_PHYS_CONTIG      (2u << 27)
#define ATTR_COH_CACHED       (3u << 29)
#define ATTR_COH_WRITE_BACK   (5u << 29)
#define ATTR2_GPU_CACHEABLE_NO (2u)

#define OWNER_TAG 0x6e766b6du   /* 'nvkm' */

int main(int argc, char **argv)
{
	size_t sz = (argc > 1) ? strtoull(argv[1], NULL, 0) : (4u << 20);
	uint32_t h_client = 0, h_device = 0, h_subdev = 0, h_mem = 0;
	uint32_t s;
	int dev_fd = -1;
	const char *map_win = "?";

	ctl_fd = open("/dev/nvidiactl", O_RDWR);
	if (ctl_fd < 0) { perror("open /dev/nvidiactl"); return 1; }

	{
		struct rm_api_version v;
		char ver[64] = "";
		FILE *f = fopen("/proc/driver/nvidia/version", "r");
		if (f) {
			char line[256];
			if (fgets(line, sizeof line, f)) {
				char *k = strstr(line, "Kernel Module  ");
				if (k) sscanf(k + 15, "%63s", ver);
			}
			fclose(f);
		}
		memset(&v, 0, sizeof v);
		v.cmd = getenv("PROBE_VER_OVERRIDE") ? NV_RM_API_VERSION_CMD_OVERRIDE
						    : NV_RM_API_VERSION_CMD_STRICT;
		if (getenv("PROBE_NO_VER")) goto skip_ver;
		snprintf(v.version_string, sizeof v.version_string, "%s", ver);
		if (ioctl(ctl_fd, NVESC(NV_ESC_CHECK_VERSION_STR, struct rm_api_version), &v) < 0)
			fprintf(stderr, "note: CHECK_VERSION_STR failed: %s\n", strerror(errno));
		printf("CHECK_VERSION_STR   ver=\"%s\" reply=%u returned=\"%s\" cmd=%u\n",
		       ver, v.reply, v.version_string, v.cmd);
skip_ver: ;
	}

	/* Open a device node and bind it to the ctl fd, exactly as libcuda does
	 * before it allocates NV01_DEVICE_0. */
	{
		dev_fd = getenv("PROBE_NO_REG") ? -1 : open("/dev/nvidia0", O_RDWR);
		if (dev_fd < 0) {
			printf("open /dev/nvidia0: %s\n", strerror(errno));
		} else {
			struct nv_ioctl_register_fd r = { .ctl_fd = ctl_fd };
			int rc = ioctl(dev_fd, NVESC(NV_ESC_REGISTER_FD,
						  struct nv_ioctl_register_fd), &r);
			printf("REGISTER_FD         dev_fd=%d rc=%d (%s)\n", dev_fd, rc,
			       rc < 0 ? strerror(errno) : "ok");
		}
	}

	/* 1. client */
	s = rm_alloc(0, 0, &h_client, NV01_ROOT, NULL, 0);
	printf("NV01_ROOT           status=0x%x (%s) hClient=0x%08x\n", s, st(s), h_client);
	if (s != 0) { fprintf(stderr, "RESULT: NO_CLIENT\n"); return 1; }

	/* 2. device (deviceId 0 == the first RM device; sysmem is not tied to a
	 *    particular GPU, the device is only the allocation context). */
	{
		struct { const char *name; uint32_t share; int nvos21; uint32_t psz; } dv[] = {
			{ "64/share=hClient", 1, 0, (uint32_t)sizeof(struct nv0080_alloc_parameters) },
			{ "64/share=0",       0, 0, (uint32_t)sizeof(struct nv0080_alloc_parameters) },
			{ "21/share=0",       0, 1, (uint32_t)sizeof(struct nv0080_alloc_parameters) },
			{ "21/share=hClient", 1, 1, (uint32_t)sizeof(struct nv0080_alloc_parameters) },
			{ "64/share=0/psz=0", 0, 0, 0 },
		};
		for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; i++) {
			struct nv0080_alloc_parameters d;
			uint32_t h = 0;
			memset(&d, 0, sizeof d);
			d.device_id      = 0;
			d.h_client_share = dv[i].share ? h_client : 0;
			s = dv[i].nvos21
			  ? rm_alloc21(h_client, h_client, &h, NV01_DEVICE_0, &d)
			  : rm_alloc(h_client, h_client, &h, NV01_DEVICE_0, &d, dv[i].psz);
			printf("NV01_DEVICE_0[%-16s] status=0x%-4x (%-24s) h=0x%08x\n",
			       dv[i].name, s, st(s), h);
			if (s == 0 && !h_device) { h_device = h; break; }
		}
		if (h_device == 0) { fprintf(stderr, "RESULT: NO_DEVICE\n"); return 1; }
		printf("hDevice=0x%08x\n", h_device);
	}

	/* 3. subdevice — allocated because the allowlist covers it and because a
	 *    device with no subdevice is an unusual shape for RM; not required by
	 *    the memory alloc itself. */
	{
		struct nv2080_alloc_parameters sd;
		memset(&sd, 0, sizeof sd);
		sd.sub_device_id = 0;
		s = rm_alloc(h_client, h_device, &h_subdev, NV20_SUBDEVICE_0,
			     &sd, (uint32_t)sizeof sd);
		printf("NV20_SUBDEVICE_0    status=0x%x (%s) hSubdev=0x%08x\n",
		       s, st(s), h_subdev);
	}

	/* 4. sweep the sysmem allocation parameters. */
	static const struct cand cands[] = {
	  { "zeroed",            0,         0, 0, 0, 0, 0 },
	  { "owner+size only",   OWNER_TAG, 0, 0, 0, 0, 0 },
	  { "PCI",               OWNER_TAG, 0, 0, ATTR_LOC_PCI, 0, 0 },
	  { "PCI+noncontig",     OWNER_TAG, 0, 0, ATTR_LOC_PCI|ATTR_PHYS_NONCONTIG, 0, 0 },
	  { "PCI+noncontig+wb",  OWNER_TAG, 0, 0, ATTR_LOC_PCI|ATTR_PHYS_NONCONTIG|ATTR_COH_WRITE_BACK, ATTR2_GPU_CACHEABLE_NO, 4096 },
	  { "PCI+noncontig+cached", OWNER_TAG, 0, 0, ATTR_LOC_PCI|ATTR_PHYS_NONCONTIG|ATTR_COH_CACHED, ATTR2_GPU_CACHEABLE_NO, 4096 },
	  { "ANY+noncontig",     OWNER_TAG, 0, 0, ATTR_LOC_ANY|ATTR_PHYS_NONCONTIG, 0, 0 },
	  { "PCI+contig",        OWNER_TAG, 0, 0, ATTR_LOC_PCI|ATTR_PHYS_CONTIG, 0, 4096 },
	};
	const struct cand *win = NULL;
	struct nv_memory_allocation_params_v545 mp;

	for (unsigned i = 0; i < sizeof cands / sizeof cands[0]; i++) {
		const struct cand *c = &cands[i];
		uint32_t h = 0;
		memset(&mp, 0, sizeof mp);
		mp.owner     = c->owner;
		mp.type      = c->type;
		mp.flags     = c->flags;
		mp.attr      = c->attr;
		mp.attr2     = c->attr2;
		mp.size      = sz;
		mp.alignment = c->alignment;
		s = rm_alloc(h_client, h_device, &h, NV01_MEMORY_SYSTEM,
			     &mp, (uint32_t)sizeof mp);
		printf("MEMORY_SYSTEM[%-22s] status=0x%-4x (%-22s) h=0x%08x "
		       "attr_out=0x%08x size_out=0x%llx addr_out=0x%llx\n",
		       c->name, s, st(s), h, mp.attr,
		       (unsigned long long)mp.size,
		       (unsigned long long)mp.address);
		if (s == 0) {
			if (!win) { win = c; h_mem = h; continue; }
			rm_free(h_client, h_device, h);
		}
	}
	if (!win) { fprintf(stderr, "RESULT: NO_SYSMEM_ALLOC\n"); return 1; }
	printf("WINNER: %s  hMemory=0x%08x\n", win->name, h_mem);

	/* 5. map it and prove the CPU can reach the pages.
	 *
	 * NV_ESC_RM_MAP_MEMORY stores an "mmap context" on the file named by
	 * `fd` and the subsequent mmap MUST be issued on that same file; the
	 * context is one-shot, so each variant below re-issues the ioctl.
	 */
	{
		/*
		 * `ioctl_on` names the fd the ioctl is issued on; `map_on` names
		 * the fd whose one-shot mmap context it arms and which is then
		 * mmap'd.  ctl2 is a SECOND, freshly opened /dev/nvidiactl:
		 * nvidia_mmap_helper() never clears mmap_context.valid (only
		 * nv_free_file_private() does, at close), so a file can carry
		 * exactly ONE mapping for its whole lifetime and a second
		 * NV_ESC_RM_MAP_MEMORY against it returns NV_ERR_STATE_IN_USE.
		 * "ctl->ctl2" is therefore the shape a caller has to use for more
		 * than one live mapping, and "ctl->ctl x2" proves the constraint.
		 */
		int ctl2 = open("/dev/nvidiactl", O_RDWR);
		struct { const char *name; int ioctl_on_dev; int mmap_on_dev; uint32_t flags; }
		mv[] = {
			{ "ctl->ctl",       0, 0, 0 },
			{ "ctl->ctl x2",    0, 0, 0 },
			{ "ctl->ctl2",      0, 2, 0 },
			{ "dev->dev",       1, 1, 0 },
			{ "ctl->dev",       0, 1, 0 },
			{ "dev->ctl",       1, 0, 0 },
		};
		(void)ctl2;
		int ok = 0;
		for (unsigned i = 0; i < sizeof mv / sizeof mv[0]; i++) {
			struct nv_ioctl_nvos33_parameters_with_fd m;
			int iofd = mv[i].ioctl_on_dev ? dev_fd : ctl_fd;
			int mfd  = mv[i].mmap_on_dev == 2 ? ctl2
				 : mv[i].mmap_on_dev     ? dev_fd : ctl_fd;
			if (iofd < 0 || mfd < 0) continue;
			memset(&m, 0, sizeof m);
			m.h_client = h_client;
			m.h_device = h_device;
			m.h_memory = h_mem;
			m.offset   = 0;
			m.length   = sz;
			m.flags    = mv[i].flags;
			m.fd       = mfd;
			if (ioctl(iofd, NVESC(NV_ESC_RM_MAP_MEMORY,
					      struct nv_ioctl_nvos33_parameters_with_fd), &m) < 0) {
				printf("MAP[%-9s] ioctl: %s\n", mv[i].name, strerror(errno));
				continue;
			}
			if (m.status != 0) {
				printf("MAP[%-9s] status=0x%x (%s)\n", mv[i].name,
				       m.status, st(m.status));
				continue;
			}
			/* nv-mmap.c:513 — `if (vma->vm_pgoff != 0) return -EINVAL`.
			 * pLinearAddress is NOT an mmap offset: NV_ESC_RM_MAP_MEMORY
			 * arms a one-shot mmap_context on the `struct file` named by
			 * m.fd, and the mmap must be at offset 0 on that same file. */
			void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED,
				       mfd, 0);
			printf("MAP[%-9s] status=0x0 tok=0x%llx mmap=%s\n", mv[i].name,
			       (unsigned long long)m.p_linear_address,
			       p == MAP_FAILED ? strerror(errno) : "ok");
			if (p == MAP_FAILED) continue;

			volatile uint32_t *w = p;
			size_t n = sz / 4, bad = 0;
			for (size_t k = 0; k < n; k += 1024) w[k] = (uint32_t)(k ^ 0xa5a5a5a5u);
			for (size_t k = 0; k < n; k += 1024)
				if (w[k] != (uint32_t)(k ^ 0xa5a5a5a5u)) bad++;
			printf("           cpu readback mismatches=%zu of %zu at %p\n",
			       bad, n / 1024, p);
			munmap(p, sz);
			if (!bad && !ok) { ok = 1; map_win = mv[i].name; }
		}
		if (!ok) { fprintf(stderr, "RESULT: NO_MMAP\n"); return 1; }
	}

	printf("RESULT: SYSMEM_BACKING_WORKS  alloc=\"%s\" map=\"%s\" attr=0x%08x "
	       "attr2=0x%08x flags=0x%08x owner=0x%08x align=%llu params_size=%zu\n",
	       win->name, map_win, win->attr, win->attr2, win->flags, win->owner,
	       (unsigned long long)win->alignment, sizeof mp);

	rm_free(h_client, h_device, h_mem);
	if (h_subdev) rm_free(h_client, h_device, h_subdev);
	rm_free(h_client, h_client, h_device);
	rm_free(0, 0, h_client);
	close(ctl_fd);
	return 0;
}
