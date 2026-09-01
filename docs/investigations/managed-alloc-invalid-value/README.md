# `cuMemAllocManaged` returns `CUDA_ERROR_INVALID_VALUE` in the guest

**Measured 2026-09-01.** GTX 750 Ti (GM107, sm_50), driver 535.309.01, guest
kernel 6.8.0-138. Found by the first Maxwell sweep that got far enough to reach
it — see the progression in `exp/pre-turing`'s log.

## Result

The control is the finding. Same GPU model, same driver, one side bare metal
and one side an nvkvm guest:

| | bare metal | guest |
|---|---|---|
| `sm` | 50 | 50 |
| `MANAGED_MEMORY` | **1** | **1** |
| `CONCURRENT_MANAGED_ACCESS` | 0 | 0 |
| `cuInit` / `cuCtxCreate` | 0 / 0 | 0 / 0 |
| `cuMemAllocManaged(4 MiB, ATTACH_GLOBAL)` | **`CUDA_SUCCESS`** | **`CUDA_ERROR_INVALID_VALUE`** |
| `cuMemAllocManaged(4 MiB, ATTACH_HOST)` | `CUDA_SUCCESS` | not tried |
| `cuMemAlloc(4 MiB)` — plain device memory | — | **`CUDA_SUCCESS`** |

Device attributes are **identical on both sides**. Plain device allocation
works in the guest. Only the managed (unified-memory) allocation fails, and
only in the guest.

So this is **not** a Maxwell limitation. `CONCURRENT_MANAGED_ACCESS=0` is
normal for pre-Pascal and is true on bare metal too, where the same call
succeeds.

## Why it is worth more than one bug

This is the **second** independently-controlled instance of one shape:

| | hardware | API | advertised | operation |
|---|---|---|---|---|
| 1 | RTX 4070 / Ada | Vulkan | 7 device extensions enumerate as supported | `vkCreateDevice` → `VK_ERROR_INITIALIZATION_FAILED` |
| 2 | GTX 750 Ti / Maxwell | CUDA | `MANAGED_MEMORY=1` | `cuMemAllocManaged` → `CUDA_ERROR_INVALID_VALUE` |

Different GPUs, different architectures, different hosts, different APIs. In
both, a **capability query answers yes and the operation it promises fails** —
with nothing denied, nothing refused, and nothing logged. See
`vkcreatedevice-init-failed/`.

## Lead, not a conclusion

UVM is the obvious common thread and has not been confirmed.
`cuMemAllocManaged` is UVM by definition; of the seven Vulkan extensions,
`VK_NV_cuda_kernel_launch` and `VK_NVX_binary_import` are CUDA interop and the
ray-tracing ones need buffer device addresses and acceleration-structure
memory.

`nested-nvkvm-l2/` is the precedent to read first: libcuda was refused a UVM
map (`UVM_MAP_EXTERNAL_ALLOCATION`) and gave up with **no `DENY` line, no
non-zero `nvstatus`, no failing ioctl**. The same silence, in the same
subsystem, already documented here. That root cause was an fd translation a
schema table did not cover.

## Not findings

- **`CONCURRENT_MANAGED_ACCESS=0` is not the cause.** It reads 0 on bare metal
  too, where the call succeeds.
- **This is not what makes RDR2 fail.** That is the Vulkan case, on different
  hardware. They may share a cause; that is unproven.
