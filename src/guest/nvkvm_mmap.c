// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_mmap.c — mmap handling for the nvkvm guest module
 *
 * When guest userspace calls mmap() on a /dev/nvidia* fd, we:
 *  1. Ask the host to map the corresponding region of the real nvidia device.
 *  2. The host maps it, pins or registers the pages with KVM, and returns
 *     the guest physical address (GPA) base.
 *  3. We use remap_pfn_range() to insert those physical pages into the
 *     requesting process's VMA.
 *
 * The critical invariant: the pages mapped into the guest VA must be the
 * same physical pages the NVIDIA driver mapped on the host. No copy, no bounce
 * buffer. This means GPU BAR pages (VRAM, doorbells, command rings) are
 * directly accessible from guest userspace — essential for zero-copy GPU
 * compute paths.
 *
 * Validation: we validate the GPA range returned by the host against the
 * known mmap window BAR before calling remap_pfn_range. A malicious host
 * could abuse this, but we are not defending against the hypervisor.
 */

#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>   /* mmgrab/mmdrop/mmget_not_zero — audit H-9 */
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/pfn.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/pgtable.h>
#include <asm/pgtable_types.h>

#include "nvkvm.h"

/* Forward declarations for module-internal functions */
static void nvkvm_vma_open(struct vm_area_struct *vma);
static void nvkvm_vma_close(struct vm_area_struct *vma);
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len);

/*
 * nvkvm_force_range_wb — rewrite the leaf PTEs covering [start,end) to
 * write-back caching.
 *
 * Our GPA window is exposed to the guest as a prefetchable PCI BAR — a
 * non-System-RAM region.  remap_pfn_range() therefore routes through x86
 * track_pfn_remap()/reserve_pfn_range(), which SILENTLY downgrades the WB
 * pgprot we request to UC- (PAT index 2) because the range isn't RAM and the
 * MTRR over it isn't uniformly write-back.  On an Intel host this was masked:
 * KVM's EPT sets the IPAT bit and forces WB for any struct-page memslot
 * regardless of the guest PTE.  On an AMD host (NPT honors the guest PTE
 * memtype) the UC- sticks, and CPU access to these cache-coherent,
 * memfd-backed window pages runs ~100x slow (measured 0.12 GB/s vs 14 GB/s on
 * an EPYC 7K62 / RTX 3060).  The pages are genuine write-back RAM (a host
 * memfd; the GPU's DMA snoops the CPU cache), so WB is correct and coherent —
 * see the cacheability discussion in nvkvm_mmap_request_isolate() and the
 * migrate path.  Rewrite the PAT bits directly: PAT index 0 == WB on Linux's
 * PAT MSR, i.e. clear PCD/PWT (and the PAT bit) on the leaf entry.
 *
 * No TLB flush is needed: callers invoke this on PTEs that remap_pfn_range()
 * has just created for a VMA being set up inside the mmap/ioctl syscall (the
 * range was freshly mapped, after a zap in the migrate path), before userspace
 * can have touched — and thus TLB-cached — them.  Callers must hold mmap lock.
 */
void nvkvm_force_range_wb(struct mm_struct *mm,
			  unsigned long start, unsigned long end)
{
	unsigned long a;

	for (a = start; a < end; a += PAGE_SIZE) {
		pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;
		unsigned long v;

		pgd = pgd_offset(mm, a);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;
		p4d = p4d_offset(pgd, a);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;
		pud = pud_offset(p4d, a);
		if (pud_none(*pud))
			continue;
		if (pud_leaf(*pud)) {
			v = pud_val(*pud);
			set_pud(pud, __pud(v & ~(_PAGE_PCD | _PAGE_PWT |
						 _PAGE_PAT_LARGE)));
			a = ALIGN_DOWN(a, PUD_SIZE) + PUD_SIZE - PAGE_SIZE;
			continue;
		}
		pmd = pmd_offset(pud, a);
		if (pmd_none(*pmd))
			continue;
		if (pmd_leaf(*pmd)) {
			v = pmd_val(*pmd);
			set_pmd(pmd, __pmd(v & ~(_PAGE_PCD | _PAGE_PWT |
						 _PAGE_PAT_LARGE)));
			a = ALIGN_DOWN(a, PMD_SIZE) + PMD_SIZE - PAGE_SIZE;
			continue;
		}
		pte = pte_offset_kernel(pmd, a);
		v = pte_val(*pte);
		if (!(v & _PAGE_PRESENT))
			continue;
		set_pte(pte, __pte(v & ~(_PAGE_PCD | _PAGE_PWT | _PAGE_PAT)));
	}
}


const struct vm_operations_struct nvkvm_vm_ops = {
	.open  = nvkvm_vma_open,
	.close = nvkvm_vma_close,
};

static void nvkvm_vma_open(struct vm_area_struct *vma)
{
	/* Nothing to do; reference counting is on the mmap_region. */
}

static void nvkvm_vma_close(struct vm_area_struct *vma)
{
	struct nvkvm_mmap_region *region = vma->vm_private_data;
	if (!region)
		return;

	/*
	 * A fallback-backed managed range is released HERE.
	 *
	 * vma_close and mmap_release_fd can both reach a region — on process
	 * exit the kernel tears VMAs down and then closes files.  Exactly one of
	 * them must do the teardown; the claim decides which, under the same
	 * lock that guards the list.  See nvkvm_uvm_ext_release() for why this
	 * cannot be deferred to fd close the way a forwarded mapping is.
	 */
	if (region->ext_backed) {
		struct nvkvm_fd_ctx *ctx = region->ctx;
		bool mine = false;

		if (ctx) {
			spin_lock(&ctx->mmap_lock);
			if (!region->ext_claimed) {
				region->ext_claimed = true;
				list_del(&region->list);
				mine = true;
			}
			spin_unlock(&ctx->mmap_lock);
		}
		vma->vm_private_data = NULL;
		/* Losing the claim means the other side owns the region and may
		 * already have freed it, so do not touch it again on the way
		 * out — clearing vm_private_data above is all this side owes. */
		if (!mine)
			return;
		nvkvm_uvm_ext_release(region);
		return;
	}

	/* Mark VMA as gone; the actual host munmap is deferred to fd close
	 * or explicit unmap, consistent with how the NVIDIA driver expects
	 * mappings to outlive individual VMAs in some cases. */
	region->vma = NULL;
}

/*
 * nvkvm_mmap_request_isolate — mmap via the isolate path (new API).
 *
 * Sends MMAP_ON_ISOLATE which causes QEMU to:
 *   1. mmap the handle fd at any host VA (QVA)
 *   2. register GPA→QVA in a KVM memory slot
 *   3. tell the isolate to map the same fd at gva (MAP_FIXED)
 *
 * The guest then uses remap_pfn_range to insert GPA pages into the VMA.
 */
