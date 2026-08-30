# Registering a shared mapping with CUDA silently desynchronises it

**Measured 2026-08-30**, RTX 3050 Laptop GPU, driver 580.173.02, tree `8f5d104`.
Repro: `tests/repro/shared_view_desync.c`. **No nesting is involved.**

## Result

One guest, two processes, one `MAP_SHARED|MAP_ANONYMOUS` buffer inherited
across `fork()`. The parent calls `cuMemHostRegister` on it, then writes a new
pattern through its own pointer. The child reads.

| | child sees the parent's write | verdict |
|---|---|---|
| real NVIDIA driver (control) | `B` in 65536/65536 words | **PASS** |
| nvkvm, single level | `B` in **0**, still `A` in 65536/65536 | **FAIL** |

`cuMemHostRegister` returns **0**. Nothing logs a warning. The parent's own view
is perfectly consistent. The two processes simply stop sharing memory, totally
and silently, from the moment of registration.

The control matters: the same binary on the same machine against the stock
driver passes. This is nvkvm's behaviour, not CUDA's and not the GPU's.

## Why

`nvkvm_cpu_pages_migrate_range()` makes a guest buffer reachable by the host
driver by copying its pages into a memfd shared with the isolate and
repointing the guest VMA at that memfd (`VM_PFNMAP`). Relocation is sound only
for a VMA with **exactly one view**. It never checks that there is one.

Any second view keeps the original pages:

- `MAP_SHARED|MAP_ANONYMOUS` inherited across `fork()` (this test),
- a memfd deliberately shared between cooperating processes,
- a VMM's guest-RAM memfd aliased into a KVM memslot -- the nested case in
  `../nested-nvkvm-l2/`, which is this same bug with a guaranteed second view.

So the nested zeros were never a nesting bug. Nesting was just the arrangement
that made the second view certain.

## The S_PRIVATE guard does NOT cover this

`8f5d104` tests `IS_PRIVATE(inode)` to reject memfds. That is necessary but not
sufficient, and it does not help here: `MAP_SHARED|MAP_ANONYMOUS` is created by
`shmem_zero_setup()`, so it **is** `S_PRIVATE` (measured: `priv=1`), the guard
admits it, and the corruption above happened with that guard loaded.

`S_PRIVATE` distinguishes "kernel-created" from "userspace-created". The
property that actually matters is "how many mappings do these pages already
have", and the two are not the same question.

## What a real fix has to test

Sharing, not provenance. Candidates, none yet measured:

- **`folio_mapcount()` / `page_mapcount() > 1` over the pinned range** -- directly
  observes a second mapping and covers fork, shared memfd and the memslot alias
  alike. Needs thought about transient elevation and about pages not yet faulted
  into the second process.
- **`i_mmap` interval-tree walk** on the inode, to see whether a VMA other than
  ours maps the object.

Both answer "is this shared", which is the question. Until one is in place, the
honest description of the constraint is: *do not register a mapping that any
other process can see.* That is not currently stated anywhere a user would look,
and it is not enforced.

## Severity

Silent, total, and reachable from ordinary code -- shared-memory IPC plus CUDA
is a normal pattern, not an exotic one. Every ioctl returns success. A workload
would see stale data with no diagnostic. This is a correctness bug in the
single-level path, i.e. in what ships, and it should be triaged before the
release rather than filed with the nested experiment.

## Why this cannot be fixed with the kernel's page migration

The obvious proposal -- "move the physical page the way swap does, so every
mapping follows via rmap" -- identifies the right *property* but cannot use the
kernel's mechanism, and it is worth writing down why so it is not re-proposed.

Swap and `migrate_pages()` move a folio between **kernel-managed pages** while
preserving its `address_space`; rmap then rewrites every PTE. Both halves fail
here:

- **The destination has no `struct page` in the guest.** It is a host-side
  memfd, filled over virtio through bounce slots
  (`nvkvm_virtio_open_memory_handle` / `_write_memory_handle`), exposed to the
  guest as a sparse GPA window that the code itself describes as "a PCI-BAR
  (non-RAM) region" and installs with `remap_pfn_range` under
  `VM_PFNMAP|VM_IO`. `migrate_pages()` has nothing to migrate *to*.
