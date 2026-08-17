#!/bin/bash
# run_weston_head.sh — bring up the Weston DRM compositor on the nvkvm virtual
# KMS head (/dev/dri/card0) GPU-accelerated, run a GL client, screenshot, verify
# the renderer is the NVIDIA GPU (not llvmpipe). Run as root inside the guest.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
LOG=/tmp/weston.log; rm -f "$LOG"

# Clean slate: a leftover weston still holding DRM master makes new instances
# fail SET_MASTER with EBUSY (degraded, no scanout). Reap all first.
# (match process NAMES, not -f: the script path contains "weston" and -f would
#  SIGKILL this script itself.)
pkill -9 -x weston 2>/dev/null
pkill -9 'weston-' 2>/dev/null; pkill -9 -x kmscube 2>/dev/null
sleep 2

# DRM backend, GL renderer, no input needed, idle never. --debug exposes the
# weston_screenshooter protocol (gated off by default) so we can capture proof.
weston --backend=drm-backend.so --renderer=gl --idle-time=0 --debug \
       --log="$LOG" --continue-without-input >/tmp/weston.out 2>&1 &
WPID=$!
sleep 6

if ! kill -0 "$WPID" 2>/dev/null; then
    echo "WESTON DIED. log:"; cat "$LOG" /tmp/weston.out 2>/dev/null | tail -30; exit 1
fi
echo "=== weston up (pid $WPID) ==="
grep -iE "renderer|EGL|GL version|drm|crtc|connector|warning|error" "$LOG" | head -25

# Discover the actual wayland socket weston created (name varies: wayland-0/1).
SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"
echo "wayland socket: $WAYLAND_DISPLAY"

# Run real GL clients for a few seconds (proves clients composit via the GPU).
timeout 6 weston-simple-egl >/tmp/wsegl.out 2>&1 &
timeout 6 weston-terminal >/tmp/wterm.out 2>&1 &
sleep 7
# Screenshot the composited output (writes wayland-screenshot-*.png in CWD).
cd /tmp && timeout 5 weston-screenshooter 2>/tmp/shot.err
echo "=== screenshot ==="
ls -la /tmp/*screenshot*.png 2>/dev/null || { echo "no screenshot"; cat /tmp/shot.err; }

kill "$WPID" 2>/dev/null; sleep 1
echo "=== client logs ==="; tail -3 /tmp/wsegl.out; tail -3 /tmp/wterm.out
