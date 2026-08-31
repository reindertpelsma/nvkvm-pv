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
#include <linux/fs.h>          /* file_inode() — VMA type check below */
#include <uapi/linux/magic.h>  /* TMPFS_MAGIC  — VMA type check below */
#include <asm/pgtable_types.h>

#include "nvkvm.h"

/* Forward declarations for module-internal functions */
static void nvkvm_vma_open(struct vm_area_struct *vma);
static void nvkvm_vma_close(struct vm_area_struct *vma);
bool nvkvm_gpa_in_mmap_window(unsigned long gpa_base, unsigned long len);
/* Object-keyed sharing registry (defined ahead of migrate_range, below) —
 * forward-declared so nvkvm_cpu_page_release() can deregister on release. */
static void nvkvm_shared_remove_handle(__u32 handle_id);

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
	struct nvkvm_mmap_region *region = vma->vm_private_data;

	if (region)
		refcount_inc(&region->vma_refs);
}

static void nvkvm_vma_close(struct vm_area_struct *vma)
{
	struct nvkvm_mmap_region *region = vma->vm_private_data;
	if (!region)
		return;
	vma->vm_private_data = NULL;

	/*
	 * fork() inheritance is supported deliberately: the child receives the
	 * same CPU mapping and an inherited nvkvm fd still names the parent's
	 * session.  VMA splits (including partial munmap) have the same callback
	 * pattern.  Only the last copy may tear down the shared host/GPA backing.
	 */
	if (!refcount_dec_and_test(&region->vma_refs))
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
		} else {
			/* Defensive fd-teardown fallback: mmap_release_fd detached
			 * this still-live VMA from the fd list.  The region is now
			 * self-contained, and the last VMA owns its teardown. */
			mine = true;
		}
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
		/* Zeroed, not just partly filled: nvkvm_send_sync() branches on
		 * inf->isolate_id to decide between an interruptible wait and an
		 * uninterruptible one, and on the interruptible side it asks THAT
		 * isolate to interrupt txn_id.  Left as stack garbage it would
		 * interrupt an arbitrary isolate's in-flight ioctl.  This is a
		 * control-plane request, so isolate_id must stay 0. */
		struct nvkvm_inflight uinf = { 0 };
		__u32 utxn;
		umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
		/* Take the txn_id from the allocator, not off the seed: the
		 * bitmap is what stops a wrapped seed reusing an id that is
		 * still outstanding on another waiter.  Exhaustion is handled
		 * exactly like the kzalloc failure -- skip the munmap request,
		 * which leaves the host mapping to be reaped at isolate exit. */
		utxn = umsg ? nvkvm_txn_id_alloc(&nvkvm) : 0;
		if (umsg && utxn) {
			umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
			umsg->hdr.txn_id     = cpu_to_le32(utxn);
			umsg->req.isolate_id = cpu_to_le32(ctx->session->isolate_id);
			umsg->req.mmap_token = cpu_to_le32(mmap_token);
			init_completion(&uinf.done);
			uinf.txn_id = utxn;
			nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
			nvkvm_txn_id_free(&nvkvm, utxn);
		}
		kfree(umsg);
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
	refcount_set(&region->vma_refs, 1);

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
static int nvkvm_uvm_mmap_intent(struct nvkvm_fd_ctx *ctx, __u64 gva,
				 unsigned long len)
{
	struct nvkvm_uvm_fd_state *st = ctx->uvm_state;
	struct nvkvm_uvm_mapping_intent *m;
	bool found = false;

	if (!st)
		return 0;
	lockdep_assert_held(&st->ext_lock);
	mutex_lock(&st->lock);
	if (!st->shadow_valid) {
		mutex_unlock(&st->lock);
		return -EIO;
	}
	list_for_each_entry(m, &st->intents, list) {
		if (m->base == gva && m->length == len) {
			found = true;
			break;
		}
	}
	mutex_unlock(&st->lock);
	return found ? 1 : 0;
}

