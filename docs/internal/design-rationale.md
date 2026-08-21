# Design rationale

Why the architecture is shaped the way it is, and what was tried and rejected.
The mechanisms themselves are in
[`ARCHITECTURE.md`](../../ARCHITECTURE.md); this page is the reasoning.

## The premise: forward the driver interface, not an API

`nvkvm` forwards the NVIDIA Resource Manager ioctl surface. It does not
intercept CUDA calls, marshal API arguments, or ship a shim `libcuda`. The
guest's own unmodified `libcuda` — the real one, version-matched to the host
driver — issues real ioctls against what it believes is a real driver.

That choice determines almost everything else:

- **Everything built on `libcuda` works for free.** PyTorch, cuBLAS, cuFFT,
  llama.cpp, Vulkan, EGL — none of them are on any list, because the boundary is
  below all of them.
- **The steady state can be zero-copy.** An API-remoting layer must copy buffers
  across the boundary. Forwarding the driver interface means the guest can be
  handed the real GPU BAR pages, and then the boundary is not in the path at
  all. This is the whole reason the parity numbers are ratios near 1.00 rather
  than a fraction of host speed.
- **The ABI is unstable and undocumented, and that becomes your problem.** Hence
  [ABI profiles](../reference/abi-profiles.md).
- **The attack surface is the driver's, not an API's.** Hence six default-deny
  [allowlists](../reference/allowlists.md).

## Why not PCIe passthrough

Passthrough gives up the card. The host cannot use it, other guests cannot use
it, attaching and detaching means a device reset and a `vfio-pci` rebind, and it
needs an IOMMU.

It is also weaker than it looks as an isolation boundary: the GPU retains DMA
access to host RAM. `nvkvm`'s guest never receives the device — no BAR is
assigned to it, no MMIO window is handed over, and there is no DMA path from the
guest to host memory.

## Why not vGPU

No SR-IOV, no MIG, no licence, no datacenter SKU, no vendor-supplied guest
driver. Sharing here is cooperative and happens at the driver interface, so it
works on consumer GeForce hardware with a stock guest driver.

The cost is that sharing is cooperative: there is no hardware partitioning and
no scheduling guarantee between tenants.

## Why gVisor's nvproxy is the right ancestor — and where it stops

nvproxy already solved the shape of the problem: versioned ioctl dispatch
tables, a default-deny control-command allowlist, an RM object dependency graph,
fd translation, and the pointer-translation pattern of zeroing guest VAs and
substituting host pointers. `nvkvm` takes all of it; `CREDITS` maps it per file.

Three things do not carry over, and they are the three hardest parts of this
project:

