# The nested-L2 zeros: it is unshared host system memory, not a copy path

**Measured 2026-08-30**, host `claude` (RTX 4070, driver 595.84), branch
`fix/nested-l2-htod-zeros` off `nested-l2` (`36a3116`). Every L2 number below has
an **L1 control taken in the same command, on the same binary, the same boot and
the same GPU**.

The parent investigation (`README.md`) recorded the symptom as *"plain
`cuMemAlloc` device memory round-trips as zeros"* and named the next step as
*"the `cuMemcpyHtoD` staging path's GPA mapping"*. **Both the framing and the
lead are wrong, and the measurements below are what say so.** HtoD is not
broken. VIDMEM is not broken.

## What is actually broken (measured)

`tests/repro/nested_pinned_sysmem.c`, L1 and L2, same binary, same run:

```
                                        L1 (control)                 L2 (nested)
[P1] CPU writes pinned, KERNEL reads    good=16384/16384  OK          good=0/16384 zero=16384   ALL-ZERO
[P2] KERNEL writes pinned, CPU reads    good=16384/16384  OK          good=0/16384 untouched=16384  UNTOUCHED
[P5] HtoD(pageable), verified BY KERNEL 1024/1024, 16384/16384 OK    1024/1024, 16384/16384  OK
```

P1 and P2 are the whole finding. A `cuMemHostAlloc` buffer at L2 is **not shared
between the guest CPU and the GPU**: the CPU's stores are invisible to a kernel
reading the same pointer, and a kernel's stores leave the CPU's view at its
original 0xEE fill. Two disjoint sets of physical pages, one per side, with
every ioctl returning success.

`tests/repro/nested_vidmem_zeros.c` shows every reported symptom falling out of
that one fact (L1 is `OK` on every line):

```
[1] HtoD(pageable) -> kernel reads D        L2: good=65536/65536  OK        <-- HtoD WORKS
[2] then DtoH(pageable)                     L2: zero=65536/65536  ALL-ZERO
[3] HtoD from PINNED host memory            L2: zero=65536/65536  ALL-ZERO
[4] DtoH into PINNED host memory            L2: untouched 0xEE    UNTOUCHED
[5] kernel writes D; kernel re-reads D      L2: good=65536/65536  OK        <-- VIDMEM WORKS
    same D, read back with DtoH             L2: zero=65536/65536  ALL-ZERO
[6] DtoH from a MANAGED source              L2: zero=65536/65536  ALL-ZERO
[7] HtoD into a MANAGED dst                 L2: good=65536/65536  OK
[8] cuMemsetD32(D, 0xDEADBEEF)              L2: kernel sees 0xdeadbeef, DtoH sees 0x00000000
```

Read together:

- **VIDMEM is fine.** A kernel reads back what HtoD wrote ([1]) and what another
  kernel wrote ([5], [8]). The GPU is computing on correct data.
- **HtoD from pageable memory is fine** ([1], [5] of `nested_pinned_sysmem`).
  The original "HtoD staging path" lead is disproven: the device receives the
  bytes.
- **Managed memory is fine in both directions** ([7], and `cuda_managed_coherence`).
- **Everything that fails involves host system memory the GPU itself must
  touch**, and only that: `cuMemHostAlloc` buffers ([3], [4], P1, P2) and
  libcuda's internal DtoH staging buffer ([2], [6], [8]).

`cuMemcpyDtoH` returning zeros is a *consequence*: the copy engine writes the
GPU-side copy of the staging buffer, the CPU then reads its own copy, which is
freshly-faulted zero pages. The `memprobe` round-trip that started this reported
`roundtrip_bad` of exactly 255/256 of the bytes at every size — the 1/256 that
"matched" are the offsets where the source pattern was itself zero. The readback
was always all zeros, never partial corruption.

## Why nesting is what breaks it

