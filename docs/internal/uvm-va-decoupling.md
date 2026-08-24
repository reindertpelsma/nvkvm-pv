# Can the VMM choose the host VA for a UVM managed mapping?

**No.** Measured on an RTX 5090 (GB202) with driver 580.95.05 (open modules),
2026-08-24. This page records the measurements, because the question has now
been answered wrongly twice in this tree and both wrong answers are still
quoted as authority.

**Scope.** This is about `/dev/nvidia-uvm` *managed* ranges — the ones
`uvm_mmap()` creates, which is what `cuMemAllocManaged` uses. It says nothing
about **external** ranges (`UVM_CREATE_EXTERNAL_RANGE` /
`UVM_MAP_EXTERNAL_ALLOCATION`), where a GPU VA genuinely is independent of any
CPU mapping. Conflating the two is the source of the "GPU VA need not equal CPU
VA" belief, which is true for external ranges and false for managed ones.

---

## 1. The proposal

Map `/dev/nvidia-uvm` in QEMU at a **VMM-chosen** address `H`, passing `offset
== H` so `uvm_mmap()`'s pin is satisfied, record `guest_va -> H`, and translate
`H` into every UVM ioctl that carries a VA. The attraction is real: the guest
would lose all influence over QEMU's address-space layout, which removes both
the cross-process collision and the layout oracle in one change.

The pin is not the obstacle. `uvm_mmap()` requires
`vma->vm_start == (vma->vm_pgoff << PAGE_SHIFT)`
(ogkm `kernel-open/nvidia-uvm/uvm.c:791-795`), and a VMM that picks both
satisfies it trivially. Nor can the `offset` argument be used to point the range
somewhere other than the mapping — see §2a-bis.

The obstacle is that **the GPU VA of a managed range is the CPU VA of the VMA
that created it**, and the guest's kernels dereference the *guest's* pointer.
No ioctl carries that pointer, so no ioctl translation can reach it.

## 2. The measurements

### 2a. The managed pointer *is* the CPU VA of a `/dev/nvidia-uvm` VMA

`cuMemAllocManaged` on the 5090, pointer looked up in `/proc/self/maps`:

```
MANAGED_PTR=0x7824fa000000
MAPS  vma=7824fa000000-7824fa400000 rw-s 7824fa000000 00:05 498  /dev/nvidia-uvm
VMA_START_EQ_PTR=YES
```

Note the third column: the VMA's **file offset is `7824fa000000`, equal to its
start address**. That is the pin, visible from userspace.

### 2b. The GPU VA equals the CPU VA — measured, not inferred

`cuPointerGetAttribute` on the same allocation:

```
HOST_PTR   = 0x7234a8000000
DEVICE_PTR = 0x7234a8000000   (CU_POINTER_ATTRIBUTE_DEVICE_POINTER, rc=0)
GPU_VA_EQ_CPU_VA=YES
```

This is unified addressing working as designed, and it is what the driver
source says it must be:

- `uvm_va_range_create_mmap()` allocates the managed range at the VMA's bounds:
  `uvm_va_range_alloc_managed(va_space, vma->vm_start, vma->vm_end - 1)`
  (`uvm_va_range.c:224`).
- A block inherits that: `block->start = start` (`uvm_va_block.c:1308`).
- GPU PTEs are programmed at that virtual address:
  `start = UVM_ALIGN_UP(va_block->start, page_size)` feeding
  `uvm_page_tree_get_ptes(page_tables, page_size, start, size, ...)`
  (`uvm_va_block.c:7738-7769`).

So if QEMU maps the range at `H`, the GPU has `H` in its page tables. A guest
kernel launched with the guest's pointer `G` faults at `G`, finds no
`uvm_va_range`, and takes a fatal fault. There is no ioctl in the path: `G`
travels to the GPU inside kernel launch parameters, written by the guest to a
mapped pushbuffer. **This is why translation cannot rescue the design.**

The project's own bare-metal traces already said this, in the kayfabe tree:
*"On real hardware the GPU VA **is** the process VA. That is UVM unified
addressing, working as designed"*
(`kf-w310/traces/boots/w271/RESULT.md:53-60`), and
*"under UVM unified addressing the GPU VA **is** the process VA and the process
is ASLR'd"* (`w290/RESULT.md:13-16`).

### 2c. The guest's VA cannot be steered either

`strace` of the same program shows how libcuda picks the address:

```
mmap(NULL, 469757952, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x777312001000
...
mmap(0x777312000000, 4194304, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, 9, 0x777312000000)
                                                                      = 0x777312000000
```

libcuda reserves a large **anonymous** `PROT_NONE` region — so the address is
chosen by the *kernel's* mmap allocator, under ASLR — and then `MAP_FIXED`s the
UVM fd inside it with `offset == addr`. Nothing in that sequence touches a
device node we own before the address is already decided, so a guest-side
`get_unmapped_area` hook has nothing to hook: by the time `/dev/nvidia-uvm` is
mmap'd, `MAP_FIXED` has already fixed the answer.

## 2a-ter. "But you can't just open `/dev/nvidia-uvm` and mmap it" — right, and that is the point

True, and the initialisation sequence is worth spelling out, because the thing it
allocates is *not* the thing that would need to be allocated for the address to
be choosable.

```
open("/dev/nvidia-uvm")              -> fd A, private_data = UVM_FD_UNINITIALIZED
open("/dev/nvidia-uvm")              -> fd B          (yes, twice)
ioctl(A, UVM_INITIALIZE)             -> allocates the uvm_va_space,
                                        uvm_fd_type_set(A, UVM_FD_VA_SPACE)   uvm.c:959
ioctl(B, UVM_MM_INITIALIZE, {A})     -> fget(A), require UVM_FD_VA_SPACE,
                                        uvm_fd_type_set(B, UVM_FD_MM, A)      uvm.c:67-126
                                        binds the va_space to current->mm
mmap(G, len, ..., A, G)              -> creates the managed range
```

The double open is real — the `strace` in §2c shows libcuda doing exactly this
(`openat("/dev/nvidia-uvm") = 9`, `= 10`). Before `UVM_INITIALIZE`, `uvm_mmap()`'s
switch on `uvm_fd_type()` falls to `default: return -EBADFD` (`uvm.c:769-780`) —
which is precisely the `EBADFD` nvkvm's skipped stub-mirror comment records, and
it means "this fd has no va_space yet", not "this process may not map it".

**What that sequence allocates is the `va_space`.** What it does *not* allocate
is the range. There is no RM object behind a managed range, no `hMemory`, no
handle, no allocation ioctl — nothing to name, dup, or hand to a second mapper.

The contrast inside UVM itself is the clearest way to see it. UVM has exactly two
kinds of range that *are* allocated first and mapped second, and for both **the
VA is an argument to the allocating ioctl**:

| range kind | created by | who names the VA |
|---|---|---|
| `SEMAPHORE_POOL` | `UVM_ALLOC_SEMAPHORE_POOL` (68) | the ioctl; `mmap` afterwards must use that same VA |
| external | `UVM_CREATE_EXTERNAL_RANGE` (73) + `UVM_MAP_EXTERNAL_ALLOCATION` (33) | the ioctl; backed by an RM `hMemory`, GPU VA independent of any CPU mapping |
| **managed** | **`mmap` itself** | **the `mmap` address — there is no ioctl** |

`va_range_type_expects_mmap()` returns true for exactly the first two
(`uvm.c:743-757`); the `default:` arm carries the comment *"Although
UVM_VA_RANGE_TYPE_MANAGED does support mmap, it doesn't expect mmap to be called
on a pre-existing range. mmap itself creates the managed va range."*

This is also, precisely, why U-6 needed a special case. The ownership table
records a range when a range-**creating** ioctl is accepted by the driver —
`NVKVM_UVM_VA_CREATE` on 27 / 65 / 68 / 73 (`nvkvm_isolate_handlers.c:660-682`).
Managed ranges are the one kind with no such ioctl to hook, which is why the
table never learned about them and refused every later use, and why `c8ea92d`
had to add the `uvm_va_add()` call on the mmap path instead.

