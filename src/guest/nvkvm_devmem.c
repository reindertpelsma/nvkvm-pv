// SPDX-License-Identifier: GPL-2.0
/*
 * nvkvm_devmem.c — SPIKE (branch spike/dev-nvkvm-mem): /dev/nvkvm-mem.
 *
 * QUESTION THIS ANSWERS: can the guest module hand out memory that is
 * host-visible FROM BIRTH, such that two guest processes mapping the same fd
 * share it coherently — with NO relocation of any existing view involved?
 *
 * Every memory bug in this driver so far came from *migrating* pages that
 * already had a view: nvkvm_cpu_pages_migrate_range() (nvkvm_mmap.c) copies
 * an existing anon VMA's data into a host memfd and repoints ONE VMA at it,
 * and any other mapping of those original pages is left behind, silently
 * diverging (see docs/investigations/shared-mapping-desync/). This device
 * sidesteps that whole class by never having a "before" to leave behind: the
 * backing memory is a host memfd, and mmap() just remap_pfn_range()s a fresh
 * VMA onto it. There is nothing to migrate.
 *
 * This reuses the exact plumbing nvkvm_cpu_pages_migrate_range() uses to get
 * a GPA for a memory handle — read that function first, it is the template:
 *   nvkvm_virtio_open_memory_handle()   — mint a host memfd, get a handle id
 *   nvkvm_virtio_copy_handle_to_isolate() — let an isolate reference it
 *   nvkvm_virtio_mmap_on_isolate()      — QEMU mmaps the SAME fd itself and
 *                                          publishes it at a GPA inside the
 *                                          128 GiB sparse mmap window
 *   remap_pfn_range()                   — map that GPA into the caller's VMA
 *
 * DESIGN, per owner correction (this file originally minted the handle and
 * installed it in open(), at gva=0 "bookkeeping only" — that broke the
 * host's A-1 OS_DESCRIPTOR check, see below):
 *
 *   open()    — cheap: get/create this process's session, ensure its
 *               isolate. No handle, no host memfd yet — exactly like opening
 *               a regular /dev/nvidia* file.
 *   ioctl(NVKVM_MEM_IOC_SET_SIZE) — the ftruncate(2)-equivalent (see the
 *               comment on that ioctl for why it is an ioctl and not
 *               literal ftruncate()). Mints the host handle at the
 *               requested size. Optional: the first mmap() defaults to
 *               NVKVM_DEVMEM_DEFAULT_SIZE if this was never called.
 *   mmap()    — THIS is where the guest VA is finally known
 *               (vma->vm_start), and it is what fixes the A-1 denial:
 *               install the handle in the MAPPING PROCESS's OWN isolate at
 *               that real VA (nvkvm_virtio_mmap_on_isolate(gva=vma->vm_start)
 *               — the same call the migrate path makes per chunk), so the
 *               host's iso_mmap_translate() table genuinely has this range
 *               on record instead of us trying to route around the check
 *               that consults it.
 *
 * SHARING is the point of this device, not an edge case: the fd may be
 * mapped by a DIFFERENT process than the one that opened/sized it (fork,
 * SCM_RIGHTS), and each mmap() independently installs the SAME handle into
 * ITS OWN mapper's isolate at ITS OWN VA — struct nvkvm_devmem_mapping is
 * per-VMA for exactly this reason (two mappings of the same handle can, and
 * for cross-process sharing DO, belong to two different isolates). Teardown
 * is per-mapping too: nvkvm_devmem_vma_close() releases exactly the one
 * mapping whose VMA is going away, independent of the fd's own lifetime and
 * of any other live mapping of the same handle.
 *
 * SCOPE, DELIBERATELY MINIMAL (this is a spike, not a feature):
 *   - Only offset 0 is supported (vm_pgoff must be 0).
 *   - A handle's size is fixed once minted; no re-truncate to grow/shrink an
 *     already-sized fd.
 *   - No refcounting/lifetime design beyond what nvkvm_session already does.
 *     A failure partway through open()/mmap() can leak the handle/isolate
 *     mirror; this is accepted for the spike rather than built out.
 *   - A mapper's COPY_HANDLE_TO_ISOLATE registration is never explicitly
 *     withdrawn (see the release() comment) — there is no ownership-checked
 *     host request to do that for a foreign isolate today. That is the same
 *     gap the cross-session grant/refcount rework on feat/handle-lifetime
 *     exists to close properly; not attempted here.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#include "nvkvm.h"

/* Backward-compat default: a plain open()+mmap() with no SET_SIZE call gets
 * the same 2 MiB window earlier versions of this device always handed out. */
