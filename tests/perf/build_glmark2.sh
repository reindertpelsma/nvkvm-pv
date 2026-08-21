#!/bin/bash
# build_glmark2.sh — build ONE glmark2 (2023.01) into /opt/glmark2 on the HOST,
# to be run byte-identical on both sides. Share /opt/glmark2 into the guest via
# NVKVM_SHARE_DIR and copy it to guest-local disk; verify with sha256sum.
# Built against the host's older glibc so the same ELF also runs on the guest.
set -x
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    git meson ninja-build pkg-config g++ \
    libjpeg-dev libpng-dev libudev-dev \
    libwayland-dev wayland-protocols libwayland-egl-backend-dev \
    libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dev \
    libx11-dev weston xwayland strace linux-tools-common linux-tools-generic \
    numactl time 2>&1 | tail -5
rm -rf /opt/glmark2-src
git clone --depth 1 -b 2023.01 https://github.com/glmark2/glmark2 /opt/glmark2-src 2>&1 | tail -2
cd /opt/glmark2-src
meson setup build --prefix=/opt/glmark2 \
    -Dflavors=wayland-gl,wayland-glesv2,x11-gl,x11-glesv2 2>&1 | tail -20
ninja -C build 2>&1 | tail -5
ninja -C build install 2>&1 | tail -3
ls -la /opt/glmark2/bin/
sha256sum /opt/glmark2/bin/* 
echo "=== GLMARK2 BUILD DONE ==="
