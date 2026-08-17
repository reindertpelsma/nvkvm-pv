# Device nodes in the guest

The guest module presents the NVIDIA device surface that unmodified NVIDIA
userspace expects to find. Every node is a plain character device backed by the
same `file_operations` (`src/guest/nvkvm_main.c:79-87`), except the DRM render
node, which is a real DRM device.

| node | major | minor | created at |
|---|---|---|---|
| `/dev/nvidiactl` | 195 preferred, else dynamic | 255 | `src/guest/nvkvm_main.c:117-132` |
| `/dev/nvidia0` … `/dev/nvidiaN-1` | 195 preferred, else dynamic | 0…N-1 | `:134-154` |
| `/dev/nvidia-uvm` | dynamic | 0 | `:156-169` |
| `/dev/nvidia-uvm-tools` | same dynamic major | 1 | `:170-171` |
| `/dev/nvidia-modeset` | 195 required | 254 | `:173-204` |
| `/dev/dri/renderD128` (+ `card0`) | DRM core (226) | — | `src/guest/nvkvm_drm.c:845-870` |

All nodes are created through one `struct class` whose `devnode` callback forces
mode **0666** (`src/guest/nvkvm_main.c:100-105`), so unprivileged guest
processes can open them. udev creates the nodes; no manual `mknod` is needed.

## Why major 195

`src/guest/nvkvm_main.c:91-98`:

> libnvidia-ml and nvidia-smi call `mknodat(195, 255)` before opening nvidiactl
> (the major is hardcoded in the library). We must register at that exact major
> so the device can be opened after the library recreates the node. Similarly,
> nvidia0 lives at major 195 minor 0 in the real NVIDIA driver. Fall back to
> dynamic allocation if 195 is already taken.

`register_chrdev_region()` first, `alloc_chrdev_region()` as fallback, for
`nvidiactl` and the GPU nodes. `/dev/nvidia-modeset` has no fallback — it needs
195:254 or it is skipped, non-fatally (`src/guest/nvkvm_main.c:185-204`).

## Identifying a device at open

`nvidiactl` and `nvidia0..N` share major 195, so both major and minor must be
checked (`src/guest/nvkvm_main.c:760-778`):

> nvidiactl and nvidia0..N share major 195 (`NV_NVIDIA_MAJOR`) so we MUST check
> BOTH major AND minor to identify nvidiactl (minor=255). Checking major alone
> incorrectly classifies every nvidia0 open as `NVKVM_DEV_CTL`.

The resulting `dev_id` is the protocol-level device identifier
(`src/common/nvkvm_proto.h:96-100`):

| constant | value |
|---|---|
| `NVKVM_DEV_CTL` | 0 |
| `NVKVM_DEV_UVM` | 1 |
| `NVKVM_DEV_GPU(n)` | 16 + n |
| `NVKVM_DEV_DRM_RD(n)` | 32 + n |
| `NVKVM_DEV_MODESET` | 48 |
| `NVKVM_DEV_EVENTFD` | 0xFF |

`NVKVM_DEV_EVENTFD` is not a device node — it is how QEMU materialises a real
host `eventfd` to stand in for a guest one that `libcuda` passes to
`RM_ALLOC NV01_EVENT_OS_EVENT` (`src/qemu/nvkvm_handle.c:107-118`).

UVM is identified by major alone, since its major is dynamic and therefore
distinct. A consequence: **`/dev/nvidia-uvm-tools` is indistinguishable from
`/dev/nvidia-uvm` at the `dev_id` level** — both open as `NVKVM_DEV_UVM`.

## `/dev/nvidia-uvm-tools` exists only to prevent a `mknod`

`src/guest/nvkvm_main.c:156-159`:

> libcuda mknods `/dev/nvidia-uvm-tools` if absent and fails with
> `CUDA_ERROR_OPERATING_SYSTEM` (304) when mknod is denied — the simplest fix is
> to expose it from the module.

Both minors come from a single dynamic region and a single `cdev` spanning two
minors.