static int nvkvm_mmap_request_isolate(struct nvkvm_fd_ctx *ctx,
				      struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	__u64 gva    = vma->vm_start;
	__u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	__u32 prot   = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		       (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0) |
		       (vma->vm_flags & VM_EXEC  ? PROT_EXEC  : 0);
	__u32 map_flags = MAP_SHARED;
	__u64 gpa_base;
	__u32 mmap_token;
	struct nvkvm_mmap_region *region;
	int ret;

	ret = nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id,
					   ctx->handle_id,
					   gva, offset, vma_len,
					   prot, map_flags,
					   (unsigned int)ctx->session->id,
					   &gpa_base, &mmap_token);
	if (ret)
		return ret;

	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa_base, vma_len)) {
		pr_warn("nvkvm: MMAP_ON_ISOLATE returned GPA %llx outside window\n",
			gpa_base);
		/* The isolate mapping and its mmap_token already exist; without
		 * this the token is stranded in QEMU's VM-global table with
		 * nothing tracking it and no reaper that could find it. */
		nvkvm_virtio_munmap_on_isolate(ctx->session->isolate_id,
					       mmap_token);
		return -EIO;
	}

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	/*
	 * Cacheability by device class.  nvidiactl/nvidia-uvm mmaps are pinned
	 * SYSTEM memory (the host backs them as WB RAM memslots; smaps shows no
	 * VM_IO) — they must be WB-cached, or the CPU reads them uncached.  The
	 * channel completion semaphore lives here; mapping it write-combining
	 * made cuCtxSynchronize's poll an uncached read (~3us vs ~0.36us host,
	 * 8.9x), serializing decode and starving the GPU (0-5% util).  On x86 the
	 * guest-WB / host-WB / GPU-DMA views of the same memfd are cache-coherent
	 * (DMA snoops), so WB is correct.  nvidia0/DRM/modeset mmaps are real
	 * BAR/MMIO (VM_IO) and MUST stay write-combining — a WB mapping of the
	 * doorbell BAR would leave the ring store in cache and never reach the
	 * device (decode would hang).
	 */
	if (ctx->dev_id == NVKVM_DEV_CTL || ctx->dev_id == NVKVM_DEV_UVM)
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	else
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa_base >> PAGE_SHIFT),
			      vma_len, vma->vm_page_prot);
	if (ret) {
		/* Ask host to undo the mmap.  Use kmalloc, not stack — see
		 * comment in nvkvm_mmap_release_fd about CONFIG_VMAP_STACK. */
		struct {
			struct nvkvm_hdr                   hdr;
			struct nvkvm_req_munmap_on_isolate req;
		} *umsg;
		struct nvkvm_inflight uinf;
		umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
		if (umsg) {
			umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
			umsg->hdr.txn_id     = cpu_to_le32(
					atomic_inc_return(&nvkvm.next_txn_id));
			umsg->req.isolate_id = cpu_to_le32(ctx->session->isolate_id);
			umsg->req.mmap_token = cpu_to_le32(mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			kfree(umsg);
		}
		return ret;
	}

	/*
	 * WB-intended mappings (nvidiactl/nvidia-uvm SYSTEM memory) are silently
	 * downgraded to UC- by x86's PCI-BAR pfn tracking inside remap_pfn_range
	 * (see nvkvm_force_range_wb).  Force the freshly mapped PTEs back to WB so
	 * CPU access to the cache-coherent window pages runs at RAM speed.  The
	 * write-combining branch is left as-is — WC is honored as-requested and is
	 * required for the doorbell/ring BAR pages.
	 */
	if (ctx->dev_id == NVKVM_DEV_CTL || ctx->dev_id == NVKVM_DEV_UVM)
		nvkvm_force_range_wb(vma->vm_mm, vma->vm_start, vma->vm_end);

	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region) {
		/*
		 * The region IS the only handle on this mapping: mmap_token is
		 * released from nvkvm_mmap_release_fd()/the region teardown and
		 * from nowhere else, and without vm_private_data even
		 * nvkvm_vma_close() cannot find it.  "Leak the region tracking"
		 * therefore meant leaking an entry in QEMU's VM-global mmap
		 * table permanently, per call, with no reap path -- so a guest
		 * that can make this small kzalloc fail can exhaust the table
		 * for every process in the VM.  Undo the isolate-side mapping
		 * and fail the mmap instead; the kernel tears the VMA (and the
		 * PTEs remap_pfn_range installed) down for us on error, exactly
		 * as it does for the remap_pfn_range failure just above.
		 */
		nvkvm_virtio_munmap_on_isolate(ctx->session->isolate_id,
					       mmap_token);
		return -ENOMEM;
	}

	region->mmap_token = mmap_token;
	region->handle_id  = ctx->handle_id;
	region->gpa_base   = (unsigned long)gpa_base;
	region->length     = vma_len;
	region->offset     = offset;
	region->vma        = vma;

	spin_lock(&ctx->mmap_lock);
	list_add_tail(&region->list, &ctx->mmap_regions);
	spin_unlock(&ctx->mmap_lock);

	vma->vm_ops          = &nvkvm_vm_ops;
	vma->vm_private_data = region;
	return 0;
}

static int nvkvm_mmap_request_uvm_realize(struct nvkvm_fd_ctx *ctx,
					  struct vm_area_struct *vma);

/*
 * Does a recorded UVM intent cover exactly this mapping?
 *
 * UVM_ALLOC_SEMAPHORE_POOL names its VA in the ioctl and is mmap'd afterwards
 * at that same VA — va_range_type_expects_mmap() is true for SEMAPHORE_POOL and
 * DEVICE_P2P and false for MANAGED (uvm.c:743-757).  Those mappings must keep
 * going down the forwarding path.
 *
 * A managed range has no such ioctl: mmap itself creates it.  So "no matching
 * intent" is precisely "this is a managed allocation", which is the one case
 * the fallback handles.
 */
static bool nvkvm_uvm_mmap_has_intent(struct nvkvm_fd_ctx *ctx, __u64 gva,
				      unsigned long len)
{
	struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
	struct nvkvm_uvm_mapping_intent *m;
	bool found = false;

	if (!st)
		return false;
	mutex_lock(&st->lock);
	list_for_each_entry(m, &st->intents, list) {
		if (m->base == gva && m->length == len) {
			found = true;
			break;
		}
	}
	mutex_unlock(&st->lock);
	return found;
}

int nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;

	/* Basic validation: size must be page-aligned and non-zero */
	if (!vma_len || (vma_len & ~PAGE_MASK))
		return -EINVAL;
	if (vma_len > SZ_1G)
		return -EINVAL;

	/*
	 * Open establishes ctx->handle_id and ctx->session->isolate_id; mmap
	 * with either missing is a logic bug, not a fallback.
	 */
	if (!ctx->handle_id || !ctx->session->isolate_id)
		return -EBADF;

	/* Step E plan: UVM mmap → REALIZE path.  Disabled until the
	 * realize-on-existing-fd refactor lands (the current fresh-fd
	 * design loses the RM↔UVM bindings the original fd built up). */
	(void)nvkvm_mmap_request_uvm_realize;

	/*
	 * A UVM mmap with no matching intent is libcuda creating a MANAGED
	 * range — the one thing forwarding cannot do correctly (§2b of
	 * docs/internal/uvm-va-decoupling.md).  Everything else on this fd
	 * (semaphore pools, device-P2P) named its VA in an ioctl first, so it
	 * already exists in the host va_space and keeps the forwarding path.
	 */
	if (ctx->dev_id == NVKVM_DEV_UVM && ctx->uvm_state &&
	    !nvkvm_uvm_mmap_has_intent(ctx, vma->vm_start, vma_len))
		return nvkvm_uvm_ext_mmap(ctx, vma);

	return nvkvm_mmap_request_isolate(ctx, vma);
}

/*
 * UVM realize path (state-machine Step E).
 *
 * Build a state snapshot from ctx->uvm_state, pick the matching intent
 * by (gva, length), upload both into shm slots, and issue a single
 * REALIZE_UVM_MAPPING request.  QEMU does the privileged work and
 * returns a GPA we map into the VMA with remap_pfn_range.
 */
static int nvkvm_mmap_request_uvm_realize(struct nvkvm_fd_ctx *ctx,
					  struct vm_area_struct *vma)
{
	struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
	struct nvkvm_uvm_mapping_intent *m, *match = NULL;
	struct nvkvm_uvm_gpu_reg *g;
	struct nvkvm_uvm_vas_reg *v;
	struct nvkvm_uvm_range_group *r;
	struct nvkvm_uvm_state_snapshot *snap = NULL;
	struct nvkvm_uvm_realization *real = NULL;
	void *intent_slot_ptr = NULL;
	int state_slot = -1, intent_slot = -1;
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	__u64 gva    = vma->vm_start;
	__u32 prot   = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		       (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
	__u32 map_flags = MAP_SHARED;
	__u64 gpa_base = 0, realize_token = 0;
	__u32 rm_status = 0;
	__u32 intent_size = 0;
	int ret;

	mutex_lock(&st->lock);
	list_for_each_entry(m, &st->intents, list) {
		if (m->base == gva && m->length == vma_len) {
			match = m;
			break;
		}
	}
	if (!match) {
		mutex_unlock(&st->lock);
		pr_warn("nvkvm: UVM mmap %llx+%lx without matching intent\n",
			gva, vma_len);
		return -EINVAL;
	}

	state_slot = nvkvm_slot_alloc(&nvkvm);
	if (state_slot < 0) { ret = -ENOSPC; goto err_unlock; }
	intent_slot = nvkvm_slot_alloc(&nvkvm);
	if (intent_slot < 0) { ret = -ENOSPC; goto err_unlock; }

	snap = nvkvm_slot_addr(&nvkvm, state_slot);
	intent_slot_ptr = nvkvm_slot_addr(&nvkvm, intent_slot);
	if (!snap || !intent_slot_ptr ||
	    nvkvm.slot_size < sizeof(*snap) ||
	    nvkvm.slot_size < match->params_size) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	memset(snap, 0, sizeof(*snap));
	snap->init_flags = cpu_to_le64(st->init_flags);

	__u32 n_gpus = 0;
	list_for_each_entry(g, &st->registered_gpus, list) {
		if (n_gpus >= NVKVM_UVM_MAX_REG_GPUS) break;
		memcpy(snap->gpus[n_gpus].gpu_uuid, g->gpu_uuid, 16);
		n_gpus++;
	}
	snap->n_gpus = cpu_to_le32(n_gpus);

	__u32 n_vas = 0;
	list_for_each_entry(v, &st->registered_va_spaces, list) {
		if (n_vas >= NVKVM_UVM_MAX_VA_SPACES) break;
		memcpy(snap->va_spaces[n_vas].gpu_uuid, v->gpu_uuid, 16);
		snap->va_spaces[n_vas].rm_ctrl_fd_handle_id =
			cpu_to_le32(v->rm_ctrl_fd_handle_id);
		snap->va_spaces[n_vas].h_client   = cpu_to_le32(v->h_client);
		snap->va_spaces[n_vas].h_va_space = cpu_to_le32(v->h_va_space);
		n_vas++;
	}
	snap->n_va_spaces = cpu_to_le32(n_vas);

	__u32 n_rgs = 0;
	list_for_each_entry(r, &st->range_groups, list) {
		if (n_rgs >= NVKVM_UVM_MAX_RANGE_GROUPS) break;
		snap->range_group_ids[n_rgs] = cpu_to_le64(r->range_group_id);
		n_rgs++;
	}
	snap->n_range_groups = cpu_to_le32(n_rgs);

	memcpy(intent_slot_ptr, match->params, match->params_size);
	intent_size = (__u32)match->params_size;
	wmb();
	mutex_unlock(&st->lock);

	ret = nvkvm_virtio_realize_uvm_mapping(ctx->session->isolate_id,
					       ctx->handle_id,
					       NVKVM_UVM_REALIZE_MODE_SEM_POOL,
					       (unsigned int)ctx->session->id,
					       gva, vma_len, 0,
					       prot, map_flags,
					       (__u32)state_slot,
					       (__u32)intent_slot,
					       intent_size,
					       &gpa_base, &realize_token,
					       &rm_status);

	nvkvm_slot_free(&nvkvm, state_slot);
	state_slot = -1;
	nvkvm_slot_free(&nvkvm, intent_slot);
	intent_slot = -1;

	if (ret) {
		pr_warn("nvkvm: REALIZE_UVM_MAPPING failed: %d rm_status=0x%x\n",
			ret, rm_status);
		return ret;
	}
	if (!nvkvm_gpa_in_mmap_window((unsigned long)gpa_base, vma_len)) {
		pr_warn("nvkvm: REALIZE returned GPA %llx outside window\n",
			gpa_base);
		return -EIO;
	}

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa_base >> PAGE_SHIFT),
			      vma_len, vma->vm_page_prot);
	if (ret)
		return ret;

	real = kzalloc(sizeof(*real), GFP_KERNEL);
	if (real) {
		real->realize_token = realize_token;
		real->gva    = gva;
		real->gpa    = gpa_base;
		real->length = vma_len;
		mutex_lock(&st->lock);
		list_add_tail(&real->list, &st->realizations);
		mutex_unlock(&st->lock);
	}
	return 0;

