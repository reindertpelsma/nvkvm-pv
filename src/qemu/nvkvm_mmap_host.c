/*
 * nvkvm_mmap_host.c — GPU memory mapping management (host side)
 *
 * When the guest requests a mmap on a /dev/nvidia* fd, we:
 *   1. Call mmap() on the real host nvidia fd to get a host VA backed by GPU
 *      memory (BAR framebuffer, command ring, doorbell page, etc.).
 *   2. Register those host pages as a new KVM memory slot so they become
 *      visible at a guest physical address (GPA).
 *   3. Return the GPA to the guest, which then calls remap_pfn_range() in its
 *      kernel module to complete the process-level VMA mapping.
 *
 * On munmap, we remove the KVM memory slot and munmap() the host VA.
 *
 * IOMMU / cache coherence
 * =======================
 * NVIDIA GPU memory mappings fall into two categories:
 *
 *   a) Framebuffer / VRAM (BAR2): Write-Combine on x86, or MT_WRITE_COMBINE
 *      on ARM. These are MMIO pages. We set KVM_MEM_READONLY for read-only
 *      BAR regions and use KVM_SET_USER_MEMORY_REGION with appropriate flags.
 *
 *   b) System memory (pinned, write-back): These are ordinary DRAM pages that
 *      the NVIDIA driver has pinned. We use KVM_SET_USER_MEMORY_REGION
 *      directly; the guest should see them as write-back cached.
 *
 * We infer the memory type from the PROT flags and the device type:
 *   - /dev/nvidiactl and /dev/nvidia* BAR mmaps → write-combine
 *   - /dev/nvidia-uvm and system descriptor mmaps → write-back
 *
 * DMA safety
 * ==========
 * The NVIDIA driver has already pinned (and possibly DMA-mapped) the physical
 * pages before returning from the mmap call. We do not need additional VFIO
 * DMA-mapping for simple user-space compute workloads. For scenarios
 * requiring proper IOMMU passthrough (e.g., DMA engines writing directly into
 * guest-owned memory), VFIO integration is a future extension point.
 */

#include "qemu/osdep.h"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "virtio_nvgpu.h"

/*
 * Inline KVM memory region API — avoids including <linux/kvm.h> which
 * conflicts with QEMU's internal header chain (resettable.h typedef issue).
 * Values are stable ABI constants on Linux x86-64.
 */
#ifndef KVM_SET_USER_MEMORY_REGION
#define NVKVM_KVMIO                     0xAE
#define KVM_MEM_READONLY                (1UL << 1)
#define KVM_SET_USER_MEMORY_REGION      _IOW(NVKVM_KVMIO, 0x46, \
					     struct nvkvm_kvm_mem_region)
struct nvkvm_kvm_mem_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;
	uint64_t userspace_addr;
};
#else
typedef struct kvm_userspace_memory_region nvkvm_kvm_mem_region;
#endif

/* KVM VM fd opened at device init */
static int kvm_vm_fd = -1;
/* Exposed for nvkvm_isolate_handlers.c */
int nvkvm_kvm_vm_fd = -1;

void nvkvm_set_kvm_vm_fd(int fd)
{
	kvm_vm_fd     = fd;
	nvkvm_kvm_vm_fd = fd;
}

/* ── GPA allocator ────────────────────────────────────────────────────────── */

/*
 * Allocate a contiguous GPA range in the mmap window.
 * The window is a reserved region in the guest physical address space that
 * QEMU pre-configures as empty (no backing RAM) specifically for these
 * dynamic GPU mappings.
 *
 * Alignment: mappings are aligned to PAGE_SIZE (4 KiB). For BAR mappings
 * that must be 2 MiB aligned (huge pages), we align up to 2 MiB.
 */
