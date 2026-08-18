#!/bin/bash
# stage_guest_libs.sh — version-match the guest's NVIDIA userspace to the host
# driver.  Runs INSIDE the guest (over 9p at /mnt/nvkvm).  Kept as a standalone
# script (not inline in run_remote_test.sh) so shell variables expand on the
# guest, not the local shell across the nested ssh host "ssh guest '...'" hops.
#
# Two dirs matter and BOTH must match the host driver or NVML/libcuda refuse to
# init ("Driver/library version mismatch" / cuInit 803):
#   - /usr/lib/x86_64-linux-gnu      : where NVML/nvidia-smi resolve libnvidia-ml
#   - /usr/local/nvidia-guest/lib    : where CUDA apps resolve libcuda (on the
#                                      ld.so path via nvidia-guest.conf, AHEAD of
#                                      the system dir) + allocator + ptxjit
#
# The bundle dir defaults to the host's current driver bundle; override with $1.
set -u
# Default bundle path follows the HOST driver version rather than a hardcoded
# 580, so the script works on any host.
#
# This used to be `ls -d /mnt/nvkvm/host-libs-* | head -1`, i.e. whichever
# bundle sorted first ALPHABETICALLY. That is fine while a host has only ever
# had one driver, and silently wrong the moment it has had two: a box that ran
# 580.95.05 and was then moved to 595.84 has both bundles on the 9p share, and
# "580.95.05" < "595.84", so the guest was staged with the OLD userspace
# against the NEW kernel driver and libcuda refused to initialise --
# cuInit rc=803, CUDA_ERROR_SYSTEM_DRIVER_MISMATCH -- with nothing in the
# staging output suggesting the wrong bundle had been chosen.
#
# Select by the version the guest module reports for the HOST driver, and if
# that cannot be read, refuse to guess between multiple candidates.
if [ -n "${1:-}" ]; then
    GFXBUNDLE="$1"
else
    HOSTV="$( { dmesg 2>/dev/null || sudo -n dmesg 2>/dev/null; } \
              | sed -n 's/.*nvkvm: host NVIDIA driver \([0-9][0-9.]*\).*/\1/p' | tail -1 )"
    if [ -n "$HOSTV" ] && [ -d "/mnt/nvkvm/host-libs-$HOSTV" ]; then
        GFXBUNDLE="/mnt/nvkvm/host-libs-$HOSTV"
        echo "stage_guest_libs: host driver is $HOSTV -> $GFXBUNDLE"
    else
        N=$(ls -d /mnt/nvkvm/host-libs-* 2>/dev/null | wc -l)
        if [ "$N" -eq 1 ]; then
            GFXBUNDLE="$(ls -d /mnt/nvkvm/host-libs-* 2>/dev/null)"
        else
            echo "stage_guest_libs: cannot choose between $N bundles and the host" >&2
            echo "  driver version could not be read from dmesg${HOSTV:+ (got $HOSTV, no matching bundle)}." >&2
            echo "  Pass one explicitly:" >&2
            ls -d /mnt/nvkvm/host-libs-* 2>/dev/null | sed 's/^/    /' >&2
            exit 1
        fi
    fi
fi
SYS=/usr/lib/x86_64-linux-gnu
CUDADIR=/usr/local/nvidia-guest/lib
sudo mkdir -p "$CUDADIR"   # ln -s below fails if it does not exist

V=$(ls "$GFXBUNDLE"/libcuda.so.* 2>/dev/null | sed "s#.*/libcuda.so.##" | head -1)
if [ -z "$V" ]; then
    echo "stage_guest_libs: no libcuda in $GFXBUNDLE" >&2
    exit 1
fi
echo "stage_guest_libs: staging $V from $GFXBUNDLE"