So the intuition "it must be allocated, therefore its address must be nameable"
is exactly right as a rule — and managed memory is the case where the rule does
not apply. Everything in UVM whose VA you can choose, you choose in an ioctl.
Managed memory has no such ioctl, which is the same sentence as "its address is
the `mmap` address", which is the same sentence as §2b.

## 2a-bis. Does `mmap`'s `offset` argument not decouple this?

`mmap` does take an `offset`, and the obvious move is
`mmap(H, len, …, uvm_fd, G)` — place the CPU mapping at a VMM-chosen `H` while
declaring offset `G`, hoping UVM creates the range at `G`. That is the last
variant of the proposal, and it is worth killing explicitly.

**The argument is honoured, but only as a constraint.** `vm_pgoff` appears in
exactly three places in the whole UVM driver:

| site | use |
|---|---|
| `uvm.c:793-794` | the equality check `vma->vm_start != (vma->vm_pgoff << PAGE_SHIFT)` → `-EINVAL`, plus its debug print |
| `uvm.c:358-361` | `unmap_mapping_range(vma->vm_file->f_mapping, vma->vm_pgoff << PAGE_SHIFT, …)` on the disable-vma path |
| `uvm_test_file.c:99` | the `UVM_FD_TEST` device — not managed memory |

**It is never used to place the range.** `uvm_va_range_create_mmap()` reads
`vma->vm_start` and nothing else:
`uvm_va_range_alloc_managed(va_space, vma->vm_start, vma->vm_end - 1)`
(`uvm_va_range.c:224`). So `mmap(H, …, fd, G)` fails the check outright, and if
the check were patched out the range would still be created at `H` — the offset
has no path to the range's address.

This is unlike a normal file mapping, where the offset selects *which part of a
backing object* you see. A managed range has no backing object to offset into:
its pages are allocated and migrated per `uvm_va_block`, created by the mapping
itself.

The driver's own comment says why the equality is required, and the second use
above is half the reason:

> *"UVM mappings are required to set offset == VA. This simplifies things since
> we don't have to worry about address aliasing (except for fork, handled
> separately) and it makes `unmap_mapping_range` simpler."*

`unmap_mapping_range()` works on **file offsets**, and `f_mapping` is per
`struct file` — so it reaches every VMA of that fd at that offset, in every
process holding it. Pinning offset to VA is what stops two mappings of one
shared UVM fd from aliasing in `f_mapping` and tearing each other down. Note
what that implies for a design that maps one shared UVM fd twice: it would not
merely be useless (§2d), it would be actively hazardous, because an unmap on one
side unmaps the other by offset.

## 2c-bis. Cross-process sharing works. It is the number that must match, not the mm.

It is easy to over-read §2b as "the mapping and the GPU work must be in the same
process". **They must not, and nvkvm depends on their not being.** Stating it
carefully, because the distinction is the whole subject:

- **The RM VA space crosses processes, by design.**
  `UVM_REGISTER_GPU_VASPACE` takes `{rmCtrlFd, hClient, hVaSpace}`
  (`uvm_gpu.c:3831-3840`) and UVM *dupes* that RM object out of the naming
  process:
  `nvUvmInterfaceDupAddressSpace(uvm_gpu_device_handle(gpu), user_client, user_object, &gpu_va_space->duped_gpu_va_space)`
  (`uvm_va_space.c:1531-1534`). So the isolate owns the RM VA space and the
  channels; QEMU's UVM fd registers them; and the page tables UVM writes into
  (`gpu_va_space->page_tables`, `uvm_va_block.c:7730`) are **the isolate's**.
  `UVM_REGISTER_CHANNEL` bridges the same way.
- **The UVM `va_space` crosses processes too** — it is per *struct file*, so an
  fd passed by `SCM_RIGHTS` is the same `va_space` on both sides.

So a managed range created by a `mmap` in QEMU is absolutely reachable by
channels the isolate created. That is not a bug, it is the mechanism, and it is
why UVM worked at all before `b46e9c0`.

**What crosses is the handle. What does not cross is the CPU mapping — and what
reaches the GPU is a number.** UVM programs the PTEs at
`va_block->start`, i.e. at the `vm_start` of whichever process's VMA created the
range (§2b). The GPU has no idea, and no way to care, which mm that VMA lived
in; it only ever sees the address. Hence:

- QEMU maps at the guest's VA `G` → PTEs at `G` in the isolate's RM VA space →
  the guest's kernels dereference `G` → **works**. This is exactly the
  pre-`b46e9c0` design, and why it worked.
- QEMU maps at a VMM-chosen `H` → PTEs at `H` in that same isolate RM VA space →
  the guest's kernels still dereference `G` → **fatal fault**.

The failure in the second case is not "wrong address space" and it is not a
check nvkvm performs. Nothing rejects it: the mapping succeeds, the registration
succeeds, every ioctl returns `NV_OK`, and the GPU then faults on an address
nobody mapped. That is the whole difficulty — it is a numeric mismatch that no
layer is in a position to notice.

## 2d. "Can't the isolate create it and the VMM map the same object elsewhere?"

