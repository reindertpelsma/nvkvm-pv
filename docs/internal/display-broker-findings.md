# Display broker: what the source says

Findings from designing a privileged display broker — a program that owns the
host display and input so the VMM can be sandboxed. Recorded because several
successive designs died on contact with the code, and each death is a fact
about nvkvm that outlives the design that hit it. Findings 5 and 6 are
investigated-and-rejected approaches, written up with the argument that
generalises so they are not re-proposed.

Nothing here needed hardware. All of it is `drivers/gpu/drm/`, the NVIDIA open
kernel modules, and this repo.

---

## 1. DRM leases work on nvidia-drm, with exactly one condition

`DRM_IOCTL_MODE_CREATE_LEASE` produces a DRM fd restricted to a named subset of
mode objects, and it is passable over `SCM_RIGHTS`. It is **DRM core code, not
driver code**:

- `drm_ioctl.c` registers it as
  `DRM_IOCTL_DEF(DRM_IOCTL_MODE_CREATE_LEASE, drm_mode_create_lease_ioctl, DRM_MASTER)`.
- The only driver gate inside is
  `if (!drm_core_check_feature(dev, DRIVER_MODESET)) return -EOPNOTSUPP;`.

`nvidia-drm` sets `DRIVER_MODESET | DRIVER_ATOMIC` in
`nv_drm_update_drm_driver_features()` — but **only when the `modeset` module
parameter is true**. Verified against open-gpu-kernel-modules 580.159.04.

So: **leases work on NVIDIA iff `nvidia-drm.modeset=1`**, and no
NVIDIA-specific support is involved either way. `modeset=0` is the single
failure mode, and it fails as `-EOPNOTSUPP` from `CREATE_LEASE`.

Two further facts worth having:

- **Becoming DRM master needs no capability in the normal case.** Opening a
  card node when nobody holds master makes you master (`drm_open` →
  `drm_master_open`). `CAP_SYS_ADMIN` is required only to *re-take* master
  after dropping it (`drm_master_check_perm()`). A program that never drops
  master therefore needs only `video`-group access to the node.
- **A lessee can drive the head unaided.** `PRIME_FD_TO_HANDLE` is
  `DRM_RENDER_ALLOW`, `ADDFB2` is unrestricted, and `PAGE_FLIP`/`ATOMIC` are
  `DRM_MASTER` — which a lessee is. Flip-complete events arrive on the lease
  fd, so vblank needs no help from the lessor.

## 2. You cannot lease a *window*

A window is composited; a plane is not. There is no mechanism, on either
display stack, that yields a DRM plane corresponding to a window:

- **X11**: RandR 1.6 output leasing (`RRCreateLease`) works, but the X server
  **detaches the output from the desktop** for the lease's duration.
- **Wayland**: `wp_drm_lease_device_v1` exists, but every compositor that
  implements it advertises only connectors marked **non-desktop** — the
  VR-headset case. wlroots/sway, KWin and Mutter all decline to offer a
  connector that is part of the desktop. That is policy, not protocol.

So a lease-based design necessarily hands the VM **a whole monitor**, not a
window. For nvkvm that is the wrong customer: nvkvm exists so that *one* GPU
can be shared, and anyone with a monitor to spare almost certainly has a second
GPU and is better served by ordinary VFIO passthrough.

## 3. QEMU's display path is ALREADY zero-copy — do not justify a broker on copy count

Easy to get wrong, because both paths live in `src/qemu/nvkvm_present_egl.c`:

- **`nvkvm_present_gl()`** (~line 585) is the *display* path. It calls
  `dpy_gl_scanout_dmabuf(p->con, buf)` + `dpy_gl_update()`, handing the guest's
  dma-buf straight to the UI backend as the scanout. No readback.
- **`nvkvm_present_capture()`** is the *capture* path for the webapp and NVENC.
  It does `egl_fb_read()` → `glReadPixels` to CPU. This one is a copy, and it
  is not the display.

What remains in the display path is a GPU blit *inside the UI backend*
(`scanout_flush` imports the dma-buf as a texture and `glBlitFramebuffer`s it
into the window framebuffer). That is what any windowed GL client does.

**The justification for a broker is privilege separation, not copies**: the
display-server connection moves out of the VMM, so a sandboxed QEMU needs no
X11 or Wayland socket. An X11 socket inside a sandbox is close to no sandbox —
X11 has no inter-client isolation.

Related trap, documented in `patches/0008`: `dpy_gl_scanout_dmabuf()` in
`ui/console.c` is `if (dcl->ops->dpy_gl_scanout_dmabuf)` — a
`DisplayChangeListener` **without that callback is skipped silently**, no
warning. That is why SDL showed nothing before patch 0008. Any new display
backend must implement it or every frame is dropped inside QEMU.

