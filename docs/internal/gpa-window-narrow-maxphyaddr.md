# GPA window on narrow-MAXPHYADDR hosts

> **RESOLVED 2026-08-23.** Two separate problems were tangled together in this
> document, and only one of them was about MAXPHYADDR.
>
> 1. **Window placement on a 39-bit host** — real, and fixed by the adaptive
>    base this document describes. Keep all of it.
> 2. **The guest dying under sustained use** — *not* a MAXPHYADDR problem at
>    all, and the reason this doc spent three sessions on wrong theories. An
>    NVIDIA GPU mapping is `VM_IO|VM_PFNMAP`, and `nv_fault()` returns
>    `VM_FAULT_NOPAGE` **without installing a PTE** while the driver reinstates
>    a revoked mapping. Userspace answers that by re-faulting; KVM turns it into
>    a fatal `EFAULT`. Fixed by `patches/0010-kvm-retry-a-bare-KVM_RUN-EFAULT.patch`
>    — the fault clears after **~1465 ms** if you let it. See
>    `known-limitations.md` for the full write-up.
>
> On the 39-bit laptop this document is written about, `validate.sh` now passes
> **28/28** and a weston desktop runs at 60 fps indefinitely.
>
> **Everything below is the investigation as it happened.** The root-cause
> sections in particular record theories that were later disproven — the
> `r--s`-VMA explanation, PTE-level write protection, munmap holes, memslot
> collisions and allocator races were each instrumented and cleared. They are
> kept because knowing what it is *not* is most of the value here, but do not
> read any of them as the current explanation.

## Where it stood (2026-08-18, superseded)

`fix-gpa-window-base` makes the GPA window placement adaptive. On a 39-bit host it
gets **further than main** (which cannot start at all) but was **not finished**:
the guest boots, `nvidia-smi` reports the GPU, and then the guest **hangs** on
sustained use — `nvidia-smi` blocks, guest SSH stops responding, QEMU sits at ~8%
CPU. `validate.sh` had never completed on this host at the time of writing.

That gate — "do not merge until a 39-bit host reaches 28/28" — is now met.

## Reproducer

Stock Docker container, i7-11800H (39-bit), RTX 3050 Laptop, driver 580.173.02,
`/dev/kvm` present, user namespaces blocked by the default seccomp/AppArmor profile.
Deterministic.

## Fixed on the way (keep these)

- **Window base was hardcoded at 1 TiB.** Unaddressable below 41 bits. Now derived
  from host/guest MAXPHYADDR. Symptom was
  `KVM_SET_USER_MEMORY_REGION failed, slot=6, start=0x10000000000, size=0x1000000: Invalid argument`.
- **`span <= limit` is necessary but not sufficient.** The sparse window is a
  64-bit PCI BAR and the *firmware* must find a naturally-aligned hole. MEASURED:
  a 128 GiB BAR passed every arithmetic check and SeaBIOS assigned **no BAR at all**
  to the device — every other function on the bus got one. Now capped at `limit/8`
  (39-bit -> 64 GiB; no-op above 46 bits). This removed `kvm run failed Bad address`.
- **`clone3` is blocked by Docker's default seccomp**, independently of
  `CLONE_NEWUSER`. Stub falls back to `clone(2)`.

## Ruled out — do NOT re-investigate

- **Guest does not hardcode any base.** It reads `mmap_win_gpa` / `mmap_win_len`
  from virtio config (`nvkvm_virtio.c:696-699`); per-request `gpa_base` comes over
  the wire. QEMU moving the window is communicated correctly.
- **Sizes are consistent.** `nvkvm_sparse_init` takes `win = nv->gpa.sparse_size`
  (`nvkvm_mmap_host.c:457`), the BAR is created from `gpa.sparse_size`
  (`virtio_nvgpu_pci.c:115`), and config advertises `nv->sparse_size`
  (`virtio_nvgpu.c:1056`). All three are the shrunk value. No mismatch.
- **`00:07.0` is the identity PCI device**, not the window. It legitimately has no
  BARs. Check the virtio-nvgpu function instead — BAR checks against `00:07.0` are
  measuring the wrong device.

## Headroom fix — CONFIRMED PROGRESS, hang persists

Placing the block at `limit - span` left ZERO margin at the top. MEASURED: with
`ceiling = limit - limit/8` the block moved 431 GiB -> 367 GiB and SeaBIOS then
assigned the BAR immediately:

```
00:04.0 Red Hat, Inc. Device 1072
    Region 2: Memory at 6000000000 (64-bit, prefetchable) [size=64G]
```