# ── stage helper ─────────────────────────────────────────────────────────────
# Every copy in this script used to be `cp -f ... 2>/dev/null`, so a lib that
# was absent from the bundle produced NO output at all and the guest silently
# came up without it (this is how the whole GL/Vulkan stack went missing while
# the script still printed "done").  Route every copy through stage(): it says
# STAGED or MISSING, and MISSING is remembered for the summary at the end.
MISSING_LIBS=""
STAGED_N=0
stage(){ # $1 basename-in-bundle  $2 destdir  [$3.. extra symlink names]
    local f="$1" dest="$2"; shift 2
    if [ ! -f "$GFXBUNDLE/$f" ]; then
        echo "stage_guest_libs: MISSING from bundle: $f  (-> $dest)" >&2
        MISSING_LIBS="$MISSING_LIBS $f"
        return 1
    fi
    sudo cp -f "$GFXBUNDLE/$f" "$dest/" || { echo "stage_guest_libs: COPY FAILED: $f" >&2; return 1; }
    local l; for l in "$@"; do sudo ln -sf "$f" "$dest/$l"; done
    STAGED_N=$((STAGED_N+1))
    return 0
}

# -- system dir: NVML for nvidia-smi (+ libcuda for completeness) --
stage "libnvidia-ml.so.$V" "$SYS" "libnvidia-ml.so.1"
stage "libcuda.so.$V"      "$SYS" "libcuda.so.1" "libcuda.so"
# Remove a STALE differently-versioned copy, never the one just staged.
# This line used to hardcode 575.51.03; on a host actually running 575.51.03 it
# deleted the freshly-copied libs, and the cp above is 2>/dev/null so nothing
# said so -- nvidia-smi then reported "couldn't find libnvidia-ml.so".
# NB: the glob also matches the SONAME symlink (libcuda.so.1), so skip symlinks
# — only differently-versioned REGULAR files are stale.  (It happened to survive
# before only because ldconfig re-creates SONAME links from the staged files.)
for stale in "$SYS"/libcuda.so.[0-9]* "$SYS"/libnvidia-ml.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue
    case "$stale" in *".so.$V") continue ;; esac
    sudo rm -f "$stale"
done

# -- video engines: NVENC encode + NVDEC/cuvid.  libnvidia-encode.so depends on
# libnvcuvid.so, so BOTH must be present + version-matched or ffmpeg/NVENC says
# "Cannot load libnvidia-encode.so.1".  (NVENC session InitializeEncoder beyond
# this is a separate deeper forwarder gap — tracked as its own task.) --
for vlib in libnvidia-encode libnvcuvid; do
    stage "$vlib.so.$V" "$SYS" "$vlib.so.1" "$vlib.so"
done

# -- GL / EGL / Vulkan userspace.  The bundle already carried the *core* libs
# (glcore/eglcore/glsi/gpucomp/rtcore/tls/nvvm) but NOTHING in this script ever
# copied them into the guest, and the GLVND *vendor* libs (libEGL_nvidia,
# libGLX_nvidia — which is also the Vulkan ICD — libGLESv2_nvidia) plus
# libnvidia-glvkspirv were not in the bundle at all.  Net effect: the guest had
# the 10_nvidia.json EGL vendor config written below pointing at a
# libEGL_nvidia.so.0 that did not exist, no Vulkan ICD, and every GL/Vulkan app
# either failed to init or silently fell back to Mesa llvmpipe (software) —
# while this script still reported success.  Stage the whole closure.
for core in libnvidia-glcore libnvidia-eglcore libnvidia-glsi libnvidia-gpucomp \
            libnvidia-rtcore libnvidia-tls libnvidia-nvvm; do
    stage "$core.so.$V" "$SYS"          # loaded by SONAME-less direct name; no symlink
