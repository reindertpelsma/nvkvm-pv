# Known limitations

This page is the honest list. Some entries are open bugs, some are intrinsic to
the design, and a few are things that used to be claimed and no longer survive
re-measurement. Each says which it is.

Compute is not on this list. The CUDA/PyTorch/Vulkan-compute path runs at host
parity on three GPU architectures; see `tests/perf/realapp_matrix.md`.

---

## Display and graphics

### There is no scanout path — intrinsic

`nvkvm` presents a virtual KMS head to the guest (`src/guest/nvkvm_kms.c`), and
a compositor can drive it: one connector of type `DRM_MODE_CONNECTOR_VIRTUAL`
(`src/guest/nvkvm_kms.c:282-284`), one CRTC, one primary plane, a fixed
1920x1080@60 mode (`src/guest/nvkvm_kms.c:35-37`). But the head does no scanout.
The file says so at the top:

> Headless first: the virtual CRTC accepts atomic commits / page-flips and
> completes their flip events, but performs no real scanout (there is no
> physical connector).
>
> — `src/guest/nvkvm_kms.c:11-13`

Vblank is a software `hrtimer` (`src/guest/nvkvm_kms.c:46-55`), so flips
complete at the advertised refresh rate rather than at a real one.

Frames therefore leave the guest by **capture**, not by scanout. A flip on the
virtual head sends `NVKVM_REQ_PRESENT` carrying the opaque stub GEM handle plus
geometry and modifier (`src/guest/nvkvm_kms.c:120-125`); QEMU asks the owning
isolate to `PRIME_HANDLE_TO_FD` the buffer and imports the resulting host dma-buf
as an EGLImage (`src/qemu/nvkvm_present_egl.c:71-126`).

**A DRM-backend compositor hangs, and that is not a bug in `nvkvm`.** It hangs
inside NVIDIA's closed `libnvidia-egl-gbm` scanout-present path, which is
coupled to `nvidia-modeset` — a host-global, privileged display device that
`nvkvm` deliberately does not drive.

> WHY: a DRM-backend compositor on NVIDIA hangs in libnvidia-egl-gbm's
> scanout-present path (coupled to nvidia-modeset, which we don't forward). A
> HEADLESS GL compositor renders the desktop offscreen with no KMS scanout and
> works.
>
> — `tests/perf/apps/wcapflip.c:4-8`

The corresponding wall from the other direction — rendering *into* a forwarded
scanout buffer — is recorded in full at `tests/perf/apps/gbmgl_present.c:5-18`:

> **STATUS / BLOCKER: rendering into a *forwarded* scanout bo from the guest is
> unresolved. Two approaches fail on the guest NVIDIA EGL:**
> - gbm_surface + eglSwapBuffers + gbm_surface_lock_front_buffer → swap succeeds
>   but lock_front_buffer returns NULL (NVIDIA gbm doesn't populate the
>   front-buffer queue the way Mesa does here).
> - gbm_bo_create + eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT | NATIVE_PIXMAP) →
>   EGL_BAD_PARAMETER: the guest's dma-buf for a forwarded bo is a hollow proxy
>   (the real bo lives in the stub), and NVIDIA EGL rejects importing it.
>
> NVIDIA only binds a scanout bo as a render target through its internal
> gbm_surface machinery (what a real compositor drives) — getting that to yield
> a flippable bo on our virtual head is the open compositor-integration problem
> (same root as "weston composites but never flips").

