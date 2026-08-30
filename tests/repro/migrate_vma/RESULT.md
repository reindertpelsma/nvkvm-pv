# Can `migrate_vma` move a CUDA host buffer into the window?

**Measured 2026-08-31**, guest 6.8.0-138-generic under nvkvm.

`migrate_vma` is the right mechanism for all-views coherence:
`migrate_vma_unmap()` goes through `try_to_migrate()`, an **rmap walk** that
replaces the PTE in *every* mapping with a migration entry, and
`remove_migration_ptes()` restores them all to the new page. Nothing needs
hand-tracking, and mappings created later resolve correctly because the
address_space is updated, not just the VMAs.

## Source: accepted

    MAP_PRIVATE|MAP_ANONYMOUS   migrate_vma_setup -> ACCEPTED  cpages=1
    MAP_SHARED|MAP_ANONYMOUS    migrate_vma_setup -> ACCEPTED  cpages=1
      (VMA: anonymous=0 vm_file=1 vm_ops=1 -- shmem-backed)

The second is what `cuMemHostAlloc` returns (traced: `mmap(..., MAP_SHARED|
MAP_FIXED|MAP_ANONYMOUS, -1, 0)` out of a `PROT_NONE` arena libcuda reserves
itself).

**A prior objection here was wrong.** `migrate_vma` was believed to require
`vma_is_anonymous()`, which is false for `MAP_SHARED|MAP_ANONYMOUS` because
Linux backs it with an internal shmem file. On 6.8 it is accepted anyway, and
`cpages=1` confirms the page was collected as migratable.

## Destination: refused

    offering ZONE_DEVICE dst pfn=0x6800000
    after pages(): src[0]=0x4a5f409  migrated=NO -- refused
      (MIGRATE_PFN_MIGRATE cleared by migrate_vma_pages)

`migrate_vma_pages()` handles `MEMORY_DEVICE_PRIVATE` destinations specially
and otherwise expects a normal system folio. A `MEMORY_DEVICE_GENERIC` page is
neither, so the migration is dropped rather than performed.

## Where that leaves it

The destination is the binding constraint, and it is the same wall reached from
the other direction:

| destination type | accepted by `migrate_vma` | CPU-accessible | on stock kernels |
|---|---|---|---|
| normal system folio | yes | yes | yes — but it is not the window |
| `MEMORY_DEVICE_PRIVATE` | yes | **no** | yes |
| `MEMORY_DEVICE_COHERENT` | yes | yes | **no** (`CONFIG_DEVICE_COHERENT` unset) |
| `MEMORY_DEVICE_GENERIC` | **no** (measured) | yes | yes |

So on a stock guest kernel there is no destination that is both migratable and
CPU-readable. Requiring `CONFIG_DEVICE_COHERENT` would close it and means a
custom guest kernel.

## Interaction found while testing

Claiming a window sub-range with `memremap_pages()` while nvkvm is using it
**breaks CUDA outright** -- `cuCtxCreate` failed until the probe was unloaded.
nvkvm allocates window GPAs from the base upward, so any real implementation
must own the pgmap and coordinate with its own allocator rather than layering
on top. The destination test above deliberately sits 32 GiB into the window to
avoid this.

# The blocker is the SOURCE, measured

Same destination pgmap, same code path, only the source VMA shape changed:

    VMA anonymous=1  (MAP_PRIVATE|MAP_ANONYMOUS)  -> after pages(): migrated=YES
    VMA anonymous=0  (MAP_SHARED|MAP_ANONYMOUS)   -> after pages(): migrated=NO

So `migrate_vma` *will* move a private anonymous page into device memory. It
refuses a shmem page. `cuMemHostAlloc` returns `MAP_SHARED|MAP_ANONYMOUS`,
which is shmem, so the memory that needs moving is exactly the kind it will not
move.

An earlier explanation here -- "GENERIC is the wrong destination type" -- was
wrong: `MEMORY_DEVICE_PRIVATE` was refused too, for the same shmem source.

## The two APIs have complementary gaps

| | shmem source | ZONE_DEVICE destination |
|---|---|---|
| `migrate_vma_*` | **refused** (measured) | accepted (PRIVATE) |
| `migrate_pages()` | accepted (NUMA/compaction move page cache this way) | **refused** (`PageLRU=0`, measured) |

Neither pairing works on a stock kernel. And this is the same fact that blocked
the address_space route and the all-views-VMA route: **shmem owns those pages**,
so no third party can re-home them.

## The one combination that could work

`migrate_pages()` takes a `new_folio_t` callback -- the caller chooses the
destination folio. It accepts shmem sources. So a *normal* folio that happens
to live in host-visible memory would satisfy both sides at once.

That requires the window to be normal System RAM rather than ZONE_DEVICE:
hotplug it with `add_memory_driver_managed()`, then immediately reserve all of
it with `alloc_contig_range()` so nothing is ever handed to the general
allocator, and serve migration destinations from that reserve.

Costs, both real:

- The window must be **fully backed**, not sparse -- System RAM cannot contain
  GPAs that fault to the VMM.
- Every page must be carved out at init. Any part left to the general allocator
  is guest memory that can land in host-visible pages, which inverts the
  isolation property. Reserving all of it restores the property, but it has to
  be all.

## Teardown hazard found

A successful migration into `MEMORY_DEVICE_PRIVATE` left the module unremovable
(refcount -1): `memunmap_pages()` waits for every device page to be freed, and
the probe implements `migrate_to_ram` as `VM_FAULT_SIGBUS` rather than
migrating back. A real implementation must implement `migrate_to_ram`
faithfully, or teardown hangs the guest. Recovering needs a guest reboot.
