#!/usr/bin/env bash
# parity_remote.sh — runs ON a target (host OR guest). Builds the self-contained
# probes, runs the SAME workloads, and emits machine-readable metric lines to
# stdout:  "M <key> <value>".  Everything human-facing goes to stderr so the
# orchestrator (run_parity.sh) can parse stdout cleanly.
#
# This script is identical on host and guest by design — that is what makes the
# comparison a true parity test (same binary, same args, same workload).
#
# Args:
#   $1  SRC      dir holding gpu_bench.c dtoh_probe.c launchstorm.c
#   $2  MODEL    gguf path for the LLM decode test ("" to skip)
#   $3  LLAMA    llama-cli path ("" to skip)
#   $4  LLAMALIB LD_LIBRARY_PATH for llama-cli ("" if none)
#   $5  TAG      "host" or "guest" (labels only)
set -uo pipefail
SRC="${1:?srcdir}"; MODEL="${2:-}"; LLAMA="${3:-}"; LLAMALIB="${4:-}"; TAG="${5:-?}"

# "AUTO" → resolve against this target's $HOME (host=/root, guest=/home/ubuntu).
[ "$MODEL"    = AUTO ] && MODEL="$HOME/q7.gguf"
[ "$LLAMA"    = AUTO ] && LLAMA="$HOME/llm/llama-cli"
[ "$LLAMALIB" = AUTO ] && LLAMALIB="$HOME/llm/lib"

log(){ echo "[$TAG] $*" >&2; }
emit(){ printf 'M %s %s\n' "$1" "$2"; }

BUILD=/tmp/parity_build; mkdir -p "$BUILD"; : >"$BUILD/build.log"

# ── locate CUDA include (probes that #include <cuda.h> need it on the host) ──
CUINC=""
for d in /usr/local/cuda/include /usr/local/cuda-12.0/include /usr/include; do
    [ -f "$d/cuda.h" ] && { CUINC="-I$d"; break; }
done
log "cuda include: ${CUINC:-<default>}"

# gpu_bench is self-contained (dlopen libcuda, embedded PTX) — no headers.
gcc -O2 -o "$BUILD/gpu_bench"  "$SRC/gpu_bench.c"  -ldl   2>>"$BUILD/build.log" && log "built gpu_bench"  || log "BUILD FAIL gpu_bench"
gcc -O2 $CUINC -o "$BUILD/dtoh"  "$SRC/dtoh_probe.c"  -lcuda 2>>"$BUILD/build.log" && log "built dtoh_probe"  || log "BUILD FAIL dtoh_probe"
gcc -O2 $CUINC -o "$BUILD/lstorm" "$SRC/launchstorm.c" -lcuda 2>>"$BUILD/build.log" && log "built launchstorm" || log "BUILD FAIL launchstorm"
[ -s "$BUILD/build.log" ] && { log "build warnings/errors:"; sed 's/^/[build] /' "$BUILD/build.log" >&2; }

# ── environment facts (methodology guards) ──
emit gpu        "$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 | tr ' ' '_')"
emit driver     "$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)"
emit ram_avail_g "$(free -g | awk '/Mem:/{print $7}')"

# ── gpu_bench: GEMM GFLOP/s (A), launch+sync RTT (B), alloc+free RTT (C) ──
if [ -x "$BUILD/gpu_bench" ]; then
    out=$("$BUILD/gpu_bench" 2048 20 2000 2000 2>>"$BUILD/run.log") || log "gpu_bench rc=$?"
    log "gpu_bench:"; echo "$out" | sed 's/^/    /' >&2
    echo "$out" | awk '
      /^A throughput/ {for(i=1;i<=NF;i++) if($i=="GFLOP/s") print "M gemm_gflops "$(i-1)}
      /^B launch RTT/ {for(i=1;i<=NF;i++) if($i=="us/launch") print "M launch_us "$(i-1)}
      /^C alloc RTT/  {for(i=1;i<=NF;i++) if($i=="us/pair") print "M alloc_us "$(i-1)}'
