# UVM: the three-tier plan, and why it is three

Companion to [`uvm-va-decoupling.md`](uvm-va-decoupling.md), which establishes
*why* managed memory cannot simply be forwarded. This document records the
plan that follows from it, and the reasoning behind each tier — including the
arguments that were tried and abandoned, so they are not re-proposed.

Written 2026-08-24.

## The constraint, in one paragraph

A UVM *managed* range is created only by `mmap`ing `/dev/nvidia-uvm`, and its
GPU VA **is** the CPU VA of the creating VMA (`uvm_va_range_alloc_managed(
va_space, vma->vm_start, …)`). `uvm_mmap()` additionally pins `addr == offset`.
One `vma_wrapper` exists per managed range (`uvm_va_range.h:288`, asserted
`:929`), so a range cannot live in two `mm`s. QEMU has a single address space
for every guest process, so two guest processes that pick the same managed
address collide in the VMM. gVisor's nvproxy hit the identical wall and
documents it rather than solving it.

## Tier 1 — fallback: correct always, slow sometimes

**Status: built and hardware-validated** (branch `uvm-fallback-guest-side`).

The guest module downgrades *managed* to *external*: it allocates
`NV01_MEMORY_SYSTEM` on a private client/device tree, arms a CPU mapping with
`NV_ESC_RM_MAP_MEMORY`, and publishes the same object at `G` with
`CREATE_EXTERNAL_RANGE` + `MAP_EXTERNAL_ALLOCATION`. **QEMU never sees a
managed allocation**, and every ioctl involved already existed and was already
gated — zero lines of QEMU change.

What it is, plainly: **pinned host memory mapped to the GPU** —
`cudaHostAlloc` + `cudaHostGetDevicePointer` semantics. Not migrating, not
oversubscribing. See `known-limitations.md` for the full list.

This tier exists so that managed-memory programs are **correct rather than
flaky**. Accepting a slow path beats accepting an unreliable one.

## Tier 2 — the R region: fast for ~99% of programs, no host changes

**Status: designed, not built. This is the main path for most users** — see
"Why upstream will not happen" below.

QEMU reserves a fixed region `R`, one KVM memslot, sized around GPU VRAM
rather than larger (ASLR is a security policy worth keeping), holds sparse
`PROT_NONE` over everything outside it, and **publishes only R's bounds**.

The guest module hooks `open("/dev/nvidia-uvm")` and from then on steers that
process's anonymous mappings into `R` (overriding `get_unmapped_area` for that
`mm`), falling through to the normal allocator when `R` cannot satisfy — which
degrades to Tier 1 rather than failing.

Because the collisions being prevented are *cross-process*, the guest module —
the only component that sees every process's UVM activity — arbitrates `R` with
two registration states:

- **potential** — an anonymous mapping holds this range; it may become UVM;
  **reclaimable**. Needs a defined trigger (VMA teardown, process exit) or `R`
  fills and everything silently degrades: a leak that presents as "UVM got
  slower over time".
- **definitive** — a live UVM VA; never hand it to another guest process.

Dispatch: a pointer in `R` and unique takes the fast path with **real
migration**; anything outside `R`, or a duplicate, takes Tier 1.

Two things make this sound rather than merely clever:

- **The fallback makes address coordination a performance optimisation, not a
  correctness requirement**, so best-effort steering is acceptable.
- **libcuda expresses no preference.** Measured: it issues
  `mmap(NULL, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS)` and lets the kernel's
  allocator choose, then `mmap(addr, …, MAP_FIXED, uvm_fd, addr)`. So the guest
  module is *advising* an address nobody had an opinion about.

A non-UVM `MAP_FIXED` by guest userspace inside `R` is harmless: guest
processes have separate address spaces and only UVM VAs propagate to the VMM.

### Rejected: inverse reservation

An earlier form had the *guest* reserve QEMU's occupied set. **This discloses
the VMM's address-space layout to an untrusted guest** — the same class as U-7
and U-15, and worse than what it fixes. QEMU reserving `R` exclusively and
publishing only its bounds has nothing to probe.

## Tier 3 — patch the host OGKM: true semantics, permanent maintenance

**Status: not scoped. A documented escape hatch, not the supported path.**

### What is actually needed