#define NVKVM_DEVMEM_DEFAULT_SIZE (2UL << 20)
/* Spike sanity cap — bound how much host memfd a guest process can mint
 * through this device.  Not derived from anything structural, unlike
 * NVKVM_MIG_MAX_RANGE's derivation in nvkvm_mmap.c; just a guard against an
 * unbounded ask while this is a spike. */
#define NVKVM_DEVMEM_MAX_SIZE      (64UL << 20)

/*
 * The ftruncate(2)-equivalent, as an ioctl rather than literal ftruncate().
 *
 * ftruncate() cannot reach this driver no matter what we implement: this is
 * a misc/char device (S_ISCHR), and do_sys_ftruncate() refuses any inode
 * that is not S_ISREG at the VFS layer, before ->setattr or any
 * driver-specific code ever runs. A memfd created via memfd_create() is a
 * regular (shmem-backed) file, which is why ftruncate() works on THAT — but
 * /dev/nvkvm-mem is a device node whose mmap() happens to be backed by one,
 * which is a different thing to the VFS. This ioctl is the reachable
 * equivalent: same effect (size the backing memfd before mapping it),
 * different syscall, because the device type makes the POSIX one physically
 * inapplicable rather than merely unimplemented.
 */
#define NVKVM_MEM_IOC_MAGIC     'M'
#define NVKVM_MEM_IOC_SET_SIZE  _IOW(NVKVM_MEM_IOC_MAGIC, 1, __u64)

struct nvkvm_devmem_ctx {
	struct nvkvm_session *session;  /* the OPENER's session -- owns/mints
					  * the handle. Fixed at open() and
					  * never reassigned even if the fd is
					  * later shared (fork/SCM_RIGHTS): the
					  * handle's host-side session_id is
					  * fixed at OPEN_MEMORY_HANDLE time,
					  * so this mirrors that. */
	struct mutex lock;               /* serializes SET_SIZE / the
					  * mmap()-time default-size fallback
					  * against each other on this fd. */
	__u64  size;                     /* 0 until sized */
	__u32  handle_id;                /* valid once size != 0 */
};

/*
 * One per live mmap() of a devmem fd. NOT shared across mappings: two
 * mmap()s of the same fd -- from the same process, from two threads, or
 * (the real use case) from two different processes sharing the fd -- each
 * install the SAME handle into a possibly DIFFERENT isolate at a possibly
 * DIFFERENT VA, and each is torn down independently when ITS VMA goes away.
 */
struct nvkvm_devmem_mapping {
	struct nvkvm_devmem_ctx *dctx;            /* the owning fd's ctx --
						    * gives us handle_id */
	struct nvkvm_session    *mapper_session;  /* THIS mapping's own
						    * session (== dctx->session
						    * unless the fd was shared
						    * with a different
						    * process); a reference is
						    * held from mmap() to
						    * vma_close(). */
	__u32  isolate_id;
	__u32  mmap_token;
	__u64  gpa;
};

static void nvkvm_devmem_vma_close(struct vm_area_struct *vma);

static const struct vm_operations_struct nvkvm_devmem_vm_ops = {
	.close = nvkvm_devmem_vma_close,
};

