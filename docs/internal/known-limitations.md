# Known limitations

This page is the honest list. Some entries are open bugs, some are intrinsic to
the design, and a few are things that used to be claimed and no longer survive
re-measurement. Each says which it is.

Compute is not on this list. The CUDA/PyTorch/Vulkan-compute path runs at host
parity on three GPU architectures; see `tests/perf/realapp_matrix.md`.

---

## Display and graphics

### The guest DRM node does not open under the hardened isolate — FIXED 2026-08-19

The isolate drops to an unprivileged per-VM uid with no supplementary groups,
and the host's DRM render node is mode 0660 owned by `root:render`. So the stub
cannot open it, and every DRM-dependent path in the guest fails at `open()`
before any ioctl is forwarded. Measured on an RTX 3050 laptop container,
2026-08-19, holding everything else constant:

| `NVKVM_ISOLATE_MODE` | guest `open("/dev/dri/card0")` |
|---|---|
| default (`auto` → `uid+chroot`) | fails |
| `seccomp` (no uid drop) | succeeds |

CUDA, Vulkan compute and offscreen EGL are unaffected — they reach the GPU
through `/dev/nvidia*`, which the isolate does have. What is blocked is
anything that needs the DRM node itself: compositors, `kmscube`, `modetest`,
and therefore the whole virtual-KMS/present path. `tests/validate.sh` passes
28/28 throughout, because nothing in it opens the DRM node.

Fixed by not having the stub open it by name. QEMU is already the privileged
component and already parks a `/dev` dirfd for the stub before the uid drop; the
render node is now opened the same way, pre-drop, and parked at `NVKVM_DRM_FD(k)`
(`src/common/nvkvm_isolate_proto.h`). The stub `dup`s it per handle, so one guest
process closing its DRM fd cannot take the node from the others, and falls back
to opening by name when nothing was parked. Granting the isolate uid the
`render` group instead would have worked but is strictly broader — it would
reach every render node on the host, not the one this VM is entitled to.

The only sandbox widening is `fcntl` restricted to `F_DUPFD_CLOEXEC` by a BPF
argument check; duplicating an fd the process already holds grants no new reach.

With both this and the render-minor fix, on the default `auto` (→ `uid+chroot`)
mode: the guest opens `card0` and `modetest` enumerates the head — `Virtual-1`
connected, 1920x1080, 23 modes. `tests/validate.sh` stays 28/28 and the
correctness reproducers stay clean on both the ring and virtqueue paths.

### The render node is not always minor 128 — FIXED 2026-08-19

DRM hands out render minors in probe order, so on any host where another GPU
probes first — an iGPU on a hybrid-graphics laptop, the common case — the
NVIDIA node is `renderD129` or higher. The stub resolves
`NVKVM_DEV_DRM_RD(k)` to the fixed name `renderD(128+k)`, so on those hosts the
guest's DRM node failed to open with `ENOENT` while CUDA and headless EGL kept
working, and nothing noticed.

Now resolved from sysfs (`src/qemu/nvkvm_drm_node.h`): the k-th *NVIDIA* node,
whatever minor it landed on, is bind-mounted onto `renderD(128+k)` in namespace
mode, or aliased by a symlink in `/dev` when there is no mount namespace to
rewrite. Verified by A/B on the same VM in the same isolate mode — with the
alias the guest opens `card0` and `modetest` enumerates the head (Virtual-1
connected, 1920x1080, 23 modes); with it removed the open returns `ENOENT`.

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

Re-measured 2026-08-19 on an RTX 3050 / 580.173.02, *after* the two Wayland
client fixes below — unchanged, and now with a stack. `weston
--backend=drm-backend.so --renderer=gl` gets **further** than the old note
suggests: it enumerates the virtual head, lists all 23 modes, reports
`Output 'Virtual-1' enabled with head(s) Virtual-1`, and launches
`weston-desktop-shell`. Then the main thread wedges forever:

```
Thread 1 (weston):
#0  poll(nfds=1, timeout=-1)
#1  libnvidia-eglcore.so.580.173.02
#2  libEGL_nvidia.so.0
#3  libEGL_nvidia.so.0
#4  libEGL_nvidia.so.0
#5  libnvidia-egl-gbm.so.1
#6  libnvidia-egl-gbm.so.1
#7  drm-backend.so
#8  drm-backend.so
#9  drm-backend.so
```

An unbounded `poll` on one fd inside `eglcore`, entered from
`libnvidia-egl-gbm`, entered from the drm-backend's scanout path: it is waiting
for a completion that only `nvidia-modeset` produces. Clients never get
serviced. Nothing on the nvkvm side of the boundary is reached, so there is no
ioctl to allow, no index to correct, and no marshalling to add — the fix would
be to forward `nvidia-modeset`, which is a host-global privileged display device
and a deliberate non-goal. **Judgement: not worth attacking. The
headless-compositor + capture route is the viable one**, and it is the one that
now carries real client pixels.

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

### GL clients under Wayland presented nothing — TWO ROOT CAUSES FIXED 2026-08-19

**Both were guest-side gaps, and neither was an allowlist problem.** The entry
below used to say the failure was decided inside NVIDIA's closed userspace. That
was wrong, and the reason it looked right is worth keeping: both bugs produced a
failure with *no* denied ioctl and *no* unknown ioctl, so every "check the
allowlist" reflex came back clean and the conclusion "it must be NVIDIA's
userspace" followed. The decisive move in both cases was a **host/guest A/B of
the same binary**, which turns "the driver refuses" into "the guest is
different, here is the syscall where".

Measured on an RTX 3050 Laptop, driver 580.173.02, guest Ubuntu 24.04.

#### Cause 1 — `GET_DEV_INFO` handed the guest the HOST's DRM card number

`nvkvm_drm_fwd_get_dev_info` forwarded the whole
`drm_nvidia_get_dev_info_params` struct back verbatim, including
`primary_index` — the host's DRM primary minor. NVIDIA's userspace turns that
field straight into a path. On this host the NVIDIA GPU is `card2`, so:

```
ioctl(renderD128, 'd' nr 0x43 = GET_DEV_INFO) = 0      -> primary_index = 2
newfstatat("/dev/dri/card2")                 = -1 ENOENT
eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND)  = EGL_NO_DISPLAY
```

The guest has `card0`. So NVIDIA's EGL declined the Wayland platform, GLVND fell
through to Mesa, and **every Wayland GL client silently landed on llvmpipe** —
which is exactly the `GL_RENDERER: llvmpipe (LLVM 20.1.2)` that the old
`realapp_matrix.md` runs recorded, and exactly why forcing the NVIDIA vendor
gave "failed to initialize EGL display" instead of a working context. This is
the primary-node twin of the `renderD128`-vs-`renderD129` fix above: the guest
must be told about *its own* nodes. Now overwritten with `dev->primary->index`
(our KMS head is on the same drm_device), which also stops a host DRM minor
leaking into the guest.

After: a Wayland client with **default** vendor selection reports

```
GL_VENDOR:   NVIDIA Corporation
GL_RENDERER: NVIDIA GeForce RTX 3050 Laptop GPU/PCIe/SSE2
GL_VERSION:  4.6.0 NVIDIA 580.173.02
```

#### Cause 2 — the guest DRM driver had no `GEM_IMPORT_NVKMS_MEMORY`

With clients finally on NVIDIA, the documented `eglExportDMABUFImageMESA` ->
`EGL_BAD_MATCH` reproduced. `tests/perf/apps/egl_dmabuf_export_probe.c` run on
the host and in the guest, same box, same driver:

| | host | guest (before) |
|---|---|---|
| `eglExportDMABUFImageQueryMESA` | ok, `AB24`, mod `0x300000000e08014` | same |
| `eglExportDMABUFImageMESA` | **PASS**, fd | **FAIL** `EGL_BAD_MATCH` |

`strace` localises it to exactly one call:

