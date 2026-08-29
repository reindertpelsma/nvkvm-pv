# GET_DEV_INFO returns EBADF, and the guest silently falls back to llvmpipe

**Status: OPEN. Cause not found. Observed once, cleared by restarting QEMU,
did not reproduce across five subsequent boots.** Recorded because the failure
mode is silent and the next person to hit it will spend a day on the wrong layer
— which is exactly what happened here.

## What it looks like

KWin comes up on a `PlaceholderOutput`, the connector never reaches `enabled`,
and nothing ever flips. The chain, from the bottom:

```
ioctl(3, _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0x43, 0x24), …) = -1 EBADF
openat(…, "/usr/lib/gbm/dri_gbm.so", …)
```

`'d'/0x43/36B` is `DRM_IOCTL_NVIDIA_GET_DEV_INFO`. gbm's loader chatter shows the
NVIDIA backend being tried and rejecting the device:

```
calling init: /usr/lib/gbm/nvidia-drm_gbm.so
calling fini: /usr/lib/gbm/nvidia-drm_gbm.so [0]     <- rejected
calling init: /usr/lib/gbm/dri_gbm.so                <- Mesa took it
```

From there everything downstream is a *consequence*, and every one of them
points somewhere else:

| symptom | where it points | actually |
|---|---|---|
| `EGL_VENDOR "Mesa Project"`, `EGL_MESA_device_software` | Mesa/EGL config | llvmpipe, because gbm fell back |
| `libEGL warning: … driver (null)` / `failed to create dri2 screen` | broken NVIDIA userspace | consequence |
| KWin `couldn't find dev node for drm device`, `Failed to open drm node: ""` | KMS / our virtual pipe | consequence |
| KWin `Checking test buffer failed!` → PlaceholderOutput | modifiers, atomic modeset | consequence |

**The tell is `EGL_MESA_device_software`.** A software renderer on a device only
NVIDIA can drive means vendor *selection* already failed. Check that before
touching the renderer, the modifier list, or KMS.

## What is known about the mechanism

- A kretprobe showed `nvkvm_virtio_ioctl_on_isolate` returning `-9` (EBADF) with
  a valid ctx.
- `strace` of the isolate stub showed **zero ioctl syscalls** for it — so the
  request died before reaching the host: a `handle_lookup()` miss at
  `src/stub/nvkvm_stub.c:1182`.
- The host answers this ioctl fine (`primary_index=1, supports_alloc=1,
  generic_page_kind=6`), so it is not a host or ABI problem.
- It cleared on restarting the **vmm container (the QEMU process)** — not on a
  guest reboot. That places the bad state in QEMU's or the stub's process
  lifetime, not in the guest.
- It did not reproduce across five later boots (`uid+chroot` ×3, `auto` ×1,
  guest-only reboot ×1).

## Leading theory, unproven

Handle-table churn from the ~140-cycle gamescope crash-loop that immediately
preceded it (see the BAR1 leak note). That loop created and tore down RM clients
as fast as sddm could respawn them. If a teardown can leave the stub's handle
table inconsistent, this is what it would look like: a handle the guest still
believes in, absent on the stub side, failing lookup.

## What would settle it

- Reproduce deliberately: drive many rapid client create/teardown cycles against
  one long-lived QEMU, then issue GET_DEV_INFO.
- Instrument the miss: log the handle id and the table's contents at
  `nvkvm_stub.c:1182` on failure, so the next occurrence names the handle rather
  than only the errno.
- Consider whether GET_DEV_INFO should fail *loudly* rather than as EBADF. A
  silent demotion to software rendering is the worst property of this bug: every
  visible symptom accuses a different subsystem.