Not a general decoupling of CPU VA from GPU VA. The guest's identity must be
*preserved* — libcuda runs in the guest, does its usual
`mmap(NULL)`/`mmap(MAP_FIXED)` dance, and sees CPU VA == GPU VA == `G`.
**There is no host-side libcuda**: the host is QEMU and the stub issuing raw
ioctls plus an admin RM client for `GET_PID_INFO`; the present path uses EGL
for graphics, not UVM. So nothing on the host holds a managed pointer.

What is needed is that **QEMU can map the same memory at a different VA than
the process that owns the UVM range**. OGKM *requires* the equality; nvkvm
requires only that it not bind the VMM's mapping. The patch is permissive.

### Three variants, smallest-first is the right instinct

1. **Decouple GPU VA from the creating VMA.** Largest. Touches the range
   allocator, fault handling, HMM integration, the one-wrapper invariant — and
   **every teardown path that assumes the identity**, which is the least
   exercised and most dangerous code to get subtly wrong.
2. **Relax one-`vma_wrapper`-per-range** so a secondary mapping may exist in
   QEMU's `mm`, refcounting the range against two VMAs. Keeps the identity for
   the primary mapping in the isolate, so the allocator, blocks, faults and
   destroy paths keep their invariant.
3. **Back the managed range's CPU pages with a memfd**, so QEMU's mapping is an
   ordinary memfd map and the `vma_wrapper` question does not arise.

### The open question that sizes variant 3

Migration is **swap-shaped and kernel-level**, not a userspace fault handler:
HMM's form puts a device-private swap entry in the CPU PTE, and a touch goes
through `do_swap_page()` → `pgmap->ops->migrate_to_ram()` — the same path a
disk swap-in takes, with VRAM as the device.

So the question is not userspace-vs-kernel. It is **whether that machinery
works on shmem pages rather than anonymous ones**:

- UVM's *managed* path does not use HMM. It owns its VMA and `vm_ops->fault`
  and allocates its own CPU chunks — so if pages came from a memfd, UVM would
  have to handle faults on a VMA it does not own.
- UVM's *HMM* path does use device-private entries, but
  `uvm_hmm_must_use_sysmem()` restricts it to private anonymous VMAs, with an
  NVIDIA TODO referencing Bug 3660968 — which reads as a known gap rather than
  a fundamental limit.

**This is the single question that decides memfd versus `vma_wrapper`**, and it
is answerable from OGKM and the kernel's migration code. Answer it before
scheduling Tier 3.

## Why upstream will not happen, and what follows

A narrow, additive patch might in principle land in OGKM. It will not.

**Evidence, not prior:** gVisor hit this exact constraint. A Google-scale
project, nvproxy documents `cudaMallocManaged` as flaky
(`g3doc/user_guide/gpu.md:250-255`, issue #11436) and warns at boot
(`runsc/boot/loader.go:614-618`) rather than having got it fixed. The incentive
also runs the wrong way: virtualising consumer GPUs competes with vGPU, a paid
product.

So Tier 3 means **users apply patches, permanently**. Three consequences:

1. **Tier 2 is the main path, not a stopgap.** Most users will never patch a
   host GPU driver — it means rebuilding the driver, DKMS conflicts,
   secure-boot signing, and distro updates silently clobbering it. Do not gate
   Tier 2 on Tier 3.
2. **Patch size is the dominant criterion, not elegance.** A patch rebased four
   times a year is sustainable; one touching fault handling is not, because
   each NVIDIA release risks a semantic conflict that must be re-reasoned
   rather than re-applied. Optimise for *few hunks against functions that do
   not churn*.
3. **Packaging is part of the design.** Ship it DKMS-friendly, fail loudly
   rather than silently reverting on a driver update, and make "is this host
   running a patched driver?" checkable at runtime — that check belongs in
   `validate.sh`. A marginally cleaner patch that leaves users guessing why
   managed memory stopped working after `apt upgrade` is worth less.

## Summary

| tier | host changes | semantics | status |
|---|---|---|---|
| 1 — fallback | none | pinned sysmem; no migration, no oversubscription | built, validated |
| 2 — R region | none | real migration for ~99% incl. multi-process | designed |
| 3 — OGKM patch | patched driver | full managed memory; enables nested nvkvm | not scoped |
