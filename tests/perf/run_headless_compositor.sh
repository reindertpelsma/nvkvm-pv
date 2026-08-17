#!/bin/bash
# run_headless_compositor.sh — bring up a HEADLESS GPU-accelerated Wayland
# compositor on nvkvm and prove it renders a real desktop (#110).
#
# WHY HEADLESS (the #110 pivot, 2026-06-02):
#   A DRM-backend compositor (weston --backend=drm-backend) on our virtual KMS
#   head comes up GPU-accelerated but HANGS FOREVER: its main thread blocks in
#   poll(-1) deep inside NVIDIA's libnvidia-egl-gbm / libEGL / eglcore, called
#   from the drm-backend's scanout-buffer path. NVIDIA's userspace EGL GBM
#   *scanout present* path is coupled to nvidia-modeset semantics that our
#   virtual head deliberately does not provide (we never forward NVKMS). It never
#   issues a single ADDFB/ATOMIC/PAGEFLIP and never services clients.
#
#   The HEADLESS backend renders the composite into an offscreen GPU buffer with
#   NO KMS scanout — so it never touches that NVIDIA present path. Its main loop
#   is a healthy epoll_wait; GL clients connect and render via NVIDIA GL through
#   nvkvm; the composited desktop captures cleanly. This is the cloud-gaming
#   architecture (render offscreen → capture → deliver), and the basis for the
#   capture→present→host pipeline (#106/#107).
#
# Run as root inside the guest.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
LOG=/tmp/wh.log; rm -f "$LOG" /tmp/*screenshot*.png
W=${W:-1920}; H=${H:-1080}

pkill -9 -x weston 2>/dev/null; pkill -9 'weston-' 2>/dev/null
pkill -9 es2gears_wayland glmark2 2>/dev/null
sleep 2

# Headless backend + GL renderer (NVIDIA). --debug exposes the screenshooter.
weston --backend=headless-backend.so --renderer=gl --width="$W" --height="$H" \
       --idle-time=0 --debug --log="$LOG" >/tmp/wh.out 2>&1 &
WPID=$!
sleep 6
if ! kill -0 "$WPID" 2>/dev/null; then
    echo "HEADLESS WESTON DIED. log:"; tail -25 "$LOG" /tmp/wh.out; exit 1
fi
echo "=== headless weston up (pid $WPID) ==="
grep -iE "renderer|EGL vendor|GL version|GL renderer|headless|error|fail" "$LOG" | head -12

# Verify the main loop is healthy (epoll_wait, NOT stuck in NVIDIA EGL).
if command -v gdb >/dev/null; then
    echo "=== main-thread state (expect epoll_wait / wl_display_run) ==="
    gdb -p "$WPID" -batch -ex "thread 1" -ex "bt 3" 2>/dev/null \
        | grep -iE "#[0-9].*(epoll_wait|poll|egl|wl_display)" | head -3
fi

SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"
echo "wayland socket: $WAYLAND_DISPLAY"

# A real GL client renders via NVIDIA GL through nvkvm (proves the render path).
echo "=== GL client (es2gears_wayland) renders 6s ==="
timeout 6 es2gears_wayland >/tmp/gears.out 2>&1 &
sleep 6

# Capture the composited desktop (proof of content). CPU readback (slow) — the
# fast/zero-copy GPU dma-buf capture → present path is the next milestone.
cd /tmp && timeout 5 weston-screenshooter 2>/tmp/shot.err
SHOT=$(ls -t /tmp/*screenshot*.png 2>/dev/null | head -1)
if [ -n "$SHOT" ]; then
    echo "=== CAPTURED: $SHOT ($(stat -c%s "$SHOT") bytes) ==="
    command -v file >/dev/null && file "$SHOT"
else
    echo "no screenshot:"; cat /tmp/shot.err
fi
kill "$WPID" 2>/dev/null