The natural next idea, and the one that would rescue the isolate-owned option in
§6: let the **isolate** create the managed range at the guest VA (GPU VA then
equals the guest VA, per-process, nothing of QEMU's to probe), and have QEMU map
*the same UVM object* at some other address purely to have a host VA to hand
KVM as a memslot.

That is exactly how the rest of nvkvm works, and it is correct **for RM memory
objects** — an `hMemory` is a shareable object, mappable in several processes at
different VAs, with a GPU VA that RM assigns independently. It does not carry
over to UVM managed ranges, because **a managed range is not an object that has
a mapping; the mapping *is* the range**:

- `uvm_va_range_managed_t` holds exactly one `uvm_vma_wrapper_t *vma_wrapper`
  (`uvm_va_range.h:288`), and a wrapper holds exactly one
  `struct vm_area_struct *vma` (`uvm_va_range.h:117-125`). One managed range can
  structurally only ever have one VMA.
- Enforced at runtime, not just by convention: `uvm_va_range_vma()` asserts
  `vma->vm_private_data == managed_range->vma_wrapper` (`uvm_va_range.h:929`).
- There is no "map this existing range" API for managed memory.
  `va_range_type_expects_mmap()` returns true only for `SEMAPHORE_POOL` and
  `DEVICE_P2P`; the comment in the `default:` arm is explicit — *"Although
  UVM_VA_RANGE_TYPE_MANAGED does support mmap, it doesn't expect mmap to be
  called on a pre-existing range. mmap itself creates the managed va range"*
  (`uvm.c:743-757`).
- And even for the two types that *do* support it, the re-mmap path requires
  `existing_range->node.start == vma->vm_start` (`uvm.c:863-867`) — the **same**
  address. UVM never lets any range be mapped at a second address.

So `mmap`ing the same UVM fd at a different VA does not alias the range. It runs
`uvm_va_range_create_mmap()` with the new `vm_start` and produces a **second,
independent, empty** managed range at that address.

Two further places the driver says the same thing, both worth knowing because
they are the cases that look like counter-examples:

- **fork** is the one situation where a managed range's pages exist in two mms.
  UVM declares it undefined and disables the child: *"Parent vma is dup'd (fork).
  This is undefined behavior in the UVM Programming Model … the child is not
  guaranteed access to the range"* (`uvm.c:396-401`), with
  `uvm_vm_open_failure()` calling `uvm_disable_vma()` on both.
- **`UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE`** sounds like the feature being
  asked for and is not. It only drops the va_space↔mm binding
  (`uvm_va_space_mm.c:176`), so several processes can create *their own* ranges
  in one shared va_space. When the process that created a range exits while
  others still hold the fd, the range becomes a **zombie with
  `vma_wrapper == NULL`** (`uvm_va_range.h:284-288`) — it is not re-homed onto a
  surviving process's VMA, because there is nothing to re-home it to.

The last step is the one that closes the option: a KVM memslot's
`userspace_addr` is resolved in the mm of the process that owns the VM
(`kvm->mm`, QEMU's — the vCPU threads are QEMU threads). A mapping that
deliberately *doesn't* belong to the VMM process therefore cannot back a
memslot. If the isolate owns the only VMA, the guest CPU has no path to those
pages at all, and it goes on reading the sparse window's anonymous pages while
the GPU reads UVM's. Silent incoherence, which is worse than the loud failure on
`main`.

## 2e. External range over an RM sysmem buffer — the one design that does decouple

Worth taking seriously, because unlike everything above it **works**, and it
dissolves the address problem rather than fighting it.

The shape: don't create a managed range at all. Instead allocate an ordinary RM
sysmem object (`hMemory`), map it to the guest through the existing sparse-window
path, and publish it to the GPU with
`UVM_CREATE_EXTERNAL_RANGE(base=G, length)` +
`UVM_MAP_EXTERNAL_ALLOCATION(hMemory, base=G)` into the host `uvm_va_space`.

Why it escapes §2a-§2d — every objection above turns on the managed range's
address being the `mmap` address. An external range's address is an **ioctl
argument**, so nvkvm names `G` explicitly:

- **No `/dev/nvidia-uvm` mmap in QEMU at all.** No guest-chosen host address, so
  no U-15 layout oracle, and nothing of QEMU's address space is involved.
- **No cross-process collision.** `G` is a GPU VA in the isolate's RM VA space
  plus a guest CPU VA; QEMU's single address space never enters.
- **The CPU side is coherent**, and by the mechanism nvkvm already uses
  everywhere else: one RM object, mapped guest-side via GPA→window→memslot and
  GPU-side by the external mapping. Same physical pages, PCIe-snooped — this is
  the `hMemory` sharing of §2c-bis, which genuinely does cross processes.
- **U-6 needs no change.** `UVM_CREATE_EXTERNAL_RANGE` (73) is already
  `NVKVM_UVM_VA_CREATE` in the schema (`nvkvm_isolate_handlers.c:660-682`), so
  the ownership table records the range exactly as designed. The special case
  `c8ea92d` had to add for managed ranges disappears.
- Sysmem is explicitly supported as external-range backing —
  `set_ext_gpu_map_location()` branches on `mem_info->sysmem`
  (`uvm_map_external.c:628-681`).

And the current failure symptom is cured: `uvm_api_validate_va_range()` looks the
range up with `uvm_va_range_find()` and checks only that the bounds match
exactly — **it does not check the range type** (`uvm_va_range.c:762-777`). An
external range at `G` satisfies it.

### The concrete sequence, and where the decoupling actually happens

Stated as a division of labour, which is the clearest form:

1. **The isolate allocates a regular RM memory object** (sysmem `hMemory`) on its
   own client.
2. **It is named to QEMU by handle, not by address.**
   `UVM_MAP_EXTERNAL_ALLOCATION_PARAMS` carries
   `{rmCtrlFd, hClient, hMemory}` (`uvm_ioctl.h:491-503`) — the same
   cross-process naming as `UVM_REGISTER_GPU_VASPACE` (§2c-bis), and a field
   nvkvm already translates (`uvm_map_ext_fd_off` in the ABI profile).
3. **QEMU publishes it to the GPU at the guest's VA `G`:**
   `UVM_CREATE_EXTERNAL_RANGE(base=G, length)` then
   `UVM_MAP_EXTERNAL_ALLOCATION(base=G, hMemory)`. The mapping lands in
   `uvm_gpu_va_space_get(va_space, mapping_gpu)->page_tables` at
   `ext_gpu_map->node.start = base` (`uvm_map_external.c:1098-1144`) — that is
   **the isolate's registered RM VA space**, precisely where the guest's kernels
   dereference.
4. **QEMU CPU-maps the same `hMemory` at any VMM VA `H`** — an ordinary
   `/dev/nvidia0` mapping through the existing sparse window — and installs the
   memslot so the guest CPU reaches it at `G` in the guest.

**That is the decoupling the original proposal wanted.** The GPU VA is an
*ioctl argument*, so it is set to `G` and matches the guest. The VMM's CPU
address `H` is unconstrained, because an external range creates **no** CPU
mapping — so there is no guest-chosen host address, no layout oracle, and no
cross-process collision in QEMU's single address space.

**One correction to the shape**, and it is the same trap as the original brief:
step 3 must register at **`G`, the guest's pointer — not at the VMM's pointer
`H`.** Registering at `H` puts the GPU mapping where the guest never looks, and
the guest's kernels fault exactly as in §2b. The two addresses are decoupled
precisely *because* only one of them has to match anything: `G` is forced by the
guest, `H` is free.

### What it costs: this is not unified memory

The ioctls libcuda drives a managed allocation with split cleanly:

| ioctl | on an external range |
|---|---|
| `UVM_VALIDATE_VA_RANGE` (72) | **works** — no type check (`uvm_va_range.c:762-777`) |
| `UVM_FREE` (34) | **works** — explicit `case UVM_VA_RANGE_TYPE_EXTERNAL` (`uvm_va_range.c:687-688`) |
| `UVM_MIGRATE` (51) | **fails** `NV_ERR_INVALID_ADDRESS` — requires `uvm_va_space_iter_managed_first()` (`uvm_migrate.c:1159-1163`) |
| `SET_PREFERRED_LOCATION` (42) / `SET_ACCESSED_BY` (46) / read-duplication (44/45) | **fail** — all iterate `uvm_va_space_iter_managed_*` (`uvm_policy.c:429`) |

So what you get is pinned host memory mapped into the GPU: the semantics of
`cudaHostAlloc` + `cudaHostGetDevicePointer`, not of `cudaMallocManaged`. No
fault-driven migration, no VRAM residency, no oversubscription,
`cudaMemPrefetchAsync` and `cudaMemAdvise` fail. For a GPU-heavy kernel that is
every access crossing PCIe instead of hitting VRAM.

### The structural catch: nvkvm would have to emulate UVM, not forward it

The larger problem is not the driver, it is who decides. **libcuda in the guest
issues the `mmap` on `/dev/nvidia-uvm`** — it is not asking nvkvm's opinion. To
substitute this scheme the guest module would have to *not* forward that mmap,
synthesize an RM allocation plus an external range behind libcuda's back, and
then keep libcuda believing it holds a managed range while it issues
managed-only ioctls at it — several of which now fail per the table above.

That is a different project from forwarding: it is *emulating* UVM's managed
allocator. Every divergence surfaces as a silently wrong or failed CUDA call, in
a subsystem where the guest's own bookkeeping and the host's have to stay in
agreement with no way to reconcile them. It is the first proposal in this
document that is not blocked by the driver — the blocker is scope and fidelity,
which is a judgement, not a measurement.

## 2f. What the driver does with an address that is a plain RM buffer, or overlaps an external range

The dispatcher that decides this is `uvm_api_range_type_check()`
(`uvm_policy.c:59-106`), and reading it answers both halves — with one result
that sharpens U-6 and one that is a genuine, non-obvious safety property.

```c
if (uvm_va_space_range_empty(va_space, base, last_address)) {          // no UVM range here
    potential_ats_range = g_uvm_global.ats.enabled && … && uvm_is_valid_vma_range(mm, base, length);
    if (potential_ats_range && …)                    return UVM_API_RANGE_TYPE_ATS;
    else if (uvm_hmm_is_enabled(va_space) && mm && uvm_is_valid_vma_range(mm, base, length))
                                                     return UVM_API_RANGE_TYPE_HMM;
    else                                             return UVM_API_RANGE_TYPE_INVALID;
}
uvm_for_each_va_range_managed_in_contig(managed_range, va_space, base, last_address)
    managed_range_last = managed_range;
if (!managed_range_last || managed_range_last->va_range.node.end < last_address)
                                                     return UVM_API_RANGE_TYPE_INVALID;
return UVM_API_RANGE_TYPE_MANAGED;
```

**A plain RM buffer mapping, with no UVM range at that address — this is worse
than U-6 documents.** The range is empty in the va_space, so the driver falls to
the pageable branch and asks `uvm_is_valid_vma_range(mm, base, length)`. That
helper (`uvm_policy.c:39-57`) walks the caller's mm with
`find_vma_intersection()` and returns true if the interval is covered by any
contiguous run of VMAs — **it checks no VMA property whatsoever**. Not
anonymous-only, not private, not "not a device mapping". QEMU's sparse window,
an RM sysmem mapping, a memslot's backing, a mapped library: every one of them
satisfies it.

So with HMM (or ATS) enabled, a guest naming such an address gets the operation
classified as pageable memory and applied to **QEMU's live RM buffer pages**.
`audit-guest-pointers.md` frames the U-6 hazard as UVM *"migrat[ing] the
CALLER'S OWN anonymous pages (i.e. QEMU's heap, the 128 GiB sparse window …)"* —
correct, but the "anonymous" framing understates it. The qualifying condition is
*any VMA*, and the most interesting targets are precisely the non-anonymous
ones. U-6's containment check still blocks all of it (an address nvkvm never
recorded is refused before the driver sees it), so this is not a live hole — but
it is a reason the check must stay unconditional, and worth stating in the terms
the driver actually uses.

