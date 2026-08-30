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
