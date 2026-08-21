# Stage the guest NVIDIA userspace

The guest runs **unmodified NVIDIA userspace**. That is the whole point — the
guest's own `libcuda` talks to what it believes is a real driver. It follows
that the guest's userspace must be version-matched to the **host** driver, since
that is the driver actually servicing the calls.

NVML and `libcuda` enforce this themselves: a mismatch produces
"Driver/library version mismatch" or `cuInit` → 803
(`scripts/stage_guest_libs.sh:8-12`). These libraries are not redistributable,
so they cannot ship in this repository — they have to be taken from whatever
driver is installed on your host.

Two steps: build a bundle on the host, stage it in the guest.

## On the host

```bash
bash scripts/make_host_bundle.sh
```

Reads the driver version from `nvidia-smi`, collects libraries from
`/usr/lib/x86_64-linux-gnu` (override with `$SYSLIB`) into
`host-libs-<version>/` at the repository root, and copies the vendor JSON
configs into `host-libs-<version>/config/`
(`scripts/make_host_bundle.sh:20-96`). Because the repository root is the 9p
share, the bundle appears in the guest at `/mnt/nvkvm/host-libs-<version>/`.

The script splits its list three ways.

**Required** (`scripts/make_host_bundle.sh:29`) — absence is a hard failure:

```
libcuda  libnvidia-ml  libnvidia-ptxjitcompiler  libnvidia-nvvm
```

**Optional** (`:43-47`) — absence degrades a capability:

```
libnvidia-glcore libnvidia-eglcore libnvidia-glsi libnvidia-tls
libnvidia-rtcore libnvidia-gpucomp libnvidia-allocator
libnvidia-encode libnvcuvid libnvidia-glvkspirv
libEGL_nvidia libGLX_nvidia libGLESv2_nvidia libGLESv1_CM_nvidia
libnvidia-cfg
```

The five GLVND vendor libraries on the last two lines were added after being
found missing: `stage_guest_libs.sh` stages every one of them by name, so a
bundle without them made that script exit 2 with five `MISSING` lines. The first
is `libEGL_nvidia`, which is what `/usr/share/glvnd/egl_vendor.d/10_nvidia.json`
points at — without it EGL has no NVIDIA vendor and every GL/EGL client silently
falls back to Mesa llvmpipe. `libGLX_nvidia` is the Vulkan ICD named by
`nvidia_icd.json`, so its absence silently demotes Vulkan to lavapipe
(`scripts/make_host_bundle.sh:34-42`).

**Not driver-versioned** (`:59`) — see below:

```
libnvidia-egl-wayland  libnvidia-egl-gbm
```

Plus `nvidia-smi` itself (`:86`) and the JSON configs from
`/usr/share/glvnd/egl_vendor.d`, `/usr/share/egl/egl_external_platform.d` and
`/usr/share/vulkan/icd.d` (`:92-96`).

## In the guest

```bash
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh
```

It takes a bundle as `$1`, or resolves one against the **host driver version**
the guest module reported in `dmesg` (`scripts/stage_guest_libs.sh:30-50`). If
that version cannot be read and more than one bundle is present it **refuses to
guess**, prints the candidates and exits 1. It only auto-picks when exactly one
bundle exists.

That is not incidental. It used to be `ls -d /mnt/nvkvm/host-libs-* | head -1`,
i.e. whichever bundle sorted first alphabetically — fine while a host has only
ever had one driver, silently wrong the moment it has had two. A box taken from
580.95.05 to 595.84 has both bundles on the share and `"580.95.05" < "595.84"`,
so the guest was staged with the *old* userspace against the *new* kernel
driver: `cuInit rc=803`, `CUDA_ERROR_SYSTEM_DRIVER_MISMATCH`, with nothing in
the staging output suggesting the wrong bundle had been picked
(`scripts/stage_guest_libs.sh:19-29`).

The version it stages is then derived from the bundle itself — `V` is parsed out
of the `libcuda.so.*` filename — so the guest is pinned to whatever the bundle
contains (`scripts/stage_guest_libs.sh:55`).

**Check the exit status.** It exits 2 with a list if anything was missing
(`:335-346`).

Two destination directories, and both must match
(`scripts/stage_guest_libs.sh:8-12`):

| dir | what resolves there |
|---|---|
| `/usr/lib/x86_64-linux-gnu` | NVML for `nvidia-smi`, the GL/EGL/Vulkan stack, the GBM backend |
| `/usr/local/nvidia-guest/lib` | `libcuda`, `libnvidia-allocator`, `libnvidia-ptxjitcompiler`, `libnvidia-nvvm` — ahead of the system dir on the loader path via `/etc/ld.so.conf.d/nvidia-guest.conf` |

It writes four vendor/loader configuration files that are as load-bearing as the
libraries (a fifth, `/etc/X11/xorg.conf`, is covered below):

