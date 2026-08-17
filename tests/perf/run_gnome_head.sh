#!/bin/bash
# run_gnome_head.sh — bring up GNOME Shell on Wayland on the nvkvm virtual KMS
# head (mutter DRM backend, GPU-accelerated via the NVIDIA GBM path), run an app,
# and screenshot via the GNOME Shell D-Bus. Real DE proof on the virtual head.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
export LIBSEAT_BACKEND=seatd
export MUTTER_DEBUG_FORCE_KMS_MODE=simple
pkill -9 gnome-shell 2>/dev/null; pkill -9 -x mutter 2>/dev/null; pkill -9 -x weston 2>/dev/null
systemctl restart seatd 2>/dev/null; sleep 2

dbus-run-session -- bash -c '
  export XDG_RUNTIME_DIR=/run/user/0
  gnome-shell --wayland --display-server --wayland-display nvkvm-wl >/tmp/gnome.out 2>&1 &
  GS=$!
  sleep 14
  if ! kill -0 $GS 2>/dev/null; then echo "GNOME-SHELL DIED:"; tail -20 /tmp/gnome.out; exit 1; fi
  export WAYLAND_DISPLAY=nvkvm-wl
  gnome-terminal >/tmp/gt.out 2>&1 &
  glxgears >/tmp/gg.out 2>&1 &
  sleep 6
  gdbus call --session -d org.gnome.Shell -o /org/gnome/Shell/Screenshot \
    -m org.gnome.Shell.Screenshot.Screenshot true false /tmp/gnome_shot.png 2>/tmp/shot.err
  echo "screenshot rc=$?"; cat /tmp/shot.err
  kill $GS 2>/dev/null
'
echo "=== gnome-shell renderer/drm ==="
grep -iE "renderer|GL |drm|nvidia|nouveau|llvmpipe|EGL|kms|output|error|fail|refus|root" /tmp/gnome.out | head -25
ls -la /tmp/gnome_shot.png 2>/dev/null && echo HAVE_SHOT || echo NO_SHOT