exactly the computed `sparse_base`. Before this it assigned NO BAR at all. The
1/8 rule also reproduces what firmware picks unaided on a wide host: on 46-bit it
computes 0x37dbc0000000 against the 0x380000000000 the firmware chose itself.

**So "BAR is not assigned" is now RULED OUT as the cause of the hang.** The window
is present, correctly sized and at the expected base, and the guest still hangs on
first sustained use: log stalls at module load, QEMU 7.6% CPU state Ssl, guest SSH
times out, GPU 0% / 33 MiB. The hang is in what happens *after* the window exists.

## Confirmed working with NVKVM_DEBUG=1

```
nvkvm_sparse_init:   64 GiB VMM buffer 0x7b28e4000000 (memslot deferred to BAR base)
nvkvm_sparse_ensure: 64 GiB at GPA=0x6000000000 slot=64
GPU 0: NVIDIA GeForce RTX 3050 Laptop GPU (UUID: GPU-69998bd6-b034-9fe1-a839-b2d779142c60)
```

So on a 39-bit host the window is now allocated, BAR-assigned, memslot-installed at
the resolved base, and `nvidia-smi -L` returns a real UUID. **Light queries work.**

## The remaining bug

Sustained use still hangs, with `error: kvm run failed Bad address` at **CPL=3** —
the fault is in guest *userspace*, not the kernel:

```
CPL=3  CR2=0x60ece2b43000  RDI=0x200224000
Code=... 48 c1 ee 20 <89> 07 ...      (faulting insn is a store, mov %eax,(%rdi))
```

Preceded by four `nvkvm: DENY ctrl cmd 0x00730102 (not in allowlist / oversize)` —
an NV0073 display-class control, believed pre-existing and benign on a headless
guest (the Blackwell bring-up saw the same DENY with no ill effect), but it has NOT
been ruled out as related.

So: the window exists and is backed, light RM queries succeed, and guest userspace
faults on a *store* into a mapping derived from it. That points at the mapping
handed to userspace rather than the memslot itself — the GPA is right, what is
mapped on top of it is not.

## CONFIRMED: it is a fault, not a deadlock

**VERIFIED 2026-08-18 by comparing QEMU thread states across the transition:**

```
BEFORE (healthy)  4x vCPU thread  wchan = kvm_vcpu_block    <- inside KVM, running guest
AFTER  (hung)     4x vCPU thread  wchan = futex_do_wait     <- left KVM, blocked on BQL
                  kvm run failed  count = 1
```

Every vCPU has left `kvm_vcpu_block` and come to rest in `futex_do_wait`. They are
neither spinning nor executing guest code: QEMU stopped the machine after the
`KVM_RUN` EFAULT and the remaining vCPUs parked on the lock. Guest SSH dies because
nothing is executing.

This also rules out a guest-kernel deadlock: that would leave the vCPUs *in*
`kvm_vcpu_block`, running guest code that never progresses. They are not there.

Not established:
- that the fault and the unreachability happened at the same moment (the fault may
  have occurred earlier and the guest hung separately)
- that the whole VM stopped. The guest has **4 vCPUs**; one returning EFAULT does
  not necessarily stop the others, and QEMU's behaviour here was assumed, not observed
- QEMU's residual 7-13% CPU is consistent with *either* other threads running or a
  vCPU spinning. It does not discriminate.

Cheap way to re-check this on any future run, no monitor socket needed:

```
ps -L -o tid,stat,pcpu,wchan:20,comm -p $(pgrep -f qemu-system-x86_64 | head -1)
```

`kvm_vcpu_block` = running the guest. `futex_do_wait` on every vCPU = VM stopped.

So: look for why KVM cannot use the host
userspace address behind a guest GPA. EFAULT from `KVM_RUN` means the GPA resolved to a
`userspace_addr` in the memslot that the kernel could not access — a hole in the
window mapping, an unmapped extent, or an extent whose `MAP_FIXED` install did not
land where the memslot claims.

The fault is a **store from CPL=3** into a window-derived mapping, so the suspect
is what `nvkvm_mmap.c` hands to guest userspace on top of the window, not the
memslot registration (which succeeds — `sparse_ensure: 64 GiB at GPA=0x6000000000
slot=64`).

### Also ruled out
- **Host overcommit / map limits.** Laptop and a working vast host are BOTH
  `vm.overcommit_memory=0`; `max_map_count` is *higher* on the laptop (1048576 vs
  65530). Not the differentiator.
