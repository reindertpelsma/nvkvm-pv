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
satisfies it trivially.

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
  side of (1) — GPU VA would equal the guest VA in a private address space, with
  no collisions and no QEMU layout to probe. It loses the **CPU** side of (1):
  a KVM memslot's `userspace_addr` is resolved in the VM-owning process's mm
  (QEMU's), so the guest CPU would still be reading the sparse window's
  anonymous pages while the GPU reads UVM's. That is silent incoherence, which
  is worse than a loud failure.

An honest fourth option is to keep `main`'s loud failure and record why.

Whether the layout oracle is an acceptable price for working managed memory is a
judgement about this project's threat model, not something a measurement
settles. What the measurements do settle is that **"let the VMM pick the
address" is not a way out of that trade** — it buys nothing, because the
resulting mapping is not one the guest's kernels can address.