1. **nvproxy has no guest-physical address space.** The sentry shares an address
   space with the application, so a host VA *is* the application's VA. Across a
   VM boundary there is no such thing, and every mapping has to be routed
   through a GPA and a KVM memory slot. See
   [the mmap problem](../../ARCHITECTURE.md#problem-1-the-mmap-problem).
2. **nvproxy is already the per-application process.** `nvkvm` has to create one
   — hence the [isolate model](isolate-model.md).
3. **nvproxy's control-command allowlist is generated against one ABI.**
   `nvkvm` has to survive whichever driver the host happens to have. See
   [ABI profiles](../reference/abi-profiles.md).

## Why the guest gets a real kernel module

Guest userspace must find `/dev/nvidia0`, `/dev/nvidiactl`, `/dev/nvidia-uvm`,
`/dev/nvidia-uvm-tools`, `/dev/nvidia-modeset` and `/dev/dri/renderD128` at the
right majors and minors, because those numbers are hardcoded in the libraries.
`libnvidia-ml` calls `mknodat(195, 255)` before opening `nvidiactl`
(`src/guest/nvkvm_main.c:91-98`), and the Vulkan ICD requires the DRM render
node to be a real DRM device at the canonical major with the matching sysfs tree
— which only registering a `drm_driver` can produce
(`src/guest/nvkvm_drm.c:5-11`).

A kernel module is also where the intra-VM security boundary has to live. QEMU
deliberately does not enforce which guest process may touch which object,
because the guest kernel is the authority on the guest's pids, uids, namespaces
and fds (`src/qemu/nvkvm_isolate_handlers.c:1240-1252`).

## Why sessions are keyed by `mm_struct`

Not by tgid (`src/guest/nvkvm.h:45-56`):

> Linux reuses tgids after a process exits; if a previous tgid-keyed session
> lingered (refcount race) the new process with the same tgid would inherit the
> stale session, isolate, and handle table — a cross-uid info-leak path inside
> one VM.

Threads share an `mm` so they share a session, which is correct. `fork` creates
a new `mm` so the child gets a new session, which is correct. `CLONE_VM` without
`CLONE_THREAD` collapses into one session deliberately — those tasks can already
read and write each other's memory, so they are one security domain, and folding
them together "is correct, not a weakening"
(`src/guest/nvkvm_session.c:36-47`).

## Why the boundary overwrites rather than validates

A guest that zeroes its own pointers is doing the boundary a favour, not
providing a guarantee. So the stub does not check whether a pointer field is
zero — it writes over it
(`src/stub/nvkvm_stub.c:901-921`), and QEMU's dispatch does the same for the
array-carrying case (`src/qemu/nvkvm_dispatch.c:383-403`).

That is also why the aux-slot design was chosen over a generated per-command
pointer-rewrite table. A rewrite table describes where the pointers are; if the
description is wrong or incomplete for some command, a guest pointer is
forwarded. Bouncing the buffer means the pointer field is *always* rewritten to
something the far side owns, whatever the command is. A command whose inner
params contain pointers the guest did not marshal simply fails — the driver gets
a zero pointer — rather than dereferencing a guest VA.

Corollary: a **PUNT** on the fast path means "not executed"
(`src/common/nvkvm_ring_ioctl.h:18-20`), so falling back to the slow path can
never double-run a side-effecting control.

## Why the allowlists live in QEMU

Because the guest kernel module is inside the boundary being defended
(`src/qemu/nvkvm_ctrl_allowlist.h:20-22`). A check the guest performs stops
malicious guest *userspace*; only a check in QEMU stops a malicious guest
*kernel*.

Several checks exist on both sides anyway, and the reasons differ. The guest
zeroes `IDLE_CHANNELS`' pointers because it cannot marshal three arrays through
one aux slot; the stub re-zeroes them because the guest cannot be trusted to
have done it.

## Why default-deny, and what it costs

Every allowlist is default-deny with an explicit provenance note. The cost is
that a driver branch issuing an unseen command fails, and the failure is one log
line plus a CUDA error code — which is what happened bringing up 535
(`src/qemu/nvkvm_ctrl_allowlist.h:218-239`).

That is the intended trade. The alternative — default-allow with a denylist —
would mean a new driver silently gaining reach to reg-ops, HWPM, debug, fabric
and power surfaces.

The corollary is that an allowlist entry without a handler is a liability. `0x70`
(`NV_ESC_EXPORT_TO_DMABUF_FD`) was allowlisted but unhandled, so the stub would
have created a dma-buf fd that nothing closed and nothing passed back — an fd
leak per call, and a meaningless fd returned to the guest. It was removed
(`src/qemu/nvkvm_fe_alloc_allowlist.h:40-45`).

## Why one sparse window instead of per-mapping memory slots

`src/qemu/nvkvm_isolate_handlers.c:2300-2311`:

> A single `cuCtxCreate` issues >1500 tiny (4 KB) device mmaps; one memslot each
> blows past both our pool and any sane slot count.

So: one 128 GiB `MAP_NORESERVE` reservation, one `KVM_SET_USER_MEMORY_REGION`,
and every device mapping is a `MAP_FIXED` slice into it. Zero per-mmap KVM
ioctls. Teardown restores anonymous backing rather than `munmap`ing, because a
`munmap` would punch a hole in the window's single VMA.

The window's base comes from a **BAR that has no backing at all**. A RAM-backed
BAR was tried and rejected (`src/qemu/virtio_nvgpu_pci.c:29-39`):

> a RAM BAR gets a QEMU-listener-managed KVM memslot that collides with the
> window's own raw `KVM_SET_USER_MEMORY_REGION` slot (proven by the earlier probe
> — it broke `cuInit`).

Registering a pure MMIO BAR makes guest firmware assign and reserve the range,
which is the only thing needed; the raw memslot is then installed at the
firmware-assigned address and shadows the MMIO region.

## Why cacheability is chosen per device class

Because it fails in opposite directions (`src/guest/nvkvm_mmap.c:175-191`). The
channel completion semaphore is pinned system memory and must be write-back —
mapping it write-combining made `cuCtxSynchronize`'s poll 8.9x slower and
dropped GPU utilisation to 0-5%. The doorbell BAR is real MMIO and must be
write-combining — a write-back mapping would leave the ring store in cache and
never reach the device, and decode would hang.

And requesting write-back is not sufficient: x86 silently downgrades it to `UC-`
on a non-RAM prefetchable BAR range, which was invisible on Intel (EPT forces WB
via IPAT) and cost ~100x on AMD (NPT honours the guest PTE). The module rewrites
the PTEs itself (`src/guest/nvkvm_mmap.c:44-109`).

Note the shape of both of those bugs: the *correct-looking* choice was wrong,
and the wrongness was a performance cliff rather than an error.

## Why the fast path is off by default

The SPSC command-buffer ring is fully implemented, validated on all three
parties, and disabled (`src/guest/nvkvm_main.c:414-424`):