- **`DENY ctrl cmd 0x00730102`** is `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`. The
  Blackwell bring-up saw the identical DENY and still scored 28/28, and a refused
  ioctl cannot produce an EFAULT on a store. Benign. (Worth *synthesizing*
  guest-side anyway — display topology is owned by `nvkvm_kms.c`, so forwarding it
  asks the host's RM about the host's real heads, which is the wrong answer by
  construction. Housekeeping, not a fix.)

## Next step

Trace where the guest blocks on first sustained window access. `nvkvm_sparse_ensure()`
(`virtio_nvgpu.c:1054`) resolves the window to the firmware-assigned BAR base on the
guest's first config read and installs the memslot there — that resolution, and
whether the BAR base it finds is sane on a shrunk/narrow layout, is the untested path.
QEMU at ~8% CPU with no guest response suggests a spin or an uncompleted forwarded
op rather than a QEMU deadlock.

Reproduce with `NVKVM_DEBUG=1` (env var read at `virtio_nvgpu.c:1116`) — that is
what surfaced the sparse_init/sparse_ensure lines above and is the fastest way in.

Faulting addresses seen: `CR2=0x5af8dad1e000` (before the BAR cap),
`CR2=0x60ece2b43000` (after both fixes). Both userspace VAs, both on a store.

## Leading theory + the fix shape (2026-08-18, end of session)

KVM does **not** validate backing at memslot registration — it checks alignment,
size and overlap only. So an HVA inside a registered memslot can become unusable
afterwards, and the guest touching it yields `KVM_RUN -> -EFAULT`, which is fatal:
unlike the MMIO path (`KVM_RUN` returns 0 with `KVM_EXIT_MMIO`, QEMU emulates and
resumes) there is nothing to resume into.

The sparse window is a 64 GiB `MAP_NORESERVE` anonymous reservation with extents
`MAP_FIXED`'d over it and a free-list recycling them
(`nvkvm_sparse_free`, `NVKVM_GPA_FREE_MAX`). **Suspect: the free path punches a hole.**
If releasing an extent `munmap`s it without restoring anonymous backing, the memslot
still claims the range while the VMA is gone.

### The fix shape

Never destroy the VMA — only empty it. On free use `madvise(MADV_DONTNEED)`, or
re-map anonymous over the extent with `MAP_FIXED|MAP_ANONYMOUS|MAP_NORESERVE`. Both
release physical pages while keeping the mapping valid, so the next guest touch
faults in a zero page normally and KVM never sees an unbacked address. `munmap` is
the wrong verb for a recycled extent inside a fixed reservation.

### DO NOT attempt userfaultfd

UFFD looks like the textbook answer here (block on first touch, populate from the
VMM, resume). **The project owner has ruled it out — it does not work in this
design. Do not spend time on it.**

### Check first
`grep -n munmap src/qemu/nvkvm_mmap_host.c` — specifically the extent-free path.

## 2026-08-18 — root cause located (laptop, RTX 3050, 39-bit MAXPHYADDR)

### Ruled out with direct evidence
- **The "munmap punches a hole in the window" theory is dead.** QEMU was patched to
  dump `/proc/self/maps` at the exact `kvm run failed` site. At fault time the
  64 GiB window has **24 VMAs and ZERO holes** — fully covered. The
  in-window-restore invariant is already implemented and holding: every teardown
  path is guarded by `kvm_slot == NVKVM_IN_WINDOW_SLOT` and re-maps anonymous
  `MAP_NORESERVE` backing instead of munmapping.
- **No hardcoded GPA/VA left in the guest module** (only a `"nvkvm"` tag constant).
- `vm.max_map_count` = 1048576 — not VMA exhaustion.

### Root cause
The window's single memslot covers **device-fd VMAs** (`/dev/nvidia0`,
`/dev/nvidiactl`) that the NVIDIA driver maps with `VM_IO|VM_PFNMAP`. KVM
resolves a memslot HVA via `get_user_pages()` / `hva_to_pfn_remapped()`. On a
VMA that lacks the required `VM_READ`/`VM_WRITE` it cannot produce a PFN and
fails `KVM_RUN` with `EFAULT` — **unrecoverable, not a resumable MMIO exit**,
so the guest dies on the access rather than getting an error back.

Fault-time snapshot (window base `0x7d9750000000`):
```
7d9750021000-7d9750031000 rw-s /dev/nvidia0
7d9750031000-7d9750032000 r--s /dev/nvidiactl   <-- READ-ONLY page under the memslot
7d9750032000-7d9750232000 rw-s /dev/nvidia0
```
The faulting instruction is a **store** (`Code=... <89> 07` = `mov %eax,(%edi)`,
CPL=3). A guest store into that read-only page → `gup(write=1)` fails → EFAULT.

