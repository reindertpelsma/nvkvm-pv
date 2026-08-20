# Mint guest desktop — work in progress

Status: **no desktop rendering yet.** This file records what is verified, what
is only assumed, and where to resume.  Written mid-session before a planned
host power-down (SSD being moved off a failing USB-SATA bridge onto real SATA).

## Verified on the RTX 4070 box (host driver 595.84, guest Mint 22.3, 6.14.0-37)

- The two fixes in a115f5c are load-bearing and hold: with the module built and
  inserted, `/dev/dri/card0` and `/dev/dri/renderD128` appear and open.
- **EGL through GBM in the guest is the real NVIDIA driver**, not a fallback.
  `eglinfo -B` in the guest reports, for both the *GBM* and *Surfaceless*
  platforms:

      EGL vendor string: NVIDIA
      OpenGL core profile renderer: NVIDIA GeForce RTX 4070/PCIe/SSE2
      OpenGL core profile version: 4.6.0 NVIDIA 595.84

  This matters for the glamor bug: the GBM->EGL import path that Xorg needs is
  present and NVIDIA-backed at the EGL level.  The `X11` and `Wayland` EGL
  platforms both report `eglInitialize failed`, which is expected with no
  server running.
- `libEGL warning: egl: failed to create dri2 screen` is printed before the
  NVIDIA vendor is selected.  That is **Mesa failing and glvnd falling through
  to NVIDIA**, not nvkvm failing.  It is cosmetic here, but it is the same
  Mesa-first probe that produced the earlier `AIGLX: Loaded and initialized
  nouveau` line, so Mesa *is* binding the node when something lets it.
- **udev is not at fault.**  After a manual `insmod`, the DRM nodes are created
  `root:root 0600` with no `by-path` links; a plain
  `udevadm trigger --subsystem-match=drm` fixes them to `root:video 0660` /
  `root:render 0660` and creates
  `/dev/dri/by-path/pci-0000:00:07.0-card`.  So the permissions symptom is
  "module inserted late, outside the boot uevent flow", not a missing rule and
  not an nvkvm bug.  A guest that loads the module at boot will not see it.
- The native-Xorg failure reproduces exactly as reported.  lightdm restart-loops
  ~22 times; `/var/log/lightdm/x-0.log` ends every attempt with:

      (EE) Fatal server error:
      (EE) failed to create screen resources

  which is the abort that follows the `Failed to create pixmap` from
  `drmmode_set_pixmap_bo()` -> `glamor_egl_create_textured_pixmap_from_gbm_bo()`.

## Not yet established (do not repeat as fact)

- Whether the GBM bo Xorg creates survives `gbm_bo_get_fd()` / dmabuf export
  through nvkvm.  **Untested.**  This is the actual next measurement for the
  glamor bug, and the eglinfo result above says the EGL side is healthy enough
  that the export/import round trip is the thing to instrument.
- Whether Mesa (not NVIDIA) is the EGL that Xorg's glamor ends up using.  The
  `nouveau` AIGLX line suggests it may be; not confirmed.
- Whether the host does the same thing.  **Not checked.**  Per project rule this
  must be ruled out before calling the glamor failure an nvkvm bug.

## Track A (cheap path to a desktop) — in progress, not working yet

Mint ships `/usr/share/wayland-sessions/cinnamon-wayland.desktop`
(`Exec=cinnamon-session-cinnamon --wayland`), so no extra packages are needed.
lightdm cannot be used to launch it — lightdm's *greeter* is itself an X server
and dies on the same glamor bug before any session starts.  So the display
manager has to be bypassed:

    systemctl disable lightdm; systemctl set-default multi-user.target
    # /etc/systemd/system/getty@tty1.service.d/autologin.conf
    #   ExecStart=-/sbin/agetty --autologin mint --noclear %I $TERM
    # ~/.bash_profile: exec cinnamon-session-cinnamon --wayland on XDG_VTNR=1

Current symptom: the tty1 login happens and logind creates a `seat0` session on
tty1, but `cinnamon-session` **spawns no muffin and produces no output** — the
process sits in `pipe_read` and the redirected log stays 0 bytes.  Note the
session is only `Active=yes` after a `chvt 1`; it comes up `Active=no` because
lightdm left the seat on another VT.  Whether the empty-log hang is a
consequence of the inactive seat, or independent, is **not yet determined** —
that is the first thing to check on resume.

Fallback if Cinnamon-on-Wayland stays stuck: install weston and run
`weston --backend=drm --xwayland`, which is the path this repo already drives at
60 fps on this box, then run the Mint desktop apps inside it.

## Reproducing the guest module build

The guest Makefile includes `../../src/common/nvkvm_proto.h`, so copying only
`src/guest/` out of the 9p mount does not build.  Copy the whole `src/` tree:

    cp -r /mnt/nvkvm/src ~/gbuild/src
    cd ~/gbuild/src/guest && make KDIR=/lib/modules/$(uname -r)/build
    sudo modprobe drm_shmem_helper && sudo insmod ./nvkvm-guest.ko
    sudo udevadm trigger --subsystem-match=drm
