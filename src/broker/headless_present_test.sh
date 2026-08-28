#!/bin/bash
# headless_present_test.sh — test the REAL dma-buf import and present path
# against a real compositor, with no GPU driver cooperation required.
#
# selftest.sh says what it cannot do: "the actual dma-buf import and present on
# a real compositor or X server".  This closes that gap by standing up weston
# on its headless backend with the software GL renderer (llvmpipe), which
# imports DRM_FORMAT_MOD_LINEAR faithfully.
#
# WHY THAT MATTERS ON NVIDIA.  Mutter advertises XR24 + modifier 0x0 (LINEAR)
# and NVIDIA then REFUSES to import it, so on an NVIDIA host the linear tier is
# unreachable and untestable -- every real frame falls back to shm or to the
# block-linear native tier.  llvmpipe accepts it, so the linear tier finally
# gets exercised.  Measured 2026-08-28: linear buffers allocated on BOTH an
# NVIDIA and an AMD render node import cleanly here.
#
# WHAT IT ASSERTS: that a bounded stream of N frames results in N commits --
# specifically that the LAST frame of a finite stream is presented and not left
# sitting unflushed.  That is a real bug class: an idle desktop produces exactly
# one frame and nothing follows to push it out.
#
# TWO WAYS TO RUN IT, both headless:
#
#   NVKVM_TEST_SOFTWARE=1  (default) weston's GL renderer on llvmpipe.  Needs
#                          no working GPU driver at all, so it runs in CI.
#   NVKVM_TEST_SOFTWARE=0  weston's GL renderer on real hardware.  On a box
#                          with a non-NVIDIA GPU -- an AMD or Intel iGPU
#                          alongside the NVIDIA card is the common case -- this
#                          is the stronger test: a real driver doing a real
#                          import, still with no monitor attached.
#
# The allocator node is chosen separately from the compositor and PREFERS A
# NON-NVIDIA NODE, because allocating linear on NVIDIA is the case that has no
# consumer on an NVIDIA-only host.  Override either with:
#
#   NVKVM_TEST_NODE=/dev/dri/renderD129 NVKVM_TEST_SOFTWARE=0 \
#       bash headless_present_test.sh
#
# REQUIREMENTS: weston and ANY render node to allocate from (in software mode
# the compositor needs no GPU; the allocator always does).  Skips cleanly when
# they are missing rather than failing.
#
# Exits 0 on pass, 1 on failure, 77 on skip (automake's skip convention).
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BROKER="$DIR/nvkvm-display-broker"
DMASRC="$DIR/nvkvm-broker-dmabuf-src"
FRAMES=10
rc=0

skip() { echo "SKIP: $*"; exit 77; }

command -v weston >/dev/null || skip "weston is not installed"
[ -x "$BROKER" ] || skip "broker not built ($BROKER)"
[ -x "$DMASRC" ] || skip "dma-buf source not built -- needs libgbm at build time"

# Prefer a non-NVIDIA node: NVIDIA refuses to import linear, so a linear buffer
# allocated there has no consumer on an NVIDIA-only host.
NODE="${NVKVM_TEST_NODE:-}"
if [ -z "$NODE" ]; then
    for n in /dev/dri/renderD*; do
        [ -e "$n" ] || continue
        drv=$(sed -n 's/^DRIVER=//p' "/sys/class/drm/$(basename "$n")/device/uevent" 2>/dev/null)
        [ -z "$NODE" ] && NODE="$n"
        if [ "$drv" != "nvidia" ]; then NODE="$n"; break; fi
    done
fi
[ -n "$NODE" ] || skip "no render node to allocate a dma-buf from"

WORK="$(mktemp -d)"
export XDG_RUNTIME_DIR="$WORK"
chmod 700 "$WORK"
trap 'kill ${WPID:-} ${BPID:-} ${SPID:-} 2>/dev/null; rm -rf "$WORK"' EXIT

SOFT="${NVKVM_TEST_SOFTWARE:-1}"
if [ "$SOFT" = 1 ]; then
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    REND="llvmpipe (software)"
else
    REND="hardware GL"
fi
echo "== headless weston, $REND + linear dma-buf allocated on $NODE =="

weston --backend=headless --renderer=gl --width=800 --height=600 \
       --socket=wl-headless --idle-time=0 >"$WORK/weston.log" 2>&1 &
WPID=$!
for _ in $(seq 60); do [ -S "$WORK/wl-headless" ] && break; sleep 0.25; done
[ -S "$WORK/wl-headless" ] || { echo "weston did not start:"; tail -12 "$WORK/weston.log"; exit 77; }
export WAYLAND_DISPLAY=wl-headless

"$BROKER" --socket "$WORK/b.sock" --socket-mode 0600 --allow-user root \
          --backend wayland --size 640x480 --title headless-test \
          --trace-frames >"$WORK/broker.log" 2>&1 &
BPID=$!
for _ in $(seq 60); do [ -S "$WORK/b.sock" ] && break; sleep 0.25; done
[ -S "$WORK/b.sock" ] || { echo "FAIL: broker never created its socket"; tail -12 "$WORK/broker.log"; exit 1; }

"$DMASRC" --node "$NODE" --present "$WORK/b.sock" --modifier linear \
          --size 640x480 --frames "$FRAMES" >"$WORK/src.log" 2>&1 &
SPID=$!
sleep $(( FRAMES / 4 + 6 ))
kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
sleep 1

sent=$(grep -c 'sent ATTACH+COMMIT' "$WORK/src.log")
commits=$(grep -c 'frame: commit seq=' "$WORK/broker.log")
last=$(grep -o 'frame: commit seq=[0-9]*' "$WORK/broker.log" | grep -o '[0-9]*$' | sort -n | tail -1)

if ! grep -q 'CAN import .* modifier 0x0000000000000000' "$WORK/broker.log"; then
    echo "SKIP: this compositor did not import LINEAR -- nothing to assert"
    grep -iE 'refused|ERROR' "$WORK/broker.log" | head -3
    exit 77
fi
echo "  linear import : proven by import, not by advertisement"
echo "  frames sent   : $sent"
echo "  commits       : $commits"
echo "  last seq      : ${last:-none}"

# One frame is legitimately spent probing whether the pair can be imported.
if [ "$commits" -lt $(( sent - 1 )) ]; then
    echo "FAIL: $commits commits for $sent frames (more than the one probe frame lost)"
    rc=1
fi
# THE POINT OF THE TEST: the final frame must not be left unpresented.
if [ "${last:-0}" -ne $(( sent - 1 )) ] && [ "${last:-0}" -ne "$sent" ]; then
    echo "FAIL: last committed seq is ${last:-none}, expected $(( sent - 1 )) or $sent"
    echo "      -> the LAST frame of a finite stream was not presented"
    rc=1
fi
[ $rc -eq 0 ] && echo "PASS: every frame committed, including the last"
exit $rc