err_unlock:
	if (state_slot  >= 0) nvkvm_slot_free(&nvkvm, state_slot);
	if (intent_slot >= 0) nvkvm_slot_free(&nvkvm, intent_slot);
	mutex_unlock(&st->lock);
	return ret;
}

/*
 * nvkvm_gva_pfn — the PFN currently mapped at a guest VA, or 0 if nothing is.
 *
 * Same leaf walk as nvkvm_force_range_wb() above.  We cannot use gup here:
 * the mappings we need to identify are exactly the VM_PFNMAP ones that gup
 * refuses.  Caller must hold mmap_read_lock.
 */
static unsigned long nvkvm_gva_pfn(struct mm_struct *mm, unsigned long gva)
{
	pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;
	unsigned long v;

	pgd = pgd_offset(mm, gva);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;
	p4d = p4d_offset(pgd, gva);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;
	pud = pud_offset(p4d, gva);
	if (pud_none(*pud))
		return 0;
	if (pud_leaf(*pud)) {
		v = pud_val(*pud);
		if (!(v & _PAGE_PRESENT))
			return 0;
		return ((v & PTE_PFN_MASK) >> PAGE_SHIFT) +
		       ((gva & ~PUD_MASK) >> PAGE_SHIFT);
	}
	pmd = pmd_offset(pud, gva);
	if (pmd_none(*pmd))
		return 0;
	if (pmd_leaf(*pmd)) {
		v = pmd_val(*pmd);
		if (!(v & _PAGE_PRESENT))
			return 0;
		return ((v & PTE_PFN_MASK) >> PAGE_SHIFT) +
		       ((gva & ~PMD_MASK) >> PAGE_SHIFT);
	}
	pte = pte_offset_kernel(pmd, gva);
	if (!pte)
		return 0;
	v = pte_val(*pte);
	if (!(v & _PAGE_PRESENT))
		return 0;
	return (v & PTE_PFN_MASK) >> PAGE_SHIFT;
}

/*
 * ─── Audit H-9: releasing a window extent must unmap it from the guest ─────
 *
 * nvkvm_cpu_pages_migrate_range() converts an ORDINARY ANONYMOUS VMA to
 * VM_PFNMAP and remap_pfn_range()s it onto a sparse-window GPA handed out by
 * QEMU.  That VMA holds no reference on any nvkvm file — it is plain anon
 * memory that we retyped in place — so the invariant the GPU mmap regions rely
 * on ("a live VMA keeps the fd open, so nvkvm_mmap_release_fd() cannot run
 * before the last VMA is gone") DOES NOT HOLD on this path.  A process can
 * close its /dev/nvidia* fds, or merely trip the staleness reaper, while the
 * buffer is still mapped and still being written through.
 *
 * Both release paths then send MUNMAP_ON_ISOLATE + CLOSE_HANDLE, QEMU returns
 * the extent to its VM-global window free list, and nvkvm_sparse_gpa_alloc()
 * hands the very same GPA to the next isolate that asks.  The first process's
 * surviving PTEs now alias a DIFFERENT guest process's GPU/pinned memory —
 * readable and writable — which breaks intra-VM process isolation outright.
 *
 * So: every path that releases a range entry takes the guest's PTEs down
 * FIRST, via nvkvm_cpu_page_unmap_guest() below.  Two details decide whether
 * that is a real fix or a decoration:
 *
 *  - zap_vma_ptes() (nvkvm_zap_range) returns void and zaps NOTHING unless the
 *    VMA is VM_PFNMAP/VM_MIXEDMAP — the same silent no-op the install path
 *    guards against further down this file.  We check the precondition here
 *    too and shout if it ever fails, rather than reporting an unmap we did not
 *    perform.
 *  - A GVA is not a stable name for our mapping (that is the whole reason
 *    nvkvm_cpu_page_entry_live() exists).  Userspace may have munmap()ped part
 *    of the range and mapped something unrelated there.  So we zap only pages
 *    whose leaf PTE still resolves INTO THIS ENTRY'S EXTENT, coalesced into
 *    runs, and leave everything else strictly alone.  That page-granular check
 *    is also what makes the reap path safe: it cannot damage a neighbour's
 *    mapping even when the liveness probe misjudged the entry.
 */

/*
 * nvkvm_zap_entry_ptes — zap the leaf PTEs in [start,end) that still point at
 * [pfn_lo, pfn_hi).  Returns the number of BYTES that pointed at the extent
 * but could not be zapped (VMA not PFNMAP/MIXEDMAP), i.e. still-aliasing.
 *
 * Caller holds mmap_write_lock and has taken the per-VMA write lock.
 */
static unsigned long nvkvm_zap_entry_ptes(struct mm_struct *mm,
					  struct vm_area_struct *vma,
					  unsigned long start, unsigned long end,
					  unsigned long pfn_lo,
					  unsigned long pfn_hi)
{
	bool can_zap = !!(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP));
	unsigned long a, run = 0, stuck = 0;

	for (a = start; a < end; a += PAGE_SIZE) {
		unsigned long pfn = nvkvm_gva_pfn(mm, a);

		if (pfn >= pfn_lo && pfn < pfn_hi) {
			if (!run)
				run = a;
			if (!can_zap)
				stuck += PAGE_SIZE;
			continue;
		}
		if (run && can_zap)
			nvkvm_zap_range(vma, run, a - run);
		run = 0;
	}
	if (run && can_zap)
		nvkvm_zap_range(vma, run, end - run);
	return stuck;
}

