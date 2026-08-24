# QEMU patches

Everything nvkvm changes in **upstream QEMU** is in this directory: twelve
patch files, against the `v9.2.0` tag
(`ae35f033b874c627d81d51070187fbf55f0bf1a7`). Nothing else in the QEMU tree is
edited.

They exist as patches rather than as `sed` expressions in the build script for
one reason: a `git apply` is replicable by hand and a `sed` replacement is not.
A reader deciding whether to trust this can read twelve diffs; a maintainer
bumping the QEMU version resolves conflicts with ordinary tools instead of
rewriting editing logic; and "is it already applied?" is answered by
`git apply --reverse --check` rather than by grepping the tree for a comment
string, which used to mean that rewording a comment made a patch apply twice.

| patch | file | what it does |
|---|---|---|
| `0001-meson-register-virtio-nvgpu-sources.patch` | `hw/misc/meson.build` | lists the nine nvkvm `.c` files and the `nvkvm_inc/` include dir, gated on `CONFIG_VIRTIO` |
| `0002-virtio-add-virtio-nvgpu-to-device-name-table.patch` | `hw/virtio/virtio.c` | adds `[50] = "virtio-nvgpu"` to `virtio_device_names[]`, which otherwise stops at 41 and aborts on our ID |
| `0003-egl-helpers-import-dmabuf-via-texstorage.patch` | `ui/egl-helpers.c` | imports dma-bufs with `glEGLImageTargetTexStorageEXT`; NVIDIA rejects the legacy OES bind for external-only images |
| `0004-console-do-not-abort-on-deviceless-console.patch` | `ui/console.c` | skips non-graphic consoles in `qemu_console_lookup_by_device()`, which otherwise aborts QEMU on `screendump` |
| `0005-gtk-switch-to-guest-display-when-it-goes-live.patch` | `include/ui/gtk.h`, `ui/gtk.c`, `ui/gtk-egl.c`, `ui/gtk-gl-area.c` | switches the GTK window to the guest's head the first time it presents real content, once, and only from the page the window opened on |
| `0006-gtk-no-implicit-grab-on-click.patch` | `ui/gtk.c` | drops upstream's grab-on-first-left-click, so entering grab mode is Ctrl+Alt+G or the View menu and nothing else |
| `0007-gtk-grab-switches-the-guest-pointing-device.patch` | `ui/gtk.c` | on grab, makes a relative mouse the guest's current device; on ungrab, restores the absolute one. **Not known to work** — see its header |
| `0008-sdl2-show-the-guest-gpu-head.patch` | `include/ui/sdl2.h`, `ui/sdl2.c`, `ui/sdl2-gl.c`, `ui/sdl2-2d.c` | gives the SDL backend a dma-buf scanout path (it had none), creates the window from the GL path, and raises the guest's window once when it goes live. Ran on the RTX 4070 box; **the pixels themselves were only confirmed by eye** — see its header |
| `0009-sdl2-grab-switches-the-guest-pointing-device.patch` | `ui/sdl2.c` | 0007 for SDL, where `SDL_SetRelativeMouseMode()` is a real Wayland pointer lock. Pointer lock was **reported working** on that box; the evtest that would prove it was never read — see its header |
| `0010-kvm-retry-a-bare-KVM_RUN-EFAULT.patch` | `accel/kvm/kvm-all.c` | retries a bare `KVM_RUN` `EFAULT` for up to 3 s instead of killing the VM. An NVIDIA GPU mapping is `VM_IO\|VM_PFNMAP`, and `nv_fault()` returns `VM_FAULT_NOPAGE` **without installing a PTE** while the driver reinstates a revoked mapping; KVM turns that "come back later" into a fatal `EFAULT`. Measured clearing after **1465 ms** on the RTX 3050 laptop, which is why it looked permanent |
| `0011-qapi-ui-add-the-nvkvm-broker-display-type.patch` | `qapi/ui.json` | adds `-display nvkvm-broker,socket=PATH`: the display, the input grab and the compositor connection move into a separate privileged process (`src/broker/`) and QEMU keeps one unix socket. Gated on `CONFIG_LINUX` |
| `0012-hw-misc-build-the-nvkvm-broker-display-relay.patch` | `hw/misc/meson.build` | one line, adding `nvkvm_display_relay.c` to the list `0001` created. Note it is **not** gated on `CONFIG_OPENGL`: broker mode exists so a QEMU with no EGL can still display |

Each patch's commit message says *why* it is there. Read those before changing
one — several of them record a measurement (a driver version, an error code)
that is the whole justification for the change.

## Which of these should stop existing

Seven of the ten carry `ui/` changes, and `ui/` is the surface we would most
like to shed — it is generic front-end code, shared with every other QEMU user,
and the part a reviewer has least reason to trust us with.

**`0004` is an upstream QEMU bug, not an nvkvm-specific change.** The intent is
to submit it to qemu-devel. If you find it merged upstream, or you bump to a
QEMU that already has it, **delete the patch** — do not forward-port it. There
is nothing nvkvm-specific left in it once upstream has the fix. This is the one
with a clear path out, and it is worth actually sending.

