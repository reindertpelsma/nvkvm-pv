# Allowlists

Every allowlist in `nvkvm` lives in QEMU, is default-deny, and is checked before
anything reaches the host driver. The reason they are in QEMU and not in the
guest module is stated at each site: the guest kernel module is inside the trust
boundary being defended against, so a check it performs is advisory at best.

> This is a HOST/cross-VM attack-surface control (reg-ops/HWPM/debug/fabric fall
> out automatically); it lives in QEMU because the guest kernel module is
> untrusted.
>
> — `src/qemu/nvkvm_ctrl_allowlist.h:20-22`

There are nine gates, numbered below in the order an ioctl meets them. Six of
them are static default-deny tables — gates 1 and 4-8, which is the count
[`ARCHITECTURE.md`](../../ARCHITECTURE.md#the-allowlists) uses; the other three
(graphics, ioctl type, per-VM `hClient`) are code checks with no table. Seven
of the nine (1, 3-8) live in `nvkvm_req_ioctl_on_isolate()`
(`src/qemu/nvkvm_isolate_handlers.c:1223-2219`); the graphics gate is
authoritatively enforced earlier, at handle open, and the per-VM `hClient` set
is consulted from two places inside the same function.

## 1. UVM command schema (default-deny)

`src/qemu/nvkvm_isolate_handlers.c:599-645`. 31 entries. UVM ioctls are the one
family that executes in QEMU's own (privileged) process rather than in an
isolate, so an unknown `cmd` must never be forwarded blindly.

Each entry is `{ cmd, min_size, fd_off[2] }`: the minimum parameter size, and
the byte offsets of any embedded frontend-fd fields that need
handle-id→host-fd translation. `0xffff` means "no fd field".

Sizes are the exact `sizeof` from `src/abi/uvm.h`, verified against NVIDIA's
official OGKM tags rather than copied from a neighbouring nvproxy release:

> `min_size` is the EXACT struct size from our ABI (`src/abi/uvm.h`), verified
> against OGKM rather than copied from a neighbouring nvproxy release.
> `REGISTER_GPU` is 40 bytes at every one of the 216 supported OGKM tags;
> `REGISTER_CHANNEL` remains 48 rather than 56 and `MIGRATE` 48 rather than 56.
>
> — `src/qemu/nvkvm_isolate_handlers.c:589-598`

Five commands (44, 45, 53, 65, 66) carry `min_size = 0` deliberately: there is no
driver-verified layout for them and an over-strict guess had already mis-denied
`REGISTER_GPU` once. `REGISTER_GPU.rmCtrlFd` is an input fd at offset 24 and is
translated through the per-VM handle table before forwarding.

Two `min_size` values are version-variant and are overridden from the active ABI
profile at check time — `UVM_MAP_EXTERNAL_ALLOCATION` (33) and
`UVM_ALLOC_SEMAPHORE_POOL` (68) — because the table carries the V550 sizes and a
535 host legitimately sends the pre-V550 ones
(`src/qemu/nvkvm_isolate_handlers.c:1276-1290`).

Notably denied by omission: `UVM_TOOLS_READ_PROCESS_MEMORY` (62) and
`UVM_TOOLS_WRITE_PROCESS_MEMORY` (63) —

> a cross-process memory peek/poke primitive with no place in our isolation
> model
>
> — `src/qemu/nvkvm_isolate_handlers.c:641-644`

## 2. Graphics gate

`src/qemu/nvkvm_isolate_handlers.c:1555-1564`. When the device is built or
configured compute-only, every DRM (`_IOC_TYPE == 'd'`) and NVKMS ioctl is
refused. This is defence in depth — the authoritative enforcement is at handle
open (`src/qemu/nvkvm_isolate_handlers.c:199-213`), which refuses to open the
render node or the modeset device at all, so a guest that ignores the cleared
`NVKVM_CONFIG_F_GRAPHICS` config bit still has no fd to issue ioctls on.

Two knobs: the runtime `graphics=on|off` device property
(`src/qemu/virtio_nvgpu.c:1444-1449`, default on) and the compile-time
`NVKVM_QEMU_GRAPHICS=0` which forces it off *and* compiles the host EGL present
path out entirely (`src/qemu/virtio_nvgpu.h:33-50`). The guest module has the
matching `NVKVM_GRAPHICS=0` build which drops `nvkvm_drm.o` / `nvkvm_kms.o`
(`src/guest/Kbuild:14-21`). Deploy the two consistently.

## 3. ioctl type gate

`src/qemu/nvkvm_isolate_handlers.c:1546-1609`. After UVM (type 0) has returned
above, an ioctl must be type `'d'` (DRM), the single NVKMS wrapper cmd, or type
`'F'` (NVIDIA RM frontend). Anything else is `-EPERM`. The comment explains why
this gate has to exist rather than relying on the per-family lists:

> Without this, a guest crafting a cmd with a non-`'F'` type would skip ALL the
> frontend allowlists below (they all guard on `type=='F'`) and fall straight
> through to the raw `ioctl()` in the stub — the kmd dispatches on `_IOC_NR`, so
> that could reach a denied privileged escape.
>
> — `src/qemu/nvkvm_isolate_handlers.c:1546-1554`

## 4. DRM render-node NR allowlist

`src/qemu/nvkvm_drm_allowlist.h`, checked at
`src/qemu/nvkvm_isolate_handlers.c:1572`. A `switch` rather than a table.
Allowed: generic `VERSION` (0x00) and `GEM_CLOSE` (0x09); nvidia-private
`GET_DEV_INFO`, `FENCE_SUPPORTED`, `PRIME_FENCE_CONTEXT_CREATE`,
`GEM_PRIME_FENCE_ATTACH`, `GET_CLIENT_CAPABILITY`, `GEM_EXPORT_NVKMS_MEMORY`,
`GEM_ALLOC_NVKMS_MEMORY`, `DMABUF_SUPPORTED`, and the four
`SEMSURF_FENCE_*` render-path synchronisation ioctls.

Deliberately excluded, with the reasoning recorded inline
(`src/qemu/nvkvm_drm_allowlist.h`, the "Audit G-3" block):
`GEM_IMPORT_USERSPACE_MEMORY` (0x02), `GEM_MAP_OFFSET` (0x0a),
`GEM_EXPORT_DMABUF_MEMORY` (0x0d), `GEM_IDENTIFY_OBJECT` (0x0e) —

> They carry raw guest VAs / mint mappings with no guest-VA marshalling — a
> guest VA forwarded to the host render node is pinned in the stub's address
> space (stub-heap info disclosure).

Display, modeset and permission surfaces are excluded as a class.

## 5. NVKMS inner-command allowlist

`src/qemu/nvkvm_nvkms_allowlist.h`, checked at
`src/qemu/nvkvm_isolate_handlers.c:1581-1600`. `/dev/nvidia-modeset` exposes
exactly one outer ioctl, `_IOWR('m', 0, NvKmsIoctlParams)` = `0xC0106D00`, whose
16-byte wrapper is `{u32 cmdType; u32 size; u64 address}`. Gating the outer
ioctl gates nothing, so the check reads the inner `cmdType` at param offset 0.

Seven commands are allowed: `ALLOC_DEVICE` (0), `FREE_DEVICE` (1),
`REGISTER_SURFACE` (17), `UNREGISTER_SURFACE` (18), two query commands
(61, 62) captured from a live Vulkan/EGL session, and `cmdType` **60**.

> the wrapper ... can carry ANY NVKMS command to a host-GLOBAL, privileged
> display device — including the cross-client permission/sharing verbs
> (GRANT/ACQUIRE/REVOKE_PERMISSIONS, GRANT/ACQUIRE/RELEASE_SURFACE), swap-groups
> and framelock. The branch in `nvkvm_isolate_handlers.c` only gated the OUTER
> ioctl, never the inner `cmdType`.
>
> — `src/qemu/nvkvm_nvkms_allowlist.h` (header comment)

`cmdType` 60 was added 2026-08-17 (`src/qemu/nvkvm_nvkms_allowlist.h:24-46`).
Denying it broke **all** offscreen GL in the guest on driver branches 595 and
610: every colour attachment came back `GL_FRAMEBUFFER_UNSUPPORTED` (0x8CDD)
with `glGetError()` clean, while the same probe passed on bare metal on the same
box, GPU and driver. The allowed set had been captured live from a 575-era
session, and branches 595+ issue a `cmdType` that capture never saw — notably
61 and 62, the entries the list was built around, are not issued by the 610 ICD
at all. Full evidence in [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md).
The general lesson: an allowlist captured on one driver branch expires on the
next, and its failures do not necessarily surface as denials.

The header labels itself interim: the intended fix is to stop forwarding NVKMS
at all in favour of the guest-side virtual head. `cmdType` 60, 61 and 62 are all
unnamed — the `NvKmsIoctlCommand` enum that would name them ships in no header,
so they are documented as measured rather than named, and should be pinned
against `nvkms-api.h` if that enum ever becomes available.

## 6. Frontend NR allowlist

`src/qemu/nvkvm_fe_alloc_allowlist.h:25-55`, checked at
`src/qemu/nvkvm_isolate_handlers.c:1659-1668`. 23 entries — gVisor nvproxy's
575-ABI frontend set plus what it added through v570.

```
0x27 0x29 0x2a 0x2b 0x34 0x35 0x41 0x4a 0x4e 0x4f 0x54 0x57 0x58 0x5e
0xc8 0xc9 0xce 0xcf 0xd2 0xd4 0xd6 0xd7 0xda
```

`0x70` (`NV_ESC_EXPORT_TO_DMABUF_FD`) was *removed*, and the removal is
instructive: it had been allowlisted but was never handled, so

> the stub would create a real dma-buf fd that nothing closes and nothing passes
> back, leaking a stub fd per call (self-isolate fd-exhaustion) and returning a
> meaningless fd to the guest.
>
> — `src/qemu/nvkvm_fe_alloc_allowlist.h:40-45`

An allowlist entry with no handler is a liability, not a no-op.

## 7. RM_ALLOC class allowlist

`src/qemu/nvkvm_fe_alloc_allowlist.h:59-149`, checked at
`src/qemu/nvkvm_isolate_handlers.c:1735-1746`. 89 classes, read from `hClass` at
param offset 12 (the shared NVOS21/NVOS64 prefix).

Provenance is nvproxy's 575-ABI `RM_ALLOC` class set, which was already a
superset of the empirically observed CUDA classes. Two nvkvm additions are
annotated: `NV01_EVENT` (0x05) for graphics/compute completion events, and
`AMPERE_B` (0xc797) for GA10x 3D/graphics.

Two of nvproxy's omissions are kept: privileged memory (0x3f) and
`OS_DESCRIPTOR` (0x71). The third is **not**: `NV01_EVENT` (0x5) is allowed here
(`src/qemu/nvkvm_fe_alloc_allowlist.h:63`), added for graphics/compute
completion events. The header comment used to claim all three omissions were
kept; it was corrected to name the exception
(`src/qemu/nvkvm_fe_alloc_allowlist.h:10-16`). Where the two disagree, the table
is authoritative.

(0x71 does appear on the wire — `NV_ESC_RM_ALLOC_MEMORY` allocates
`NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` and the guest handles it specially, see
[the forwarding model](../internal/forwarding-model.md#os_descriptor-pinning) —
but not via `NV_ESC_RM_ALLOC`, which is where this gate sits.)

## 8. RM control-command allowlist

`src/qemu/nvkvm_ctrl_allowlist.h:28-240`, checked at
`src/qemu/nvkvm_isolate_handlers.c:1758-1771`. **166 entries**, plus two
rule-based passthroughs implemented in code rather than the table
(`src/qemu/nvkvm_isolate_handlers.c:822-827`):

- `cmd & 0x8000` — the GSP legacy mask
- `(cmd >> 16) == 0x2081` — the `NV2081_BINAPI` class

Both are GSP-routed and carry no application pointers. The same check bounds the
inner params at 1 MiB (`src/qemu/nvkvm_isolate_handlers.c:1762`).

The table is generated, not hand-written, and its provenance is recorded at the
top (`src/qemu/nvkvm_ctrl_allowlist.h:4-17`): gVisor nvproxy's 575-ABI
`compUtil`-tagged control commands, with graphics/video/profiling/fabric-only
rows excluded, unioned with the set observed empirically from `cuInit`, matmul,
`vector_add`, `big_memcpy`, the ioctl-forwarding test and `nvidia-smi`. Later
blocks add the graphics (Vulkan/EGL) surface captured from a live `vulkaninfo`
RM ioctl stream (`:170-175`) and the dma-buf import counterpart (`:178-189`).

The most useful entry to read is the last one, added during 535 bring-up
(`src/qemu/nvkvm_ctrl_allowlist.h:218-239`). `libcuda` on 535.309.01 issues
`NVC36F_CTRL_GET_CLASS_ENGINEID` (0xc36f0101) during `cuCtxCreate`; `libcuda` on
575.51.03 does not, so it was absent from a table generated against the 575 ABI.
The symptom was:

```
nvkvm: DENY ctrl cmd 0xc36f0101 (not in allowlist / oversize)
```

and `cuCtxCreate` failing `CUDA_ERROR_OPERATING_SYSTEM` (304). The commit
message for adding it justifies the addition against OGKM 535.309.01 rather than
assuming: it is a read-only query, its params are 16 bytes of four `NvU32`s with
no embedded pointers, and the identical command under its older class id
(0x906f0101) plus two siblings on the same GPFIFO interface were already allowed.

That is the expected failure mode when moving to an unexercised driver branch:
not a crash, a `DENY` line and a CUDA error code.

## 9. Per-VM RM client allowlist

Not a static table — a set built at runtime. `nvidia` `hClient` ids are a global,
access-gated namespace, not fd-scoped, so an ioctl can name a client created on
another fd or process if share rights allow
(`src/qemu/virtio_nvgpu.h:286-300`).

Every `hClient` this VM's isolates successfully use is recorded
(`src/qemu/nvkvm_isolate_handlers.c:1959-1990`), including the kernel-assigned
handle written back to `hObjNew` on a root-client alloc. Two gates then consult
it:

- **`DUP_OBJECT` source** (`src/qemu/nvkvm_isolate_handlers.c:1782-1796`): the
  `h_client_src` at NVOS55 offset 12 must belong to this VM.
- **Every `'F'` ioctl carrying an `hClient` at param offset 0**
  (`src/qemu/nvkvm_isolate_handlers.c:1811-1847`): `ALLOC`, `ALLOC_MEMORY`,
  `CONTROL`, `FREE`, `DUP_OBJECT`, `SHARE`, `MAP_MEMORY`, `UNMAP_MEMORY`,
  `MAP_MEMORY_DMA`, `UNMAP_MEMORY_DMA`, `VID_HEAP_CONTROL`. The only exemption
  is the client-creating alloc itself.

The set is grow-only, which is safe: a freed handle still belonged to this VM,
and the kernel rejects a stale handle anyway.

## What is *not* enforced here

Intra-VM access control — which guest process may touch which object — is
deliberately not checked in QEMU. The reasoning is at
`src/qemu/nvkvm_isolate_handlers.c:1240-1252`:

> intra-VM, per-guest-process access control ... is emulated entirely by the
> guest kernel module — it owns the guest's pids/uids/namespaces/fds and is the
> authority. QEMU must NOT second-guess it with a session-ownership check: doing
> so would wrongly reject a handle that the guest LEGITIMATELY shared into
> another isolate via a guest-commanded `COPY_HANDLE_TO_ISOLATE` (e.g. CUDA
> IPC), and it adds no security (malicious guest userspace is blocked by the
> guest module; a malicious guest kernel would just forge `session_id`). QEMU's
> boundary is cross-VM / host-process ... not intra-VM.