int nvkvm_mmap_request(struct nvkvm_fd_ctx *ctx, struct vm_area_struct *vma)
{
	unsigned long vma_len = vma->vm_end - vma->vm_start;
	int intent;

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
	if (ctx->dev_id == NVKVM_DEV_UVM && ctx->uvm_state) {
		int ret;

		/* One transaction with ALLOC_SEMAPHORE_POOL/FREE: classification
		 * and the selected mmap must not straddle an ioctl whose host state
		 * and local intent are committed under this same lock. */
		mutex_lock(&ctx->uvm_state->ext_lock);
		intent = nvkvm_uvm_mmap_intent(ctx, vma->vm_start, vma_len);
		pr_debug("nvkvm: UVM mmap gva=0x%lx len=0x%lx classification=%s\n",
			 vma->vm_start, vma_len,
			 intent < 0 ? "invalid-shadow" :
			 intent ? "forwarded-intent" : "managed-fallback");
		if (intent < 0)
			ret = intent;
		else if (!intent)
			ret = nvkvm_uvm_ext_mmap(ctx, vma);
		else
			ret = nvkvm_mmap_request_isolate(ctx, vma);
		mutex_unlock(&ctx->uvm_state->ext_lock);
		return ret;
	}

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
	if (!st->shadow_valid || !st->realize_valid) {
		mutex_unlock(&st->lock);
		return -EIO;
	}
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
		snap->gpus[n_gpus].rm_ctrl_fd_handle_id =
			cpu_to_le32(g->rm_ctrl_fd_handle_id);
		snap->gpus[n_gpus].h_client = cpu_to_le32(g->h_client);
		snap->gpus[n_gpus].h_smc_part_ref =
			cpu_to_le32(g->h_smc_part_ref);
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
 * cpu_pages_lock, so we take mmap_lock with no nvkvm lock held.
 *
 *   nvkvm_cpu_pages_reap_stale() runs from the two migration entry points —
 *   nvkvm_efault_resolve() drops mmap_read_lock before calling
 *   nvkvm_cpu_page_migrate(), and nvkvm_cpu_pages_migrate_range() reaps before
 *   it takes mmap_write_lock itself.  Neither reaches here holding mmap_lock.
 *
 * BUT ONE CALLER OF nvkvm_cpu_pages_free() DOES HOLD mmap_write_lock, AND THIS
 * COMMENT USED TO CLAIM NONE DID.  The claim was that it runs only from
 * ->release, where fput() defers the last put to task_work, or from a proxy-GEM
 * / dma-buf put.  There is a third way in, and it is a straight self-deadlock
 * on a non-recursive rwsem:
 *
 *   remove_vma()                       [kernel, mmap_write_lock HELD]
 *     -> nvkvm_vma_close()             (this file)
 *     -> nvkvm_uvm_ext_release()       (nvkvm_uvm_ext.c)
 *     -> ext_unwind()
 *     -> nvkvm_fd_ctx_put(r->ext_map_ctl)
 *     -> nvkvm_fd_ctx_destroy()        (nvkvm_main.c)
 *     -> nvkvm_cpu_pages_free()
 *     -> here -> mmap_write_lock(mm)   [SAME mm]
 *
 * It does not fire today, and only by luck: ext_map_ctl is an internally
 * created NVKVM_DEV_CTL context whose ioctls all go down the _nomm path, so it
 * never runs nvkvm_efault_resolve() and never accumulates a cpu_pages entry —
 * and with the list empty this function is never called.  NOTHING ENFORCES
 * THAT.  One forwarded ioctl on that context that EFAULTs, or one future
 * migration recorded against it, and a plain munmap() of a managed range wedges
 * the calling thread with mmap_write_lock held: unkillable, and every reader of
 * its /proc/<pid>/maps hangs behind it.  Process exit is the one safe case,
 * because mmget_not_zero() below already fails once exit_mmap() has run.
 *
 * NOT FIXED HERE, deliberately, because every available fix is a design choice
 * and not a patch:
 *   - a trylock here would have to skip the zap on failure, which reinstates
 *     the H-9 cross-process aliasing this function exists to prevent;
 *   - deferring the ext_map_ctl put to a workqueue fixes it but adds a new
 *     deferred-teardown lifetime (and a module-unload flush) to a path whose
 *     whole point is that the host range teardown must NOT be deferred; and
 *   - splitting the ctl-context teardown so the cpu_pages part runs outside
 *     mmap_lock changes the ordering nvkvm_fd_ctx_destroy() documents.
 * Pick one with the UVM state machine's owner.  Until then: DO NOT let an
 * ext_map_ctl context acquire cpu_pages entries.
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

	/* Object-keyed sharing: forget this handle the moment ANY cp entry
	 * referencing it goes away (creator's or a sharer's) — conservative,
	 * always safe.  See the comment above nvkvm_cpu_pages_share_range(). */
	if (!cp->page)
		nvkvm_shared_remove_handle(cp->handle_id);

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
			/* A live file-backed VMA owns a struct-file reference, so
			 * normal fd destruction cannot reach this branch before its
			 * final vm_ops.close removed the region.  Never dereference
			 * region->vma here: after a split it names only one of several
			 * VMAs and may already be stale. */
			if (WARN_ON_ONCE(refcount_read(&region->vma_refs) != 0)) {
				/* This should be impossible because vm_file keeps the
				 * fd alive.  If a future kernel/lifetime change violates
				 * it, detach safely and let the last VMA close release the
				 * backing instead of freeing beneath a live mapping. */
				region->ctx = NULL;
				region->ext_claimed = false;
				list_del_init(&region->list);
				continue;
			}
			list_del(&region->list);
			nvkvm_uvm_ext_release(region);
			continue;
		}
		if (region->handle_id && isolate_id) {
			struct {
				struct nvkvm_hdr                   hdr;
				struct nvkvm_req_munmap_on_isolate req;
			} *umsg;
			/* Zeroed: see the identical unwind in nvkvm_mmap().
			 * It matters more here -- this runs at process exit with
			 * a SIGKILL already pending, so nvkvm_send_sync() takes
			 * the interruptible branch by construction and would act
			 * on whatever isolate_id the stack happened to hold.
			 * uinf.isolate_id stays 0: this is control plane, and the
			 * wait must not be interruptible. */
			struct nvkvm_inflight uinf = { 0 };
			__u32 utxn;
			umsg = kzalloc(sizeof(*umsg), GFP_KERNEL);
			utxn = umsg ? nvkvm_txn_id_alloc(&nvkvm) : 0;
			if (umsg && utxn) {
				umsg->hdr.type       = cpu_to_le32(NVKVM_REQ_MUNMAP_ON_ISOLATE);
				umsg->hdr.txn_id     = cpu_to_le32(utxn);
				umsg->req.isolate_id = cpu_to_le32(isolate_id);
				umsg->req.mmap_token = cpu_to_le32(region->mmap_token);
				init_completion(&uinf.done);
				uinf.txn_id = utxn;
				nvkvm_send_sync(&nvkvm, umsg, sizeof(*umsg), &uinf);
				nvkvm_txn_id_free(&nvkvm, utxn);
			}
			kfree(umsg);
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
 * Is this VMA's file one of the kernel's in-memory pseudo-filesystems, i.e. is
 * the mapping still just process memory?  MAP_SHARED|MAP_ANONYMOUS carries an
 * internal shmem (or, without CONFIG_SHMEM, ramfs) file; a mapping of anything
 * else is a real file whose pages we must not replace.
 */
static bool nvkvm_vma_file_is_memory(struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(vma->vm_file);

	if (!inode || !inode->i_sb)
		return false;

	/*
	 * Provenance is NOT the test.  An earlier revision returned
	 * IS_PRIVATE(inode) here to reject memfds, because a VMM's guest-RAM
	 * memfd is the aliased case that corrupted under nesting.  That asked
	 * who CREATED the object, which is the wrong question twice over: it
	 * missed MAP_SHARED|MAP_ANONYMOUS shared across fork() (shmem_zero_setup
	 * sets S_PRIVATE, so it passed), and it refused single-view memfds that
	 * were never a problem.
	 *
	 * The real test -- "do these pages already have another mapping" -- is
	 * page_mapcount() in nvkvm_cpu_pages_migrate_range(), which subsumes
	 * this one: an aliased guest-RAM memfd has a second view by definition.
	 * Measured: with the mapcount check in place, the nested case is still
	 * refused with this predicate back to plain magic, and a single-view
	 * memfd works again.  So keep this answering only its original
	 * question -- is the mapping still just process memory.
	 */
	return inode->i_sb->s_magic == TMPFS_MAGIC ||
	       inode->i_sb->s_magic == RAMFS_MAGIC;
}

/*
 * Bulk migration chunk size — moved up from just above
 * nvkvm_cpu_pages_migrate_range() (its full rationale is there) so the
 * object-keyed sharing code below can chunk the registry the identical way
 * the relocate path does.  That identical alignment is what makes a
 * sharer's per-chunk (inode, pgoff) keys land exactly on the keys the
 * original registrant recorded: both processes computed cbase from the
 * same vm_start/vm_pgoff (fork() copies the VMA verbatim) stepped by the
 * same constant, so chunk N's pgoff is bit-identical on both sides.
 */
#define NVKVM_MIG_CHUNK   (2UL << 20)

/*
 * ─── Object-keyed cross-process sharing (fork_both_register.c) ─────────────
 *
 * The mapcount guard below refuses to RELOCATE a range that is mapped more
 * than once — correctly, since relocating repoints only the caller's own
 * VMA and strands every other view on the original pages (see the guard's
 * own comment).  But it is one-sided: it stops a SECOND registrant from
 * stranding a FIRST, not a first registrant from stranding everyone.  Linux
 * does not eagerly copy PTEs for a shared file-backed VMA at fork(), so the
 * FIRST registration on such a range sees mapcount == 1 (the sibling's PTE
 * does not exist yet) and sails through, relocating the object's pages into
 * a memfd and repointing only its own VMA.  The sibling's later
 * registration then hits the guard, refuses, and — before this — left the
 * two processes on divergent memory with every ioctl reporting success.
 *
 * Fix: key the migration by the BACKING OBJECT (inode, pgoff), not by GVA —
 * a GVA is per-process and useless for recognising "this is the same
 * buffer" across two different address spaces.  Before a range that is
 * backed by a shmem object (MAP_SHARED|MAP_ANONYMOUS — the only case that
 * can legitimately have a second view; see nvkvm_vma_file_is_memory())
 * would be relocated, check whether its object was already migrated under
 * an earlier registration.  If every chunk the caller would touch is
 * already covered, do not relocate again: map the SAME memfds into THIS
 * process's isolate at THIS process's VA and retype this VMA to point at
 * the same window GPAs.  Both VMAs then reference one memfd and can never
 * desync — and the mapcount guard never runs for this call, because
 * nothing here is being relocated.  Runs in the registrant's own process
 * context, so no foreign mmap_lock is ever touched.
 *
 * The registry only ever answers "was this EXACT (inode, pgoff, len) chunk
 * already migrated" — no partial-coverage stitching.  A partial match falls
 * straight through to the ordinary pin-and-relocate path below, mapcount
 * guard included, unaffected by any of this: the guard still refuses to
 * relocate any range that turns out to have a second view.  That is enough
 * for the tested shape (whole-buffer refork registration, the realistic
 * case for cuMemHostRegister) without teaching the registry to reason about
 * overlapping sub-ranges nothing here needs.
 *
 * Lifetime: `inode` is stored ONLY for pointer-identity comparison and is
 * NEVER dereferenced through the registry (the same discipline
 * nvkvm_cpu_page_entry_live() already uses for `cp->mm`).  An entry is
 * removed the moment ANY nvkvm_cpu_page referencing its handle is released
 * (nvkvm_cpu_page_release(), below) — conservative (it may drop the entry
 * while another process is still actively sharing the same handle) but
 * always safe, and it guarantees the registry never outlives the mapping
 * that justified trusting the inode pointer in the first place: at least
 * one live, unretyped reference to that inode (the surviving process's own
 * vm_file) still exists whenever an entry for it does.
 *
 * This registry alone is not sufficient for fork_both_register.c's actual
 * shape -- two peer processes racing out of fork() with no ordering between
 * them, not "one finishes, then later another registers." See the pending-
 * claim mechanism (nvkvm_shared_resolve() and friends) below the registry
 * helpers for why and how that race is closed.
 */
struct nvkvm_shared_range {
	struct list_head list;
	struct inode    *inode;    /* identity only — never dereferenced */
	unsigned long    pgoff;    /* first page of this chunk, in PAGE_SIZE units */
	unsigned long    len;      /* bytes */
	__u32            handle_id;
};

static DEFINE_MUTEX(nvkvm_shared_lock);
static LIST_HEAD(nvkvm_shared_ranges);

static __u32 nvkvm_shared_find(struct inode *inode, unsigned long pgoff,
			       unsigned long len)
{
	struct nvkvm_shared_range *sr;
	__u32 handle = 0;

	mutex_lock(&nvkvm_shared_lock);
	list_for_each_entry(sr, &nvkvm_shared_ranges, list) {
		if (sr->inode == inode && sr->pgoff == pgoff && sr->len == len) {
			handle = sr->handle_id;
			break;
		}
	}
	mutex_unlock(&nvkvm_shared_lock);
	return handle;
}

static void nvkvm_shared_add(struct inode *inode, unsigned long pgoff,
			     unsigned long len, __u32 handle_id)
{
	struct nvkvm_shared_range *sr = kzalloc(sizeof(*sr), GFP_KERNEL);

	if (!sr)
		return;   /* best-effort: worst case a later sharer re-migrates */
	sr->inode     = inode;
	sr->pgoff     = pgoff;
	sr->len       = len;
	sr->handle_id = handle_id;
	mutex_lock(&nvkvm_shared_lock);
	list_add_tail(&sr->list, &nvkvm_shared_ranges);
	mutex_unlock(&nvkvm_shared_lock);
}

static void nvkvm_shared_remove_handle(__u32 handle_id)
{
	struct nvkvm_shared_range *sr, *tmp;

	mutex_lock(&nvkvm_shared_lock);
	list_for_each_entry_safe(sr, tmp, &nvkvm_shared_ranges, list) {
		if (sr->handle_id == handle_id) {
			list_del(&sr->list);
			kfree(sr);
		}
	}
	mutex_unlock(&nvkvm_shared_lock);
}

/* Caller holds nvkvm_shared_lock. */
static bool nvkvm_shared_fully_covered_locked(struct inode *inode,
					      unsigned long pgoff_base,
					      unsigned long range_len)
{
	unsigned long coff;

	for (coff = 0; coff < range_len; coff += NVKVM_MIG_CHUNK) {
		unsigned long clen = min((unsigned long)NVKVM_MIG_CHUNK,
					 range_len - coff);
		unsigned long pgoff = pgoff_base + (coff >> PAGE_SHIFT);
		struct nvkvm_shared_range *sr;
		bool hit = false;

		list_for_each_entry(sr, &nvkvm_shared_ranges, list) {
			if (sr->inode == inode && sr->pgoff == pgoff &&
			    sr->len == clen) {
				hit = true;
				break;
			}
		}
		if (!hit)
			return false;
	}
	return true;
}

/*
 * ─── The race the registry alone does not close ────────────────────────────
 *
 * fork_both_register.c is not "register, then later someone else registers
 * the same object" -- it is two ALREADY-RUNNING processes independently
 * calling migrate_range for the same object at close to the same time, with
 * no ordering between them.  A plain "check the registry, share if found,
 * else migrate" has a window: both calls can probe before EITHER has
 * finished migrating and recorded anything, both see nothing, and both fall
 * into the ordinary pin-and-relocate path -- reintroducing exactly the
 * mapcount race this whole mechanism exists to close, just delayed rather
 * than removed.  Measured: this is not a corner case, it is the common case
 * for two peer processes racing out of fork() with no synchronisation.
 *
 * Fix: a second, lighter-weight PENDING list.  The first caller to reach a
 * given inode with no completed registry entry does not just start
 * migrating -- it publishes a pending claim for that inode first.  Every
 * later caller for the SAME inode, arriving before the claim resolves,
 * waits on it instead of racing past it, then re-decides once it resolves:
 * on success the registry now has the answer and it shares; on failure the
 * claim is gone and it becomes the new claimant and tries the migration
 * itself.  Either way, no second caller ever begins pinning while a sibling
 * is mid-migration on the same object.
 *
 * Granularity is per INODE, not per (inode, pgoff, len): a second caller for
 * a genuinely disjoint sub-range of the same large object waits for an
 * unrelated migration to finish before it can even start its own. That is a
 * throughput cost, not a correctness one -- disjoint concurrent
 * sub-registrations of one object are not a shape any current caller
 * produces (cuMemHostRegister registers one buffer, one object, in full).
 * Keying finer is a straightforward follow-up if that ever changes.
 *
 * Refcounting: `refs` starts at 1 (the claimant's own reference) and is
 * touched only under nvkvm_shared_lock. A waiter increments it while still
 * holding the lock (so it can only find `pend` while it is still on the
 * list) before dropping the lock to sleep; nvkvm_shared_claim_release()
 * unlinks `pend` from the list and decrements under the SAME lock section,
 * so no thread can observe (let alone increment-reference) a pending entry
 * that is no longer reachable from the list -- the classic "list_del and
 * the recount that retires an object happen in one critical section"
 * invariant. Whoever's decrement reaches zero frees it; complete_all() runs
 * strictly before that free can be reached by any waiter still asleep, since
 * a waiter cannot resume (and therefore cannot free) before complete_all()
 * wakes it.
 */
struct nvkvm_shared_pending {
	struct list_head list;
	struct inode    *inode;   /* identity only — never dereferenced */
	struct completion done;
	int              refs;
};

static LIST_HEAD(nvkvm_shared_pending_list);

/* Caller holds nvkvm_shared_lock. */
static struct nvkvm_shared_pending *
nvkvm_shared_find_pending_locked(struct inode *inode)
{
	struct nvkvm_shared_pending *p;

	list_for_each_entry(p, &nvkvm_shared_pending_list, list)
		if (p->inode == inode)
			return p;
	return NULL;
}

/*
 * nvkvm_shared_claim_release — the claimant's migration attempt (success or
 * failure) is decided.  Unlink the pending marker so it can no longer be
 * found, wake every waiter so they re-decide, and free it once the last
 * reference (ours, or a waiter still catching up on the lock) is gone.
 */
static void nvkvm_shared_claim_release(struct nvkvm_shared_pending *pend)
{
	bool free_it;

	mutex_lock(&nvkvm_shared_lock);
	list_del(&pend->list);
	free_it = (--pend->refs == 0);
	mutex_unlock(&nvkvm_shared_lock);

	complete_all(&pend->done);
	if (free_it)
		kfree(pend);
}

/* A waiter's turn is over (it woke up, or never had to wait) — drop the
 * reference it took to observe `pend` safely across the sleep. */
static void nvkvm_shared_pending_put(struct nvkvm_shared_pending *pend)
{
	bool free_it;

	mutex_lock(&nvkvm_shared_lock);
	free_it = (--pend->refs == 0);
	mutex_unlock(&nvkvm_shared_lock);
	if (free_it)
		kfree(pend);
}

/*
 * nvkvm_shared_resolve — decide what a possibly-shmem-backed [start,end)
 * should do, waiting out any concurrent sibling migration of the same
 * object along the way.  See the race comment above.
 *
 * Returns:
 *   true  -- SHARE.  *inode_out / *pgoff_out set; caller runs
 *            nvkvm_cpu_pages_share_range() and returns its result.
 *   false -- MIGRATE the ordinary way.  *claim_out is NULL when the range
 *            is not shmem-backed at all (nothing to claim or release).
 *            Otherwise *claim_out is this call's own claim and MUST be
 *            passed to nvkvm_shared_claim_release() from every exit path of
 *            the ordinary migrate, exactly once, once success or failure is
 *            known — that release is what wakes any sibling waiting to
 *            re-check the registry.
 */
static bool nvkvm_shared_resolve(struct mm_struct *mm,
				 unsigned long start, unsigned long end,
				 struct inode **inode_out,
				 unsigned long *pgoff_out,
				 struct nvkvm_shared_pending **claim_out)
{
	*claim_out = NULL;

	for (;;) {
		struct vm_area_struct *vma;
		struct inode *inode = NULL;
		unsigned long pgoff_base = 0;
		bool is_shmem;
		struct nvkvm_shared_pending *pend;

		mmap_read_lock(mm);
		vma = find_vma(mm, start);
		is_shmem = vma && vma->vm_start <= start && vma->vm_end >= end &&
			  vma->vm_file && nvkvm_vma_file_is_memory(vma) &&
			  !(vma->vm_flags &
			    (VM_PFNMAP | VM_IO | VM_MIXEDMAP | VM_HUGETLB));
		if (is_shmem) {
			inode = file_inode(vma->vm_file);
			pgoff_base = vma->vm_pgoff +
				((start - vma->vm_start) >> PAGE_SHIFT);
		}
		mmap_read_unlock(mm);

		if (!is_shmem)
			return false;   /* ordinary MAP_PRIVATE|ANON: nothing to claim */

		mutex_lock(&nvkvm_shared_lock);
		if (nvkvm_shared_fully_covered_locked(inode, pgoff_base,
						      end - start)) {
			mutex_unlock(&nvkvm_shared_lock);
			*inode_out = inode;
			*pgoff_out = pgoff_base;
			return true;
		}
		pend = nvkvm_shared_find_pending_locked(inode);
		if (pend) {
			pend->refs++;
			mutex_unlock(&nvkvm_shared_lock);
			wait_for_completion(&pend->done);
			nvkvm_shared_pending_put(pend);
			continue;   /* re-decide: share, wait again, or claim */
		}

		pend = kzalloc(sizeof(*pend), GFP_KERNEL);
		if (!pend) {
			/* Best-effort: proceed unclaimed rather than fail the
			 * registration over a bookkeeping allocation. A racing
			 * sibling in this rare case falls back to the pre-fix
			 * behaviour (mapcount guard decides) instead of
			 * waiting -- correct, just not optimal. */
			mutex_unlock(&nvkvm_shared_lock);
			return false;
		}
		pend->inode = inode;
		pend->refs  = 1;
		init_completion(&pend->done);
		list_add_tail(&pend->list, &nvkvm_shared_pending_list);
		mutex_unlock(&nvkvm_shared_lock);

		*inode_out  = inode;
		*pgoff_out  = pgoff_base;
		*claim_out  = pend;
		return false;
	}
}

/*
 * nvkvm_cpu_pages_share_range — the share path proper.  Caller has already
 * confirmed every chunk of [start,end) resolves in the registry.  Maps each
 * existing handle into THIS process's isolate at THIS process's VA and
 * retypes this VMA to point at the same window GPAs — no pin, no gup, no
 * mapcount check, because nothing is being relocated.
 *
 * On any failure this returns the error directly rather than falling back
 * to the relocate path: chunks already shared in this call stay mapped
 * (the same "partial registration is a legal intermediate state" contract
 * nvkvm_cpu_pages_migrate_range's own err_unpin path documents), and
 * falling back would try to gup pages that, for the chunks already shared,
 * no longer back this VMA at all.
 */
static int nvkvm_cpu_pages_share_range(struct nvkvm_fd_ctx *ctx,
				       unsigned long start, unsigned long end,
				       unsigned long prot,
				       struct inode *want_inode,
				       unsigned long pgoff_base)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long coff, range_len = end - start;
	__u32 isolate_id = ctx->session ? ctx->session->isolate_id : 0;
	bool cow_forced_write = false;
	int ret;

	if (!isolate_id)
		return -ENOENT;

	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start || vma->vm_end < end) {
		mmap_write_unlock(mm);
		return -EFAULT;
	}
	/* Re-validate identity under the write lock — see the probe's comment. */
	if ((vma->vm_flags & (VM_PFNMAP | VM_IO | VM_MIXEDMAP | VM_HUGETLB)) ||
	    !vma->vm_file || file_inode(vma->vm_file) != want_inode ||
	    vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT) != pgoff_base) {
		mmap_write_unlock(mm);
		return -EAGAIN;
	}

	/* Same VMA retype the relocate path performs — see its comments above
	 * (is_cow_mapping / VM_SHARED / vm_get_page_prot) for why. */
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	if (is_cow_mapping(vma->vm_flags)) {
		vm_flags_clear(vma, VM_MAYWRITE);
		cow_forced_write = true;
	}
	vma->vm_page_prot = vm_get_page_prot(cow_forced_write
					     ? (vma->vm_flags | VM_SHARED)
					     : vma->vm_flags);
	mmap_write_unlock(mm);

	for (coff = 0; coff < range_len; coff += NVKVM_MIG_CHUNK) {
		unsigned long clen = min((unsigned long)NVKVM_MIG_CHUNK,
					 range_len - coff);
		unsigned long cbase = start + coff;
		unsigned long pgoff = pgoff_base + (coff >> PAGE_SHIFT);
		__u32 handle = nvkvm_shared_find(want_inode, pgoff, clen);
		__u64 gpa = 0;
		__u32 token = 0;
		struct nvkvm_cpu_page *cp;

		if (!handle)
			return -EAGAIN;   /* raced: entry disappeared mid-loop */

		ret = nvkvm_virtio_copy_handle_to_isolate(handle, isolate_id);
		if (ret)
			return ret;

		ret = nvkvm_virtio_mmap_on_isolate(isolate_id, handle,
						   cbase, 0, clen,
						   (__u32)prot, MAP_SHARED,
						   (unsigned int)ctx->session->id,
						   &gpa, &token);
		if (ret) {
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			return ret;
		}
		if (!nvkvm_gpa_in_mmap_window(gpa, clen)) {
			nvkvm_virtio_munmap_on_isolate(isolate_id, token);
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			return -EIO;
		}

		mmap_write_lock(mm);
		vma = find_vma(mm, cbase);
		if (!vma || vma->vm_start > cbase || vma->vm_end < cbase + clen ||
		    !(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP))) {
			mmap_write_unlock(mm);
			nvkvm_virtio_munmap_on_isolate(isolate_id, token);
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			return -EFAULT;
		}
		vma_start_write(vma);
		nvkvm_zap_range(vma, cbase, clen);
		ret = remap_pfn_range(vma, cbase, (unsigned long)(gpa >> PAGE_SHIFT),
				      clen, vma->vm_page_prot);
		if (!ret)
			nvkvm_force_range_wb(mm, cbase, cbase + clen);
		mmap_write_unlock(mm);
		if (ret) {
			nvkvm_virtio_munmap_on_isolate(isolate_id, token);
			nvkvm_virtio_close_handle_on_isolate(handle, isolate_id);
			return ret;
		}

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
			mmgrab(mm);
			mutex_lock(&ctx->cpu_pages_lock);
			list_add_tail(&cp->list, &ctx->cpu_pages);
			mutex_unlock(&ctx->cpu_pages_lock);
		}   /* else: mapping is live; accept the tracking leak, as above */
	}
	return 0;
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
 *
 * (NVKVM_MIG_CHUNK itself is defined above, ahead of the sharing code that
 * also needs it — see the comment there.)
 */

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
	bool cow_forced_write = false;
	bool is_shmem_obj = false;         /* object-keyed sharing, see below */
	struct inode *reg_inode = NULL;
	unsigned long reg_pgoff_base = 0;
	struct nvkvm_shared_pending *shared_claim = NULL;

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

	/*
	 * Object-keyed sharing (see the comment block above
	 * nvkvm_cpu_pages_share_range()): if every chunk of this range is
	 * already migrated under an earlier registration of the same backing
	 * object, share it instead of relocating a second time.  Decided here,
	 * BEFORE any page is pinned and before the mapcount guard can even be
	 * reached — the share path is for ranges that are NOT being relocated,
	 * so it must never be reachable through that guard.
	 *
	 * nvkvm_shared_resolve() also closes the race two peer processes
	 * hit racing straight out of fork() with no synchronisation between
	 * them (see its comment): it waits out a sibling's in-flight
	 * migration of the same object rather than letting both fall into
	 * the relocate path together, and hands this call a claim to release
	 * once its own attempt (below) is decided.
	 */
	{
		struct inode *sh_inode = NULL;
		unsigned long sh_pgoff = 0;

		if (nvkvm_shared_resolve(mm, start, end, &sh_inode, &sh_pgoff,
					 &shared_claim))
			return nvkvm_cpu_pages_share_range(ctx, start, end, prot,
							   sh_inode, sh_pgoff);
	}

	range_len = end - start;
	npages    = range_len >> PAGE_SHIFT;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages) { ret = -ENOMEM; goto err_unpin; }

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
	/*
	 * IT MUST BE PLAIN PROCESS MEMORY BEFORE WE RETYPE IT.
	 *
	 * The VA comes from userspace (the UVM ioctl at nvkvm_ioctl.c names it),
	 * so find_vma() returns whatever the caller pointed at -- and the next
	 * line converts that VMA to VM_PFNMAP|VM_IO and remaps window pages over
	 * it.  Doing that to a file mapping replaces the caller's view of a FILE
	 * with GPA-window memory while the fs's vm_ops stay installed, and doing
	 * it to hugetlb or to another driver's mapping corrupts state that is not
	 * ours.  The pin loop above does not catch this: get_user_pages_fast()
	 * with FOLL_WRITE succeeds perfectly well on a writable file mapping.
	 *
	 * The blast radius is the caller's own address space, which is why this is
	 * minor -- but the comments below reason about "ordinary user memory" and
	 * nothing checked it.
	 *
	 * What is admitted: MAP_PRIVATE|MAP_ANONYMOUS (no vm_file at all) and the
	 * MAP_SHARED anonymous case, which Linux backs with an internal shmem file
	 * and which the remap_pfn_range note below records as a supported input.
	 * shmem is recognised by its superblock magic rather than by
	 * vma_is_shmem(), which the kernel declares but does not export to modules.
	 * RAMFS_MAGIC is accepted alongside it because that is what backs the same
	 * MAP_SHARED anonymous mapping on a CONFIG_SHMEM=n kernel.  Everything else
	 * -- a real filesystem, hugetlbfs, an existing device mapping -- is refused.
	 */
	if ((vma->vm_flags & (VM_PFNMAP | VM_IO | VM_MIXEDMAP | VM_HUGETLB)) ||
	    (vma->vm_file && !nvkvm_vma_file_is_memory(vma))) {
		mmap_write_unlock(mm);
		pr_warn_ratelimited(
			"nvkvm: refusing to migrate 0x%lx-0x%lx: the VMA is not plain anonymous memory (vm_flags=0x%lx file=%d)\n",
			start, end, (unsigned long)vma->vm_flags,
			!!vma->vm_file);
		ret = -EINVAL; goto err_unpin;
	}

	/*
	 * Captured once, under the same write lock that is about to retype
	 * this VMA, for the registry insert at the end of each chunk below.
	 * NULL/false when this is an ordinary MAP_PRIVATE|MAP_ANONYMOUS range
	 * (nothing to key: it can never legitimately have a second view, so
	 * sharing does not apply and nothing is recorded).  See the
	 * object-keyed sharing comment above nvkvm_cpu_pages_share_range().
	 */
	is_shmem_obj = vma->vm_file && nvkvm_vma_file_is_memory(vma);
	if (is_shmem_obj) {
		reg_inode      = file_inode(vma->vm_file);
		reg_pgoff_base = vma->vm_pgoff +
			((start - vma->vm_start) >> PAGE_SHIFT);
	}
	/*
	 * Under mmap_write_lock, and after the VMA type check, on purpose.
	 *
	 * Audit 2026-08-31: this ran BEFORE the lock was taken, which left a
	 * TOCTOU in the same bug class it exists to close -- a sibling thread
	 * calling fork() between the check and the conversion shares these anon
	 * pages COW, so the count the check saw (1) is stale by the time the
	 * VMA is retyped, and the child is left on the original pages exactly
	 * as in the desync this guards against. dup_mmap() takes
	 * mmap_write_lock on the parent's mm, so holding it here excludes that.
	 *
	 * After the type check as well, so a hugetlb or real-file VMA is
	 * refused with the accurate "not plain anonymous memory" rather than a
	 * misleading "shared with another mapping".
	 */
	/*
	 * THE PAGES MUST NOT ALREADY BE MAPPED BY ANYONE ELSE.
	 *
	 * Everything below relocates this range: the data is copied into a host
	 * memfd and the VMA is repointed at a GPA window.  That rewrites THIS
	 * mm's view and nothing else.  Any second view keeps the original pages
	 * and silently stops sharing with this one -- measured total divergence,
	 * with every ioctl returning success, in
	 * docs/investigations/shared-mapping-desync/.
	 *
	 * Ways in, all ordinary rather than exotic:
	 *   - MAP_SHARED|MAP_ANONYMOUS inherited across fork()
	 *   - a memfd deliberately shared between cooperating processes
	 *   - a VMM's guest-RAM memfd aliased into a KVM memslot (nesting)
	 *
	 * The earlier vm_file/s_magic/S_PRIVATE tests all ask who CREATED the
	 * object.  That is the wrong question and missed the fork case entirely
	 * (shmem_zero_setup() sets S_PRIVATE, so MAP_SHARED|MAP_ANONYMOUS
	 * passed).  The question is how many mappings the pages already have,
	 * and page_mapcount() answers it directly for every case above.
	 *
	 * We hold a pin on each page here, which does not itself raise mapcount
	 * -- so 1 means "this VMA only" and >1 means someone else is mapping it.
	 *
	 * This REFUSES rather than repairing.  Repairing would mean migrating
	 * every other view onto the new pages, which needs an i_mmap walk plus
	 * mmap_write_lock on foreign mms -- the kernel's lock order is
	 * mmap_lock outer, i_mmap_rwsem inner, so doing it in that direction is
	 * an ABBA inversion, and rmap_walk() is not exported to modules.  Until
	 * that is solved, an honest -EINVAL beats silent corruption.
	 */
	for (i = 0; i < (unsigned long)got; i++) {
		int mc = page_mapcount(pages[i]);

		if (mc > 1) {
			pr_warn_ratelimited(
				"nvkvm: refusing to migrate 0x%lx-0x%lx: page %lu is mapped %d times -- the range is shared with another mapping, and relocating it would desynchronise them (see docs/investigations/shared-mapping-desync)\n",
				start, end, i, mc);
			/* Under mmap_write_lock now -- every sibling error
			 * path in this section unlocks before unwinding, and
			 * err_unpin does not. */
			mmap_write_unlock(mm);
			ret = -EINVAL;
			goto err_unpin;
		}
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
	/*
	 * Make it a SHARED passthrough rather than stripping VM_MAYWRITE.
	 *
	 * remap_pfn_range() refuses a sub-VMA remap on a COW mapping, and
	 * is_cow_mapping() is (VM_SHARED|VM_MAYWRITE) == VM_MAYWRITE. Clearing
	 * VM_MAYWRITE satisfied that test but left the VMA private-and-
	 * unwritable: vm_get_page_prot() maps (VM_WRITE, !VM_SHARED) to a
	 * read-only protection -- that is how COW is expressed -- so the PTEs
	 * installed below came out read-only, and with VM_MAYWRITE gone the
	 * resulting write fault had no way to resolve.
	 *
	 * MEASURED: a MAP_PRIVATE buffer registered fine and then SIGSEGV'd on
	 * the first write, while the same binary on the stock driver wrote it
	 * happily (tests/repro/private_register_write.c). That is ordinary
	 * malloc'd heap, and the 30-check suite missed it because every CUDA
	 * check registers cuMemHostAlloc memory, which is MAP_SHARED.
	 *
	 * Setting VM_SHARED makes is_cow_mapping() false for the same reason,
	 * so remap_pfn_range() is satisfied -- and it is the honest description
	 * of what this VMA now is. After the conversion it is a straight
	 * passthrough of host window pages: there is no COW left to perform, and
	 * a fork must share the window rather than try to copy it.
	 *
	 * VM_MAYSHARE is set with it because mmap() always pairs the two
	 * (_calc_vm_trans sets both for MAP_SHARED); VM_SHARED alone is a flag
	 * combination the kernel never produces on its own. Nothing asserts the
	 * invariant today, so this changes no behaviour -- it avoids leaving a
	 * VMA in a shape future mm code may reasonably assume cannot exist.
	 */
	if (is_cow_mapping(vma->vm_flags)) {
		/*
		 * Two constraints that look contradictory, satisfied separately.
		 *
		 * remap_pfn_range() refuses a sub-VMA remap on a COW mapping
		 * (is_cow_mapping() is (VM_SHARED|VM_MAYWRITE) == VM_MAYWRITE),
		 * so VM_MAYWRITE has to go. But vm_get_page_prot() encodes COW by
		 * mapping (VM_WRITE, !VM_SHARED) to a READ-ONLY protection, so
		 * deriving the protection from the flags afterwards gives
		 * read-only PTEs and the process SIGSEGVs on its first write.
		 *
		 * Setting VM_SHARED fixed the write and broke something else:
		 * measured, it makes a MAP_PRIVATE buffer genuinely shared, so
		 * after fork the parent sees the child's writes (16384/16384)
		 * where the stock driver keeps them private (0/16384).
		 * tests/repro/fork_mapping_semantics.c is that measurement.
		 *
		 * The COW is already resolved before we reach here:
		 * get_user_pages_fast(FOLL_WRITE) above breaks it and the KERNEL
		 * does the copy, which is the right place for it. By this point
		 * the pages are exclusively ours, and after the retype the VMA is
		 * VM_PFNMAP over device memory, where COW has no meaning -- so
		 * the read-only encoding describes nothing real.
		 *
		 * Clear VM_MAYWRITE for remap_pfn_range; set the protection
		 * writable EXPLICITLY below rather than deriving it. The VMA
		 * stays private.
		 */
		vm_flags_clear(vma, VM_MAYWRITE);
		cow_forced_write = true;
	}
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
	/* Derive normally, except in the COW case above where the flags no
	 * longer describe the mapping: ask for the protection a SHARED
	 * writable mapping would get, without marking the VMA shared. */
	vma->vm_page_prot = vm_get_page_prot(cow_forced_write
					     ? (vma->vm_flags | VM_SHARED)
					     : vma->vm_flags);
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

		/* Object-keyed sharing: make this chunk findable by a sibling
		 * process registering the same backing object later.  See the
		 * comment block above nvkvm_cpu_pages_share_range(). */
		if (is_shmem_obj)
			nvkvm_shared_add(reg_inode,
					 reg_pgoff_base + (coff >> PAGE_SHIFT),
					 clen, handle);

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
	/* Object-keyed sharing: our attempt succeeded — wake anyone waiting
	 * on this object so they re-check the (now populated) registry. */
	if (shared_claim)
		nvkvm_shared_claim_release(shared_claim);
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
	/* Object-keyed sharing: our attempt failed — release the claim (the
	 * registry has nothing for this object) so a waiting sibling becomes
	 * the new claimant and tries the migration itself, rather than
	 * waiting forever on a claim that will never resolve. */
	if (shared_claim)
		nvkvm_shared_claim_release(shared_claim);
	return ret;
}