/*
 * nvkvm_cpu_page_unmap_guest — drop the guest mapping of a range entry before
 * its window extent is handed back to QEMU.  See the H-9 block above.
 *
 * Copy entries (page != NULL) are skipped: they were never remapped, their GVA
 * still translates to the pinned anon page, and no window GPA of theirs is in
 * the guest's page tables at all — only in the isolate's.
 *
 * Locking.  The lock order in this file is cpu_pages_lock -> mmap_lock (see
 * nvkvm_cpu_page_entry_live); nothing takes them the other way.  Both callers
 * have already unlinked the entry onto a private list and dropped
 * cpu_pages_lock, so we take mmap_lock with no nvkvm lock held.  Neither
 * caller is ever reached with mmap_lock already held:
 *
 *   nvkvm_cpu_pages_reap_stale() runs from the two migration entry points —
 *   nvkvm_efault_resolve() drops mmap_read_lock before calling
 *   nvkvm_cpu_page_migrate(), and nvkvm_cpu_pages_migrate_range() reaps before
 *   it takes mmap_write_lock itself.
 *
 *   nvkvm_cpu_pages_free() runs only from nvkvm_fd_ctx_destroy(), i.e. from
 *   ->release (fput() defers the last put to task_work, so it never fires
 *   under the munmap/exit_mmap mmap_write_lock) or from a proxy-GEM /
 *   dma-buf put, none of which hold mmap_lock.
 *
 * Lifetime.  cp->mm is pinned by the mmgrab() taken when the entry was
 * recorded, so the struct is guaranteed to still be there.  mmget_not_zero()
 * then asks the separate question of whether the ADDRESS SPACE still exists:
 * if it does not, exit_mmap() has already unmapped every VMA and there are by
 * construction no PTEs left to zap — the common process-exit case, where
 * current->mm is NULL anyway because exit_mm() precedes exit_files().
 */
static void nvkvm_cpu_page_unmap_guest(struct nvkvm_cpu_page *cp)
{
	struct mm_struct *mm = cp->mm;
	struct vm_area_struct *vma;
	unsigned long start, end, pfn_lo, pfn_hi, stuck = 0;

	if (cp->page || !mm || !cp->length)
		return;

	start  = cp->gva;
	end    = cp->gva + cp->length;
	if (end <= start)                      /* overflow paranoia */
		return;
	pfn_lo = (unsigned long)(cp->gpa >> PAGE_SHIFT);
	pfn_hi = pfn_lo + (cp->length >> PAGE_SHIFT);

	if (!mmget_not_zero(mm))
		return;

	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	while (vma && vma->vm_start < end) {
		unsigned long s = max(start, vma->vm_start);
		unsigned long e = min(end,   vma->vm_end);

		if (s < e) {
			/*
			 * Same reason the install path takes it: mmap_lock
			 * alone does not exclude a fault holding only the
			 * per-VMA read lock (CONFIG_PER_VMA_LOCK).
			 */
			vma_start_write(vma);
			stuck += nvkvm_zap_entry_ptes(mm, vma, s, e,
						      pfn_lo, pfn_hi);
		}
		vma = find_vma(mm, vma->vm_end);
	}
	mmap_write_unlock(mm);
	mmput(mm);

	if (stuck)
		pr_warn("nvkvm: H-9: %lu bytes of GPA 0x%llx still mapped at 0x%lx under a non-PFNMAP VMA; extent released while aliased\n",
			stuck, (unsigned long long)cp->gpa, start);
}

/*
 * nvkvm_cpu_page_release — common tail for both release paths: unmap from the
 * guest, unmap from the isolate, close the handle, drop the pin, free.
 *
 * The ORDER is the fix (audit H-9): the guest's PTEs must be gone BEFORE
 * MUNMAP_ON_ISOLATE/CLOSE_HANDLE lets QEMU recycle the GPA.  Caller must not
 * hold cpu_pages_lock, and the entry must already be off ctx->cpu_pages.
 */
static void nvkvm_cpu_page_release(struct nvkvm_cpu_page *cp, __u32 isolate_id)
{
	nvkvm_cpu_page_unmap_guest(cp);

	if (isolate_id) {
		nvkvm_virtio_munmap_on_isolate(isolate_id, cp->mmap_token);
		nvkvm_virtio_close_handle_on_isolate(cp->handle_id, isolate_id);
	}
	nvkvm_virtio_close_handle(cp->handle_id);
	if (cp->page)            /* range entries are page==NULL */
		put_page(cp->page);
	if (cp->mm)              /* the mmgrab() from the migrate path */
		mmdrop(cp->mm);
	list_del(&cp->list);
	kfree(cp);
}

/*
 * nvkvm_cpu_page_entry_live — is this tracking entry still describing the
 * mapping it was created for?
 *
 * Both migration paths remember a GVA and then treat a later request for the
 * same GVA as already done.  A GVA is not a stable name for a buffer: free a
 * pinned host buffer and allocate another and the allocator hands back the
 * same address, at which point the entry names a mapping that belongs to a
 * buffer that no longer exists.  Nothing downstream notices — the isolate
 * still has the previous buffer's memory mapped there, so the GPU reads the
 * previous buffer's bytes and returns them as this buffer's result.
 *
 * So check the guest page tables rather than trusting the address:
 *
 *   range entry (page == NULL) — the VA was remapped onto the memfd's GPA, so
 *     it is still ours exactly while the leaf PTE still points at that GPA.
 *   copy entry  (page != NULL) — the VA was left as ordinary memory and its
 *     page pinned, so it is still ours exactly while the VA still translates
 *     to the page we pinned.
 *
 * Lock order: callers hold cpu_pages_lock and this takes mmap_read_lock under
 * it.  No path takes them the other way round.
 */
static bool nvkvm_cpu_page_entry_live(struct nvkvm_cpu_page *cp)
{
	struct mm_struct *mm = current->mm;
	bool live;

	if (!mm)
		return true;   /* no user context to check against; leave it */

	/*
	 * Only the address space that created an entry can judge it.  An fd
	 * shared with another process (a fork, or an explicit SCM_RIGHTS pass)
	 * gives us a different mm in which these GVAs mean nothing, and every
	 * entry would look dead.  Compare the pointer; never dereference it.
	 */
	if (cp->mm && cp->mm != mm)
		return true;

	if (!cp->page) {
		unsigned long pfn;

		mmap_read_lock(mm);
		pfn = nvkvm_gva_pfn(mm, cp->gva);
		mmap_read_unlock(mm);
		return pfn && pfn == (unsigned long)(cp->gpa >> PAGE_SHIFT);
	}

	{
		struct page *cur = NULL;

		if (get_user_pages_fast(cp->gva, 1, 0, &cur) != 1)
			return false;
		live = (cur == cp->page);
		put_page(cur);
	}
	return live;
}

/*
 * nvkvm_cpu_pages_reap_stale — drop every tracking entry whose guest mapping
 * has gone away, releasing the isolate mapping and the memfd with it.
 *
 * Audit H-9: releasing goes through nvkvm_cpu_page_release(), which zaps the
 * entry's surviving window PTEs first.  That also disarms the sharp edge in
 * nvkvm_cpu_page_entry_live() below — it probes only cp->gva, so overmapping
 * just the FIRST page of a multi-page chunk declares the whole chunk dead and
 * lands it here with real, live PTEs still in it.  The probe is deliberately
 * left as-is: it only decides whether to re-migrate, and making it stickier
 * (any page still ours => live) would resurrect the stale-buffer aliasing it
 * was written to fix, while making it stricter (every page ours => live) just
 * moves the false verdict around.  Since the release now unmaps the extent
 * page by page, a wrong verdict costs the process its mapping (it faults, and
 * a re-registration re-migrates) instead of costing another process its
 * isolation.
 *
 * Called before either migration path consults its "already migrated?" cache,
 * so that a reused address re-migrates instead of inheriting the mapping of
 * whatever used to live there.  The list holds one entry per migrated page or
 * 2 MiB chunk of a live registration, so this is a short walk on a cold path.
 */
void nvkvm_cpu_pages_reap_stale(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_cpu_page *cp, *tmp;
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;
	LIST_HEAD(dead);

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry_safe(cp, tmp, &ctx->cpu_pages, list)
		if (!nvkvm_cpu_page_entry_live(cp))
			list_move_tail(&cp->list, &dead);
	mutex_unlock(&ctx->cpu_pages_lock);

	list_for_each_entry_safe(cp, tmp, &dead, list)
		nvkvm_cpu_page_release(cp, isolate_id);
}

/*
 * nvkvm_cpu_page_migrate — pin a guest physical page and upload it to a
 * memfd so the isolate can access it at the same GVA via MAP_FIXED.
 *
 * On first access the page is pinned, copied to QEMU via shared memory, and
 * mapped in the isolate.  Subsequent accesses to the same page_gva are no-ops
 * (the mapping is already live).
 */
