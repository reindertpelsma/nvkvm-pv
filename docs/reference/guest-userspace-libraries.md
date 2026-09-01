# Guest userspace libraries

The guest runs stock NVIDIA userspace against the host's driver, so it must be
version-matched to the **host** driver. This page is the reference list; the
procedure is in
[Stage the guest NVIDIA userspace](../howto/stage-guest-libraries.md).

Two scripts implement it: `scripts/make_host_bundle.sh` collects from the host,
`scripts/stage_guest_libs.sh` installs inside the guest.

## Version matching

NVML and `libcuda` enforce the match themselves. A mismatch gives
"Driver/library version mismatch" or `cuInit` → 803
(`scripts/stage_guest_libs.sh:8-12`).

The staged version is derived from the bundle, not from a constant: `V` is
parsed out of the bundle's `libcuda.so.*` filename
(`scripts/stage_guest_libs.sh:55`). Every subsequent name is `<lib>.so.$V`.

**Except two libraries, which are not driver-versioned at all.** See
[below](#not-driver-versioned).

## Where things go

| directory | contents | why |
|---|---|---|
| `/usr/lib/x86_64-linux-gnu` | NVML, the GL/EGL/Vulkan stack, video engines, the GBM backend | where NVML and the loaders resolve |
| `/usr/local/nvidia-guest/lib` | `libcuda`, `libnvidia-allocator`, `libnvidia-ptxjitcompiler`, `libnvidia-nvvm` | ahead of the system dir via `/etc/ld.so.conf.d/nvidia-guest.conf` |
| `/usr/lib/x86_64-linux-gnu/gbm/` | `nvidia-drm_gbm.so` → `../libnvidia-allocator.so.$V` | Mesa's `libgbm` dlopens `<drmdriver>_gbm.so` by DRM driver name |
| `/usr/local/bin/nvidia-smi` | the host's binary | must match the driver |

Both library directories must match the host driver
(`scripts/stage_guest_libs.sh:8-12`).

## The list

### Required

Absence is a hard failure; `make_host_bundle.sh` exits non-zero
(`scripts/make_host_bundle.sh:28`, `:86-90`).

| library | staged to | without it |
|---|---|---|
| `libcuda` | both | nothing CUDA works |
| `libnvidia-ml` | system (+ `.so.1`) | `nvidia-smi` cannot enumerate |
| `libnvidia-ptxjitcompiler` | cuda dir (+ `.so.1`) | `cuModuleLoadData(PTX)` → `CUDA_ERROR_JIT_COMPILER_NOT_FOUND` (221) |
| `libnvidia-nvvm` | cuda dir, linked as **both** `.so.1` and `.so.4` | same failure, one layer down |

`libnvidia-nvvm` needs both SONAME links because `libnvidia-ptxjitcompiler`
dlopens `libnvidia-nvvm.so.4` on driver ≥ 12.0
(`scripts/stage_guest_libs.sh:161-166`). Staging the JIT compiler without nvvm
leaves `cuInit` and plain memcpy passing and only PTX module loads failing —
"which reads as 'kernels are broken', not as 'a library is missing'". Found
during Ada/Turing bring-up.

### Optional — absence degrades a capability

`scripts/make_host_bundle.sh:43-47`.

| library | staged to | covers |
|---|---|---|
| `libnvidia-glcore` | system | GL |
| `libnvidia-eglcore` | system | EGL |
| `libnvidia-glsi` | system | GL system interface |
| `libnvidia-gpucomp` | system | shader compilation |
| `libnvidia-rtcore` | system | ray tracing — staged, but see below |

> **Ray tracing does not currently work in the guest.** `libnvidia-rtcore` is
> staged because the driver expects it, not because the path functions: seven
> Vulkan device extensions — including `VK_KHR_acceleration_structure`,
> `VK_KHR_ray_query` and `VK_KHR_ray_tracing_pipeline` — make `vkCreateDevice`
> return `VK_ERROR_INITIALIZATION_FAILED`, while the identical probe succeeds
> on the host. See
> [known limitations](../internal/known-limitations.md#seven-device-extensions-fail-vkcreatedevice--ray-tracing-does-not-work-2026-09-01).
| `libnvidia-tls` | system | thread-local storage helper |
| `libnvidia-glvkspirv` | system | Vulkan SPIR-V |
| `libnvidia-allocator` | both (+ the GBM backend symlink) | GBM buffer allocation |
| `libnvidia-encode` | system (+ `.so.1`, `.so`) | NVENC |
| `libnvcuvid` | system (+ `.so.1`, `.so`) | NVDEC, and NVENC depends on it |

The GLVND vendor libraries are staged by `stage_guest_libs.sh` from whatever the
bundle contains (`scripts/stage_guest_libs.sh:123-128`):

| library | SONAME link |
|---|---|
| `libEGL_nvidia` | `.so.0` |
| `libGLX_nvidia` | `.so.0` — **this is also the Vulkan ICD** |
| `libGLESv2_nvidia` | `.so.2` |
| `libGLESv1_CM_nvidia` | `.so.1` |
| `libnvidia-cfg` | `.so.1` |

`libnvidia-encode` depends on `libnvcuvid`, so both must be present and
version-matched or ffmpeg reports "Cannot load libnvidia-encode.so.1"
(`scripts/stage_guest_libs.sh:101-104`). Staging them makes the encoder loadable;
it does not make it work — see
[Known limitations](../internal/known-limitations.md#nvenc--the-5755103-hang-did-not-reproduce-2026-08-20).

### Not driver-versioned

`scripts/make_host_bundle.sh:49-54`:

```
libcuda.so.575.51.03              <- driver version
libnvidia-egl-wayland.so.1.1.19   <- upstream egl-wayland version
libnvidia-egl-gbm.so.1.1.2        <- upstream egl-gbm version
```

`libnvidia-egl-wayland` and `libnvidia-egl-gbm` come from separate upstream
projects — [NVIDIA/egl-wayland](https://github.com/NVIDIA/egl-wayland) and
[NVIDIA/egl-gbm](https://github.com/NVIDIA/egl-gbm) — that are merely packaged
alongside the driver, and carry their own SONAME version.

A glob on `$lib.so.$V` therefore never matches them and skips them **in
silence**. `make_host_bundle.sh` collects them by resolving the real file behind
the `.so.1` SONAME link instead (`scripts/make_host_bundle.sh:74-81`), and
`stage_guest_libs.sh` globs `$lib.so.*` for these two specifically
(`scripts/stage_guest_libs.sh:134-155`).

These are the EGL **external platform** libraries — a different mechanism from
the GLVND vendor config. The failure mode is the one worth memorising
(`scripts/make_host_bundle.sh:56-58`):

> without them a Wayland client falls back to llvmpipe while the EGL device
> platform still reports NVIDIA -- i.e. software rendering that looks correct in
> a screenshot.

## Configuration files

As load-bearing as the libraries. All written by `stage_guest_libs.sh`;
`make_host_bundle.sh` also collects the host's own copies into
`host-libs-$V/config/` for reference (`scripts/make_host_bundle.sh:88-96`).

| file | contents | without it |
|---|---|---|
| `/usr/share/glvnd/egl_vendor.d/10_nvidia.json` | `library_path: libEGL_nvidia.so.0` | `libGLX_nvidia` never enters its NVIDIA device-detection path (it only sees Mesa) and the Vulkan ICD bows out to llvmpipe (`:206-209`) |
| `/usr/share/vulkan/icd.d/nvidia_icd.json` | `library_path: libGLX_nvidia.so.0`, `api_version: 1.4.303` | the loader enumerates only lavapipe, so `vulkaninfo` reports llvmpipe and a "Vulkan parity" run measures the CPU rasteriser (`:220-223`) |
| `/usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json` | `library_path: libnvidia-egl-wayland.so.1` | see below |
| `/usr/share/egl/egl_external_platform.d/15_nvidia_gbm.json` | `library_path: libnvidia-egl-gbm.so.1` | `EGL_PLATFORM_GBM` on an NVIDIA gbm device is unhandled |
| `/etc/ld.so.conf.d/nvidia-guest.conf` | `/usr/local/nvidia-guest/lib` | the CUDA dir is off the loader path entirely and everything resolves from the system dir instead (`:195-198`) |
| `/etc/modprobe.d/blacklist-nvkvm-nouveau.conf` | `blacklist nouveau` | nouveau auto-binds the BAR-less `nvkvm-gpu` identity device and can only fail/noise (`:235-241`) — needs a guest reboot |

The Wayland one, verbatim (`scripts/stage_guest_libs.sh:153-163`):

> A Wayland GL client (es2gears, glmark2, any toolkit) calls
> `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND)`; with no
> `10_nvidia_wayland.json` the NVIDIA external platform is never loaded, so
> glvnd hands the client to MESA's libEGL, which tries DRI2 on the nvkvm PCI
> device and fails:
> ```
> libEGL warning: pci id for fd 4: 10de:2504, driver (null)
> libEGL warning: egl: failed to create dri2 screen
> ```
> The client still connects and maps a surface, so the compositor composites a
> correctly-sized window that is entirely BLACK — which looks like a working
> desktop in a screenshot unless you check the client actually drew something.

## The GBM backend symlink

Three pieces are needed for GPU-accelerated GL/EGL on the virtual KMS head, and
if any is missing the NVIDIA path is silently skipped and Mesa falls back to
llvmpipe (`scripts/stage_guest_libs.sh:128-133`):

1. **The GBM backend.** Mesa's `libgbm` dlopens `<drmdriver>_gbm.so` from the
   GBM backends directory, by the card's DRM driver name — which is
   `nvidia-drm`. The NVIDIA backend *is* `libnvidia-allocator`; the host ships
   `nvidia-drm_gbm.so` as a symlink to it. Without this,
   `gbm_create_device()` on card0 returns a Mesa "dri" device and the NVIDIA EGL
   platform never gets a chance. The script's comment calls this "the whole
   'EGL fails to init on the head' wall".
2. **`libnvidia-egl-gbm.so.1`** plus its `15_nvidia_gbm.json`.
3. **`libnvidia-allocator` in the system directory**, so the backend symlink
   resolves for non-CUDA GL applications — compositors do not add
   `/usr/local/nvidia-guest/lib` to their path.

## Verifying

Do not trust a screenshot. Check renderer strings.

```bash
nvidia-smi                                    # driver version + GPU model
vulkaninfo | grep deviceName                  # must not say llvmpipe
eglinfo                                       # check EACH platform, not just device
glxinfo -B 2>/dev/null | grep -i renderer
```

For a Wayland client, the client's own `GL_RENDERER` is the only thing that
settles it — the compositor can be GPU-accelerated while its clients are not,
and that is the currently observed state
(`tests/perf/realapp_matrix.md`, "Display / desktop — re-validated"). See
[Known limitations](../internal/known-limitations.md#gl-clients-under-wayland-presented-nothing--two-root-causes-fixed-2026-08-19).

`stage_guest_libs.sh` reports what it staged and exits **2** if anything was
missing from the bundle (`scripts/stage_guest_libs.sh:335-346`). Treat a
nonzero exit as "do not trust a subsequent parity run".