static uint64_t alloc_gpa(VirtIONvgpu *nv, size_t length)
{
	uint64_t gpa;
	size_t align = (length >= (2 << 20)) ? (2 << 20) : 4096;

	pthread_mutex_lock(&nv->mmap_win_lock);
	/* Align current pointer */
	nv->mmap_win_cur = (nv->mmap_win_cur + align - 1) & ~(align - 1);

	if (nv->mmap_win_cur + length > nv->mmap_win_size) {
		pthread_mutex_unlock(&nv->mmap_win_lock);
		fprintf(stderr, "nvkvm: mmap window exhausted\n");
		return 0;
	}

	gpa = nv->mmap_win_gpa + nv->mmap_win_cur;
	nv->mmap_win_cur += length;
	/* every 64 MB consumed, print where we are */
	if ((nv->mmap_win_cur & ((64UL << 20) - 1)) < length)
		NVKVM_DBG(
			"nvkvm: mmap_win used=%llu MB / %llu MB\n",
			(unsigned long long)(nv->mmap_win_cur >> 20),
			(unsigned long long)(nv->mmap_win_size >> 20));
	pthread_mutex_unlock(&nv->mmap_win_lock);
	return gpa;
}

/* Thin wrapper used by nvkvm_isolate_handlers.c for double-mmap GPA allocation */
void nvkvm_mmap_win_alloc(VirtIONvgpu *nv, size_t length, uint64_t *gpa_out)
{
	*gpa_out = alloc_gpa(nv, length);
}

/* ── Sparse GPA window ────────────────────────────────────────────────────── */

/* Forward decls — definitions follow this block. */
static int  kvm_add_memory_region(uint64_t gpa, void *hva, size_t length,
				   bool readonly, int *slot_out);
static void kvm_remove_memory_region(int slot);

/*
 * One-shot setup of the sparse window — large MAP_NORESERVE region in
 * QEMU's mm + a single KVM memory slot covering the whole GPA range.
 * Host kernel demand-faults pages on first access by either side
 * (QEMU CPU, nvidia driver via DMA, or guest via EPT).
 *
 * Called from virtio_nvgpu_device_realize after the kvm-vm fd is known.
 */
int nvkvm_sparse_init(VirtIONvgpu *nv)
{
	if (nv->sparse_vmm_va) return 0;  /* already initialised */

	void *va = mmap(NULL, NVKVM_SPARSE_GPA_SIZE,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
			-1, 0);
	if (va == MAP_FAILED) {
		NVKVM_DBG(
			"nvkvm_sparse_init: mmap %llu GiB failed: %s\n",
			(unsigned long long)(NVKVM_SPARSE_GPA_SIZE >> 30),
			strerror(errno));
		return -errno;
	}

	/*
	 * #55: do NOT raw-install the KVM memslot here.  The window's GPA is the
	 * firmware-assigned base of the reservation BAR, which isn't known until
	 * the guest programs the BAR (after device realize).  We only reserve the
	 * host VMM buffer now; nvkvm_sparse_ensure() installs the memslot at the
	 * resolved base on first use (or falls back to the fixed base).
	 */
	pthread_mutex_init(&nv->sparse_lock, NULL);
	nv->sparse_gpa_base = 0;
	nv->sparse_size     = NVKVM_SPARSE_GPA_SIZE;
	nv->sparse_vmm_va   = va;
	nv->sparse_cur      = 0;
	nv->sparse_kvm_slot = -1;
	/* #80/H-1: window free-list (recycled extents). */
	nv->sparse_free   = g_new0(struct nvkvm_gpa_extent, NVKVM_GPA_FREE_MAX);
	nv->sparse_free_n = 0;
	NVKVM_DBG("nvkvm_sparse_init: %llu GiB VMM buffer %p (memslot deferred to BAR base)\n",
		  (unsigned long long)(NVKVM_SPARSE_GPA_SIZE >> 30), va);
	return 0;
}

/*
 * #55: resolve the window base (BAR-assigned, else fixed fallback) and install
 * the single raw KVM memslot there exactly once.  Returns the base GPA, or 0 if
 * the window buffer is unavailable / the install failed.
 */
