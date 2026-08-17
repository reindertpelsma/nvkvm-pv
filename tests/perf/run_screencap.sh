#!/bin/bash
# run_screencap.sh — zero-copy GPU screen capture via wlr-screencopy → dma-buf
# (#110).  Brings up headless sway (wlroots, NVIDIA GL through nvkvm), runs a
# continuously-rendering GL workload as a client, and measures sustained capture
# fps of the composited output into a GPU bo.  The compositor importing the
# capture client's dma-buf is the cross-isolate import nvkvm now supports.
#
#   usage: run_screencap.sh [nframes] [workload-cmd...]
#     nframes       frames to capture (default 300)
#     workload-cmd  GL client sway runs (default es2gears_wayland)
set -u
SRC=${SRC:-/mnt/nvkvm}
APPS="$SRC/tests/perf/apps"
PROTO_SCAP="$SRC/tests/perf/protocols/wlr-screencopy-unstable-v1.xml"
PROTO_DMABUF=/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
B=/tmp/scapbuild; mkdir -p "$B"

NFRAMES="${1:-300}"; shift || true
WORKLOAD="${*:-es2gears_wayland}"

export XDG_RUNTIME_DIR=/run/user/$(id -u)
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null

echo "=== generate protocol glue ==="
wayland-scanner client-header "$PROTO_SCAP"   "$B/wlr-screencopy-unstable-v1-client-protocol.h" || exit 1
wayland-scanner private-code  "$PROTO_SCAP"   "$B/wlr-screencopy-unstable-v1-protocol.c"        || exit 1
wayland-scanner client-header "$PROTO_DMABUF" "$B/linux-dmabuf-unstable-v1-client-protocol.h"   || exit 1
wayland-scanner private-code  "$PROTO_DMABUF" "$B/linux-dmabuf-unstable-v1-protocol.c"          || exit 1

echo "=== build wlr_screencap ==="
cc -O2 -o "$B/wlr_screencap" "$APPS/wlr_screencap.c" \
   "$B/wlr-screencopy-unstable-v1-protocol.c" "$B/linux-dmabuf-unstable-v1-protocol.c" \
   -I"$B" -I/usr/include/libdrm \
   $(pkg-config --cflags --libs wayland-client gbm libdrm) || exit 1
echo "build OK"

echo "=== bring up headless sway (GL, NVIDIA via nvkvm) ==="
pkill -9 sway 2>/dev/null; pkill -9 'es2gears\|glmark2\|wlr_screencap' 2>/dev/null
sleep 2
cat > "$B/sway.conf" <<EOF
output HEADLESS-1 mode 1920x1080@60Hz
exec $WORKLOAD
EOF
WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_NO_HARDWARE_CURSORS=1 \
  WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 \
  sway -c "$B/sway.conf" >/tmp/sway.log 2>&1 &
SWPID=$!
sleep 7
kill -0 $SWPID 2>/dev/null || { echo "sway DIED"; tail -20 /tmp/sway.log; exit 1; }
SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"
echo "sway up, socket=$WAYLAND_DISPLAY, workload=$WORKLOAD"
sleep 2   # let the workload start rendering

echo "=== capture $NFRAMES frames ==="
"$B/wlr_screencap" "$NFRAMES" --ppm /tmp/screencap.ppm
RC=$?

pkill -9 sway 2>/dev/null; pkill -9 'es2gears\|glmark2' 2>/dev/null
exit $RC
