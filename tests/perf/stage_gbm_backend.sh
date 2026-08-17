#!/bin/bash
# stage_gbm_backend.sh — stage the NVIDIA GBM backend so gbm_create_device() on
# the virtual nvidia-drm card0 selects NVIDIA (not Mesa dri/llvmpipe).
# The backend IS libnvidia-allocator; Mesa libgbm loads <drmdriver>_gbm.so by
# name, so nvidia-drm_gbm.so must exist in the gbm backends dir.
set -u
# Default to whatever bundle make_host_bundle.sh actually produced, the same way
# stage_guest_libs.sh picks it up. The previous default was a hardcoded
# "host-libs-580" that no longer matches any driver the host scripts stage, so
# the script failed with "no libnvidia-allocator" on every current box and the
# GBM backend silently stayed Mesa/llvmpipe.
B="${1:-$(ls -d /mnt/nvkvm/host-libs-* 2>/dev/null | head -1)}"
SYS=/usr/lib/x86_64-linux-gnu
V=$(ls "$B"/libnvidia-allocator.so.* 2>/dev/null | sed "s#.*/libnvidia-allocator.so.##" | grep -E '^[0-9]' | head -1)
if [ -z "$V" ]; then echo "no libnvidia-allocator in $B"; exit 1; fi
sudo cp -f "$B/libnvidia-allocator.so.$V" "$SYS/"
sudo ln -sf "libnvidia-allocator.so.$V" "$SYS/libnvidia-allocator.so.1"
sudo ln -sf "../libnvidia-allocator.so.$V" "$SYS/gbm/nvidia-drm_gbm.so"
sudo ldconfig
echo "staged GBM backend (allocator $V)"
ls -la "$SYS/gbm/"