What *does* work, and is measured: a headless GL compositor.
`weston --backend=headless-backend --renderer=gl` comes up on the GPU —
weston's own log reports `EGL vendor: NVIDIA`, `GL version: OpenGL ES 3.2 NVIDIA
575.51.03`, `GL renderer: NVIDIA GeForce RTX 3060/PCIe/SSE2` — and
`weston-screenshooter` captured a populated 1920x1080 desktop
(`tests/perf/realapp_matrix.md`, "Display / desktop — re-validated").

If you want a virtual monitor, this is not that. Treat the head as a capture
surface.

### GL clients under Wayland render on the GPU but present nothing — open

**Re-measured 2026-08-17 on 575.51.03: the llvmpipe half of this entry no longer
reproduces.** With the full host bundle staged (`stage_guest_libs.sh`) and the
NVIDIA GBM backend in place (`stage_gbm_backend.sh` — whose default bundle path
was broken until this date, silently leaving the backend as Mesa), a Wayland GL
client reports `GL_RENDERER: NVIDIA GeForce RTX 3060/PCIe/SSE2`. The client gets
a real NVIDIA context.

What it still does not do is put pixels on screen.
`verify_client_window_differential.sh` with `glmark2-wayland`:

```
    GL_RENDERER:    NVIDIA GeForce RTX 3060/PCIe/SSE2
  A(no client) vs B(client): 0/518400 sampled px differ (0.00%)  -> IDENTICAL
  B(client) vs C(client+1s): 0/518400 sampled px differ (0.00%)  -> IDENTICAL
```

The renderer string alone would have read as a pass here; the differential is
what shows it is not one. The client never presents a buffer because it cannot
export one — `eglExportDMABUFImageMESA` returns `EGL_BAD_MATCH` while
`eglExportDMABUFImageQueryMESA` succeeds, and that failure involves **no denied
and no unknown ioctl** (measured: zero `DENY` in the QEMU log, no
`nvkvm: AUDIT unknown ioctl` in guest `dmesg`). See
[`cross-isolate-sharing.md`](cross-isolate-sharing.md) for the traces and for
the two allowlist-blame hypotheses this falsifies.

Cross-isolate *import* — a compositor importing a client-allocated GPU bo — is
separately confirmed working byte-exact; it is not what blocks this.

The original observation is kept below for history:

The compositor renders on the GPU. Its clients currently do not.

> **GL *clients* rendering through nvkvm: DID NOT REPRODUCE.** Wayland GL
> clients land on Mesa software rendering: `glmark2-wayland` reports
> `GL_RENDERER: llvmpipe (LLVM 20.1.2)`, and es2gears_wayland maps a window that
> composites entirely BLACK. Forcing the NVIDIA EGL vendor
> (`__EGL_VENDOR_LIBRARY_FILENAMES=.../10_nvidia.json`) fails at
> `eglGetDisplay()` with `0x3003` (EGL_BAD_ALLOC); `eglinfo` shows the Wayland
> platform bound to "Mesa Project" while the device platform binds NVIDIA.
>
> — `tests/perf/realapp_matrix.md`

One contributing factor has since been identified and fixed on the tooling side:
`libnvidia-egl-wayland` is not driver-versioned, so every staging loop that
globbed `$lib.so.$DRIVER_VERSION` skipped it silently. `make_host_bundle.sh` now
collects it by resolving the `.so.1` SONAME link instead
(`scripts/make_host_bundle.sh:37-47, 62-71`). That closes the *staging* gap; it
does not close this one. The last recorded observation with the external
platform present is in the same paragraph of the matrix:

> staging the NVIDIA EGL Wayland external platform turned the black window into
> no window at all, while the desktop kept looking correct either way. Always
> pair a capture with the client's renderer string.

Under investigation. Do not assume a screenshot proves anything here — check the
client's `GL_RENDERER`.

### Offscreen GL was broken on driver branches 595 and 610 — fixed 2026-08-17

Kept here because the diagnosis is the useful part, and because the shape of
this bug will recur.

For a while, creating an offscreen framebuffer inside a guest failed on
595.84 and 610.43.02. Every colour attachment format returned
`GL_FRAMEBUFFER_UNSUPPORTED` (`0x8CDD`) from `glCheckFramebufferStatus`, with
`glGetError()` clean at every step. Context creation and `GL_RENDERER` were
healthy (`GL_VENDOR='NVIDIA Corporation'`,
`GL_VERSION='OpenGL ES 3.2 NVIDIA 610.43.02'`), so any check that stopped at
the renderer string reported these drivers as fully working. It tracked the
driver version and not the ABI profile row — 595.84 and 580.95.05 take the
same profile row on the same box and disagree — which made "NVIDIA regressed
it in 595" the natural conclusion.

**It was ours.** The test that settled it took half an hour: install 610.43.02
on a host and run `tests/validate.sh` on bare metal, no VM. It passes —
`gl_draw_pixel_check PASS`, and a five-format probe reports
`0/5 configurations incomplete`. 595.84 on bare metal passes too. Booting a
guest on that *same box, same GPU, same driver* gives `5/5 incomplete`.

The cause is `src/qemu/nvkvm_nvkms_allowlist.h`. Its allowed set was captured
live from a 575-era Vulkan/EGL session, and branches 595+ issue an NVKMS inner
`cmdType=60` that the capture never saw. Default-deny returned `-EACCES` /
`NV_ERR_NOT_SUPPORTED`; the ICD's response was to unregister the surface and
declare every format unrenderable. Allowing `60` takes the 610.43.02 guest from
27/28 to **28/28**, and fixes 595.84 identically. Full trace, including the
`0/1440 → 17/152 → 60/32 → 18/16` NVKMS sequence and why cmdType 60 cannot be
named from any shipped header, is in
[`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md).

