# Can the nvkvm window carry `struct page`? Yes.

**Measured 2026-08-31**, guest 6.8.0-138-generic under nvkvm, RTX 3050 host.
Probe: `zd_probe.c`. Window found from the guest's PCI resources: device
`00:04.0` BAR2, a 64 GiB prefetchable region at guest-physical
`0x60_0000_0000`.

## Result

    memremap_pages OK in 16435 us, vaddr=ffff8da200000000
    pfn_to_page(first)=fffff10840000000  last=fffff108401fffc0
    round-trip: pfn=0x6000000 -> page -> pfn=0x6000000  OK
    is_zone_device_page(first)=1   refcount=1
    page_zonenum=4   pgmap matches ours
    memmap ~2048 KiB for 128 MiB mapped

`memunmap_pages()` tears down cleanly and the range re-maps to the same address
afterwards, with no `BUG`/`WARN` in between.

Kernel support is present and exported: `CONFIG_ZONE_DEVICE=y`,
`CONFIG_SPARSEMEM_VMEMMAP=y` (needed for sub-section granularity),
`memremap_pages`/`memunmap_pages` both in `kallsyms`.

`MEMORY_DEVICE_GENERIC`, not `MEMORY_DEVICE_PRIVATE`: private pages are
deliberately not CPU-mappable (a fault migrates them back), which is backwards
for a window the CPU and GPU must both read.

## Why this matters

The window is installed today with `remap_pfn_range()` under `VM_PFNMAP|VM_IO`.
Such a mapping has **no `struct page`, and therefore no rmap** -- the kernel
does not track who maps it. That is the root of the shared-mapping bug: with no
rmap, relocating a range can only rewrite the VMA in front of it, so every
other view is silently left behind.

A `struct page` is not tied to a process. If the window's pages carry one, the
mappings become rmap-tracked and the kernel does the "find every view"
bookkeeping we were otherwise going to hand-roll -- which is the same machinery
that makes swap coherent across mappings created *after* the page moved.

## Cost

1.56% of the mapped size, in memmap. Trivial per registration; 1 GiB if the
whole 64 GiB window were mapped eagerly, so it must be done per sub-range on
demand rather than once at probe.

## Proven here, and NOT proven

**Proven:** `memremap_pages()` succeeds on this region, the PFNs get valid
struct pages that round-trip, they are ZONE_DEVICE with our pgmap, and teardown
is clean and repeatable.

**Not proven, and each could still sink the approach:**

1. That these pages can be inserted into a user VMA at all --
   `vm_insert_page()` rejects non-normal pages, so it would have to be
   `VM_MIXEDMAP` + `vmf_insert_mixed()`.
2. That once inserted they are genuinely **rmap-tracked**, which is the entire
   point. Untested.
3. That they can be `migrate_pages()` destinations. ZONE_DEVICE pages are not
   allocated from the page allocator, and generic-device migration support is
   narrower than device-private.
4. CPU access through the mapping. The probe deliberately does not touch the
   range: the window is sparse, and an unbacked GPA exits to the VMM as MMIO.
   This needs a sub-range nvkvm has actually backed with a memfd.

(2) is the one to test next, and it is cheap: insert a page into a VMA, fork,
and check whether the kernel reports both mappings.
