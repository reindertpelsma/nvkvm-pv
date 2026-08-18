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
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#include "hw/boards.h"      /* current_machine->ram_size / maxram_size */
#include "hw/core/cpu.h"    /* first_cpu                               */
#include "qom/object.h"     /* object_property_get_uint("phys-bits")   */

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

/* ── Physical address width + GPA window placement ────────────────────────── */

/*
 * Host MAXPHYADDR.
 *
 * CPUID leaf 0x80000008, EAX bits 7:0 is the authoritative source and is what
 * KVM itself uses to bound a memslot's GPA (kvm_mmu_max_gfn() derives from the
 * host's shadow_phys_bits) — which is exactly the check that rejects a 1 TB
 * window on a 39-bit part.  /proc/cpuinfo's "address sizes" line is the same
 * number formatted for humans and is only a fallback: it is absent on some
 * architectures and can be filtered by container runtimes.
 */
uint32_t nvkvm_host_phys_bits(void)
{
	static uint32_t cached;
	if (cached)
		return cached;

#if defined(__x86_64__) || defined(__i386__)
	{
		uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
		/* Leaf 0x80000008 only exists if the max extended leaf covers it. */
		if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) &&
		    eax >= 0x80000008u &&
		    __get_cpuid(0x80000008u, &eax, &ebx, &ecx, &edx)) {
			uint32_t bits = eax & 0xffu;
			if (bits >= 32 && bits <= 64) {
				cached = bits;
				return cached;
			}
		}
	}
#endif

	/* Fallback: "address sizes\t: 39 bits physical, 48 bits virtual" */
	{
		FILE *f = fopen("/proc/cpuinfo", "r");
		char line[256];
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				unsigned int b = 0;
				if (strncmp(line, "address sizes", 13) != 0)
					continue;
				const char *colon = strchr(line, ':');
				if (colon && sscanf(colon + 1, " %u bits physical", &b) == 1 &&
				    b >= 32 && b <= 64) {
					cached = b;
					break;
				}
			}
			fclose(f);
		}
	}
	return cached;   /* 0 if we could not determine it */
}

/*
 * Guest MAXPHYADDR.
 *
 * The guest's addressable range is what actually matters for a GPA the guest
 * must be able to reach, and QEMU may cap it below the host's.  x86 resolves
 * X86CPU::phys_bits during CPU realize and exposes it as the "phys-bits"
 * property; -cpu host and -cpu max default host-phys-bits=on, so this normally
 * equals the host width.  Read it through QOM rather than target/i386/cpu.h:
 * these sources are built into system_ss (target-independent), where the x86
 * CPU headers are not available.
 */
uint32_t nvkvm_guest_phys_bits(void)
{
	uint64_t v;
	Error *err = NULL;

	if (!first_cpu)
		return 0;
	v = object_property_get_uint(OBJECT(first_cpu), "phys-bits", &err);
	if (err) {
		error_free(err);
		return 0;
	}
	/* 0 means "auto, not resolved yet"; anything outside [32,64] is not a
	 * width we should trust. */
	if (v < 32 || v > 64)
		return 0;
	return (uint32_t)v;
}

/*
 * Conservative upper bound on the top of guest RAM in GPA space.
 *
 * On x86 whatever does not fit under the 4 GiB PCI hole is remapped above
 * 4 GiB, so the true top is 4 GiB + (ram_size - below_4g_size), which is
 * always <= 4 GiB + ram_size.  maxram_size covers a configured memory-hotplug
 * region, which is mapped above RAM as well.
 */
static uint64_t nvkvm_guest_ram_top(void)
{
	uint64_t ram = 0;

	if (current_machine) {
		ram = (uint64_t)current_machine->ram_size;
		if ((uint64_t)current_machine->maxram_size > ram)
			ram = (uint64_t)current_machine->maxram_size;
	}
	return (4ULL << 30) + ram;
}

static uint64_t nvkvm_align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static uint64_t nvkvm_align_down(uint64_t v, uint64_t a)
{
	return v & ~(a - 1);
}