uint64_t nvkvm_sparse_ensure(VirtIONvgpu *nv)
{
	if (!nv->sparse_vmm_va)
		return 0;
	pthread_mutex_lock(&nv->sparse_lock);
	if (nv->sparse_kvm_slot >= 0) {
		uint64_t b = nv->sparse_gpa_base;
		pthread_mutex_unlock(&nv->sparse_lock);
		return b;
	}
	uint64_t base;
	if (nv->window_base_get) {
		/* BAR transport: use its firmware-assigned GPA.  If 0, the guest
		 * hasn't programmed the BAR yet (e.g. an early config read during
		 * PCI enumeration) — do NOT install at a fallback now, or we'd
		 * cache the wrong base; wait for a later call once it's mapped. */
		base = nv->window_base_get(nv->window_base_opaque);
		if (base == 0) {
			pthread_mutex_unlock(&nv->sparse_lock);
			return 0;
		}
	} else {
		base = NVKVM_SPARSE_GPA_BASE;   /* no BAR transport — fixed fallback */
	}
	int slot = -1;
	int rc = kvm_add_memory_region(base, nv->sparse_vmm_va,
				       nv->sparse_size, false, &slot);
	if (rc) {
		pthread_mutex_unlock(&nv->sparse_lock);
		fprintf(stderr, "nvkvm: sparse memslot install at GPA=0x%llx failed: %d\n",
			(unsigned long long)base, rc);
		return 0;
	}
	nv->sparse_gpa_base = base;
	nv->sparse_kvm_slot = slot;
	pthread_mutex_unlock(&nv->sparse_lock);
	NVKVM_DBG("nvkvm_sparse_ensure: %llu GiB at GPA=0x%llx slot=%d\n",
		  (unsigned long long)(nv->sparse_size >> 30),
		  (unsigned long long)base, slot);
	return base;
}

void nvkvm_sparse_fini(VirtIONvgpu *nv)
{
	if (!nv->sparse_vmm_va) return;
	if (nv->sparse_kvm_slot >= 0) kvm_remove_memory_region(nv->sparse_kvm_slot);
	munmap(nv->sparse_vmm_va, nv->sparse_size);
	nv->sparse_vmm_va = NULL;
	g_free(nv->sparse_free);
	nv->sparse_free   = NULL;
	nv->sparse_free_n = 0;
	pthread_mutex_destroy(&nv->sparse_lock);
}

uint64_t nvkvm_sparse_gpa_alloc(VirtIONvgpu *nv, size_t size)
{
	if (!nv->sparse_vmm_va) return 0;
	/* #55: install the memslot at the resolved (BAR-assigned) base on first
	 * use; returns 0 if the window couldn't be installed. */
	if (nvkvm_sparse_ensure(nv) == 0) return 0;
	size = (size + 4095) & ~4095ULL;
	pthread_mutex_lock(&nv->sparse_lock);

	/*
	 * #80/H-1: first-fit reuse from the free-list before advancing the bump
	 * pointer, so a mmap/munmap (or cuMemAlloc/Free) loop recycles window
	 * space instead of leaking it.  Prefer the smallest sufficient extent to
	 * limit fragmentation; on an oversize extent, carve from its front and
	 * leave the remainder on the list.
	 */
	if (nv->sparse_free) {
		uint32_t best = nv->sparse_free_n;
		for (uint32_t i = 0; i < nv->sparse_free_n; i++) {
			if (nv->sparse_free[i].len >= size &&
			    (best == nv->sparse_free_n ||
			     nv->sparse_free[i].len < nv->sparse_free[best].len))
				best = i;
		}
		if (best < nv->sparse_free_n) {
			uint64_t off = nv->sparse_free[best].off;
			if (nv->sparse_free[best].len == size) {
				/* exact: drop the slot (swap-remove) */
				nv->sparse_free[best] =
					nv->sparse_free[--nv->sparse_free_n];
			} else {
				/* carve from the front, keep the remainder */
				nv->sparse_free[best].off += size;
				nv->sparse_free[best].len -= size;
			}
			pthread_mutex_unlock(&nv->sparse_lock);
			return nv->sparse_gpa_base + off;
		}
	}

	uint64_t off = (nv->sparse_cur + 4095) & ~4095ULL;
	if (off + size > nv->sparse_size) {
		pthread_mutex_unlock(&nv->sparse_lock);
		fprintf(stderr, "nvkvm_sparse_gpa_alloc: window exhausted\n");
		return 0;
	}
	nv->sparse_cur = off + size;
	if ((nv->sparse_cur & ((256UL << 20) - 1)) < size)
		NVKVM_DBG(
			"nvkvm: sparse_win used=%llu MB / %llu MB\n",
			(unsigned long long)(nv->sparse_cur >> 20),
			(unsigned long long)(nv->sparse_size >> 20));
	pthread_mutex_unlock(&nv->sparse_lock);
	return nv->sparse_gpa_base + off;
}

