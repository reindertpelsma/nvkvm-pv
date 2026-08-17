#!/bin/bash
# diag_client_surface_path.sh — the client has an NVIDIA GL context but its
# window contributes ZERO pixels to the composited output.  Find where it stops.
#
# Established by verify_client_window_differential.sh:
#   GL_RENDERER = NVIDIA GeForce RTX 3060  (so the EGL external platform loaded,
#                                           the context is real and on the GPU)
#   capture(client running) == capture(no client), bit-identical
#   => the client's buffers never become a compositor-visible surface.
#
# Candidate stop points, in order of the buffer's journey:
#   1. client blocks in eglSwapBuffers / frame callback  -> backtrace shows it
#   2. compositor never advertises a buffer-sharing protocol the NVIDIA platform
#      can use (zwp_linux_dmabuf_v1 / wl_drm / wl_eglstream_controller)
#   3. buffer is submitted but import into the compositor fails -> weston log
# Run as root inside the guest.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
W=${W:-1920}; H=${H:-1080}
LOG=/tmp/wh.log

pkill -9 -x weston 2>/dev/null
pkill -9 -x glmark2-wayland 2>/dev/null; pkill -9 -x es2gears_wayland 2>/dev/null
sleep 2
rm -f "$LOG" /tmp/client.out

weston --backend=headless-backend.so --renderer=gl --width="$W" --height="$H" \
       --idle-time=0 --debug --log="$LOG" >/tmp/wh.out 2>&1 &
WPID=$!
sleep 6
SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"

echo "########## 1. COMPOSITOR EGL/renderer capabilities ##########"
grep -iE "EGL |GL |dmabuf|dma_buf|renderer|extension" "$LOG" | head -40

echo
echo "########## 2. COMPOSITOR-ADVERTISED GLOBALS ##########"
# WAYLAND_DEBUG=1 on a trivial client dumps the registry the compositor sends.
# Look for: zwp_linux_dmabuf_v1, wl_drm, wl_eglstream_controller.
WAYLAND_DEBUG=1 timeout 5 weston-info >/tmp/globals.out 2>&1 || \
WAYLAND_DEBUG=1 timeout 5 es2gears_wayland >/tmp/globals.out 2>&1
grep -oE "global # *[0-9]+ [a-z_0-9]+" /tmp/globals.out | awk '{print $NF}' | sort -u
echo "--- buffer-sharing protocols specifically ---"
for p in zwp_linux_dmabuf_v1 wl_drm wl_eglstream_controller wl_shm zwp_linux_explicit_synchronization_v1; do
    if grep -q "$p" /tmp/globals.out 2>/dev/null; then echo "  PRESENT: $p"; else echo "  ABSENT : $p"; fi
done

echo
echo "########## 3. CLIENT: run and see where it stops ##########"
glmark2-wayland --run-forever >/tmp/client.out 2>&1 &
CPID=$!
sleep 12

if kill -0 "$CPID" 2>/dev/null; then
    echo "client pid $CPID ALIVE"
    echo "--- CPU time consumed (spinning vs blocked) ---"
    ps -o pid,etime,time,stat,wchan:24,comm -p "$CPID"
    echo "--- /proc/$CPID/stack-ish: wchan ---"
    cat /proc/$CPID/wchan 2>/dev/null; echo
    echo "--- backtrace of every thread (where is it stuck?) ---"
    if command -v gdb >/dev/null; then
        gdb -p "$CPID" -batch -ex "thread apply all bt 12" 2>/dev/null | head -60
    else
        echo "(gdb not installed)"
    fi
    echo "--- last syscalls ---"
    if command -v strace >/dev/null; then
        timeout 4 strace -p "$CPID" -f -e trace=poll,ppoll,recvmsg,sendmsg,ioctl,futex 2>&1 | tail -25
    else
        echo "(strace not installed)"
    fi
else
    echo "client EXITED"
fi

echo
echo "--- client stdout/stderr (full) ---"
cat /tmp/client.out

echo
echo "########## 4. COMPOSITOR LOG after client ran ##########"
tail -30 "$LOG"

pkill -9 -x glmark2-wayland 2>/dev/null
kill "$WPID" 2>/dev/null