```
host:  ioctl(renderD128, _IOWR('d', 0x41, 32)) = 0
       -> DRM_IOCTL_PRIME_HANDLE_TO_FD -> dma-buf fd -> DRM_IOCTL_GEM_CLOSE
guest: ioctl(renderD128, _IOWR('d', 0x41, 32)) = -1 EINVAL
```

`nr 0x41` is `DRM_COMMAND_BASE + 0x01`, `GEM_IMPORT_NVKMS_MEMORY` — the ioctl
that wraps an RM memory object in a GEM object so it can be PRIME-exported. It
is the whole of `eglExportDMABUFImageMESA`'s kernel work. `nvkvm_drm_ioctls[]`
had no `[0x01]`, so the **DRM core** rejected it with `-EINVAL` before nvkvm's
dispatch ran — which is why guest dmesg had no `AUDIT unknown ioctl` and the
QEMU log had no `DENY`. Both absences were evidence the call never reached us.

Wired as the exact mirror of `GEM_EXPORT_NVKMS_MEMORY` (0x09), which was already
implemented for the import direction: same aux marshalling of the embedded
`{ int memFd }` blob, then the returned stub GEM handle proxied into the guest
DRM core sized to `mem_size`. Layout verified byte-for-byte against an
`LD_PRELOAD` ioctl trace on the host. Also added to the QEMU render-node
allowlist and to the stub's offset-8 pointer substitution / memFd resolution /
post-ioctl pointer scrub.

#### Evidence that the whole client path now runs

`egl_dmabuf_export_probe` in the guest: `RESULT: PASS dma-buf export works`.

QEMU's own log, one line per client swapchain buffer — each
`GEM_IMPORT_NVKMS_MEMORY` immediately followed by that buffer being brokered
from the client's isolate to the compositor's:

```
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x3) -> importer(iso=1) gem=0x2
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x4) -> importer(iso=1) gem=0x3
isolate=3 cmd=0xc0206441 ret=0
nvkvm xiso: owner(iso=3 gem=0x5) -> importer(iso=1) gem=0x4
```

`WAYLAND_DEBUG` on the client shows a healthy triple-buffered dma-buf present
loop — `zwp_linux_buffer_params.create`, then `attach`/`damage`/`commit` cycling
`wl_buffer@31/32/33` at ~25 ms — where before it emitted no `create` at all.

And pixels: `weston-screenshooter` on headless weston captures the composited
desktop with a **populated** client window — es2gears' gears, and glmark2's
Horse scene from a client reporting `GL_RENDERER: NVIDIA GeForce RTX 3050`.
Those windows were black.

`tests/validate.sh` stays **28/28**.

#### What is still broken — the next wall

Two things, both only reachable now that the NVIDIA client path actually runs.
Neither reproduced before, because before this the client was llvmpipe.