/*
 * #80/H-1: return [gpa, gpa+size) to the window free-list.  Coalesces with the
 * bump watermark (fast path for LIFO free, keeps the free-list empty under
 * same-size churn) and with an adjacent free extent; otherwise appends.  If the
 * free-list is full it logs once and leaks the extent — bounded degradation,
 * never a crash.
 */
void nvkvm_sparse_gpa_free(VirtIONvgpu *nv, uint64_t gpa, size_t size)
{
	if (!nv->sparse_vmm_va || !nv->sparse_free) return;
	if (gpa < nv->sparse_gpa_base) return;
	uint64_t off = gpa - nv->sparse_gpa_base;
	size = (size + 4095) & ~4095ULL;
	if (size == 0 || off + size > nv->sparse_size) return;

	pthread_mutex_lock(&nv->sparse_lock);

	/* Fast path: freeing the current tail just lowers the watermark, then
	 * absorbs any free extents that became adjacent to it. */
	if (off + size == nv->sparse_cur) {
		nv->sparse_cur = off;
		bool merged = true;
		while (merged) {
			merged = false;
			for (uint32_t i = 0; i < nv->sparse_free_n; i++) {
				if (nv->sparse_free[i].off + nv->sparse_free[i].len
				    == nv->sparse_cur) {
					nv->sparse_cur = nv->sparse_free[i].off;
					nv->sparse_free[i] =
						nv->sparse_free[--nv->sparse_free_n];
					merged = true;
					break;
				}
			}
		}
		pthread_mutex_unlock(&nv->sparse_lock);
		return;
	}

	/* Coalesce with an adjacent existing free extent. */
	for (uint32_t i = 0; i < nv->sparse_free_n; i++) {
		if (nv->sparse_free[i].off + nv->sparse_free[i].len == off) {
			nv->sparse_free[i].len += size;       /* extend upward   */
			pthread_mutex_unlock(&nv->sparse_lock);
			return;
		}
		if (off + size == nv->sparse_free[i].off) {
			nv->sparse_free[i].off  = off;        /* extend downward */
			nv->sparse_free[i].len += size;
			pthread_mutex_unlock(&nv->sparse_lock);
			return;
		}
	}

	if (nv->sparse_free_n < NVKVM_GPA_FREE_MAX) {
		nv->sparse_free[nv->sparse_free_n].off = off;
		nv->sparse_free[nv->sparse_free_n].len = size;
		nv->sparse_free_n++;
	} else {
		static bool warned;
		if (!warned) {
			fprintf(stderr, "nvkvm: GPA free-list full (%d); "
				"leaking a window extent\n", NVKVM_GPA_FREE_MAX);
			warned = true;
		}
	}
	pthread_mutex_unlock(&nv->sparse_lock);
}

void *nvkvm_gpa_to_vmm_va(VirtIONvgpu *nv, uint64_t gpa, size_t size)
{
	if (!nv->sparse_vmm_va) return NULL;
	if (gpa < nv->sparse_gpa_base) return NULL;
	uint64_t off = gpa - nv->sparse_gpa_base;
	if (off >= nv->sparse_size || off + size > nv->sparse_size) return NULL;
	return (char *)nv->sparse_vmm_va + off;
}

/* ── KVM memory slot management ───────────────────────────────────────────── */

