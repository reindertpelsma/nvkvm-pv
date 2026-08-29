# KWin could not drive the nvkvm head — SOLVED

Measured 2026-08-29 on the laptop (RTX 3050 Ti Mobile, host driver 580.173.02),
on a SteamOS A/B slot written by an OTA update.

## Root cause

**A truncated NVIDIA userspace install, never repaired.** Not a SteamOS
regression, not a KWin bug, not our DRM driver.

The `.run` extraction was OOM-killed part-way through. The payload installs
`libnvidia-glcore` early, so the provisioning script's version probe still
reported the correct `580.173.02`, logged *"NVIDIA userspace matches host"*, and
skipped the install — on that boot and every boot after it. The slot sat at 35
of 50 nvidia libraries permanently.

What was missing was the tail of the file list, and it was display-fatal:

```
/usr/lib/gbm/nvidia-drm_gbm.so                            <- absent
/usr/share/egl/egl_external_platform.d/15_nvidia_gbm.json <- absent
```

gbm selects its backend by DRM driver name. Without that one symlink KWin got
Mesa's `dri_gbm.so` on our NVIDIA-identity device, and everything downstream
followed from that single substitution.

## Symptom chain

| layer | what was observed |
|---|---|
| gbm | falls back to `dri_gbm.so` (Mesa) — no NVIDIA backend present |
| EGL | NVIDIA cannot serve `EGL_PLATFORM_GBM`; `couldn't find dev node for drm device` |
| KWin atomic | `Checking test buffer failed!`, `Applying output configuration failed!` |
| DRM | `status=connected`, `enabled=disabled`, zero flips |
| desktop | every KDE component on a `PlaceholderOutput`, broker window frozen |
| KWin legacy (`KWIN_DRM_NO_AMS=1`) | output enables, 655 frames flow, then SIGSEGV in `libgallium` every ~2s |

The legacy-path crash was the clearest tell: the backtrace landed in **Mesa**,
on a device only NVIDIA can drive.

## The fix

`nvkvm-steamos` now checks for the *files the display path needs* rather than
the version, and repairs a truncated tree instead of skipping it
(`nvidia_userspace_complete`, guarded by `tests/nvidia_userspace_completeness_test.sh`).
The sentinel list is deliberately profile-aware — CUDA, OpenCL and firmware are
discarded on purpose by `--profile steamos`.

Verified end to end on the affected slot: the check named both missing files,
the repair install ran to `rc=0` and restored 50/50 libraries, and the desktop
came up on the clean default path — output enabled, zero KWin fallback
messages, flips carrying the native block-linear modifier
`0x0300000000606014`, stable session under load 0.57.

## Ruled out along the way, each by measurement

Recorded so nobody re-runs them:

| hypothesis | evidence against |
|---|---|
| BAR1 VA leak | KWin failed before allocating anything |
| DRM layout / stray `controlD64` | the PC guest has an identical layout and renders 33,287 frames |
| KWin version | 6.4.3 on both the working and failing guest |
| GL broken | `gl_probe` passes — draws to an FBO and reads back correct pixels |
| missing NVML/CUDA | KWin needs neither; `--profile steamos` trims them by design |
| udev / logind seat | `TAGS=:uaccess:seat:master-of-seat:`, `[MASTER] drm:card0` on seat0 |
| libdrm enumeration | all PCI sysfs attrs readable, full 256-byte config space, identical libdrm in both slots |
| no render node | `renderD128` exists and the driver sets `DRIVER_RENDER` |
| missing GLVND EGL vendor config | both slots have `10_nvidia.json` and `libEGL_nvidia.so.0` |
| we don't advertise the right modifier | block-linear **is** advertised; only LINEAR is withheld, and it is still accepted at AddFB |
| second GPU confusion | only one DRM device exists; pinning `KWIN_DRM_DEVICES` changed nothing |
| SteamOS 20260716.1 regression | **wrong** — the image is fine; the slot written under it was incomplete |

Files here: `context.txt` (system state), `kwin-backtrace.txt` (the Mesa SIGSEGV).