1. **Frames stop updating after the first few.** The client presents correctly
   for a while and then stops committing; captures taken afterwards are
   byte-identical. Measured: `commits before=62 after 2s=62`, and in another run
   the client was still committing at t=10 s — so it is non-deterministic, in
   the tens-to-hundreds-of-frames range, not a fixed frame count. The prime
   suspect is the un-marshalled sync fd: `SEMSURF_FENCE_CREATE` (0x15) returns
   `p->fd`, a **stub-local** sync fd, straight to the guest — the guest source
   already flags this ("cross-boundary sync-fd passback is a separate
   milestone", `src/guest/nvkvm_drm.c`). A number that means nothing in the
   guest is exactly the shape of a lost frame-completion wakeup. **Not
   confirmed** — no experiment in this round isolated it.
2. **Intermittent guest-fatal `kvm run failed Bad address`.** Within roughly
   10-60 s of a GL client running, QEMU dies with an unserviceable guest access
   at CPL=3 inside NVIDIA userspace: `mov ecx,[rdx+0x84]` where `rdx` is a
   page-aligned pointer loaded out of a driver struct — i.e. a mapped GPU page
   whose backing the host could not resolve. Reproduced four times with the same
   faulting instruction bytes at different ASLR bases; the last operations
   before it are always a burst of `SEMSURF_FENCE_CREATE` plus `RM_MAP_MEMORY` /
   `WINMAP` window mappings. This looks like the mmap/WINMAP window path, not
   the dma-buf path, and it is **not** diagnosed.

#### Both resolved 2026-08-20 (RTX 4070 / 595.84 / host kernel 7.0)

**(1) confirmed and fixed.** The suspect above was right. `GET_DEV_INFO`
forwarded the host's `supports_sync_fd = 1`, so libnvidia-egl-wayland chose the
sync-fd presentation path and waited on a fence built from a stub-local
descriptor number. The guest now reports `supports_sync_fd = 0` — we do not
implement passback, so we must not claim it — and NVIDIA's userspace picks a
path that does not need one. glmark2-wayland in the guest went from hanging on
scene 1 (6+ minutes, 0 CPU time, main thread in `pthread_cond_wait` inside
`libEGL_nvidia`) to completing all 20 scenes.

Note the symptom on this box was a hard hang on the *first* swap, not the
"stops after tens-to-hundreds of frames" seen earlier. Same unimplemented
mechanism; do not assume the older observation is fully explained by it.

**(2) did not reproduce.** No `kvm run failed Bad address` in any run on this
box: glmark2 continuous for 150 s+, a full 20-scene run, and a five-minute soak
of a live guest desktop (190,013 frames presented, 0 errors). That is **not**
proof it is fixed — the earlier reproductions were on different hardware (RTX
3050 laptop), and several unrelated fixes have landed since. Treat it as open
and unreproduced rather than closed.

#### (2) DIAGNOSED AND FIXED 2026-08-23 (RTX 3050 **laptop**, driver 580.173.02)

It was never a narrow-MAXPHYADDR bug, a hole in the sparse window, a memslot
collision, or an allocator race — all four were instrumented and cleared. Nor
was it specific to the display path in principle; it just needs the GPU to go
idle, which only a desktop session does.

An NVIDIA GPU mapping is `VM_IO|VM_PFNMAP`, and `nv_fault()`
(`kernel-open/nvidia/nv-mmap.c`) has two paths that return `VM_FAULT_NOPAGE`
**without installing a PTE** while the driver reinstates a revoked mapping:

```c
if (!down_read_trylock(&nv_system_pm_lock))   return VM_FAULT_NOPAGE;
if (!nvl->safe_to_mmap) { rm_schedule_gpu_wakeup(...); return VM_FAULT_NOPAGE; }
```

Both mean "retry, it is coming". Userspace honours that by re-faulting. KVM
calls `fixup_user_fault()` once, `follow_pte()` still finds nothing, and it
returns `-EFAULT`, which QEMU has always treated as fatal — so the guest dies
on a mapping that was about to be reinstated.

Fixed by `patches/0010-kvm-retry-a-bare-KVM_RUN-EFAULT.patch`: `RIP` has not
advanced, so re-entering the guest re-executes the same access. **The fault
clears after ~1465 ms.** That number is why it looked permanent: an earlier
attempt retried 64 times at 200 us, covered ~13 ms, and concluded it never
clears.

Why the 4070/T4 never saw it: `validate.sh` passes 28/28 either way because
compute never lets the GPU idle, and neither box has aggressive mobile RTD3
(`DynamicPowerManagement: 2`, `runtime_status=suspended` for 6.7 h on the
laptop). `nvidia-smi -pm 1` alone does **not** prevent it.

**Two more traps found on the way in, both of which looked like "graphics does
not work" and were neither:**

- The stub dup'd `NVKVM_DRM_FD(k)` unconditionally while QEMU only parked a
  node there in uid-drop mode. In other modes that descriptor number belonged
  to something else — measured `/dev/nvidiactl` — and the dup *succeeded*, so
  the open-by-name fallback never ran and every DRM ioctl went to the wrong
  device. `GET_DEV_INFO` returned EINVAL, EGL could not associate the CUDA
  device with a DRM node, and clients fell back to llvmpipe. A silently-wrong
  fd is worse than a missing one: fix is QEMU parking in every mode and passing
  `--drm-fds=N` so the stub fails closed instead of guessing.
- cloud-init's default user is in no groups beyond its own, so the guest user
  could not `open()` `/dev/dri/renderD128` (0660 `root:render`) at all. Plain
  EACCES, before any of the above was reachable.

Do not read this entry as "Wayland clients work" in general. Read it as: on the
measured box they get a real NVIDIA context, produce real GPU buffers, the
compositor composites them, and the frames now reach a host window and keep
coming.

### Display work needs nvidia-drm.modeset=1 on the host (2026-08-20)

Compute and offscreen GL need only `EGL_EXT_platform_device`.  The
display/compositor path additionally needs the **GBM** platform, and NVIDIA only
provides GBM when its DRM module is loaded with `modeset=1`.  Without it,
libgbm silently falls back to Mesa and every compositor lands on llvmpipe --
inside the guest that is indistinguishable from an nvkvm bug.

Measured on a vast.ai RTX 3060 (driver 580.159.04), same probe before and after:

```
                     modeset=N (default)     modeset=Y
gbm backend name     drm    <- Mesa          nvidia
EGL vendor           Mesa Project            NVIDIA
GL_RENDERER          llvmpipe                NVIDIA GeForce RTX 3060/PCIe/SSE2
```

The host showed this with nvkvm not involved at all, and the guest inherited it:
the guest's GBM only reaches NVIDIA once the **host** module has modeset on.

Fix on the host:

```
rmmod nvidia_drm && modprobe nvidia_drm modeset=1     # or nvidia-drm.modeset=1 on the kernel cmdline
cat /sys/module/nvidia_drm/parameters/modeset          # expect Y
```

Triage before any display debugging on a new host -- run a GBM probe on the
**host** first, because if the host cannot do NVIDIA GBM the guest never will:

```
gbm backend name=nvidia + EGL vendor=NVIDIA  -> usable
gbm backend name=drm    + EGL vendor=Mesa    -> check modeset before suspecting nvkvm
```

Note this is easy to misread as an environment limitation.  It is not: the box
is fully usable for display work once modeset is on.  `tests/validate.sh` is
28/28 there either way, because compute and offscreen GL go through
`EGL_EXT_platform_device` and never touch GBM.

### Wayland desktops render — FIXED 2026-08-20 (drm_crtc_vblank_on)

A full Wayland desktop now runs in the guest on the GPU and is displayed in a
QEMU window on the host, interactively, confirmed on a physical RTX 4070.

**The cause was vblank bookkeeping, not rendering.** We never called
`drm_crtc_vblank_on/off`, so the vblank sequence and timestamps the core reports
to userspace never advanced.  Flips still *completed* -- `nvkvm_pipe_update()`
falls back to `drm_crtc_send_vblank_event()` when `drm_crtc_vblank_get()` fails
-- which is exactly why this hid for so long: compositors kept flipping and the
screen stayed black.

weston's own scene graph named it:

```
Layer 0 (pos 0xffffffff):
  View 0 (desktop shell fade surface): (0,0) -> (1920,1080)
    [fully opaque] solid-colour buffer [R 0 G 0 B 0 A 1]
```

weston was compositing correctly the entire time -- it was animating its
desktop-shell fade against a clock that never moved, so an opaque black overlay
sat above the panel and every client.  Every "weston renders nothing" reading
was really "weston renders the fade overlay".

Two further fixes were needed to get compositors that far, both in the same
area:

- **`8e499a4`** — `drm_simple_display_pipe_init()` builds the plane's IN_FORMATS
  blob using the helper's own LINEAR-only `format_mod_supported`, so every
  block-linear modifier we listed was filtered out before our override existed.
  Compositors were offered exactly `XRGB8888 + LINEAR`, and NVIDIA cannot use a
  LINEAR dma-buf as an EGLImage render target (measured: `GL_INVALID_OPERATION`
  on bind, incomplete FBO).  wlroots said so outright ("EGLImage not supported"
  / "Failed to create FBO"); weston failed silently.  The blob is now rebuilt
  after our callback is installed.
- **`f4201ff`** — advertise only modifiers the driver really implements.  Once
  the blob reached clients, the 16 hand-rolled BL2D guesses became harmful: a
  client that picks an invented modifier gets a buffer the driver cannot render
  into.  Xorg/glamor died with "Failed to create pixmap" (a *different* glamor
  failure from the open `GL_OUT_OF_MEMORY` one below — this one is fixed).  Only
  `0x0300000000606014` (SCANOUT|RENDERING) is published; LINEAR is still
  *accepted* for cursors, just not offered.

**Measured on the physical RTX 4070 (595.84):** guest flip deltas 16.4–18.1 ms,
median 16.7 ms = **59.9 Hz**, with the desktop displayed in a QEMU GTK window.

**RESOLVED 2026-08-20: there was no ceiling.  The "~21 presents/s" was an
artifact of counting log lines, which only some paths emit.**  Re-measured with
per-frame counters compiled into the present path:

| stage | rate |
|---|---|
| guest KMS flips | 59.9 Hz |
| host submits | 60.0/s |
| host window swaps | 60.0/s |
| dropped | 0 |

Every one of 60 consecutive one-second samples was exactly 60 frames, and it
holds at 60.0/s with 8 concurrent EGL clients (`glmark2` 58.0/s windowed, 60.0/s
fullscreen).  The pipeline is display-refresh-bound, not overhead-bound.

**Re-validated 2026-08-20 on the security branch, on the same physical RTX 4070.**
The 15-fix boundary-audit commit (`53e3a96`) had only ever been compile-tested;
it changed the isolate liveness/timeout paths, the GPA-window teardown, the
mmap/munmap ownership checks, the S-4 present import-cache keying and the
default-deny gates, all of which sit under this measurement. Re-run against the
known-good desktop workload, with the frame counters now in-tree
(`NVKVM_PRESENT_TIMING=1`):

| load | rate | dropped |
|---|---|---|
| desktop + 4 concurrent `weston-simple-egl` | 60.0/s | 0 |
| the above + Firefox on a live Wikipedia article | 60.0/s | 0 |

263 consecutive one-second samples, 16,139 frames consumed, **zero dropped**;
per-present mean 2.3–4.7 ms. Every sample fell in 58.0–60.3 frames/s and the
modal value was exactly 60.0. Firefox was verified *rendering* — the article
laid out with images, and interactive on the physical display — not merely
launched. No regression from the security fixes is visible at the display layer.

Two things had to be true for that measurement to mean anything, and both cost
time to discover:

- **Console selection.** QEMU adds a default stdvga, which becomes console 0,
  while nvkvm's scanout console is registered second. A `-display gtk` window
  therefore opens on the *text* console and shows a **black screen** while the
  guest desktop runs perfectly on console 1 — and a bare `screendump` captures
  that console instead of the 1920x1080 desktop.

  **This note used to say the fix was `-vga none`. It is not, and that advice
  has been withdrawn.** Deleting the boot console costs more than it saves:
  without it GRUB has no video device and stalls in `gfxterm` init with nothing
  on any console to say so, and the guest gets no `/dev/fb0` for early kernel
  messages. Both problems are *selection* problems, so name things instead of
  removing them:
  - `screendump <file> nvkvm0` targets our console by device id (hence `id=` in
    `run_test_vm.sh`). Verified: bare `screendump` gives the 1280x800 boot
    console and `screendump <file> nvkvm0` the 1920x1080 desktop, same VM.
  - Inside the guest, select the DRM node by **driver**, not by index — with a
    VGA present the guest sees `card0 -> bochs-drm` and `card1 -> nvidia`, and
    a compositor taking `card0` silently renders on llvmpipe. This is the more
    dangerous half and is now documented user-facing in `docs/howto/run.md`.

  `tests/validate.sh` is 28/28 with the default VGA present.

  **Automatic handover now works** and this note's "no API to switch" is
  resolved: upstream has none, but we build QEMU from source, so the GTK front
  end is patched to move to a console the first time it presents a real (non
  placeholder) surface — see `scripts/build_qemu.sh` step 6d. It is a one-shot
  latch keyed on "still on the page the window opened on, and the target is
  above it", so the VGA going live cannot consume the shot, the user is never
  dragged back after switching manually, and the single-console `-vga none`
  case is a no-op. All five GTK entry points are hooked, not just the one the
  default present mode uses — readback goes through `gd_switch()` while
  `NVKVM_PRESENT_MODE=gl` goes through the egl/gl-area scanout paths and would
  otherwise silently never switch.

  `-display gtk,show-tabs=on` (or **View → Show Tabs**) still makes the manual
  switch discoverable, which is worth having while debugging the head.

  The hardware-accurate shape — nvkvm's own device carrying a boot framebuffer,
  so there is one console and nothing to select — remains the cleaner long-term
  design, and is now an option rather than a requirement.
- **A verified-animating client.** An idle compositor correctly stops flipping,
  so the rate reads ~0 with a perfectly healthy pipeline.


The leading suspect at the time — a per-present `nvkvm_isolate_present_export()`
IPC round-trip plus `PRIME_HANDLE_TO_FD`, uncached — was **measured at 0.07 ms**
and cannot cap 60 to 21.  The planned export cache was therefore *not* built: it
would have been a patch for a problem that did not exist.  The two earlier A/B
results below stand as real (the `glReadPixels` removal genuinely moved 10 → 21
by the old metric), but the "21" endpoint was never a true rate.

| change | host rate (old log-line metric) |
|---|---|
| per-frame `glReadPixels` removed (GL zero-copy, `2e9413f`) | 10/s → 21/s |
| present send made non-blocking in the guest (workqueue, `8b0592c`) | 21/s → 21/s (guest reached 60 Hz) |
| present send made fully fire-and-forget | 21/s → 22/s (noise; not landed) |

**Count frames with a counter, not with log lines.**  A separate "2.5 fps with
4.8-second freezes" reading from the same session was the kernel's printk
ratelimiter — 291 suppressed callbacks per 5 s window is itself ~60/s.  Both
looked like real performance bugs; neither was.

**Measurement traps that produced false findings here, all confirmed:**

- `import fd=` only logs on a cache *miss*, so a healthy cached pipeline shows
  "N presents, 0 imports" and looks dead.  This produced a false "gfx_update
  never fires" root cause on two separate boxes.
- A compositor that stops flipping when nothing changes is *correct*.  Any flip
  or present count taken without a verified-animating client is meaningless.
- `rmmod` fails silently while a compositor holds the DRM node (refcount seen at
  177) and the following `insmod` reports `File exists` while the OLD module
  stays resident.  Verify the loaded module by symbol
  (`grep -c <new_symbol> /proc/kallsyms`) before trusting any kernel-side
  measurement, and reboot the guest to swap modules reliably.
- QEMU's stderr is block-buffered when redirected to a file, so per-second
  bucket counts read zero.

### X11 renders correctly but the host only sees the first frame (2026-08-20)

Xorg with the modesetting driver renders a live desktop on our KMS node — an
in-guest X root capture shows xclock ticking and a working xterm.  But it logs

```
modeset(0): Using 24bpp hw front buffer with 32bpp shadow
```

i.e. it modesets once and then blits damage straight into the front buffer,
never issuing a plane update and never requesting vblank.  Our present path is
driven by `nvkvm_pipe_update()` (page flips), so the host receives the modeset
frame and then a frozen picture over a live desktop.

An attempt to fix this by tracking the scanned-out fb and re-exporting it from
the vblank tick regressed Xorg startup (`Failed to create pixmap`) and was
reverted.  Re-add the pieces one at a time rather than together.

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

### NVIDIA's own X driver (the DDX) cannot run in the guest — intrinsic

The DDX reaches the GPU perfectly well — with BAR geometry advertised it opens
`/dev/nvidiactl` and `/dev/nvidia0`, and every RM ioctl through nvkvm returns 0
— and then asks NVKMS to select a display subsystem. The NVKMS behind this
device is the **host's**: it owns the host's connectors, not nvkvm's virtual
head. nvkvm denies that command (`NVKMS_IOCTL_DECLARE_EVENT_INTEREST`,
`cmdType=33`), and allowing it one rung at a time simply walks the DDX down to
`QUERY_CONNECTOR_STATIC_DATA` over the *host's* six physical connectors.
Forwarding further would not give the guest a display; it would give the guest
the host's display.

Closing this needs a **virtual NVKMS** — one that answers `QUERY_DISP` /
`QUERY_CONNECTOR_STATIC_DATA` with nvkvm's own 1920x1080 head and terminates
`SET_MODE`/`FLIP` at our KMS pipe. That is a real piece of work, versioned per
driver branch, not a missing forward.

**Scope: narrow.** It costs you the things that require that specific driver —
`nvidia-settings`, and professional-application features that depend on it. It
does not cost you a desktop: a stock distro's own Xorg session runs on the nvkvm
head with `modesetting` + `AccelMethod "none"`, GL clients accelerated on NVIDIA
through render offload, and GL, Vulkan and CUDA are unaffected. The config that
selects that path is installed by `scripts/stage_guest_libs.sh` as part of guest
staging, so it is not a step anyone has to take.

Mechanism, host-vs-guest traces and the measurements:
`docs/internal/mint-guest-desktop.md`.

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

### NVENC — the 575.51.03 hang DID NOT REPRODUCE (2026-08-20)

**Re-tested on an RTX 3060, driver 575.51.03 — the same driver this entry was
written against — and hardware encode completes.** The documented repro (a
trivial 5 s 720p solid-colour source through `h264_nvenc`) exits 0 rather than
hanging; `hevc_nvenc` likewise; a real file encode produced 920 KB of H.264; and
via PyAV both encoders returned packets for 18 of 20 submitted frames.

That is **not** proof the original finding was wrong. It was observed on
different hardware, and a great deal has changed in this tree since. Treat this
as *does not reproduce here*, not as fixed — and if you can still reproduce the
hang, the detail below is what to compare against.

The original entry follows, unedited:

---

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

### Managed memory (`cuMemAllocManaged`) does not work — OPEN, fails loudly

**State on `main` (`2406a3c`).** Nothing maps `/dev/nvidia-uvm` on the host.
`uvm_mmap()` → `uvm_va_range_create_mmap()` is the *only* thing that puts a
managed range into a `uvm_va_space` (ogkm `kernel-open/nvidia-uvm/uvm.c:759-858`),
so no range exists — not for the U-6 ownership table to record, not for
`uvm_api_validate_va_range()` to find. `cuMemAllocManaged` returns
`CUDA_ERROR_INVALID_VALUE`. `cuCtxCreate` and the rest of the UVM surface work;
only managed memory is missing.

This is universal, not architecture-specific: reproduced on Blackwell GB202, Ada
AD104 and Ampere GA102. `validate.sh`'s `cuda_managed_alloc` and
`cuda_managed_coherence` exist to report it, and they are the first tests in this
project that could see it — the gap dates to `b46e9c0` (28 May 2026) and went
three months unnoticed because nothing called `cuMemAllocManaged`.

**Why it is not a one-line fix.** The mapping has to be at the *guest's* VA, and
QEMU has one address space:

- The managed pointer is the CPU VA of the `/dev/nvidia-uvm` VMA, and the GPU VA
  is that same number — measured, `cuPointerGetAttribute` returns the host
  pointer unchanged. So a VMM-chosen host address produces a mapping the guest's
  kernels cannot address, and no ioctl translation can help: the pointer reaches
  the GPU inside kernel launch parameters, not inside an ioctl.
- The guest's VA cannot be steered either. libcuda reserves the range with an
  anonymous `PROT_NONE` `mmap(NULL, …)` and then `MAP_FIXED`s the UVM fd inside
  it, so the address is settled by the guest kernel's ASLR before any device we
  own is touched.
- Mapping at the guest VA in QEMU (what `c8ea92d` did) makes managed memory work,
  but hands the guest an address-space layout oracle over the QEMU process —
  recorded as U-15 in
  [audit-guest-pointers.md](audit-guest-pointers.md).
- Letting the **isolate** own the mapping and having QEMU map the same UVM
  object elsewhere does not work either: a managed range holds exactly one
  `vma_wrapper`, so it has exactly one VMA, and mmap'ing the fd at a second
  address creates a second independent range rather than aliasing the first
  (`uvm_va_range.h:288`, `uvm.c:743-757`).

> **Correction, 2026-08-24.** The revert commit `2406a3c` states that "libcuda
> picks the same UVM VA in every process, so the collision is deterministic". That
> is **false**, and so is the `ARCHITECTURE.md` sentence it was taken from.
> Measured on RTX 5090 / 580.95.05-open: **12 concurrent processes produced 12
> distinct addresses.** libcuda reserves via an anonymous `PROT_NONE mmap(NULL,…)`
> — guest-kernel ASLR picks the address — then `MAP_FIXED`s the UVM fd inside it.
> The collision is a birthday collision at 2 MiB granularity over ~14 TiB: rare,
> not certain. `known-limitations.md`'s original "where their allocators happened
> to land" was right; the revert's repudiation of it was wrong.
>
> This also means the guest VA **cannot be steered** — it is settled before any
> device we own is touched.
>
> The revert itself stands: it closes U-15 (the layout oracle). Only its reasoning
> about collision frequency was wrong, and that changes the trade-off, not the
> conclusion. See `docs/internal/uvm-va-decoupling.md`.

What is **not** solved: two guest processes that genuinely want the same VA at
the same time. The second takes the fallback. Measured on 1x RTX 5090: two
concurrent CUDA processes both got working managed memory, with one unrelated
fallback logged — but that is a property of where their allocators happened to
land, not a guarantee. Fixing it properly needs the guest's UVM VAs steered into
a region QEMU has reserved, which libcuda's own address selection does not
currently allow.
>
> Full measurements and the option analysis: [UVM VA decoupling](uvm-va-decoupling.md).

### Managed memory is pinned, non-migrating, and cannot oversubscribe

The fallback backs a managed range with guest RAM the guest module allocates,
and publishes those same physical pages to the GPU as a UVM **external** range.
That makes `cudaMallocManaged` correct and coherent — one set of pages, seen by
the guest CPU, by QEMU and by the GPU — but it is **not** unified memory, and
the differences are user-visible:

- **No migration.** Pages never move to VRAM. Every GPU access crosses PCIe.
  For a GPU-heavy kernel over a large managed buffer this is the difference
  between VRAM bandwidth and host bandwidth, and it is the first thing to
  suspect if a `cudaMallocManaged` workload is inexplicably slow here. Real UVM
  would migrate on fault; `UVM_MIGRATE` fails on an external range
  (`NV_ERR_INVALID_ADDRESS`, `uvm_migrate.c:1159-1163`).
- **No oversubscription.** Real UVM lets a managed allocation exceed VRAM and
  pages it in and out. The fallback cannot: the allocation is guest RAM, bounded
  by guest RAM, and there is no eviction path. An application sized to rely on
  oversubscribing VRAM will fail to allocate rather than run slowly.
- **`cudaMemAdvise` returns `invalid argument`.** `SET_PREFERRED_LOCATION`,
  `SET_ACCESSED_BY` and read-duplication are all managed-only
  (`uvm_policy.c:429`). Measured: the failures are visible to the application
  and do not affect correctness — an app that ignores the return value still
  gets correct data. Visible beats silent, but code that checks these calls will
  see them fail.
- **`cudaMemPrefetchAsync`** returns success and does nothing.
- **The pages are pinned, and it gets worse with size.** The guest module holds
  them with `alloc_pages()` and the host pins the matching HVA range via the RM
  OS-descriptor (`pin_user_pages()`). A 1 GiB managed allocation therefore
  **permanently locks 1 GiB of guest RAM** for as long as it lives, with no
  oversubscription and no reclaim — where real UVM would keep most of it in VRAM
  and page the rest. Treat a large managed footprint as a hard reservation of
  guest RAM, sized on top of everything else the guest needs. It is also in
  tension with **ballooning** and with **live migration**: those pages can be
  neither reclaimed nor moved. nvkvm does not support live migration anyway, but
  the balloon interaction is real.
- **Chunked backing, and a 4 GiB ceiling.** Contiguity is required per
  *descriptor*, not per range: UVM finds an external range by any address inside
  it and keeps a per-GPU range tree of sub-mappings, so one range is covered by
  many allocations. The guest takes the backing largest-block-first with
  `alloc_pages()` (max `MAX_PAGE_ORDER`, 4 MB on x86_64) and the host makes one
  RM descriptor per chunk. Both sides cap the count at 1024, so **the largest
  managed allocation is 4 GiB** — and a 4 GiB one costs 1024 RM objects, which
  is a real resource on the host side, not just bookkeeping.
  Fragmentation degrades gracefully: smaller chunks still work, they just map
  with a smaller GPU page size (`uvm_map_external.c:931-940` derives the mapping
  page size from `pageSize | base | length | map_offset`), costing TLB coverage.
  The ordinary 1 GiB guard on `nvkvm_mmap_request()` still bounds *forwarded*
  device mmaps; it does not apply to fallback ranges, which are guest RAM the
  host only describes.

`cuda_micro` case 6 is named `uvm_migrate` and measures a GPU↔CPU cycle. It
still passes, but under the fallback there is no migration happening, so the
number it reports stops meaning what its name says.

### Reserving the UVM sub-window costs the sparse window 16 GiB

The per-mmap UVM memslots need GPAs the big sparse memslot does not cover, so
`nvkvm_sparse_init()` carves `min(NVKVM_MMAP_WIN_SIZE, win/8)` off the top —
16 GiB of 128 on a normal host. The advertised window and the VMM buffer are
unchanged; what shrinks is the bump allocator behind every bulk BAR/sysmem
mmap, from 128 GiB to 112. No workload measured here has come close to either
figure, and the window has a free-list, but it is a real reduction and it is
where to look first if `sparse window full` ever appears.


### A100 (Ampere GA100): `cuCtxCreate` fails with 999 — OPEN, first datacenter-Ampere test

Measured 2026-08-20 on an **A100 80GB PCIe**, host driver **580.126.09**, ABI
profile 580, guest kernel 6.8, on a nested-virt host (massed-compute).

Bring-up is clean and CUDA gets a long way in before dying:

```
PASS  nvidia_smi_gpu        NVIDIA A100 80GB PCIe
PASS  cuda_init             rc=0
PASS  cuda_device_count     1
PASS  cuda_device_name      NVIDIA A100 80GB PCIe
PASS  cuda_compute_cap      sm_80
FAIL  cuda_ctx_create       rc=999 (CUDA_ERROR_UNKNOWN)
```

Everything downstream of the context skips, so the run is **22 PASS / 1 FAIL /
5 SKIP**. The failure is not flag-dependent and not specific to one entry point:
`cuCtxCreate_v2` returns 999 for flags 0/1/2/4, and `cuDevicePrimaryCtxRetain` —
the path most runtimes actually take — returns 999 as well.

**What is NOT the cause**, checked: no `DENY` and no allowlist refusal anywhere
in the host log, and no nvkvm error in the guest ring (`RING MAPPED ... 3-way
OK`, `synthesized GET_PIDS` and nothing else). So the driver is being reached and
is failing the request, rather than nvkvm refusing to forward it.

**Graphics on the same box is fine**, which is the surprising part — Vulkan
compute, EGL and GL all pass *with pixel verification*, on a card with no display
engine at all:

```
PASS  vk_compute_dispatch   4096 elements, data[i]=i*3+7 verified on A100
PASS  gl_renderer           'NVIDIA A100 80GB PCIe/PCIe/SSE2'
PASS  gl_draw_pixel_check   inside==triangle, outside==clear
```

**Localised to an ioctl (2026-08-20), with `NVKVM_DEBUG=1`.** The context
creation drives a repeating pair, and the second half always fails:

```
ioctl_on_isolate: cmd=0xc0104629  ret=0  nvstatus=0x0    NV_ESC_RM_FREE          ok
ioctl_on_isolate: cmd=0xc020464f  ret=0  nvstatus=0x57   NV_ESC_RM_UNMAP_MEMORY  OBJECT_NOT_FOUND
...repeats, then: nvkvm_isolate: killed isolate 3
```

`nvstatus 0x57` is `NV_ERR_OBJECT_NOT_FOUND` — the *same* status as the Hopper
Vulkan failure, though at a different call site. The driver is reached and
answers; nvkvm is not refusing anything.

> **Note added 2026-08-21, corrected.** The shared status code is a
> coincidence, not a shared cause — that conclusion still holds, but the reason
> first given for it does not. ~~The Hopper Vulkan failure turned out to be a
> defect in the 570.124.06 driver branch.~~ It was **ours**: `HOPPER_USERMODE_A`
> (`0xc661`) had no alloc-param size entry, so nvkvm forwarded a NULL parameter
> block and RM built the usermode aperture from defaults. The 570-branch-driver
> account was reached by elimination on a box that could not exhibit the bug.
> Do not treat these two `0x57`s as one bug. See
> [correctness.md](../reference/correctness.md#vulkan-compute-on-hopper--root-caused-it-was-ours-and-it-was-never-a-driver-bug).

Note the ordering: a FREE immediately
followed by an UNMAP of what looks like the same object.

Also seen, and believed benign: `NV_ESC_SYS_PARAMS` (0xd6) returns `-EBUSY`
early on. That ioctl sets `memblock_size` once per driver instance, so a second
client getting EBUSY is expected rather than a fault — noted here only so the
next person does not chase it.

**ROOT CAUSE FOUND (2026-08-20): `p_linear_address` is zeroed and never
refilled.** Instrumenting the handles settled it. The UNMAP does *not* follow
the FREE — it precedes it, on the same object, and carries a NULL address:

```
A100DBG UNMAP hClient=0xc1d0003c hDevice=0x5c000002 hMemory=0x5c000157 va=0x0 nvstatus=0x57
A100DBG FREE  hRoot=0xc1d0003c   hParent=0x5c000002 hObject=0x5c000157        nvstatus=0x0
```

`va=0x0` is the fault. The guest zeroes the field on purpose
(`src/guest/nvkvm_ioctl.c`, `case NV_ESC_RM_UNMAP_MEMORY`) — correctly, since a
guest VA is meaningless in the isolate's address space — and the comment there
states the contract: *"host fills in from its map table"*.

**Nothing on the live host path fills it in.** The only code that ever writes
`p_linear_address` host-side is in `src/qemu/nvkvm_dispatch.c`, which is inside
the `#if 0` legacy block and is not reachable. The stub knows NR `0x4f` only
well enough to find its `status` field offset. So the driver receives every
`RM_UNMAP_MEMORY` with VA 0, cannot find a mapping there, and answers
`NV_ERR_OBJECT_NOT_FOUND`. The guest half of a two-part design shipped; the host
half exists only as dead code.

**Scope — how sure we are this is *the* cause.** Of **1885** forwarded ioctls in
one failing `cuCtxCreate`, this is the **only** non-zero `nvstatus` in the entire
trace; everything else returns 0. That makes it the sole visible fault rather
than one symptom among several. It is *not* proof: `cuCtxCreate` could still be
failing on something that never surfaces as an ioctl status. Treat it as the
strongest candidate, not a closed case.

**Why consumer cards do not trip on it** is not established either. The likely
reading is that the failed UNMAP is harmless there because the FREE immediately
after tears the object down anyway — but that is a guess, and the honest version
is that this bug has probably been present and silent on every card.

**The fix** is to implement the host side of the stated contract: record
`(hClient, hDevice, hMemory) -> host VA` when `NV_ESC_RM_MAP_MEMORY` (0x4e)
succeeds, and substitute it on `0x4f`. That is a real feature, not a patch, and
it needs hardware to verify.

This is the first GA100 ever tested here — every other Ampere row is a consumer
GA10x die. It is deliberately **not** added to the tested-platforms table, whose
stated bar is "reached a real CUDA kernel launch": this did not.


### Nested nvkvm (L2-inside-L1): `cuCtxCreate` fails with 999 most of the time — OPEN, first double-forward test

Measured 2026-08-22 on **6x Tesla T4** (Turing TU104, PCIe, datacenter — a card
with 28/28 on the single-hop tested-platforms table), host driver 580.178.04,
ABI profile 580. Topology: L0 bare metal -> L1 (an ordinary nvkvm guest, already
verified 28/28-equivalent on this box) -> L2, a **second** nvkvm guest booted
*inside* L1, using a copy of the same patched QEMU binary and the same
`virtio-nvgpu-pci-non-transitional` + `nvkvm-gpu` device pair L1 itself sees from
L0. No code changes were needed to reach this topology — `nvkvm_guest.ko` and the
QEMU-side device do not know or care that their "host" is itself a forwarded
guest.

**Everything below `cuCtxCreate` works, twice-forwarded, cleanly:**

```
PASS  nvkvm_guest.ko load (L2, unmodified nvkvm-guest.ko)
PASS  L2 sees "host reports 6 GPU(s)"                    (double hop)
PASS  nvidia-smi inside L2 lists all 6 T4s               (double hop)
PASS  cuInit                                              rc=0
PASS  cuDriverGetVersion                                  13000
PASS  cuDeviceGetCount                                    6
PASS  cuDeviceGet / cuDeviceGetName                       "Tesla T4", any index
FLAKY cuCtxCreate_v2                                       rc=999 (CUDA_ERROR_UNKNOWN) most attempts, rc=0 occasionally
```

**It is not deterministic and not device-specific.** Across ~9 back-to-back
`cuCtxCreate` attempts (fresh process each time, so a fresh isolate each time)
spread across all 6 device indices, **one attempt succeeded** (device 2, first
attempt of the run) and every other attempt on every device — including that
same device 2 on a later run — failed with 999. Re-running the identical,
statically-linked-against-nothing-but-libdl test binary directly on L1 (single
hop, no L2) succeeds every time, immediately, so the test binary and the T4s
themselves are not in question — this is specific to the second forwarding hop.

**Signature common to every observed failure**, with `NVKVM_DEBUG=1` on the L1
QEMU process hosting L2: `cuCtxCreate` gets a long way into RM object setup —
several `RM_ALLOC`s each followed by the QEMU-side `post-alloc SHARE`
(`nvkvm_isolate_handlers.c:2765` grant), an `RM_MAP_MEMORY` that resolves a real
`pLinear` address — and then, right before giving up: one more `RM_ALLOC` of
class `0x40` (`NV01_MEMORY_LOCAL_USER`) succeeds, is **immediately** `RM_FREE`d
(`cmd=0xc0104629`), a batch of `REAP_HANDLE`s runs, and `nvkvm_isolate: killed
isolate N` ends the trace. No `UVM` string appears anywhere in the log for a
failing attempt — the failure is upstream of `UVM_INITIALIZE`, inside RM setup,
which rules out the UVM/`RS_SHARE_TYPE_ALL` dup path this file documents
elsewhere in this doc as already fixed.

The only non-zero `nvstatus` seen in *some* (not all) failing runs is 15
consecutive `NV_ERR_NOT_SUPPORTED` (`0x56`) answers to RM control command
`0x2080182a`, called in a tight retry loop — notable because the *same* command,
on the *same* GPU, in the *same* process's `cuInit` a moment earlier, had
returned `0x0`. But a later failing run showed the identical alloc-then-
immediate-free-then-kill ending with **no** `0x56` anywhere in it, so that status
is at most one symptom, not a confirmed single cause — same "strongest
candidate, not a closed case" caveat the A100 entry above gives its own finding.

**Not chased further**, for lack of time on the rented box: whether isolate/
handle-table growth across repeated failed attempts (isolate ids and handle ids
were still climbing after ~9 attempts, all leaking one alloc/free/reap cycle's
worth of bookkeeping) is itself contributing to the failure rate, whether the
one success was a genuine race won rather than a fluke, and whether this is the
same root cause as the A100 finding above (both share the "clean enumeration,
`cuCtxCreate` dies with 999, alloc/free of one object right before" shape, but
the A100 case's confirmed mechanism — `RM_UNMAP_MEMORY`'s `p_linear_address`
never refilled, `nvstatus 0x57` — does not appear here at all; if related, it is
a different manifestation, not the same code path).

**What a retry would need:** an `NVKVM_DEBUG=1` trace of a *successful*
non-nested `cuCtxCreate` on this same L1, to diff against the doubly-forwarded
failing traces above and see whether `0x2080182a` (or anything else) actually
diverges, rather than being present-but-harmless in the working case too.
Not done here because the only L1 available was the live demo QEMU process
already serving a human audience, and restarting it to attach `NVKVM_DEBUG` was
out of scope for this pass.


### Priority note: display matters on consumer and workstation parts, not datacenter

Stated by the maintainer, recorded so nobody spends a night on the wrong bug.

Nobody attaches a monitor to an H100 and runs nvkvm with a display on it. On
**datacenter parts (A100, H100, and similar), display is the lowest priority
there is** — a graphics failure on one of those is not a release blocker, and a
graphics *pass* on one is not evidence the display path is healthy. What matters
on those cards is CUDA.

**Workstation cards are consumer for this purpose.** RTX A-series, RTX 6000 Ada,
RTX PRO 6000 and similar go into large desktops with monitors attached, so the
display path matters on them exactly as much as on a GeForce part. Do not group
them with datacenter silicon just because the name looks professional.

Headless display on datacenter parts may matter eventually for cloud-gaming-style
use, where frames are captured and streamed rather than scanned out. That is a
different feature from a physical display and is not what the entries below are
about.

Practical consequence: the A100 passing `vk_compute_dispatch`, EGL and a
pixel-checked GL draw while failing `cuCtxCreate` is **exactly the wrong way
round** for that card. The Vulkan pass is still useful as evidence — it is what
eliminated the "datacenter part" theory for the Hopper failure (itself since
resolved as a 570-branch driver defect) — but it is not a
result to celebrate, and the CUDA failure is the one that counts.

### X11 clients fall back to llvmpipe on a display-less A100 — NOT AN NVKVM BUG

**RETRACTED 2026-08-20.** This entry first claimed the EGL render-node query
failed *through nvkvm*. It does not: the host behaves identically, so nvkvm is
faithfully reproducing what the driver does on bare metal.

```
GUEST  device 0 (NVIDIA):  EGL_DRM_RENDER_NODE_FILE_EXT = (NULL)  err 0x300c
HOST   device 0 (NVIDIA):  EGL_DRM_RENDER_NODE_FILE_EXT = (NULL)  err 0x300c
```

The A100 has no display engine, so its CUDA-capable EGL device carries no DRM
association to report — on bare metal exactly as in the guest. A headless GNOME
on a bare-metal A100 would give its X11 clients llvmpipe for the same reason.
The behaviour below is therefore a property of that card plus this driver
package, not a forwarding gap, and there is nothing here for nvkvm to fix.

**Original observation retained**, because the chain is still worth knowing: Reproduced on an A100 (GA100, Ubuntu
26.04, GNOME 50 headless) — a different card, compositor and OS from the RTX
4070/weston case below, which makes it an nvkvm gap rather than anything
app-, card- or compositor-specific.

The chain, each step observed:

```
mutter:   Failed to query EGL render node path: One or more argument values are invalid.
Xwayland: glamor: GBM Wayland interfaces not available
Xwayland: Failed to initialize glamor, falling back to sw
glxgears: GL_RENDERER = llvmpipe (LLVM 21.1.8), Mesa 26.0.8
```

The compositor asks EGL for the render node backing its device; the query fails,
so it cannot advertise the GBM/dmabuf Wayland interfaces that Xwayland's glamor
requires; glamor initialisation fails; Xwayland falls back to software; and every
X11 client on that display gets llvmpipe.

**Wayland clients are unaffected.** On the same machine at the same moment,
GNOME Shell itself is genuinely GPU-accelerated — `nvidia-smi` lists it as a
client with type `M+C+G` holding 50 MiB. So this is specifically the
X11-via-Xwayland path, not GL in the guest generally.

The failing call is `eglQueryDeviceStringEXT(device,
EGL_DRM_RENDER_NODE_FILE_EXT)` — which fails on the host too.

**How this was got wrong, since the mistake is worth more than the finding:**
the guest was inspected, something was broken, and it was attributed to nvkvm
without checking whether the host did the same thing. That is the third time in
one session — the same error produced a phantom NVML bug (the unversioned
`libnvidia-ml.so` is missing on the host as well) and a phantom staging
no-op. **Check the host before blaming the forwarder.**

**Inferred, not proven**: that the same missing attribute explains the weston
`GL_OUT_OF_MEMORY` symptom recorded below. The two look like one cause with two
presentations — weston failed to allocate where GNOME degrades to software — but
that has not been retested on the 4070 since the root cause was found.

### X11 on weston/RTX 4070: a signal restarted a non-idempotent ioctl — FIXED 2026-08-20

**Fixed**, root-caused on the physical RTX 4070 (driver 595.84, guest weston
13.0.0 / Xwayland 23.2.6) with a discriminating regression test
([`tests/repro/signal_restart_export.c`](../../tests/repro/signal_restart_export.c)).

| | before | after |
|---|---|---|
| `(EE)` lines in one weston log | 982 | **0** |
| `GL_OUT_OF_MEMORY` events | 43 | **0** |
| X11 client windows | mapped but blank | **render** |

`glxgears`, `glmark2`, `xterm`, `xclock` and `xeyes` all draw correctly and
concurrently, alongside native Wayland EGL clients and Firefox in the same
compositor.

#### This is NOT the same bug as the A100 llvmpipe fallback — measured

The section below on the EGL render-node query records a *different* X11
failure, found on an A100/GNOME box, and the two were suspected to be one bug.
**They are not.** Checked directly on the 4070, in the pre-fix logs:

| signal | A100 / GNOME | RTX 4070 / weston |
|---|---|---|
| `Failed to query EGL render node path` | yes | **absent** |
| `glamor: GBM Wayland interfaces not available` | yes | **absent** |
| `Failed to initialize glamor, falling back to sw` | yes | **absent** |
| compositor GBM support | — | `DRM: supports GBM modifiers` |
| `GL_RENDERER` seen by the X client | `llvmpipe (LLVM 21.1.8), Mesa 26.0.8` | **`NVIDIA GeForce RTX 4070/PCIe/SSE2`** |

On the 4070 **glamor initialised fine and ran on the real GPU** — there was no
software fallback at any point. Same user-visible symptom ("X11 is broken"),
two unrelated causes. Fixing this one does not fix the render-node query, and
vice versa; keep them separate.

#### What was actually wrong here

`nvkvm_send_sync()` waited *interruptibly* for the host round-trip on the
isolate ioctl path. On a signal it aborted the in-flight host ioctl, waited for
the descriptor, then discarded the response and returned `-ERESTARTSYS` — so
the kernel restarted the syscall from the top.

The request is already on the wire by the time a signal can arrive, so **the
operation may already have taken effect** — and the export calls on that path
are not idempotent:

- `'F'` nr `0xd4` — parks an RM object on an export fd
- `'d'` nr `0x49` (`GEM_EXPORT_NVKMS_MEMORY`) — associates a bo with it

Both are steps of `eglExportDMABUFImageMESA`. Repeat either on the same handle
— exactly what a restarted syscall does — and the driver correctly refuses with
`EINVAL`, because the export already exists. **NVIDIA's EGL reports that refusal
as `GL_OUT_OF_MEMORY`**, which is the single piece of mislabelling that sent
this down the wrong path for so long.

Fix: complete the syscall instead of restarting it. The response is already in
hand after the uninterruptible wait, so hand it back — the ordinary kernel rule
for a syscall that has made unrepeatable progress. The abort request is kept, so
a genuinely long GPU operation still cannot pin the thread (verified: `SIGTERM`
on a GL client is honoured in 13 ms), and the signal is not lost.

#### Why only X11, and why only Xwayland

**Xwayland arms a periodic `SIGALRM`** (its frame/scheduler timer), which
guarantees a signal eventually lands inside one of these ioctls. weston and
native Wayland clients do not — which is exactly why they always worked and only
X11 broke. Nothing about X11 *as a protocol* was at fault; it was the timer.

The regression test reproduces it with **no compositor, no X server and no
Xwayland at all** — just EGL dma-buf exports plus a `SIGALRM` — which is what
confirms the cause is the signal-restart rather than anything about X11:

```
without the fix:  16/200 exports succeeded, 184 FAILED
with the fix:    300/300 exports succeeded,   0 failed
```

#### Three wrong diagnoses, all of which looked right

1. **"An allocation/VRAM problem."** Never was. The card has 12 GB and the
   desktop used a few hundred MB. `GL_OUT_OF_MEMORY` from NVIDIA's EGL means *a
   failed import/export*, not exhaustion. Do not chase memory limits on it.
2. **"No client window is ever mapped; `xwininfo` sees a 10x10 stub."** False,
   and it aimed the search at the wrong layer. Measured while blank: the window
   is mapped, correctly sized, framed by the WM and `IsViewable`; the client
   renders on the real GPU at a clean **58 FPS**, vsync-throttled by weston's own
   frame callbacks. **Every signal reported success and the screen was empty.**
   What failed was the window's *content* reaching the compositor. (The "10x10
   stub" is the Weston WM's own helper window — the real client window is a
   child of the WM frame, which `xwininfo -root -children` does not show. Use
   `-tree`.)
3. **"Some heavy client (Minecraft/CEF) is at fault."** No application was ever
   at fault. `glxgears` reproduces it exactly.

#### The two cheap tests that settled it

- **The trivial client.** `glxgears` under `weston --xwayland` reproduces it in
  full, which eliminates every application-specific theory in one command.
- **The bare-metal control.** The *same* weston + Xwayland + glxgears + driver on
  the host, with an ioctl trace: **zero `ERESTARTSYS`, zero `EINVAL` across
  21,389 ioctls.** In the guest, **every** `EINVAL` was immediately preceded by
  an `ERESTARTSYS` on the same fd — 51 of them. That comparison turned a
  correlation into a cause, and it was the step that had been missing.

`strace` decodes these by number without knowing the driver, so it mislabels
them — the same `'d'` nr `0x49` prints as `DRM_IOCTL_I915_GEM_PIN` on one box and
`DRM_IOCTL_VIRTGPU_GET_CAPS` on another. Match the raw
`_IOC(..., 0x64, 0x49, 0x18)` form, not the pretty name.

#### Regression-testing

```bash
tests/repro/signal_restart_export.c     # headless, runs anywhere the GPU comes up
# or end to end, needing a display:
weston --backend=drm --renderer=gl --socket=wayland-0 --idle-time=0 --xwayland --debug &
DISPLAY=:0 glxgears &      # must be VISIBLE, not merely running at 60 FPS
weston-screenshooter       # needs weston --debug, else "unauthorized"
```

**Check the screenshot, not the frame rate.** A blank X11 window still reports a
mapped viewable window, a real NVIDIA renderer and a healthy 58 FPS. No counter
anywhere in the stack distinguishes it from a working one — only the pixels do.

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

### `UVM_DISABLE_READ_DUPLICATION` is answered locally on fallback ranges

`libcuda` issues cmd 45 on a freshly created managed range, **inside**
`cuMemAllocManaged`, and treats its failure as fatal to the allocation.
Measured on an RTX 4060 / 580.95.05 by injecting `NV_ERR_INVALID_ADDRESS` into
one command at a time: 45 alone turns `cuMemAllocManaged` into
`CUDA_ERROR_INVALID_VALUE` with a NULL pointer, while the same injection into
51 / 42 / 43 / 44 / 46 / 47 is tolerated and the data still verifies.

A managed-memory fallback range is an **external** range host-side, and read
duplication is a managed-only policy — `read_duplication_set()` goes through
`uvm_api_range_type_check()`, which returns `UVM_API_RANGE_TYPE_INVALID` for a
non-managed range and hence `NV_ERR_INVALID_ADDRESS`
(`uvm_policy.c:59-106`, `:877-879`). Forwarding it therefore cannot succeed.

The guest answers it instead (`src/guest/nvkvm_main.c`, before the UVM state
recording). What the answer asserts is *"read duplication is not enabled on this
range"*, which for an external range is simply true: there is nothing to
disable. Two properties keep this honest:

- **It is guest-side.** The guest is untrusted anyway, so an answer it
  synthesises grants it nothing it did not already have. QEMU fabricating a
  driver response would be a different thing entirely — a trusted component
  inventing kernel results — and is deliberately not what happens here.
- **It is scoped, never blanket.** `nvkvm_mmap_range_is_ext_backed()` requires
  exact containment in one range *this fd backed itself*. Cmd 45 against any
  other range is forwarded and fails honestly.

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

### The compute-only build configuration is not exercised by anyone

`src/qemu/` has four meaningful build configurations, and the default build
covers exactly one of them:

| config | selected by |
|---|---|
| A | graphics + `CONFIG_OPENGL` — the default, and the only one normally built |
| B | `-DNVKVM_QEMU_GRAPHICS=0` |
| C | `!CONFIG_OPENGL` |
| D | both |

B, C and D compile a different arm of every `#if defined(CONFIG_OPENGL) &&
NVKVM_QEMU_GRAPHICS` in the present path — including the no-op stubs. Because
nothing builds them routinely, those stubs drift from the header they are
supposed to satisfy and nobody notices. This is not hypothetical: during the
2026-08-20 audit, config B was found **already broken** —

```
nvkvm_present_egl.c:659:6: error: conflicting types for 'nvkvm_present_submit'
```

— a stub whose signature had fallen behind its own declaration. It was repaired
in `53e3a96`, and all four configurations were then compiled clean across all 11
nvkvm translation units under QEMU's full warning set.

There is no CI in this repo. Until there is, **build B/C/D by hand before
touching the present path**, or the compute-only deployment breaks silently
while every local build stays green. For C and D the `!CONFIG_OPENGL` arm has to
be selected genuinely — a `config-host.h` shim ahead of the build dir on the
`-iquote` path — rather than approximated through the graphics gate, or the arm
you think you are testing is not the one that compiles.

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


### Corroborated on unrelated hardware, same day

This is not specific to the T4 box or to CUDA.  On a completely different
machine -- an RTX 4070 desktop, different host CPU vendor, different guest --
nested nvkvm came up the same way: the L2 module loaded, `nvidia-smi` listed
the GPU, and then **`clCreateContext` failed**.  That is OpenCL rather than the
CUDA driver API, on different silicon, and it fails at the same point in the
same order: enumeration works, context creation does not.

Two machines, two APIs, one boundary.  Whatever this is, it is a property of
the second forwarding hop and not of a card, a driver build, or a guest image
-- which is the useful half of the finding, because it means a fix can be
developed anywhere rather than only on rented multi-GPU hardware.