int nvkvm_cpu_page_migrate(struct nvkvm_fd_ctx *ctx,
			   unsigned long page_gva, unsigned long prot)
{
	struct nvkvm_cpu_page *cp;
	struct page *page = NULL;
	int shm_slot = -1;
	void *slot_ptr;
	__u32 handle_id = 0;
	__u64 gpa_base;
	__u32 mmap_token;
	int ret;

	/* Already mapped?  Reap first, so an entry left over from a buffer that
	 * has since been freed cannot answer for this address (see
	 * nvkvm_cpu_page_entry_live). */
	nvkvm_cpu_pages_reap_stale(ctx);

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list) {
		if (cp->gva == page_gva) {
			mutex_unlock(&ctx->cpu_pages_lock);
			return 0;
		}
	}
	mutex_unlock(&ctx->cpu_pages_lock);

	/* Pin the physical page (write access so writeback can update it). */
	ret = get_user_pages_fast(page_gva, 1, FOLL_WRITE, &page);
	if (ret != 1) {
		pr_warn("nvkvm: cpu_page_migrate gup gva=0x%lx ret=%d\n",
			page_gva, ret);
		return (ret < 0) ? ret : -EFAULT;
	}

	/* Copy page content into a shared memory slot. */
	shm_slot = nvkvm_slot_alloc(&nvkvm);
	if (shm_slot < 0) { ret = -ENOSPC; goto err_page; }

	slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
	if (!slot_ptr || nvkvm.slot_size < PAGE_SIZE) {
		ret = -ENOMEM;
		goto err_slot;
	}

	{
		void *kaddr = kmap_local_page(page);
		memcpy(slot_ptr, kaddr, PAGE_SIZE);
		kunmap_local(kaddr);
	}
	wmb();

	/* Create a QEMU-side memfd for this page. */
	ret = nvkvm_virtio_open_memory_handle((unsigned int)ctx->session->id,
					      PAGE_SIZE, &handle_id);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate open_memory_handle gva=0x%lx ret=%d\n",
			page_gva, ret);
		goto err_slot;
	}

	/* Upload the page content to the memfd. */
	ret = nvkvm_virtio_write_memory_handle(handle_id, 0, shm_slot, PAGE_SIZE);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate write_memory_handle gva=0x%lx ret=%d\n",
			page_gva, ret);
		goto err_handle;
	}

	nvkvm_slot_free(&nvkvm, shm_slot);
	shm_slot = -1;

	/* Send the handle to the isolate and map it at page_gva. */
	ret = nvkvm_virtio_copy_handle_to_isolate(handle_id,
						  ctx->session->isolate_id);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate copy_handle_to_isolate gva=0x%lx isolate=%u ret=%d\n",
			page_gva, ctx->session->isolate_id, ret);
		goto err_handle;
	}

	ret = nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id, handle_id,
					   page_gva, 0, PAGE_SIZE,
					   prot, MAP_SHARED,
					   (unsigned int)ctx->session->id,
					   &gpa_base, &mmap_token);
	if (ret) {
		pr_warn("nvkvm: cpu_page_migrate mmap_on_isolate gva=0x%lx isolate=%u ret=%d\n",
			page_gva, ctx->session->isolate_id, ret);
		goto err_iso_handle;
	}

	/* Stash the GPA on the tracking node so a later range-swap can install
	 * it into libcuda's VMA in one shot.  Per-page VM_PFNMAP toggling on a
	 * VMA that still holds anon pages breaks gup_fast for the unmigrated
	 * neighbours, so we don't touch the guest VMA here. */

	/* Track the migration for writeback and cleanup at fd close. */
	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp) {
		/*
		 * "Accept the tracking leak" was not a leak of a kzalloc — cp is
		 * the ONLY record of this mapping, so without it nothing ever
		 * sends MUNMAP_ON_ISOLATE or CLOSE_HANDLE for it, and both the
		 * mmap token and the handle sit in QEMU's VM-global tables until
		 * the session dies (the handle permanently: see err_iso_handle).
		 * The page stays pinned too.  Unwind and fail the resolve.
		 */
		ret = -ENOMEM;
		nvkvm_virtio_munmap_on_isolate(ctx->session->isolate_id,
					       mmap_token);
		goto err_iso_handle;
	}
	cp->page        = page;
	cp->mm          = current->mm;
	cp->gva         = page_gva;
	cp->gpa         = gpa_base;
	cp->length      = PAGE_SIZE;
	cp->handle_id   = handle_id;
	cp->mmap_token  = mmap_token;
	cp->prot        = (__u32)prot;
	/* H-9: pin the mm_struct for the entry's lifetime so the release path
	 * may pass it to mmget_not_zero() instead of only comparing it.  A copy
	 * entry has no guest window PTEs to take down, but the ref keeps the
	 * field's meaning — and its mmdrop() — uniform across both entry
	 * kinds.  Released in nvkvm_cpu_page_release(). */
	if (cp->mm)
		mmgrab(cp->mm);

	mutex_lock(&ctx->cpu_pages_lock);
	list_add_tail(&cp->list, &ctx->cpu_pages);
	mutex_unlock(&ctx->cpu_pages_lock);
	return 0;

err_iso_handle:
	/*
	 * Reached only once copy_handle_to_isolate() has SUCCEEDED, which is
	 * what takes the isolate reference (nvkvm_handle_ref_isolate, on ret==0
	 * only — so the copy-failure branch above must not come here).  That
	 * reference makes nvkvm_handle_close() return -EBUSY, and the guest
	 * does not check the return value, so CLOSE_HANDLE on its own left the
	 * handle-table entry in_use for the life of the session: the only code
	 * that clears a held isolate_refcount is the full-session teardown in
	 * nvkvm_handle_close_session().  No reaper touches nv->handles.  Drop
	 * the reference first, exactly as nvkvm_cpu_page_release() and the
	 * chunk_fail_mapped unwind below both do.
	 */
	nvkvm_virtio_close_handle_on_isolate(handle_id, ctx->session->isolate_id);
err_handle:
	nvkvm_virtio_close_handle(handle_id);
err_slot:
	if (shm_slot >= 0)
		nvkvm_slot_free(&nvkvm, shm_slot);
err_page:
	put_page(page);
	return ret;
}

/*
 * nvkvm_efault_resolve — map a faulting GVA into the isolate after an EFAULT
 * from ioctl_on_isolate.
 *
 * GPU VMA (set up via MMAP_ON_ISOLATE): re-send the mmap command (handles
 * races where the isolate lost the mapping).
 *
 * CPU VMA (anonymous / file-backed, not GPU): pin the faulting page and upload
 * it to the isolate via a memfd (CPU userptr physical page migration).
 */
int nvkvm_efault_resolve(struct nvkvm_fd_ctx *ctx, __u64 fault_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long page_gva;
	unsigned long prot;
	int ret;

	if (!ctx->session->isolate_id)
		return -ENOENT;

	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)fault_addr);
	if (!vma || vma->vm_start > (unsigned long)fault_addr) {
		mmap_read_unlock(mm);
		return -EFAULT;
	}

	if (vma->vm_ops == &nvkvm_vm_ops && vma->vm_private_data) {
		/* GPU VMA — re-send MMAP_ON_ISOLATE */
		struct nvkvm_mmap_region *region = vma->vm_private_data;
		__u32 handle_id = region->handle_id;
		__u64 gva       = vma->vm_start;
		__u64 length    = vma->vm_end - vma->vm_start;
		__u64 offset    = region->offset;
		__u32 gprot     = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
				  (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
		mmap_read_unlock(mm);

		if (!handle_id)
			return -ENOENT;

		__u64 gpa_base;
		__u32 mmap_token;
		return nvkvm_virtio_mmap_on_isolate(ctx->session->isolate_id,
						    handle_id, gva, offset,
						    length, gprot, MAP_SHARED,
						    (unsigned int)ctx->session->id,
						    &gpa_base, &mmap_token);
	}

	/* CPU VMA — migrate the faulting page */
	page_gva = (unsigned long)fault_addr & PAGE_MASK;
	prot     = (vma->vm_flags & VM_READ  ? PROT_READ  : 0) |
		   (vma->vm_flags & VM_WRITE ? PROT_WRITE : 0);
	mmap_read_unlock(mm);

	ret = nvkvm_cpu_page_migrate(ctx, page_gva, prot);
	return ret;
}

/*
 * nvkvm_cpu_pages_refresh — re-upload every migrated CPU page before the GPU
 * reads it.
 *
 * A migration is a COPY: the guest page is copied into a shared slot that the
 * host driver reads.  cpu_page_migrate then remembers the GVA and returns
 * early next time, which is correct only while that GVA keeps holding the same
 * data.  It does not: free a pinned host buffer and allocate another, the
 * allocator hands back the same address, and the slot still holds the previous
 * buffer's bytes.  The GPU then computes from stale data with no error --
 * cuMemcpyHtoD returning a freed buffer's contents, an OpenCL kernel reading an
 * all-zero input.  See docs/reference/correctness.md.
 *
 * So refresh before forwarding, exactly mirroring the writeback afterwards:
 * upload guest -> slot here, download slot -> guest there.  Uncapped on
 * purpose, unlike writeback's batch of 64 -- a partial upload is a wrong
 * answer, not a slow one.
 */
