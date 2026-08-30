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

## Scope decision: I/O memory stays unsupported, ordinary RAM must work

Registering **PCI BAR or other I/O memory** with `cuMemHostRegister` stays
refused (`VM_PFNMAP|VM_IO|VM_MIXEDMAP|VM_HUGETLB`). In a guest without device
passthrough an application's pointers come from `malloc`/`mmap`, CUDA device
memory, or managed memory; the apparent exceptions (V4L2 buffers, dma-buf,
RDMA regions) are virtio-backed and therefore ordinary guest RAM. This is
rarer in a VM than on bare metal, and costs little to leave out.

**Do not confuse that with the shared-mapping cases.** `MAP_SHARED|MAP_ANONYMOUS`
across `fork()` and a shared memfd are *ordinary guest RAM*. They are refused
today for a different reason -- relocation desynchronises the other views --
and scoping out I/O does nothing for them. They are exactly what should work.

### Correction: range-granting an fd is not a thing

An earlier revision of this section claimed the `iso_mmap_tbl` broker could
grant the isolate "only the GPA runs covering the buffer", giving zero-copy
without weakening isolation. **That was wrong.** `iso_mmap_tbl` brokers
mappings *QEMU itself makes*; it is not a range restriction on an fd the
isolate holds. A file descriptor carries no range attribute -- an existing
memfd cannot be split, and arbitrary pages cannot be aliased into a fresh one,
which is precisely why the copy exists in the first place.

So zero-copy on arbitrary registered memory requires handing the isolate the
guest-RAM object and mediating **every** `mmap` and `munmap` on it --
`SECCOMP_RET_USER_NOTIF`, dealloc accounting, and killing isolates that do not
comply. That is a supervised boundary rather than a hard one, and it is the
same design as "solution 1" with the same fragilities. It is not a way around
them.

### Why the scope decision matters anyway

It reduces the no-relocate design to a single case, ordinary RAM, which makes
it tractable. With that plus one launch change the pieces already exist:

1. Boot guest RAM as `memory-backend-memfd,share=on` so it is a shareable host
   object -- **and accept that the isolate then holds that object**, with
   range control enforced only by mediating every mmap/munmap (see the
   correction above). Today `scripts/run_test_vm.sh` uses a plain `-m`, so there is
   nothing to grant -- this is the precondition, and it is a config change
   rather than a redesign. **Unverified:** that this composes with nvkvm's
   sparse window and memslot handling.
2. Grant the isolate only the GPA runs covering the registered buffer, via the
   existing `iso_mmap_tbl` broker (`iso_mmap_translate()` validates
   `(isolate_id, base, len)`; `iso_mmap_free()` revokes before munmap). The
   isolate never receives the whole guest-RAM object, which is what made the
   naive version an isolation break.
3. No copy, therefore one physical home, therefore every view stays coherent
   by construction -- fork-shared, memfd and private alike, with no test to
   get wrong. It also removes the per-registration bounce copy of every byte.

Two measurable obstacles remain:

- **Scatter.** A guest-virtual buffer is physically fragmented, so a
  registration costs one token per contiguous GPA run against a table of 8192.
  64 KiB is fine; 2 GiB may not be. The current copy buys contiguity, which is
  what the token budget was sized against.
- **Pin lifetime.** Pins must be held for the whole registration and grants
  revoked exactly on unregister, or the isolate later reads unrelated guest
  data through a stale grant. The copy gets this property for free.

### Tractable subset, worth doing first -- and it escapes the argument above

Note what makes this different: it needs **no** access to guest RAM. The
isolate already maps a per-registration host memfd today, and this keeps that
exposure exactly as it is -- no shared guest RAM, no seccomp mediation, no new
boundary.

`cuMemHostAlloc` *chooses* its memory, unlike `cuMemHostRegister`. Satisfying
it directly from the already-shared window means no relocation ever happens,
so a child inheriting the mapping across `fork()` inherits something already
host-visible and it simply works. That is a bounded guest-module change and it
covers the common pattern of allocating pinned staging buffers, without
needing (1)-(3) above.

## Confirmed: sharing a memfd between isolates already works

Checked against `nvkvm_req_mmap_on_isolate()`. The permission gate is

    if (h->session_id != req->session_id ||
        !session_has_isolate(nv, req->session_id, req->isolate_id))
            resp->status = EPERM;

