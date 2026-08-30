// SPDX-License-Identifier: GPL-2.0
/*
 * zd_probe.c -- can the nvkvm GPA window carry `struct page`?
 *
 * The window is installed today with remap_pfn_range() over a non-System-RAM
 * BAR, so it has no struct page. That is why it cannot be a target for
 * migrate_pages()/rmap -- the machinery that makes swap coherent across every
 * mapping of a file, including ones created later. memremap_pages() exists to
 * give non-RAM a struct page; this asks whether it works here, and at what
 * cost, WITHOUT touching nvkvm's own paths.
 *
 * Deliberately does not read or write the range by default. The window is
 * sparse: only sub-ranges nvkvm has mapped a memfd into are backed, and a
 * load/store to an unbacked GPA leaves the guest for QEMU as MMIO. Pass
 * touch=1 only for a range you know is backed.
 *
 *   insmod zd_probe.ko base=0x6000000000 size=0x8000000     # 128 MiB
 *   insmod zd_probe.ko base=... size=... touch=1            # only if backed
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/memremap.h>
#include <linux/mm.h>
#include <linux/io.h>

static unsigned long base = 0x6000000000UL;
static unsigned long size = 128UL << 20;
static int touch;
module_param(base, ulong, 0444);
module_param(size, ulong, 0444);
module_param(touch, int, 0444);
MODULE_PARM_DESC(base,  "guest-physical base of the region (nvkvm window BAR2)");
MODULE_PARM_DESC(size,  "bytes to map; memmap costs size/4096*64 bytes of RAM");
MODULE_PARM_DESC(touch, "1 = read/write the range (ONLY if it is backed)");

static struct dev_pagemap pgmap;
static void *vaddr;

static int __init zd_init(void)
{
	unsigned long pfn, npages = size >> PAGE_SHIFT;
	struct page *p0, *pn;
	u64 t0, t1;

	pr_info("zd_probe: base=0x%lx size=0x%lx (%lu pages, memmap ~%lu KiB)\n",
		base, size, npages, (npages * sizeof(struct page)) >> 10);

	/* MEMORY_DEVICE_GENERIC: CPU-accessible device memory. NOT
	 * MEMORY_DEVICE_PRIVATE, whose pages are deliberately not CPU-mappable
	 * (a fault migrates them back), which is the opposite of what a shared
	 * host window needs. */
	memset(&pgmap, 0, sizeof(pgmap));
	pgmap.type              = MEMORY_DEVICE_GENERIC;
	pgmap.range.start       = base;
	pgmap.range.end         = base + size - 1;
	pgmap.nr_range          = 1;
	pgmap.ops               = NULL;

	t0 = ktime_get_ns();
	vaddr = memremap_pages(&pgmap, NUMA_NO_NODE);
	t1 = ktime_get_ns();

	if (IS_ERR(vaddr)) {
		pr_err("zd_probe: memremap_pages FAILED: %ld\n", PTR_ERR(vaddr));
		pr_err("zd_probe:   (-EINVAL usually means alignment: needs "
		       "sub-section (2MiB) granularity)\n");
		vaddr = NULL;
		return PTR_ERR(vaddr ?: ERR_PTR(-EINVAL));
	}
	pr_info("zd_probe: memremap_pages OK in %llu us, vaddr=%px\n",
		(t1 - t0) / 1000, vaddr);

	/* The point of the exercise: do these PFNs have struct pages, and do
	 * they round-trip? That is what rmap and migrate_pages() require. */
	pfn = base >> PAGE_SHIFT;
	p0  = pfn_to_page(pfn);
	pn  = pfn_to_page(pfn + npages - 1);
	pr_info("zd_probe: pfn_to_page(first)=%px last=%px\n", p0, pn);
	pr_info("zd_probe: round-trip first: pfn=0x%lx -> page -> pfn=0x%lx %s\n",
		pfn, page_to_pfn(p0), page_to_pfn(p0) == pfn ? "OK" : "MISMATCH");
	pr_info("zd_probe: is_zone_device_page(first)=%d refcount=%d\n",
		is_zone_device_page(p0), page_ref_count(p0));
	pr_info("zd_probe: page_zonenum=%u  pgmap=%px (ours=%px) %s\n",
		page_zonenum(p0), p0->pgmap, &pgmap,
		p0->pgmap == &pgmap ? "OK" : "UNEXPECTED");

	if (touch) {
		u32 *q = (u32 *)vaddr;
		pr_info("zd_probe: touch=1, writing/reading first word\n");
		WRITE_ONCE(q[0], 0xA5A5A5A5);
		pr_info("zd_probe: readback=0x%08x %s\n", READ_ONCE(q[0]),
			READ_ONCE(q[0]) == 0xA5A5A5A5 ? "OK" : "MISMATCH");
	} else {
		pr_info("zd_probe: touch=0, not accessing the range "
			"(sparse window: unbacked GPAs exit to the VMM)\n");
	}
	return 0;
}

static void __exit zd_exit(void)
{
	if (vaddr) {
		memunmap_pages(&pgmap);
		pr_info("zd_probe: memunmap_pages done\n");
	}
}

module_init(zd_init);
module_exit(zd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe whether the nvkvm GPA window can carry struct page");