| file | why |
|---|---|
| `/usr/share/glvnd/egl_vendor.d/10_nvidia.json` | without it `libGLX_nvidia` never enters its NVIDIA device-detection path and the Vulkan ICD bows out to llvmpipe (`:299-312`) |
| `/usr/share/vulkan/icd.d/nvidia_icd.json` | the Vulkan ICD on Linux *is* `libGLX_nvidia.so.0`; without the manifest the loader enumerates only lavapipe and a "Vulkan parity" run measures the CPU rasteriser (`:315-325`) |
| `/usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json` | see below |
| `/usr/share/egl/egl_external_platform.d/15_nvidia_gbm.json` | the EGL external platform for `EGL_PLATFORM_GBM` on an NVIDIA gbm device |

and one symlink that is easy to miss: `$SYS/gbm/nvidia-drm_gbm.so` →
`libnvidia-allocator.so.$V` (`:142-149`). Mesa's `libgbm` dlopens
`<drmdriver>_gbm.so` by the card's DRM driver name (`nvidia-drm`), and the
NVIDIA backend *is* `libnvidia-allocator`. Without it, `gbm_create_device()` on
card0 returns a Mesa "dri" device and the NVIDIA EGL platform never gets a
chance.

It also installs the guest's **X configuration**, which is deliberate scope
rather than an afterthought: `/etc/X11/xorg.conf`, copied from
[`data/xorg/nvkvm-xorg.conf`](../../data/xorg/nvkvm-xorg.conf) with the `BusID`
rewritten to the address nvkvm's device actually has in *your* guest
(`scripts/stage_guest_libs.sh:405-440`). A stock distro left alone picks one of
the two X paths that cannot work on the nvkvm head; this file names the third,
which does.

| | |
|---|---|
| it will not overwrite an `xorg.conf` you wrote | it recognises its own file by the `nvkvm-xorg.conf` marker on line 1, and otherwise prints what to merge |
| it must be `/etc/X11/xorg.conf`, not an `xorg.conf.d` drop-in | with a drop-in the NVIDIA package's `OutputClass` still matches, Xorg tries the DDX first, and the server exits rather than falling through |
| to skip it entirely | `NVKVM_STAGE_XORG=0 sudo -E bash .../stage_guest_libs.sh` |