## `/dev/nvidia0` count

Controlled by the read-only module parameter `num_gpus` (default 1), clamped to
16 (`src/guest/nvkvm_main.c:67-69`, `:134-135`). QEMU independently enumerates
the host's `/dev/nvidia0..15` when the guest asks
(`src/qemu/nvkvm_isolate_handlers.c:121-129`).

## `/dev/nvidia-modeset` is doubly gated

It appears only when both:

- the module was built with graphics (`NVKVM_GRAPHICS=1`, the default), and
- QEMU advertised `NVKVM_CONFIG_F_GRAPHICS` for this VM.

The ordering that makes the second check work is deliberate
(`src/guest/nvkvm_main.c:180-184`):

> the virtio probe runs before `register_devices` (`register_virtio_driver`
> probes synchronously), so `graphics_enabled` already reflects the host's
> `NVKVM_CONFIG_F_GRAPHICS` by now; compute-only VMs never create the modeset
> device.

The device exposes exactly one ioctl — `_IOWR('m', 0, NvKmsIoctlParams)` =
`0xC0106D00`, a 16-byte wrapper `{u32 cmd; u32 size; u64 address}` whose single
embedded pointer is staged in the aux slot
(`src/common/nvkvm_proto.h:102-112`). Its inner `cmdType` is allowlisted
separately; see [Allowlists](allowlists.md#5-nvkms-inner-command-allowlist).

## `/dev/dri/renderD128` is a real DRM device

Not a raw cdev. `src/guest/nvkvm_drm.c:5-11`:

> The NVIDIA Vulkan/EGL userspace enumerates the GPU through the DRM render node
> `/dev/dri/renderD128`, NOT through `/dev/nvidia*` (which carry compute). The
> ICD stats the node, derives its rdev major, and requires
> `/sys/dev/char/<major>:128/device/drm` to exist before opening it. Both the
> canonical DRM major (226) and that sysfs tree are owned by the kernel DRM core
> and can only be obtained by registering a real DRM device — a raw cdev cannot
> claim major 226. So we register a render-only `drm_driver` here.

Driver features are `DRIVER_RENDER | DRIVER_GEM | DRIVER_MODESET |
DRIVER_ATOMIC` (`src/guest/nvkvm_drm.c:766`), so the core creates both
`renderD128` and a primary `card0` — the virtual KMS head lives on the same
device so render and scanout share one DRM device with no cross-device PRIME
(`:763-765`).

It reports itself as `nvidia-drm`, `"NVIDIA DRM driver"`, date `20160202`,
version 0.0.0 (`src/guest/nvkvm_drm.c:775-781`), verified against the host's
nvidia-drm. `DRM_IOCTL_VERSION` is answered by the guest DRM core from those
fields and never forwarded.

The node's sysfs parent is the `nvkvm-gpu` identity PCI device, claimed by a
guest PCI driver literally named `"nvidia"` because the ICD reads
`DRIVER=nvidia` from the device's uevent (`src/guest/nvkvm_drm.c:784-812`). The
probe is a no-op — it touches no registers and no BARs.

Its ioctl table wires eight entries, all `DRM_RENDER_ALLOW`, indexed by
`nr - DRM_COMMAND_BASE`; gaps have a `NULL` `.func`, which the DRM core rejects
with `-EINVAL` (`src/guest/nvkvm_drm.c:686-720`). Two are answered locally
rather than forwarded: `GEM_IDENTIFY_OBJECT` (0x0e), and
`GET_DRM_FILE_UNIQUE_ID` (0x18), the latter because the host implementation
returns a host kernel pointer (`src/guest/nvkvm_drm.c:433-455`):

> which would leak a host heap address across the VM boundary (KASLR-defeat
> aid), the exact class of leak the render-node allowlist exists to block.

## The `/proc` and `/sys` shims

Not device nodes, but part of the same surface. `libcuda` parses
`/proc/driver/nvidia/params` and checks `/sys/module/nvidia{,_uvm}/initstate`
during `cuInit`/`cuCtxCreate`; the real `nvidia.ko` is not loaded in the guest,
so without these it bails into `CUDA_ERROR_UNKNOWN` (999)
(`src/guest/nvkvm_hostfile.c:1-15`).

The module installs proc entries and sysfs kobjects that delegate each read to
the host over `NVKVM_REQ_READ_HOST_FILE`:

```
/proc/driver/nvidia/params
/proc/driver/nvidia/gpus/0000:00:07.0/information
/proc/driver/nvidia/gpus/0000:00:07.0/registry
/sys/module/nvidia/initstate
/sys/module/nvidia_uvm/initstate
```

The sysfs kobjects are created under `/sys/module/`, so they appear as sibling
fake modules named `nvidia` and `nvidia_uvm`
(`src/guest/nvkvm_hostfile.c:135-170`).

The BDF in those paths is the **guest** BDF, hardcoded
(`src/guest/nvkvm_hostfile.c:28-31`), which is why the QEMU command line places
the identity device at `addr=7`.

The selection enum is the security boundary: the guest names a `file_id`, never
a path. See [the virtio protocol](virtio-protocol.md#read_host_file).

## Session and fd identity

An `open()` of any of these nodes binds the fd to a **session**, keyed by
`mm_struct` — the address space, not the tgid
(`src/guest/nvkvm.h:44-56`):

> Linux reuses tgids after a process exits; if a previous tgid-keyed session
> lingered (refcount race) the new process with the same tgid would inherit the
> stale session, isolate, and handle table — a cross-uid info-leak path inside
> one VM. `mm_struct *` is a strong identity (refcounted, never reused while
> alive), so equal-mm means same process; threads share an mm so they share a
> session (correct); fork creates a new mm so the child gets a new session
> (correct).

Tasks sharing an address space via `CLONE_VM` without `CLONE_THREAD` are folded
into one session deliberately — they can already read and write each other's
memory, so they are one security domain
(`src/guest/nvkvm_session.c:36-47`).

An fd bound at `open()` keeps pointing at the session it was opened in. An fd
inherited across `fork()` therefore continues to use the parent's session and
isolate; there is no re-keying on fork.

Each fd's context is refcounted separately from the session, because a proxy GEM
object can outlive its `drm_file` via a cross-file PRIME re-import and still
needs to forward `GEM_CLOSE` on the exact context that owns the host fd
(`src/guest/nvkvm.h:198-206`).

## Non-forwarded ioctls

Two ioctls never leave the guest:

- **`NV2080_CTRL_CMD_GPU_GET_PIDS`** is synthesised entirely from the module's
  session table (`src/guest/nvkvm_main.c:820-893`). Forwarding it would be
  wrong twice over: the stub runs in its own pid namespace as a distinct RM
  client, so the host RM cannot see other isolates' pids, and the pids it would
  report are host pids meaningless in the guest. Each session's pid is rendered
  through `pid_vnr()` in the *caller's* namespace, so a process invisible from
  the caller's namespace is skipped — which gives correct isolation for
  Docker-inside-the-guest.
- **`UVM_VALIDATE_VA_RANGE`** is served from a 16-entry per-session cache
  (`src/guest/nvkvm.h:104-113`):

  > libcuda re-validates the SAME (base,len) range thousands of times per
  > pageable cuMemcpy (~191µs forwarded each → the DtoH bottleneck). VALIDATE is
  > an idempotent registration check, so we cache its rm_status and serve
  > repeats locally. Conservatively cleared on ANY UVM free/unmap/unregister so
  > a stale "valid" can never outlive a teardown.

Its sibling `NV2080_CTRL_CMD_GPU_GET_PID_INFO` takes the opposite route: it *is*
forwarded, but each guest pid is rewritten to `0x80000000 | isolate_id` for QEMU
to resolve to a real host pid, and restored on the way back
(`src/guest/nvkvm_main.c:895-961`, `src/qemu/nvkvm_isolate_handlers.c:1351-1407`).