Three things to carry forward:

- **An allowlist captured on one driver branch expires on the next.** This one
  was three branches stale. `61`/`62`, the entries it was built around, are not
  even issued by the 610 ICD any more.
- **A denied ioctl does not necessarily surface as a denial.** This one surfaced
  four layers up as a GL enum, with no GL error set and nothing in guest
  `dmesg`. The only evidence was one line in the QEMU log.
- **"Works on bare metal?" is the cheapest question in this project.** It costs
  one box and separates *our* bugs from the driver's, and `tests/validate.sh`
  runs unmodified on bare hardware (`guest_module` FAILs, which is the correct
  answer there, and `abi_profile` SKIPs).

### PRIME-imported buffers are backed by guest pages, not host memory

`nvkvm_gem_get_sg_table()` (`src/guest/nvkvm_drm.c:132-155`) lazily allocates
plain guest pages when NVIDIA's EGL maps a proxy GEM's dma-buf attachment. Its
own comment labels this unfinished:

> ITERATION 1: plain pages to unblock the map and observe what NVIDIA EGL does
> next; stub page-sharing (so the host GPU reaches the same memory) is wired
> onto the registration ioctl the next strace reveals.
>
> — `src/guest/nvkvm_drm.c:126-131`

The subsequent `GEM_EXPORT_NVKMS_MEMORY` → `IMPORT_OBJECT_FROM_FD` handshake
(`src/guest/nvkvm_drm.c:558-642`) is what actually reconnects the import to the
real host memory object. The `sg_table` exists to let the attachment succeed.

### The virtual head is deliberately minimal

> Scope is deliberately minimal: one connector (fixed 1080p), one CRTC, one
> primary plane, atomic helpers. No multi-head / HDCP / overlays.
>
> — `src/guest/nvkvm_kms.c:17-18`

Compressed block-linear modifiers (`k=8`) are accepted for completeness but
their compression state is not shared across the boundary; a compositor capture
target should use the uncompressed `k=6` variants
(`src/guest/nvkvm_kms.c:186-188`).

### The host present slot is depth-1, newest-wins

`nvkvm_present_submit()` closes any frame the display has not yet drained
(`src/qemu/nvkvm_present_egl.c:479-480`, `/* drop the frame the display hasn't
taken yet */`). Under a fast guest and a slow host display, frames are dropped
rather than queued.

Zero-copy GL scanout to the host window is not auto-detected — the default is
CPU readback, because QEMU's console API does not expose the window's own GL
renderer and guessing wrong yields a blank window
(`src/qemu/nvkvm_present_egl.c:293-306`). Set `NVKVM_PRESENT_MODE=gl` only on a
host whose desktop is confirmed to be NVIDIA-rendered.

### NVKMS is still forwarded

The NVKMS allowlist calls itself interim:

> INTERIM: the real fix is to stop forwarding NVKMS entirely and emulate a
> virtual head in the guest; this allowlist shrinks the reachable kernel surface
> until then. cmdTypes 61/62 are the two query-class commands the ICD issues
> during enumeration; identify them precisely against nvkms-api.h before relying
> on this long-term.
>
> — `src/qemu/nvkvm_nvkms_allowlist.h` (header comment)

