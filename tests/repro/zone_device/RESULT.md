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

# Stage 2 result: the pages ARE rmap-tracked

**Measured 2026-08-31**, same guest. `zd_probe.ko` inserts window pages into a
user VMA; `zd_rmap_test` reads the kernel's own accounting across mmap and
fork.

    insert_path   vm_insert_page (err=0)
    mapcount      before=0   after mmap=1   after fork=2
    refcount      1 -> 2 -> 3
    PageLRU 0   PageReserved 1   page_mapping NULL   zonenum 4
    RESULT: PASS -- the kernel tracks these mappings (rmap works)

Against the four open points:

**1. Insertable into a user VMA -- YES, via `vm_insert_page()`.** The normal
page path accepted a ZONE_DEVICE page. The `vmf_insert_mixed()`/`VM_MIXEDMAP`
fallback was never needed, which matters: that path installs a `pte_special`
the kernel does not account, and would have bought nothing.

**2. rmap-tracked -- YES.** `page_mapcount()` rises with each mapping,
including across `fork()`. This is the property `VM_PFNMAP` lacks and the whole
reason relocation could only ever rewrite one VMA.

**3. `migrate_pages()` destination -- NO.** `PageLRU=0`, `PageReserved=1`:
standard migration isolates LRU folios and these are not on the LRU. Less
important than it looks -- see below.

**4. CPU access -- still untested.** The probe maps an unbacked part of the
sparse window and deliberately does not touch it. Low risk rather than unknown:
the window is demonstrably CPU-accessible when backed (every CUDA check reads
its results back through it) and ZONE_DEVICE changes page metadata, not memory.
Not claimed until run.

## What this changes

If the window were populated with `vm_insert_page()` over ZONE_DEVICE pages
instead of `remap_pfn_range()` under `VM_PFNMAP`, the kernel would track every
mapping itself. The `page_mapcount() > 1` guard added to refuse shared
relocation becomes the *natural* accounting rather than a special case, and the
"find and convert every other view" problem -- foreign `mmap_write_lock`,
shmem `vm_ops` refaulting, views created after registration -- does not arise,
because nothing is being relocated.

That is the argument for allocate-in-window, arrived at independently: with
rmap present, (3) stops mattering, since migration is only needed to move pages
*into* the window, and a buffer allocated there never has to move.

**Unquantified:** `PageReserved=1` is worth understanding before relying on
this -- several mm paths skip reserved pages, and it may constrain what else
these mappings can participate in.

# Stage 3: GUP, PIN, CPU access — all four questions closed

**Measured 2026-08-31**, same guest and window.

    get_user_pages_fast:  OK
    pin_user_pages_fast:  OK
    CPU access:  initial read 0x00000000, write 0x5A5A5A5A, readback 0x5a5a5a5a
    mapcount:    0 -> 1 (mmap) -> 2 (fork)

**`PageReserved=1` constrains nothing that matters here.** It does not block
GUP or `pin_user_pages`, and the paths it does exclude (swap, reclaim) are ones
this memory must be excluded from anyway.

**GUP and PIN both work**, which was the real question behind
`PageReserved`: nvkvm GUPs the registered buffer and the host driver pins it
for DMA. Either failing would have ended the approach regardless of how good
the rmap accounting is.

**CPU access caveat.** The mapping is write-back cached, so a write followed
immediately by a read can be served from cache without reaching the device.
The result proves the mapping is *usable*; it does not prove that particular
GPA is backed. Distinguishing them needs a flush, or a read through a second
independent mapping.

## Status of the four

| # | question | result |
|---|---|---|
| 1 | insertable into a user VMA | **YES**, via `vm_insert_page` (normal path) |
| 2 | rmap-tracked | **YES**, mapcount rises per mapping |
| 3 | `migrate_pages()` destination | **NO** (`PageLRU=0`) — and not needed |
| 4 | GUP / PIN / CPU access | **YES** / **YES** / usable (see caveat) |