/*
 * KVM memory slot pool.
 *
 * KVM_CAP_NR_MEMSLOTS is typically 512 on x86_64.  We carve out slots
 * [NVKVM_KVM_SLOT_BASE, NVKVM_KVM_SLOT_BASE + NVKVM_KVM_SLOT_COUNT) for
 * dynamic GPU mappings; below the base is reserved for QEMU's static
 * regions (RAM, BIOS, virtio bars, etc.).
 *
 * Until 2026-05-28 this was a monotonic counter `next_kvm_slot++` that
 * never recycled — after a few CUDA processes the pool was exhausted,
 * KVM_SET_USER_MEMORY_REGION started returning -EINVAL, and the guest
 * wedged on the next mmap.  Now: a freelist of recycled slots + a
 * watermark for never-used slots.  Allocation prefers the freelist so
 * KVM's internal LRU has a chance to age out stale EPT entries before
 * the same slot number is reused.
 */
#define NVKVM_KVM_SLOT_BASE   64
#define NVKVM_KVM_SLOT_COUNT  448      /* 64..511 inclusive */

static pthread_mutex_t kvm_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int             kvm_slot_water = NVKVM_KVM_SLOT_BASE; /* next never-used */
static int             kvm_slot_free_head;                   /* freelist top  */
static int             kvm_slot_free_stack[NVKVM_KVM_SLOT_COUNT];

/* Diagnostics — read+printed under kvm_slot_lock. */
static int             kvm_slot_in_use;        /* live slots */
static int             kvm_slot_in_use_peak;   /* watermark of live slots */
static uint64_t        kvm_slot_alloc_count;   /* lifetime allocs */
static uint64_t        kvm_slot_free_count;    /* lifetime frees  */

int nvkvm_kvm_slot_alloc(void)
{
	int slot = -1;
	pthread_mutex_lock(&kvm_slot_lock);
	if (kvm_slot_free_head > 0) {
		slot = kvm_slot_free_stack[--kvm_slot_free_head];
	} else if (kvm_slot_water <
		   NVKVM_KVM_SLOT_BASE + NVKVM_KVM_SLOT_COUNT) {
		slot = kvm_slot_water++;
	}
	if (slot >= 0) {
		kvm_slot_alloc_count++;
		if (++kvm_slot_in_use > kvm_slot_in_use_peak)
			kvm_slot_in_use_peak = kvm_slot_in_use;
	}
	pthread_mutex_unlock(&kvm_slot_lock);
	return slot;
}

void nvkvm_kvm_slot_release(int slot)
{
	if (slot < NVKVM_KVM_SLOT_BASE ||
	    slot >= NVKVM_KVM_SLOT_BASE + NVKVM_KVM_SLOT_COUNT)
		return;
	pthread_mutex_lock(&kvm_slot_lock);
	if (kvm_slot_free_head < NVKVM_KVM_SLOT_COUNT)
		kvm_slot_free_stack[kvm_slot_free_head++] = slot;
	kvm_slot_free_count++;
	if (kvm_slot_in_use > 0)
		kvm_slot_in_use--;
	pthread_mutex_unlock(&kvm_slot_lock);
}

/* Diagnostic — declared in virtio_nvgpu.h, exposed for QEMU-side prints. */
void nvkvm_kvm_slot_stats(int *in_use, int *peak,
			  uint64_t *allocs, uint64_t *frees)
{
	pthread_mutex_lock(&kvm_slot_lock);
	if (in_use) *in_use = kvm_slot_in_use;
	if (peak)   *peak   = kvm_slot_in_use_peak;
	if (allocs) *allocs = kvm_slot_alloc_count;
	if (frees)  *frees  = kvm_slot_free_count;
	pthread_mutex_unlock(&kvm_slot_lock);
}