void nvkvm_cpu_pages_refresh(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_cpu_page *cp;
	struct { __u32 handle_id; struct page *page; } *batch;
	int n = 0, cap = 0;

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list)
		if (cp->page)
			cap++;
	mutex_unlock(&ctx->cpu_pages_lock);
	if (!cap)
		return;

	batch = kmalloc_array(cap, sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return;

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list) {
		/* Range entries (page==NULL) are mapped, not copied: the guest
		 * VMA points at the memfd GPA, so there is nothing to re-upload. */
		if (!cp->page || n >= cap)
			continue;
		get_page(cp->page);
		batch[n].handle_id = cp->handle_id;
		batch[n].page      = cp->page;
		n++;
	}
	mutex_unlock(&ctx->cpu_pages_lock);

	for (int i = 0; i < n; i++) {
		int shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0)
			goto put;
		{
			void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
			if (slot_ptr) {
				void *kaddr = kmap_local_page(batch[i].page);
				memcpy(slot_ptr, kaddr, PAGE_SIZE);
				kunmap_local(kaddr);
				wmb();
				nvkvm_virtio_write_memory_handle(batch[i].handle_id,
								 0, shm_slot,
								 PAGE_SIZE);
			}
		}
		nvkvm_slot_free(&nvkvm, shm_slot);
put:
		put_page(batch[i].page);
	}
	kfree(batch);
}

/*
 * nvkvm_cpu_pages_writeback — for every writable migrated CPU page, read the
 * current memfd content back into the original guest physical page.
 *
 * Called after ioctl_on_isolate so that DtoH copies written by the NVIDIA
 * driver into the isolate's mapping are reflected back to the guest.
 */
void nvkvm_cpu_pages_writeback(struct nvkvm_fd_ctx *ctx)
{
#define NVKVM_WB_BATCH  64
	struct { __u32 handle_id; struct page *page; } batch[NVKVM_WB_BATCH];
	int n = 0;
	struct nvkvm_cpu_page *cp;

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cp, &ctx->cpu_pages, list) {
		/* Range entries (bulk migrate) have page==NULL: the guest VMA is
		 * remapped to the memfd GPA, so it reads the GPU's writes directly
		 * — no per-page writeback, and no page to deref. */
		if (!cp->page || !(cp->prot & PROT_WRITE) || n >= NVKVM_WB_BATCH)
			continue;
		get_page(cp->page);
		batch[n].handle_id = cp->handle_id;
		batch[n].page      = cp->page;
		n++;
	}
	mutex_unlock(&ctx->cpu_pages_lock);

	for (int i = 0; i < n; i++) {
		int shm_slot = nvkvm_slot_alloc(&nvkvm);
		if (shm_slot < 0)
			goto put;

		if (nvkvm_virtio_read_memory_handle(batch[i].handle_id, 0,
						    shm_slot, PAGE_SIZE) == 0) {
			void *slot_ptr = nvkvm_slot_addr(&nvkvm, shm_slot);
			if (slot_ptr) {
				void *kaddr = kmap_local_page(batch[i].page);
				rmb();
				memcpy(kaddr, slot_ptr, PAGE_SIZE);
				kunmap_local(kaddr);
				set_page_dirty(batch[i].page);
			}
		}
		nvkvm_slot_free(&nvkvm, shm_slot);
put:
		put_page(batch[i].page);
	}
#undef NVKVM_WB_BATCH
}

/*
 * nvkvm_cpu_pages_free — clean up all CPU page migrations for this fd.
 * Unmaps each range from the GUEST (audit H-9 — the anon-derived VM_PFNMAP
 * VMAs of migrate_range hold no reference on this fd, so we can get here with
 * the buffer still mapped), then from the isolate, closes the handle, and
 * releases the pin.
 */
void nvkvm_cpu_pages_free(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_cpu_page *cp, *tmp;
	LIST_HEAD(to_free);
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;

	mutex_lock(&ctx->cpu_pages_lock);
	list_splice_init(&ctx->cpu_pages, &to_free);
	mutex_unlock(&ctx->cpu_pages_lock);

	list_for_each_entry_safe(cp, tmp, &to_free, list)
		nvkvm_cpu_page_release(cp, isolate_id);
}

void nvkvm_mmap_release_fd(struct nvkvm_fd_ctx *ctx)
{
	struct nvkvm_mmap_region *region, *tmp;
	LIST_HEAD(to_free);
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;

	spin_lock(&ctx->mmap_lock);
	{
		struct nvkvm_mmap_region *r;
		/* Claim every fallback region while still holding the lock, so a
		 * concurrent vma_close sees ext_claimed and steps aside rather
		 * than tearing down a region this loop is about to free. */
		list_for_each_entry(r, &ctx->mmap_regions, list)
			if (r->ext_backed)
				r->ext_claimed = true;
	}
	list_splice_init(&ctx->mmap_regions, &to_free);
	spin_unlock(&ctx->mmap_lock);

	/*
	 * The two send-sync call sites below previously built `umsg` on the
	 * stack and passed it to sg_init_one().  On CONFIG_VMAP_STACK kernels
	 * (Ubuntu's default) virt_to_page() returns a bogus physical page for
	 * vmapped stack, so QEMU's DMA read sees zeros — hdr.type lands as 0
	 * and the QEMU dispatch rejects it with "unknown request type 0".
	 * The whole virtio queue then deadlocks because the inflight record
	 * never completes.  Copy into a kmalloc'd buffer (same workaround
	 * simple_req uses).
	 */
	list_for_each_entry_safe(region, tmp, &to_free, list) {
		if (region->ext_backed) {
			/* Same teardown as vma_close: the GPU mapping goes
			 * first, then the CPU mapping, then the RM object. */
			list_del(&region->list);
			if (region->vma)
				region->vma->vm_private_data = NULL;
			nvkvm_uvm_ext_release(region);
			continue;
		}
		if (region->handle_id && isolate_id) {
			struct {
				struct nvkvm_hdr                   hdr;
				struct nvkvm_req_munmap_on_isolate req;
			} *umsg;
			struct nvkvm_inflight uinf;
			umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
			if (umsg) {
				umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
				umsg->hdr.txn_id     = cpu_to_le32(
						atomic_inc_return(&nvkvm.next_txn_id));
				umsg->req.isolate_id = cpu_to_le32(isolate_id);
				umsg->req.mmap_token = cpu_to_le32(region->mmap_token);
				init_completion(&uinf.done);
				uinf.txn_id = le32_to_cpu(umsg->hdr.txn_id);
				nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
				kfree(umsg);
			}
		}
		list_del(&region->list);
		kfree(region);
	}
}

/*
 * nvkvm_gpa_in_mmap_window — check whether [base, base+len) falls inside
 * the dedicated mmap GPA window that the host allocated for us.
 * The window boundaries are populated during virtio init from the BAR.
 */
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len)
{
	/* The mmap window is exposed as a second memory region by the host;
	 * its start GPA and size are stored in nvkvm.mmap_window_* at init. */
	if (!nvkvm.mmap_window_gpa_base || !nvkvm.mmap_window_len)
		return false;
	if (gpa_base < nvkvm.mmap_window_gpa_base)
		return false;
	if (gpa_base + len > nvkvm.mmap_window_gpa_base +
			     nvkvm.mmap_window_len)
		return false;
	if (gpa_base + len < gpa_base)   /* overflow check */
		return false;
	return true;
}

/*
 * nvkvm_cpu_pages_migrate_range — migrate every guest page covering
 * [gva, gva+len) onto memfds shared with the isolate.  Pages already
 * migrated are skipped (cpu_page_migrate dedupes by gva).  Used to set
 * up the OS_DESCRIPTOR backing before forwarding NV_ESC_RM_ALLOC_MEMORY
 * so the kernel pin_user_pages call on the stub's task finds the same
 * physical pages libcuda is writing to in the guest.
 */
/*
 * Bulk migration chunk size.  Each chunk = ONE memfd + ONE mmap_on_isolate +
 * ONE remap_pfn_range (vs the old per-4KB-page path: ~5 forwarded round-trips
 * EACH — measured 2.44s for a 16MB OS_DESCRIPTOR).  2MB amortizes the ~4 fixed
 * per-chunk forwards over 512 pages (negligible).
 *
 * Migration is strictly PER CHUNK: copy the chunk into its memfd, swap that
 * chunk's PTEs onto the memfd's GPA, release that chunk's pinned guest pages —
 * and only then start the next chunk.  That ordering is what bounds the
 * duplicated data, and it is worth being precise about what is and is not
 * transient here:
 *
 *   - The memfd is NOT transient.  It is the backing store the guest VMA
 *     points at for the lifetime of the registration (the mapping has to be
 *     genuine passthrough), so total memfd bytes necessarily equal the
 *     registered size.  That is the data in its final home, not overhead, and
 *     it scales with the request because it must.
 *   - The pinned guest pages are the data's OLD home.  They are what we
 *     release as migration proceeds.
 *   - The DUPLICATE is only the window in which one chunk exists in both
 *     places at once — between copying it into its memfd and dropping its
 *     pins.  Per-chunk ordering pins that window at exactly ONE chunk, 2 MiB,
 *     whether the caller registers 16 MiB or 2 GiB.
 *
 * This function used to run Phase 1 (create and fill every chunk's memfd)
 * across the whole range before Phase 2 (one VMA swap) touched anything.  That
 * keeps every memfd alive simultaneously, so the duplicate scaled with the
 * request, and a fixed 8-entry chunk array plus a 16 MiB -E2BIG check were
 * added to bound it.  The ceiling was a side effect of that batching rather
 * than a cost anyone chose: the chunk SIZE had already made per-iteration
 * overhead negligible, so batching chunks on top of it bought nothing.
 * Restoring the per-chunk order removes the array, removes the per-call
 * ceiling, and tightens the duplicate from 16 MiB to 2 MiB.
 */