**Overlapping an external range is, by contrast, safe — and is itself a guard.**
If any range covers part of the interval, `uvm_va_space_range_empty()` is false
and the **pageable branch is never reached at all**. The managed iteration then
finds nothing (an external range is not managed), `managed_range_last` stays
NULL, and the call returns `UVM_API_RANGE_TYPE_INVALID`.

That is a genuinely useful property: creating an external range over an address
*removes* it from the "empty → treat as the caller's CPU memory" path. An
external range is not merely inert here, it is protective.

**Partial overlap with a managed range is also refused**:
`managed_range_last->va_range.node.end < last_address` → `INVALID`. The interval
must be *fully* covered by managed ranges. This is exactly the rule U-6's
`uvm_va_covers()` enforces — full containment in one recorded range, deliberately
not a union — so nvkvm's check and the driver's own semantics agree, which is
the reassuring outcome for a control that was designed independently.

## 2g. Shared anonymous memory / memfd, "both declare it UVM" — this is HMM, and it is unimplemented

The most natural-sounding variant of all: the isolate creates the managed range
at the guest's `G`; the pages live in the **memfd QEMU and the isolate already
share** for GPA backing; QEMU maps that memfd at any VMM VA `H` and memslots it.
GPU faults migrate as usual; a VMM-side fault finds the page resident on the GPU
and the driver migrates it back. Each side uses its own pointer — the isolate
`G`, the VMM `H` — and nobody has to agree on a number.

It is a coherent design, and it names a real driver mechanism. It is also the
one thing in this whole investigation that NVIDIA has explicitly not built.

**"Both declare it UVM" has no API.** A managed range can only be created by
`mmap` of `/dev/nvidia-uvm` (§2a-ter): there is no call that takes an existing
anonymous or memfd VMA and makes it managed. So the range cannot live in the
shared memfd by that route at all.

**The mechanism that *does* make ordinary memory GPU-managed is HMM** — and HMM
is restricted to **private anonymous** memory. `uvm_hmm_must_use_sysmem()`
(`uvm_hmm.c:3917-3939`) is unambiguous, comment included:

```c
// TODO: Bug 3660968: Remove this hack as soon as HMM migration is implemented
// for VMAs other than anonymous private memory.
...
    // migrate_vma_setup() can't migrate VM_SPECIAL so we have to force GPU
    // remote mapping.
    // TODO: Bug 3660968: add support for file-backed migrations.
    return !vma_is_anonymous(vma) ||
           (vma->vm_flags & VM_SPECIAL) ||
           vma_is_dax(vma) ||
           is_vm_hugetlb_page(vma);
```

A memfd mapping is neither private nor anonymous, so `must_use_sysmem` is true
and the pages are **pinned in host memory with the GPU remote-mapping them over
PCIe**. The "GPU faults, UVM migrates, done" step is precisely what gets
disabled. A second refusal sits alongside it: GPU atomics on a `MAP_SHARED`
VMA report a fatal fault outright (`uvm_hmm.c:2594-2602`,
`vma->vm_flags & (VM_SHARED | VM_HUGETLB)` → `NV_ERR_NOT_SUPPORTED`).

**And the return path is why file-backed is the unimplemented case.** The
mechanism imagined for a VMM-side fault is real *for HMM*: a page migrated to
the GPU is left as a **device-private swap entry**, and a CPU touch faults into
`->migrate_to_ram`, which pulls it back. But device-private entries live in one
mm's page tables and anon rmap. The shmem page cache cannot hold one — which is
exactly what Bug 3660968 is about. So the second mapping in QEMU could never see
"the page is resident on the GPU"; it would see an ordinary shmem page that UVM
never touched.

**Finally, the two halves of the idea are mutually exclusive.** To let two
processes work against one `va_space` you would set
`UVM_INIT_FLAGS_MULTI_PROCESS_SHARING_MODE` — but that makes
`uvm_va_space_mm_enabled()` return false (`uvm_va_space_mm.c:172-180`), and
`uvm_hmm_is_enabled()` requires `uvm_va_space_mm_enabled(va_space)`
(`uvm_hmm.c:148-153`). Turning on the sharing turns off HMM. The design needs
both at once and the driver offers them only apart.

**Where it lands.** Accept the forced-sysmem outcome and you have arrived at
§2e — pinned host memory, GPU mapping it remotely over PCIe, no migration to
VRAM. The two roads converge: whichever way you avoid putting a managed range at
one agreed address, you end up with correct, coherent, non-migrating memory.
That may well be the right trade for nvkvm. It is just not unified memory, and
it is worth being clear that the migration is what is being given up.

### Does this clarify the original design?

Partly, and in a way worth recording: it explains the *intent* behind
`ARCHITECTURE.md`'s *"the stub owns the real UVM mapping in its own address
space and the GPU reaches the memory by DMA"*. That sentence describes something
close to this idea. But no implementation of it exists in the history — the
stub-side UVM mmap is skipped (`do_stub_mirror`), `c8c6024`'s own commit message
records `cuCtxCreate 800`, and both prior implementations mapped at the guest's
`req->offset` in QEMU. So it reads as a design that was reasoned about and
written down, not one that ran.

## 3. A claim in this tree that the measurements falsify

`ARCHITECTURE.md:265-268`, and the revert commit that quotes it, both rest on:

> *"libcuda picks the same UVM VA in every process, and QEMU has one address
> space — so the second process hit `EEXIST` and `cuCtxCreate` failed with 304."*

**That is not what libcuda does.** Twelve concurrent processes, three rounds of
four, all on one box:

```
round 1: 0x7b3750000000  0x728300000000  0x7bd700000000  0x7c57b8000000
round 2: 0x761f5e000000  0x7f06fc000000  0x742354000000  0x71e688000000
round 3: 0x7dab40000000  0x7592f4000000  0x723480000000  0x7f42e8000000
```

Twelve distinct addresses, spread across the ASLR'd mmap band, as §2c predicts.
The cross-process collision that `b46e9c0` was chasing on 2026-05-28 is a
*birthday collision at 2 MiB granularity over a ~14 TiB band* — rare, not
deterministic.

The `0x200000000` figure quoted elsewhere in the tree (`"isolates 8 and 10 both
create 0x200000000"`) is a `UVM_CREATE_EXTERNAL_RANGE` base — an RM-chosen GPU
VA for an **external** range. It is not a managed mmap address, and it is not
evidence about libcuda's managed VA selection.

