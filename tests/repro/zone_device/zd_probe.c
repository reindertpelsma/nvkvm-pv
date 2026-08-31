// SPDX-License-Identifier: GPL-2.0
/*
 * zd_probe.c -- can the nvkvm GPA window carry `struct page`, and does that
 * buy us rmap?
 *
 * Stage 1 (already answered, kept as the precondition): memremap_pages() over
 * the window BAR gives valid ZONE_DEVICE struct pages.
 *
 * Stage 2 (the point): a struct page is only useful here if the kernel then
 * TRACKS the mappings. VM_PFNMAP has no rmap, which is why relocating a range
 * today can only rewrite the VMA in front of it. So: insert a window page into
 * a user VMA, and ask the kernel how many mappings it thinks exist.
 *
 *   /dev/zd_probe   mmap  -> inserts window pages into the caller's VMA
 *   /proc/zd_probe  read  -> the kernel's own view: mapcount, refcount, flags
 *
 * The insertion is tried two ways because they have different rmap semantics
 * and it is not obvious which (if either) accepts a ZONE_DEVICE page:
 *   vm_insert_page()   normal-page path, rmap-tracked, may reject non-normal
 *   vmf_insert_mixed() VM_MIXEDMAP path, may install a pte_special with no rmap
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/pfn_t.h>
#include <linux/pagemap.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

/* ioctl: ask the kernel to GUP/pin a user address that maps window pages.
 * This is the property the whole design rests on -- nvkvm GUPs the buffer and
 * the host driver pins it, so if PageReserved or ZONE_DEVICE blocks GUP the
 * approach is dead regardless of how good the rmap accounting is. */
#define ZD_IOC_GUP  _IOW('Z', 1, unsigned long)
#define ZD_IOC_PIN  _IOW('Z', 2, unsigned long)
#define ZD_IOC_HOLD _IOW('Z', 3, unsigned long)   /* pin and KEEP it */
#define ZD_IOC_DROP _IO('Z', 4)                   /* release the held pin */

static unsigned long base = 0x6000000000UL;
static unsigned long size = 128UL << 20;
static int touch;
static int mode = 0;   /* 0 = try vm_insert_page then vmf_insert_mixed */
static int devtype = 0;  /* 0 = MEMORY_DEVICE_GENERIC, 1 = MEMORY_DEVICE_COHERENT */
module_param(devtype, int, 0444);
MODULE_PARM_DESC(devtype, "0=GENERIC, 1=COHERENT (COHERENT is the type whose "
			  "refcount the driver owns, so page_free can fire)");
static atomic_t free_calls = ATOMIC_INIT(0);
module_param(base, ulong, 0444);
module_param(size, ulong, 0444);
module_param(touch, int, 0444);
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "0=auto, 1=vm_insert_page only, 2=vmf_insert_mixed only");

/*
 * The whole question for lifetime: does the kernel tell us when the last
 * reference to a window page goes away? If it does, a handle can be released
 * exactly when nothing can reach its pages any more -- no polling, and no
 * check-then-close race. If it does not, releasing safely needs something
 * else entirely.
 */
static void zd_page_free(struct page *page)
{
	int n = atomic_inc_return(&free_calls);
	if (n <= 4)
		pr_info("zd_probe: page_free() called for pfn=0x%lx (call #%d)\n",
			page_to_pfn(page), n);
}

static const struct dev_pagemap_ops zd_pgmap_ops = {
	.page_free = zd_page_free,
};

/* A deliberately leaked pin, to model a guest process that pins a window page
 * and outlives the registration -- the case the lifetime rule must catch. */
static struct page *held_page;

static struct dev_pagemap pgmap;
static void *vaddr;
static unsigned long base_pfn;

/* what the last mmap actually managed to do, for the /proc report */
static const char *last_path = "(no mmap yet)";
static int         last_err;