#define NVKVM_MIG_CHUNK   (2UL << 20)

/*
 * Largest range a single call will migrate.  This is no longer structural —
 * the loop is O(1) in memory — but a guest process should not be able to make
 * the guest kernel pin an unbounded amount in one ioctl, so the sanity check
 * stays.  2 GiB is derived from the host-side table this path actually
 * consumes: every chunk takes one entry in QEMU's fixed
 * NVKVM_ISO_MMAP_MAX = 8192 mmap-token table
 * (src/qemu/nvkvm_isolate_handlers.c), and that table is shared by every
 * isolate in the VM.  2 GiB / 2 MiB = 1024 tokens = 1/8 of it, so even eight
 * concurrent maximal registrations fit.  The same number keeps the
 * struct page * array at 4 MiB (kvmalloc, vmalloc-backed) and sits at 1/64 of
 * the 128 GiB sparse GPA window, so neither of those is the binding
 * constraint.  Measured: the host driver itself accepts >= 4 GiB, so this is a
 * guest-side policy limit, not a hardware one.
 */
#define NVKVM_MIG_MAX_RANGE   (2ULL << 30)

int nvkvm_cpu_pages_migrate_range(struct nvkvm_fd_ctx *ctx,
				  __u64 gva, __u64 len, unsigned long prot)
{
	unsigned long start = (unsigned long)gva & PAGE_MASK;
	unsigned long end   = ((unsigned long)gva + len + PAGE_SIZE - 1) &
			      PAGE_MASK;
	unsigned long range_len, npages, coff;
	unsigned long done_pages = 0;   /* pages already unpinned (migrated) */
	unsigned long dup_peak = 0;     /* max bytes duplicated at any instant */
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct page **pages = NULL;
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;
	size_t slot_bytes = nvkvm.slot_size;
	struct nvkvm_cpu_page *cpdup;
	long got = 0;
	unsigned long i;
	int ret = 0, nck = 0;
	ktime_t _t0 = ktime_get();   /* DIAG */

	if (!len)
		return 0;
	if (end < start)                  /* overflow */
		return -EINVAL;
	if (end - start > NVKVM_MIG_MAX_RANGE)   /* sanity, see the #define */
		return -E2BIG;
	if (!isolate_id)
		return -ENOENT;
	if (slot_bytes < PAGE_SIZE)
		return -ENOMEM;

	/* Dedup: if the start page is already migrated (range re-registered),
	 * the VMA is already VM_PFNMAP and gup would fail — treat as done.
	 *
	 * Only entries whose mapping is still installed may answer, so reap the
	 * stale ones first.  libcuda re-uses the address of a freed pinned host
	 * buffer for the next one, and without this the second buffer dedups
	 * against the first and inherits its memory: the GPU then reads the
	 * freed buffer's bytes.  See nvkvm_cpu_page_entry_live(). */
	nvkvm_cpu_pages_reap_stale(ctx);

	mutex_lock(&ctx->cpu_pages_lock);
	list_for_each_entry(cpdup, &ctx->cpu_pages, list)
		if (cpdup->gva == start) { mutex_unlock(&ctx->cpu_pages_lock); return 0; }
	mutex_unlock(&ctx->cpu_pages_lock);

	/* A new registration changes the set of valid ranges — drop any cached
	 * VALIDATE results so a stale "valid" can't survive a free+remap. */
	nvkvm_session_vcache_clear(ctx->session);

	range_len = end - start;
	npages    = range_len >> PAGE_SHIFT;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/*
	 * Pin every page up front — BEFORE any VMA mutation, so a later
	 * VM_PFNMAP toggle can't break gup_fast on the remainder.
	 *
	 * That constraint is about ACQUIRING the pins, not about holding them
	 * to the end: once we hold a reference on a page we can reach it with
	 * kmap_local_page() forever, with no further page-table or VMA lookup.
	 * So each chunk's pins are dropped as soon as that chunk is remapped,
	 * which is what keeps the duplicated data down to one chunk.
	 */
	while (got < npages) {
		long n = get_user_pages_fast(start + (got << PAGE_SHIFT),
					     npages - got, FOLL_WRITE, pages + got);
		if (n <= 0) { ret = (n < 0) ? (int)n : -EFAULT; goto err_unpin; }
		got += n;
	}

	/*
	 * ONE VMA conversion for the whole range, up front.  It moves no data,
	 * and it does not split the VMA: vm_flags_set()/vm_flags_clear() take
	 * no address range — they OR/AND bits on the vm_area_struct itself.
	 * Splitting is an mprotect()/madvise() behaviour (__split_vma()), which
	 * this path never invokes.
	 *
	 * Doing the conversion here rather than per chunk also means the
	 * per-chunk loop below only ever touches PTEs.
	 */
	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start || vma->vm_end < end) {
		mmap_write_unlock(mm);
		ret = -EFAULT; goto err_unpin;
	}
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	/*
	 * remap_pfn_range() refuses ANY sub-VMA remap on a copy-on-write
	 * mapping: is_cow_mapping() is (VM_SHARED|VM_MAYWRITE) == VM_MAYWRITE,
	 * and for such a VMA it returns -EINVAL unless the remap covers the
	 * whole VMA exactly.  Ordinary user memory — malloc(), or a
	 * MAP_PRIVATE|MAP_ANONYMOUS mmap — is exactly that, so before this
	 * every multi-chunk registration, and every single-chunk registration
	 * that did not happen to span its VMA exactly, failed with -EINVAL.
	 * That surfaced to userspace as CUDA_ERROR_INVALID_VALUE and was a
	 * second, undocumented cap sitting UNDER the 16 MiB one: measured on a
	 * 575.51.03 guest, a MAP_SHARED buffer registered fine at 4/8/16 MiB
	 * while a MAP_PRIVATE buffer of the identical size did not.
	 *
	 * We are converting this VMA into a straight passthrough mapping of
	 * host memfd pages; there is no copy-on-write left to perform, so drop
	 * VM_MAYWRITE.  VM_WRITE is untouched, so userspace keeps write access
	 * — only a later mprotect() trying to re-add PROT_WRITE is refused.
	 */
	if (is_cow_mapping(vma->vm_flags))
		vm_flags_clear(vma, VM_MAYWRITE);
	/*
	 * CACHED (write-back), NOT pgprot_noncached.  The GPA window is backed by
	 * a memfd — normal host RAM in a KVM RAM memslot — not real device MMIO.
	 * On x86 the guest's WB view, the stub's WB view of the same memfd, and
	 * the GPU's DMA are all cache-coherent (DMA snoops), so WB is correct.
	 * Mapping it UC made the guest's post-DtoH read of the result a stream of
	 * uncached, unprefetched loads — measured 0.07 GB/s vs 9.6 GB/s on the
	 * host (130x).  HtoD was unaffected because the guest fills the buffer
	 * while it is still cached anon memory (before the swap) and the stub
	 * then reads the memfd as host RAM — the guest never reads through the
	 * window on HtoD.  Leaving vm_page_prot at its default keeps it WB.
	 *
	 * NB: requesting WB here is necessary but NOT sufficient — because the GPA
	 * window is a PCI-BAR (non-RAM) region, remap_pfn_range silently downgrades
	 * the PTEs to UC-.  On Intel the EPT IPAT bit hid this; on AMD it does not,
	 * so we rewrite the PTEs to WB with nvkvm_force_range_wb() after each
	 * chunk's remap.
	 */
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	mmap_write_unlock(mm);

	/*
	 * Per chunk, strictly in order: move the data, register it, release its
	 * old home.  A partially migrated range is a legal intermediate state —
	 * nothing in it is usable from CUDA until the call returns — so this
	 * does not need to be atomic across the range, only across each chunk.
	 */
	for (coff = 0; coff < range_len; coff += NVKVM_MIG_CHUNK) {
		unsigned long clen = min((unsigned long)NVKVM_MIG_CHUNK,
					 range_len - coff);
		unsigned long cbase = start + coff;
		__u32 handle = 0, token = 0;
		__u64 gpa = 0;
		unsigned long uoff;
		struct nvkvm_cpu_page *cp;

		ret = nvkvm_virtio_open_memory_handle(
			(unsigned int)ctx->session->id, clen, &handle);
		if (ret)
			goto err_unpin;

		/* Upload clen bytes in slot-sized batches (one forward per slot,
		 * vs one per 4KB page in the old path). */
		for (uoff = 0; uoff < clen; uoff += slot_bytes) {
			unsigned long this = min((unsigned long)slot_bytes,
						 clen - uoff);
			unsigned long p;
			int slot = nvkvm_slot_alloc(&nvkvm);
			void *sp;

			if (slot < 0) { ret = -ENOSPC; goto chunk_fail_h; }
			sp = nvkvm_slot_addr(&nvkvm, slot);
			if (!sp) { nvkvm_slot_free(&nvkvm, slot); ret = -ENOMEM; goto chunk_fail_h; }
			for (p = 0; p < this; p += PAGE_SIZE) {
				unsigned long pidx = (coff + uoff + p) >> PAGE_SHIFT;
				void *ka = kmap_local_page(pages[pidx]);
				memcpy((char *)sp + p, ka, PAGE_SIZE);
				kunmap_local(ka);
			}
			wmb();
			ret = nvkvm_virtio_write_memory_handle(handle, uoff, slot,
							       (__u32)this);
			nvkvm_slot_free(&nvkvm, slot);
			if (ret) goto chunk_fail_h;
			continue;
chunk_fail_h:
			nvkvm_virtio_close_handle(handle);
			goto err_unpin;
		}

		/*
		 * From here until this chunk's pins are dropped, this chunk's
		 * data exists in two places: the pinned guest pages and the
		 * memfd.  That window is one chunk wide and never widens.
		 */
		if (clen > dup_peak)
			dup_peak = clen;

		ret = nvkvm_virtio_copy_handle_to_isolate(handle, isolate_id);
		if (ret) { nvkvm_virtio_close_handle(handle); goto err_unpin; }

		ret = nvkvm_virtio_mmap_on_isolate(isolate_id, handle,
						   cbase, 0, clen,
						   (__u32)prot, MAP_SHARED,
						   (unsigned int)ctx->session->id,
						   &gpa, &token);
		if (ret) {
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			nvkvm_virtio_close_handle(handle);
			goto err_unpin;
		}
		if (!nvkvm_gpa_in_mmap_window(gpa, clen)) {
			ret = -EIO;
			goto chunk_fail_mapped;
		}

		/*
		 * Swap THIS chunk's PTEs onto the memfd's GPA.  zap and remap
		 * happen inside one mmap_write_lock section, so there is never
		 * an instant in which a page of the range is unmapped: the
		 * chunks behind us point at their memfds, the chunks ahead of
		 * us still have their original anon PTEs, and userspace can
		 * touch either without faulting into a hole.
		 */
		mmap_write_lock(mm);
		vma = find_vma(mm, cbase);
		if (!vma || vma->vm_start > cbase || vma->vm_end < cbase + clen) {
			mmap_write_unlock(mm);
			ret = -EFAULT;
			goto chunk_fail_mapped;
		}
		/*
		 * Take the per-VMA write lock before touching PTEs.  mmap_lock
		 * alone does not exclude a fault that took only the per-VMA read
		 * lock (CONFIG_PER_VMA_LOCK, on by default since 6.4); such a
		 * fault could install an anon page between the zap and the
		 * remap, and remap_pte_range() BUG_ON()s a non-empty PTE.
		 * vm_flags_set() got us this implicitly on the whole-range
		 * conversion above; here we ask for it directly.
		 */
		vma_start_write(vma);
		/*
		 * Zapping ordinary anon PTEs in a VMA that is already VM_PFNMAP
		 * is safe on x86_64: CONFIG_ARCH_HAS_PTE_SPECIAL is set, so
		 * vm_normal_page() short-circuits on !pte_special(pte) BEFORE it
		 * consults vm_flags, and anon PTEs still resolve to their struct
		 * page and get refcounted and rmap-removed correctly.  VM_PFNMAP
		 * only changes the outcome for pte_special() entries, which is
		 * exactly what remap_pfn_range() installs (pte_mkspecial) for the
		 * chunks already migrated.  unmap_single_vma()'s untrack_pfn()
		 * is gated on VM_PAT, which track_pfn_remap() only sets for a
		 * remap covering the entire VMA — never the case for a chunk of
		 * a larger range.
		 */
		/* Check the precondition ourselves, because the failure is
		 * silent: zap_vma_ptes() returns void and zaps NOTHING unless
		 * the VMA is VM_PFNMAP/VM_MIXEDMAP.  The whole-range conversion
		 * above makes it VM_PFNMAP so this cannot fire, but if it ever
		 * did, remap_pfn_range() would meet non-empty PTEs and BUG_ON,
		 * and a skipped zap would leave the guest reading its old anon
		 * pages while the GPU reads the memfd — the exact silent
		 * divergence this driver already had once.  Bail instead. */
		if (!(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP))) {
			mmap_write_unlock(mm);
			pr_warn("nvkvm: refusing to migrate 0x%lx: VMA is not PFNMAP (flags 0x%lx)\n",
				cbase, (unsigned long)vma->vm_flags);
			ret = -EINVAL;
			goto chunk_fail_mapped;
		}
		nvkvm_zap_range(vma, cbase, clen);
		ret = remap_pfn_range(vma, cbase,
				      (unsigned long)(gpa >> PAGE_SHIFT),
				      clen, vma->vm_page_prot);
		if (!ret)
			nvkvm_force_range_wb(mm, cbase, cbase + clen);
		mmap_write_unlock(mm);
		if (ret)
			goto chunk_fail_mapped;

		/*
		 * The chunk is live and the VMA points at it.  Record it BEFORE
		 * dropping the pins, so cleanup at fd close can never miss a
		 * mapping we have already installed.  page==NULL marks a range
		 * entry: no writeback and no put_page at cleanup, because the
		 * guest reads the GPU's writes straight out of the memfd.
		 */
		cp = kzalloc(sizeof(*cp), GFP_KERNEL);
		if (cp) {
			cp->page       = NULL;
			cp->mm         = mm;
			cp->gva        = cbase;
			cp->gpa        = gpa;
			cp->length     = clen;
			cp->handle_id  = handle;
			cp->mmap_token = token;
			cp->prot       = (__u32)prot;
			/* H-9: record the whole chunk, not just its first
			 * page, and pin the mm — the release paths have to
			 * zap [gva, gva+length) before this extent can be
			 * recycled by another isolate.  mmdrop() in
			 * nvkvm_cpu_page_release(). */
			mmgrab(mm);
			mutex_lock(&ctx->cpu_pages_lock);
			list_add_tail(&cp->list, &ctx->cpu_pages);
			mutex_unlock(&ctx->cpu_pages_lock);
		}   /* else: mapping is live; accept the tracking leak */

		/*
		 * Release this chunk's pinned pages.  The data's only home is
		 * now the memfd, so the guest gets these page frames back
		 * immediately instead of at the end of the whole registration.
		 * This is the half of the per-chunk order that actually bounds
		 * the duplicate.
		 */
		for (i = coff >> PAGE_SHIFT;
		     i < (coff + clen) >> PAGE_SHIFT; i++)
			put_page(pages[i]);
		done_pages = (coff + clen) >> PAGE_SHIFT;
		nck++;
		continue;