static int nvkvm_devmem_open(struct inode *inode, struct file *filp)
{
	struct nvkvm_devmem_ctx *dctx;
	int ret;

	if (!nvkvm_transport_ready(&nvkvm))
		return -ENODEV;

	dctx = kzalloc(sizeof(*dctx), GFP_KERNEL);
	if (!dctx)
		return -ENOMEM;
	mutex_init(&dctx->lock);

	dctx->session = nvkvm_session_get_or_create(current->mm, current->tgid);
	if (IS_ERR(dctx->session)) {
		ret = PTR_ERR(dctx->session);
		dctx->session = NULL;
		goto err_free;
	}

	ret = nvkvm_ensure_isolate(dctx->session);
	if (ret)
		goto err_session;

	filp->private_data = dctx;
	pr_info("nvkvm-mem: open pid=%d tgid=%d session=%u isolate=%u\n",
		task_pid_nr(current), current->tgid, dctx->session->id,
		dctx->session->isolate_id);
	return 0;

err_session:
	nvkvm_session_put(dctx->session);
err_free:
	kfree(dctx);
	return ret;
}

/* Caller holds dctx->lock. Mints the handle exactly once; a second call
 * (SET_SIZE called twice, or SET_SIZE followed by mmap()'s own default-size
 * fallback) is a no-op if the size matches and -EBUSY if it does not --
 * this device does not support re-truncating an already-sized fd. */
static int nvkvm_devmem_ensure_handle(struct nvkvm_devmem_ctx *dctx, __u64 size)
{
	__u32 handle_id;
	int ret;

	if (dctx->handle_id)
		return (dctx->size == size) ? 0 : -EBUSY;

	ret = nvkvm_virtio_open_memory_handle(dctx->session->id, size,
					      &handle_id);
	if (ret) {
		pr_warn("nvkvm-mem: open_memory_handle(size=%llu) failed: %d\n",
			(unsigned long long)size, ret);
		return ret;
	}

	dctx->handle_id = handle_id;
	dctx->size      = size;
	return 0;
}

