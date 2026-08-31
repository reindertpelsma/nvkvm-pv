# Two processes registering the same shared buffer

**Measured 2026-08-31.** Repro: `tests/repro/fork_both_register.c`. This test
FAILS today. It is committed as a record of a real gap, not as a passing check,
and is deliberately NOT wired into `tests/validate.sh` yet — see "status".

## The shape

A process holds two buffers and has not touched CUDA:

    A = MAP_PRIVATE|MAP_ANONYMOUS    (ordinary heap)
    B = MAP_SHARED |MAP_ANONYMOUS

It forks. **Both** processes then bring CUDA up independently and register
**both** buffers for DMA at the same virtual addresses. The child writes to
both. The parent reads.

## The contract, defined by the stock NVIDIA driver

    parent: cuda=1  registerA=1 registerB=1
    child : cuda=1  registerA=1 registerB=1
    parent sees child's write:  A(private)=0/16384   B(shared)=16384/16384
    VERDICT: PASS

Private stays private, shared stays shared, all four registrations succeed.

## What nvkvm does

    parent: cuda=1  registerA=1 registerB=1     <- parent's registration SUCCEEDED
    child : cuda=1  registerA=1 registerB=0     <- child's was REFUSED
    parent sees child's write:  A(private)=0/16384   B(shared)=0/16384
    VERDICT: FAIL

`A` is correct. `B` is not: it silently stops being shared.

    nvkvm: refusing to migrate 0x7778af2bc000-0x7778af2cc000: page 0 is mapped
           2 times -- the range is shared with another mapping, and relocating
           it would desynchronise them

## The finding: the mapcount guard is ONE-SIDED

The guard refused the **child** and let the **parent** through. So the parent's
migration completed — repointing its view at a fresh memfd — while the page was
still shared, and the child never migrated. The two views are desynced anyway,
which is exactly what the guard exists to prevent.

`page_mapcount() > 1` is therefore **necessary but not sufficient**. It stops a
*later* registrant from stranding an *earlier* view. It does not stop the
*first* registrant from stranding everyone else. Whoever registers first wins.

This is a weaker outcome than the corruption it replaced — the memory is not
wrong, the sharing is simply gone — but it is still a silent deviation from the
stock driver.

## Why the session gate is not the cause

Worth recording, because it looks like a candidate and is not.
`nvkvm_cpu_pages_migrate_range()` passes the SAME value to both calls:

    nvkvm_virtio_open_memory_handle(ctx->session->id, ...)      /* sets h->session_id */
    nvkvm_virtio_mmap_on_isolate(..., ctx->session->id, ...)    /* sets req->session_id */

So `h->session_id == req->session_id` always holds on this path, in every
process. The `h->session_id != req->session_id` clause in
`nvkvm_req_mmap_on_isolate()` is a structural no-op here and cannot fire.
Removing it does not affect this scenario.

## Where the fix has to go

The child should not migrate its own copy at all. Its pages were already
migrated by the parent, so the correct behaviour is to map the **existing
handle** into the child's isolate — one memfd, two isolates, both VMAs pointing
at the same window region, coherent by construction.

That requires two things:

1. The guest module must recognise that a range's underlying object has already
   been migrated, and by which handle. The child's VMA still points at the
   original shmem pages, so this cannot be answered from the VMA alone — it
   needs a registry keyed on the backing object.
2. `MMAP_ON_ISOLATE` must accept a handle minted by another session, since the
   two processes are separate sessions. That is where dropping the session gate
   becomes necessary and meaningful — not for the migrate path above, but for
   this deliberate share.

Related: the guest keys sessions by `mm` while the VMM is built for a session
holding one isolate per `mm` (`isolate_ids[256]`, "one per guest mm";
`isolate_refcount`, "# isolates that hold this handle"). Correcting that scope
may make (2) unnecessary.

## Status

Not fixed. `tests/validate.sh` stays at 34 passing checks so that a red sweep
still means a regression rather than this known gap. Wire this test into the
suite as check 35 when the fix lands.