done
stage "libnvidia-glvkspirv.so.$V" "$SYS"
stage "libEGL_nvidia.so.$V"       "$SYS" "libEGL_nvidia.so.0"
stage "libGLX_nvidia.so.$V"       "$SYS" "libGLX_nvidia.so.0"
stage "libGLESv2_nvidia.so.$V"    "$SYS" "libGLESv2_nvidia.so.2"
stage "libGLESv1_CM_nvidia.so.$V" "$SYS" "libGLESv1_CM_nvidia.so.1"
stage "libnvidia-cfg.so.$V"       "$SYS" "libnvidia-cfg.so.1"
# OpenCL vendor library.  Without it the OpenCL loader finds no platform and
# apps fail with "unknown OpenCL platform" (e.g. Geekbench --gpu).  Paired with
# the /etc/OpenCL/vendors/nvidia.icd manifest written further down, which names
# exactly this SONAME.
stage "libnvidia-opencl.so.$V"    "$SYS" "libnvidia-opencl.so.1"

# -- EGL GBM stack (#102 modeset): GPU-accelerated GL/EGL on the virtual KMS
# head needs THREE pieces, all of which must be present or the NVIDIA path is
# silently skipped and Mesa falls back to llvmpipe (software):
#
#   1. The GBM *backend* — Mesa's libgbm dlopens "<drmdriver>_gbm.so" from the
#      gbm backends dir by the card's DRM driver name ("nvidia-drm"). The NVIDIA
#      backend IS libnvidia-allocator (the host ships nvidia-drm_gbm.so as a
#      symlink to it). Without this, gbm_create_device() on card0 returns a Mesa
#      "dri" device and the NVIDIA EGL platform never even gets a chance. THIS
#      was the whole "EGL fails to init on the head" wall (#102 chunk 5).
#   2. libnvidia-egl-gbm.so.1 — the EGL external platform that handles
#      EGL_PLATFORM_GBM on an NVIDIA gbm device (config 15_nvidia_gbm.json).
#   3. libnvidia-allocator in the SYSTEM lib dir so the backend symlink resolves
#      for non-CUDA GL apps (compositors don't add /usr/local/nvidia-guest/lib).
GBMDIR="$SYS/gbm"
sudo mkdir -p "$GBMDIR"
# (1)+(3): allocator in the system dir + the GBM backend symlink to it.
if [ -f "$GFXBUNDLE/libnvidia-allocator.so.$V" ]; then
    sudo cp -f "$GFXBUNDLE/libnvidia-allocator.so.$V" "$SYS/"
    sudo ln -sf "libnvidia-allocator.so.$V"    "$SYS/libnvidia-allocator.so.1"
    sudo ln -sf "../libnvidia-allocator.so.$V" "$GBMDIR/nvidia-drm_gbm.so"
fi
# (2): the EGL external platforms (their own versions, not driver $V) and the
# egl_external_platform.d configs that tell EGL they exist.
#
# The configs matter as much as the libraries.  A Wayland GL client (es2gears,
# glmark2, any toolkit) calls eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND); with
# no 10_nvidia_wayland.json the NVIDIA external platform is never loaded, so
# glvnd hands the client to MESA's libEGL, which tries DRI2 on the nvkvm PCI
# device and fails:
#     libEGL warning: pci id for fd 4: 10de:2504, driver (null)
#     libEGL warning: egl: failed to create dri2 screen
# The client still connects and maps a surface, so the compositor composites a
# correctly-sized window that is entirely BLACK — which looks like a working
# desktop in a screenshot unless you check the client actually drew something.
# That is exactly what happened here before these two files were staged.
EGLEXT=/usr/share/egl/egl_external_platform.d
sudo mkdir -p "$EGLEXT"
for plat in wayland gbm; do
    lib="libnvidia-egl-$plat"
    found=""
    for f in "$GFXBUNDLE/$lib".so.*; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        sudo cp -f "$f" "$SYS/" && found="$b"
    done
    if [ -n "$found" ]; then
        sudo ln -sf "$found" "$SYS/$lib.so.1"
        STAGED_N=$((STAGED_N+1))
        case "$plat" in
            wayland) cfg="$EGLEXT/10_nvidia_wayland.json" ;;
            gbm)     cfg="$EGLEXT/15_nvidia_gbm.json"     ;;
        esac
        printf '{\n    "file_format_version" : "1.0.0",\n    "ICD" : {\n        "library_path" : "%s.so.1"\n    }\n}\n' \
            "$lib" | sudo tee "$cfg" >/dev/null
    else
        echo "stage_guest_libs: MISSING from bundle: $lib.so.*" >&2
        MISSING_LIBS="$MISSING_LIBS $lib.so.*"
    fi
