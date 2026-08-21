# Cross-isolate GPU buffer sharing (#110)

Two guest processes sharing one GPU buffer — a Wayland client hands its rendered
buffer to a compositor. Each guest process gets its own isolate (host process)
with its own RM client, which is what makes this hard: an RM object minted in
stub A is meaningless in stub B.

**Status: the path works, verified by bytes**, and as of 2026-08-19 it carries a
real Wayland GL client's frames to a compositor — see "RESOLVED" below for the
one missing render-node ioctl that had been stopping the *export* half. This page records the design, the
evidence, and the parts that are still open. Every claim below is either
MEASURED on the box named at the bottom, or explicitly marked as not run.

---

## The design

The host side is the ordinary cross-process case — the stubs are normal host
processes and QEMU holds a unix socket to each, so a host dma-buf fd can be
relayed between them with `SCM_RIGHTS`. The interesting half is guest-side
bookkeeping, and it turns out **no guest-visible token is needed at all**.

```
guest process A                 guest process B (compositor)
  gbm_bo_create                   recv dma-buf fd over the Wayland socket
  gbm_bo_get_fd  ── dma-buf ──►   eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
        │                                       │
        │ guest DRM core exports the            │ guest DRM core PRIME-imports;
        │ proxy GEM as a guest dma_buf          │ dedups to the SAME proxy GEM
        ▼                                       ▼
                     one struct nvkvm_gem_object
             { owner ctx -> isolate A, stub GEM handle }
                                 │
                     NVIDIA EGL issues GEM_EXPORT_NVKMS_MEMORY (0x09)
                                 │  caller isolate != owner isolate
                                 ▼
                    nvkvm_gem_resolve_fwd  ->  NVKVM_REQ_XISO_IMPORT
                                 │
                                 ▼  QEMU (the broker)
        stub A: PRIME_HANDLE_TO_FD ──► host dma-buf ──► stub B: PRIME_FD_TO_HANDLE
                                 │
                     stub-B-local GEM handle returned to the guest,
                     cached on the proxy (xiso_importer_iso / xiso_gem)
```

### Releasing the brokered handle

The broker mints a real GEM in the importer's stub. Nothing used to close it:
there is no `XISO_RELEASE` opcode, and QEMU keeps no record of brokered GEMs the
way it records relayed RM handles (`xrm_handles`). So the importer's stub held a
PRIME reference to every client buffer it had ever imported, for the
compositor's whole life, and the owner's `GEM_CLOSE` could not reclaim that
memory. The single-entry cache made it worse: a second importer overwrote the
entry without closing it, orphaning the handle for good.

The release lives with the proxy, not with the isolate. `xiso_ctx` holds a
reference on the importer's fd context -- the same reason the owner ctx is
referenced -- so the importer's stub fd stays addressable, and
`nvkvm_gem_free()` forwards `GEM_CLOSE` there before dropping the ref. Evicting
a cached entry closes it first. Caching the bare `(isolate_id, handle_id)`
instead of the ctx would be fail-open: both ids are table-allocated and
reusable, so a close issued after the importer died could land on an unrelated
isolate's GEM.

The cost of that reference is a lifetime inversion worth knowing about: a proxy
that outlives its importer pins the importer's ctx, hence its session, hence its
isolate process, until the owner frees the buffer. That is the same trade the
owner ctx reference already makes. The alternative -- a QEMU-side per-isolate
record of brokered GEMs, reclaimed on isolate death, mirroring `xrm_handles` --
has not been built: it is only a backstop against a guest that never frees,
which is a guest DoS on its own isolate's memory.

### Why there is no cookie

The obvious design is a guest-minted unguessable cookie naming {isolate, RM
object}, carried by a synthetic dma_buf. That is not necessary here, and not
having it is strictly better.

`gbm_bo_get_fd` on a proxy GEM goes through the guest's **own DRM core**
(`drm_gem_prime_handle_to_fd`) — it is not forwarded, and the resulting dma_buf
is a guest dma_buf wrapping our `struct nvkvm_gem_object`. When process B calls
`PRIME_FD_TO_HANDLE` on that fd, the DRM core recognises its own dma_buf and
returns **the same GEM object**, with a fresh handle in B's file. So the
{owner isolate, owner stub handle} binding lives in guest kernel memory on an
object both processes legitimately reference, reached only by passing a real fd
through a real kernel fd-passing mechanism.