## The security-critical design point this exposes

Refcounting is not free. Today the window is `VM_PFNMAP`: no `struct page`, so
no reference can be taken, and teardown is unconditional. Give the pages a
`struct page` and a guest process can `pin_user_pages()` one — directly, or via
O_DIRECT, or by handing it to another driver for DMA — and that pin can
**outlive the registration**.

That turns revocation into a correctness *and* security question:

- If QEMU unmaps the memfd from that GPA while a guest pin is live, the guest
  holds a pin on a PFN that may later back **a different isolate's memory**.
  That is a cross-isolate read/write primitive, reachable from ordinary guest
  userspace.
- Refusing to revoke while pinned is the safe direction, but then a guest can
  pin window pages and never release them, holding host memfd pages
  indefinitely.

So the rule has to be explicit: **a window GPA may not be re-pointed at
different backing while any reference to its pages exists.** Either revocation
blocks on the refcount, or a GPA range is retired rather than reused. Getting
this wrong is worse than the bug this whole line of work started from, because
it crosses an isolate boundary rather than corrupting one process's own data.

Nothing measured here blocks the approach. This is the piece that has to be
designed rather than discovered.

# Stage 4: lifetime — can a release gate be built on refcount?

**Measured 2026-08-31.** The proposed rule is: the VMM memfd behind a window
range is released only when nothing references its pages, so a GPA can never be
re-pointed at different backing under a live reference.

## No stock-kernel ZONE_DEVICE type gives a free callback

| type | CPU-accessible | `page_free` fires | available on stock Ubuntu |
|---|---|---|---|
| `MEMORY_DEVICE_GENERIC` | yes | **no** | yes |
| `MEMORY_DEVICE_COHERENT` | yes | yes | **no** |
| `MEMORY_DEVICE_PRIVATE` | **no** | yes | yes |

Measured, not inferred:

- GENERIC: `page_free_calls 0` after a full map/fork/unmap cycle, and the
  refcount sits at 1 with nothing mapped -- the pgmap holds a permanent
  reference, so the count never reaches 0 and the callback cannot fire.
- COHERENT: `memremap_pages` fails, `WARNING at mm/memremap.c:330`.
  `CONFIG_DEVICE_COHERENT` is **not set** on 6.8.0-138-generic, and requiring a
  custom guest kernel is not an option.
- PRIVATE: excluded by design -- its pages are deliberately not CPU-mappable
  (a CPU fault migrates them back), which is backwards for a window the CPU and
  GPU both read.

## So the gate is a check, and the check is sound

    baseline (nothing mapped)        refcount=1
    after mmap                       refcount=2
    after a pin is taken and HELD    refcount=1026
    after munmap, pin still held     refcount=1025   <-- the gate
    after the pin is dropped         refcount=1

A pin that outlives its mapping stays visible, and the count returns exactly to
the baseline once released. **A release gate on `refcount == 1` is sound.**

Three consequences for the implementation:

1. `FOLL_PIN` adds `GUP_PIN_COUNTING_BIAS` (1024). The gate is `== 1`; a pin
   appears as +1024, not +1. Code must not assume small increments.
2. **No notification.** Without `page_free` nothing signals the last reference
   dropping, so a deferred release needs a retry trigger: `-EBUSY` on close,
   retried at process exit, session teardown, or the module-load `RESET`.
3. The GENERIC baseline is exactly 1, which keeps the gate a constant compare.

## A test bug that impersonated a blocker

The first run of this reported `refcount unmap=1025 drop=1025`, i.e. "unpin
does not work on ZONE_DEVICE pages" -- which would have been a genuine dead
end. It was the probe: `copy_from_user()` ran for every ioctl including
`ZD_IOC_DROP`, an `_IO` with no argument, so it returned `-EFAULT` before
reaching the switch and the unpin never executed. The tell was in the kernel
log -- a `HELD pin` line with no matching `dropping held pin` line. Check that
the code under test actually ran before believing a negative result.
