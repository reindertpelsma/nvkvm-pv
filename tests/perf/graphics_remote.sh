#!/usr/bin/env bash
# graphics_remote.sh — runs ON a target. Exercises the GRAPHICS / media paths
# that nvkvm forwards (DRI render node /dev/dri/renderD128, Vulkan ICD, NVDEC/
# NVENC video engines) and emits METRIC/CHECK lines. Headless throughout — no X.
#
#   Vulkan enumerate : vulkaninfo must report the RTX 3060 (not llvmpipe/none)
#   Vulkan compute   : vkpeak fp32 GFLOPS (pure compute, no surface)
#   OpenGL render    : egl_offscreen via EGL device platform -> FBO (real GPU GL)
#   Video decode     : ffmpeg NVDEC (h264_cuvid) fps
#   Video encode     : ffmpeg NVENC (h264_nvenc) fps
#
# Args: $1 SRCDIR (holds apps/)   $2 TAG (host|guest)
set -uo pipefail
SRC="${1:?srcdir}"; TAG="${2:-?}"
log(){ echo "[$TAG] $*" >&2; }
B=/tmp/gfx; mkdir -p "$B"

# ── Vulkan device enumeration (proves the ICD binds the GPU through the DRI node) ──
log "=== vulkan enumerate ==="
if command -v vulkaninfo >/dev/null; then
    dev=$(vulkaninfo 2>/dev/null | grep -m1 -iE 'deviceName' | sed 's/.*= *//')
    log "vulkan deviceName: ${dev:-<none>}"
    if echo "$dev" | grep -qiE 'nvidia|rtx|geforce'; then echo "METRIC vk_enum_gpu 1"; echo "CHECK vk_enum_gpu ok"
    else echo "METRIC vk_enum_gpu 0"; echo "CHECK vk_enum_gpu FAIL"; fi
else log "no vulkaninfo"; fi

# ── Vulkan compute throughput: vkpeak (build from source if absent) ──
log "=== vulkan compute (vkpeak) ==="
VP="$HOME/bench/vkpeak/build/vkpeak"
if [ ! -x "$VP" ]; then
    ( mkdir -p "$HOME/bench"; cd "$HOME/bench"; [ -d vkpeak ] || git clone -q --depth=1 https://github.com/nihui/vkpeak.git
      cd vkpeak && git submodule update --init --recursive -q 2>/dev/null
      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/tmp/vkpeak_$TAG.log 2>&1 && cmake --build build -j4 >>/tmp/vkpeak_$TAG.log 2>&1 ) || true
fi
if [ -x "$VP" ]; then
    out=$("$VP" 0 2>&1); log "vkpeak: $(echo "$out"|grep -iE 'fp32|device'|tr '\n' ' ')"
    fp32=$(echo "$out" | grep -iE 'fp32-scalar' | grep -oE '[0-9.]+ GFLOPS' | grep -oE '[0-9.]+' | head -1)
    [ -n "$fp32" ] && { echo "METRIC vkpeak_fp32_GFLOPs $fp32"; echo "CHECK vkpeak_fp32_GFLOPs ok"; }
else log "vkpeak unavailable"; fi

# ── OpenGL offscreen render (EGL device platform -> FBO) ──
log "=== opengl offscreen (egl) ==="
if gcc -O2 "$SRC/apps/egl_offscreen.c" -o "$B/egl_offscreen" -lEGL -lGLESv2 2>/tmp/egl_$TAG.be; then
    out=$(timeout 60 "$B/egl_offscreen" 2>>/tmp/egl_$TAG.be); echo "$out" | grep -E '^METRIC|^CHECK'
    log "egl: $(echo "$out"|tr '\n' ' ') | $(grep -i renderer /tmp/egl_$TAG.be|tail -1)"
else log "egl build failed: $(head -1 /tmp/egl_$TAG.be)"; fi

# ── Video transcode: NVDEC decode + NVENC encode (GPU video engines) ──
# Every ffmpeg call is timeout-wrapped — cuvid/-f null can hang, and a hang must
# become a FAIL (missing metric), never wedge the harness or the GPU.
log "=== ffmpeg NVENC/NVDEC ==="
if command -v ffmpeg >/dev/null; then
    SRCMP4="$B/src.mp4"
    # -pix_fmt yuv420p IS LOAD-BEARING.  testsrc + libx264 defaults produce
    # yuv444p, and NVDEC accepts 4:2:0 only -- h264_cuvid then refuses the file
    # with "Codec h264_cuvid is not supported with this chroma format" before
    # decoding a frame.  That refusal was read as "NVDEC is broken on the host
    # too", which is why the decode row below was deleted.  MEASURED 2026-09-05.
    [ -s "$SRCMP4" ] || timeout 60 ffmpeg -y -hide_banner -f lavfi -i testsrc=size=1920x1080:rate=30:duration=10 \
        -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$SRCMP4" >/tmp/ffsrc_$TAG.log 2>&1
    # NVENC encode: synthetic frames -> h264_nvenc.  -benchmark -> "fps=" summary.
    enc=$(timeout 90 ffmpeg -y -hide_banner -benchmark -f lavfi -i testsrc=size=1920x1080:rate=30:duration=20 \
        -c:v h264_nvenc -preset p1 -f null - 2>&1 | grep -oE 'fps= *[0-9.]+' | tail -1 | grep -oE '[0-9.]+')
    [ -n "$enc" ] && { echo "METRIC nvenc_h264_fps $enc"; echo "CHECK nvenc_h264_fps ok"; } || log "nvenc failed/timeout"
    # NVDEC decode.  Excluded for weeks on the belief that cuvid "produces 0
    # frames and hangs on the bare-metal host too".  It does not: the source was
    # 4:4:4 and cuvid takes 4:2:0, so it failed FAST (rc=234) rather than
    # hanging.  MEASURED 2026-09-05, RTX 4070, driver 595.84, same 300-frame
    # 1080p 4:2:0 source: host h264_cuvid 33.3x, guest 13.3x -- both decoded all
    # 300 frames.  An excluded row cannot catch a regression, so it is back.
    dec=$(timeout 90 ffmpeg -y -hide_banner -benchmark -c:v h264_cuvid -i "$SRCMP4" \
        -f null - 2>&1 | grep -oE 'speed= *[0-9.]+' | tail -1 | grep -oE '[0-9.]+')
    [ -n "$dec" ] && { echo "METRIC nvdec_h264_speed $dec"; echo "CHECK nvdec_h264_speed ok"; } || log "nvdec failed/timeout"
    log "ffmpeg: nvenc=${enc:-?} fps  nvdec=${dec:-?}x"
else log "no ffmpeg"; fi

log "done"
