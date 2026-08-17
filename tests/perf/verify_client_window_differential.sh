#!/bin/bash
# verify_client_window_differential.sh — prove the CLIENT's window is drawing,
# not just that the desktop has pixels in it.
#
# "100% non-black" on a weston capture proves nothing: weston's default desktop
# background is a full-screen gradient, so a capture is ~100% non-black even
# with no client at all, and even with a client whose window is solid black.
# The only conclusive evidence is DIFFERENTIAL:
#   A = capture with NO client            (desktop background alone)
#   B = capture with the client running   (A + the client's window)
#   C = capture 1s after B                (client still animating)
# A vs B non-zero  => the client's window contributed pixels.
# B vs C non-zero  => the client is actively rendering new frames, not showing
#                     one stale/black buffer.
# Run as root inside the guest.
set -u
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
W=${W:-1920}; H=${H:-1080}
CLIENT=${CLIENT:-glmark2-wayland}
LOG=/tmp/wh.log

pkill -9 -x weston 2>/dev/null
pkill -9 -x glmark2-wayland 2>/dev/null; pkill -9 -x es2gears_wayland 2>/dev/null
sleep 2
rm -f "$LOG" /tmp/*screenshot*.png /tmp/client.out /tmp/capA.png /tmp/capB.png /tmp/capC.png

weston --backend=headless-backend.so --renderer=gl --width="$W" --height="$H" \
       --idle-time=0 --debug --log="$LOG" >/tmp/wh.out 2>&1 &
WPID=$!
sleep 6
kill -0 "$WPID" 2>/dev/null || { echo "COMPOSITOR: DIED"; tail -20 "$LOG"; exit 1; }
grep -iE "GL renderer" "$LOG" | head -1

SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
export WAYLAND_DISPLAY="$(basename "${SOCK:-wayland-1}")"

snap(){ # $1 destination
    rm -f /tmp/*screenshot*.png
    (cd /tmp && timeout 10 weston-screenshooter 2>/dev/null)
    local s; s=$(ls -t /tmp/*screenshot*.png 2>/dev/null | head -1)
    [ -n "$s" ] && mv "$s" "$1"
}

echo "=== A: baseline, NO client ==="
snap /tmp/capA.png; ls -l /tmp/capA.png 2>/dev/null || echo "  (no capture)"

echo "=== starting client: $CLIENT ==="
case "$CLIENT" in
    glmark2-wayland) glmark2-wayland --run-forever >/tmp/client.out 2>&1 & ;;
    *)               "$CLIENT" >/tmp/client.out 2>&1 & ;;
esac
CPID=$!
sleep 12
kill -0 "$CPID" 2>/dev/null && echo "client: ALIVE" || echo "client: EXITED EARLY"
grep -iE "GL_RENDERER" /tmp/client.out | head -1

echo "=== B, C: with client (1s apart) ==="
snap /tmp/capB.png
sleep 1
snap /tmp/capC.png

python3 - <<'PY'
import zlib, struct, os

def load(p):
    d = open(p,'rb').read()
    pos, idat, w, h, ctype = 8, b'', 0, 0, 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
        if typ == b'IHDR': w,h,_,ctype = struct.unpack('>IIBB', d[pos+8:pos+18])
        elif typ == b'IDAT': idat += d[pos+8:pos+8+ln]
        pos += 12+ln
    raw = zlib.decompress(idat)
    nch = {0:1,2:3,4:2,6:4}[ctype]; stride = w*nch
    out = bytearray(); prev = bytearray(stride); i = 0
    for y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-nch] if x>=nch else 0
            b = prev[x]; c = prev[x-nch] if x>=nch else 0
            if   f==1: line[x] = (line[x]+a)&255
            elif f==2: line[x] = (line[x]+b)&255
            elif f==3: line[x] = (line[x]+(a+b)//2)&255
            elif f==4:
                pp=a+b-c; pa,pb,pc=abs(pp-a),abs(pp-b),abs(pp-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x] = (line[x]+pr)&255
        out += line; prev = line
    return w,h,nch,bytes(out)

def diff(p,q,la,lb):
    if not (os.path.exists(p) and os.path.exists(q)):
        print(f"  {la} vs {lb}: MISSING CAPTURE"); return
    w,h,n,A = load(p); _,_,_,B = load(q)
    if len(A)!=len(B): print("  size mismatch"); return
    # count differing pixels and locate the bounding box of the change
    nd=0; minx=w; maxx=0; miny=h; maxy=0
    stride=w*n
    for y in range(0,h,2):
        row=y*stride
        for x in range(0,w,2):
            o=row+x*n
            if abs(A[o]-B[o])>8 or abs(A[o+1]-B[o+1])>8 or abs(A[o+2]-B[o+2])>8:
                nd+=1
                if x<minx:minx=x
                if x>maxx:maxx=x
                if y<miny:miny=y
                if y>maxy:maxy=y
    tot=(w//2)*(h//2)
    print(f"  {la} vs {lb}: {nd}/{tot} sampled px differ ({100.0*nd/tot:.2f}%)", end="")
    if nd: print(f"  changed-region bbox=({minx},{miny})-({maxx},{maxy}) {maxx-minx+1}x{maxy-miny+1}")
    else:  print("  -> IDENTICAL")

print("=== DIFFERENTIAL ===")
diff('/tmp/capA.png','/tmp/capB.png','A(no client)','B(client)')
diff('/tmp/capB.png','/tmp/capC.png','B(client)','C(client+1s)')
PY

echo "=== client fps ==="
grep -iE "fps|score" /tmp/client.out | tail -6
pkill -9 -x glmark2-wayland 2>/dev/null; pkill -9 -x es2gears_wayland 2>/dev/null
kill "$WPID" 2>/dev/null