fi

# ── dtoh_probe: warm DtoH GB/s, HtoD GB/s, byte-exact correctness ──
if [ -x "$BUILD/dtoh" ]; then
    out=$("$BUILD/dtoh" 2>>"$BUILD/run.log") || log "dtoh rc=$?"
    log "dtoh_probe:"; echo "$out" | sed 's/^/    /' >&2
    echo "$out" | awk '
      /warm DtoH/    {print "M dtoh_gbs "$4}
      /^HtoD:/       {print "M htod_gbs "$2}
      /^correctness/ {print "M correct "$2}'
fi

# ── launchstorm: pure empty cuCtxSynchronize cost (completion-poll path) ──
if [ -x "$BUILD/lstorm" ]; then
    out=$("$BUILD/lstorm" 2>>"$BUILD/run.log") || log "launchstorm rc=$?"
    log "launchstorm:"; echo "$out" | sed 's/^/    /' >&2
    echo "$out" | awk '
      /empty sync/ {for(i=1;i<=NF;i++){if($i ~ /us\/sync/){v=$(i-1); gsub(/[()]/,"",v); print "M empty_sync_us "v}}}'
fi

# ── LLM decode: tok/s (llama-cli own eval rate) + GPU util sampled during decode ──
if [ -n "$MODEL" ] && [ -n "$LLAMA" ] && [ -s "$MODEL" ] && [ -x "$LLAMA" ]; then
    # Warm the model file into page cache so load is not disk-bound (methodology #5).
    cat "$MODEL" >/dev/null 2>&1 || true
    UL="$BUILD/util.log"; : >"$UL"
    # Background util sampler (best-effort; guest nvidia-smi may not report util).
    ( for _ in $(seq 1 400); do
        nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>/dev/null
        sleep 0.25
      done ) >"$UL" 2>/dev/null &
    SAMP=$!
    LLOG="$BUILD/llama.log"
    LD_LIBRARY_PATH="$LLAMALIB" "$LLAMA" -m "$MODEL" -ngl 99 -c 2048 -n 200 -st \
        -p "Explain in two sentences why GPU virtualization is useful for cloud computing." \
        >"$LLOG" 2>&1 || log "llama rc=$?"
    kill "$SAMP" 2>/dev/null; wait "$SAMP" 2>/dev/null
    log "llama timings:"; grep -iE 'Generation:|eval time|tokens per second' "$LLOG" | sed 's/^/    /' >&2
    # decode tok/s. New llama.cpp: "[ Prompt: X t/s | Generation: Y t/s ]".
    # Old format fallback: the "eval time" line that is NOT "prompt eval".
    tps=$(grep -oE 'Generation: *[0-9.]+ *t/s' "$LLOG" | grep -oE '[0-9.]+' | tail -1)
    [ -z "$tps" ] && tps=$(grep -iE 'eval time' "$LLOG" | grep -vi 'prompt' | grep -oE '[0-9.]+ tokens per second' | grep -oE '^[0-9.]+' | tail -1)
    [ -n "$tps" ] && emit llm_tok_s "$tps"
    # GPU util: drop the first 40% of samples (model load + prompt eval = GPU
    # idle/bursty) and report the median of the steady-state decode tail.
    # (sort-based: no gawk asort dependency — targets default to mawk.)
    nsamp=$(grep -c . "$UL" 2>/dev/null || echo 0)
    if [ "${nsamp:-0}" -ge 5 ]; then
        start=$(( nsamp * 40 / 100 ))
        util_p50=$(grep . "$UL" | tail -n +$((start+1)) | sort -n | awk '{a[n++]=$1} END{if(n)print a[int(n/2)]}')
        [ -n "$util_p50" ] && emit llm_util_p50 "$util_p50"
    fi
fi

log "done"