`/dev/nvidia-modeset` is host-global and privileged. If you do not need
Vulkan/EGL, run compute-only (`graphics=off` on the QEMU device, `make
NVKVM_GRAPHICS=0` for the guest module) and the device never opens.

The allowlist now carries a third unnamed entry, `cmdType=60`, added
2026-08-17 because branches 595+ need it for any offscreen render target (see
the fixed entry above). Like 61/62 it is required in practice and unaudited in
principle: the `NvKmsIoctlCommand` enum that would name it is not in any
shipped header — the DKMS tree ships only the wrapper struct in
`nvidia-modeset/nvkms-ioctl.h`. That makes the interim status *more* pressing,
not less: the allowlist is now three commands wide on evidence that amounts to
"a real ICD issued it", and it has already been shown to break silently and
non-locally when a driver branch moves.

---

## Video

### NVENC does not work in the guest on 575.51.03 — open

Do not claim hardware video encode works.

The host encodes fine (94 fps). In the guest, `ffmpeg -c:v h264_nvenc` never
completes, even on a trivial 5 s 720p solid-colour source, and wedges the
harness's ssh channel with it. `gdb` on the live guest puts the main thread in a
`clock_gettime` spin at:

```
cuMemcpy2D_v2  <- libnvcuvid.so.1  <- h264_nvenc
```

This is not the older `InitializeEncoder` "generic error (20)" signature — that
was a fast failure with a known cause (a missing alloc-params size-table entry
for `NV01_CONTEXT_DMA`) and was fixed. This is a hang further in, at frame
upload. Generic pitched 2D copies are fine — a `cudaMemcpy2D` probe completes
byte-exact on both sides — so it is specific to the encoder's surface path.

Whether this is a 575-vs-580 driver regression or a forwarder gap is
**undetermined**: the running VM's QEMU stdout was not captured, so there is no
`DENY`/allowlist evidence either way.

Historical documents in this tree claim 720p NVENC parity (0.96x) on driver
580.159.04. That measurement is real but was taken on a different driver and has
not been reproduced. Reproduce the current state with
`tests/perf/run_graphics.sh`; the NVENC row now fails one-sided rather than
vanishing from the table.

NVDEC (`h264_cuvid`) has no baseline at all: it decodes 0 frames and hangs on
the bare-metal host too, on the ffmpeg build used
(`tests/perf/realapp_matrix.md`).

---

## Numbers you should not quote

- **"795 fps glmark2"** — appears in older notes with no commit citation, in a
  document that also carries a confirmed transcription error, and the
  re-validation pass explicitly marks it "unconfirmed on this driver"
  (`tests/perf/realapp_matrix.md`). Do not use it.
- The **2026-06-01 matrix** was taken on driver 580.159.04 with different
  toolkits on each side (host cuBLAS/cuFFT 11.5 vs guest 12.x). The
  2026-08-17 re-validation on 575.51.03 used one statically-linked binary per
  workload on both sides, sha256-verified identical in the guest. Prefer the
  re-validation numbers.
- **SGEMM (cuBLAS), FFT (cuFFT), gpu-burn and the llama.cpp LLM rows** were not
  run in the re-validation pass. Their 580.159.04 ratios stand only for that
  driver.

---

## Security

### This is not a hardened multi-tenant boundary

`nvkvm` has had two internal security audits — their findings are visible all
over the source as `audit`-tagged comments, and the fixes are real (default-deny
allowlists on five surfaces, a seccomp-confined rootless isolate, integer-
overflow hardening in dispatch, GPA/KVM-slot reclamation). It has had no
external audit.

Specific things a reader should weigh:

- **UVM ioctls execute in QEMU's own privileged process**, not in an isolate.
  This is forced by the driver: UVM binds its file's `nvfp` to the calling task's
  `mm` during `UVM_INITIALIZE`, and the matching `mmap` must come from the same
  `mm` — which is QEMU, because QEMU is what installs the KVM memory region
  (`src/qemu/nvkvm_isolate_handlers.c:1229-1237`). The schema allowlist
  (`src/qemu/nvkvm_isolate_handlers.c:599-645`) is the mitigation.