/*
 * Resolve the GPA window block.
 *
 * Placement rule
 * ==============
 *   bits  = min(host MAXPHYADDR, guest MAXPHYADDR)   [the binding limit]
 *   limit = 1 << bits                                [one past the last GPA]
 *   span  = 1 GiB (shm slot) + mmap_win + sparse
 *   base  = align_down(limit - span, 1 GiB)          [as high as it will go]
 *
 * i.e. the block is packed against the top of the addressable space, and the
 * check is on base + span <= limit — the *whole* window must fit, not just its
 * base.  Top-down placement keeps the block as far from guest RAM as the
 * address space allows (the property the old 1 TB constant was reaching for)
 * and keeps it clear of firmware's 64-bit BAR allocator, which fills the PCI64
 * hole bottom-up starting just above RAM.
 *
 * The floor the block may not cross is guest RAM top plus room for the sparse
 * reservation BAR (which firmware must place below us) plus 1 GiB of slack:
 *
 *   floor = align_up(4 GiB + ram_size, 1 GiB) + sparse_size + 1 GiB
 *
 * If base < floor the sparse window is halved and we try again, down to
 * NVKVM_SPARSE_GPA_SIZE_MIN, logging each shrink.  If even the floor size does
 * not fit we fail with a message naming every number involved, rather than
 * silently handing back a window that would break at the 1500th mmap.
 */