static int kvm_add_memory_region(uint64_t gpa, void *hva, size_t length,
				 bool readonly, int *slot_out)
{
	int slot = nvkvm_kvm_slot_alloc();
	if (slot < 0) {
		int in_use, peak;
		uint64_t allocs, frees;
		nvkvm_kvm_slot_stats(&in_use, &peak, &allocs, &frees);
		NVKVM_DBG(
			"nvkvm: KVM slot pool EXHAUSTED — in_use=%d peak=%d "
			"lifetime alloc/free=%llu/%llu (cap=%d)\n",
			in_use, peak,
			(unsigned long long)allocs,
			(unsigned long long)frees,
			NVKVM_KVM_SLOT_COUNT);
		return -ENOSPC;
	}

	struct nvkvm_kvm_mem_region region = {
		.slot            = (uint32_t)slot,
		.flags           = readonly ? (uint32_t)KVM_MEM_READONLY : 0,
		.guest_phys_addr = gpa,
		.memory_size     = length,
		.userspace_addr  = (uint64_t)(uintptr_t)hva,
	};
	if (kvm_vm_fd < 0) {
		NVKVM_DBG(
			"nvkvm: kvm_vm_fd not set; GPU mmap will not be "
			"directly accessible in guest\n");
		nvkvm_kvm_slot_release(slot);
		*slot_out = -1;
		return 0;  /* non-fatal for initial bring-up */
	}
	if (ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		NVKVM_DBG(
			"nvkvm: KVM_SET_USER_MEMORY_REGION slot=%d failed: %s\n",
			slot, strerror(errno));
		nvkvm_kvm_slot_release(slot);
		return -errno;
	}

	/* Light per-100-slots progress print so we can spot exhaustion
	 * trends in real time during multi-process testing. */
	pthread_mutex_lock(&kvm_slot_lock);
	int in_use_now = kvm_slot_in_use;
	pthread_mutex_unlock(&kvm_slot_lock);
	if (in_use_now % 100 == 0)
		NVKVM_DBG(
			"nvkvm: kvm slot watermark in_use=%d peak=%d cap=%d\n",
			in_use_now, kvm_slot_in_use_peak,
			NVKVM_KVM_SLOT_COUNT);

	*slot_out = slot;
	return 0;
}

static void kvm_remove_memory_region(int slot)
{
	struct nvkvm_kvm_mem_region region = {
		.slot         = (uint32_t)slot,
		.memory_size  = 0,  /* size=0 removes the slot */
	};
	if (kvm_vm_fd >= 0)
		ioctl(kvm_vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
	nvkvm_kvm_slot_release(slot);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int nvkvm_mmap_create(VirtIONvgpu *nv, struct nvkvm_host_fd *hfd,
		      uint64_t offset, size_t length,
		      int prot, int flags,
		      struct nvkvm_mmap_region **region_out)
{
	void *hva;
	struct nvkvm_mmap_region *region;

	/* Round up to page size */
	length = (length + 4095) & ~4095UL;

	hva = mmap(NULL, length, prot, flags, hfd->fd, (off_t)offset);
	if (hva == MAP_FAILED) {
		NVKVM_DBG(
			"nvkvm: host mmap fd=%d offset=0x%llx len=%zu: %s\n",
			hfd->fd, (unsigned long long)offset, length,
			strerror(errno));
		return -errno;
	}

	region = g_new0(struct nvkvm_mmap_region, 1);
	region->host_va = hva;
	region->length  = length;
	/* guest_pa and kvm_slot filled by nvkvm_mmap_map_to_guest */

	*region_out = region;
	return 0;
}

int nvkvm_mmap_map_to_guest(VirtIONvgpu *nv,
			    struct nvkvm_mmap_region *region)
{
	uint64_t gpa;
	int slot = -1;
	int ret;

	gpa = alloc_gpa(nv, region->length);
	if (!gpa) {
		munmap(region->host_va, region->length);
		return -ENOMEM;
	}

	ret = kvm_add_memory_region(gpa, region->host_va, region->length,
				    false, &slot);
	if (ret) {
		munmap(region->host_va, region->length);
		return ret;
	}

	region->guest_pa = gpa;
	region->kvm_slot = slot;
	return 0;
}

void nvkvm_mmap_unmap_from_guest(VirtIONvgpu *nv,
				 struct nvkvm_mmap_region *region)
{
	if (region->kvm_slot >= 0) {
		kvm_remove_memory_region(region->kvm_slot);
		region->kvm_slot = -1;
	}
	/* GPA is not returned to the pool (simple bump allocator).
	 * A future version could use a proper free-list. */
}

void nvkvm_mmap_destroy(VirtIONvgpu *nv,
			struct nvkvm_mmap_region *region)
{
	if (region->host_va && region->host_va != MAP_FAILED)
		munmap(region->host_va, region->length);
	g_free(region);
}