- **Intra-VM access control is the guest kernel's job, by design.** QEMU does
  not check which guest process may touch which object; it checks cross-VM and
  host-process boundaries only
  (`src/qemu/nvkvm_isolate_handlers.c:1240-1252`). A malicious guest *kernel* is
  outside the model that check would defend.
- **The guest does not defend against the host.** `src/guest/nvkvm_mmap.c:18-20`:
  "A malicious host could abuse this, but we are not defending against the
  hypervisor."
- **The isolate sandbox has four rungs, and in a container you will not be on
  the strongest one.** `namespace` mode (namespaces + `pivot_root` + seccomp) is
  the strongest and **cannot run under Docker's default profile** — `docker run`
  with no security flags blocks `CLONE_NEWUSER` via its default seccomp profile
  and AppArmor policy, regardless of what `kernel.unprivileged_userns_clone` and
  `user.max_user_namespaces` say. The default mode `auto` therefore lands
  containerized deployments on `uid+chroot`, which is **materially weaker**: no
  pid/net/ipc/uts namespace, `/dev/nvidia-uvm` reachable by the stub (bypassing
  QEMU's UVM allowlist), and `/dev/shm` shared with the host. `auto` logs this
  at warning level at every start and the effective mode is readable back via
  the `isolate-mode-active` QOM property. Full per-rung comparison:
  [The isolate model → Isolation modes](isolate-model.md#isolation-modes).
- **`NVKVM_ISOLATE_MODE=none`** removes every boundary including the stub's
  seccomp filter. It requires an explicit
  `NVKVM_ISOLATE_UNSAFE_ACK=i-understand-this-removes-all-isolation` and is a
  debugging hatch, never a deployment mode. `auto` will never select it.
  **`NVKVM_ISOLATE_NO_HARDEN=1`** is the legacy hatch and maps to the `seccomp`
  rung — namespaces off, stub seccomp filter still applied, which is what it has
  always done. `NVKVM_STUB_DEBUG=1` additionally keeps the stub's stdio and its
  inherited environment (`src/qemu/nvkvm_isolate.c`).

Do not put untrusted tenants behind this.

---

## Functional gaps worth knowing

### `NV_ESC_RM_IDLE_CHANNELS` degrades to single-channel

The multi-channel form carries three guest-userspace pointers to handle arrays,
which the single-aux-slot path cannot marshal. The guest zeroes them *and*
`num_channels` (`src/guest/nvkvm_ioctl.c:462-479`), and the stub re-zeroes the
same 28 bytes at the boundary (`src/stub/nvkvm_stub.c:1280-1283`).

> multi-channel idle degrades to a best-effort single-channel drain, which is
> fine for the pre-teardown use libcuda makes of this call.
>
> — `src/guest/nvkvm_ioctl.c:471-473`

### Some ioctls are faked successful

`NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO` and `NV_ESC_RM_UNMAP_MEMORY` both look up
a CPU mapping by the caller's VA in the host driver's per-process table. In the
forwarded model the stub's mapping was registered at the stub's VA, and `libcuda`
passes its own guest VA, so the lookup always returns
`NV_ERR_OBJECT_NOT_FOUND`. The guest saves the caller's values and fakes success
on the response path (`src/guest/nvkvm_main.c:1305-1339`). The mapping still
works — it is installed through the GPA window, not through that kernel record.

### `NV0000_CTRL_CMD_GPU_GET_ID_INFO` drops the name

`szName` is an output pointer the host driver cannot write to. Rather than
forward a raw guest VA, the guest zeroes it; the driver null-checks it
(`src/guest/nvkvm_main.c:1628-1639`). `cuInit` does not need it and `nvidia-smi`
gets the model elsewhere.

### `GET_DRM_FILE_UNIQUE_ID` is answered locally, differently

The host implementation returns `(u64)filep->driver_priv` — a host kernel
pointer. Forwarding it would leak a host heap address across the VM boundary.
The guest synthesises an opaque per-fd id instead
(`src/guest/nvkvm_drm.c:433-455`).

### Bounded resources that degrade rather than fail

- **KVM memory slots**: 448 (slots 64–511), free-list plus watermark
  (`src/qemu/nvkvm_mmap_host.c:390-435`). Exhaustion is logged loudly. This was
  a monotonic counter until 2026-05-28 and the guest wedged on the next mmap.
- **Sparse GPA window**: 128 GiB with a 16384-entry free-list
  (`src/qemu/virtio_nvgpu.h:103`, `:178`). A full free-list logs once and leaks
  one extent — "bounded degradation, never a crash"
  (`src/qemu/nvkvm_mmap_host.c:296-300`).
- **Isolate mmap table**: 8192 entries; on overflow the mapping is torn down and
  the request fails rather than leaking
  (`src/qemu/nvkvm_isolate_handlers.c:2434-2470`).
- **Guest RAM ≥ 1 TB is refused.** The GPA windows sit at fixed addresses (1 TB
  shm, 1.5 TB mmap, 2 TB sparse); a VM with ≥1 TB RAM would overlap and silently
  corrupt, so realize fails loudly instead
  (`src/qemu/virtio_nvgpu.c:1239-1254`).

### Dead code you may trip over while reading

`src/qemu/nvkvm_dispatch.c` and `src/qemu/nvkvm_frontend.c` implement the
original synchronous path where QEMU ran the ioctl itself. The live path is
`IOCTL_ON_ISOLATE` → the stub. The `NV_ESC_RM_IDLE_CHANNELS` case in dispatch
says so explicitly:

> NOTE (audit P2-1): this dispatch path is NOT wired into the live
> `IOCTL_ON_ISOLATE` flow (`handle_ioctl` is static/unused). The authoritative
> `IDLE_CHANNELS` pointer-sanitisation lives in the stub (`nvkvm_stub.c`, nr
> 0x41 block). Kept here for the (currently dead) synchronous path; do not rely
> on it.
>
> — `src/qemu/nvkvm_dispatch.c:375-379`

The legacy `NVKVM_REQ_OPEN` / `_CLOSE` / `_IOCTL` / `_MMAP` / `_MUNMAP`
handlers in `src/qemu/virtio_nvgpu.c` are under `#if 0` as tombstones
(`src/qemu/virtio_nvgpu.c:230-236`).

One comment in the graphics layer was stale in the other direction, and has been
corrected: the `Audit G-3` block in `src/qemu/nvkvm_drm_allowlist.h` justified
its four exclusions by asserting the guest DRM proxy "wires only GET_DEV_INFO /
DMABUF_SUPPORTED / SEMSURF_FENCE_* + GEM_CLOSE". `nvkvm_drm_ioctls[]`
(`src/guest/nvkvm_drm.c:688-720`) also wires `0x09`, `0x0b`, `0x0e` and `0x18`.
The conclusion survives — `0x02`/`0x0a`/`0x0d` are genuinely absent from the
guest table, and `0x0e` is wired but answered entirely guest-side
(`src/guest/nvkvm_drm.c:391-405`) and never forwarded — but the stated reason
did not, and a reason that no longer holds is worse than no reason.

The `src/guest/nvkvm_drm.c:23-25` header note is *not* stale: the three ioctls
it defers (`IMPORT_USERSPACE_MEMORY`, `MAP_OFFSET`, `EXPORT_DMABUF`) are still
absent from the guest table.

### Pinned host memory

Registration runs at roughly **250-350 MB/s** in the guest, against ~12-17 GB/s on
the host, because every chunk is copied through the shared-memory slot ring.
Measured on RTX 3060 / 575.51.03:

| size | guest | host |
|---|---|---|
| 16 MiB | 82 ms | 1.3 ms |
| 1 GiB | 3.5 s | 68.5 ms |
| 2 GiB | 5.7 s | 123.3 ms |

A single `cudaHostRegister` above **2 GiB** returns `-E2BIG`. That ceiling is
derived rather than arbitrary: each 2 MiB chunk consumes one entry in QEMU's fixed
8192-entry mmap-token table, shared across every isolate in the VM, so a maximal
registration takes 1024 tokens and eight of them fit concurrently.

The throughput gap is in the slot-batched upload loop, not in the per-chunk
migration path — the chunk size was chosen so per-chunk overhead is negligible
against the data copied. It has been characterised but not optimised.

