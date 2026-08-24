# nvkvm architecture

This document explains how a CUDA call inside a KVM guest reaches a real NVIDIA
GPU that the host has not given up, and how the five problems that stop most
attempts at this are actually solved. Every mechanism claim cites `file:line` so
you can check it.

If you only read one section, read [The mmap problem](#problem-1-the-mmap-problem)
and [The data plane](#problem-5-the-data-plane) — between them they explain why
this is fast rather than merely functional.

**Contents**

- [The shape](#the-shape)
- [The request path, end to end](#the-request-path-end-to-end)
- [Problem 1: the mmap problem](#problem-1-the-mmap-problem)
- [Problem 2: nested guest pointers](#problem-2-nested-guest-pointers)
- [Problem 3: ABI versioning](#problem-3-abi-versioning)
- [Problem 4: the isolation model](#problem-4-the-isolation-model)
- [Problem 5: the data plane](#problem-5-the-data-plane)
- [Lineage](#lineage)

---

## The shape

There are four processes and one kernel module.

```
 ┌─ GUEST (KVM) ──────────────────────┐   ┌─ HOST ──────────────────────────────┐
 │                                    │   │                                     │
 │  libcuda / PyTorch / Vulkan / EGL   │   │   QEMU                              │
 │      │ ioctl(/dev/nvidia*)          │   │   ├── virtio-nvgpu device           │
 │      ▼                              │   │   │   handle table, allowlists,     │
 │  nvkvm-guest.ko                     │   │   │   GPA windows, KVM memslots     │
 │   ├─ size lookup                    │   │   │                                 │
 │   ├─ copy params from user          │   │   └── one isolate per guest process │
 │   ├─ stage secondary buf in aux slot │  │       (nvkvm_stub, freestanding,    │
 │   ├─ zero guest VAs (cooperative;   │   │        rootless, seccomp'd)         │
 │   │   NOT a security control)       │   │                                     │
 │   └─ virtio TX ────────────────────────────►    │ ioctl()                     │
 │                                     │   │       ▼                             │
 │  /dev/nvidiactl  /dev/nvidia0       │   │   NVIDIA driver ──► GPU             │
 │  /dev/nvidia-uvm /dev/nvidia-uvm-tools  │        ▲                            │
 │  /dev/nvidia-modeset  /dev/dri/renderD128│       │ host apps keep using it     │
 └────────────────────────────────────┘   └─────────────────────────────────────┘
```

- **`src/guest/`** (7.6 kLoC) — a GPL kernel module that registers the NVIDIA
  device nodes, intercepts their ioctls, sanitises the parameter blobs, and
  forwards them over virtio. It also registers a real DRM device so the Vulkan
  ICD will bind, and a virtual KMS head.
- **`src/qemu/`** (11 kLoC) — a virtio device built into QEMU. It owns the
  handle table, every allowlist, the guest-physical address windows, the KVM
  memory slots, and the isolate processes.
- **`src/stub/`** (2.8 kLoC) — the isolate. A freestanding static binary
  (`-nostdlib -static -fPIE`) embedded in QEMU and launched from a memfd. One
  per guest process. It holds the real device fds and runs the real ioctls.
- **`src/common/`, `src/abi/`** — wire protocol, ABI profile table, NVIDIA
  struct definitions.

The virtio device is `virtio-nvgpu-pci` (virtio type 50, PCI id `0x1072`,
`src/qemu/virtio_nvgpu.h:79`, `src/qemu/virtio_nvgpu_pci.c:133`). Three
virtqueues: TX (guest→host requests), RX (responses), EVT (async events)
(`src/common/nvkvm_proto.h:74-77`).

A second, identity-only PCI device `nvkvm-gpu` exists solely so the guest's DRM
render node has an NVIDIA-vendor sysfs parent — the Vulkan ICD refuses to bind
otherwise, and the virtio transport must keep vendor `0x1AF4` or the guest's
virtio-pci driver won't bind. It has no BARs, no MMIO, no DMA, and reads its
vendor/device/subsystem ids from the host GPU's sysfs
(`src/qemu/virtio_nvgpu.c:1323-1407`).

---

## The request path, end to end

Take a single `ioctl(fd, NV_ESC_RM_CONTROL, &params)` from `libcuda` in the
guest and follow it.

```
 guest userspace          guest kernel module            QEMU                    isolate stub          host kernel
 ───────────────          ───────────────────            ────                    ────────────          ───────────
 ioctl(fd, cmd, p) ──────► nvkvm_ioctl()
                           │
                        1. size = nvkvm_ioctl_param_size(cmd)
                           copy_from_user(params_buf, p, size)
                           │
                        2. secondary buffer? copy it into aux_buf
                           (nvos54.params → aux)
                           │
                        3. nvkvm_sanitize_ioctl_params()
                             every guest VA field := 0
                             every guest fd field := handle_id
                           │
                        4. shm slot alloc; memcpy params+aux into
                           the shared-memory window
                           │
                        5. virtio TX: NVKVM_REQ_IOCTL_ON_ISOLATE ─────► nvkvm_tx_handler
                                                                        │
                                                                     6. slot_blob() bounds-checks
                                                                        slot vs size
                                                                        │
                                                                     7. thread-pool offload
                                                                        │
                                                                     8. allowlists:
                                                                        UVM schema / graphics /
                                                                        ioctl type / DRM nr /
                                                                        NVKMS cmdType /
                                                                        frontend nr / alloc class /
                                                                        ctrl cmd / hClient set
                                                                        │
                                                                     9. ISOLATE_CMD_IOCTL over
                                                                        SOCK_SEQPACKET (+ abi_profile) ──► worker thread
                                                                                                           │
                                                                                                       10. param/aux recv'd into
                                                                                                           PRIVATE mmap'd blobs
                                                                                                           │
                                                                                                       11. OVERWRITE nvos54.params
                                                                                                           with &aux_buf (a stub VA)
                                                                                                           translate embedded fds
                                                                                                           handle_id → local fd
                                                                                                           │
                                                                                                       12. ioctl(fd, cmd, param) ──► NVIDIA
                                                                                                           │                        driver
                                                                                                       13. zero the pointer again,
                                                                                                           restore handle_ids,
                                                                                                           extract nvstatus
                                                                                                           │
                                                                    14. ◄─────────────────────────────────┘ ISOLATE_RESP_IOCTL
                                                                        reader thread matches txn_id
                                                                        │
                        15. ◄──────────────────────── virtio RX response (retval, nvstatus, fault_addr)
                           │
                        16. EFAULT + fault_addr? resolve the mapping, retry once (bounded)
                           │
                        17. copy params_buf + aux_buf back to userspace
                           │
 ◄───────────────────────  return
```

Three things about that path are worth naming now, because they are what the
rest of this document is about.

**Step 3 is a convenience, not a control; step 11 is the boundary.** The guest
zeroes guest VAs so the cooperative path works, but the guest is untrusted and a
malicious one simply skips step 3 — so step 3 can never be counted as
protection. Only the host-side overwrite in step 11 is a security control, and
its coverage today is partial and per-ioctl rather than categorical. See
[Problem 2](#problem-2-nested-guest-pointers) for exactly what it does and does
not cover.

**Step 12 runs in a process, not in QEMU.** The isolate has the guest process's
address-space layout mirrored and its own RM client. See
[Problem 4](#problem-4-the-isolation-model).

**This path is not on the hot path.** Once a CUDA channel exists, kernel
launches do not traverse any of it. See [Problem 5](#problem-5-the-data-plane).

---

## Problem 1: the mmap problem

### Why it is the hard part

gVisor's own nvproxy design document names this as the difficult piece of
proxying NVIDIA ioctls, and it is worth being precise about why.

`NV_ESC_RM_MAP_MEMORY` does not create a mapping. It *prepares* one: the driver
records, against a file descriptor, that a subsequent `mmap()` at a particular
offset should be satisfied by particular GPU pages. The `mmap()` that consumes
that preparation happens later, on a **different fd**, and may come from a
different process entirely. A proxy that only sees ioctls sees half a
transaction. A proxy that also intercepts `mmap` still has the problem that the
pages it would need to hand out are host pages, and the caller is inside a VM.

This is where the two other public non-vendor attempts stop.
`straylight-software/isospin-microvm` states it plainly — mmap not implemented,
control path only — and therefore cannot run CUDA at all, because CUDA cannot
allocate a context without mapping the channel's control pages.

### What nvkvm does

The mapping is established in three places at once: a host `mmap` of the real
device fd, a KVM memory slot making those exact physical pages visible at a
guest-physical address, and a guest `remap_pfn_range` installing that GPA into
the calling process's VMA. No copy anywhere in the chain.

The guest-side invariant is stated at the top of the file
(`src/guest/nvkvm_mmap.c:12-16`):

> The critical invariant: the pages mapped into the guest VA must be the same
> physical pages the NVIDIA driver mapped on the host. No copy, no bounce
> buffer. This means GPU BAR pages (VRAM, doorbells, command rings) are directly
> accessible from guest userspace — essential for zero-copy GPU compute paths.

**Guest side** — `nvkvm_mmap_request_isolate()`,
`src/guest/nvkvm_mmap.c:144-248`. When guest userspace `mmap()`s a
`/dev/nvidia*` fd, the module:

1. sends `NVKVM_REQ_MMAP_ON_ISOLATE` carrying the guest VA, the fd offset, the
   length and the prot (`:159-164`);
2. **validates the GPA the host returned against the advertised window**
   (`:168-172`, `nvkvm_gpa_in_mmap_window()` at `:746-760`) — a range check plus
   an overflow check;
3. sets `VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP` (`:174`);
4. picks cacheability by device class (below);
5. calls `remap_pfn_range(vma, vma->vm_start, gpa_base >> PAGE_SHIFT, ...)`
   (`:193-195`).

After step 5, guest userspace holds page-table entries pointing at real GPU BAR
pages. The guest kernel module is out of the picture for every subsequent
access.

**Host side** — `nvkvm_req_mmap_on_isolate()`,
`src/qemu/nvkvm_isolate_handlers.c:2263-2478`. QEMU allocates a GPA from a
pre-reserved window and `MAP_FIXED`s the real device fd over that window's
backing:

```c
qva = mmap(target, len, req->prot,
           MAP_SHARED | MAP_FIXED, h->fd, (off_t)req->offset);
```
— `src/qemu/nvkvm_isolate_handlers.c:2332-2333`

and then does *no KVM ioctl at all*, because one memslot already covers the
whole window (`:1852-1854`).

### The sparse window, and why it exists

The naive design — one KVM memory slot per mapping — does not survive contact
with CUDA:

> A single `cuCtxCreate` issues >1500 tiny (4 KB) device mmaps; one memslot each
> blows past both our pool and any sane slot count. By `MAP_FIXED`'ing the device
> fd into the sparse window's VA range we reuse the one memslot
> `nvkvm_sparse_init()` already installed — zero per-mmap KVM ioctls.
> Sparse/holey device regions inside one big prereserved KVM memory region are
> fully supported by KVM (per-page gup on fault).
>
> — `src/qemu/nvkvm_isolate_handlers.c:2300-2311`

So: at device realize, QEMU reserves 128 GiB of anonymous `MAP_NORESERVE`
address space (`src/qemu/nvkvm_mmap_host.c:146-149`) and installs **one**
`KVM_SET_USER_MEMORY_REGION` covering the whole thing
(`src/qemu/nvkvm_mmap_host.c:209-210`). Individual device mappings are then
`MAP_FIXED` slices carved out of that region. Teardown restores the anonymous
backing rather than `munmap`ing, because a `munmap` would punch a hole in the
window's single VMA (`src/qemu/nvkvm_isolate_handlers.c:2412-2420`).

The window's guest-physical base is not a constant. QEMU registers a 128 GiB
prefetchable 64-bit MMIO BAR with no backing, purely so guest firmware
*assigns and reserves* the range, then installs the raw memslot at the
firmware-assigned address (`src/qemu/virtio_nvgpu_pci.c:29-42`,
`src/qemu/nvkvm_mmap_host.c:184-224`). A RAM-backed BAR was tried and rejected:
its QEMU-listener-managed memslot collided with the raw one and broke `cuInit`
(`src/qemu/virtio_nvgpu_pci.c:30-34`).

`/dev/nvidia-uvm` is the exception. Its kernel `mmap` handler requires
`vm_start == (vm_pgoff << PAGE_SHIFT)`, so it cannot be `MAP_FIXED` at an
address that differs from the offset. It gets a GPA from the window riding the
window's anonymous backing, with no QEMU-side device mmap at all
(`src/qemu/nvkvm_isolate_handlers.c:3249-3279`). The earlier design that
`MAP_FIXED`'d UVM at `req->offset` in QEMU's address space collided across
concurrent processes, so the second process hit `EEXIST` and `cuCtxCreate`
failed with 304.

Three corrections to that paragraph, all measured on an RTX 5090 / 580.95.05 on
2026-08-24 — see [UVM VA decoupling](docs/internal/uvm-va-decoupling.md):

- ~~*the stub owns the real UVM mapping in its own address space and the GPU
  reaches the memory by DMA*~~ — **it does not.** `mmap_on_isolate` skips the
  stub mirror for UVM (`do_stub_mirror = hd->dev_id != NVKVM_DEV_UVM`), so
  since `b46e9c0` (28 May 2026) *nobody* maps `/dev/nvidia-uvm`. This sentence
  documents the regression as though it were the design, which is why managed
  memory was broken for three months with no test able to see it.
- ~~*`libcuda` picks the same UVM VA in every process*~~ — **it does not.**
  libcuda reserves the range with an anonymous `PROT_NONE` `mmap(NULL, …)`, so
  the address is ASLR'd. Twelve concurrent processes produced twelve distinct
  addresses. The collision is a rare birthday collision, not a certainty.
- *"cannot be `MAP_FIXED` anywhere QEMU chooses"* is too strong as a statement
  about the **pin** — QEMU controls both address and offset. But QEMU still
  cannot choose the address, for a different and stronger reason: the GPU VA of
  a managed range **is** the CPU VA of the VMA that created it (measured:
  `cuPointerGetAttribute` returns the host pointer unchanged), and the guest's
  kernels dereference the guest's pointer. A VMM-chosen address produces a
  mapping the guest cannot address.

### Cacheability is not a detail

This is the part that turns a working mapping into a fast one, and getting it
wrong fails in two opposite directions. The rule
(`src/guest/nvkvm_mmap.c:175-191`):

> Cacheability by device class. `nvidiactl`/`nvidia-uvm` mmaps are pinned SYSTEM
> memory (the host backs them as WB RAM memslots; smaps shows no `VM_IO`) — they
> must be WB-cached, or the CPU reads them uncached. The channel completion
> semaphore lives here; mapping it write-combining made `cuCtxSynchronize`'s poll
> an uncached read (~3us vs ~0.36us host, 8.9x), serializing decode and starving
> the GPU (0-5% util). On x86 the guest-WB / host-WB / GPU-DMA views of the same
> memfd are cache-coherent (DMA snoops), so WB is correct. `nvidia0`/DRM/modeset
> mmaps are real BAR/MMIO (`VM_IO`) and MUST stay write-combining — a WB mapping
> of the doorbell BAR would leave the ring store in cache and never reach the
> device (decode would hang).

Both halves of the GPU launch path are in that paragraph: the **doorbell** (BAR
MMIO, must be write-combining or the store never leaves the CPU) and the
**completion semaphore** (system memory, must be write-back or the poll costs
8.9x).

Requesting write-back is necessary but not sufficient. Because the window is a
prefetchable PCI BAR — a non-System-RAM region — x86's
`track_pfn_remap()`/`reserve_pfn_range()` silently downgrades the requested WB
pgprot to `UC-`. On Intel this was invisible: KVM's EPT sets the IPAT bit and
forces WB regardless of the guest PTE. On AMD, where NPT honours the guest PTE
memtype, the `UC-` sticks:

> CPU access to these cache-coherent, memfd-backed window pages runs ~100x slow
> (measured 0.12 GB/s vs 14 GB/s on an EPYC 7K62 / RTX 3060).
>
> — `src/guest/nvkvm_mmap.c:44-61`

So the module walks the freshly created leaf PTEs and clears `PCD`/`PWT`/`PAT`
by hand (`nvkvm_force_range_wb()`, `src/guest/nvkvm_mmap.c:68-109`), handling
`pud_large`/`pmd_large` as well as 4K leaves. No TLB flush is needed because the
range was just mapped inside the `mmap`/`ioctl` syscall and userspace cannot
have touched it yet (`:63-66`).

### The other direction: `OS_DESCRIPTOR`

`NV_ESC_RM_ALLOC_MEMORY` with `hClass = NV01_MEMORY_SYSTEM_OS_DESCRIPTOR`
(0x71) asks the driver to pin **the calling task's** user pages. In this
forwarding model the calling task is the stub, not `libcuda` — so the kernel
would pin stub-owned anonymous memory and the GPU would DMA against pages
unrelated to anything the guest touches.

The fix runs in the guest sanitiser (`src/guest/nvkvm_ioctl.c:379-412`):

> migrate every guest page in `[p_memory, p_memory+limit+1)` onto memfds shared
> with the stub via the existing CPU-page migration path. The stub already maps
> those memfds at the same VA (`MAP_FIXED`), so the kernel's `pin_user_pages`
> call finds tmpfs pages that alias libcuda's guest userspace — GPU DMA, libcuda
> memcpy, and host kernel all touch the same physical pages.

`p_memory` is deliberately left unchanged so the kernel sees the same VA the
stub has mapped.

`nvkvm_cpu_pages_migrate_range()` (`src/guest/nvkvm_mmap.c:823-1138`) does this
in 2 MiB chunks — one memfd, one batched upload, one `mmap_on_isolate` and one
`remap_pfn_range` per chunk — with the chunk size chosen for a specific reason
(`src/guest/nvkvm_mmap.c:771-803`):

> Each chunk = ONE memfd + ONE `mmap_on_isolate` + ONE `remap_pfn_range` (vs the
> old per-4KB-page path: ~5 forwarded round-trips EACH — measured 2.44s for a
> 16MB OS_DESCRIPTOR). 2MB amortizes the ~4 fixed per-chunk forwards over 512
> pages (negligible).

Migration is strictly **per chunk**: copy the chunk into its memfd, swap that
chunk's PTEs onto the memfd's GPA, release that chunk's pinned guest pages, and
only then start the next chunk (`src/guest/nvkvm_mmap.c:948-1117`). That
ordering is what bounds the duplicated data — the window in which one chunk
exists both in the pinned guest pages and in its memfd is exactly one chunk,
2 MiB, whether the caller registers 16 MiB or 2 GiB. The memfd itself is *not*
transient: it is the backing store the guest VMA points at for the lifetime of
the registration, so total memfd bytes necessarily equal the registered size.

The single VMA conversion happens once, up front, before any chunk moves
(`src/guest/nvkvm_mmap.c:890-945`): set `VM_PFNMAP | VM_IO | VM_DONTEXPAND |
VM_DONTDUMP`, and clear `VM_MAYWRITE` on a copy-on-write mapping —
`remap_pfn_range()` refuses any sub-VMA remap of a COW mapping unless it covers
the whole VMA exactly, which is what ordinary `malloc()`/`MAP_PRIVATE` memory
is, so before that fix every multi-chunk registration failed `-EINVAL` and
surfaced as `CUDA_ERROR_INVALID_VALUE` (`src/guest/nvkvm_mmap.c:907-925`). Same
cacheability lesson as above, recorded again with numbers: mapping this range
uncached made the guest's post-DtoH read "a stream of uncached, unprefetched
loads — measured 0.07 GB/s vs 9.6 GB/s on the host (130x)"
(`src/guest/nvkvm_mmap.c:926-944`).

A single call is capped at 2 GiB (`NVKVM_MIG_MAX_RANGE`,
`src/guest/nvkvm_mmap.c:806-821`) — a guest-side policy limit derived from
QEMU's fixed 8192-entry mmap-token table, not a hardware one. This replaced an
earlier 16 MiB `-E2BIG` check that was a side effect of batching every chunk's
memfd before the VMA swap; see
[known limitations](docs/internal/known-limitations.md#pinned-host-memory).

### Demand faults

If an ioctl still dereferences a VA the stub does not have mapped, the stub's
`SIGSEGV` handler records the faulting address and the response carries it back
(`fault_addr`, `src/common/nvkvm_proto.h:441`). The guest then resolves it —
re-sending `MMAP_ON_ISOLATE` for a GPU VMA, or migrating the page for a CPU VMA
— and retries with `NVKVM_IOCTL_FL_RETRY_EFAULT` set, up to 128 times
(`nvkvm_efault_resolve()`, `src/guest/nvkvm_mmap.c:566-615`; retry loop at
`src/guest/nvkvm_main.c:2017-2034`). Each forwarded ioctl also carries a VMA
whitelist — every VMA in `current->mm` as `{start, end, prot}`, capped at 1024
entries, staged in a third shared-memory slot
(`src/guest/nvkvm_virtio.c:1176-1231`) — which is the isolate's authority for
what a demand fault may legitimately map.

---

## Problem 2: nested guest pointers

### Why it is hard

Many `RM_CONTROL` commands carry a parameter struct that itself contains further
userspace pointers. `NVOS54_PARAMETERS.params` points at command-specific inner
params; some of *those* contain pointers again (an `InfoList` array, three
version-string buffers, two channel-handle arrays).

A VMM cannot dereference a guest userspace pointer. It is a virtual address in
an address space QEMU does not have, and even if it did, the value is entirely
attacker-controlled.

`nestrilabs/virtio-nvgpu` solves this with a generated V1→V2 command-rewrite
table: a per-command description of where the pointers are, produced ahead of
time. nvkvm does something different.

### What nvkvm does: aux-slot bounce plus boundary-side overwrite

The secondary buffer never travels as a pointer. It travels as **data**, in a
second shared-memory slot ("the aux slot"), and the pointer field is
reconstructed at the far end pointing at a buffer the far end owns.

Three steps, in three different address spaces:

Before the steps: **only step 2 is a security control.** Step 1 runs in the
guest kernel module, which is inside the trust boundary's *untrusted* side. It
exists so the cooperative path works — the host cannot reconstruct a secondary
buffer it was never sent — and a malicious guest is free to skip it entirely.
Every claim about what nvkvm guarantees rests on step 2 alone. Read step 1 as
"how the data gets staged", never as "how the pointer gets neutralised".

**1. Guest: copy out, then zero.** Before the sanitiser runs, the guest copies
the secondary buffer out of userspace into the aux slot
(`src/guest/nvkvm_main.c:1419-1437` for `RM_CONTROL`;
`:1641-1760` for `RM_ALLOC` NVOS21, `:1761-…` for NVOS64;
`:1360-1418` for the NVKMS wrapper). Then `nvkvm_sanitize_ioctl_params()`
zeroes the pointer field:

```c
case NV_ESC_RM_CONTROL: {
        struct nvos54_parameters *p = buf;
        p->params = 0;               /* secondary buf in aux slot */
        break;
}
```
— `src/guest/nvkvm_ioctl.c:359-363`

Same for `p_alloc_parms` and `p_rights_requested` on `RM_ALLOC`
(`src/guest/nvkvm_ioctl.c:347-357`), `p_linear_address` on
`RM_MAP_MEMORY`/`RM_UNMAP_MEMORY` (`:416-438`), the NVKMS `address` field
(`src/guest/nvkvm_main.c:1383`).

Where the *inner* params contain further pointers, the guest extends the aux
blob and zeroes those too — the driver will write into the extension:

- **`InfoList` family** (`GR_GET_INFO`, `FB_GET_INFO`, `BUS_GET_INFO`, the
  `GET_CAPS` variants…): `aux = [params][list_size * entry_size]`, the
  `info_list` pointer at offset 8 zeroed (`src/guest/nvkvm_main.c:1551-1586`).
  Entry size is per-command — 8 bytes for the `GET_INFO` family, 4 for
  `GET_ENGINES`/`GET_CLASSLIST`, 1 for the `GET_CAPS` family whose count is a
  byte length. The table exists twice, guest and stub, and is marked "MUST stay
  in sync" (`src/stub/nvkvm_stub.c:452-455`).
- **`FIFO_GET_CHANNELLIST`**: two list pointers, both IN+OUT, both zeroed
  (`src/guest/nvkvm_main.c:1510-1549`).
- **`SYSTEM_GET_BUILD_VERSION`**: three string buffers, aux extended by `3*sz`
  (`src/guest/nvkvm_main.c:1598-1626`).
- **`GPU_GET_ID_INFO`**: `szName` is output-only and optional, so the guest just
  zeroes it rather than marshalling (`src/guest/nvkvm_main.c:1628-1639`).

**2. Boundary: overwrite at the far end.** The stub does not check whether the
guest zeroed anything. It picks the pointer's *offset* from the cmd — which
selects the struct layout, and which a guest cannot use to skip the write — and
then writes either the aux pointer or an explicit 0:

```c
int ptr_off = -1;
if ((job.cmd == NVKVM_NVKMS_IOCTL_CMD &&
     job.param_size >= NVKVM_NVKMS_PARAMS_SIZE) ||
    (job_type == 'd' && (job_nr == 0x54 || job_nr == 0x49) &&
     job.param_size >= 16)) {
        ptr_off = NVKVM_NVKMS_ADDR_OFF;
} else if (job_type == 'F' && (job_nr == 0x2a || job_nr == 0x2b) &&
           job.param_size >= 24) {
        ptr_off = 16;
} else if (job.aux_size > 0 && job.param_size >= 24) {
        ptr_off = 16;
}
if (ptr_off >= 0) {
        uint64_t ptr_val = (job.aux_size > 0)
                ? (uint64_t)(uintptr_t)job.aux_buf : 0;
        __builtin_memcpy((char *)job.param_buf + ptr_off,
                         &ptr_val, sizeof(uint64_t));
}
```
— `src/stub/nvkvm_stub.c:901-921`

Offset 16 is where both `NVOS54.params` and `NVOS21`/`NVOS64.p_alloc_parms`
live (four 4-byte handles precede them). The NVKMS wrapper and two DRM ioctls
(`SEMSURF_FENCE_CTX_CREATE`, nr 0x54, and `GEM_EXPORT_NVKMS_MEMORY`, nr 0x49)
carry their single pointer at offset 8 (`NVKVM_NVKMS_ADDR_OFF`,
`src/common/nvkvm_proto.h:112`) instead.

The first two arms are the **U-2** fix. Both rewrites used to be gated on
`job.aux_size > 0` — a value the guest chooses — so a guest sending
`aux_size == 0` fell out of both branches and its own eight bytes reached
`stub_ioctl()` and the driver verbatim (`src/stub/nvkvm_stub.c:876-900`).

**What step 2 does not cover — stated plainly.** This rewrite is the closest
thing in the tree to a categorical mechanism, and it is not one:

- ~~**It is conditional on `job.aux_size`, which the guest chooses.**~~ —
  **fixed (U-2).** The pointer's *offset* is now chosen from the cmd (which
  selects the struct layout and which the guest cannot use to skip the write),
  and the field is then set to either the aux pointer or an explicit 0. With
  `aux_size == 0` the guest's bytes no longer survive. `aux_size == 0` occurs on
  the ordinary live path (measured: 11 `RM_CONTROL` + 24 `RM_ALLOC` calls in one
  CUDA run), so this was not a theoretical corner.
- **It only ever covers offset 16.** Pointer fields at other offsets —
  `NVOS64.pRightsRequested@24`, `NVOS33.pLinearAddress@32`,
  `NVOS56.pNewCpuAddress@24`, DRM `event_nvkms_params_ptr@32` — have no
  host-side site that writes them at all.
- **It says nothing about pointers *inside* the aux blob**, which is where most
  of the surface is. Those are handled by roughly a dozen hand-written
  per-command blocks (`src/stub/nvkvm_stub.c`), each guarded by a count or size
  the guest supplies. Those guards are still guest-derived, but they no longer
  **fail open**: since U-2/U-4 every one of them writes an explicit 0 into the
  pointer field when its guard does not fire, instead of leaving the guest's
  bytes there.
- **`NVOS54.paramsSize` is never checked against `aux_size`**, so the driver's
  copy length and the buffer it copies from/to are independently
  guest-controlled.
- ~~**`NVOS32` (`RM_VID_HEAP_CONTROL`) is forwarded opaquely with no `function`
  gate**~~ — **fixed (U-3).** `function` (NvU32 at offset 8) is now gated
  default-deny in QEMU: only `NVOS32_FUNCTION_ALLOC_SIZE` (2) is forwarded, so
  `ALLOC_OS_DESCRIPTOR` (27, whose `[IN]` descriptor address the driver pins
  with `pin_user_pages`) and `HW_ALLOC` (19, `bindResultFunc` / `pHandle`) are
  refused. This does **not** affect the deliberate OS-descriptor path, which is
  `NV_ESC_RM_ALLOC_MEMORY` (nr 0x27) with `hClass == 0x71`, a different ioctl.
- **UVM ioctls do not go through the stub at all.** They run in QEMU's own
  process (`src/qemu/nvkvm_isolate_handlers.c`) and carry `base`/`length`
  virtual-address ranges that UVM interprets in QEMU's address space. Since
  **U-6** those ranges are validated host-side against a per-UVM-handle
  ownership table: a range is usable only if nvkvm recorded it when the driver
  accepted the command that created it (`CREATE_EXTERNAL_RANGE`,
  `ALLOC_SEMAPHORE_POOL`, `MAP_DYNAMIC_PARALLELISM_REGION`,
  `REGISTER_CHANNEL`), and `UVM_MIGRATE.semaphoreAddress` — the one UVM field
  the driver *writes* through — is unconditionally zeroed. The ioctl still runs
  in QEMU's process; that placement is unchanged.
- **The SPSC command ring bypasses this path entirely** — see
  [the SPSC ring](#the-residual-control-traffic-and-the-spsc-ring).
  `ring_exec_one` (`src/stub/nvkvm_stub.c`) consults no allowlist whatsoever.
  (Its pointer rewrite is no longer `aux_size`-gated — it now writes an explicit
  0 when there are no inner params — but that closes only the fail-open half;
  the missing allowlist, U-1, is untouched.)

So the honest statement of the invariant is: *the host side neutralises the
pointer field of the three highest-traffic ioctls in the common case, and each
additional pointer field is covered only where someone wrote a case for it.* It
is not "no guest pointer is ever forwarded" today. `docs/internal/audit-guest-pointers.md`
enumerates every entry point against this property, ranks the gaps, and argues
for replacing the per-ioctl approach with a schema-driven default-deny sweep.

Severity is bounded by the isolation model rather than by this mechanism: the
stub is unprivileged and Phase 0 `clone()`s it into fresh user/pid/net/ipc/uts
namespaces under a seccomp allowlist, so a guest VA that does reach the driver
corrupts within one isolate — one guest process's own GPU context — rather than
QEMU, the host, or another VM. The UVM path is the exception, because it lands
in QEMU. Bounding the blast radius is not the same as closing the bug class, and
this document should not be read as claiming otherwise.

`job.aux_buf` is a private anonymous mapping in the stub, filled by `recv()`
from the socket (`src/stub/nvkvm_stub.c:1944-1945`) — not shared memory, not
guest-writable. The inner-pointer reconstructions for the `InfoList`,
`GET_BUILD_VERSION`, `FIFO_GET_CHANNELLIST` and `EXPORT_OBJECT_TO_FD` families
happen the same way, pointing into the aux blob's extension region
(`src/stub/nvkvm_stub.c:986-1160`).

**3. Boundary: zero again on the way back.** Every substituted pointer is a
stub virtual address and must not be visible to the guest
(`src/stub/nvkvm_stub.c:1404-1458`). Same for the version-string pointers, the
`InfoList` pointer, and the DRM/NVKMS ones.

### "The boundary, not the untrusted guest, must ensure no guest pointer is ever forwarded"

That sentence is the design principle, and it is written in the code at the
place where it was most nearly violated. `NV_ESC_RM_IDLE_CHANNELS` carries three
`NvP64` pointers to arrays of channel handles, plus a count.

Read the block below as a statement of intent, not as the shipping control. It
**lived** in `src/qemu/nvkvm_dispatch.c`, whose every call site was inside the
`#if 0` in `src/qemu/virtio_nvgpu.c` — the file still compiled and linked, so it
read as live while nothing in it executed. **That file was deleted on 2026-08-24
(DEAD-1)**, along with `nvkvm_frontend.c`; the block is quoted here from history
(`git show 68a35c0:src/qemu/nvkvm_dispatch.c`) because it is still the best
worked example of the principle, and because the reasoning in it is worth more
than the code was. The control that does ship is quoted further down, in the
stub. See the DEAD-1 accounting at the top of `src/qemu/virtio_nvgpu.c` for what
each deleted fix protected and where its live equivalent is.

```c
/*
 * Audit G-2: the boundary (not the untrusted guest) must ensure
 * no guest pointer is ever forwarded.  Always overwrite the
 * p_* fields: when num_channels>0 they point into the aux slot
 * (3 arrays of n handles), otherwise they are zeroed.  Use
 * 64-bit math and a hard cap so a malicious guest cannot
 * overflow the size check (3*n*sizeof in 32-bit wraps for
 * n≈0x55555556, which would push p_devices/p_channels past the
 * aux buffer → host OOB read).
 */
if (n > 0) {
        size_t needed = (size_t)3u * n * sizeof(nvhandle_t);
        if (n > NVKVM_IDLE_MAX_CHANNELS ||
            !ctx->aux_buf || ctx->aux_size < needed)
                return -EINVAL;
        p->p_clients  = (nvp64_t)(uintptr_t)ctx->aux_buf;
        p->p_devices  = p->p_clients + (size_t)n * sizeof(nvhandle_t);
        p->p_channels = p->p_devices + (size_t)n * sizeof(nvhandle_t);
} else {
        p->p_clients = p->p_devices = p->p_channels = 0;
}
```
— was `src/qemu/nvkvm_dispatch.c:383-403`, cap defined at `:21`, at `68a35c0`

Note the integer-overflow hardening specifically: `3 * n * sizeof(nvhandle_t)`
computed in 32 bits wraps for `n ≈ 0x55555556`, which would let a small
`aux_size` pass the check while `p_devices` and `p_channels` land past the end
of the aux buffer — a guest-driven out-of-bounds read in the privileged VMM. The
fix is 64-bit arithmetic *and* a hard cap of 4096 channels, not one or the other.
(Both were unreachable, per the note above, and the code is now gone. The
reasoning is worth keeping as the template for what a live version must do —
which the stub's version, quoted below, is.)

Two further points make this a good illustration rather than a footnote.

**The guest also zeroes it — and that is not the protection.**
`src/guest/nvkvm_ioctl.c:462-479` forces the single-channel form guest-side,
zeroing all three pointers and `num_channels`, with its own reasoning:

> the `p_*` fields are guest user pointers to handle arrays. We do NOT marshal
> them (the single-aux-slot path can't carry three arrays), so never forward
> them — the host driver would dereference a guest VA in the stub's address
> space.

If the guest's zeroing were the mechanism, a malicious guest kernel would simply
skip it.

**The dispatch-side fix was dead code, and the real one had to move to the
boundary.** The dead copy said so itself, in a banner it carried until the file
was deleted (`nvkvm_dispatch.c:375-379` at `68a35c0`):

> NOTE (audit P2-1): this dispatch path is NOT wired into the live
> `IOCTL_ON_ISOLATE` flow (`handle_ioctl` is static/unused). The authoritative
> `IDLE_CHANNELS` pointer-sanitisation lives in the stub (`nvkvm_stub.c`, nr
> 0x41 block). Kept here for the (currently dead) synchronous path; do not rely
> on it.

A banner is a weaker instrument than a deletion, which is the lesson DEAD-1
drew: the file wore that warning for months and still got counted as a path.

The live neutralisation is in the stub, on a private copy of the parameters
(`src/stub/nvkvm_stub.c:1268-1283`):

> so the guest-controlled `NvP64` array pointers were reaching the host driver,
> which would walk them as user pointers in the stub's address space. […]
> `job.param_buf` is a private recv'd copy, so this is race-free (not subject to
> the SHM double-fetch, audit P2-2).

The "SHM double-fetch" reference points at the other reason the stub must own a
private copy: QEMU's ioctl worker copies the shared-memory param/aux blobs into
private heap buffers before the allowlist checks and copies them back after
(`src/qemu/virtio_nvgpu.c:641-661`), so a guest cannot mutate a field between
the check and the use.

### Embedded file descriptors

A guest fd number is meaningless on the host, and a host fd number must never
be visible to the guest. Every embedded fd therefore travels as a **handle id** —
an opaque 32-bit token from QEMU's global table.

- **Guest**: `guest_fd_to_handle_id()` (`src/guest/nvkvm_ioctl.c:230-251`) does
  `fget`, checks `nvkvm_file_is_ours()` — a guest can only name one of our own
  device fds — and returns the fd's `handle_id`. Applied to
  `UVM_MM_INITIALIZE.uvm_fd`, `UVM_REGISTER_GPU_VASPACE.rm_ctrl_fd`,
  `UVM_REGISTER_CHANNEL.rm_ctrl_fd`, `UVM_MAP_EXTERNAL_ALLOCATION`'s
  profile-indexed `rm_ctrl_fd`, `NV_ESC_RM_ALLOC_MEMORY.fd`,
  `NV_ESC_RM_MAP_MEMORY.fd`, `NV_ESC_REGISTER_FD.ctl_fd`,
  `NV_ESC_ALLOC_OS_EVENT.fd`, `NV_ESC_FREE_OS_EVENT.fd`
  (`src/guest/nvkvm_ioctl.c:268-546`).
- **Stub**: `handle_lookup()` maps handle id → its own local fd immediately
  before the ioctl, and restores the handle id immediately after, "never leak a
  stub fd" (`src/stub/nvkvm_stub.c:1228-1265`, `:1299-1314`).

Sentinels are respected rather than translated: `libcuda` passes `-1` for "no
ctrl fd" and `0` or `-1` for "no associated fd", and translating those would
turn a valid call into `-EBADF` (`src/guest/nvkvm_ioctl.c:281-283`, `:369-377`).

---

## Problem 3: ABI versioning

NVIDIA guarantees no ioctl ABI stability across driver releases. The structs
`libcuda` passes are a private contract, versioned only by driver and userspace
shipping together. A forwarder sits inside that contract.

`nvkvm` keys the version-variant layouts off the host driver's version, in a
table shared by all three components: `src/common/nvkvm_abi.h`. Eight rows
(515, 525, 535, 545, 550, 570, 580, 610), nine values each — one per distinct
layout measured across every published OGKM branch, 515 through 610. Selection
takes the full `major.minor.patch` because two boundaries fall *inside* a
branch: 535 splits at the Confidential Computing channel fields (535.54.03
measures 304 B, 535.86.05 measures 360 B) and 550 splits at the V550 UVM array
(550.40.07 measures 1200 B, 550.40.53 measures 9264 B). Full detail in
[`docs/reference/abi-profiles.md`](docs/reference/abi-profiles.md); the
architecturally interesting parts are below.

**Selection is deterministic from the version string**, so the guest and QEMU
independently arrive at the same profile without a negotiation
(`src/common/nvkvm_abi.h:15-19`). QEMU probes the host driver at realize with
`NV_ESC_CHECK_VERSION_STR` (`src/qemu/virtio_nvgpu.c:1159-1169`) and publishes
the string in the shared-memory control block; the guest parses the same string.
QEMU **additionally** stamps the profile id into every `ISOLATE_CMD_IOCTL`
(`abi_profile`, `src/common/nvkvm_isolate_proto.h:107-109`, set at
`src/qemu/nvkvm_isolate.c:1820`) so the stub parses nothing at all.

**Values are measured, never derived.** `tools/abi_derive.sh` compiles a probe
against NVIDIA open-gpu-kernel-modules at each version tag and prints
`sizeof`/`offsetof` for exactly the nine fields
(`tools/abi_derive.sh:20-51`). Its header explains why that discipline exists:

> WHY THIS EXISTS. `src/common/nvkvm_abi.h` carries a version-keyed table of
> struct sizes and offsets. Those numbers were originally DERIVED by arithmetic
> ("V550 grew the array +9180 bytes, so 535 must be 9264-9180 = 84") rather than
> measured. Three of the five 535-row values were wrong -- `uvm_map_ext_size` is
> really 1200, not 84 -- and the error is SILENT: a wrong size does not fail to
> compile, it forwards a truncated struct and the kernel reads past the buffer.
>
> So: never hand-derive a row. Run this and paste what it prints.
>
> — `tools/abi_derive.sh:5-12`

That is the whole argument. 84 versus 1200 is not a rounding error; it is the
guest copying 84 bytes of a 1200-byte struct into a shared slot and the host
kernel's `copy_from_user` reading 1116 bytes of whatever follows. Nothing fails
loudly. The compile succeeds, the ioctl is accepted, and the driver acts on
garbage.

The *offset* variant of the same mistake is documented with its observed
symptom (`src/guest/nvkvm_ioctl.c:301-312`):

> `struct uvm_map_external_allocation_params` hardcodes V550, so dereferencing
> `p->rm_ctrl_fd` reads offset 9248 on EVERY driver. On a 535 host libcuda's
> struct is only 1200 bytes, so that read lands 8 KiB past the real field, the
> garbage fails `guest_fd_to_handle_id()`, and the ioctl returns `-EBADF` from
> inside the guest — nothing ever reaches QEMU, so the QEMU debug log shows no
> error at all. Observed on GTX 1660 SUPER / 535.309.01 as `cuCtxCreate -> 999`.

A third variant is worth naming because it is a different failure class again:
the *fallback row*. `nvkvm_abi_by_id()` used to fall back to
`&nvkvm_abi_profiles[1]` with a comment saying "570/575 default", but index 1 is
the 550 row. On a 570/575 host that was harmless by coincidence — the two fields
the stub reads through that path are identical in both rows — and on a 535 host
it would have placed an embedded-fd fixup ~8 KiB past the real field. It now
looks the default up by id "so the code and the comment cannot drift apart
again" (`src/common/nvkvm_abi.h:279-303`).

The profile is consulted at nine call sites across all three components — the
guest's ioctl size table and sanitiser, its per-`hClass` alloc-param sizing,
QEMU's expected-size table and UVM schema floor, and the stub's UVM fd offset
and NVOS46 status offset. `tests/abi_parity` asserts the compiled-in table
against measured values.

**Six of the eight rows have actually been booted**: 535 (on 535.309.01) and
570 (on 575.51.03) earlier, then 545 (545.23.08), 550 (550.54.14), 580 (at both
ends of its range, 580.95.05 and 595.84) and 610 (610.43.02) in the boot-matrix
run. The remaining two, 515 and 525, are measured from OGKM source by
`tools/abi_derive.sh` and asserted by `tests/abi_parity` but have not been
brought up: the drivers that select them do not build against kernel 6.8, which
is what every KVM-capable test host ran. Deriving a layout is not booting one.
See [`tests/BOOT_MATRIX.md`](tests/BOOT_MATRIX.md) and
[`docs/reference/supported-drivers.md`](docs/reference/supported-drivers.md).

One category of version drift the profile table does **not** cover, and which
bit during 535 bring-up: which control commands `libcuda` chooses to issue.
`libcuda` on 535.309.01 calls `NVC36F_CTRL_GET_CLASS_ENGINEID` during
`cuCtxCreate`; `libcuda` on 575.51.03 does not. The allowlist, generated against
the 575 ABI, denied it, and `cuCtxCreate` returned
`CUDA_ERROR_OPERATING_SYSTEM` (304) with a single `DENY` line in QEMU's log
(`src/qemu/nvkvm_ctrl_allowlist.h:218-239`). That is the expected shape of a
new-driver-branch failure: not a crash, an `EACCES` and a CUDA error code.

---

## Problem 4: the isolation model

### One isolate per guest process

The unit of isolation is the guest *process*, not the guest. Each guest process
(keyed by `mm_struct`) gets exactly one host isolate: a separate process running
`nvkvm_stub`, holding its own dup'd device fds and its own RM client
(`src/common/nvkvm_proto.h:26-31`).

This is not primarily a security decision — it is forced by the driver. The
NVIDIA RM validates that the calling task owns the client
(`rmclientValidate` compares `pClient->pOSInfo` against the calling task's
`nvfp`), and UVM binds its file's `nvfp` to the calling task's `mm` during
`UVM_INITIALIZE`. Run the ioctls from a shared process and the driver rejects
them. The security property falls out of satisfying the driver.

Consequences visible throughout the code:

- **`/dev/nvidiactl` and `/dev/nvidiaN` are opened by the stub**, not by QEMU,
  so the file's `nvfp` lineage matches the process running the RM ioctls. QEMU
  reserves a handle slot, asks the stub to open the device
  (`ISOLATE_CMD_OPEN_DEVICE`), and receives a `SCM_RIGHTS` copy back to keep in
  its own table (`src/qemu/nvkvm_isolate_handlers.c:192-303`).
- **`/dev/nvidia-uvm` is opened twice by the stub itself** at startup, and a
  `RECEIVE_FD` carrying a UVM fd from QEMU is deliberately *dropped* in favour
  of one of those local opens (`src/stub/nvkvm_stub.c:255-266`, `:2722-2728`).
- **UVM ioctls run in QEMU's process**, because UVM's mmap must come from the
  same `mm` that ran `UVM_INITIALIZE`, and that mmap is what installs the KVM
  region (`src/qemu/nvkvm_isolate_handlers.c:1229-1237`). This is the one place
  where privileged QEMU executes a guest-named ioctl, which is why the UVM
  schema allowlist exists.
- **`RM_SHARE` after every successful alloc.** The kernel's default share policy
  is `RS_SHARE_TYPE_PID`, granting `DUP_OBJECT` only when the caller's PID
  matches the resource owner's. In a split-process model those PIDs differ, so
  UVM's kernel-internal client cannot dup `libcuda`'s VA space and `cuCtxCreate`
  fails with `NV_ERR_INSUFFICIENT_PERMISSIONS`. QEMU issues an `NV_ESC_RM_SHARE`
  granting `RS_ACCESS_DUP_OBJECT` on the new handle
  (`src/qemu/nvkvm_isolate_handlers.c:1992-2109`).

  The share type is `RS_SHARE_TYPE_ALL`, and the reasoning is worth reading in
  full because it is counter-intuitive
  (`src/qemu/nvkvm_isolate_handlers.c:2065-2088`):

  > This is NOT a cross-tenant hole: cross-VM/host containment comes from the
  > handle NAMESPACE (reach-gating), not the share type. A foreign client cannot
  > RESOLVE another client's object — the dup fails at `clientGetResourceRef`
  > (`NV_ERR_OBJECT_NOT_FOUND`, 0x57) BEFORE the share policy is consulted.
  > Proven by `tests/security/poc_cross_proc_dup`: an unprivileged host
  > neighbour, with a valid device parent, naming the exact live
  > `(hClientSrc,hObjectSrc)` of a guest VRAM object, is denied 0x57 EVEN UNDER
  > TYPE_ALL — i.e. even when ALL grants it the DUP right, it still can't reach
  > the object.

  It replaced an earlier `TYPE_CLIENT` grant that depended on a hardcoded "UVM
  is the first RM client" assumption and broke on any reboot or init-order
  change.

### What the isolate actually is

A freestanding static PIE. No libc, no pthread — futex-based mutex/cond, raw
syscall wrappers, a `clone3` trampoline and a tiny printf all live in
`src/stub/stub_freestanding.h` (`src/stub/nvkvm_stub.c:4-7`). It applies its own
`R_X86_64_RELATIVE` relocations before touching global data
(`src/stub/nvkvm_stub.c:2711-2745`), because it is `fexecve`'d from a memfd with
no dynamic linker.

It is embedded in the QEMU binary as a byte array (`xxd -i`,
`src/stub/Makefile:32-34`) and launched from an anonymous memfd — no file on
disk (`src/qemu/nvkvm_isolate.c:803-864`).

One reader thread, 16 worker threads
(`NVKVM_STUB_WORKERS`, `src/stub/nvkvm_stub.c:433`). Non-IOCTL commands run
inline on the reader; IOCTLs are queued to the pool and matched back by
`txn_id`.

### The sandbox

Applied in the forked child before `exec`, while it still has the privilege to
create namespaces (`src/qemu/nvkvm_isolate.c:50-64`):

1. `CLONE_NEWUSER` with a single-line rootless uid/gid map, plus `CLONE_NEWPID |
   CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNS` in one `clone`
   (`src/qemu/nvkvm_isolate.c:124-133`). The child is PID 1 of the new pid ns and
   `clone()` returns its real host pid, so there is no intermediate process.
2. A minimal root: a 256 KiB tmpfs containing bind mounts of *only* the nvidia
   device nodes, `pivot_root` into it, old root detached, then remounted
   read-only (`src/qemu/nvkvm_isolate.c:155-231`).
3. `PR_SET_NO_NEW_PRIVS`, `PR_SET_DUMPABLE=0`, capability bounding set dropped,
   effective/permitted/inheritable zeroed, ambient cleared
   (`src/qemu/nvkvm_isolate.c:66-81`).
4. Every inherited fd above the reserved ones closed via `close_range(2)`
   (`src/qemu/nvkvm_isolate.c:276-308`).
5. stdout/stderr redirected to `/dev/null`
   (`src/qemu/nvkvm_isolate.c:837-852`), environment cleared
   (`src/qemu/nvkvm_isolate.c:860`).
6. After the worker pool is spawned, a seccomp allowlist with `TSYNC`
   (`src/stub/nvkvm_stub.c:2610-2692`) — 20 syscalls, everything else `EPERM`.

Three of those steps carry post-mortems worth reading.

**The `/dev` dirfd was an escape hatch.** An earlier version parked an `O_PATH`
handle to the *whole host `/dev`* at a fixed fd and pivoted into an empty tmpfs:

> That handle was an escape hatch — a compromised stub could
> `openat(dd, "../../etc/shadow", O_RDONLY)` and read any host file, because the
> dirfd's ".." resolved to the (still-referenced) host root above /dev. Fix (the
> runc device-bind idiom): construct a tmpfs root holding ONLY /dev/nvidia*,
> pivot into it, then open the dirfd AFTER the pivot so its ".." is the sandbox
> root, which contains nothing but those nodes.
>
> — `src/qemu/nvkvm_isolate.c:141-148`

**Closing inherited fds is not hygiene.** `src/qemu/nvkvm_isolate.c:261-266`:

> Without this the stub inherits QEMU's KVM vm fd, the memory-backend fds, every
> other isolate's socket-pair, and so on — turning any stub RCE into "set
> arbitrary host memory region visible to the guest" via
> `KVM_SET_USER_MEMORY_REGION`.

**Plain W^X is insufficient.** The seccomp filter denies `PROT_EXEC` on `mmap`
and `mprotect` outright, not just `W|X` together
(`src/stub/nvkvm_stub.c:2624-2641`):

> an attacker can mmap a page RW, write shellcode, then mprotect it R-X — each
> step passes W^X but the result is executable attacker code. The stub's own
> `.text` is mapped executable by the ELF loader BEFORE seccomp and it never JITs
> (libcuda runs in the guest), so no runtime mapping ever needs `PROT_EXEC`.

`TSYNC` matters for the same class of reason: the worker pool is spawned before
`apply_seccomp()` runs, and a non-TSYNC filter would bind to the reader thread
only, leaving the threads that run all attacker-influenced ioctl handling
completely unsandboxed (`src/stub/nvkvm_stub.c:2684-2689`).

A `SIGSEGV` inside the stub terminates it rather than returning
(`src/stub/nvkvm_stub.c:674-686`): returning would re-execute the faulting
instruction forever and pin a host core; `exit_group` is async-signal-safe, and
QEMU's reader sees the dead socket and fails every pending caller with
`-ECONNRESET`. One isolate dies.

### Handles and fd translation

QEMU owns a global handle table (`src/qemu/nvkvm_handle.c`, 65536 slots). A
handle wraps either an open `/dev/nvidia*` fd or a memfd
(`src/qemu/nvkvm_handle.h:17-31`). Handles are distributed to isolates by
`SCM_RIGHTS` and refcounted; `close_handle` returns `-EBUSY` while any isolate
still holds one (`src/qemu/nvkvm_handle.c:310-331`).

The guest sees only handle ids. The stub sees only its own local fds. Neither
sees the other's. `nvkvm_handle_acquire_fd()` returns a `dup` taken atomically
under the table lock, because an ioctl runs on QEMU's thread pool while
`CLOSE_HANDLE` runs on the TX thread and could otherwise `close()`+recycle the
fd mid-ioctl (`src/qemu/nvkvm_handle.c:251-277`).

### The allowlists

Six default-deny gates, all in QEMU, all in `nvkvm_req_ioctl_on_isolate()`:

| gate | entries | file |
|---|---|---|
| UVM command schema | 31 | `src/qemu/nvkvm_isolate_handlers.c:599-645` |
| DRM render-node NR | 14 | `src/qemu/nvkvm_drm_allowlist.h` |
| NVKMS inner cmdType | 7 | `src/qemu/nvkvm_nvkms_allowlist.h` |
| frontend ioctl NR | 23 | `src/qemu/nvkvm_fe_alloc_allowlist.h:25-55` |
| `RM_ALLOC` class | 89 | `src/qemu/nvkvm_fe_alloc_allowlist.h:59-149` |
| RM control command | 166 + 2 rules | `src/qemu/nvkvm_ctrl_allowlist.h:28-240` |

Plus a runtime per-VM `hClient` set built from observed successful allocs
(`src/qemu/nvkvm_isolate_handlers.c:1959-1990`), consulted on `DUP_OBJECT`'s
source client and on every `'F'` ioctl carrying an `hClient` at param offset 0.

They live in QEMU rather than the guest because the guest kernel module is
inside the boundary being defended (`src/qemu/nvkvm_ctrl_allowlist.h:20-22`).
Because they live in QEMU, they apply to the **virtqueue** path only; the SPSC
ring reaches the stub without passing through QEMU and is currently ungated
(see [the SPSC ring](#the-residual-control-traffic-and-the-spsc-ring)).
The RM control list is generated from gVisor nvproxy's `compUtil`-tagged set
unioned with empirically observed traffic; full provenance and the reasoning
behind each deliberate exclusion is in
[`docs/reference/allowlists.md`](docs/reference/allowlists.md).

### What the trust boundary actually is

- **QEMU's boundary is cross-VM and host-process.** Not intra-VM. Intra-VM
  access control is emulated by the guest kernel module, which owns the guest's
  pids, uids, namespaces and fds and is the authority
  (`src/qemu/nvkvm_isolate_handlers.c:1240-1252`). QEMU deliberately does not
  second-guess it — doing so would reject a handle the guest legitimately shared
  into another isolate (CUDA IPC) and would add nothing, since a malicious guest
  kernel would just forge `session_id`.
- **The guest does not defend against the host.**
  `src/guest/nvkvm_mmap.c:18-20`: "A malicious host could abuse this, but we are
  not defending against the hypervisor."
- **The guest never receives the device.** No BAR is assigned to it, no MMIO
  window is handed over, and there is no DMA path from the guest to host memory.
  Compare PCIe passthrough, where the GPU retains DMA access to host RAM.
- **This is not a hardened multi-tenant boundary.** Two internal audits, no
  external audit, experimental software. See
  [`docs/internal/known-limitations.md`](docs/internal/known-limitations.md).

---

## Problem 5: the data plane

The parity numbers — 1.00x on memory bandwidth, on tensor-core matmul, on
ResNet-50 training — are not the result of a fast forwarder. They are the result
of the forwarder not being in the path.

### In steady state there is no call

Once a CUDA channel exists, launching work is a **store to a mapped page**.

`libcuda` writes GPFIFO entries into its command-ring pages and then rings the
doorbell with a write-combining store to a mapped BAR page. Those pages were
installed into the guest process's page tables by `remap_pfn_range` and point at
the real GPU BAR (see [Problem 1](#problem-1-the-mmap-problem)). The store goes
CPU → write-combining buffer → GPU BAR. It touches the guest kernel module: no.
The virtqueue: no. QEMU: no. The stub: no. There is no VM exit.

Completion is a poll on the channel semaphore, which lives in pinned system
memory mapped write-back — which is exactly why the cacheability rule in
`src/guest/nvkvm_mmap.c:175-191` is load-bearing rather than cosmetic. Mapping
the semaphore write-combining turned a 0.36 µs poll into a 3 µs poll and dropped
GPU utilisation to 0-5%.

There is no doorbell interception anywhere in the tree. That absence is the
design.

The cost is paid up front instead: a single `cuCtxCreate` issues more than 1500
device `mmap`s, each a virtqueue round trip
(`src/qemu/nvkvm_isolate_handlers.c:2303-2306`). The sparse window exists to
make that setup cost survivable — one KVM memslot instead of 1500.

### The residual control traffic, and the SPSC ring

What remains on the forwarded path is control traffic: `RM_CONTROL` polls,
allocations, frees. That is where the latency tax shows up (alloc/free round
trip is the standing optimisation target) and it is what the command-buffer ring
addresses.

The ring is a lock-free single-producer/single-consumer byte ring in memory
shared by **all three parties** — QEMU, the isolate, and the guest
(`src/common/nvkvm_ring.h`). QEMU mints one memfd holding two back-to-back
rings, request (guest→isolate) at offset 0 and response at
`nvkvm_ring_resp_off()`, 64 KiB of data each
(`src/common/nvkvm_ring.h:64-73`). It reaches the isolate by `SCM_RIGHTS`
(`src/qemu/nvkvm_isolate.c:1370-1383`) and the guest by being `MAP_FIXED` into
the sparse GPA window — so it rides the window's single pre-installed memslot
with no new KVM ioctl (`src/qemu/nvkvm_isolate.c:1306-1338`). The guest
`memremap`s it write-back and checks that it sees QEMU's initialisation
(`src/guest/nvkvm_main.c:505-540`).

Synchronisation is four atomics total: the producer release-stores `tail`, the
consumer acquire-loads it, and symmetrically for `head`
(`src/common/nvkvm_ring.h:116`, `:157`, `:184`, `:226`). Counters are
free-running byte counts on separate 64-byte lines; records never straddle the
wrap (a skip record pads to the end instead, `:135-149`).

The consumer treats the ring as hostile, and the mechanism that makes that safe
is one parameter (`src/common/nvkvm_ring.h:106-110`):

> FF-1: `cap` is the caller's TRUSTED ring capacity (snapshotted at setup), NOT
> `r->size` — that control word lives in shared memory the peer can forge.
> Masking off with a trusted power-of-two bounds every write into the real data
> region; a forged head/tail then only mis-accounts free space (logic/teardown),
> never an OOB write.

A malformed record tears down that one isolate (`stub_exit(140)`,
`src/stub/nvkvm_stub.c:2474`) — per-guest DoS, never out-of-bounds.

**Wake-up without a futex.** There is no synchronisation word in guest-writable
memory. The guest's per-session pump kthread issues a blocking `ENTER_LOOP`
virtqueue request; QEMU offloads it to its thread pool
(`src/qemu/virtio_nvgpu.c:853-868`) and forwards it to the stub, whose reader
thread spins on the request ring, executing eligible controls **inline** — no
worker hand-off, "that latency is the whole point"
(`src/stub/nvkvm_stub.c:2233-2234`). At every drain edge it still polls the
socket non-blocking, so slow-path IOCTL/INTERRUPT/EXIT stay serviced
(`src/stub/nvkvm_stub.c:2416-2442`).

The exit edge is the interesting bit
(`src/common/nvkvm_ring.h:243-247`):

> Exit-edge invariant (lost-wakeup-free, the analog of a futex presleep
> re-check): after the consumer decides to exit on its idle timeout, it must call
> `has_work()` ONE more time; if true it aborts the exit and keeps looping, and
> it completes `enter_loop` only when this returns false. The guest must order
> publish-T (release-store tail) BEFORE testing whether `enter_loop` is still in
> flight, and re-enter when `enter_loop` completes with
> `last_processed < last_published`.

The guest side is level-triggered rather than edge-triggered: the pump's inner
`while (has_work(req_ring))` re-evaluates after every `ENTER_LOOP` return
(`src/guest/nvkvm_main.c:305-328`), so a record published in the gap is never
lost.

**Only flat `RM_CONTROL`s ride it** (`src/common/nvkvm_ring_ioctl.h:14-20`) —
type `'F'`, NR `0x2a`, nvos54, params ≤ 256 B and aux ≤ 4096 B, and the inner
control not in the pointer-carrying set (`InfoList` family,
`GET_BUILD_VERSION`, `EXPORT_OBJECT_TO_FD`, `FIFO_GET_CHANNELLIST`). Anything
else the stub **PUNTs** — and PUNT means *not executed*, so the guest's fallback
to the virtqueue can never double-run a side-effecting control
(`ring_ctrl_must_punt()`, `src/stub/nvkvm_stub.c:2309-2340`).

**It is a pure optimisation with a fallback at every layer** — setup failure,
BAR not yet programmed, ring full, ineligible command, stub PUNT, all land on
the virtqueue path (`src/common/nvkvm_isolate_proto.h:284-286`,
`src/qemu/nvkvm_isolate.c:966-970`, `src/guest/nvkvm_main.c:280-282`).

**Open issue: the ring path is not gated by any allowlist.** The safety argument
above is about *record framing* — a forged head/tail or a malformed record can
only DoS the isolate, never write out of bounds. It is not an argument about
record *contents*. Every gate described under
[the allowlists](#the-allowlists) — the RM control-command allowlist, the
per-VM `hClient` allowlist, the `DUP_OBJECT` source-client check, the `'F'`-type
default-deny — lives in `nvkvm_req_ioctl_on_isolate` (QEMU), and the ring does
not pass through QEMU at all: the guest writes records into the shared memfd and
`ring_exec_one` (`src/stub/nvkvm_stub.c:2342`) executes them. `grep allowlist
src/stub/nvkvm_stub.c` returns only seccomp comments. `ring_ctrl_must_punt`
decides *marshalling* eligibility, not *permission*, and it accepts
`aux_size == 0` explicitly. (The inner-params pointer rewrite on that path is no
longer fail-open — it writes an explicit 0 when there are no inner params — but
that is a pointer fix, not the missing allowlist.)

Note also that the ring is created for every isolate unless the **host** sets
`NVKVM_RING_DISABLE` (`src/qemu/nvkvm_isolate.c:988`); the guest-side
`ring_enable` module parameter defaulting to false
(`src/guest/nvkvm_main.c:424`) is a guest-side performance switch and carries no
security weight. Tracked in `docs/internal/audit-guest-pointers.md` (U-1), where
the recommendation is to route ring records through the same gates as the socket
path or, given the measured result below, to delete the ring.

**And it is off by default.** This is the honest and most informative part of
the whole data-plane story (`src/guest/nvkvm_main.c:414-424`):

> Default OFF: the ring is correct + validated (HW: 1446 flat RM_CONTROLs/decode
> offloaded, byte-exact matmul, 4x multi-proc, zero regression) but measurement
> showed it does NOT improve LLM decode throughput — control-RTT is only ~1-2% of
> per-token time (the bottleneck is GPU compute + the mapped doorbell/fence
> launch path), and keeping the isolate spinning costs host CPU. Enable it for
> workloads that ARE control-latency-bound:
> `echo 1 > /sys/module/nvkvm_guest/parameters/ring_enable`

Read that as the summary of this section. The ring is a fully implemented,
three-party-validated fast path for forwarded control calls, and it makes almost
no difference — because the forwarded control calls are already only 1-2% of the
time. The steady state was never the ioctl path.

### The slow path is still asynchronous

For completeness: forwarded ioctls do not serialise on QEMU's TX thread.
`NVKVM_REQ_IOCTL_ON_ISOLATE` is offloaded to QEMU's thread pool so a blocking
stub round trip does not stall the single TX thread and starve other guests
(`src/qemu/virtio_nvgpu.c:886-926`). Multiple ioctls are in flight per isolate,
matched by `txn_id` against per-caller condvars
(`src/qemu/nvkvm_isolate.c:1773-1864`).

Signals work: a guest task blocked in a forwarded ioctl that receives a signal
causes `NVKVM_REQ_INTERRUPT`, which the stub turns into a `SIGUSR1` posted to
the worker running that `txn_id`, making its in-flight host ioctl return
`-EINTR` (`src/common/nvkvm_proto.h:327-333`, `src/stub/nvkvm_stub.c:710-728`).
The `SIGUSR1` handler is deliberately empty and registered without `SA_RESTART`
(`src/stub/nvkvm_stub.c:699-708`).

`VIRTIO_RING_F_EVENT_IDX` is disabled, and the reason is a good example of
async completions interacting badly with a default
(`src/qemu/virtio_nvgpu.c:1064-1075`): with out-of-order completions, interrupt
suppression could strand the last used-ring entry and the guest would hang
forever in `wait_for_completion`.

MSI-X is forced on (4 vectors: 3 VQs + config). On shared INTx, demuxing reads
each sharing device's ISR status register over MMIO on *every* completion
interrupt — measured ~4170 IRQs and ~2150 ISR-read MMIO exits per token during
LLM decode, "the dominant decode tax"
(`src/qemu/virtio_nvgpu_pci.c:85-99`).

---

## Lineage

`nvkvm` is heavily derived from **gVisor's `nvproxy`** (Apache-2.0). The
per-file mapping is in [`CREDITS`](CREDITS); the concepts taken wholesale are the
versioned ioctl dispatch tables, the pointer-translation pattern
(zero guest VAs, substitute host pointers, restore before returning), the RM
object dependency graph with cascade-free, the OS-descriptor pinning approach,
fd translation, the per-container isolation model, and the test patterns.

ABI struct definitions come from **NVIDIA open-gpu-kernel-modules** (MIT /
GPL-2.0), specifically `nvos.h`, `nv-ioctl.h`, `nv-ioctl-numbers.h` and
`uvm_ioctl.h`.

The largest departures from nvproxy are the three this document is organised
around: nvproxy runs in a sentry that shares an address space with the
application, so it never faces the guest-physical-address problem; it has no VM
boundary to place mmaps across; and it does not need a per-process host isolate,
because the sentry already is one.

## Where to go next

| | |
|---|---|
| [`docs/howto/`](docs/howto/) | build, run, stage guest libraries, add a driver version |
| [`docs/reference/`](docs/reference/) | ABI profiles, allowlists, virtio protocol, device nodes, supported drivers |
| [`docs/internal/`](docs/internal/) | forwarding model, isolate model, known limitations |
