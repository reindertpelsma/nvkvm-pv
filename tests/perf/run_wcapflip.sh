#!/bin/bash
# run_wcapflip.sh — headless GPU compositor -> weston_output_capture into an
# nvkvm GPU dma-buf -> flip on the virtual head -> present to host (#110).
# Builds wcapflip from the vendored protocol, brings up headless weston + a GL
# client, and runs the capture-flip loop.  Run as root in the guest.
set -u
SRC=${SRC:-/mnt/nvkvm}            # repo (9p) inside the guest
APPS="$SRC/tests/perf/apps"
PROTO_WCAP="$SRC/tests/perf/protocols/weston-output-capture.xml"
PROTO_DMABUF=/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
B=/tmp/wcapbuild; mkdir -p "$B"

MODE="${1:-dmabuf}"               # dmabuf | shm
NFRAMES="${2:-120}"

export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

echo "=== generate protocol glue ==="
wayland-scanner client-header "$PROTO_WCAP"   "$B/weston-output-capture-client-protocol.h" || exit 1
wayland-scanner private-code  "$PROTO_WCAP"   "$B/weston-output-capture-protocol.c"        || exit 1
wayland-scanner client-header "$PROTO_DMABUF" "$B/linux-dmabuf-unstable-v1-client-protocol.h" || exit 1
wayland-scanner private-code  "$PROTO_DMABUF" "$B/linux-dmabuf-unstable-v1-protocol.c"        || exit 1

echo "=== build wcapflip ==="
cc -O2 -o "$B/wcapflip" "$APPS/wcapflip.c" \
   "$B/weston-output-capture-protocol.c" "$B/linux-dmabuf-unstable-v1-protocol.c" \
   -I"$B" -I/usr/include/libdrm \
   $(pkg-config --cflags --libs wayland-client gbm libdrm) || exit 1
echo "build OK"

echo "=== bring up headless weston (GL) ==="
pkill -9 -x weston 2>/dev/null; pkill -9 'weston-' es2gears_wayland glmark2 2>/dev/null
sleep 2
dmesg -C 2>/dev/null || sudo dmesg -C 2>/dev/null
weston --backend=headless-backend.so --renderer=gl --width=1920 --height=1080 \
       --idle-time=0 --debug --log=/tmp/wh.log >/tmp/wh.out 2>&1 &
WPID=$!
sleep 6
kill -0 $WPID 2>/dev/null || { echo "weston DIED"; tail -15 /tmp/wh.log; exit 1; }
SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"
echo "weston up, socket=$WAYLAND_DISPLAY"

echo "=== run a continuously-animating GL client (es2gears) ==="
timeout 30 es2gears_wayland >/tmp/eg.out 2>&1 &

echo "=== run wcapflip ($MODE, $NFRAMES frames) ==="
MARG=""; [ "$MODE" = shm ] && MARG=--shm; [ "$MODE" = udmabuf ] && MARG=--udmabuf
"$B/wcapflip" /dev/dri/card0 "$NFRAMES" $MARG
RC=$?
echo "wcapflip rc=$RC"
echo "=== present-path flips reported by guest kernel ==="
( dmesg 2>/dev/null || sudo dmesg ) | grep -c "nvkvm present: flip"
( dmesg 2>/dev/null || sudo dmesg ) | grep "nvkvm present: flip" | tail -2

pkill -9 -x weston 2>/dev/null; pkill -9 es2gears_wayland 2>/dev/null
exit $RC