bool nvkvm_gpa_layout_compute(struct nvkvm_gpa_layout *out,
			      char *errbuf, size_t errlen)
{
	uint32_t host_bits, guest_bits, bits;
	uint64_t limit, ram_top, sparse_size;

	memset(out, 0, sizeof(*out));

	host_bits  = nvkvm_host_phys_bits();
	guest_bits = nvkvm_guest_phys_bits();

	/* Take the narrower of the two: the host width bounds what KVM will
	 * accept for a memslot, the guest width bounds what the guest can
	 * address.  A GPA needs to satisfy both. */
	bits = 0;
	if (host_bits && guest_bits)
		bits = host_bits < guest_bits ? host_bits : guest_bits;
	else if (host_bits)
		bits = host_bits;
	else if (guest_bits)
		bits = guest_bits;

	if (!bits) {
		/* Neither probe worked.  36 bits (64 GiB) is the x86-64
		 * architectural minimum, so it is the only safe assumption. */
		bits = 36;
		fprintf(stderr, "nvkvm: could not determine host or guest "
			"physical address width; assuming %u bits\n", bits);
	}

	/*
	 * QEMU cannot represent a GPA wider than its target address space.
	 * TARGET_PHYS_ADDR_SPACE_BITS is the right constant but it is
	 * *target-specific* (target/i386/cpu-param.h) and these sources are
	 * compiled into system_ss, which is built once for all targets — so it
	 * is not visible here.  Use it when it is, and otherwise fall back to
	 * x86-64's value (52), which is also the architectural ceiling for
	 * MAXPHYADDR.
	 */
#ifdef TARGET_PHYS_ADDR_SPACE_BITS
	if (bits > TARGET_PHYS_ADDR_SPACE_BITS)
		bits = TARGET_PHYS_ADDR_SPACE_BITS;
#else
	if (bits > 52)
		bits = 52;
#endif

	limit   = (bits >= 64) ? UINT64_MAX : (1ULL << bits);
	ram_top = nvkvm_guest_ram_top();

	out->host_bits  = host_bits;
	out->guest_bits = guest_bits;
	out->bits       = bits;
	out->limit      = limit;
	out->ram_top    = ram_top;
	out->mmap_size  = NVKVM_MMAP_WIN_SIZE;

	/*
	 * Cap the sparse window at 1/8 of the addressable space before we even
	 * start.  span <= limit is necessary but NOT sufficient: the sparse
	 * window is a 64-bit PCI BAR, and the *guest firmware* has to find a
	 * naturally-aligned hole for it.  MEASURED on a 39-bit host (i7-11800H,
	 * limit 512 GiB): a 128 GiB BAR passed every arithmetic check here and
	 * SeaBIOS then assigned no BAR at all to the device -- every other
	 * function on the bus got one, 00:07.0 got none -- leaving the guest
	 * with no window and KVM_RUN faulting on first touch.  A BAR that is a
	 * large fraction of the whole address space is not placeable in
	 * practice, however well it fits on paper.
	 *
	 * 1/8 is a no-op on any host wide enough to matter (46 bits gives an
	 * 8 TiB cap against a 128 GiB window) and forces the shrink loop to run
	 * exactly where it is needed.
	 */
	sparse_size = NVKVM_SPARSE_GPA_SIZE;
	while (sparse_size > NVKVM_SPARSE_GPA_SIZE_MIN &&
	       sparse_size > limit / 8)
		sparse_size /= 2;

	for (;; sparse_size /= 2) {
		uint64_t span, base, floor;

		span  = NVKVM_SHM_GPA_SLOT + NVKVM_MMAP_WIN_SIZE + sparse_size;
		span  = nvkvm_align_up(span, NVKVM_GPA_ALIGN);

		/*
		 * Room below us for guest RAM *and* for the sparse reservation
		 * BAR.  A 64-bit PCI BAR is naturally aligned to its own size,
		 * so reserve align_up(ram_top + slack, sparse_size) +
		 * sparse_size below the block.
		 *
		 * Measured with SeaBIOS 1.16.3, and deliberately NOT assumed:
		 * on a 39-bit host the 128 GiB BAR landed at limit - 128 GiB,
		 * i.e. exactly this computed sparse_base; on a 46-bit host it
		 * landed at 0x380000000000 (56 TiB), 8 TiB below the top.  So
		 * firmware's choice is not a simple function of the width, and
		 * nvkvm_sparse_ensure() validates whatever it gets rather than
		 * predicting it.  This floor is conservative in the common
		 * case; what it really buys is an honest "cannot fit" answer
		 * when there would be nowhere to put the BAR at all.
		 */
		floor = nvkvm_align_up(ram_top + NVKVM_GPA_ALIGN, sparse_size)
			+ sparse_size;

		if (span <= limit) {
			/*
			 * Leave the top eighth of the address space free
			 * rather than butting the block against the ceiling.
			 * MEASURED: on a 46-bit host firmware placed the BAR
			 * at 0x380000000000 -- 8 TiB below the top, i.e. one
			 * eighth clear -- so it evidently wants room up there.
			 * On a 39-bit host `limit - span` leaves ZERO margin
			 * and SeaBIOS assigned no BAR at all.  Reserve the
			 * same proportion it chooses for itself when it has
			 * the space.
			 */
			uint64_t ceiling = limit - (limit / 8);

			base = nvkvm_align_down(ceiling - span, NVKVM_GPA_ALIGN);
			if (base >= floor) {
				out->sparse_size = sparse_size;
				out->block_base  = base;
				out->block_size  = span;
				out->floor       = floor;
				out->shm_base    = base;
				out->mmap_base   = base + NVKVM_SHM_GPA_SLOT;
				out->sparse_base = out->mmap_base +
						   NVKVM_MMAP_WIN_SIZE;
				out->shrunk      =
					(sparse_size != NVKVM_SPARSE_GPA_SIZE);
				return true;
			}
			out->floor = floor;
		}

		if (sparse_size <= NVKVM_SPARSE_GPA_SIZE_MIN) {
			snprintf(errbuf, errlen,
			 "nvkvm: the GPA windows do not fit in this VM's "
			 "physical address space. host MAXPHYADDR %u bits, "
			 "guest MAXPHYADDR %u bits -> usable GPA limit %llu GiB. "
			 "Guest RAM top is %llu GiB, so the windows must start "
			 "above %llu GiB, and the smallest layout nvkvm supports "
			 "needs %llu GiB (1 GiB shm + %llu GiB mmap window + "
			 "%llu GiB sparse window) — %llu GiB more than there is "
			 "room for. Reduce guest RAM to at most %llu GiB, or run "
			 "on a host with more physical address bits.",
			 host_bits, guest_bits,
			 (unsigned long long)(limit >> 30),
			 (unsigned long long)(ram_top >> 30),
			 (unsigned long long)(floor >> 30),
			 (unsigned long long)(span >> 30),
			 (unsigned long long)(NVKVM_MMAP_WIN_SIZE >> 30),
			 (unsigned long long)(sparse_size >> 30),
			 (unsigned long long)(((floor + span) - limit) >> 30),
			 (unsigned long long)(
				limit > (span + sparse_size + (5ULL << 30))
				? ((limit - span - sparse_size -
				    (5ULL << 30)) >> 30)
				: 0));
			return false;
		}

		fprintf(stderr,
			"nvkvm: sparse GPA window %llu GiB does not fit below "
			"the %llu GiB GPA limit with %llu GiB of guest RAM — "
			"halving to %llu GiB\n",
			(unsigned long long)(sparse_size >> 30),
			(unsigned long long)(limit >> 30),
			(unsigned long long)(ram_top >> 30),
			(unsigned long long)((sparse_size / 2) >> 30));
	}
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

	/* Size comes from the realize-time layout, not the compile-time
	 * constant: on a narrow host it may have been deliberately shrunk. */
	size_t win = nv->gpa.sparse_size ? (size_t)nv->gpa.sparse_size
					 : (size_t)NVKVM_SPARSE_GPA_SIZE;

	void *va = mmap(NULL, win,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE,
			-1, 0);
	if (va == MAP_FAILED) {
		NVKVM_DBG(
			"nvkvm_sparse_init: mmap %llu GiB failed: %s\n",
			(unsigned long long)(win >> 30),
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
	nv->sparse_size     = win;
	nv->sparse_vmm_va   = va;
	nv->sparse_cur      = 0;
	nv->sparse_kvm_slot = -1;
	/* #80/H-1: window free-list (recycled extents). */
	nv->sparse_free   = g_new0(struct nvkvm_gpa_extent, NVKVM_GPA_FREE_MAX);
	nv->sparse_free_n = 0;
	NVKVM_DBG("nvkvm_sparse_init: %llu GiB VMM buffer %p (memslot deferred to BAR base)\n",
		  (unsigned long long)(win >> 30), va);
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
		/*
		 * Firmware is not obliged to place the BAR somewhere KVM will
		 * accept.  Check the *whole* range against the resolved GPA
		 * limit before handing it to KVM_SET_USER_MEMORY_REGION, and
		 * fall back to our own computed base rather than dying, so a
		 * firmware that mis-places a 64-bit BAR degrades instead of
		 * taking the VM down.
		 */
		/*
		 * Firmware could also place the BAR on top of the shm or legacy
		 * mmap regions, which are plain memslots it does not know
		 * about.  Installing the window there would shadow shm and
		 * corrupt every ioctl parameter slot, so prefer our own base.
		 */
		if (base < nv->gpa.sparse_base &&
		    base + nv->sparse_size > nv->gpa.block_base) {
			fprintf(stderr,
				"nvkvm: firmware placed the window BAR at GPA=0x%llx, "
				"overlapping the shm/mmap regions at 0x%llx; using the "
				"computed base 0x%llx instead\n",
				(unsigned long long)base,
				(unsigned long long)nv->gpa.block_base,
				(unsigned long long)nv->gpa.sparse_base);
			base = nv->gpa.sparse_base;
		}
		if (nv->gpa.limit &&
		    (base >= nv->gpa.limit ||
		     nv->sparse_size > nv->gpa.limit - base)) {
			fprintf(stderr,
				"nvkvm: firmware placed the window BAR at GPA=0x%llx "
				"+%llu GiB, which crosses this VM's %u-bit GPA limit "
				"(%llu GiB); using the computed base 0x%llx instead\n",
				(unsigned long long)base,
				(unsigned long long)(nv->sparse_size >> 30),
				nv->gpa.bits,
				(unsigned long long)(nv->gpa.limit >> 30),
				(unsigned long long)nv->gpa.sparse_base);
			base = nv->gpa.sparse_base;
		}
	} else {
		/* No BAR transport — use the computed fallback base. */
		base = nv->gpa.sparse_base;
	}
	if (base == 0) {
		pthread_mutex_unlock(&nv->sparse_lock);
		fprintf(stderr, "nvkvm: sparse window has no usable base GPA\n");
		return 0;
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
int nvkvm_window_restore_anon(void *qva, size_t len)
{
	if (!qva || qva == MAP_FAILED || !len)
		return 0;
	void *r = mmap(qva, len, PROT_READ | PROT_WRITE,
		       MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE | MAP_FIXED,
		       -1, 0);
	if (r == MAP_FAILED) {
		/* The window now has a hole under a live memslot.  This is
		 * fatal-ish: the guest will take an unrecoverable EFAULT on the
		 * next touch of this GPA, so make the cause visible here rather
		 * than at the far end.  Most likely vm.max_map_count exhaustion
		 * from VMA splitting inside the window. */
		fprintf(stderr,
			"nvkvm: FATAL: failed to restore anon backing over "
			"window VA %p+%zu: %s -- sparse window now has a hole "
			"under its KVM memslot (check vm.max_map_count)\n",
			qva, len, strerror(errno));
		return -errno;
	}
	return 0;
}

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