done

# -- canonical CUDA dir: libcuda + allocator + ptxjit (what apps actually load) --
# The bare "libcuda.so" linker name is staged alongside the SONAME because
# JIT-compiling stacks link against it at RUNTIME: Triton (and therefore vLLM,
# torch.compile, and anything using Inductor) shells out to
# `gcc ... -lcuda -L<cuda dir>` to build its cuda_utils shim on first use, and
# `-lcuda` resolves ONLY through the unversioned symlink.  Without it the guest
# fails with "/usr/bin/ld: cannot find -lcuda" wrapped in an InductorError,
# which reads as a compiler problem rather than a missing symlink.  The host has
# this link from the normal driver package; the guest only gets what we stage.
stage "libcuda.so.$V"                  "$CUDADIR" "libcuda.so.1" "libcuda.so"
stage "libnvidia-allocator.so.$V"      "$CUDADIR" "libnvidia-allocator.so.1"
stage "libnvidia-ptxjitcompiler.so.$V" "$CUDADIR" "libnvidia-ptxjitcompiler.so.1"
# ptxjitcompiler dlopens libnvidia-nvvm.so.4 (driver >= 12.0). Staging the JIT
# compiler WITHOUT nvvm just moves the failure one layer down: cuInit and plain
# memcpy still pass, and every cuModuleLoadData(PTX) dies with
# CUDA_ERROR_JIT_COMPILER_NOT_FOUND (221) -- which reads as "kernels are broken",
# not as "a library is missing". Found during Ada/Turing bring-up 2026-08-17.
stage "libnvidia-nvvm.so.$V"           "$CUDADIR" "libnvidia-nvvm.so.1" "libnvidia-nvvm.so.4"
# Sweep STALE differently-versioned copies out of the CUDA dir.  This used to be
# an `rm -f $CUDADIR/lib*.so.575.51.03` with the version HARDCODED — the exact
# bug the $SYS block above documents as fixed, left unfixed here.  On a host
# actually running 575.51.03 it deleted the three files the lines above had just
# copied, leaving libcuda.so.1 & friends as DANGLING symlinks in the dir that
# ld.so searches FIRST.  Apps still worked only by accident (ld.so skips the
# dangling link and falls through to $SYS), which is why the dir looked like it
# "contains symlinks whose targets are only in /usr/lib/x86_64-linux-gnu".
# Same version-aware sweep as $SYS: never delete what was just staged.
for stale in "$CUDADIR"/*.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue      # SONAME links are ours, not stale versions
    case "$stale" in *".so.$V") continue ;; esac
    sudo rm -f "$stale"
done

# version-matched nvidia-smi binary.  The bundle name used to be hardcoded as
# "nvidia-smi-580", so on any bundle that names it plainly "nvidia-smi" (or
# nvidia-smi-575) this silently did nothing and the guest was left with either
# no nvidia-smi or a mismatched distro one.  Accept any of the spellings.
for smi in "$GFXBUNDLE/nvidia-smi-$V" "$GFXBUNDLE/nvidia-smi-${V%%.*}" "$GFXBUNDLE/nvidia-smi"; do
    [ -f "$smi" ] || continue
    sudo cp -f "$smi" /usr/local/bin/nvidia-smi && sudo chmod +x /usr/local/bin/nvidia-smi
    echo "stage_guest_libs: nvidia-smi <- $(basename "$smi")"
    break
done
[ -x /usr/local/bin/nvidia-smi ] || echo "stage_guest_libs: MISSING from bundle: nvidia-smi" >&2

# Put $CUDADIR on the loader path.  The header of this script has always claimed
# CUDA apps resolve libcuda there "via nvidia-guest.conf, AHEAD of the system
# dir" — but nothing ever created that file, so the dir was off the ld.so path
# entirely and every app silently resolved libcuda from $SYS instead.
if [ ! -f /etc/ld.so.conf.d/nvidia-guest.conf ]; then
    echo "$CUDADIR" | sudo tee /etc/ld.so.conf.d/nvidia-guest.conf >/dev/null
    echo "stage_guest_libs: created /etc/ld.so.conf.d/nvidia-guest.conf -> $CUDADIR"
fi

# -- sweep differently-versioned NVIDIA libraries out of $SYS BEFORE ldconfig --
#
# THIS MUST HAPPEN BEFORE ldconfig, AND IT MATTERS.  stage() creates the SONAME
# symlink itself with `ln -sf`, so immediately after staging the links are
# correct.  But every NVIDIA library of a given kind carries the SAME SONAME
# regardless of version -- libEGL_nvidia.so.550.54.14 and
# libEGL_nvidia.so.610.43.02 both declare SONAME libEGL_nvidia.so.0 -- so when
# ldconfig then scans this directory and finds two candidates it version-sorts
# them and re-points libEGL_nvidia.so.0 at the HIGHER-numbered one.  If the
# guest was previously staged against a newer driver, that is the STALE file,
# and ldconfig silently undoes the staging that just happened.
#
# Observed exactly this way: a guest staged 610.43.02, then re-staged 550.54.14
# after the host driver was changed.  All 22 libs copied, the script printed
# "done", and libEGL_nvidia.so.0/libGLX_nvidia.so.0 still pointed at the 610
# files.  CUDA was fine (libcuda resolves out of $CUDADIR, which has no second
# version in it), but the 610 GL/Vulkan vendor libraries could not initialise
# against a 550 kernel driver, so EGL and Vulkan fell back to Mesa llvmpipe --
# a SOFTWARE rasteriser -- while every log line said success.
#
# Only regular files are candidates (never the SONAME symlinks), and only ones
# whose suffix looks like a real driver version (3-digit major), so that
# genuinely differently-versioned components such as
# libnvidia-egl-gbm.so.1.1.1 are left alone.
for stale in "$SYS"/libnvidia-*.so.[0-9]* "$SYS"/libcuda.so.[0-9]* \
             "$SYS"/libnvcuvid.so.[0-9]* "$SYS"/libEGL_nvidia.so.[0-9]* \
             "$SYS"/libGLX_nvidia.so.[0-9]* "$SYS"/libGLESv2_nvidia.so.[0-9]* \
             "$SYS"/libGLESv1_CM_nvidia.so.[0-9]*; do
    [ -e "$stale" ] || continue
    [ -L "$stale" ] && continue
    case "$stale" in *".so.$V") continue ;; esac
    case "${stale##*.so.}" in [1-9][0-9][0-9].*) ;; *) continue ;; esac
    echo "stage_guest_libs: removing stale $(basename "$stale") (host driver is $V)"
    sudo rm -f "$stale"
done

sudo ldconfig

# Re-assert the GLVND vendor / ICD SONAME links after ldconfig, and verify.
# ldconfig is the component that historically broke these, so do not trust it:
# check where each link actually points and say so.
for pair in "libEGL_nvidia.so.0:libEGL_nvidia.so.$V" \
            "libGLX_nvidia.so.0:libGLX_nvidia.so.$V" \
            "libGLESv2_nvidia.so.2:libGLESv2_nvidia.so.$V"; do
    link="$SYS/${pair%%:*}"; want="${pair##*:}"
    [ -f "$SYS/$want" ] || continue
    sudo ln -sf "$want" "$link"
    got="$(readlink "$link")"
    if [ "$got" = "$want" ]; then
        echo "stage_guest_libs: $(basename "$link") -> $got"
    else
        echo "stage_guest_libs: WARNING $(basename "$link") -> $got (wanted $want)" >&2
    fi
done

# GLVND EGL vendor config for NVIDIA.  Without /usr/share/glvnd/egl_vendor.d/
# 10_nvidia.json, libGLX_nvidia never enters its NVIDIA device-detection path
# (it only sees mesa) and the Vulkan ICD bows out to llvmpipe.  Points at
# libEGL_nvidia.so.0 (staged above).
sudo mkdir -p /usr/share/glvnd/egl_vendor.d
sudo tee /usr/share/glvnd/egl_vendor.d/10_nvidia.json >/dev/null <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "libEGL_nvidia.so.0"
    }
}
JSON

# OpenCL ICD manifest.  The OpenCL loader (libOpenCL.so.1) finds vendors only
# through /etc/OpenCL/vendors/*.icd; with no manifest it enumerates zero
# platforms and apps fail with "unknown OpenCL platform" (e.g. Geekbench --gpu)
# even though libnvidia-opencl is present.  Written only when that library
# actually staged, so a host without it does not get a manifest pointing at a
# missing library (which would be worse than none: the loader would log an
# error for every OpenCL app).
if [ -e /usr/local/nvidia-guest/lib/libnvidia-opencl.so.1 ] ||
   [ -e /usr/lib/x86_64-linux-gnu/libnvidia-opencl.so.1 ]; then
    sudo mkdir -p /etc/OpenCL/vendors
    printf 'libnvidia-opencl.so.1\n' | sudo tee /etc/OpenCL/vendors/nvidia.icd >/dev/null
    echo "stage_guest_libs: OpenCL ICD -> /etc/OpenCL/vendors/nvidia.icd"
else
    echo "stage_guest_libs: libnvidia-opencl not staged -- OpenCL apps will find no platform" >&2
fi

# Vulkan ICD manifest.  The Vulkan ICD on Linux IS libGLX_nvidia.so.0 (staged
# above).  Without this manifest the loader enumerates only Mesa's lavapipe, so
# vulkaninfo reports "llvmpipe" and a Vulkan parity run silently measures the
# CPU rasterizer instead of the GPU.
sudo mkdir -p /usr/share/vulkan/icd.d
sudo tee /usr/share/vulkan/icd.d/nvidia_icd.json >/dev/null <<'JSON'
{
    "file_format_version" : "1.0.1",
    "ICD": {
        "library_path": "libGLX_nvidia.so.0",
        "api_version" : "1.4.303"
    }
}
JSON

# Blacklist nouveau: the emulated NVIDIA-id PCI device (nvkvm-gpu, the DRM render
# node's parent) would otherwise have nouveau auto-bind and probe it.  The device
# has no BARs, so nouveau can only fail/noise — keep it off the device entirely.
if [ ! -f /etc/modprobe.d/blacklist-nvkvm-nouveau.conf ]; then
    echo "blacklist nouveau" | sudo tee /etc/modprobe.d/blacklist-nvkvm-nouveau.conf >/dev/null
    echo "stage_guest_libs: blacklisted nouveau (reboot to take effect)"
fi

# Summary.  This script used to print "done" unconditionally, which read as
# success even when most of the bundle was absent.  Report what was missing and
# exit non-zero so a caller/CI notices.
echo "stage_guest_libs: staged $STAGED_N libs from $GFXBUNDLE (driver $V)"
if [ -n "$MISSING_LIBS" ]; then
    echo "stage_guest_libs: INCOMPLETE — missing from bundle:" >&2
    for m in $MISSING_LIBS; do echo "    $m" >&2; done
    echo "stage_guest_libs: GL/EGL/Vulkan/NVENC workloads may fall back to software or fail" >&2
    exit 2
fi
echo "stage_guest_libs: done ($V)"