/* ── /proc/zd_probe: the kernel's own view of the first window page ─────── */
static int zd_show(struct seq_file *m, void *v)
{
	struct page *p;

	if (!vaddr) { seq_puts(m, "not mapped\n"); return 0; }
	p = pfn_to_page(base_pfn);

	seq_printf(m, "insert_path      %s (err=%d)\n", last_path, last_err);
	seq_printf(m, "is_zone_device   %d\n", is_zone_device_page(p));
	seq_printf(m, "page_mapcount    %d\n", page_mapcount(p));
	seq_printf(m, "page_ref_count   %d\n", page_ref_count(p));
	seq_printf(m, "PageLRU          %d\n", PageLRU(p));
	seq_printf(m, "PageReserved     %d\n", PageReserved(p));
	seq_printf(m, "PageAnon         %d\n", PageAnon(p));
	seq_printf(m, "page_mapping     %px\n", page_mapping(p));
	seq_printf(m, "zonenum          %u\n", page_zonenum(p));
	seq_printf(m, "pgmap_type       %s\n",
		   pgmap.type == MEMORY_DEVICE_COHERENT ? "COHERENT" : "GENERIC");
	seq_printf(m, "page_free_calls  %d\n", atomic_read(&free_calls));
	return 0;
}
static int zd_open(struct inode *i, struct file *f) { return single_open(f, zd_show, NULL); }
static const struct proc_ops zd_proc_ops = {
	.proc_open = zd_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

/* ── /dev/zd_probe: put window pages into a user VMA ────────────────────── */
static int zd_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long i, npages = len >> PAGE_SHIFT;
	struct page *p;
	int ret = -EINVAL;

	if (!vaddr) return -ENODEV;
	if (npages > (size >> PAGE_SHIFT)) return -EINVAL;

	/* (1) vm_insert_page: the normal-page path. If this works the mapping
	 *     is an ordinary file-backed-ish mapping and rmap applies. */
	if (mode == 0 || mode == 1) {
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		for (i = 0; i < npages; i++) {
			p = pfn_to_page(base_pfn + i);
			ret = vm_insert_page(vma, vma->vm_start + (i << PAGE_SHIFT), p);
			if (ret) break;
		}
		if (!ret) { last_path = "vm_insert_page"; last_err = 0; return 0; }
		pr_info("zd_probe: vm_insert_page rejected at page %lu: %d\n", i, ret);
		if (mode == 1) { last_path = "vm_insert_page"; last_err = ret; return ret; }
		zap_vma_ptes(vma, vma->vm_start, len);
	}

	/* (2) vmf_insert_mixed: the VM_MIXEDMAP path devices normally use.
	 *     Expected to succeed -- the open question is whether the result is
	 *     rmap-tracked or a pte_special the kernel does not account. */
	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP);
	for (i = 0; i < npages; i++) {
		vm_fault_t vf = vmf_insert_mixed(vma, vma->vm_start + (i << PAGE_SHIFT),
						 __pfn_to_pfn_t(base_pfn + i, PFN_DEV | PFN_MAP));
		if (vf & VM_FAULT_ERROR) {
			pr_info("zd_probe: vmf_insert_mixed failed at %lu (vf=0x%x)\n", i, vf);
			last_path = "vmf_insert_mixed"; last_err = -EFAULT;
			return -EFAULT;
		}
	}
	last_path = "vmf_insert_mixed"; last_err = 0;
	return 0;
}

static long zd_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	unsigned long uaddr = 0;
	struct page *pg = NULL;
	long n;

	/* Only the address-taking commands have an argument. Copying for all of
	 * them made ZD_IOC_DROP (an _IO with no arg) fail with -EFAULT before
	 * the switch, which read as "unpin does not work" -- a test bug that
	 * looked exactly like a kernel limitation. */
	if (cmd != ZD_IOC_DROP &&
	    copy_from_user(&uaddr, (void __user *)arg, sizeof(uaddr)))
		return -EFAULT;

	switch (cmd) {
	case ZD_IOC_GUP:
		n = get_user_pages_fast(uaddr, 1, FOLL_WRITE, &pg);
		pr_info("zd_probe: get_user_pages_fast(0x%lx) -> %ld%s\n", uaddr, n,
			n == 1 ? "" : "  <-- FAILED");
		if (n == 1) {
			pr_info("zd_probe:   page=%px pfn=0x%lx zone_device=%d "
				"mapcount=%d refcount=%d reserved=%d\n",
				pg, page_to_pfn(pg), is_zone_device_page(pg),
				page_mapcount(pg), page_ref_count(pg), PageReserved(pg));
			put_page(pg);
		}
		return n == 1 ? 0 : (n < 0 ? n : -EFAULT);

	case ZD_IOC_HOLD:
		if (held_page) return -EBUSY;
		n = pin_user_pages_fast(uaddr, 1, FOLL_WRITE, &held_page);
		if (n != 1) { held_page = NULL; return -EFAULT; }
		pr_info("zd_probe: HELD pin on pfn=0x%lx refcount now %d\n",
			page_to_pfn(held_page), page_ref_count(held_page));
		return 0;

	case ZD_IOC_DROP:
		if (!held_page) return -ENOENT;
		pr_info("zd_probe: dropping held pin on pfn=0x%lx (refcount %d)\n",
			page_to_pfn(held_page), page_ref_count(held_page));
		unpin_user_page(held_page);
		held_page = NULL;
		return 0;

	case ZD_IOC_PIN:
		/* FOLL_PIN is what a driver doing DMA uses, and it is stricter
		 * than a plain GUP reference -- notably it refuses some page
		 * types outright. This is the call the NVIDIA driver makes. */
		n = pin_user_pages_fast(uaddr, 1, FOLL_WRITE, &pg);
		pr_info("zd_probe: pin_user_pages_fast(0x%lx) -> %ld%s\n", uaddr, n,
			n == 1 ? "" : "  <-- FAILED");
		if (n == 1) {
			pr_info("zd_probe:   pinned page=%px pfn=0x%lx zone_device=%d\n",
				pg, page_to_pfn(pg), is_zone_device_page(pg));
			unpin_user_page(pg);
		}
		return n == 1 ? 0 : (n < 0 ? n : -EFAULT);
	}
	return -ENOTTY;
}

