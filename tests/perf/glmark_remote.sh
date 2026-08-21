#!/bin/bash
# glmark_remote.sh — run the SAME glmark2 build on host and guest, under the
# SAME headless weston, and emit per-scene FPS so the aggregate score can be
# decomposed.  Companion to glmark_compare.sh.
#
#   bash glmark_remote.sh <tag>        tag = host | guest
#
# Emits, on stdout, lines of the form:
#   METRIC <key> <value>
#   SCENE  <config> <scene-with-options> <fps>
# and leaves raw glmark2 output in /tmp/glmark_<tag>_<config>.txt
set -u
TAG="${1:-unknown}"
GM_DIR="${GM_DIR:-/opt/glmark2}"
GM="$GM_DIR/bin/glmark2-wayland"
export GLMARK2_DATA_PATH="$GM_DIR/share/glmark2"
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

# ── bring up a headless weston (no KMS, no physical display needed) ───────
start_weston() {
    for p in $(pgrep -x weston); do kill -9 "$p"; done 2>/dev/null
    sleep 1
    rm -f /tmp/weston_$TAG.log
    # weston >= 10 spells it --renderer=gl; weston 9 spells it --use-gl.  The
    # host container is 22.04 (weston 9) and the guest cloud image is 24.04
    # (weston 13), so pick per side rather than hardcoding.
    local GLOPT=--renderer=gl
    weston --help 2>&1 | grep -q -- '--use-gl' && GLOPT=--use-gl
    weston --backend=headless-backend.so "$GLOPT" \
           --width=1920 --height=1080 --idle-time=0 \
           --log=/tmp/weston_$TAG.log >/tmp/weston_$TAG.out 2>&1 &
    WPID=$!
    for i in $(seq 1 30); do
        SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
        [ -n "$SOCK" ] && break
        sleep 1
    done
    [ -z "${SOCK:-}" ] && { echo "[$TAG] weston did not come up" >&2; tail -20 /tmp/weston_$TAG.log >&2; return 1; }
    export WAYLAND_DISPLAY="$(basename "$SOCK")"
    echo "[$TAG] weston up pid=$WPID display=$WAYLAND_DISPLAY" >&2
    grep -iE "GL renderer|GL version|EGL vendor" /tmp/weston_$TAG.log | sed "s/^/[$TAG] /" >&2
    return 0
}

# ── one glmark2 configuration ─────────────────────────────────────────────
# $1 config name, rest = glmark2 args
run_cfg() {
    local cfg="$1"; shift
    local out=/tmp/glmark_${TAG}_${cfg}.txt
    echo "[$TAG] === $cfg : $* ===" >&2
    /usr/bin/time -f "WALL %e USER %U SYS %S MAXRSS %M" -o /tmp/gmtime_${TAG}_${cfg}.txt \
        "$GM" "$@" >"$out" 2>&1
    local score
    score=$(grep -E '^\s*glmark2 Score:' "$out" | grep -oE '[0-9]+' | tail -1)
    [ -n "$score" ] && echo "METRIC ${cfg}_score $score"
    # per-scene: "[build] use-vbo=false: FPS: 3200 FrameTime: 0.312 ms"
    grep -oE '^\[[a-z0-9:=.,-]+\][^:]*: FPS: [0-9]+' "$out" | while read -r line; do
        local sc fps
        sc=$(echo "$line" | sed -E 's/^\[([^]]*)\](.*): FPS: [0-9]+$/\1|\2/' | tr -d ' ')
        fps=$(echo "$line" | grep -oE '[0-9]+$')
        echo "SCENE $cfg ${sc} $fps"
    done
    if [ -f /tmp/gmtime_${TAG}_${cfg}.txt ]; then
        awk -v c="$cfg" '/WALL/{print "METRIC "c"_wall "$2; print "METRIC "c"_cpu_user "$4; print "METRIC "c"_cpu_sys "$6}' \
            /tmp/gmtime_${TAG}_${cfg}.txt
    fi
    sed -n '1,12p' "$out" | sed "s/^/[$TAG] /" >&2
}

start_weston || exit 1
sleep 2

echo "[$TAG] clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)" >&2

# 1. The number to quote: the FULL default suite, off-screen (no presentation).
#    Quote this one, never a single scene -- see cold_vs_warm below for why.
run_cfg suite_offscreen --off-screen
# 2. The full suite windowed, presented through the compositor.
run_cfg suite_windowed
# 3. Cold-vs-warm: the SAME scene four times in one process.  In the guest the
#    first repeat lands ~0.37x of host and every later one 0.88-0.93x, so a
#    one-scene invocation measures the cold path and nothing else.
run_cfg cold_vs_warm    --off-screen -b build -b build -b build -b build
# 4. Resolution sweep on one scene: same draw calls, more pixels.  A ratio that
#    climbs toward 1.0 with resolution means per-frame CPU cost, not per-pixel.
run_cfg res_800         --off-screen --size 800x600   -b build -b build
run_cfg res_1920        --off-screen --size 1920x1080 -b build -b build
run_cfg res_3840        --off-screen --size 3840x2160 -b build -b build

for p in $(pgrep -x weston); do kill -9 "$p"; done 2>/dev/null
echo "[$TAG] done" >&2
