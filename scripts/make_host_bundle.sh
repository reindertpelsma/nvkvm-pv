#!/usr/bin/env bash
# make_host_bundle.sh — collect the NVIDIA userspace the GUEST needs, from the
# driver installed on THIS host, into host-libs-<version>/.
#
# WHY THIS EXISTS.  The guest must run NVIDIA userspace version-matched to the
# HOST driver: NVML and libcuda refuse to initialise against a mismatch
# ("Driver/library version mismatch", cuInit 803).  Those libraries are not
# redistributable, so they cannot live in this repository -- they have to be
# taken from whatever driver you have installed.  This used to be a hand-copied
# directory, which meant a first-time user hit a script that referred to a
# bundle nobody had told them to build.
#
# Run on the HOST.  Then, inside the guest: scripts/stage_guest_libs.sh
set -euo pipefail

OUT_ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
SYSLIB="${SYSLIB:-/usr/lib/x86_64-linux-gnu}"

# Driver version.  nvidia-smi is the obvious source but it is not always
# usable: inside a container the NVIDIA runtime injects the libraries and the
# device nodes, yet NVML can still fail to initialise ("Unknown Error"), which
# used to leave V holding an error message.  The injected library filename
# carries the same version and is always there, so fall back to it.
V="${NVKVM_DRIVER_VERSION:-}"
if [ -z "$V" ] && command -v nvidia-smi >/dev/null; then
    V="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null \
         | head -1 | tr -d '[:space:]')"
fi
case "$V" in
    [0-9]*.[0-9]*) ;;                       # looks like a version, keep it
    *) V="$(ls "$SYSLIB"/libnvidia-ml.so.*.* 2>/dev/null | head -1 \
            | sed 's#.*/libnvidia-ml\.so\.##')" ;;
esac
case "$V" in
    [0-9]*.[0-9]*) ;;
    *) echo "make_host_bundle: could not determine the host driver version." >&2
       echo "                  Pass it explicitly: NVKVM_DRIVER_VERSION=580.173.02 $0" >&2
       exit 1 ;;
esac

DEST="$OUT_ROOT/host-libs-$V"
mkdir -p "$DEST"
echo "make_host_bundle: host driver $V -> $DEST"

# REQUIRED: without these the guest cannot enumerate or run CUDA at all.
REQUIRED="libcuda libnvidia-ml libnvidia-ptxjitcompiler libnvidia-nvvm"
# OPTIONAL: absence degrades a capability rather than breaking bring-up.
#   glcore/eglcore/glsi/tls/rtcore/gpucomp -> GL/Vulkan
#   allocator/egl-gbm/egl-wayland          -> EGL platforms, GBM, Wayland clients
#   encode/nvcuvid                         -> NVENC / NVDEC (encode needs BOTH)
#
# The GLVND *vendor* libraries below were missing from this list, while
# stage_guest_libs.sh stages every one of them by name -- so the bundle this
# script produced made that script exit 2 with five MISSING lines, the first of
# them libEGL_nvidia.  That is the library /usr/share/glvnd/egl_vendor.d/
# 10_nvidia.json points at: without it EGL has no NVIDIA vendor to load and
# EVERY GL/EGL client silently falls back to Mesa llvmpipe -- the exact
# software-rendering symptom this bundle exists to prevent.  libGLX_nvidia is
# also the Vulkan ICD named by nvidia_icd.json, so its absence silently demotes
# Vulkan to lavapipe.  Collect what the staging script actually stages.
OPTIONAL="libnvidia-glcore libnvidia-eglcore libnvidia-glsi libnvidia-tls \
          libnvidia-rtcore libnvidia-gpucomp libnvidia-allocator \
          libnvidia-encode libnvcuvid libnvidia-glvkspirv libnvidia-opencl \
          libEGL_nvidia libGLX_nvidia libGLESv2_nvidia libGLESv1_CM_nvidia \
          libnvidia-cfg"

# NOT driver-versioned.  libnvidia-egl-wayland and libnvidia-egl-gbm come from
# separate upstream projects (NVIDIA/egl-wayland, NVIDIA/egl-gbm) that are merely
# packaged alongside the driver, so they carry their OWN SONAME version:
#     libcuda.so.575.51.03            <- driver version
#     libnvidia-egl-wayland.so.1.1.19 <- upstream egl-wayland version
#     libnvidia-egl-gbm.so.1.1.2      <- upstream egl-gbm version
# Globbing "$lib.so.$V" therefore NEVER matches them and skips them in silence.
# They are the EGL *external platform* libraries; without them a Wayland client
# falls back to llvmpipe while the EGL device platform still reports NVIDIA --
# i.e. software rendering that looks correct in a screenshot.
SONAME_VERSIONED="libnvidia-egl-wayland libnvidia-egl-gbm"

missing_required=""
n=0
for lib in $REQUIRED $OPTIONAL; do
    f="$SYSLIB/$lib.so.$V"
    if [ -f "$f" ]; then
        cp -f "$f" "$DEST/"; n=$((n+1))
    elif echo "$REQUIRED" | grep -qw -- "$lib"; then
        missing_required="$missing_required $lib"
    else
        echo "make_host_bundle: optional $lib.so.$V not installed -- skipping" >&2
    fi
done

for lib in $SONAME_VERSIONED; do
    # take the real file behind the .so.1 SONAME link, whatever it is versioned as
    real=$(readlink -f "$SYSLIB/$lib.so.1" 2>/dev/null || true)
    if [ -n "$real" ] && [ -f "$real" ]; then
        cp -f "$real" "$DEST/"; n=$((n+1))
        echo "make_host_bundle: $lib -> $(basename "$real")  (not driver-versioned)"
    else
        echo "make_host_bundle: optional $lib not installed -- Wayland/GBM clients will fall back to software" >&2
    fi
done

# nvidia-smi itself, so the guest can report what it sees.
[ -x /usr/bin/nvidia-smi ] && { cp -f /usr/bin/nvidia-smi "$DEST/"; n=$((n+1)); }

# EGL/Vulkan loader configuration.  These are plain JSON and drive which vendor
# the loaders pick; without the external-platform files, Wayland clients fall
# back to software while the device platform still reports NVIDIA -- a failure
# that looks like success in a screenshot.
for d in /usr/share/glvnd/egl_vendor.d /usr/share/egl/egl_external_platform.d /usr/share/vulkan/icd.d; do
    [ -d "$d" ] || continue
    sub="$DEST/config/${d#/usr/share/}"; mkdir -p "$sub"
    find "$d" -maxdepth 1 -name '*nvidia*.json' -exec cp -f {} "$sub/" \; 2>/dev/null || true
done

if [ -n "$missing_required" ]; then
    echo "make_host_bundle: FAILED -- required libraries missing from $SYSLIB:$missing_required" >&2
    echo "make_host_bundle: the installed driver looks incomplete for version $V" >&2
    exit 1
fi

echo "make_host_bundle: collected $n files into $DEST"
echo "make_host_bundle: next, inside the guest: sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh"