> the ring is correct + validated (HW: 1446 flat RM_CONTROLs/decode offloaded,
> byte-exact matmul, 4x multi-proc, zero regression) but measurement showed it
> does NOT improve LLM decode throughput — control-RTT is only ~1-2% of per-token
> time (the bottleneck is GPU compute + the mapped doorbell/fence launch path),
> and keeping the isolate spinning costs host CPU.

That is the correct conclusion from the measurement, and it is also the clearest
statement of what the architecture actually is: the steady-state path was never
the ioctl path, so optimising the ioctl path does not move the needle. Enable it
for control-latency-bound workloads:

```
echo 1 > /sys/module/nvkvm_guest/parameters/ring_enable
```

## Why the ring has no futex

`src/common/nvkvm_ring.h:229-236`:

> The wait/wake mechanism is NOT a futex. The isolate's idle state is its
> existing blocking `recvmsg`; a virtqueue `enter_loop` call drives it into this
> consumer loop and stays in flight until the loop exits. All blocking lives on
> the guest side — there is no synchronisation word in guest-writable memory.

A futex word in shared memory would be a guest-writable value the host waits on.
The `_reserved_doorbell` field at `src/common/nvkvm_ring.h:53-57` is the fossil
of that abandoned design, kept only as padding so the region layout is
unchanged. (It has nothing to do with the GPU doorbell.)

Correctness then rests on a documented invariant rather than a primitive: the
consumer re-checks `has_work()` once more after deciding to exit, and the guest
is level-triggered — it re-evaluates after every `ENTER_LOOP` return rather than
assuming an in-flight session covers a just-published record
(`src/common/nvkvm_ring.h:243-247`, `src/guest/nvkvm_main.c:305-328`).

## Why the isolate is freestanding

No libc means a much smaller post-RCE syscall surface and no dynamic linker to
subvert. The seccomp allowlist is 20 syscalls, and the entries that had no
freestanding caller were deleted specifically to shrink it
(`src/stub/nvkvm_stub.c:2670-2672`).

It also makes the "no file on disk" property clean: the binary is a byte array
in QEMU, written to a memfd and `fexecve`'d
(`src/qemu/nvkvm_isolate.c:803-864`). The cost is self-relocation
(`src/stub/nvkvm_stub.c:2697-2745`) and hand-rolled `msghdr`/`cmsghdr`/`siginfo`
structs.

## Choices deliberately left as limitations

- **No scanout.** A DRM-backend compositor hangs inside NVIDIA's closed
  `libnvidia-egl-gbm` scanout path, which is coupled to `nvidia-modeset`. Frames
  leave by capture instead. This is intrinsic, not a bug.
  See [Known limitations](known-limitations.md#there-is-no-scanout-path--intrinsic).
- **NVKMS is still forwarded**, and its own allowlist calls that interim: the
  real fix is to stop forwarding it entirely in favour of the guest-side virtual
  head (`src/qemu/nvkvm_nvkms_allowlist.h`).
- **`IDLE_CHANNELS` degrades to single-channel**, because marshalling three
  arrays through one aux slot was judged not worth it for a call `libcuda` only
  makes at teardown (`src/guest/nvkvm_ioctl.c:462-479`).
- **UVM ioctls run in privileged QEMU**, because the driver requires the mmap
  and the `UVM_INITIALIZE` to share an `mm` and the mmap is what installs the
  KVM region. Mitigated by a default-deny schema
  (`src/qemu/nvkvm_isolate_handlers.c:1229-1237`, `:516-562`).
- **`GET_DRM_FILE_UNIQUE_ID` returns a different value than the host would**,
  because the host's is a kernel pointer (`src/guest/nvkvm_drm.c:433-455`).

## A note on how this codebase records its own mistakes

Most of the comments quoted throughout these docs describe a bug that was
actually shipped, with its symptom and often its measurement. That is unusual
and it is worth preserving as a convention, because the failure modes in this
domain share a property: **they are silent**.

- A wrong ABI struct size does not fail to compile — it forwards a truncated
  struct and the kernel reads past the buffer
  (`tools/abi_derive.sh:5-12`).
- A wrong field offset returns `-EBADF` from inside the guest, so the host log
  shows no error at all (`src/guest/nvkvm_ioctl.c:301-312`).
- A missing guest library leaves the staging script printing "done"
  (`scripts/stage_guest_libs.sh:63-67`).
- A missing EGL external platform yields a correctly-sized black window that
  looks like a working desktop in a screenshot
  (`scripts/stage_guest_libs.sh:153-163`).
- A missing QEMU build define produces a binary that builds fine and comes up
  with forwarding off (`scripts/build_qemu.sh:212-225`).
- A wrong cacheability choice is a 100x slowdown on one CPU vendor and invisible
  on the other (`src/guest/nvkvm_mmap.c:44-61`).

When you change something here, assume the failure will not announce itself. Say
so in the comment.