`0003` is not upstreamable as written: it resolves the symbol rather than
checking for `EXT_EGL_image_storage` properly, and has only been tested against
NVIDIA. Fixing both would make it upstreamable — the underlying problem is not
ours alone.

`0005` is not upstreamable as written either, and should not be sent as-is: it
is nvkvm-specific policy ("the interesting console is the one that is not the
boot VGA") hardcoded into a generic front end, with an nvkvm-branded symbol and
log line. The gap it fills is general — any device whose console registers after
the VGA has this problem, and upstream gives a console no way to say "I have
something worth looking at". A `dpy_console_request_focus()`-shaped mechanism,
with the front end owning the policy, would be the upstreamable version. Until
someone writes that, this stays downstream.

`0006` is a behaviour change upstream deliberately wants for mouse-only guests,
so it cannot go as-is; the upstreamable version is a display option
(`-display gtk,grab-on-click=off`) defaulting to today's behaviour.

`0007` and `0009` are the same idea in two front ends, which is itself the
argument for the upstreamable version: a shared helper in `ui/input.c` that
expresses "a grab wants a relative device" once, instead of once per backend.
`0007` was tried by hand and did not work — on GTK it cannot, because that
backend never asks the compositor for a lock. `0009` is the same change where
the lock is real, and pointer lock was reported working with it in place.
Neither has an evtest trace behind it; both headers say so at the top.

`0008` is two things wearing one number, and only one of them is ours. Its
first three parts — SDL had no `dpy_gl_scanout_dmabuf`, nothing on the scanout
path created a window, and nothing ever set `qemu_egl_display` for an SDL
window — are plain upstream gaps, and `ui/sdl2.c` is the last front end with a
GL context and no dma-buf scanout. Those belong on qemu-devel roughly as they
stand. The window-raise is nvkvm policy in a generic front end, exactly like
`0005`, and would need the same `dpy_console_request_focus()`-shaped mechanism
before it could go anywhere.

`0001` and `0002` are downstream by nature: they register a device that is not
upstream and claim a virtio ID that is not assigned. They will never leave.

## "It works, but it is slow"

A second hardware session, on an RTX 3050, took `0008` + `0009` all the way: desktop
rendering, pointer lock working, Super forwarded to the guest, and a single
ctrl-alt-g each way. The half of `0009` that closed the gap between "the lock
engages" and "the mode is usable" was written as a separate `0010` at the time
and squashed in later (dcaff60); the number `0010` now belongs to the KVM
EFAULT retry above.

The earlier session with `0008` + `0009` reported the SDL window usable
and pointer lock working, and also reported it as slow. That is not diagnosed.
The ranked suspects, the check for each, and the reason none of them is a line
of code in `0008`, are written out under **IF IT IS SLOW** in
[`0008-sdl2-show-the-guest-gpu-head.patch`](0008-sdl2-show-the-guest-gpu-head.patch).
The short version: `nvkvm_present_decide_mode()` defaults to **readback**
unless `NVKVM_PRESENT_MODE=gl` is in the environment, and readback copies 8 MB
out of the GPU and 8 MB back into it every frame. Check `grep "window mode"` in
the QEMU log before looking anywhere else.

## What is *not* here

The device itself — ~12,000 lines across nine `.c` files and their headers in
`src/qemu/` (eleven until DEAD-1 deleted `nvkvm_dispatch.c` and
`nvkvm_frontend.c` on 2026-08-24) — is **copied** into `hw/misc/`, not patched in. It touches nothing
upstream, so a patch would be 12,900 lines of pure addition and no easier to
review than the files themselves. See [`docs/howto/build.md`](../docs/howto/build.md).

## Applying them

```bash
cd /opt/qemu-src
git apply --check /path/to/nvkvm/patches/*.patch   # all-or-nothing dry run
git apply         /path/to/nvkvm/patches/*.patch
```

`scripts/build_qemu.sh` does exactly this, plus the same `--reverse --check`
already-applied test for each patch so a re-run is a no-op.

`0005`–`0007` only touch files that are compiled when QEMU is configured
`--enable-gtk`, and `0008`–`0009` only files compiled when it is configured
`--enable-sdl` (both come from `NVKVM_QEMU_UI=1`). The default headless build
applies all five and never compiles a line of them.

## Regenerating them after a QEMU version bump

```bash
git clone --depth=1 --branch vX.Y.Z https://gitlab.com/qemu-project/qemu.git /tmp/qemu-next
cd /tmp/qemu-next
git checkout -b nvkvm-X.Y
git am /path/to/nvkvm/patches/*.patch    # fix conflicts, keep the messages
git format-patch --no-signature --zero-commit -o /path/to/nvkvm/patches vX.Y.Z..HEAD
```

Then rename the generated files back to the short names used here, and update
`QEMU_VERSION` in `scripts/build_qemu.sh`.