nvkvm makes host system memory GPU-visible by *aliasing one object into two
address spaces*: a memfd created by the VMM is mapped into the isolate stub at
the guest's VA (`MAP_FIXED`) so the driver's `pin_user_pages()` on the stub's
task finds pages that alias guest userspace, and the same object is mapped into
the VMM's sparse window and exposed to the guest as a memslot GPA. `src/stub/
nvkvm_stub.c` states the contract directly:

> the memfd `MAP_FIXED` aliasing is what lets `pin_user_pages()` on the stub's
> task find pages that alias guest userspace

That aliasing is two-level by construction. At L2 the chain needs three levels:
L2's guest CPU, the L1 stub, and the real driver at L0. Each level establishes
its own alias, and **nothing carries an alias established at one level down to
the next** — so the object the L2 guest CPU reaches and the object the GPU is
pinned against are different memory.

For the OS_DESCRIPTOR half of this there is a specific, identified mechanism
(**code-level, not yet confirmed by measurement — see below**).
`nvkvm_cpu_pages_migrate_range()` (`src/guest/nvkvm_mmap.c`) relocates the
caller's pages onto a fresh memfd and `remap_pfn_range()`s the window GPA over
the caller's VMA. At L2 the caller of that path inside L1 is the **L1 stub**,
and the VMA it is asked to relocate is the `MAP_FIXED` memfd mapping that L1's
QEMU created for L2 and simultaneously exposes as L2's memslot. Its eligibility
guard is:

```c
if ((vma->vm_flags & (VM_PFNMAP | VM_IO | VM_MIXEDMAP | VM_HUGETLB)) ||
    (vma->vm_file && !nvkvm_vma_file_is_memory(vma))) { ... refuse ... }
```

`nvkvm_vma_file_is_memory()` admits `TMPFS_MAGIC`, which is correct for the
`MAP_SHARED|MAP_ANONYMOUS` case it was written for — and is exactly what a
`memfd_create()` object also reports (`nvkvm_handle_open_memory()` in
`src/qemu/nvkvm_handle.c`). So the guard **admits precisely the one VMA it must
refuse**: a mapping that is already aliased into a guest's memslot. Relocating
it moves only the stub's view; L1's QEMU keeps exposing the original memfd to
L2. The two views separate, silently, with every step returning success.

## What is proven and what is not

**Proven by measurement:** at L2, host system memory shared with the GPU is not
shared at all (P1/P2); VIDMEM, managed memory and HtoD are all correct; the
zeros are a read from the other page set. The original lead is disproven.

**Not yet measured:** which of the two sysmem-aliasing mechanisms breaks first,
and the mechanism above for the migration path. `cuMemHostAlloc` produced **no**
`migrate_range` line at L2 (the only 64 KiB migration in the L2 log is the
`cuMemHostRegister` buffer of probe step P3), so that buffer travels the
RM-alloc + `mmap_on_isolate` path, not the OS_DESCRIPTOR path — and it is broken
too. Both mechanisms are implicated; only the general property is proven.

The instrument for the next measurement is committed: the DIAG commit on this
branch prints `comm`, `pid`, `file` and the superblock magic for every
`migrate_range`. Rebuild **L1's** guest module with it and run a CUDA workload at
L2. If L1 logs migrations whose `comm` is the isolate stub and whose `file=1
magic=0x1021994` (TMPFS), the mechanism above is confirmed.

## Two things that are NOT findings

- **The L2 kernel oops is not this bug and is not nesting-specific.** `NEXT.md`
  named it the prime suspect. It is a `WARNING at mm/memory.c:5514
  handle_mm_fault` with `Comm: nvz2` and `CR2: 0x5dd31fb6e000` — the exact
  address probe step P3 printed for its `cuMemHostRegister` buffer. **The same
  probe segfaults identically at L1** (`EXIT=139` at `[P3]`, on the control run),
  so it is a pre-existing `cuMemHostRegister` + kernel-deref fault, not the
  zeros. It was adjacent to the failure and it is not the cause. P3 is kept in
  the probe but runs last, so its crash cannot cost the measurements above.
- **The GPA windows are identical at both levels.** L1 and L2 both log
  `block base 0xdfdbc0000000 size 145 GiB [shm 0xdfdbc0000000, mmap
  0xdfdc00000000 +16 GiB, sparse 0xdfe000000000 +128 GiB]` and the same 48-bit
  GPA width. The windows are not the discriminator.