which binds a handle to a **session**, not to an isolate. Naming a different
`isolate_id` of the same session passes. Memfds are first-class here --
`NVKVM_HANDLE_TYPE_MEMORY` lives in the same per-device
`VirtIONvgpu.handles` table as `TYPE_NVIDIA`, and `isolate_refcount` is
documented as "# isolates that hold this handle", plural by design. So one
pinned buffer's memfd can back several isolates **with existing commands and
no VMM change**, the same way RM objects are shared.

### This also narrows an earlier over-correction

A previous section claimed range-granting an fd "is not a thing". Too broad.
`MMAP_ON_ISOLATE` *does* bounds-check `(offset, length)` against the object --
`obj_end` from `h->size`, plus `length <= nv->sparse_size` -- so range-limited
granting exists **for a bounded object**. The objection only holds when the
object is all of guest RAM, where a check against `h->size` constrains nothing
and per-range policy would have to come from somewhere else (seccomp
user-notify). A design that keeps one memfd per pinned buffer stays in the
regime that already works; a design that hands over the whole guest-RAM object
does not.

### What is actually left

Not the VMM. The open problem is guest-side: pointing *every* view at that one
memfd means repointing the other processes' VMAs, which needs
`mmap_write_lock` on foreign mms (against the kernel's mmap_lock-outer /
i_mmap_rwsem-inner order) and replacing shmem's `vm_ops` on a VMA its owner
never asked to have converted -- otherwise the next fault there repopulates the
original page. That is the piece to prototype before calling this easy; the
sharing primitive it depends on is already present.

## Correction: the case that matters IS cross-session, so the VMM does change

The section above confirmed handles are session-scoped rather than
isolate-scoped and concluded "no VMM change". That is right for threads and
wrong for the case this document is about, because it did not check what a
session *is*. From `struct nvkvm_session`:

> Sessions are keyed by `mm` (address-space identity), not by tgid. [...]
> threads share an mm so they share a session (correct); **fork creates a new
> mm so the child gets a new session (correct).**

Parent and child sharing a buffer are therefore in *different* sessions, always.
Two consequences:

1. **`MMAP_ON_ISOLATE` refuses it.** The gate requires
   `h->session_id == req->session_id`, so granting the buffer's memfd to the
   other process's isolate returns `EPERM`.
2. **Teardown is owner-keyed, not refcounted.** `nvkvm_handle_close_session()`
   closes every handle whose `session_id` matches the dying session, so a
   shared handle dies with its creator even while another session maps it.

### Size of the change

Small in code, substantial in review. Two focused edits: an explicit
cross-session *grant* (not merely relaxing the equality test), and
refcount-based teardown instead of owner-identity teardown.

Both sit on the boundary this code is most careful about. Session keying is
`mm`-based specifically because tgid reuse was "a cross-uid info-leak path
inside one VM", and `isolate_refcount` today guards a weaker property. A
cross-session grant lets process A make pages mappable by process B's isolate,
so it needs the guest kernel to vouch that B really maps those pages, and
revocation that holds when A exits first. That is a security design task.

**Summary of scope:** nothing for threads (same session); small,
security-sensitive VMM work for the fork/multi-process case that actually
matters; and the guest-side repointing of foreign VMAs remains the larger
piece either way.

### Measured: session : isolate : guest mm are 1:1

Two comments appeared to disagree -- the guest says sessions are keyed by `mm`
(so fork makes a new one), while the host annotates `isolate_ids[256]` as
"one per guest mm", which would only make sense if a session spanned several
mms. Resolved by measurement rather than by re-reading them.

Two concurrent CUDA processes in one guest:

    2 session 1 RING MAPPED
    2 session 2 RING MAPPED
    distinct sessions seen: session 1 session 2

Two processes, two sessions. The guest calls
`nvkvm_session_get_or_create(current->mm, current->tgid)` and creates exactly
one isolate per session, so session, isolate and guest `mm` are 1:1 in
practice; the 256-slot array is headroom, not fan-out.

**Consequence:** a buffer shared between two processes is cross-session *by
construction*, never incidentally. The `h->session_id == req->session_id` gate
in `MMAP_ON_ISOLATE` therefore always blocks it, and the cross-session grant is
unavoidable for the fork/memfd case rather than being an edge case to design
around.