A cookie would be a *weaker* construction: it would be a number, and numbers can
be guessed, replayed or forged. An fd cannot be forged — possession is the
proof, and the kernel is what enforces it. The unguessability requirement is
satisfied by not having a guessable thing in the first place.

Cross-VM is structural rather than checked: `nv->handles`, `nv->sessions` and
`nv->isolates` all hang off the per-guest `VirtIONvgpu`, so one guest's request
cannot name another guest's isolate — there is no namespace in which to express
it.

### What the broker validates

`nvkvm_req_xiso_import` (`src/qemu/nvkvm_isolate_handlers.c`) treats every field
as an assertion:

- graphics must be enabled, else `EPERM`;
- both `owner_handle_id` and `importer_handle_id` must be **render-node**
  handles, so the guest cannot steer QEMU into PRIME-exporting an arbitrary fd;
- each named isolate must belong to the session that owns the handle presented
  with it (`session_has_isolate`, added on this branch). The guest asserts two
  independent (isolate, handle) pairings and the boundary now checks both
  against QEMU's own bookkeeping, rather than relying on the target stub's
  `handle_lookup` to fail with `-EBADF`. This deliberately does *not* require
  owner and importer to share a session — differing sessions is the feature.

The guest never supplies a host fd number, and never chooses which isolate it
shares with: the importer isolate is always the caller's own, taken from the
caller's ctx, and the owner isolate comes from the proxy GEM the guest kernel
holds. Both are filled in by the guest kernel, and both are re-derived and
checked by QEMU.