static long nvkvm_devmem_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	struct nvkvm_devmem_ctx *dctx = filp->private_data;
	int ret;

	if (!dctx)
		return -ENODEV;

	switch (cmd) {
	case NVKVM_MEM_IOC_SET_SIZE: {
		__u64 size;

		if (copy_from_user(&size, (void __user *)arg, sizeof(size)))
			return -EFAULT;
		if (!size || (size & (PAGE_SIZE - 1)) ||
		    size > NVKVM_DEVMEM_MAX_SIZE)
			return -EINVAL;

		mutex_lock(&dctx->lock);
		ret = nvkvm_devmem_ensure_handle(dctx, size);
		mutex_unlock(&dctx->lock);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static int nvkvm_devmem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nvkvm_devmem_ctx *dctx = filp->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;
	struct nvkvm_devmem_mapping *m;
	struct nvkvm_session *mapper;
	__u32 isolate_id;
	__u64 gpa = 0;
	__u32 token = 0;
	int ret;

	if (!dctx)
		return -ENODEV;
	if (vma->vm_pgoff != 0)
		return -EINVAL;          /* offset support is out of scope */

	mutex_lock(&dctx->lock);
	if (!dctx->handle_id) {
		/* Never explicitly sized -- default, preserving plain
		 * open()+mmap() behaviour from before SET_SIZE existed. */
		ret = nvkvm_devmem_ensure_handle(dctx, NVKVM_DEVMEM_DEFAULT_SIZE);
		if (ret) {
			mutex_unlock(&dctx->lock);
			return ret;
		}
	}
	if (len > dctx->size) {
		mutex_unlock(&dctx->lock);
		return -EINVAL;
	}
	mutex_unlock(&dctx->lock);

	/*
	 * SHARING: this fd may be mapped from a DIFFERENT process than the one
	 * that opened/sized it (fork, SCM_RIGHTS) -- that is this device's
	 * whole point. The mapping is installed into the CALLING process's OWN
	 * session/isolate, not dctx->session (the opener's): for the common
	 * case (a process mapping its own fd) the two are the same object; for
	 * a shared fd they are not, and it is the MAPPER's isolate that must
	 * receive the installation below, at the mapper's own VA.
	 */
	mapper = nvkvm_session_get_or_create(current->mm, current->tgid);
	if (IS_ERR(mapper))
		return PTR_ERR(mapper);
	ret = nvkvm_ensure_isolate(mapper);
	if (ret) {
		nvkvm_session_put(mapper);
		return ret;
	}
	isolate_id = mapper->isolate_id;

	/*
	 * Make the handle referenceable from the mapper's isolate.
	 * COPY_HANDLE_TO_ISOLATE carries no ownership check by design on the
	 * host side (nvkvm_req_copy_handle_to_isolate(), nvkvm_isolate_
	 * handlers.c) -- it IS the mechanism CUDA IPC itself uses to legally
	 * share a handle across isolates, so nothing here is bypassing
	 * anything. Safe to call again for an isolate that already has it.
	 */
	ret = nvkvm_virtio_copy_handle_to_isolate(dctx->handle_id, isolate_id);
	if (ret) {
		pr_warn("nvkvm-mem: copy_handle_to_isolate(handle=%u isolate=%u) failed: %d\n",
			dctx->handle_id, isolate_id, ret);
		nvkvm_session_put(mapper);
		return ret;
	}

	/*
	 * TASK 1 fix (A-1, dev-nvkvm-mem spike): install the handle in the
	 * MAPPER's isolate at the GUEST'S REAL VA (vma->vm_start) -- the exact
	 * same call the migrate path makes per chunk (nvkvm_cpu_pages_
	 * migrate_range(), nvkvm_mmap.c), with the VA that only exists now, at
	 * mmap() time (open()/SET_SIZE run before userspace has chosen where
	 * to map). This is what populates the host's iso_mmap_translate()
	 * table, which is what the A-1 check in nvkvm_req_mmap_on_isolate()
	 * (the OS_DESCRIPTOR / cuMemHostRegister path) verifies before
	 * forwarding a registration for this range. This SATISFIES that
	 * invariant -- the range genuinely becomes host-installed -- rather
	 * than routing around the check.
	 */
	ret = nvkvm_virtio_mmap_on_isolate(isolate_id, dctx->handle_id,
					   vma->vm_start, /*offset=*/0, len,
					   PROT_READ | PROT_WRITE, MAP_SHARED,
					   mapper->id, &gpa, &token);
	if (ret) {
		pr_warn("nvkvm-mem: mmap_on_isolate(handle=%u isolate=%u session=%u va=0x%lx) failed: %d\n",
			dctx->handle_id, isolate_id, mapper->id,
			vma->vm_start, ret);
		nvkvm_session_put(mapper);
		return ret;
	}
	if (!nvkvm_gpa_in_mmap_window(gpa, len)) {
		pr_warn("nvkvm-mem: host returned gpa=0x%llx outside the mmap window — refusing\n",
			gpa);
		nvkvm_virtio_munmap_on_isolate(isolate_id, token);
		nvkvm_session_put(mapper);
		return -EIO;
	}

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		nvkvm_virtio_munmap_on_isolate(isolate_id, token);
		nvkvm_session_put(mapper);
		return -ENOMEM;
	}
	m->dctx           = dctx;
	m->mapper_session = mapper;    /* reference transferred; dropped in
					 * nvkvm_devmem_vma_close() */
	m->isolate_id     = isolate_id;
	m->mmap_token     = token;
	m->gpa            = gpa;

	/*
	 * Same shape as nvkvm_cpu_pages_migrate_range()'s chunk remap
	 * (nvkvm_mmap.c), minus everything that function does to retype an
	 * EXISTING anon VMA with live pages: this VMA is brand new (mmap()
	 * f_ops run before any page has ever been faulted into it), so there
	 * is nothing to zap and nothing to pin/unpin.
	 */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP |
			 VM_SHARED | VM_MAYSHARE);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_ops = &nvkvm_devmem_vm_ops;
	vma->vm_private_data = m;

	ret = remap_pfn_range(vma, vma->vm_start,
			      (unsigned long)(gpa >> PAGE_SHIFT), len,
			      vma->vm_page_prot);
	if (ret) {
		vma->vm_private_data = NULL;
		nvkvm_virtio_munmap_on_isolate(isolate_id, token);
		nvkvm_session_put(mapper);
		kfree(m);
		return ret;
	}

	/*
	 * The GPA window is a PCI-BAR (non-RAM) region from the guest's point
	 * of view, so remap_pfn_range() silently downgrades the PTEs to UC-.
	 * The pages are genuine host RAM (a memfd) and cache-coherent; rewrite
	 * to WB exactly as the migrate path does.  f_op->mmap runs with
	 * mm->mmap_lock held for write already (mmap_region()), which is what
	 * nvkvm_force_range_wb() requires from its caller.
	 */
	nvkvm_force_range_wb(vma->vm_mm, vma->vm_start, vma->vm_end);

	pr_info("nvkvm-mem: mmap pid=%d tgid=%d handle=%u mapper_session=%u isolate=%u gpa=0x%llx -> va=0x%lx len=0x%lx\n",
		task_pid_nr(current), current->tgid, dctx->handle_id,
		mapper->id, isolate_id, gpa, vma->vm_start, len);
	return 0;
}