## 4. The guest cannot render into a host-supplied buffer

The most attractive broker design is: broker allocates a buffer, passes the fd
down to QEMU, QEMU installs it as the guest's scanout target, the guest renders
directly into it, the broker attaches it to its window. Fd travels downward
only; nothing is described back up; no copies anywhere.

**It does not work, because the guest allocates its own scanout buffer and
chooses its properties.** From `src/guest/nvkvm_drm.c:377`, on
`nvkvm_fb_stub_handle()`:

> Present path (#102): map a scanout framebuffer back to the host/stub buffer
> behind it. **A compositor's scanout buffer is an NVIDIA bo allocated via the
> render node**, so its guest GEM is one of our proxy objects carrying the stub
> handle + owning isolate.

The present path is a **lookup**, not an installation: it discovers which host
buffer the guest already chose. `nvkvm_kms.c` confirms the same shape from the
other side — the virtual head is a `drm_simple_display_pipe` that accepts
whatever framebuffer the compositor flips, and its modifier list exists so that
*the guest's* GBM can negotiate a layout (`0x0300000000606014` and
`0x0300000000e08014`, "read off real bos that NVIDIA's own GBM produced
in-guest").

Four specific obstacles, any one of which is fatal:

1. **The compositor owns the decision.** Mutter, Cinnamon, wlroots and Xorg
   allocate their scanout bos through GBM/EGL. There is no interface for
   "render into this buffer I am handing you", and adding one means patching
   every guest compositor.
2. **The modifier is chosen by the guest, after the fact.** The compositor
   negotiates it from the plane's `IN_FORMATS`. A broker that allocated first
   cannot know what the guest will pick, and the guest's pick must not flow
   upward.
3. **There are several buffers, not one.** A compositor cycles a handful of
   scanout bos — measured at 3, which is why `nvkvm_present_egl.c` carries an
   8-entry import cache. Substituting a single broker buffer for all of them
   destroys double buffering; substituting one means the other frames still
   show the guest's own allocations.
4. **They are RM objects inside a specific isolate.** The tree can *import*
   across isolates (`nvkvm_gem_xiso_*`), producing a proxy GEM — but that is an
   import after allocation, not a substitution at allocation time, and
   scanout-ness is not a flag visible on the underlying RM allocation.

**Consequence**: a windowed broker must have QEMU draw the guest frame into the
broker's target, which costs one GPU blit. Given finding 3, that blit replaces
the one the UI backend does today rather than adding to it — so the cost is
roughly par, and the gain is privilege separation. It is not the free win the
zero-copy framing suggested.

On Wayland there is a genuine, modest improvement available on top: the broker
can attach its target to a `wl_surface` via `zwp_linux_dmabuf_v1`, so the
compositor takes the buffer as the surface's content rather than as something
to draw into a framebuffer the client owns. That skips the client-side blit
QEMU's GTK/SDL path cannot avoid. On X11 expect parity with today, not better.

## 5. Buffer substitution: possible, but it cannot pay for itself

The idea: since every guest GPU allocation is forwarded, intercept the
compositor's scanout allocation and hand back a buffer the broker allocated.
Then the guest renders straight into the broker's buffer, the fd only ever
travels downward, and the privileged side parses nothing.

**Scanout-ness IS observable**, and an earlier version of this document was
wrong to say otherwise. It is not an attribute you have to dig out of an RM
allocation — it arrives as its own allowlisted ioctl.
`src/qemu/nvkvm_drm_allowlist.h:56`:

> `GEM_ALLOC_NVKMS_MEMORY` (0x0b): **the NVIDIA gbm backend's scanout-buffer
> allocation** (#109 present path). … Params are flat scalars
> (handle/block_linear/compressible/memory_size/flags) with NO embedded guest VA.

But the same comment carries the qualifier that decides it:

> **SCANOUT is only a memory-layout capability, not a CRTC attachment.**

So 0x0b means "give me a buffer with scanout-capable layout", not "this is the
buffer that will be flipped". Compositors allocate such buffers speculatively as
direct-scanout candidates they may reject, and every isolate on the desktop uses
the same ioctl. Matching would have to be heuristic — on `memory_size` plus
`block_linear`/`compressible` — and a wrong match does not fail loudly; it
silently redirects some other client's framebuffer into the display.

**The decisive objection is not the matching, though — it is that substitution
cannot deliver what it was for, even with perfect matching.** A compositor
renders into its scanout bos *in rotation*, typically three of them, which is
why `nvkvm_present_egl.c` carries an 8-entry import cache. The broker would
therefore hold N buffers and still have to be told **which one is currently
front** — information that only the flip knows (`nvkvm_pipe_update` →
`nvkvm_req_present`, which forwards the stub handle). That is a message flowing
VMM → broker.

So substitution does not remove the upward channel. It shrinks it from
"fd + geometry" to "an index into a set the broker owns" — and the entire
justification for taking on heuristic matching, plus substitution logic inside
the stub, was that it removed the channel completely. It does not. Rejected.

Secondary reason, worth keeping: the logic would live in
`src/stub/nvkvm_stub.c`, a freestanding `-nostdlib -static` binary with no libc,
whose design goal is to be a dumb forwarder. Putting a fuzzy size-and-layout
match in the least-privileged component, in a codebase whose recurring bug class
is cross-boundary confusion, is the wrong direction.

## 6. Moving the blit into the isolate: the stub is not a GPU client

The idea: have the isolate blit the guest's scanout bo into the broker's buffer
with the copy engine, so QEMU never touches pixels at all.

**Correcting the obvious wrong reason first: the copy-engine classes ARE
allowlisted.** `nvkvm_fe_alloc_allowlist.h` carries `0xc5b5`, `0xc6b5`,
`0xc7b5`, `0xc8b5`, `0xc9b5` and `0xcab5` — the DMA_COPY families from Turing
through Blackwell. Anyone rejecting this on "the classes aren't permitted" is
right by accident, and will re-propose it the moment they check.

The real obstacle is that **allocating a class is not submitting work.** The
stub (`src/stub/nvkvm_stub.c`, ~3,400 lines) is a freestanding static binary:
`-nostdlib -static -fPIE`, no libc, no pthread, futex-based primitives, a
seccomp filter, and a worker pool whose entire job is "execute the ioctl QEMU
forwarded and write back the response". It never submits GPU work; the *guest*
does, and the stub relays.

A copy-engine blit would make the stub a real GPU client: allocate a channel and
a pushbuffer, build DMA objects and semaphores, submit, and wait. That converts
the smallest and most heavily audited component in the system into one that
drives the GPU — the opposite of what it exists to be. It also has no GL context
and no way to acquire one without a libc and a driver stack. Rejected.

## 7. In broker mode QEMU needs no graphics stack at all

The sharpest consequence of the fd-up design, and the reason it is worth doing.

`nvkvm_req_present()` already **receives** a host dma-buf fd from the stub over
`SCM_RIGHTS`. Today it stashes that fd in a slot and the consumer either hands
it to the UI (`dpy_gl_scanout_dmabuf`) or imports and reads it back. In broker
mode the substitution is at exactly that point: **relay the same fd onward over
`SCM_RIGHTS` instead of consuming it.** QEMU forwards a descriptor it was handed
and touches no pixels.

Checked, and it holds:

- **The GL path never imports in nvkvm's own code.** `nvkvm_present_gl()` builds
  a `QemuDmaBuf` and calls `dpy_gl_scanout_dmabuf()`; the *UI backend* imports.
  `nvkvm_present_egl_ensure()` — the private EGL context — is reached from only
  two places: `nvkvm_present_capture()` (#107 capture) and
  `nvkvm_present_thread_start()`, which serves the **readback** path only. In
  broker mode there is no UI backend and no readback, so nothing imports.
- **No cursor compositing exists to lose.** The guest head has no cursor plane
  (`drm_simple_display_pipe_init()` creates only the primary), and carrying the
  cursor to the host via `dpy_cursor_define`/`dpy_mouse_set` is explicitly *not
  implemented* — `docs/internal/mint-guest-desktop.md` records that the
  supported path is weston, which "composites its own cursor and needs no cursor
  plane". The pointer is already inside the scanout buffer.
- **No damage tracking, scaling or format conversion.** Whole frames are
  presented. The one memcpy in the path (`nvkvm_present_publish`) exists solely
  to feed QEMU's 2D console and disappears with it.
- **`qemu_console_resize()` is bookkeeping**, not a texture — and the geometry
  goes to the broker in the descriptor instead.

So: **a sandboxed QEMU in broker mode holds no display-server connection and no
graphics driver stack.** No `libEGL`, no `libnvidia-eglcore`, no GL context, and
no `/dev/dri/renderD*` for display. The container needs the virtio device, the
isolate sockets and the broker socket.

Two profiles, and the README must say which:

| profile | needs |
|---|---|
| broker display only | no EGL, no GL, no render node |
| broker display **+ capture/NVENC** | EGL and the render node, for `nvkvm_present_capture()` — but still no display server |

One implementation consequence: `nvkvm_present_egl.c` is compiled under
`#if defined(CONFIG_OPENGL) && NVKVM_QEMU_GRAPHICS`. A no-EGL broker build must
therefore put the relay path **outside** the `CONFIG_OPENGL` gate, or disabling
OpenGL compiles the present path away entirely.

~~Unverified~~ **— now compiled, see finding 8.** On X11,
`DRI3PixmapFromBuffers` + `PresentPixmap` lets the broker present a dma-buf with
no GL context, the way `zwp_linux_dmabuf_v1` does on Wayland, so the broker
needs no GL on either backend.

---

## 8. X11 DRI3/Present needs no GL — compiled, not run

Finding 7 left this as reasoning because the `xcb-dri3`/`xcb-present` headers
were absent. They were installed and the code was written against them, so the
reasoning is now a compile:

- `xcb_dri3_pixmap_from_buffers()` (DRI3 1.2) takes width/height, per-plane
  stride and offset, **depth and bpp**, an explicit **modifier**, and the fds.
  `xcb_dri3_pixmap_from_buffer()` (DRI3 1.0) is the implicit-modifier form.
  Neither needs a GL context; the import happens **in the X server**.
- `xcb_dri3_get_supported_modifiers(window, depth, bpp)` returns the window and
  screen modifier lists. That is the X-side equivalent of
  `zwp_linux_dmabuf_v1`'s `modifier` events, i.e. a real answer to "validate
  against what the GPU advertises" rather than a hardcoded list.
- `xcb_present_pixmap()` puts it on screen; `PresentCompleteNotify` is the
  pacing signal and `PresentIdleNotify` is the buffer release. Both arrive as
  XGE generic events, which is why the backend is **pure xcb**: mixing Xlib's
  event queue with `xcb_poll_for_event()` loses them.

Three constraints that shape the backend, all from the protocol rather than
from taste:

1. **`PresentPixmap` requires the pixmap and the target window to be the same
   size**, and a window-managed toplevel is sized by the WM. So the backend
   uses **two windows**: the toplevel owns focus/grab/fullscreen, and a plain
   child window — which no WM ever touches — is the present target, resized to
   whatever the guest just flipped.
2. **DRI3 has no fourcc**, only depth + bpp; channel order comes from the
   visual. XRGB8888 and ARGB8888 are both imported as depth 24 / 32 bpp, which
   is correct for a scanout (alpha is not composited) and also satisfies
   Present's depth-match rule with a single depth-24 child window.
3. **xcb takes ownership of every fd handed to it and closes it once sent**
   (`xcbext.h`: "the file descriptor given is owned by xcb"). The broker's
   `attach()` contract is that the caller owns the fd, so the X11 backend must
   `dup()` before handing it over. Getting this wrong is a double close in the
   privileged process.

Still unverified, and it is the interesting half: whether the **NVIDIA DDX**
accepts NVIDIA's block-linear modifiers through `PixmapFromBuffers`, and
whether `DRI3GetSupportedModifiers` reports them at all. That needs the GPU.

## 9. `zwp_linux_dmabuf_v1` must be bound at version 3, not the latest

A trap worth recording because binding "the newest version the compositor
offers" is the reflex, and here it silently breaks format validation.

From **version 4** the compositor is required NOT to send the `format` and
`modifier` events — the per-surface `zwp_linux_dmabuf_feedback_v1` object
replaces them. A broker that binds 4 therefore ends up with an **empty**
advertised-format set, and since finding-3-style validation rejects anything
not in that set, **every frame is rejected** with a message about an
unadvertised format. Cap the bind at 3.

Version 2 is the floor: `create_immed` arrived there. `create` (version 1)
answers asynchronously, and waiting for the answer means a compositor round
trip inside the path that also carries input — the coupling this design forbids.

---

## Status

The code on branch `display-broker` implemented design 1 (DRM lease), which
findings 2 and 4 supersede. Branch **`display-broker-v2`** implements the
fd-up design of finding 7: the guest's scanout dma-buf, which QEMU is already
handed over `SCM_RIGHTS` every frame, is relayed onward to the broker, which
wraps it in a `wl_buffer` or a DRI3 pixmap.

What carried over from `display-broker` unchanged in shape: the unix socket,
`SO_PEERCRED`, the grab/focus/hotkey policy, motion coalescing, the privilege
drop, the `--backend test` harness and `selftest.sh`. What was deleted:
`nb_session_drm.c`, the lease halves of both backends, the KMS ioctls in the
QEMU-side file, and — with them — the last reason for the broker to link
libdrm at all.

What is new, and is the part that did not exist before: the privileged side now
**parses** input from an untrusted VMM (a descriptor and an fd), so the
validator in `nb_validate_desc()` is where the security of the whole design
now lives. `src/broker/README.md` §3 is its specification and §7 is the honest
verified/not-verified list.
