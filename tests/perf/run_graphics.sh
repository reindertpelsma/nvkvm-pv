#!/usr/bin/env bash
# run_graphics.sh — graphics + media host-vs-guest parity (the DRI path).
# Exercises Vulkan (enumerate + compute), headless OpenGL render (EGL device
# platform), and NVDEC/NVENC video through nvkvm's forwarded /dev/dri/renderD128
# + /dev/nvidia*. Serial on the shared GPU. Companion to run_matrix.sh.
#
# Usage: tests/perf/run_graphics.sh      Env: HOST_SSH=vh GUEST_SSH=vg GATE=0.90
set -uo pipefail
HOST_SSH="${HOST_SSH:-vh}"; GUEST_SSH="${GUEST_SSH:-vg}"; GATE="${GATE:-0.90}"
REPO="$(realpath "$(dirname "$0")/../..")"; PERF="$REPO/tests/perf"
STAGE=/tmp/gfx_src; OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

stage_and_run(){ local al="$1" tag="$2"
    echo "==> [$tag] staging graphics to $al"
    ssh -o LogLevel=ERROR "$al" "rm -rf $STAGE && mkdir -p $STAGE/apps" || return 1
    ssh -o LogLevel=ERROR "$al" "cat > $STAGE/apps/egl_offscreen.c" < "$PERF/apps/egl_offscreen.c"
    ssh -o LogLevel=ERROR "$al" "cat > $STAGE/graphics_remote.sh" < "$PERF/graphics_remote.sh"
    echo "==> [$tag] running graphics/media (serial; shared GPU)"
    ssh -o LogLevel=ERROR "$al" "bash $STAGE/graphics_remote.sh $STAGE $tag" >"$OUT/$tag.m" 2>"$OUT/$tag.err"
    grep -E '^\[' "$OUT/$tag.err" | sed 's/^/    /'
}
stage_and_run "$HOST_SSH" host || exit 1
stage_and_run "$GUEST_SSH" guest || exit 1

declare -A hv gv hc gc
while read -r t k v; do [ "$t" = METRIC ]&&hv[$k]="$v"; [ "$t" = CHECK ]&&hc[$k]="$v"; done < "$OUT/host.m"
while read -r t k v; do [ "$t" = METRIC ]&&gv[$k]="$v"; [ "$t" = CHECK ]&&gc[$k]="$v"; done < "$OUT/guest.m"

echo ""; echo "============== nvkvm graphics / media parity =============="
printf "%-24s %10s %10s %7s %5s %s\n" "workload" "host" "guest" "ratio" "ok" "verdict"
echo "----------------------------------------------------------"
PASS=0; FAIL=0; NORUN=0
row(){ local label="$1" k="$2" h="${hv[$2]:-}" g="${gv[$2]:-}"
    # Same fix as run_matrix.sh: a workload with no metric on EITHER side used to
    # `return` and disappear from the table + the counts entirely.  For this
    # runner that was especially misleading — with no Vulkan ICD staged in the
    # guest, the vulkan rows simply were not printed and the table looked clean.
    if [ -z "$h$g" ]; then
        NORUN=$((NORUN+1))
        printf "%-24s %10s %10s %7s %5s %s\n" "$label" "—" "—" "" "-" "DID-NOT-RUN"
        return
    fi
    local r="-" v="FAIL" gck="${gc[$2]:-na}"
    if [ -n "$h" ]&&[ -n "$g" ]; then r=$(awk -v h="$h" -v g="$g" 'BEGIN{if(h+0==0){print"-";exit}printf"%.2f",g/h}')
        local m; m=$(awk -v r="$r" -v t="$GATE" 'BEGIN{print(r>=t)?1:0}')
        [ "$m" = 1 ]&&[ "$gck" != FAIL ]&&v="PASS"; fi
    [ "$v" = PASS ]&&PASS=$((PASS+1))||FAIL=$((FAIL+1))
    printf "%-24s %10s %10s %7s %5s %s\n" "$label" "${h:-—}" "${g:-—}" "${r:+x$r}" "$gck" "$v"; }
echo "── Vulkan ──"
row "device enumerate"   vk_enum_gpu
row "compute fp32 (vkpeak)" vkpeak_fp32_GFLOPs
echo "── OpenGL (headless) ──"
row "offscreen render"   egl_gl_Mtri_s
echo "── Video engines ──"
row "NVENC h264 encode"  nvenc_h264_fps
echo "----------------------------------------------------------"
echo "PASS: $PASS   FAIL: $FAIL   DID-NOT-RUN: $NORUN   (gate guest/host >= $GATE; correctness must pass)"
[ "$NORUN" != 0 ] && echo "RESULT: INCOMPLETE — $NORUN workload(s) produced no metric on either side"
echo "=========================================================="
[ "$FAIL" = 0 ] && [ "$NORUN" = 0 ]