- **It re-homes into a different object.** Swap never moves a page from one
  `address_space` into another; this moves guest anon/shmem pages into a host
  memfd. Migration preserves object identity by design.

Hand-rolling the rmap walk instead does not rescue it: the other view is an
ordinary shmem VMA, so PFN PTEs would contradict its `vm_flags`, and the next
fault there would go through shmem's fault handler and repopulate the original
page regardless.

## Correction: "do not relocate" breaks the isolation model

The paragraph below was written before accounting for what the copy BUYS, and
is left in place with this correction rather than deleted, because the idea is
the obvious one and will be proposed again.

**Mapping the guest-RAM object into the isolate would hand a GPU-facing
sandboxed process the entire guest.** The per-registration memfd is not an
implementation detail: because it contains only the registered bytes, it is
the boundary that keeps the isolate from reading all of guest memory. nvkvm
spends real effort on that confinement (per-VM uid, chroot, seccomp), and this
would undo it.

The idea survives only in range-limited form -- the isolate may map *only* the
ranges it has been granted. That is architecturally consistent with what
already exists: QEMU brokers every isolate mapping through `iso_mmap_tbl`
(`NVKVM_ISO_MMAP_MAX` 8192), `iso_mmap_translate()` validates
`(isolate_id, base, len)` against recorded entries, and `iso_mmap_free()`
invalidates before the isolate-side munmap. Granting and revoking ranges is
the existing model.

But three constraints stand in the way, and together they are why the copy is
defensible rather than merely convenient:

1. **Guest RAM is not a shareable object today.** `scripts/run_test_vm.sh`
   boots with a plain `-m "$VM_MEM"`: anonymous private memory in QEMU's
   address space. There is nothing to range-map, and anonymous memory cannot
   be shared with a separate process at all. This would require
   `memory-backend-memfd,share=on`, which makes *all* guest RAM a shareable
   object -- acceptable only because exposure stays gated by what QEMU maps,
   but a real change in blast radius.
2. **Scatter versus the token table.** The copy buys contiguity: 2 GiB in
   2 MiB chunks is 1024 tokens, 1/8 of the table. Without it the design
   inherits whatever physical scatter the guest allocator produced -- worst
   case 4 KiB runs, 524288 tokens for the same 2 GiB against a table of 8192.
3. **Lifetime.** With a copy, the isolate's memfd is independent of what the
   guest later does with its pages. Without one, the isolate maps live guest
   pages: the pins must be held for the whole registration and revoked exactly
   on unregister, or the isolate later reads unrelated guest data through a
   stale mapping. The copy provides that property for free.

So the near-term sharing check below is not obviously just a stopgap. Given
(1)-(3) it may be the right permanent answer for shared mappings, with a
no-copy path reserved for a design that solves all three.

## Superseded: do not relocate

The guest's pages are already host memory, inside the VMM's memslot. The host
does not need a *copy* in a second memfd -- it needs to reach the pages where
they already are. One physical home means every view stays valid by
construction, and memfd vs fork-shared vs private becomes irrelevant rather
than something to detect.

That is the same conclusion as "allocate the backing once from a DMA-able
window at the bottom level", reached from the opposite direction, and it also
removes the per-chunk copy that currently costs a full bounce of every
registered byte.

Open questions before this is a plan, none yet measured: whether the guest can
describe a scattered GPA set to the host cheaply enough (the current design
buys contiguity with the copy), and whether the stub's pin can be held stable
against guest-side reclaim of those pages.

## Interim

Detection is cheap and makes the failure honest; a sharing test
(`page_mapcount() > 1` over the pinned range, or an `i_mmap` walk) would turn
silent divergence into `-EINVAL` for fork-shared and shared-memfd cases alike,
the same way `8f5d104` did for the memslot case. It is a stopgap: it refuses
work rather than performing it correctly.