## 4. What the history actually is

| commit | what it did |
|---|---|
| pre-`b46e9c0` | UVM mapped in QEMU **at `req->offset`** (the guest VA) — the only VMM-side UVM mapping that has ever existed in this tree |
| `c8c6024` | moved UVM ioctls **and** the mmap into QEMU. Its own commit message records the result: *"cuInit OK, cuCtxCreate now 800 (NOT_PERMITTED)"* — a step, not a working design |
| `b46e9c0` (28 May) | removed the mapping entirely to dodge the collision. UVM silently broken; nothing tested it |
| `c8ea92d` (23 Aug) | restored the mapping, again at the guest VA. Managed memory works single-process; adds a layout oracle |
| `2406a3c` (24 Aug) | reverted `c8ea92d`; honest broken state with tests that report it |

Two corrections to how this arc has been described:

- **A VMM-chosen-VA UVM mapping has never existed here.** Both prior
  implementations mapped at `req->offset`. `c8c6024` is not a working design to
  restore — check its own "Result:" line.
- **`c8c6024` did not make UVM work.** The claim that UVM worked single-process
  before 28 May is true only of the code *before* `c8c6024`, which also mapped
  at the guest VA.

## 5. The security finding, and where it stands

`c8ea92d` passed a guest VA to `mmap()` as both the host address and the file
offset (`nvkvm_isolate_handlers.c:3427,3447` on that commit). That violates the
invariant `docs/internal/audit-guest-pointers.md:35` is scoped against — *"no
guest pointer is ever forwarded to the host NVIDIA driver"* — and it hands the
guest an **address-space layout oracle**: request an address, observe mapping
versus fallback, learn whether QEMU had something there. Same primitive class as
U-7, which this project rated HIGH for exactly that reason.

`MAP_FIXED_NOREPLACE` and the `real != want` relocation check correctly prevent
*overwrite*. They do not prevent *probing*, and probing is the finding.

**As of `2406a3c` the oracle is gone**, because the mapping is gone: `main` makes
no UVM device mmap at all, so there is no guest-influenced host address and no
observable difference to probe. The invariant holds on `main` today. What `main`
does not have is working managed memory.

## 6. So what is actually available

The three properties cannot currently be had together:

1. managed memory works (guest CPU and GPU see the same pages),
2. no cross-process collision,
3. no guest influence on host address-space layout.

- **Map at the guest VA in QEMU** (`c8ea92d`): gets (1). Loses (3) outright, and
  (2) probabilistically — though §3 shows the collision is rare, not
  deterministic, so this is a far better functional trade than the revert
  assumed.
- **Map at a VMM-chosen VA in QEMU**: would get (2) and (3) — but not (1), for
  the reason in §2b. It does not work at all.