Why that file is the working path, and what is genuinely unavailable without
NVIDIA's own X driver:
[the guest's own Xorg session](run.md#the-guests-own-xorg-session-a-stock-distro-desktop).

Finally it blacklists `nouveau` (`:328-333`) — the emulated `nvkvm-gpu` PCI
device has no BARs, so nouveau auto-binding to it can only fail and make noise.
Takes effect after a guest reboot.

## Why the failures here are silent

This is the single most important thing to understand about staging, and it is
why both scripts are unusually chatty.

Every failure mode in this area produces a system that *looks* correct.

**A missing library used to produce no output at all.** Every copy was
`cp -f ... 2>/dev/null`, so an absent library was invisible and the script still
printed "done" (`scripts/stage_guest_libs.sh:63-67`):

> this is how the whole GL/Vulkan stack went missing while the script still
> printed "done"

Every copy now routes through a `stage()` helper that prints `STAGED` or
`MISSING` and remembers the misses for the summary (`:70-81`).

**A hardcoded version deleted the libraries it had just staged.** The
stale-sweep was `rm -f $CUDADIR/lib*.so.575.51.03`, with the version literal. On
a host actually running 575.51.03 it removed the three files the lines above had
just copied, leaving `libcuda.so.1` and friends as *dangling symlinks* in the
directory `ld.so` searches first. Applications kept working by accident —
`ld.so` skips a dangling link and falls through to the system directory — but
`libnvidia-ptxjitcompiler` is staged **only** in that directory, so every
`cuModuleLoadData(PTX)` died with `CUDA_ERROR_JIT_COMPILER_NOT_FOUND` (221),
which reads as "kernels are broken", not "a library is missing"
(`scripts/stage_guest_libs.sh:207-221`).

Both sweeps are now version-aware and skip symlinks:

```sh
for stale in "$CUDADIR"/*.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue      # SONAME links are ours, not stale versions
    case "$stale" in *".so.$V") continue ;; esac
    sudo rm -f "$stale"
done
```
— `scripts/stage_guest_libs.sh:176-181`

**A missing loader path was documented but never created.** The script's own
header claimed CUDA apps resolve `libcuda` from `/usr/local/nvidia-guest/lib`
"via nvidia-guest.conf, AHEAD of the system dir", but nothing created that file,
so the directory was off the `ld.so` path entirely and every app silently
resolved from the system directory instead (`scripts/stage_guest_libs.sh:235-242`).

**A missing JIT dependency moves the failure one layer down.**
`libnvidia-ptxjitcompiler` dlopens `libnvidia-nvvm.so.4`. Staging the JIT
compiler without nvvm leaves `cuInit` and plain memcpy passing, and only PTX
module loads failing (`scripts/stage_guest_libs.sh:201-206`). Found during
Ada/Turing bring-up.

**A missing external platform produces a correctly-sized black window.** This
is the sharpest example. `scripts/stage_guest_libs.sh:153-163`:

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

## The version-matching trap: not everything is driver-versioned

Almost every NVIDIA library the guest needs carries the driver version in its
filename, so the natural staging loop is a glob on `$lib.so.$V`. Two of them do
not, and the mismatch is completely silent.

```
libcuda.so.575.51.03              ← driver version
libnvidia-egl-wayland.so.1.1.19   ← upstream egl-wayland version
libnvidia-egl-gbm.so.1.1.2        ← upstream egl-gbm version
```

`libnvidia-egl-wayland` and `libnvidia-egl-gbm` come from separate upstream
projects — [NVIDIA/egl-wayland](https://github.com/NVIDIA/egl-wayland) and
[NVIDIA/egl-gbm](https://github.com/NVIDIA/egl-gbm) — that are merely packaged
alongside the driver. They carry their own SONAME version. Globbing
`$lib.so.$V` therefore never matches them, matches nothing, and — before the
`stage()` helper existed — said nothing.

`make_host_bundle.sh` handles them separately, by resolving the real file behind
the `.so.1` SONAME link (`scripts/make_host_bundle.sh:62-71`):

```sh
for lib in $SONAME_VERSIONED; do
    real=$(readlink -f "$SYSLIB/$lib.so.1" 2>/dev/null || true)
    if [ -n "$real" ] && [ -f "$real" ]; then
        cp -f "$real" "$DEST/"; n=$((n+1))
        echo "make_host_bundle: $lib -> $(basename "$real")  (not driver-versioned)"
    else
        echo "make_host_bundle: optional $lib not installed -- Wayland/GBM clients will fall back to software" >&2
    fi
done
```

and `stage_guest_libs.sh` globs `$lib.so.*` rather than `$lib.so.$V` for exactly
these two, then creates the `.so.1` link and writes the matching JSON config
(`scripts/stage_guest_libs.sh:134-155`).

These are the EGL **external platform** libraries — a different mechanism from
the GLVND vendor config. The failure signature is worth memorising, because it
is the reason this section exists:

> without them a Wayland client falls back to llvmpipe while the EGL device
> platform still reports NVIDIA -- i.e. software rendering that looks correct in
> a screenshot.
>
> — `scripts/make_host_bundle.sh:44-46`

Everything about that failure is legible except the part that matters. `eglinfo`
shows NVIDIA. The device platform binds NVIDIA. The compositor is
GPU-accelerated. A screenshot looks right. Only the *client's* `GL_RENDERER`
says `llvmpipe`.

**So: always pair a capture with the client's renderer string.** Staging the
external platform is necessary for GPU-accelerated Wayland clients; it has not
by itself been shown sufficient. See
[Known limitations](../internal/known-limitations.md#gl-clients-under-wayland-presented-nothing--two-root-causes-fixed-2026-08-19).

## What each library is for

| library | staged to | without it |
|---|---|---|
| `libcuda` | both | nothing CUDA works |
| `libnvidia-ml` | system | `nvidia-smi` cannot enumerate |
| `libnvidia-ptxjitcompiler` | cuda dir | `cuModuleLoadData(PTX)` → 221 |
| `libnvidia-nvvm` | cuda dir (as `.so.1` **and** `.so.4`) | same, one layer down |
| `libnvidia-allocator` | both + the GBM backend symlink | GBM falls back to Mesa |
| `libnvidia-encode`, `libnvcuvid` | system | "Cannot load libnvidia-encode.so.1" (both are needed; encode depends on cuvid) |
| `libnvidia-{glcore,eglcore,glsi,gpucomp,rtcore,tls}` | system | GL/EGL init fails |
| `libnvidia-glvkspirv` | system | Vulkan shader compilation |
| `libEGL_nvidia`, `libGLX_nvidia`, `libGLESv2_nvidia`, `libGLESv1_CM_nvidia` | system | no EGL vendor, no Vulkan ICD |
| `libnvidia-cfg` | system | device configuration queries |
| `libnvidia-egl-wayland`, `libnvidia-egl-gbm` | system, own version | Wayland/GBM clients fall back to software, silently |

Staging `libnvidia-encode` and `libnvcuvid` makes the encoder *loadable*. That
is not the same as making it work: an `h264_nvenc` hang was recorded on driver
575.51.03 and then **did not reproduce** on a re-test on the same driver, which
is neither a fix nor a retraction. Read
[Known limitations](../internal/known-limitations.md#nvenc--the-5755103-hang-did-not-reproduce-2026-08-20)
before relying on hardware encode in a guest.
