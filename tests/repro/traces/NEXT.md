# Nested L2 pinned-sysmem zeros — what's still missing

Status at 2026-08-30 (PC powered off mid-investigation).

## Established

At L2, `cuMemHostAlloc` pinned sysmem is **not shared**: the CPU and the GPU
resolve it to two disjoint sets of physical pages.

- P1: CPU writes the buffer, GPU reads zeros.
- P2: GPU writes 0xEE, CPU still sees the buffer untouched.

Every previously-reported symptom follows from that one fact, including the
"cuMemAlloc returns zeros" framing this started as. HtoD is *not* broken --
it appeared to work because the GPU genuinely received data; the DtoH read
came back from the other page set. L1 is byte-exact throughout, so this is
specific to the second nesting level.

The guest module logs a **kernel oops immediately after a 16-page migration**
(64 KiB -- exactly the probe's buffer) in the path that relocates guest anon
pages into the shared window. That is the prime suspect and is very likely
the same bug, not a second one.

## The one thing still uncaptured

The full oops trace from the **L2 guest's** dmesg. Host dmesg has no matching
lines; the oops is inside L2. Note `dmesg` is restricted in the guests -- it
needs `sudo`, which is why the first capture attempt returned only
"Operation not permitted" and the second returned only an SSH timeout.

    /root/nested-l2/l1.sh "./l2.sh \"sudo dmesg | grep -iE 'nvkvm|BUG:|Oops|Call Trace|RIP:|migrat' | tail -80\""

Both levels must be booted first, which takes several minutes.

## Why the trace decides it

The working hypothesis is that at L2 the "shared window" is itself L1
guest-physical, so pages already migrated once get migrated a second time,
and the second relocation is what splits the CPU's view from the GPU's. If
the trace lands in the migration path, the fix belongs in the **guest
module**, not QEMU. Until the trace exists this is a strong hypothesis and
should not be written up as a root cause.

## Repro sources (this box, uncommitted)

`tests/repro/nested_vidmem_zeros.c`, `nested_pinned_sysmem.c`,
`nested_pinned_vma.c` on branch `fix/nested-l2-htod-zeros`.
