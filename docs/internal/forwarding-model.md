# The forwarding model

How one ioctl becomes one ioctl on the other side of a VM boundary. This page is
the detail behind
[ARCHITECTURE.md § the request path](../../ARCHITECTURE.md#the-request-path-end-to-end)
and [§ nested guest pointers](../../ARCHITECTURE.md#problem-2-nested-guest-pointers).

## The invariant

**No guest virtual address is ever forwarded, and the boundary — not the guest —
is what guarantees it.**

The guest zeroes every pointer-sized field carrying a guest VA before the call
crosses. The stub then *overwrites* those fields unconditionally with pointers
into buffers it owns. If the guest skipped its half, nothing changes: the stub
does not read the field, it writes it.

The design principle is stated at the site where it was most nearly violated
(`src/qemu/nvkvm_dispatch.c:383-385`):

> the boundary (not the untrusted guest) must ensure no guest pointer is ever
> forwarded. Always overwrite the `p_*` fields.

## Guest side, step by step

`nvkvm_ioctl()` — `src/guest/nvkvm_main.c:1018-2338`. Registered as both
`unlocked_ioctl` and `compat_ioctl` (`:83-84`), so 32-bit callers take the
identical path with no compat translation.

### 1. Size lookup

`nvkvm_ioctl_param_size(cmd)` (`src/guest/nvkvm_ioctl.c:46-201`) resolves in
three tiers:

- the NVKMS wrapper, matched on the **full** command word;
- UVM, matched on the full 32-bit word — UVM has its own numbering starting at
  `0x30000001` (`src/guest/nvkvm_ioctl.c:43-45`);
- frontend ioctls, matched on `_IOC_NR` only, "consistent with how the NVIDIA
  driver ignores IOC_TYPE" (`:40-41`).

Unknown → `-ENOTTY`. Three sizes come from the ABI profile rather than `sizeof`
(`UVM_MAP_EXTERNAL_ALLOCATION`, `UVM_ALLOC_SEMAPHORE_POOL`,
`NV_ESC_RM_MAP_MEMORY_DMA`); a handful take the size from `_IOC_SIZE`.

A fidelity audit then compares the resolved size against `_IOC_SIZE(cmd)` and
logs every mismatch (`src/guest/nvkvm_main.c:1042-1056`):

> if our `param_size` differs from the size the caller encoded in the cmd
> (`_IOC_SIZE`), we will forward a TRUNCATED/over-long, malformed buffer to the
> host kernel — the low bytes match but the call is wrong (cf. the NVOS32
> 88-vs-184 bug).

UVM commands are excluded from the audit because their command numbers encode a
bogus `_IOC_SIZE` that the UVM driver ignores.

### 2. Copy in

Capped at 64 KiB (one shm slot). `NULL` args are tolerated because
`libnvml` calls `UVM_DEINITIALIZE` with one and the native driver accepts it
(`src/guest/nvkvm_main.c:1061-1078`).

### 3. Save originals — before anything mutates them

`src/guest/nvkvm_main.c:1207-1303`. Every pointer, size and fd field that will
be blanked is snapshotted, because CUDA verifies they round-trip unchanged. The
ordering constraint is a shipped-and-fixed bug (`:1207-1217`):

> Bug we already hit: capturing AFTER the inline sanitizer captured
> `alloc_parms_size = 0x38` (the class-derived size we filled in for the host
> driver) instead of CUDA's original 0 — and the restore then "restored" that
> bogus value back over the kernel's writeback.

### 4. Stage the secondary buffer in the aux slot

`src/guest/nvkvm_main.c:1341-1352`:

> For ioctls with embedded secondary buffers: extract the secondary data BEFORE
> the sanitizer zeroes the pointer fields. We carry the data in the aux slot so
> the host can reconstruct it without ever seeing a raw guest VA.

Four producers of aux data:

**NVKMS** (`:1360-1418`) — the wrapper's `address` at offset 8 points at `size`
bytes the kernel reads *and* writes. Copy in, zero the pointer. For
`REGISTER_SURFACE` (cmd 17) with `useFd` set, up to three plane fds at
stride 32 from offset 16 are swapped for handle ids.

**`NV_ESC_RM_CONTROL`** (`:1419-1640`) — `aux = [params, params_size)`. Then a
series of per-command extensions that *grow* the aux blob so out-pointers inside
the inner params can be serviced:

| inner command | what is staged |
|---|---|
| `GET_PID_INFO` | each guest pid → `0x80000000 \| isolate_id`, originals saved |
| `EXPORT_OBJECT_TO_FD` (0x3d05) | fd@16 → handle id |
| `IMPORT_OBJECT_FROM_FD` (0x3d06) | fd@0 → handle id |
| `FIFO_GET_CHANNELLIST` | two IN+OUT lists appended; both pointers zeroed; bounded at 4096 channels |
| the `InfoList`/`Caps` family | one list appended, pointer zeroed; bounded at 65536 entries |
| `SYSTEM_GET_BUILD_VERSION` | three string buffers appended (≤512 each); all three pointers zeroed |
| `GPU_GET_ID_INFO` (0x202) | `szName`@16 simply zeroed, not marshalled |

The `InfoList`/`Caps` unification is driven by one function
(`src/guest/nvkvm_main.c:982-992`):

> RM_CONTROL commands whose inner params begin with an embedded list preamble
> `{ u32 count@0; pad@4; NvP64 ptr@8 }` that the driver writes through. […] The
> two families are structurally identical (`u32@0`, `ptr@8`); only the unit of
> the count differs, so they share one handler parameterised by this size.

Entry size is 8 for the `GET_INFO` family, 4 for `GPU_GET_ENGINES` and
`GPU_GET_CLASSLIST`, 1 for the `GET_CAPS` family whose count is a byte length.
The same table exists in the stub and is marked "MUST stay in sync"
(`src/stub/nvkvm_stub.c:452-455`).

`GPU_GET_ID_INFO` gets the cheap treatment for a stated reason
(`src/guest/nvkvm_main.c:1628-1637`):

> The name is optional — cuInit doesn't need it and nvidia-smi gets the model
> elsewhere — so zero the pointer rather than forward a raw guest VA the stub
> would deref (the driver null-checks szName). This closes the last unsanitized
> embedded pointer among the allowlisted control cmds.

**`NV_ESC_RM_ALLOC`, NVOS21** (`:1641-1760`) and **NVOS64** (`:1761-1928`) — the
class-specific alloc params. Size comes from a large `hClass` switch; NVOS64
prefers the caller's `alloc_parms_size` and falls back to the class table when
it is 0, because "CUDA often leaves `alloc_parms_size=0` and relies on the
driver to size the buffer by hClass" (`:1771-1775`).

Several entries in that table are documented bug fixes and are the fastest way
to understand what a missing entry costs:

| class | comment |
|---|---|
| `NV50_THIRD_PARTY_P2P` (0x503c) | "libcuda allocs this during cuInit with `alloc_parms_size=0`. Without this case we copy 0 bytes → host RM returns `NV_ERR_NOT_SUPPORTED` (0x56) and cuInit fails `CUDA_ERROR_NOT_SUPPORTED` (801)" (`:1691-1696`) |
| `NV01_MEMORY_VIRTUAL` (0x70) | "libGLX's EGL device enum allocs this with `alloc_parms_size=0`; without this the kernel sees `hVASpace=0` → `NV_ERR_INVALID_ARGUMENT`" (`:1665-1670`) |
| `NV_SEMAPHORE_SURFACE` (0xda) | "the kernel sees empty params → `NV_ERR_INVALID_ARGUMENT` (0x1f) and `libnvidia-eglcore` later NULL-derefs the missing object" (`:1673-1678`) |
| `NV01_CONTEXT_DMA` (0x0002) | "NVENC binds a context-DMA with size=0 … and `InitializeEncoder` fails" (`:1680-1685`) |

After the NVOS64 copy-in, `alloc_parms_size` is written back so the host driver
sees a consistent `(params, paramsSize)` pair (`:1889-1891`).

### 5. Sanitise

`nvkvm_sanitize_ioctl_params()` — `src/guest/nvkvm_ioctl.c:253-554`. Two tiers,
and the gate between them matters (`:334-341`):

> Bare UVM ioctls (e.g. `UVM_PAGEABLE_MEM_ACCESS` = 0x27) use type 0 and their NR
> can collide with frontend NRs (`NV_ESC_RM_ALLOC_MEMORY` is also 0x27). Without
> this gate we'd reinterpret an 8-byte UVM struct as a 48-byte nvos02-with-fd,
> read garbage as `p->fd`, `fget()` → EBADF.

Frontend actions: zero `p_alloc_parms`/`p_rights_requested`, zero
`nvos54.params`, zero `p_linear_address` on NVOS33/34/56, and translate embedded
fds.

### 6. Forward, with a bounded EFAULT retry

```c
#define NVKVM_MAX_EFAULT_RETRIES 128
for (retries = 0; retries < NVKVM_MAX_EFAULT_RETRIES; retries++) {
        ret = nvkvm_virtio_ioctl_on_isolate(...);
        if (ret != -EFAULT || !fault_addr)
                break;
        ioctl_flags |= NVKVM_IOCTL_FL_RETRY_EFAULT;
        if (nvkvm_efault_resolve(ctx, fault_addr))
                { ret = -EFAULT; break; }
}
```
— `src/guest/nvkvm_main.c:2017-2034`

`nvkvm_efault_resolve()` (`src/guest/nvkvm_mmap.c:566-615`) finds the VMA
covering the fault and branches: a GPU VMA (`vm_ops == &nvkvm_vm_ops`) gets a
re-sent `MMAP_ON_ISOLATE`; an ordinary CPU VMA gets the faulting page pinned and
uploaded to a memfd.

### 7. Restore and copy back

Pointer restoration (`src/guest/nvkvm_main.c:2086-2161`):

> CUDA expects to read its own pointer back unchanged across the ioctl; not
> restoring them causes cuInit to give up with `CUDA_ERROR_NO_DEVICE`.

Params are copied back **even on driver error** (`:2163-2174`):

> The NVIDIA RM may populate response fields even when returning an error (e.g.
> `CHECK_VERSION_STR` fills `version_string` and returns EINVAL). Only skip if
> the transport itself failed.

Aux copy-out has to undo each extension: `FIFO_GET_CHANNELLIST` re-reads the
original user struct to recover both list pointers and writes each list back to
its original user address; the `InfoList` family does the same; the three
version strings are scattered back; NVKMS plane fds are restored
(`:2176-2332`).

## The VMA whitelist

Every forwarded ioctl carries a snapshot of `current->mm`'s VMAs — `{start, end,
prot}` per entry, capped at 1024 — in a third shm slot
(`src/guest/nvkvm_virtio.c:1176-1231`,
`src/common/nvkvm_proto.h:444-453`). It is the isolate's authority for what a
demand fault may legitimately map.

`current->mm` can be `NULL`, and that used to be a hard oops
(`src/guest/nvkvm_virtio.c:1176-1190`):

> a forwarded ioctl may run from a context with no user mm — most notably
> `GEM_CLOSE` forwarded out of `nvkvm_gem_free()` during `drm_release()` at
> PROCESS EXIT, where the kernel has already run `exit_mm()` before
> `exit_files()` closes the DRM fd. `mmap_read_lock(NULL)` then faults at
> `&NULL->mmap_lock` (offset 0xb0) — a hard oops that, for a compositor holding
> DRM master, leaves the master stuck and wedges all later modeset (`SET_MASTER`
> EBUSY).
>
> No mm means no guest VAs to whitelist, and the commands that reach this path
> without an mm (`GEM_CLOSE`) carry no embedded user pointers anyway, so skipping
> the whitelist is correct, not a workaround.

## QEMU side

The worker copies the shared-memory param and aux blobs into **private heap
buffers** before running the allowlist checks, and copies them back after
(`src/qemu/virtio_nvgpu.c:641-661`). This is what makes the checks meaningful:
a guest cannot mutate a field between the check and the use.

Then the six allowlist gates, in order — see
[Allowlists](../reference/allowlists.md).

## Stub side

`worker_thread()` — `src/stub/nvkvm_stub.c:812-1453`.

**Overwrite the pointer.** Offset 16 for `NVOS54.params` and
`NVOS21`/`NVOS64.p_alloc_parms` (four 4-byte handles precede them); offset 8 for
the NVKMS wrapper and two DRM ioctls
(`src/stub/nvkvm_stub.c:838-866`). The target, `job.aux_buf`, is a private
anonymous mapping filled by `recv()` — not shared memory, not guest-writable
(`:1857-1862`).

**Reconstruct inner pointers** into the aux blob's extension region: the
`InfoList` family, `FIFO_GET_CHANNELLIST`'s two lists,
`GET_BUILD_VERSION`'s three strings (`:945-1076`).

**Translate embedded fds**, handle id → local fd, immediately before the ioctl:

| ioctl | offset |
|---|---|
| `NV_ESC_RM_MAP_MEMORY` (0x4e) | 48 |
| `NV_ESC_RM_ALLOC_MEMORY` (0x27) | 48 |
| `NV_ESC_ALLOC_OS_EVENT` (0xce), `FREE_OS_EVENT` (0xcf) | 8 |
| `NV_ESC_REGISTER_FD` (0xc9) | 0 |
| UVM `MM_INITIALIZE` (75) | 0 |
| UVM `REGISTER_GPU_VASPACE` (25), `REGISTER_CHANNEL` (27) | 16 |
| UVM `MAP_EXTERNAL_ALLOCATION` (33) | **from the ABI profile** |
| `RM_ALLOC` `NV01_EVENT_OS_EVENT` (0x79) | aux+16 (`Data`) |
| `EXPORT_OBJECT_TO_FD` | aux+16 |
| `IMPORT_OBJECT_FROM_FD` | aux+0 |
| NVKMS `REGISTER_SURFACE` planes | aux+16, +48, +80 |

(`src/stub/nvkvm_stub.c:1084-1219`, `:869-1025`.)

**Run the ioctl**, publishing `worker_inflight_txn[slot]` first so a concurrent
`INTERRUPT` can find it (`:1277-1286`).

**Restore and zero.** Every substituted pointer is zeroed and every handle id
put back — "never leak a stub fd" (`:1299-1369`).

**Extract `nvstatus`** by `_IOC_NR`, not by size, because multiple structs share
a length but place `Status` at different offsets (`:1379-1417`). `NVOS46`'s
offset comes from the ABI profile.

## Per-command special cases worth knowing

### `OS_DESCRIPTOR` pinning

`NV_ESC_RM_ALLOC_MEMORY` with `hClass = 0x71` pins **the calling task's** pages
— and the calling task is the stub. The guest migrates the range onto memfds the
stub already maps at the same VA, so `pin_user_pages` finds tmpfs pages that
alias `libcuda`'s guest userspace (`src/guest/nvkvm_ioctl.c:379-412`, full quote
in [ARCHITECTURE.md](../../ARCHITECTURE.md#the-other-direction-os_descriptor)).
Chunked at 2 MiB, capped at 16 MiB per call
(`src/guest/nvkvm_mmap.c:770-785`).

### `IDLE_CHANNELS` degrades to single-channel

Three guest pointers to handle arrays that the single-aux-slot path cannot
carry. The guest forces the single-channel form
(`src/guest/nvkvm_ioctl.c:462-479`) and the stub re-zeroes the same 28 bytes at
the boundary on a private copy (`src/stub/nvkvm_stub.c:1180-1196`). The
integer-overflow hardening on the aux-slot variant of this path is at
`src/qemu/nvkvm_dispatch.c:383-403`.

### Two ioctls are faked successful

`NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO` and `NV_ESC_RM_UNMAP_MEMORY` look up a
CPU mapping by the caller's VA in the host driver's per-process table. The stub
registered the mapping at its own VA; `libcuda` passes a guest VA. The lookup
always returns `NV_ERR_OBJECT_NOT_FOUND` (`src/guest/nvkvm_main.c:1305-1339`):

> The mapping still works because we install the BAR pages into the guest's GPA
> window via QEMU, so this ioctl is effectively informational. We save the
> caller's values so we can fake success on the response path.

### `RM_SHARE` after every successful alloc

The kernel's default share policy is `RS_SHARE_TYPE_PID`. In a split-process
model the PIDs differ, so UVM cannot dup `libcuda`'s VA space and `cuCtxCreate`
fails `NV_ERR_INSUFFICIENT_PERMISSIONS`. QEMU issues an `NV_ESC_RM_SHARE`
granting `RS_ACCESS_DUP_OBJECT` on each new handle
(`src/qemu/nvkvm_isolate_handlers.c:1490-1610`). Why `TYPE_ALL` is not a
cross-tenant hole is argued at `:1561-1586` and demonstrated by
`tests/security/poc_cross_proc_dup`.

### `UVM_REGISTER_CHANNEL` recording is skipped

Deliberately (`src/guest/nvkvm_main.c:1168-1171`): "libcuda calls it but the
kernel-side channel is already known via RM handles — recording is for parity,
not correctness."

## Reading pitfalls

- **`src/qemu/nvkvm_dispatch.c` and `src/qemu/nvkvm_frontend.c` are the old
  synchronous path** where QEMU ran the ioctl itself. The live path is
  `IOCTL_ON_ISOLATE` → the stub. `src/qemu/nvkvm_dispatch.c:375-379` says so.
- **`REALIZE_UVM_MAPPING` is unreachable** — the guest call site is disabled
  behind a `(void)` cast (`src/guest/nvkvm_mmap.c:270-275`). The transport, the
  QEMU handler and the stub handler are all present and all dead.
- **The legacy `NVKVM_REQ_IOCTL` guest transport is `#if 0`'d** and references a
  struct field that no longer exists (`src/guest/nvkvm_virtio.c:486-595`). It
  will not compile if re-enabled; it is not a fallback.
- **Two mmap-teardown call sites bypass the txn-id allocator**, using a raw
  counter instead of the `(epoch<<12)|slot` encoding
  (`src/guest/nvkvm_mmap.c:207-208`, `:726-727`). Harmless today because demux
  is by data cookie, but it can produce spurious "txn_id mismatch" warnings, and
  the stack `struct nvkvm_inflight` at those sites leaves `isolate_id`
  uninitialised — which is the field that selects the interruptible wait path.
