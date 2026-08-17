# Cross-isolate GPU buffer sharing (#110)

Two guest processes sharing one GPU buffer — a Wayland client hands its rendered
buffer to a compositor. Each guest process gets its own isolate (host process)
with its own RM client, which is what makes this hard: an RM object minted in
stub A is meaningless in stub B.

**Status: the path works, verified by bytes.** This page records the design, the
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

---

## Still open: `eglExportDMABUFImageMESA` → `EGL_BAD_MATCH`

This, not the import path, is what stops a GL client from presenting.

```
== 5. eglExportDMABUFImageQueryMESA  <-- the primitive under test ==
    fourcc=0x34324241 ('AB24') planes=1 modifier=0x300000000e08014
== 6. eglExportDMABUFImageMESA (actually get the fd) ==
RESULT: FAIL eglExportDMABUFImageMESA EGL_BAD_MATCH
```

Query succeeds, export fails. The client therefore never produces a buffer to
attach, which is exactly the observed "binds `zwp_linux_dmabuf_v1`, gets
feedback, never emits `buffer_params.create`" symptom.

**Two standing hypotheses in this tree are now falsified by measurement.** Both
`nvkvm_fe_alloc_allowlist.h` and `nvkvm_drm_allowlist.h` attribute the missing
dma-buf export to their own default-deny (the removed `NV_ESC_EXPORT_TO_DMABUF_FD`
and `GEM_EXPORT_DMABUF_MEMORY` entries). Running the probe with tracing on:

- guest `dmesg` contains **no** `nvkvm: AUDIT unknown ioctl` line — libEGL never
  issues an ioctl the guest module rejects;
- the QEMU log contains **zero** `DENY` lines of any kind.

An `rmdump` trace of the failing call shows the ioctls it *does* issue all
succeeding — `RM nr=0xd4 ... ret=0`, then `RM_CONTROL cmd=0x00003d05 ...
status=0x0 ret=0`. So `EGL_BAD_MATCH` is decided **inside NVIDIA's userspace
EGL**, before or independently of any kernel call that nvkvm gates. Re-adding
either allowlist entry would not, on this evidence, change the result.

(Separately, and worth fixing regardless: `src/abi/nvgpu.h` defines
`NV_ESC_EXPORT_TO_DMABUF_FD` as `0x70`. Upstream it is `NV_IOCTL_BASE + 17` =
`0xd9`, and the matching struct in that header is a 40-byte single-handle form
where upstream uses a 128-handle batch. Neither is on a live path today, but the
constant is wrong if anyone re-enables it.)

What was **not** determined: why libEGL declines. Distinguishing "the EGLImage
was created from a texture rather than a native buffer" from "the driver refuses
export on this device type" needs a host A/B of the same probe under the same
EGL client-side conditions.

---

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
