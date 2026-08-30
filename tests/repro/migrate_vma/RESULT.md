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
