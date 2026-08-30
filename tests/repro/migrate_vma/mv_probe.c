// SPDX-License-Identifier: GPL-2.0
/*
 * mv_probe.c -- will migrate_vma_setup() accept the memory libcuda hands us?
 *
 * migrate_vma is the right mechanism for all-views coherence: migrate_vma_unmap()
 * goes through try_to_migrate(), an rmap walk that replaces the PTE in EVERY
 * mapping with a migration entry, and remove_migration_ptes() restores them all
 * to the new page. Nothing has to be hand-tracked.
 *
 * The doubt is the source, not the destination. cuMemHostAlloc returns
 * MAP_SHARED|MAP_ANONYMOUS memory, which Linux backs with an internal shmem
 * file -- so the VMA has vm_file and shmem_vm_ops, and vma_is_anonymous() is
 * FALSE for it. migrate_vma has historically wanted anonymous VMAs.
 *
 * This reports what the VMA actually is and what setup() returns, for both a
 * MAP_PRIVATE|MAP_ANONYMOUS range and a MAP_SHARED|MAP_ANONYMOUS one.
 *
 * Aborts the migration immediately (all dst entries left 0), which is the
 * documented way to back out: pages are restored by finalize.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/migrate.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/sched/mm.h>
#include <linux/memremap.h>

/*
 * A private pgmap for the destination test, deliberately high in the 64 GiB
 * window: nvkvm allocates its GPAs from the base upward, and claiming a range
 * it is using breaks CUDA outright (measured -- cuCtxCreate failed until the
 * probe was unloaded).
 */
static unsigned long dstbase = 0x6800000000UL;   /* window base + 32 GiB */
static unsigned long dstsize = 2UL << 20;
module_param(dstbase, ulong, 0444);
module_param(dstsize, ulong, 0444);
static struct dev_pagemap dpg;
static void *dvaddr;
static int dst_ready;

#define MV_IOC_TRY  _IOW('M', 1, unsigned long)
#define MV_IOC_DST  _IOW('M', 2, unsigned long)   /* try a ZONE_DEVICE destination */

static long mv_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	unsigned long uaddr, start, end;
	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	unsigned long *src = NULL, *dst = NULL;
	struct migrate_vma mig;
	int ret;

	if (cmd != MV_IOC_TRY && cmd != MV_IOC_DST) return -ENOTTY;
	if (copy_from_user(&uaddr, (void __user *)arg, sizeof(uaddr))) return -EFAULT;

	start = uaddr & PAGE_MASK;
	end   = start + PAGE_SIZE;   /* one page is enough to answer the question */

	src = kcalloc(1, sizeof(*src), GFP_KERNEL);
	dst = kcalloc(1, sizeof(*dst), GFP_KERNEL);
	if (!src || !dst) { ret = -ENOMEM; goto out; }

	mmap_read_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start) {
		mmap_read_unlock(mm);
		pr_info("mv_probe: no VMA at 0x%lx\n", start);
		ret = -EFAULT; goto out;
	}
	pr_info("mv_probe: VMA 0x%lx: anonymous=%d vm_file=%d vm_ops=%d flags=0x%lx\n",
		start, vma_is_anonymous(vma), !!vma->vm_file, !!vma->vm_ops,
		(unsigned long)vma->vm_flags);
	mmap_read_unlock(mm);

	memset(&mig, 0, sizeof(mig));
	mig.vma          = vma;
	mig.start        = start;
	mig.end          = end;
	mig.src          = src;
	mig.dst          = dst;
	mig.flags        = MIGRATE_VMA_SELECT_SYSTEM;
	mig.pgmap_owner  = NULL;

	mmap_write_lock(mm);
	ret = migrate_vma_setup(&mig);
	if (ret) {
		pr_info("mv_probe: migrate_vma_setup -> %d  <-- REFUSED\n", ret);
	} else {
		pr_info("mv_probe: migrate_vma_setup -> 0  ACCEPTED  cpages=%lu npages=%lu src[0]=0x%lx\n",
			mig.cpages, mig.npages, src[0]);

		if (cmd == MV_IOC_DST && dst_ready && mig.cpages == 1 &&
		    (src[0] & MIGRATE_PFN_MIGRATE)) {
			/* The real question: can a window ZONE_DEVICE page be the
			 * destination? If migrate_vma_pages() takes it, the rmap
			 * walk in remove_migration_ptes() updates EVERY mapping
			 * and all-views coherence needs no hand-tracking. */
			struct page *dpage = pfn_to_page(dstbase >> PAGE_SHIFT);

			get_page(dpage);
			lock_page(dpage);
			dst[0] = migrate_pfn(page_to_pfn(dpage));
			pr_info("mv_probe: offering ZONE_DEVICE dst pfn=0x%lx\n",
				page_to_pfn(dpage));
			migrate_vma_pages(&mig);
			pr_info("mv_probe: after pages(): src[0]=0x%lx migrated=%s\n",
				src[0],
				(src[0] & MIGRATE_PFN_MIGRATE) ? "YES" : "NO -- refused");
			migrate_vma_finalize(&mig);
		} else {
			migrate_vma_pages(&mig);
			migrate_vma_finalize(&mig);
			pr_info("mv_probe: aborted and finalized (no pages moved)\n");
		}
	}
	mmap_write_unlock(mm);

out:
	kfree(src); kfree(dst);
	return ret;
}

static const struct file_operations mv_fops = {
	.owner = THIS_MODULE, .unlocked_ioctl = mv_ioctl,
};
static struct miscdevice mv_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "mv_probe", .fops = &mv_fops, .mode = 0666,
};
static int __init mv_init(void)
{
	memset(&dpg, 0, sizeof(dpg));
	dpg.type        = MEMORY_DEVICE_GENERIC;
	dpg.range.start = dstbase;
	dpg.range.end   = dstbase + dstsize - 1;
	dpg.nr_range    = 1;
	dvaddr = memremap_pages(&dpg, NUMA_NO_NODE);
	if (IS_ERR(dvaddr)) {
		pr_warn("mv_probe: dst pgmap unavailable (%ld); TRY still works\n",
			PTR_ERR(dvaddr));
		dvaddr = NULL;
	} else {
		dst_ready = 1;
		pr_info("mv_probe: dst pgmap at 0x%lx ready\n", dstbase);
	}
	return misc_register(&mv_misc);
}
static void __exit mv_exit(void)
{
	misc_deregister(&mv_misc);
	if (dvaddr) memunmap_pages(&dpg);
}
module_init(mv_init);
module_exit(mv_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Does migrate_vma_setup accept shmem-backed MAP_SHARED|MAP_ANONYMOUS?");