chunk_fail_mapped:
		nvkvm_virtio_munmap_on_isolate(isolate_id, token);
		nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
		nvkvm_virtio_close_handle(handle);
		goto err_unpin;
	}

	/* Every chunk is migrated, recorded and unpinned.  Nothing is left to
	 * do but free the descriptor array. */
	kvfree(pages);
	pr_info("nvkvm DIAG: migrate_range(bulk) %lu pages, %d chunks, dup_peak=%lu B in %lld us\n",
		npages, nck, dup_peak,
		ktime_to_us(ktime_sub(ktime_get(), _t0)));
	return 0;

err_unpin:
	/*
	 * Drop only the pins we still hold.  Pages below done_pages belong to
	 * chunks that are already migrated: their pins were released at the end
	 * of their own iteration and their mappings are recorded in
	 * ctx->cpu_pages, so nvkvm_cpu_pages_free() will tear them down at fd
	 * close.  Unpinning them again here would be a double put_page().
	 *
	 * Those already-migrated chunks stay mapped.  A partially migrated range
	 * is a legal intermediate state, and there is no clean way to undo one:
	 * the original anon pages are gone.  The caller gets the error and the
	 * registration fails, which is the same thing userspace saw before.
	 */
	for (i = done_pages; i < (unsigned long)got; i++)
		put_page(pages[i]);
	kvfree(pages);
	pr_warn("nvkvm: migrate_range(bulk) failed ret=%d at chunk %d (%d chunk(s) already migrated and left mapped)\n",
		ret, nck, nck);
	return ret;
}
