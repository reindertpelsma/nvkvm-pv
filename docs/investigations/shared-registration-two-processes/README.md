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