- **Map in the isolate** (per-guest-process mm): gets (2) and (3), and the GPU
  side of (1) — the isolate would map at the guest VA in a private mm, so the
  PTEs land at `G` in its own RM VA space, with no collisions and no QEMU layout
  to probe. (Note this works for the reason in §2c-bis, not against it: the mm
  is irrelevant, the number is what matters.) It loses the **CPU** side of (1):
  a KVM memslot's `userspace_addr` is resolved in the VM-owning process's mm
  (QEMU's), so the guest CPU would still be reading the sparse window's
  anonymous pages while the GPU reads UVM's. That is silent incoherence, which
  is worse than a loud failure.

- **External range over RM sysmem** (§2e): gets (2) and (3) outright, and (1) in
  the sense that CPU and GPU see the same coherent pages — at the cost of the
  memory no longer being *managed* (no migration, no oversubscription,
  `UVM_MIGRATE` and the policy ioctls fail), and of nvkvm having to emulate
  libcuda's managed allocator instead of forwarding it. The only option here not
  blocked by the driver.

An honest fifth option is to keep `main`'s loud failure and record why.

Whether the layout oracle is an acceptable price for working managed memory is a
judgement about this project's threat model, not something a measurement
settles. What the measurements do settle is that **"let the VMM pick the
address" is not a way out of that trade** — it buys nothing, because the
resulting mapping is not one the guest's kernels can address.

---

## 6b. M1 / M2 / M3 — measured, RTX 4060, driver 580.95.05, 2026-08-24

Tooling: `tools/uvm_va_probe.c` plus an `LD_PRELOAD` tracer/fault-injector whose
struct offsets and status codes come from `offsetof()` on the ogkm headers
compiled in — nothing transcribed.

### M1 — libcuda tolerates every failure an external range would produce, except one

Baseline first: a managed allocation draws 92 UVM ioctls, and the commands aimed
at the managed range are `VALIDATE_VA_RANGE`, `DISABLE_READ_DUPLICATION`,
`MIGRATE`, `SET_PREFERRED_LOCATION`, `SET_ACCESSED_BY`,
`ENABLE_READ_DUPLICATION`, `UNSET_ACCESSED_BY`, `UNSET_PREFERRED_LOCATION`.
**The kernel's own access needs no `MIGRATE` at all** — GPU faults service it
in-kernel — so migration is not on the data path; only an explicit
`cudaMemPrefetchAsync` issues one.

Then each command was made to return `NV_ERR_INVALID_ADDRESS`, one at a time —
exactly what an external range yields:

| injected cmd | `cudaMallocManaged` | data |
|---|---|---|
| 51 `MIGRATE` | ok | correct |
| 42 / 43 `SET/UNSET_PREFERRED_LOCATION` | ok | correct |
| 46 / 47 `SET/UNSET_ACCESSED_BY` | ok | correct |
| 44 `ENABLE_READ_DUPLICATION` | ok | correct |
| **45 `DISABLE_READ_DUPLICATION`** | **fails, error 1, NULL ptr** | — |

`UVM_DISABLE_READ_DUPLICATION` is issued *during* `cudaMallocManaged` and its
failure is fatal to the allocation. Failing all the others together while
letting 45 succeed gives `DATA_CORRECT`, with `cudaMemAdvise` visibly returning
`invalid argument` to the application and `cudaMemPrefetchAsync` returning
success.

**So M1 passes only conditionally**: nvkvm would have to answer cmd 45 itself
rather than forward it. That answer is *true* for an external range — there is
no read duplication on such a range, so "disabled" is the correct state — but it
is nvkvm synthesising a UVM result rather than relaying one, which is a new kind
of thing for this codebase and should be an explicit decision, not a detail.

### M2 — external ranges are the mainstream path, and are vidmem-backed

A plain `cudaMalloc` produces 24 `CREATE_EXTERNAL_RANGE` + `MAP_EXTERNAL_ALLOCATION`
pairs at bases such as `0x200000000` and `0x10000000000`. Two consequences:

- **The vidmem question is settled: yes.** External ranges routinely back VRAM;
  `cudaMalloc` is that path. The trade is therefore *static placement instead of
  adaptive migration*, not "always pinned in host memory". What is lost is UVM
  choosing placement dynamically, plus oversubscription.
  Whether QEMU can CPU-map a **vidmem** object for the memslot is a separate,
  unresolved question — device VMAs and KVM memslots interact badly
  (`nvkvm_isolate_handlers.c`, the `WINMAP` `mprotect` probe and anon fallback)
  — and needs the VM harness, not a container box.
- `0x200000000` is confirmed as a `CREATE_EXTERNAL_RANGE` base, exactly as §3
  claimed when correcting the "libcuda picks the same VA" story.

`cudaHostAlloc(Mapped)` + `cudaHostGetDevicePointer` gave `HOST_PTR ==
DEVICE_PTR` and a clean value check — sysmem coherent between CPU and GPU.

### M3 — overlap is policed, and this is the safe outcome

Probing `CREATE_EXTERNAL_RANGE` against libcuda's own `va_space` fd:

| target | result |
|---|---|
| exactly over a managed range | `NV_ERR_UVM_ADDRESS_IN_USE` |
| inside a managed range | `NV_ERR_UVM_ADDRESS_IN_USE` |
| exactly over a `cudaMalloc` GPU VA | `NV_ERR_UVM_ADDRESS_IN_USE` |
| inside a `cudaMalloc` GPU VA | `NV_ERR_UVM_ADDRESS_IN_USE` |
| fresh unused base | `NV_OK` |
| same base twice | `NV_ERR_UVM_ADDRESS_IN_USE` |

UVM's range tree is authoritative for the whole `va_space` and refuses overlap
between managed ranges, external ranges and RM-assigned GPU VAs alike. The
feared case — two owners of one GPU VA, page tables written by whoever came last
— **does not occur**.

Worth being precise about *why* RM-assigned addresses are visible to UVM: they
are registered with it. libcuda routes GPU VA allocations through
`CREATE_EXTERNAL_RANGE`/`MAP_EXTERNAL_ALLOCATION`, and channel ranges through
`REGISTER_CHANNEL` — the same commands nvkvm's schema already marks
`NVKVM_UVM_VA_CREATE`. So the arbitration holds because everything placed in a
UVM-registered GPU VA space goes through UVM.

For nvkvm's boundary this is doubly reassuring: a guest-supplied `G` that
collides with anything is refused *by the driver*, so U-6 records nothing and
the allocation degrades rather than landing on top of something. And the RM VA
space in question is the guest process's own, created by its own isolate — a
guest choosing `G` adversarially is choosing within its own GPU address space,
not a shared one.

## 6c. M4 — the backing object can be the VMM's *own* memory

The open question after M1-M3 was where the RM memory object comes from. M4
answers it, and the answer is better than the design assumed: **QEMU does not
need the guest to allocate anything, and does not need a new RM object at all —
it can register memory it already owns.**

Measured on an RTX 3060, driver 595.84 (a second driver branch, deliberately):

```
[1] private anonymous @ 0x7dae98000000
    cudaHostRegister(private anon)      -> 0    devPtr == host ptr

[2] memfd MAP_SHARED: mapping#1 @ 0x7dae9801d000  mapping#2 @ 0x7dae7c49d000
    cudaHostRegister(memfd MAP_SHARED)  -> 0
    cudaHostGetDevicePointer            -> 0    devPtr = 0x7dae9801d000
    COHERENCE via mapping#2 (different CPU VA): 0/524288 mismatched
RESULT: MEMFD_BACKING_WORKS
```

Two things matter here.

**`memfd` + `MAP_SHARED` is registerable.** That is exactly what QEMU's sparse
window is made of. Note the contrast with §2g: HMM *refuses* shared VMAs
(`uvm_hmm.c:2594-2602`) and forces sysmem for anything non-anonymous. RM's
OS-descriptor path has no such restriction — so the mechanism HMM cannot provide
is available through external ranges.

**Coherence holds through a second CPU mapping at a different address.** The
kernel's writes were made visible through `mapping#2`, which never took part in
the registration. That is precisely the split the design needs:

| who | how it reaches the pages |
|---|---|
| QEMU | registers the window extent at its own host VA, memslots it |
| the guest | the same physical pages via the GPA, at its own address `G` |
| the GPU | the external mapping |

**The exact lifecycle to replicate**, straight from the trace — three ioctls,
all already in nvkvm's schema with the right `va_mode`:

```
CREATE_EXTERNAL_RANGE    base=0x7dae9801d000 len=0x200000   (73, VA_CREATE)
MAP_EXTERNAL_ALLOCATION  base=0x7dae9801d000 len=0x200000   (33, VA_USE)
UVM_FREE                 base=0x7dae9801d000 len=0          (34, VA_FREE)
```

libcuda passes `base` = the CPU address it registered. **The one substitution
nvkvm makes is passing `base` = `G`, the guest's VA**, which is what puts the GPU
mapping where the guest's kernels dereference. `base` is an independent IN
parameter (`uvm_ioctl.h:491-503`) and is stored verbatim as the range start
(`ext_gpu_map->node.start = base`, `uvm_map_external.c:1131`), and §M3 showed
`CREATE_EXTERNAL_RANGE` accepts an arbitrary fresh base — so this is
well-supported, though binding a *given* `hMemory` at a base unrelated to its CPU
mapping is the one mechanism assumption not yet directly exercised.

**Consequence for Phase 4.** Because the backing is QEMU's own window memory,
the guest supplies *no* address at any point: it supplies a handle and a length,
QEMU picks the host VA from its window allocator, and the only guest number in
play is `G` — which is used as a **GPU** virtual address in the guest's own RM VA
space, never as a host CPU address. That is a stronger statement than the
original design could have made.

## 6d. Can the fallback be built with no QEMU change? No — and the blocker is one of ours

The proposal was: translate the UVM call **in the guest module** into an external
RM allocation via **dma-buf**, so the fallback needs no QEMU or isolate edit.
Three findings, in the order they close the question.

### 1. There is no dma-buf import path

NVIDIA's dma-buf support is **export only**. `nv_dma_buf_export()` is declared
*and implemented* (`kernel-open/common/inc/nv-dmabuf.h:29`).
`nv_dma_import_dma_buf()` is **declared and never defined** — it appears in
`nv.h:998` in both copies of the header and in no `.c` file in the open modules.
The remaining `dma_buf_attach` hits are `conftest.sh` probes for
`struct dma_buf_attachment::peer2peer`, not an import implementation.

And UVM's external path does not take an fd of any kind.
`UVM_MAP_EXTERNAL_ALLOCATION` resolves its allocation with

```c
nvUvmInterfaceDupMemory(uvmGpuDeviceHandle device,
                        NvHandle hClient, NvHandle hPhysMemory,
                        NvHandle *hDupMemory, UvmGpuMemoryInfo *pGpuMemoryInfo);
```

(`uvm_map_external.c:909`, contract at `nv_uvm_interface.h:745-767`) — RM
handles, with `NV_ERR_OBJECT_NOT_FOUND` "if the allocation is not found under the
provided client" and `NV_ERR_NOT_SUPPORTED` "if the allocation is not a physical
allocation". There is no seam where a dma-buf fd could enter.

### 2. The mechanism that *does* work is the OS-descriptor path — and it is host-only by construction

M4 (§6c) already proved the effect the proposal wants: memfd/`MAP_SHARED` pages
registered and made GPU-visible, coherent through a second CPU mapping. That
works via RM's **OS descriptor** allocation, `NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR`
— what `cudaHostRegister` issues underneath.

Its argument is a **virtual address in the caller's address space**. From this
project's own audit (`nvkvm_isolate_handlers.c:2210-2227`):

> the driver then reads `data.AllocOsDesc.descriptor` (an NvP64 inside that
> union) and hands it straight to `os_lock_user_pages()` i.e.
> `pin_user_pages()`, with `data.AllocOsDesc.limit` as the length — pinning an
> arbitrary attacker-named address range **in the isolate's address space**

So the descriptor must name **host** pages, in the **host** process. A guest
module cannot issue it meaningfully: the address it would supply is a guest
address, and forwarding one is the exact pattern U-6/U-15 exist to prevent.

### 3. nvkvm already blocks it deliberately — this is U-3

`NVOS32` is default-denied down to a single function. Allowed: `2`
(`NVOS32_FUNCTION_ALLOC_SIZE`), whose union arm contains no input pointer.
`27` (`ALLOC_OS_DESCRIPTOR`) is refused, and the audit says why: it is an
arbitrary-address pin primitive, with the driver's only checks being page
alignment and an overflow test. gVisor's nvproxy takes the same position.

**So the answer to "can the guest module do this alone" is no, twice over.**
Not merely "it lacks `hDevice`/`hSubdevice`" (it does — it tracks only
`h_client`/`h_va_space` in `nvkvm_uvm_vas_reg`, and could learn the rest by
observing RM traffic). Even with every handle it needs, a guest-issued
`ALLOC_OS_DESCRIPTOR` **is** U-3. The correct move is not to relax that control
but to keep it and have QEMU compose the call itself, over memory QEMU owns.

### What the fallback therefore requires

Host-side, and modest — but real:

- QEMU issues `ALLOC_OS_DESCRIPTOR` over a window extent **it** owns, yielding an
  `hMemory`. The guest never names an address; U-3's default-deny for
  guest-originated `NVOS32` stays exactly as it is.
- QEMU then issues `CREATE_EXTERNAL_RANGE(base=G)` + `MAP_EXTERNAL_ALLOCATION`
  (the §6c lifecycle), `G` being the guest's VA used as a **GPU** address.
- Guest-side: intercept the UVM `mmap`, record the range as external-backed, and
  answer cmd 45 locally for exactly those ranges (§M1).

An alternative backing worth weighing: rather than a window extent, QEMU could
OS-descriptor the **guest RAM pages behind `G`** — the guest module can supply
the GPA list (it already walks its own page tables, `nvkvm_mmap.c:78-104`,
`:462-488`) and QEMU owns the GPA→HVA mapping. That removes the window
allocation and the extra copy of the pages, at the cost of a new virtio message
carrying a page list. Either way the descriptor is composed host-side from
host addresses, which is the property that matters.

## 7. What would settle it: the two measurements that decide the external-range design

§2e is the only option not blocked by the driver, and its remaining risk is not
in the driver at all — it is whether **libcuda can be kept believing it holds a
managed range** while nvkvm backs it with an external range. That is empirical
and cheap to answer. Neither measurement needs a VM; both run on bare metal on
one rented box.

**M1 — the ioctl sequence libcuda actually issues against a managed range.**
Trace `cuMemAllocManaged` → kernel launch → `cuCtxSynchronize` →
`cuMemPrefetchAsync` → `cuMemFree`, recording every UVM cmd in order with its
params (the `tools/nv_ioctl_trace.c` shim, or `strace -e ioctl`). This decides
feasibility outright: §2e survives only if the commands on the **critical path**
are ones an external range answers — `VALIDATE_VA_RANGE` (72) and `FREE` (34)
both work — and the ones that fail (`MIGRATE` 51, `SET_PREFERRED_LOCATION` 42,
`SET_ACCESSED_BY` 46, read-duplication 44/45) are either not issued for a plain
allocation, or issued and tolerated. If libcuda hard-fails a `cuMemAllocManaged`
when `UVM_MIGRATE` returns `NV_ERR_INVALID_ADDRESS`, the design is dead and the
trace says so in one run.

**M2 — a positive control for the substitution itself, end to end on hardware.**
A small raw-ioctl C program: allocate an RM sysmem object; register a GPU VA
space in a UVM va_space; `UVM_CREATE_EXTERNAL_RANGE(base=B)` +
`UVM_MAP_EXTERNAL_ALLOCATION(base=B, hMemory)` at an arbitrary `B`; confirm
`UVM_VALIDATE_VA_RANGE(B)` returns `NV_OK`; then have the GPU read/write at `B`
and check the CPU sees it through a separate mapping of the same object at a
*different* CPU address. That is §2e's whole claim — GPU VA `B`, CPU VA free,
one coherent object — verified rather than inferred from source.

**If both pass**, the implementation is: guest module stops forwarding the UVM
`mmap` for managed allocations and instead requests the synthesis; QEMU allocates
the RM object via the isolate, publishes it at the guest's `G`, and CPU-maps it
wherever it likes for the memslot. **U-6 needs no change** — cmd 73 is already
`NVKVM_UVM_VA_CREATE`. Validation: `validate.sh` (30 cases) including
`cuda_managed_alloc` / `cuda_managed_coherence`, `cuda_micro` 5 and 6,
`u3_u6_gate_test` still `PASS (0 accepted)`, and the forced-collision
two-process test — which should now be uninteresting, since QEMU's address space
is no longer involved in a managed allocation at all.

**The trade to decide before building it**, because no measurement settles it:
`cudaMallocManaged` would become correct, coherent and non-migrating — the
semantics of mapped pinned host memory. Oversubscription and VRAM residency go
away, and a GPU-heavy kernel crosses PCIe on every access. For nvkvm's compute
workloads that may be entirely acceptable; it should be an explicit choice, and
`known-limitations.md` should say so plainly rather than letting users discover
it as a performance mystery.

---

## 8. Is per-stream attach structural, or is it a vacuous hint we can answer?

`UnifiedMemoryStreams` dies on `cudaStreamAttachMemAsync(..., cudaMemAttachSingle)`
(§known-limitations).  The question is whether that is a real wall or a hint
that is already unconditionally satisfied by pinned-sysmem backing.  Source
says: **it is a hint, and against this backing it is vacuous.**

### What `UVM_SET_RANGE_GROUP` actually does

`uvm_api_set_range_group()` (`uvm_range_group.c:300-360`), in order:

1. look the range group up; unknown id → `NV_ERR_OBJECT_NOT_FOUND`
2. if the group is **not** migratable, every overlapping managed range must have
   a preferred location, else `NV_ERR_INVALID_ADDRESS`
3. **require the whole `[base, last]` to be covered by managed ranges with no
   gaps** — `!managed_range_last || managed_range_last->end < last_address` →
   `NV_ERR_INVALID_ADDRESS`.  *This is the check we fail.*
4. `uvm_range_group_assign_range()` — the bookkeeping
5. **if the group is migratable, `goto done` — an explicit early exit before any
   page movement**
6. otherwise migrate pages to the preferred location

So for a migratable group — which is what a plain stream attach uses — the
entire effect is step 4. No page moves, no mapping change, no visibility change.

### Every consumer of group membership is a migration path

Tracing what the membership is *read* for:

| consumer | what it drives |
|---|---|
| `uvm_range_group_all_migratable`, `..._migratable_page_mask`, `..._address_migratable` (`uvm_va_block.c:11322-11327`, `:12833`) | whether pages **may migrate** |
| `migrated_ranges` lists (`uvm_va_block.c:4145-4148`, `:8300-8303`, `uvm_va_range.c:1696-1699`) | re-checking migration policy after a change |
| `range_map_uvm_lite_gpus`, `uvm_va_block_unmap_preferred_location_uvm_lite` (`uvm_va_range.c:1060-1061`, `:1572-1588`) | UVM-Lite preferred-location pinning |

There is no consumer that affects ordering, coherence, or fault behaviour for a
range that is permanently resident in sysmem and permanently mapped to the CPU
and to every registered GPU.  We never migrate, so every one of them is inert.

### The one place range groups carry correctness meaning is out of scope

UVM-Lite is the pre-Pascal case, and the driver defines it exactly that way:
`calc_uvm_lite_gpus_mask()` adds a GPU to the mask when
`!uvm_processor_mask_test(&va_space->faultable_processors, gpu_id)`
(`uvm_va_range.c:1628-1632`) — i.e. **non-faultable GPUs only**.

On `concurrentManagedAccess == 0` hardware, `cudaMemAttachSingle` really did
carry a correctness guarantee: it let the CPU touch a range while kernels ran.
That is live on non-faulting GPUs. **nvkvm's oldest supported architecture is
Turing** (`docs/reference/tested-platforms.md`; `ARCH_FLOOR` in
`scripts/sweep_matrix.py` starts at `turing`), which is faultable, so UVM-Lite
cannot arise on anything nvkvm supports.

And note the direction of the difference: with our backing the CPU may *always*
touch the range while kernels run, because it is ordinary pinned sysmem with a
permanent CPU mapping. Answering the hint with success is **more** permissive
than the guarantee, never less — there is no case where an application relying
on the documented semantics gets less than it asked for.

### Honest gap in the evidence

The specific ioctl behind `cudaStreamAttachMemAsync` was **inferred, not
traced**. `UVM_SET_RANGE_GROUP` fits (it is managed-only, carries a VA range at
the offset nvkvm's schema records, and returns the observed
`NV_ERR_INVALID_ADDRESS` → `CUDA_ERROR_INVALID_VALUE`), but so would
`SET_PREFERRED_LOCATION` / `SET_ACCESSED_BY` / read-duplication. The conclusion
does not depend on which: **every managed-only policy ioctl in that set is a
migration hint, and all of them are vacuous against a range that never
migrates.** One `LD_PRELOAD` trace on a box settles which, and should be run
before the fix rather than after.

### The rule this suggests, and its limit

Answer locally the ones whose failure is **fatal**; keep returning honest errors
for the ones an application survives. Cmd 45 met that bar; per-stream attach
meets it too. `cudaMemAdvise` does not — its failure is visible and harmless,
and a visible error beats a silent no-op for anything a user might need to
diagnose. Scoping stays as it is: only ranges this fd backed itself, never
blanket.

**Not yet verified**: that `UnifiedMemoryStreams` then produces *correct output*
rather than merely not crashing. Not crashing is not the bar.

## 9. Would pooling freed ranges remove the per-allocation cost?

**For allocation-heavy workloads, yes — substantially. For `UnifiedMemoryPerf`,
no, and that is the more important half of the answer.**

Per allocation the fallback currently costs 3 virtio round trips (BACK, MAP,
UNBACK) and 4+ host ioctls (`CREATE_EXTERNAL_RANGE`, `ALLOC_OS_DESCRIPTOR` and
`MAP_EXTERNAL_ALLOCATION` per chunk, `UVM_FREE`, RM free). Measured:
`cuda_micro` case 5 at **1842 µs/op** for a 1 MiB alloc+touch+free.

**What pooling could remove depends on whether libcuda reuses the VA.** This
tree already answers that: the `c8ea92d` reclaim work records that *"libcuda
reuses one address for a `cuMemAllocManaged`/`cuMemFree` loop — without it, 1
mapping was followed by 23 fallbacks at one address"*. If the VA repeats, a pool
keyed on `(gva, length)` can retain the external range, the descriptors and the
mappings intact, and a repeat allocation becomes **zero host round trips** —
the 1842 µs would collapse toward the virtio floor. If the VA does not repeat,
only the RM object create/destroy is saved (still the most expensive host part),
which is worth maybe 30-50%, not orders of magnitude.

**But `UnifiedMemoryPerf` is not allocation-bound.** It sat in `Rl` burning
8m30s of CPU in 8m30s of wall clock — a process blocked on virtio round trips
would be sleeping, not spinning. Its cost is the GPU touching sysmem across
PCIe, which pooling does not address at all. The 500× cliff there is the
*absence of migration*, and no amount of allocation caching touches it.

So pooling is worth doing for the alloc/free-loop shape and does not rescue the
workloads that repeatedly stream a managed buffer through the GPU. It should not
be counted on to change the merge calculus.

**Cost**: retained backing is retained *pinned guest RAM*, on top of a pinning
story that is already the headline limitation. It needs a bounded pool with
eviction, and the bound needs documenting next to the existing reservation note.
There is also a correctness obligation: a pooled entry must be dropped, not
reused, if a later mmap at that VA has a different length or is not a fallback
range, or a stale GPU mapping survives at an address the guest has repurposed.

---

## 10. Correction: a guest-issued OS descriptor is *not* U-3, and the fallback could have been built on it

§6d said the fallback could not be guest-module-only because a guest-issued
`ALLOC_OS_DESCRIPTOR` **is** U-3. **That was too broad, and the original premise
was right on this point.** There are two OS-descriptor routes and only one is
gated:

| route | ioctl | gated? |
|---|---|---|
| NVOS32 function 27 | `NV_ESC_RM_VID_HEAP_CONTROL` (nr `0x4a`) | **denied** — this is U-3, and is what §6d analysed |
| class `NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` (`0x71`) | `NV_ESC_RM_ALLOC_MEMORY` (nr `0x27`) | **allowed and live** — U-14's deliberate path |

The U-3 gate's own comment says so (`nvkvm_isolate_handlers.c:2742-2746`:
*"NOT affected: U-14's deliberate OS-descriptor path… Different ioctl, different
struct, untouched by this gate"*), and the code confirms it: the alloc-class
allowlist is applied under `if (nr == 0x2b)` only
(`nvkvm_isolate_handlers.c:2778`), so nr `0x27` — which *is* in the frontend NR
allowlist — carries no class check at all. The guest side is live at
`src/guest/nvkvm_ioctl.c:409-440`.

### Would `MAP_EXTERNAL_ALLOCATION` accept such an object? Yes.

`dupMemory()` gates on the **address space of the memdesc, not the class**:

```c
if (memdescGetAddressSpace(pAdjustedMemDesc) != ADDR_FBMEM &&
    memdescGetAddressSpace(pAdjustedMemDesc) != ADDR_SYSMEM &&
    memdescGetAddressSpace(pAdjustedMemDesc) != ADDR_FABRIC_MC &&
    memdescGetAddressSpace(pAdjustedMemDesc) != ADDR_FABRIC_V2)
    status = NV_ERR_NOT_SUPPORTED;
```

(`src/nvidia/src/kernel/rmapi/nv_gpu_ops.c`, `dupMemory()`), and
`osCreateMemFromOsDescriptor()` builds its memdesc with `ADDR_SYSMEM`
(`arch/nvalloc/unix/src/osmemdesc.c:215-216`, `:502-503`, `:634-635`,
`:798-799`). So an OS descriptor qualifies.

It is also already proven empirically — by the *current* design. What QEMU
allocates through NVOS32 fn 27 is the same kind of object, an OS descriptor with
an `ADDR_SYSMEM` memdesc, and `MAP_EXTERNAL_ALLOCATION` duplicated and mapped it
on real hardware. Both routes converge on the same `Memory` object; only the
issuing ioctl and the owning client differ.

**Same-client dup is the common case, not an edge.** libcuda does exactly it for
every `cudaMalloc`: the M1 trace shows 24 `CREATE_EXTERNAL_RANGE` +
`MAP_EXTERNAL_ALLOCATION` pairs using libcuda's own `hClient`.

### So why keep the current design? Cost, and a shared resource — not U-3.

**It copies every page, unconditionally.** The migration path
(`nvkvm_mmap.c`, the bulk loop) does `kmap_local_page()` + `memcpy()` into a
2 MiB shm slot and `write_memory_handle()` for **every page** of the range. For
a *fresh* managed allocation every one of those bytes is provably zero — the
guest module just allocated the pages — so the entire copy is wasted work, and
nothing in the path knows to skip it. This tree's own DIAG output measures
~20 ms per 2 MiB chunk (~100 MB/s), i.e. **~10 s per GiB**. The current design
runs the whole 4 MiB→2 GiB ladder in 1.75 s *including* a full CPU touch and
verify.

**It is bounded at 2 GiB per registration, and the bound is a shared per-VM
resource.** From the tree's own comment (`nvkvm_mmap.c:1631-1640`): *"every
chunk takes one entry in QEMU's fixed `NVKVM_ISO_MMAP_MAX = 8192` mmap-token
table … and that table is shared by every isolate in the VM. 2 GiB / 2 MiB =
1024 tokens = 1/8 of it."* So one maximal managed allocation would consume an
eighth of a VM-wide table. The current design's descriptors are per-range RM
objects, not entries in that table, and reach 4 GiB.

### What the alternative would genuinely have bought

This is not a defence of what exists — the simplifications are real: no
QEMU-side RM composition, no admin client, no GPA list and no GPA validation, no
cross-client dup, and CPU/GPU page identity by *construction* (memfd aliasing)
rather than by argument. If the copy could be skipped for freshly-allocated
pages — a "these are new, map don't copy" flag on the migration path — the
balance would likely tip the other way, at the cost of touching a path shared
with the real OS-descriptor use case.

**Standing conclusion:** the current design stays, on the grounds of copy cost
and the shared-table bound. The U-3 argument in §6d was wrong and is withdrawn.