static const struct file_operations zd_fops = {
	.owner          = THIS_MODULE,
	.mmap           = zd_mmap,
	.unlocked_ioctl = zd_ioctl,
};
static struct miscdevice zd_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "zd_probe", .fops = &zd_fops, .mode = 0666,
};

static int __init zd_init(void)
{
	unsigned long npages = size >> PAGE_SHIFT;
	struct page *p0;
	u64 t0, t1;

	pr_info("zd_probe: base=0x%lx size=0x%lx (%lu pages, memmap ~%lu KiB)\n",
		base, size, npages, (npages * sizeof(struct page)) >> 10);

	memset(&pgmap, 0, sizeof(pgmap));
	pgmap.type        = devtype ? MEMORY_DEVICE_COHERENT : MEMORY_DEVICE_GENERIC;
	pgmap.range.start = base;
	pgmap.range.end   = base + size - 1;
	pgmap.nr_range    = 1;
	pgmap.ops         = &zd_pgmap_ops;

	t0 = ktime_get_ns();
	vaddr = memremap_pages(&pgmap, NUMA_NO_NODE);
	t1 = ktime_get_ns();
	if (IS_ERR(vaddr)) {
		pr_err("zd_probe: memremap_pages FAILED: %ld\n", PTR_ERR(vaddr));
		vaddr = NULL;
		return -EINVAL;
	}
	base_pfn = base >> PAGE_SHIFT;
	p0 = pfn_to_page(base_pfn);
	pr_info("zd_probe: memremap_pages OK in %llu us, vaddr=%px\n", (t1-t0)/1000, vaddr);
	pr_info("zd_probe: type=%s zone_device=%d refcount=%d LRU=%d zonenum=%u\n",
		devtype ? "COHERENT" : "GENERIC",
		is_zone_device_page(p0), page_ref_count(p0), PageLRU(p0), page_zonenum(p0));
	pr_info("zd_probe: initial refcount is the thing that decides lifetime: "
		"if the pgmap holds one permanently it never reaches 0 and "
		"page_free() can never fire\n");

	if (touch) {
		u32 *q = (u32 *)vaddr;
		WRITE_ONCE(q[0], 0xA5A5A5A5);
		pr_info("zd_probe: kernel vaddr readback=0x%08x %s\n", READ_ONCE(q[0]),
			READ_ONCE(q[0]) == 0xA5A5A5A5 ? "OK" : "MISMATCH");
	}

	proc_create("zd_probe", 0444, NULL, &zd_proc_ops);
	if (misc_register(&zd_misc)) pr_warn("zd_probe: misc_register failed\n");
	return 0;
}

static void __exit zd_exit(void)
{
	if (held_page) { unpin_user_page(held_page); held_page = NULL; }
	misc_deregister(&zd_misc);
	remove_proc_entry("zd_probe", NULL);
	if (vaddr) { memunmap_pages(&pgmap); pr_info("zd_probe: memunmap_pages done\n"); }
}
module_init(zd_init);
module_exit(zd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Does the nvkvm window support struct page, and does that buy rmap?");
