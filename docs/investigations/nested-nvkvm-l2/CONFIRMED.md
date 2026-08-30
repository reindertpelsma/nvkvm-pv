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
