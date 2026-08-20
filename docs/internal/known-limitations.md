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

### X11 clients: Xwayland's glamor cannot allocate through nvkvm — OPEN, unconfirmed

Wayland clients work; **X11 clients under `weston --xwayland` do not get a window.**
Observed on the RTX 4070 box, driver 595.84, guest weston with `--xwayland`:

```
118x glamor0: GL_OUT_OF_MEMORY ... Failed to allocate memory for texture.
 98x glamor0: GL_OUT_OF_MEMORY ... Failed to acquire the EGL Image memory.
```

4,949 `(EE)` lines in one weston log, every backtrace through
`libnvidia-eglcore.so.595.84`, the first firing seconds after Xwayland starts.
Xwayland does **not** exit — it runs, but its glamor acceleration cannot
allocate textures or EGLImages through the forwarded driver, so no client
window is ever mapped (`xwininfo` sees the root plus a 10x10 stub).

**Scope: this is not app-specific.** Every X11 client goes through the same
glamor path, so expect it to affect X apps generally — plausibly including
GNOME-on-Xwayland. The present path is a *different* code path and is
unaffected: it measures 60.0 swaps/s with zero drops in the same session. The
distinguishing feature is that Xwayland is a **second** GL consumer allocating
while the compositor already holds the GPU.

**What is NOT established**, and should not be repeated as if it were:
whether the OOM is genuine allocation exhaustion or an unimplemented/limited
path in the forwarding; and whether it is what makes the one launcher tested
(Minecraft, which embeds Chromium/CEF) die with `std::bad_function_call`. The
two co-occur, but that launcher created its browser window *before* crashing,
its binary is stripped, and there is no core — so the link is unproven.

**The cheapest decisive test, when a GPU box is available:** run a trivial X
client — `glxgears`, or `xterm` — under `weston --xwayland`. If it hits the
same glamor `GL_OUT_OF_MEMORY`, the application is irrelevant and this is a
pure nvkvm EGLImage-allocation bug, which is worth more than any individual
app. Do that before investigating any specific application.


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

