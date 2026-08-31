# The nested-L2 zeros, confirmed on second hardware

**Measured 2026-08-30**, host: RTX 3050 Laptop GPU, **Intel** i7-11800H,
**39-bit** MAXPHYADDR, driver 580.173.02 (proprietary), `kvm_intel.nested=Y`,
tree `c7aa4bd`, QEMU rebuilt from that tree (11.1.1).

This is a *different machine on every axis* from the box where the bug was
found (RTX 4070, AMD, 46-bit, driver 595.84 open module). Both the L1 control
and the L2 run were taken on this one machine, same binary, same boot.

## Result: reproduces byte-identically

|  | L1 (control) | L2 (nested) |
|---|---|---|
| `[P1]` GPU's view of pinned sysmem | `good=16384/16384` | `zero=16384` **ALL-ZERO** |
| `[P2]` CPU's view after GPU wrote | `good=16384/16384` | `untouched=16384` **0xEE** |
| `[P5]` HtoD, verified by kernel | correct | **correct** |

Identical to the RTX 4070 signature. **The fault is not architecture-,
vendor-, driver-flavour- or address-width-specific.** It is structural.

`[P5]` passing at L2 again confirms the earlier retraction: HtoD is not broken,
and "cuMemAlloc returns zeros" was never the right description. `[P3]`
(`cuMemHostRegister`) segfaults at **both** L1 and L2 here, independently
confirming that crash is incidental rather than causal.

## Mechanism: measured, not inferred

`migrate_range` was instrumented (commit `9ec1d44`) to print the migrating
task, the GVA, and the backing VMA's `s_magic`. `evidence/l1_migrate_range_rtx3050.txt`
holds the raw lines. Twelve migrations, two clearly separated populations:

**L1 control** — six migrations, `comm=nps` (the probe process itself):

    comm=nps pid=4803 gva=0x7c7128600000 file=1 magic=0x1021994

`0x7c7128600000` is exactly the `cuMemHostAlloc host=` the L1 run printed. The
probe's own pinned buffer is migrated, and P1/P2 pass. Migration is not
inherently broken.

**L2 run** — six migrations, `comm=memfd:nvkvm_stu` (the per-VM isolate stubs,
six distinct pids):

    comm=memfd:nvkvm_stu pid=7630 gva=0x7c1a03021000 file=1 magic=0x1021994

`0x1021994` is `TMPFS_MAGIC`, which is **also what `memfd_create` reports**.
So at L2 the thing being relocated is the *VMM's memfd* -- L2's guest RAM --
not the probe's buffer. `migrate_range`'s guard admits it because it cannot
distinguish a memfd already aliased into a guest memslot from ordinary tmpfs.

Relocating that region moves the stub's view only; the L2 guest's own mapping,
and the GPU's, still point at the original pages. That is the two-disjoint-page-
sets symptom, arrived at from the other direction.

**Measured:** the L2-side migrations are file-backed `TMPFS_MAGIC` VMAs
belonging to the stub, and the L1-side ones are the probe's own buffer.
**Inferred (consistent with, not proven by, these lines):** that the guest and
GPU mappings are the ones left behind. Proving that last step wants the L2
guest's PFN for the buffer before and after a migration.

## Consequence for the fix

Tightening the `migrate_range` guard is now clearly *necessary* but the shape
matters: `TMPFS_MAGIC` alone cannot separate "ordinary tmpfs page worth
migrating" from "memfd already aliased into a memslot". The guard needs to test
aliasing, not filesystem type. The L1 control shows a naive tightening would
regress the working case, since L1's legitimate migrations carry the same
magic.

## The guard, and what it costs

`nvkvm_vma_file_is_memory()` now tests `IS_PRIVATE(inode)` as well as the
superblock magic. Measured at both levels, one machine, one boot:

| population | caller | `priv` | n |
|---|---|---|---|
| L1, libcuda's own pinned buffer | `comm=nps` | **1** | 5 |
| L1, plain anonymous (no vm_file) | `comm=nps` | −1 | 1 |
| L2, stub's aliased guest memfd | `comm=memfd:nvkvm_stu` | **0** | 6 |

No overlap, so the guard rejects exactly the aliased case.

**Verified after the change.** L1 control unchanged: `16384/16384` both
directions, HtoD correct at every size. L2 now logs

    nvkvm: refusing to migrate 0x707ebb621000-0x707ebb821000: the VMA is not
    plain anonymous memory (vm_flags=0x80000fb file=1)
    nvkvm: migrate_range(bulk) failed ret=-22 at chunk 0

and CUDA fails honestly instead of returning zeros.

### The trade-off is real and is a judgement call

| | L2 score | CUDA at L2 | silent corruption | L1 panic risk |
|---|---|---|---|---|
| without the guard | **26P / 4F / 0S** | mostly works | **yes, 4 checks** | yes (async #PF) |
| with the guard | **22P / 1F / 7S** | `cuCtxCreate` rc=304 | none | none observed |

The guard costs *all* nested CUDA: `cuCtxCreate` needs the OS_DESCRIPTOR
registration this path serves, so every CUDA check downstream skips. The
headline nested number goes from 26/30 to 22/30 and that is not cosmetic.

What survives at L2 is more than the number suggests: **Vulkan compute still
dispatches and verifies on-GPU** (`vk_compute_dispatch`, 4096 elements checked),
EGL and OpenGL still render and read back correctly. Nested GPU compute is
therefore still available at L2 -- just not through CUDA's pinned-sysmem path.

**Recommendation: keep the guard.** Silent numerical corruption with every
ioctl returning success is the worst failure mode available, it also panicked
the outer guest, and nesting is an experiment rather than a shipped feature --
so the honest -EINVAL costs nothing real today. Reverting is one line if that
judgement is wrong.

The way to get 26/30 *and* correctness is the allocation change, not the guard:
back the registration from a DMA-able window allocated once at the bottom
level, instead of re-aliasing a memfd per level.

### What this is NOT, and where the guard over-rejects

**This is not "NVIDIA DMA does not work on memfd-backed memory."** It does --
on the stock driver and here. The driver pins whatever backs the VMA and does
not care which filesystem it came from.

The fault is in nvkvm's **relocation**, a step that only exists because the
guest's pages have to become visible to the host stub. `migrate_range` copies
the pages into a shared memfd and repoints the guest VMA at it with
`VM_PFNMAP`. That is sound only for a VMA with **exactly one view**. A VMM's
guest-RAM memfd has two -- the stub's mapping and the KVM memslot backing the
inner guest -- so moving one desynchronises the other. `TMPFS_MAGIC` was a
proxy for "ordinary process memory", and memfd breaks the proxy by being shmem
*and* shared.

**Known over-rejection.** `S_PRIVATE` rejects every memfd, not only aliased
ones. A single-view case -- `memfd_create` + `mmap` + `cuMemHostRegister`
inside an ordinary (non-nested) guest -- was safe before and now returns
`-EINVAL`. The property that actually matters, "is this already aliased into a
memslot", is not observable from the guest module, so `S_PRIVATE` stands in for
it. That is a deliberate trade: refuse a narrow working case rather than keep
silently corrupting the nested one. It is untested whether any real workload
hits it; if one does, the fix is not to widen the magic check again but to make
aliasing itself detectable.