/* Per-mapping teardown -- runs whenever THIS VMA goes away (munmap(), the
 * mapping process exiting, ...), independent of the fd's own lifetime and of
 * any other live mapping of the same handle in a different isolate. */
static void nvkvm_devmem_vma_close(struct vm_area_struct *vma)
{
	struct nvkvm_devmem_mapping *m = vma->vm_private_data;

	if (!m)
		return;

	nvkvm_virtio_munmap_on_isolate(m->isolate_id, m->mmap_token);
	pr_info("nvkvm-mem: munmap handle=%u mapper_session=%u isolate=%u gpa=0x%llx\n",
		m->dctx ? m->dctx->handle_id : 0,
		m->mapper_session ? m->mapper_session->id : 0,
		m->isolate_id, m->gpa);

	nvkvm_session_put(m->mapper_session);
	vma->vm_private_data = NULL;
	kfree(m);
}

static int nvkvm_devmem_release(struct inode *inode, struct file *filp)
{
	struct nvkvm_devmem_ctx *dctx = filp->private_data;

	if (!dctx)
		return 0;

	pr_info("nvkvm-mem: release pid=%d tgid=%d handle=%u\n",
		task_pid_nr(current), current->tgid, dctx->handle_id);

	/*
	 * Live mappings tear themselves down via nvkvm_devmem_vma_close() as
	 * their own VMAs go away -- independently, possibly in OTHER
	 * processes this fd was shared with -- so there is nothing to do for
	 * them here.
	 *
	 * KNOWN LIMITATION (spike, not fixed here): this only withdraws the
	 * OPENER's own isolate registration for the handle
	 * (CLOSE_HANDLE_ON_ISOLATE, session-checked, which the opener's own
	 * isolate always satisfies) and then closes the handle outright. A
	 * mapper process's COPY_HANDLE_TO_ISOLATE registration in ITS isolate
	 * is never explicitly withdrawn -- nvkvm_req_close_handle_on_isolate()
	 * gates on session_has_isolate(h->session_id, isolate_id), which a
	 * foreign mapper's isolate does not satisfy, so there is no
	 * ownership-checked host request today that could do it. If the
	 * opener's fd (and so the handle) outlives every mapper's use of it,
	 * nothing observes this. If it does not, closing the handle here
	 * while a foreign isolate still references it either fails softly
	 * (host-side nvkvm_handle_close() refuses with -EBUSY while
	 * isolate_refcount > 0, silently ignored here since this path cannot
	 * propagate an error) or, if the opener's whole SESSION is torn down
	 * (nvkvm_handle_close_session(), which force-closes ignoring
	 * isolate_refcount -- a separate, pre-existing inconsistency with
	 * nvkvm_handle_close()'s own guard), pulls the fd out from under a
	 * still-live foreign mapping. This is the same gap the cross-session
	 * grant/refcount rework on feat/handle-lifetime exists to close
	 * properly; not attempted here.
	 */
	if (dctx->handle_id) {
		if (dctx->session && dctx->session->isolate_id)
			nvkvm_virtio_close_handle_on_isolate(dctx->handle_id,
							     dctx->session->isolate_id);
		nvkvm_virtio_close_handle(dctx->handle_id);
	}
	if (dctx->session)
		nvkvm_session_put(dctx->session);

	kfree(dctx);
	filp->private_data = NULL;
	return 0;
}