We now request `PROT_READ|PROT_WRITE` for every in-window mapping, and every
neighbouring VMA from the same code path duly comes back `rw-s`. That single
page still comes back `r--s`, so **the driver's own mmap handler clears
VM_WRITE** on it. It is read-only by the driver's construction, not by our
prot argument.

### Fixed in this branch (both correct hardening, neither is the cure)
1. `nvkvm_window_restore_anon()` — all 7 in-window restore sites now go through a
   checked helper that screams if a restore ever fails (previously the `mmap`
   return value was ignored at every site, so a failure would have silently left
   a hole under a live memslot).
2. In-window mappings are created `PROT_READ|PROT_WRITE` instead of inheriting
   the guest's `req->prot`. Narrowing the host VMA bought no isolation (the
   memslot already exposes the range RW; guest PTE permissions come from the
   guest's own mapping) and was an EFAULT hazard. This did fix a write-only
   `/dev/nvidia0` VMA that was previously `-w-s`.

### Next step
A driver-imposed read-only page cannot be safely backed by the sparse memslot.
Either keep such pages out of the window (give them their own non-memslot /
MMIO-trapped path so the access is emulated and resumable), or determine why the
guest stores to it at all — it may be a *mis-mapped* object (guest expects a
writable allocation, we placed a read-only `nvidiactl` page at that GPA).
Worth checking which handle/offset produces this 4 KiB `nvidiactl` mapping.

## 2026-08-18 (later) — faulting GPA CONFIRMED, both earlier theories dead

Instrumented QEMU at the `kvm run failed` site with `cpu_synchronize_state()` +
`cpu_get_phys_page_debug()` (QEMU's own page-table walker; `KVM_TRANSLATE`
returns `valid=0` for every register in that post-fault context and is useless
here).

**Confirmed:** faulting guest VA `0x200224000` -> **GPA `0x6000056000`**
= sparse window base `0x6000000000` + `0x56000`.

At that HVA the fault-time maps snapshot shows:
```
734917032000-734917232000 rw-s 00000000 00:66 16   /dev/nvidia0
```
i.e. a **live, read-write** device VMA. 24 in-window VMAs, **zero holes**.

### Both earlier theories are now disproven
- **Not a hole / not munmap.** Zero holes at fault time (again).
- **Not narrowed VMA protection.** The `mprotect` probe fires and converts the
  two driver-readonly pages (`gpa=0x6000021000`, `gpa=0x6000031000`) to
  anonymous backing, and **the fault still happens**. The faulting page is a
  different, fully `rw-s` mapping. The r--s pages were a red herring.

### Current best explanation (PTE-level, not VMA-level)
KVM resolves a memslot HVA on a `VM_IO|VM_PFNMAP` VMA via
`hva_to_pfn_remapped()`, which enforces:
```c
if (write_fault && !pte_write(*ptep)) { r = -EFAULT; goto out; }
```
A `/dev/nvidia0` VMA can be `rw-s` while the driver has that individual PTE
installed read-only. A guest store then EFAULTs even though the VMA looks fine
and `fixup_user_fault()` cannot upgrade it. Matches all evidence: rw-s VMA,
CPL=3 store, GPA inside a live mapping, no hole.

Ruled out this round: GPU runtime power management (host GPU is D0 /
runtime_status=active / P0 at fault time).

### Separate bug found in the WINMAP log (worth fixing on its own)
GPA `0x6000021000` is handed out **twice** to two live mappings:
```
WINMAP fd=60 dev_id=0  off=0x0 len=4096   gpa=0x6000021000 req_prot=0x1
WINMAP fd=63 dev_id=16 off=0x0 len=65536  gpa=0x6000021000 req_prot=0x2
```
The 64 KiB mapping overlays the 4 KiB one at the same window offset. Whether
this is a legitimate free-then-reuse or a `nvkvm_sparse_gpa_alloc()` bug needs
checking — a 4 KiB free-list extent must not satisfy a 64 KiB request.

### Design point raised during this session (not yet implemented)
Any host VMA under the memslot whose effective permissions are narrower than
what the guest writes is a **guest-triggerable VMM abort** (QEMU treats EFAULT
as fatal). Propagating the host driver's *effective* access bits down so the
guest maps userspace no wider would turn such a write into an ordinary guest
SIGSEGV instead. Contained-by-construction, and worth doing regardless of this
bug. Note it is hardening, not a security boundary: a malicious guest kernel
module can map RW anyway, so the real boundary fix is QEMU not treating EFAULT
as fatal (it is resumable in principle -- RIP has not advanced -- the obstacle
is that a bare gup failure does not report the faulting GPA to userspace).