## RESOLVED — 2026-08-31, branch `fix/cow-private-registration`

`tests/repro/fork_both_register.c` now PASSES, reliably (60+ consecutive runs,
including under 8-way CPU stress — see "Measurement" below). The fix is
object-keyed sharing, exactly as sketched in "Where the fix has to go" above,
plus two things that sketch did not anticipate and had to be found by testing
against the real driver rather than reasoned out in advance.

### What shipped

1. **Registry, keyed by (inode, pgoff, len)** — `nvkvm_shared_add()` /
   `nvkvm_shared_find()` in `src/guest/nvkvm_mmap.c`. When a chunk is
   successfully migrated and the source VMA is file-backed shmem
   (`nvkvm_vma_file_is_memory()`), its handle is recorded against the backing
   object, not the GVA. A later registration of the same object shares the
   existing handle (`nvkvm_cpu_pages_share_range()`) instead of relocating
   again — no pin, no gup, no mapcount check, because nothing is being
   relocated.
2. **The session gate removal predicted above** — `nvkvm_req_mmap_on_isolate()`
   no longer requires `h->session_id == req->session_id`; `session_has_isolate()`
   stays (see `src/qemu/nvkvm_isolate_handlers.c`). This is what lets the
   sharer's own session map a handle it did not create into its own isolate.
3. **A race the sketch did not anticipate**: two ALREADY-RUNNING processes
   calling `migrate_range()` for the same object with no ordering between
   them can both probe the registry before either has finished — both see
   nothing and both fall into the relocate path together, reproducing the
   exact desync this whole mechanism exists to close, just delayed rather
   than removed. Fixed with a per-inode pending-claim list
   (`nvkvm_shared_resolve()`): the first caller for an object with no
   completed registry entry publishes a claim; a second caller for the same
   inode waits on it (`wait_for_completion`, no locks held across the sleep)
   and re-decides once it resolves — share if the claimant succeeded, or
   become the new claimant if it failed.
4. **A second, non-obvious wrinkle in the mapcount guard itself**, found only
   by running the real test repeatedly against the real driver: see below.

### Why `page_mapcount() > 1` needed a threshold change, and why it is still safe

The one-sidedness this doc originally recorded (parent sails through at
mapcount 1, child is refused at mapcount 2) is not the only way the guard's
baseline of "1" breaks down. Even with the claim/wait serialization above,
BOTH siblings' single attempts were measured to fail sometimes, fully
sequentially (one claims, fails, releases; then, separately, the other claims
and fails too) — not a timing race between the two ioctl calls at all.

**The actual mechanism, confirmed by direct measurement, not assumption:**
`get_user_pages_fast(FOLL_WRITE)` in the pin loop is an ordinary write fault.
If the calling process had no PTE for the page yet, that gup call
*permanently* installs one — exactly as an ordinary write through the pointer
would — regardless of whether this migrate_range() attempt goes on to succeed
or fail. `err_unpin`'s `put_page()` only drops the extra pin gup itself took;
it does not and cannot unmap the page-table entry the underlying fault
installed. So a sibling that fails once leaves a real, ordinary PTE behind,
and every later attempt on the same object — by anyone — sees it. Combined
with `fork_both_register.c`'s own setup (the parent writes the buffer BEFORE
forking, so it always already holds a PTE the child does not inherit — Linux
does not eagerly copy PTEs for `MAP_SHARED|MAP_ANONYMOUS` at fork, confirmed
separately via `/proc/self/pagemap`), whichever sibling's own gup runs while
the other's PTE is still resident sees `mapcount == 2`, deterministically, not
probabilistically.

An earlier draft of this fix's code comments blamed CUDA's own bring-up
(`cuInit`/`cuCtxCreate`) for faulting a sibling's PTE in ahead of time. That
was directly disproved with a probe that checks `/proc/self/pagemap`'s present
bit after each of `cuInit`/`cuDeviceGet`/`cuCtxCreate`/`registerA`, on a page
it never otherwise touches: the bit stays 0 throughout. CUDA bring-up does not
touch unrelated host memory. Recorded here so the wrong explanation is not
rediscovered.