static const struct file_operations nvkvm_devmem_fops = {
	.owner          = THIS_MODULE,
	.open           = nvkvm_devmem_open,
	.release        = nvkvm_devmem_release,
	.mmap           = nvkvm_devmem_mmap,
	.unlocked_ioctl = nvkvm_devmem_ioctl,
};

/*
 * nvkvm_devmem_vma_lookup — TASK 1 (pass-through registration): recognise a
 * VMA as one of OUR /dev/nvkvm-mem mappings and hand back the handle/GPA/
 * isolate that already backs it, so a caller can reuse it instead of
 * migrating.  Used from nvkvm_sanitize_ioctl_params()'s NV_ESC_RM_ALLOC_MEMORY
 * handling (nvkvm_ioctl.c) to skip nvkvm_cpu_pages_migrate_range() ENTIRELY
 * for a devmem-backed OS_DESCRIPTOR registration -- deliberately called from
 * the caller of migrate_range(), not from inside it, so migrate_range() and
 * its VM_PFNMAP/page_mapcount() guards stay byte-for-byte unchanged.
 *
 * Identification is by struct file ->f_op pointer identity: nvkvm_devmem_fops
 * is static to this file, so `vma->vm_file->f_op == &nvkvm_devmem_fops` can
 * only be true for a file opened through THIS device's ->open. The isolate_id
 * handed back is the ONE THIS VMA WAS INSTALLED INTO (vma->vm_private_data's
 * m->isolate_id) -- the mapper's own isolate, which for a shared fd is not
 * necessarily dctx->session's (the opener's).
 *
 * Returns false, and writes nothing to the out-params, for every VMA that is
 * not ours, or is ours but has no live installed mapping (shouldn't happen
 * once ->mmap has returned 0, but checked rather than assumed).
 */
bool nvkvm_devmem_vma_lookup(struct vm_area_struct *vma, __u32 *handle_id,
			     __u64 *gpa, __u32 *isolate_id)
{
	struct nvkvm_devmem_mapping *m;

	if (!vma || !vma->vm_file || vma->vm_file->f_op != &nvkvm_devmem_fops)
		return false;
	if (vma->vm_ops != &nvkvm_devmem_vm_ops || !vma->vm_private_data)
		return false;

	m = vma->vm_private_data;
	if (!m->dctx)
		return false;

	if (handle_id)
		*handle_id = m->dctx->handle_id;
	if (gpa)
		*gpa = m->gpa;
	if (isolate_id)
		*isolate_id = m->isolate_id;
	return true;
}

static struct miscdevice nvkvm_devmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "nvkvm-mem",
	.fops  = &nvkvm_devmem_fops,
	.mode  = 0666,
};

int __init nvkvm_devmem_init(void)
{
	int ret = misc_register(&nvkvm_devmem_misc);

	if (ret)
		return ret;
	pr_info("nvkvm-mem: SPIKE device registered (/dev/nvkvm-mem, default %lu byte window, NVKVM_MEM_IOC_SET_SIZE up to %lu)\n",
		NVKVM_DEVMEM_DEFAULT_SIZE, NVKVM_DEVMEM_MAX_SIZE);
	return 0;
}

void nvkvm_devmem_exit(void)
{
	misc_deregister(&nvkvm_devmem_misc);
}
