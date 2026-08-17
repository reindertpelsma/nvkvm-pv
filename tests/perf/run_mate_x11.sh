#!/bin/bash
# run_mate_x11.sh — bring up Xorg (generic modesetting DDX) on the nvkvm virtual
# KMS head, start a MATE desktop + a GL app, screenshot the root window, and
# report the GL renderer (NVIDIA GPU vs llvmpipe). Run as root in the guest.
set -u
pkill -9 -x Xorg 2>/dev/null; pkill -9 mate-session 2>/dev/null; pkill -9 marco 2>/dev/null
sleep 1
# Allow X to start as root from a non-seat ssh session.
printf 'allowed_users=anybody\nneeds_root_rights=yes\n' > /etc/X11/Xwrapper.config
cat > /tmp/xorg-nvkvm.conf <<XCONF
Section "ServerFlags"
    Option "AutoAddGPU" "off"
EndSection
Section "Device"
    Identifier "nvkvm-head"
    Driver     "modesetting"
    Option     "kmsdev" "/dev/dri/card0"
EndSection
Section "Screen"
    Identifier "scr0"
    Device     "nvkvm-head"
EndSection
XCONF

rm -f /tmp/Xorg.0.log
X :0 -config /tmp/xorg-nvkvm.conf -logfile /tmp/Xorg.0.log vt3 >/tmp/x.out 2>&1 &
XPID=$!
sleep 5
if ! kill -0 "$XPID" 2>/dev/null; then
    echo "XORG DIED:"; grep -iE "\(EE\)|\(WW\)|fatal|abort|modeset|glamor|nvidia" /tmp/Xorg.0.log | tail -25; exit 1
fi
export DISPLAY=:0
echo "=== Xorg up; screen/modeset ==="
grep -iE "modeset|glamor|Output|connected|EGL|renderer|GL_|randr" /tmp/Xorg.0.log | tail -15

echo "=== GL renderer (glxinfo) ==="
glxinfo 2>/dev/null | grep -iE "OpenGL renderer|OpenGL vendor|direct rendering" | head

# Desktop + a GL app.
marco --replace >/tmp/marco.out 2>&1 &
sleep 1
mate-panel >/tmp/panel.out 2>&1 &
mate-terminal >/tmp/term.out 2>&1 &
glxgears -geometry 600x600+400+200 >/tmp/gears.out 2>&1 &
glmark2 --off-screen >/tmp/glmark2.out 2>&1 &
sleep 6

echo "=== screenshot ==="
import -window root /tmp/mate_desktop.png 2>/tmp/shot.err && ls -la /tmp/mate_desktop.png || cat /tmp/shot.err
echo "=== glxgears fps ==="; grep -iE "frames|FPS" /tmp/gears.out | tail -2
echo "=== glmark2 ==="; grep -iE "GL_RENDERER|glmark2 Score|^  GL" /tmp/glmark2.out | head -6

pkill -x glxgears 2>/dev/null; pkill -9 mate-session 2>/dev/null
pkill -x marco 2>/dev/null; pkill mate-panel 2>/dev/null; pkill mate-terminal 2>/dev/null
kill "$XPID" 2>/dev/null
