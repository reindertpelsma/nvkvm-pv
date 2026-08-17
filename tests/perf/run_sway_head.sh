#!/bin/bash
# run_sway_head.sh — bring up a sway (wlroots) Wayland desktop on the nvkvm
# virtual KMS head, GPU-accelerated, run real apps (Wayland-native foot + a GL
# app, X11 glxgears + mate-panel via XWayland), and capture a screenshot with
# grim (wlr-screencopy). Proves a real desktop renders on the GPU through nvkvm.
# Run as root in the guest.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=drm
export WLR_DRM_DEVICES=/dev/dri/card0
export LIBSEAT_BACKEND=seatd
pkill -9 -x sway 2>/dev/null; pkill -9 foot 2>/dev/null; pkill -9 -x glxgears 2>/dev/null
systemctl restart seatd 2>/dev/null; sleep 2

cat > /tmp/sway.conf <<CONF
output * mode 1920x1080@60Hz
output * bg #1a3b1a solid_color
exec foot
exec sh -c 'sleep 1; DISPLAY=:0 glxgears -geometry 700x500 || glxgears -geometry 700x500'
exec sh -c 'sleep 1; mate-panel'
# capture then leave the session up briefly, then exit cleanly
exec sh -c 'sleep 6; grim /tmp/sway_shot.png 2>/tmp/grim.err; echo GRIM_RC=\$? >/tmp/grim.rc; sleep 1; swaymsg exit'
CONF

# wlroots refuses the NVIDIA proprietary driver without --unsupported-gpu.
WLR_DRM_NO_MODIFIERS=1 WLR_RENDERER=gles2 timeout 25 sway --unsupported-gpu -c /tmp/sway.conf >/tmp/sway.out 2>&1
echo "=== sway exit: $? ==="
echo "--- renderer/drm ---"
grep -iE "renderer|GLES|GL |drm|NVIDIA|nouveau|llvmpipe|EGL|master|output|error|fail" /tmp/sway.out | grep -ivE "load_msg|deprecat" | head -25
echo "--- grim ---"; cat /tmp/grim.rc 2>/dev/null; cat /tmp/grim.err 2>/dev/null
ls -la /tmp/sway_shot.png 2>/dev/null && echo HAVE_SHOT || echo NO_SHOT