**The fix**: `mc_allowed` is 1 by default, or `shared_claim->refs` (read fresh
under `nvkvm_shared_lock`, not cached) when a claim is held — i.e. "1 (self)
plus however many OTHER processes are, RIGHT NOW, verifiably blocked inside
our own `nvkvm_shared_resolve()` for this exact object." Why this is safe, not
just permissive, answered precisely because a wrong CREDIT here (not a wrong
retry) is the actual danger:

- **What proves a credited reference is a genuine co-registrant, not a third
  party**: the only way `refs` is incremented is a waiter's OWN live VMA over
  this exact object resolving to a matching `inode` in
  `nvkvm_shared_find_pending_locked()`. There is no path to being counted that
  does not go through a real `migrate_range()` ioctl call on a VMA that
  genuinely maps the object. An attacker who could reach that path already has
  ordinary OS-level, POSIX `MAP_SHARED` access to the same bytes — this guard
  is about GPU-view coherence, not confidentiality, so that access is not a
  new capability.
- **Can a credited waiter walk away without reconciling?** No —
  `nvkvm_shared_resolve()`'s post-wait `continue` unconditionally re-enters its
  decision loop; the only two outcomes for a woken waiter are SHARE (repoint
  its own VMA) or become the new claimant (subject to this same guard again).
  This is enforced by control flow, not by trusting the calling process.
- **What if a credited waiter is killed mid-wait?** `wait_for_completion()`
  (the non-interruptible variant) does not observe signals, so `SIGKILL` to a
  waiter does not take effect until its claim resolves and it runs the
  re-decide above. Verified live with an adversarial probe
  (`tests/repro/handle_outlives_creator_session.c`'s sibling investigation —
  see "Measurement" below): a waiter `SIGKILL`ed while its `/proc` state read
  `D` stayed `D` (alive) for the full duration of the claimant's registration
  and only exited, `WIFSIGNALED`, after the claimant finished. There is no
  window where a dying credited waiter's contribution is real but its
  reconciliation is skipped.

Net: `refs` cannot be inflated by anything that is not itself bound to go
through this same protocol, so crediting it never lets a genuinely uninvolved
view — e.g. `shared_view_desync.c`'s child, which never calls
`migrate_range()` at all and so is never counted — get relocated past.
Confirmed by that test still refusing, unchanged, across repeated runs both
before and after this change.

### The bounded retry, and its measured necessity

`refs`-crediting only helps when the two siblings' calls temporally overlap.
Since they frequently do not (measured via dmesg: fully sequential,
non-overlapping claim/release pairs, both refused, before the retry existed),
the mapcount check retries a bounded number of times (25 × 2ms) with the write
lock dropped across the sleep, giving a genuinely cooperating sibling that
has not yet reached its own call time to do so and become a counted waiter.

**Measured 2026-08-31, 8-vCPU guest, `stress-ng --cpu 8` running throughout a
30-run batch (plus a 30-run idle batch for comparison):** zero hard refusals
in either batch. Of ~270 mapcount checks per batch, ~95-97% needed 0 retries
(crediting alone already covered them); under stress the remainder needed up
to 10 retries (worst observed), well inside the 25-attempt budget. On
exhaustion this fails CLOSED (`-EINVAL`, reported with the retry count via
`pr_warn_ratelimited`) — refusing a legitimate registration under extreme load
is an acceptable outcome here; relocating past an unaccounted view is not, and
nothing in this design trades one for the other.

### Status

Fixed. `tests/validate.sh` still validates its original 34 checks (this repro
lives in `tests/repro/`, run separately — see the CI/branch notes for how it
is exercised). `fork_both_register.c` is the gate this fix targets and passes
it; `shared_view_desync.c` (the deliberately-must-still-refuse control) and
`tests/repro/private_register_write.c` were re-verified unaffected.
