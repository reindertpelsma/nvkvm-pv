# KWin cannot drive the nvkvm head on SteamOS 20260716.1 — OPEN

Measured 2026-08-29 on the laptop (RTX 3050 Ti Mobile, host driver 580.173.02),
guest booted from an A/B slot written by a SteamOS OTA.

## Symptom

The broker window is frozen. The guest is healthy and the desktop session runs,
but nothing is ever presented.

```
status  = connected
enabled = disabled        <- no modeset ever completed
flips   = 0
```

Every KDE component logs `qt.qpa.wayland: There are no outputs - creating
placeholder screen`, and KWin says:

```
kwin_core:         Failed to open drm node: ""
kwin_scene_opengl: couldn't find dev node for drm device
kwin_wayland_drm:  Checking test buffer failed!
kwin_core:         Applying output configuration failed!
```

## Both KWin paths fail, differently

| path | result |
|---|---|
| atomic (default) | test buffer rejected, output never enabled, ZERO frames |
| legacy (`KWIN_DRM_NO_AMS=1`) | output ENABLES and frames flow (655 sent) — then KWin SIGSEGVs every ~2s |

The legacy-path crash is **inside Mesa**, not NVIDIA:

```
#1 libgallium-25.3.0.so + 0x82323c
#2 libgallium-25.3.0.so + 0x82336b
...
```

SteamOS ships a radeonsi Mesa. So KWin is driving our NVIDIA-identity DRM device
with Mesa's GL stack. The likely chain: device-node resolution fails, KWin
cannot take the NVIDIA EGL device path, falls back to GBM+Mesa, and Mesa
crashes on a device it cannot drive.

That makes `couldn't find dev node for drm device` the primary fault; the
segfault is downstream of it.

## Ruled out, each by measurement

| hypothesis | evidence against |
|---|---|
| BAR1 VA leak (the usual `enabled=disabled` cause) | no allocation failure; KWin fails BEFORE allocating |
| DRM layout / the stray `controlD64` | the PC guest has an IDENTICAL layout and renders 33,287 frames |
| KWin version | 6.4.3 on both the working and failing guest |
| GL/EGL broken | `gl_probe` PASSES — draws a triangle to an FBO, reads back correct pixels |
| missing NVML/CUDA | KWin needs neither; `--profile steamos` trims them by design |
| udev / PCI attributes | all correct: `0x10de:0x25a2`, `device` -> the PCI dev, `subsystem` -> `/sys/bus/pci` |
| missing GLVND EGL vendor config | both slots have `10_nvidia.json` AND `libEGL_nvidia.so.0` |

## The one difference that remains

|  | works | fails |
|---|---|---|
| SteamOS | 20260707.10 | 20260716.1 |
| kernel | 6.16.12-valve24.4 | 6.16.12-valve24.5 |

Same nvkvm module source, same host, same KWin, same DRM layout.

## Where to resume

`coredumpctl info /usr/bin/kwin_wayland` on a guest that has run the legacy path
gives the Mesa frame; 407 dumps were on disk when this was captured. The useful
next step is finding why `drmGetDeviceNameFromFd2`-style resolution returns
empty for our device on valve24.5 when it works on valve24.4 — that is what
pushes KWin onto the Mesa path in the first place.

Workaround for a user, with the crash caveat: none. `KWIN_DRM_NO_AMS=1` trades a
black screen for a crash loop (and pins the CPU via drkonqi + systemd-coredump).

Files here: `context.txt` (system state), `kwin-backtrace.txt` (the SIGSEGV).
