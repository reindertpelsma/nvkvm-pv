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
 * backing memory is a host memfd from the moment open() returns, and mmap()
 * just remap_pfn_range()s a fresh VMA onto it. There is nothing to migrate.
 *
 * This reuses the exact plumbing nvkvm_cpu_pages_migrate_range() uses to get
 * a GPA for a memory handle — read that function first, it is the template:
 *   nvkvm_virtio_open_memory_handle()   — mint a host memfd, get a handle id
 *   nvkvm_virtio_copy_handle_to_isolate() — let this session's isolate see it
 *   nvkvm_virtio_mmap_on_isolate()      — QEMU mmaps the SAME fd itself and
 *                                          publishes it at a GPA inside the
 *                                          128 GiB sparse mmap window
 *   remap_pfn_range()                   — map that GPA into the caller's VMA
 *
 * SCOPE, DELIBERATELY MINIMAL (this is a spike, not a feature):
 *   - Fixed NVKVM_DEVMEM_SIZE (2 MiB) window. No ftruncate/resize.
 *   - The memory handle + GPA are obtained ONCE, in open(). Every mmap() on
 *     that struct file (including from a fork()'d process sharing the same
 *     fd) maps the SAME GPA — that is the entire point of the experiment.
 *   - Only offset 0 is supported (vm_pgoff must be 0).
 *   - No refcounting/lifetime design beyond what nvkvm_session already does.
 *     A failure partway through open() can leak the handle/isolate mirror;
 *     this is accepted for the spike (see nvkvm_devmem_open()'s error path
 *     comment) rather than built out.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/err.h>

#include "nvkvm.h"

/* Fixed size for the spike — see file header. Must be page-aligned. */
#define NVKVM_DEVMEM_SIZE (2UL << 20)

struct nvkvm_devmem_ctx {
	struct nvkvm_session *session;
	__u32 handle_id;     /* memory handle, from OPEN_MEMORY_HANDLE */
	__u32 mmap_token;    /* from MMAP_ON_ISOLATE, needed to munmap it */
	__u64 gpa;           /* GPA of the window inside the guest's mmap
			      * window (nvkvm_gpa_in_mmap_window()) */
	bool  ready;          /* gpa/token valid, safe to remap_pfn_range */
};

static int nvkvm_devmem_open(struct inode *inode, struct file *filp)
{
	struct nvkvm_devmem_ctx *dctx;
	__u32 isolate_id;
	__u64 gpa = 0;
	__u32 token = 0;
	int ret;

	if (!nvkvm_transport_ready(&nvkvm))
		return -ENODEV;

	dctx = kzalloc(sizeof(*dctx), GFP_KERNEL);
	if (!dctx)
		return -ENOMEM;

	dctx->session = nvkvm_session_get_or_create(current->mm, current->tgid);
	if (IS_ERR(dctx->session)) {
		ret = PTR_ERR(dctx->session);
		dctx->session = NULL;
		goto err_free;
	}

	ret = nvkvm_ensure_isolate(dctx->session);
	if (ret)
		goto err_session;
	isolate_id = dctx->session->isolate_id;

	ret = nvkvm_virtio_open_memory_handle(dctx->session->id,
					      NVKVM_DEVMEM_SIZE,
					      &dctx->handle_id);
	if (ret) {
		pr_warn("nvkvm-mem: open_memory_handle failed: %d\n", ret);
		goto err_session;
	}

	ret = nvkvm_virtio_copy_handle_to_isolate(dctx->handle_id, isolate_id);
	if (ret) {
		pr_warn("nvkvm-mem: copy_handle_to_isolate failed: %d\n", ret);
		/* SPIKE: leak the handle on this path rather than build unwind
		 * plumbing — stated in the file header. */
		goto err_session;
	}

	/* gva is bookkeeping only (identity/adjacency key on the host side,
	 * U-9 fix) — it is NOT used to place anything, so 0 is fine here;
	 * there is no real guest VA yet, mmap() hasn't been called. */
	ret = nvkvm_virtio_mmap_on_isolate(isolate_id, dctx->handle_id,
					   /*gva=*/0, /*offset=*/0,
					   NVKVM_DEVMEM_SIZE,
					   PROT_READ | PROT_WRITE, MAP_SHARED,
					   dctx->session->id, &gpa, &token);
	if (ret) {
		pr_warn("nvkvm-mem: mmap_on_isolate failed: %d\n", ret);
		goto err_session;
	}

	if (!nvkvm_gpa_in_mmap_window(gpa, NVKVM_DEVMEM_SIZE)) {
		pr_warn("nvkvm-mem: host returned gpa=0x%llx outside the mmap window — refusing\n",
			gpa);
		nvkvm_virtio_munmap_on_isolate(isolate_id, token);
		ret = -EIO;
		goto err_session;
	}

	dctx->gpa        = gpa;
	dctx->mmap_token = token;
	dctx->ready      = true;

	filp->private_data = dctx;
	pr_info("nvkvm-mem: open pid=%d tgid=%d handle=%u session=%u isolate=%u gpa=0x%llx\n",
		task_pid_nr(current), current->tgid, dctx->handle_id,
		dctx->session->id, isolate_id, gpa);
	return 0;

err_session:
	nvkvm_session_put(dctx->session);
err_free:
	kfree(dctx);
	return ret;
}

static int nvkvm_devmem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nvkvm_devmem_ctx *dctx = filp->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;

	if (!dctx || !dctx->ready)
		return -ENODEV;
	if (vma->vm_pgoff != 0)
		return -EINVAL;          /* offset support is out of scope */
	if (len > NVKVM_DEVMEM_SIZE)
		return -EINVAL;

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

	{
		int ret = remap_pfn_range(vma, vma->vm_start,
					  (unsigned long)(dctx->gpa >> PAGE_SHIFT),
					  len, vma->vm_page_prot);
		if (ret)
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

	pr_info("nvkvm-mem: mmap pid=%d tgid=%d gpa=0x%llx -> va=0x%lx len=0x%lx\n",
		task_pid_nr(current), current->tgid, dctx->gpa, vma->vm_start,
		len);
	return 0;
}

static int nvkvm_devmem_release(struct inode *inode, struct file *filp)
{
	struct nvkvm_devmem_ctx *dctx = filp->private_data;

	if (!dctx)
		return 0;

	pr_info("nvkvm-mem: release pid=%d tgid=%d handle=%u\n",
		task_pid_nr(current), current->tgid, dctx->handle_id);

	if (dctx->ready && dctx->session && dctx->session->isolate_id) {
		nvkvm_virtio_munmap_on_isolate(dctx->session->isolate_id,
					       dctx->mmap_token);
		nvkvm_virtio_close_handle_on_isolate(dctx->handle_id,
						     dctx->session->isolate_id);
	}
	if (dctx->handle_id)
		nvkvm_virtio_close_handle(dctx->handle_id);
	if (dctx->session)
		nvkvm_session_put(dctx->session);

	kfree(dctx);
	filp->private_data = NULL;
	return 0;
}

static const struct file_operations nvkvm_devmem_fops = {
	.owner   = THIS_MODULE,
	.open    = nvkvm_devmem_open,
	.release = nvkvm_devmem_release,
	.mmap    = nvkvm_devmem_mmap,
};

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
	pr_info("nvkvm-mem: SPIKE device registered (/dev/nvkvm-mem, %lu byte window)\n",
		NVKVM_DEVMEM_SIZE);
	return 0;
}

void nvkvm_devmem_exit(void)
{
	misc_deregister(&nvkvm_devmem_misc);
}
