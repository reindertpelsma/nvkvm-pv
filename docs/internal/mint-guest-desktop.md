# A Linux Mint desktop on the nvkvm head

Status: **working, accelerated, unattended — via weston.**  Cinnamon's own
session does not work yet, and the reason is a real nvkvm gap (below).

![Mint apps on the nvkvm head](../img/mint-guest-weston-desktop.png)

## What works (measured on the RTX 4070 box, host driver 595.84)

Guest: Linux Mint 22.3, kernel 6.14.0-37-generic, installed by
`scripts/setup_mint_guest.sh`.

The guest **boots unattended to a composited desktop on the nvkvm KMS head**,
and it is genuinely on the GPU.  After a cold `systemctl reboot` of the guest,
with nothing typed:

- `nvkvm-guest.service` active, `nvkvm_guest` loaded, all 3 9p mounts present
- `/dev/dri/card0` and `renderD128` present as `root:video` / `root:render` 0660
- `weston --backend=drm --xwayland` running on tty1, and from its log:

      using /dev/dri/card0
      Using rendering device: /dev/dri/renderD128
      EGL vendor: NVIDIA
      GL version: OpenGL ES 3.2 NVIDIA 595.84
      GL renderer: NVIDIA GeForce RTX 4070/PCIe/SSE2

Clients are accelerated on both display protocols — checked separately rather
than assumed:

| client path | renderer |
|---|---|
| Xwayland (`glxinfo -B`, `DISPLAY=:0`) | `NVIDIA GeForce RTX 4070/PCIe/SSE2`, GL 4.6.0 NVIDIA 595.84 |
| native Wayland (`eglinfo -B`, Wayland platform) | `NVIDIA GeForce RTX 4070/PCIe/SSE2`, GL 4.6.0 NVIDIA 595.84 |
| `glxgears -info` | `GL_RENDERER = NVIDIA GeForce RTX 4070/PCIe/SSE2`, **60.0 FPS** vsync-locked |

Firefox, Nemo and glxgears were run together and all three rendered (see the
screenshot).  No llvmpipe anywhere on any of these paths.

## Why not Cinnamon's own session — the cursor-plane gap

Cinnamon's Wayland session gets *much* further than Xorg does.  muffin brings
up KMS on the nvkvm head correctly, and then dies:

    muffin-WARNING: Failed to set hardware cursor
                    (drmModeSetCursor failed: No such device or address),
                    using OpenGL from now on
    cinnamon-session-binary: WARNING: Application 'cinnamon-wayland.desktop'
                    killed by signal 11

The backtrace from the core is unambiguous:

    #0  disable_hw_cursor_for_crtc        (libmuffin.so.0)
    #1  meta_cursor_renderer_update_cursor
    #2  meta_cursor_renderer_set_position
    #3  meta_backend_real_post_init
    #4  meta_backend_native_post_init
    #5  meta_backend_post_init
    #6  meta_init_backend
    #7  main                              (cinnamon)

So there are **two separate defects stacked**, and they should not be conflated:

1. **nvkvm gap (ours).**  `drmModeSetCursor` returns `-ENXIO` because nvkvm
   exposes no cursor plane.  `drm_simple_display_pipe_init()` creates exactly
   one plane — the primary — and never sets `crtc->cursor`, and the legacy
   cursor ioctl returns `-ENXIO` when a CRTC has neither `cursor_set`/
   `cursor_set2` funcs nor a cursor plane.

   **This is not something the host does too** — checked directly, because that
   is the assumption that has been wrong repeatedly on this project:

   | | planes | Overlay | Primary | Cursor |
   |---|---|---|---|---|
   | host `nvidia-drm` (`modetest -M nvidia-drm -p`) | 12 | 4 | 4 | **4** |
   | guest nvkvm | **1** | 0 | 1 | **0** |

2. **muffin bug (theirs).**  Failing that ioctl is legal — muffin even says it
   will fall back to an OpenGL cursor — but it then SEGVs *inside the fallback*.
   A driver without a cursor plane is a supported configuration upstream, so
   muffin crashing here is its own bug, not a consequence of ours.

Note the shape of the fix matters.  Registering a *no-op* cursor plane just to
make the ioctl succeed would stop the crash and leave the user with **no visible
pointer at all**, because muffin would then believe the hardware cursor works
and stop compositing one.  A correct fix has to carry the cursor through to the
host — the guest plane's content forwarded to QEMU's console cursor
(`dpy_cursor_define` / `dpy_mouse_set`) — which means guest KMS + protocol +
QEMU-side work, not a one-liner.  Until then weston is the supported path:
it composites its own cursor and needs no cursor plane.

Also observed, not yet chased: `cinnamon --wayland --nested` inside weston
starts fully (applets load, no GL errors in its log) but its window renders
solid black.

## The native-Xorg glamor failure (unchanged, still open)

    (EE) modeset(0): Failed to create pixmap
    (EE) Fatal server error:
    (EE) failed to create screen resources

from `drmmode_set_pixmap_bo()` -> `glamor_egl_create_textured_pixmap_from_gbm_bo()`.
Reproduced; lightdm restart-loops on it ~22 times.

One datum narrows it: **EGL-over-GBM in the guest is genuinely NVIDIA**, not a
software fallback.  `eglinfo -B` reports `EGL vendor: NVIDIA` and
`NVIDIA GeForce RTX 4070/PCIe/SSE2` for both the GBM and surfaceless platforms.
So glamor is not failing because EGL fell back to software.  The untested next
measurement is whether `gbm_bo_get_fd()` / dmabuf export survives the round trip
through nvkvm.  **Not yet checked whether the host does the same thing** — that
must happen before calling this one an nvkvm bug.

`libEGL warning: egl: failed to create dri2 screen` appears before NVIDIA is
selected; that is Mesa failing and glvnd falling through, not nvkvm failing.
It is the same Mesa-first probe that produced the earlier
`AIGLX: Loaded and initialized nouveau` line.

## Traps worth not re-hitting

- **The infinite login loop.**  `/usr/bin/cinnamon-session` is a shell wrapper
  that, for a wayland session, re-execs through a LOGIN shell
  (`exec bash -c "exec -l '$SHELL' -c '$0 -l $*'"`) to inherit the login
  environment.  If `~/.bash_profile` is what launches the session, that login
  shell re-reads it and starts the session again — an infinite exec chain that
  spins at 100% CPU, spawns no compositor, and prints **nothing at all**.  The
  0-byte session log is the whole symptom.  `setup_mint_guest.sh` guards it with
  `NVKVM_SESSION_LAUNCHED`, which survives because exec preserves the
  environment.
- **DRM node permissions after a manual insmod** are `root:root` 0600 with no
  `by-path` links, because the nodes miss the boot uevent flow.  It is not a
  missing udev rule; `udevadm trigger --subsystem-match=drm` fixes it.
- **The guest module will not build from a copy of `src/guest/` alone** — it
  includes `../../src/common/nvkvm_proto.h`.  Copy the whole `src/` tree, or
  build in place from `/mnt/nvkvm`.
- lightdm must stay disabled; enabled, it restart-loops forever on the glamor
  bug and generates continuous disk I/O.
