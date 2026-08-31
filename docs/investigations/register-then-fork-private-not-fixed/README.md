# `fork_mapping_semantics.c` fails: register-then-fork MAP_PRIVATE is not fixed

**Recorded 2026-08-31, branch `fix/cow-private-registration`.** Repro:
`tests/repro/fork_mapping_semantics.c`. This test FAILS today. It is
committed as a record of a known, real gap — not wired into
`tests/validate.sh`, and must not be until it actually passes.

## What this test checks, and why it is not the same test as `cow_after_fork.c`

Both tests are about migrate_range's COW handling (commit `79b6e45`,
"guest/mmap: keep registered private memory PRIVATE as well as writable"),
but they exercise it in the two different orders that order can happen in,
and `79b6e45` only fixed one of them:

| | order | `cow_after_fork.c` | `fork_mapping_semantics.c` |
|---|---|---|---|
| shape | fork, **then** register | register, **then** fork |
| who registers | the CHILD, on its inherited COW heap | the PARENT, before any fork exists |
| result | **PASS** | **FAIL** |

`cow_after_fork.c` is fork-then-register: a heap buffer exists before CUDA is
touched, the process forks, and the CHILD registers the buffer it inherited
COW. That is exactly the shape `79b6e45` fixed and its own commit message
describes correctly (see the correction note below for what that message
originally overclaimed).

`fork_mapping_semantics.c` is register-then-fork: the PARENT registers a
`MAP_PRIVATE|MAP_ANONYMOUS` buffer BEFORE forking at all, then forks, and
checks whether the CHILD's inherited view stays private (per the contract:
`MAP_PRIVATE` — each side sees only its own writes) or has become shared.

## Why it fails: VM_PFNMAP PTEs are copied verbatim at fork

`79b6e45`'s fix operates entirely on VMA *flags* and page *protection*: for a
COW mapping it clears `VM_MAYWRITE` (so `remap_pfn_range()` accepts the
sub-VMA remap) and sets `vm_page_prot` explicitly to a writable protection
*without* setting `VM_SHARED` on the VMA — so the VMA's own flags still read
"private". That is sufficient for the fork-then-register shape, because
nothing forks again after the retype.

It is not sufficient for register-then-fork, because once the retype has
happened, the VMA is `VM_PFNMAP` with `remap_pfn_range()`-installed *special*
PTEs pointing at the window GPA. Linux copies `VM_PFNMAP` mappings' PTEs
**directly** at `fork()` — there is no `struct page` to apply copy-on-write
semantics to, so there is nothing IS_COW-shaped about a PFN mapping at all;
the child simply gets the identical PFN mapping the parent has. `79b6e45`'s
"stay private in the flags" trick changes what `is_cow_mapping()` reports and
what protection bits get installed, but neither of those governs whether
`fork()`'s `copy_page_range()` treats a `VM_PFNMAP` VMA as one it must
special-case for privacy — it does not, by design, because `VM_PFNMAP`
memory is not normally forked at all in application code that expects
per-process privacy.

Net effect: register a `MAP_PRIVATE` buffer, then fork, and the child's write
is visible to the parent — the mapping became, in practice, indistinguishable
from `MAP_SHARED` the moment migration retyped it, regardless of what its
`vm_flags` say.

## Confirmed as pre-existing, not a regression

Verified with a control build: `tests/repro/fork_mapping_semantics.c` fails
**identically** on bare commit `79b6e45` (before any of the object-keyed
sharing work in this branch), with the exact same
`=> WRONG: expected private, observed shared` output on its MAP_PRIVATE case.
This is not something introduced by the object-keyed sharing fix; it is a gap
`79b6e45` left standing, mislabeled by that commit's own message (see next
section).

## `79b6e45`'s commit message overclaimed

That commit's message did not distinguish the two orderings and read as
though it fixed the general "does registration preserve a MAP_PRIVATE
mapping's fork contract" question. It does not: it fixes the mapping staying
**private in its VMA flags while being writable** (the SIGSEGV bug
`private_register_write.c` catches), for the fork-then-register order that
`cow_after_fork.c` exercises. It says nothing about, and does not fix,
what happens when a fork occurs *after* the VMA has already been retyped.
A follow-up commit on this branch corrects the record without touching
`79b6e45`'s code.

## Status

Not fixed. Recorded here so it is not carried only in memory. Do not wire
`tests/repro/fork_mapping_semantics.c` into `tests/validate.sh` until it
actually passes — a red validate.sh run must always mean a regression, never
this known, already-understood gap.

Any future fix here needs a different mechanism than flags/protection
tricks — something that intervenes at fork() time itself for a `VM_PFNMAP`
VMA created by this migration path (e.g. detecting "this VM_PFNMAP range came
from an nvkvm COW migration" in a fork hook and re-splitting it per-process),
which is a materially bigger change than `79b6e45`'s and out of scope for the
object-keyed sharing fix this branch otherwise delivers.