Who *may* import remains the guest kernel's call, per the standing access-model
split (intra-VM rights are the guest's; cross-VM and host-process boundaries are
QEMU's). What gates it is possession of the dma-buf fd.

---

## Evidence

Measured 2026-08-17 on a vast RTX 3060, driver **575.51.03**, guest Ubuntu 24.04
(kernel 6.8.0-137), QEMU built from `scripts/build_qemu.sh`.

### Rung 1 — no regression

```
===DMABUF_IMPORT===
bo: stride=1024 modifier=0x0 dmabuf_fd=23
<<< eglCreateImageKHR END: img=0x5838209058d1 egl_err=0x3000
RESULT import=OK
```

`egl_dmabuf_export_probe` runs and reproduces the documented failure exactly
(see "Still open" below).

### Rung 2 — cross-isolate share, verified by bytes

`tests/perf/apps/xiso_import_probe.c` (import succeeds):

```
importer: EGL 1.5 vendor=NVIDIA
<<< eglCreateImageKHR END img=0x56e921eacc81 err=0x3000
RESULT import=OK
```

That alone is **not** sufficient evidence, because the proxy GEM's
`get_sg_table` can hand out plain zeroed guest pages — an import can succeed
while the importer looks at unrelated memory. `tests/perf/apps/xiso_bytes_probe.c`
was written for this branch to settle it. Process A paints four distinct
non-zero quadrant colours through GL; process B imports the fd, binds it to an
FBO and reads it back:

```
exporter: GL_RENDERER=NVIDIA GeForce RTX 3060/PCIe/SSE2
exporter: bo stride=1024 mod=0x300000000e08014 dmabuf=23
exporter: self-readback q0=0xff604020 (expect r=20 g=40 b=60)
importer: GL_RENDERER=NVIDIA GeForce RTX 3060/PCIe/SSE2
importer: EGLImage=0x5c27c6ab4e71
importer: checked=65536 mismatch=0 nonzero=65536
RESULT xiso_bytes=PASS checked=65536 quadrants=4
```

All 65536 pixels match exactly and none are zero. QEMU's log shows the broker
firing once, between two different isolates:

```
nvkvm xiso: owner(iso=1 gem=0x1) -> importer(iso=2) gem=0x1
```

Re-measured after rebuilding QEMU with the new `session_has_isolate` check: same
PASS, broker still fires, zero `session mismatch` rejections — the added
validation does not reject the legitimate path.

Note the modifier: `0x300000000e08014` is block-linear, not LINEAR. Sharing does
not depend on a linear layout.

### Rung 3 — weston output capture into a GPU dma-buf: NOT nvkvm's failure

```
scanout bo: 1920x1080 stride=7680 modifier=0x0
capture failed: GL: unsupported buffer
RESULT presented=0/60 mode=dmabuf  0.0 fps
```

Same weston, same nvkvm, `--shm` instead:

```
RESULT presented=30/30 mode=shm  23.4 fps (1283 ms)
```

Only the buffer type differs. `GL: unsupported buffer` is weston's own string,
emitted from `gl_renderer_do_capture` (confirmed by `strings` on
`libweston-13/gl-renderer.so`), i.e. weston's GL renderer declining the buffer
type for a capture target. Weston itself is on the GPU throughout:

```
EGL vendor: NVIDIA
dmabuf support: modifiers
GL renderer: NVIDIA GeForce RTX 3060/PCIe/SSE2
```

**Not run:** the bare-metal host control for this specific comparison. The host
image ships weston 9, which has neither `--renderer=gl` nor the
`weston_output_capture_v1` protocol, so the same script cannot execute there.
The weston-side attribution above rests on the shm/dmabuf differential plus the
symbol, not on a host A/B.

### Rung 4 — GL client under headless weston: renderer yes, pixels no

```
    GL_RENDERER:    NVIDIA GeForce RTX 3060/PCIe/SSE2
  A(no client) vs B(client): 0/518400 sampled px differ (0.00%)  -> IDENTICAL
  B(client) vs C(client+1s): 0/518400 sampled px differ (0.00%)  -> IDENTICAL
```

Half of rung 4 is met and half is not, and the differential is what separates
them. The client gets a **real NVIDIA context** — which supersedes the
`known-limitations.md` claim that Wayland GL clients land on
`llvmpipe (LLVM 20.1.2)` — but contributes **zero pixels** to the composited
output. Reported as a failure precisely because the renderer string alone would
have looked like a pass.

**Superseded 2026-08-19 — both halves now pass.** The zero-pixel half had two
independent guest-side causes, neither of them an allowlist problem; see
"RESOLVED" below and `known-limitations.md`. A client now emits
`zwp_linux_buffer_params.create` and cycles three dma-buf `wl_buffer`s, and
`weston-screenshooter` captures a populated client window (glmark2's Horse
scene, from a client reporting `GL_RENDERER: NVIDIA GeForce RTX 3050`). What is
still open is *sustained* presentation: the stream stalls after the first
burst.

---

## RESOLVED 2026-08-19: `eglExportDMABUFImageMESA` → `EGL_BAD_MATCH`

The section this replaces concluded that `EGL_BAD_MATCH` was "decided **inside**
NVIDIA's userspace EGL, before or independently of any kernel call that nvkvm
gates", on the evidence that no ioctl was denied and none was unknown. Both
observations were correct. The conclusion drawn from them was not.

It named the missing experiment itself — "needs a host A/B of the same probe
under the same EGL client-side conditions" — and that experiment settles it.
Same box (RTX 3050 Laptop, 580.173.02), same
`tests/perf/apps/egl_dmabuf_export_probe.c`:

| | host | guest |
|---|---|---|
| `eglExportDMABUFImageQueryMESA` | ok | ok |
| `eglExportDMABUFImageMESA` | **PASS**, fd=24 | **FAIL** `EGL_BAD_MATCH` |

A host PASS means it is not the driver refusing on principle; it is the guest
being different. `strace` on both narrows the difference to one syscall:

```
host:  ioctl(9 /*renderD128*/, _IOWR('d', 0x41, 32)) = 0
       ioctl(9, DRM_IOCTL_PRIME_HANDLE_TO_FD) = 0      -> the dma-buf fd
       ioctl(9, DRM_IOCTL_GEM_CLOSE) = 0
guest: ioctl(9 /*renderD128*/, _IOWR('d', 0x41, 32)) = -1 EINVAL
```

`nr 0x41` = `DRM_COMMAND_BASE + 0x01` = `GEM_IMPORT_NVKMS_MEMORY`: wrap an RM
memory object in a GEM object so PRIME can export it. `nvkvm_drm_ioctls[]` had
no `[0x01]` entry, so `drm_ioctl()` in the **DRM core** returned `-EINVAL` for a
driver ioctl outside the table — before nvkvm's dispatch, before QEMU, before
any allowlist. That is precisely why there was no `AUDIT unknown ioctl` and no
`DENY`: those absences meant the call never reached us, not that it was never
made.

The struct layout was recovered from an `LD_PRELOAD` ioctl interposer on the
host rather than guessed (32 bytes: `mem_size` @0, `nvkms_params_ptr` @8,
`nvkms_params_size` @16, OUT `handle` @24; the pointee's first field is
`{ int memFd }` — the same shape 0x09 carries). So it is the exact mirror of
`GEM_EXPORT_NVKMS_MEMORY` (0x09), and is implemented as one: same aux
marshalling, plus the OUT-handle proxying that 0x0b already does.

Guest, after: `RESULT: PASS dma-buf export works (fd=25)`.

The keystone this unblocks is visible in QEMU's own log — every client
swapchain buffer goes straight from the new ioctl into the cross-isolate broker
that Rung 2 proved byte-exact:

```
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x3) -> importer(iso=1) gem=0x2
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x4) -> importer(iso=1) gem=0x3
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x5) -> importer(iso=1) gem=0x4
```

Rung 4 (a GL client under headless weston) is now met on both halves: real
NVIDIA renderer **and** real pixels. See `known-limitations.md` for the full
before/after, for the second root cause found alongside it (`GET_DEV_INFO`
handing the guest the host's `primary_index`, which is what had been putting
every Wayland client on llvmpipe), and for the two things that are still broken
past this point — frame updates stalling after the first burst, and an
intermittent guest-fatal `kvm run failed Bad address` in the mmap/WINMAP window
path.

(The stale-constant note stands and is still worth fixing: `src/abi/nvgpu.h`
defines `NV_ESC_EXPORT_TO_DMABUF_FD` as `0x70` where upstream is
`NV_IOCTL_BASE + 17` = `0xd9`, with a 40-byte single-handle struct where
upstream uses a 128-handle batch. It is not on a live path — the live export
path is the DRM one above — but the constant is wrong if anyone re-enables it.)

## Also fixed on this branch

- `nvkvm_isolate.c`: the reader-exit path woke pending IOCTLs, the sync slot and
  the present slot, but never the xiso slot. A stub dying mid-broker left
  `nvkvm_isolate_xiso_import` blocked on `xiso_cond` forever while holding
  `xiso_lock`; because the broker runs inline on the virtio TX thread, that
  wedges the whole guest's GPU I/O rather than one isolate. Now woken with
  `-ECONNRESET`, matching the two slots that already had this guard.
- `tests/perf/stage_gbm_backend.sh` defaulted to a hardcoded `host-libs-580`
  matching no bundle the current host scripts produce, so it exited "no
  libnvidia-allocator" on every current box and left the GBM backend as
  Mesa/llvmpipe. Now auto-detects, as `stage_guest_libs.sh` does.

## The same shape again: RM export/import (CUDA VMM shareable handles)

**Resolved 2026-08-21.** The design above was written for dma-bufs and a
compositor. The identical problem turned up in a completely different consumer
— NCCL's shared-memory transport — and the same answer worked, which is the
useful thing to record here.

`cuMemExportToShareableHandle(CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR)` parks
an RM object on a freshly-opened `/dev/nvidiactl` fd via RM control **0x3d05**
(fd at inner offset 16). The exporter hands that fd to a peer **process** over a
Unix socket with `SCM_RIGHTS`; the peer then calls **0x3d08**
(`GET_EXPORT_OBJECT_INFO`) and **0x3d06** (`IMPORT_OBJECT_FROM_FD`), both with
the fd at inner offset 0. That is NCCL v2.27.3 `transport/shm.cc:590`, and it is
what every world>=2 NCCL job does during connection setup.

The order is MEASURED, not assumed — `tools/nv_ioctl_trace.c` on the bare-metal
host:

```
exporter  TRACE OPEN  fd=54 path=/dev/nvidiactl
exporter  TRACE CTRL  cmd=0x00003d05 params[24]=01000000 0200005c 0200005c b000005c 36000000 ...
                                                                          ^^^^^^^^ fd=0x36=54 @16
importer  TRACE CTRL  cmd=0x00003d08 params[80]
importer  TRACE CTRL  cmd=0x00003d06 params[20]=37000000 01000000 0200005c 0200005c b000005c
                                               ^^^^^^^^ fd=0x37=55 @0
```

Two guest processes are two isolates, so this is exactly the situation at the
top of this page: the guest kernel rewrites the embedded fd to a VM-global
`handle_id` (so no guest fd number and no guest VA crosses the boundary), but
the fd behind that handle was opened by the **exporter's** stub, and the
importer stub's `handle_lookup()` missed.

Three things were wrong, and the first two are what made this look like an
allowlist problem when it was not:

| | symptom |
|---|---|
| guest never translated the fd for 0x3d08 (only 0x3d05/0x3d06) | a raw guest fd *number* forwarded to a stub where it means something else |
| 0x3d08 not in `nvkvm_ctrl_allowlist.h` | `DENY` x6 per import; libcuda gave up before ever issuing 0x3d06 |
| the handle named a **foreign isolate's** fd | the real defect |

**Allowing 0x3d08 alone is a non-fix** — that was tried, recorded in
`tests/BOOT_MATRIX.md`, and correctly reverted. It is necessary but not
sufficient, and the fd relay is the other half. Both halves land together or
neither does.

### Why no new mechanism was needed

QEMU already holds a copy of every handle's fd (`struct nvkvm_handle.fd`), so
unlike the dma-buf case there is not even a stub round-trip to arrange: the
broker is `ISOLATE_CMD_RECEIVE_FD`, which already exists, relaying a dup into
the importing stub. The stub's existing `handle_lookup()` then resolves it on
the 0x3d05/0x3d06 path that was already there. The whole cross-isolate half is
`nvkvm_xrm_materialise()` in `src/qemu/nvkvm_isolate_handlers.c`.

Confinement is the same argument as for xiso and is structural rather than
checked: `nv->handles` hangs off the per-guest `VirtIONvgpu`, so there is no
namespace in which one guest could name another guest's handle. QEMU re-derives
the owning isolate from the handle itself rather than trusting anything the
guest said about it. Within the VM, entitlement stays the guest kernel's call
and is enforced by fd possession — the importer can only name this handle
because it was handed a real fd through a real kernel fd-passing mechanism.

Relays are recorded per isolate (`xrm_handles`) because the stub's
`handle_store()` overwrites without closing: pushing a second dup of the same
handle would leak the first inside the stub.

### Evidence

`tests/repro/cumem_export_import.c` is the oracle — the smallest thing that does
export / `SCM_RIGHTS` / import across two *separate* processes. It `dlopen`s
libcuda and declares the entry points itself, so the same binary runs on the
host and in the guest with no CUDA toolkit installed.

Measured on 6x RTX A4000, driver 570.124.06:

| | host | guest before | guest after |
|---|---|---|---|
| `cuMemImportFromShareableHandle` (same GPU) | PASS | **FAIL** CUDA 101 | **PASS** |
| 2 MiB readback of the exporter's pattern | 0 mismatch | — | **0 mismatch** |
| cross-GPU (dev0 -> dev1) | import OK, `cuMemSetAccess` 101 | import FAIL | import OK, `cuMemSetAccess` 101 |

The cross-GPU row is host *parity*, not a bug: the A4000 has no P2P, so mapping
device-0 memory for device-1 access is invalid on this hardware on both sides.
Import succeeding and `cuMemSetAccess` then failing is what the host does too.

Note the readback: import returning success is not sufficient evidence, for the
same reason it was not in Rung 2 above — the importer has to be shown looking at
the *exporter's bytes*. It is.

## Reproducing

In the guest, after `stage_guest_libs.sh` and `stage_gbm_backend.sh`:

```bash
cc -O2 -o xiso_bytes_probe /mnt/nvkvm/tests/perf/apps/xiso_bytes_probe.c \
   -I/usr/include/libdrm $(pkg-config --cflags --libs gbm libdrm egl glesv2)
./xiso_bytes_probe /tmp/sock          # expect: RESULT xiso_bytes=PASS
```

The user running it must be in the `render` group (and `video` for card0
probes); the cloud image does not add `ubuntu` to either, and the failure mode
is an unchecked `open()` returning -1 followed by a segfault inside gbm.

Run QEMU with `NVKVM_DEBUG=1` to see the `nvkvm xiso:` broker line — without it
the broker is silent and a successful import is indistinguishable from one that
never needed brokering.
