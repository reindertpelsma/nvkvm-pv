#!/usr/bin/env bash
# matrix_remote.sh — runs ON a target (host OR guest). Builds + runs the whole
# real-app matrix and emits machine-readable lines to stdout:
#   METRIC <key> <value>      CHECK <key> <ok|FAIL>
# Human-facing text goes to stderr. Each section is guarded so one failure does
# not abort the rest. Identical on host and guest -> a true parity comparison.
#
# Args: $1 SRCDIR (holds apps/)   $2 VENV (python or AUTO)   $3 TAG (host|guest)
#       $4 MODEL (AUTO|"")  $5 LLAMA (AUTO|"")  $6 LLAMALIB (AUTO|"")
set -uo pipefail
SRC="${1:?srcdir}"; VENV="${2:-AUTO}"; TAG="${3:-?}"
MODEL="${4:-AUTO}"; LLAMA="${5:-AUTO}"; LLAMALIB="${6:-AUTO}"
[ "$VENV" = AUTO ] && VENV="$HOME/aienv/bin/python"
[ "$MODEL" = AUTO ] && MODEL="$HOME/q7.gguf"
[ "$LLAMA" = AUTO ] && LLAMA="$HOME/llm/llama-cli"
[ "$LLAMALIB" = AUTO ] && LLAMALIB="$HOME/llm/lib"
log(){ echo "[$TAG] $*" >&2; }

# ───────────────────────── CUDA compute suite (native nvcc build) ────────────
log "=== CUDA compute suite ==="
B=/tmp/mat_apps; mkdir -p "$B/bin"; cp "$SRC"/apps/*.cu "$B/" 2>/dev/null
( cd "$B"
  for f in *.cu; do n=${f%.cu}; ex=""
    case $n in sgemm_cublas) ex="-lcublas";; fft_cufft) ex="-lcufft";; esac
    nvcc -O3 -arch=sm_86 -o bin/$n $f $ex 2>/tmp/$n.be || { echo "[$TAG] BUILD FAIL $n" >&2; }
  done )
for b in stream_triad reduce nbody blackscholes mandelbrot conv2d sgemm_cublas fft_cufft sha256 memcpy2d; do
    [ -x "$B/bin/$b" ] || { log "missing $b"; continue; }
    out=$(timeout 90 "$B/bin/$b" 2>&1); echo "$out" | grep -E '^METRIC|^CHECK'
    log "$b: $(echo "$out"|tr '\n' ' ')"
done

# ───────────────────────── PyTorch AI suite ─────────────────────────────────
log "=== PyTorch AI suite ==="
if [ -x "$VENV" ]; then
    "$VENV" "$SRC/apps/ai_bench.py" 2>>/tmp/ai_$TAG.err | grep -E '^METRIC|^CHECK'
    log "ai stderr: $(tail -1 /tmp/ai_$TAG.err 2>/dev/null)"
else log "no venv python at $VENV"; fi

# ───────────────────────── gpu-burn (stability + sustained FLOPS) ────────────
log "=== gpu-burn ==="
GB="$HOME/bench/gpu-burn"
if [ ! -x "$GB/gpu_burn" ]; then
    mkdir -p "$HOME/bench"; ( cd "$HOME/bench"; [ -d gpu-burn ] || git clone -q --depth=1 https://github.com/wilicc/gpu-burn.git; cd gpu-burn && make >/tmp/gb_$TAG.log 2>&1 ); fi
if [ -x "$GB/gpu_burn" ]; then
    out=$(cd "$GB"; timeout 60 ./gpu_burn 20 2>&1)
    gf=$(echo "$out" | grep -oE '[0-9]+ Gflop/s' | grep -oE '^[0-9]+' | sort -n | tail -1)
    err=$(echo "$out" | grep -oE 'errors: [0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)
    [ -n "$gf" ] && echo "METRIC gpu_burn_GFLOPs $gf"
    echo "CHECK gpu_burn_GFLOPs $([ "${err:-1}" = 0 ] && echo ok || echo FAIL)"
    log "gpu_burn: peak=${gf:-?} Gflop/s errors=${err:-?}"
else log "gpu_burn build failed"; fi

# ───────────────────────── llama.cpp LLM (decode + prefill) ──────────────────
log "=== llama.cpp 7B ==="
if [ -s "$MODEL" ] && [ -x "$LLAMA" ]; then
    cat "$MODEL" >/dev/null 2>&1   # warm page cache
    LLOG=/tmp/llama_$TAG.log
    # Long prompt (~1800 tok) so prefill t/s measures COMPUTE throughput, not the
    # fixed per-prefill launch latency a tiny prompt would expose (that tax is
    # measured separately by run_parity.sh's launch RTT). Decode t/s is prompt-
    # length-independent, so one run yields both meaningful numbers.
    PR=""; for i in $(seq 1 120); do PR="$PR The quick brown fox jumps over the lazy dog while GPU virtualization forwards ioctls across the hypervisor boundary efficiently."; done
    LD_LIBRARY_PATH="$LLAMALIB" "$LLAMA" -m "$MODEL" -ngl 99 -c 4096 -n 200 -st \
        -p "$PR" >"$LLOG" 2>&1 || log "llama rc=$?"
    gen=$(grep -oE 'Generation: *[0-9.]+ *t/s' "$LLOG" | grep -oE '[0-9.]+' | tail -1)
    pre=$(grep -oE 'Prompt: *[0-9.]+ *t/s'     "$LLOG" | grep -oE '[0-9.]+' | tail -1)
    [ -n "$gen" ] && echo "METRIC llm_decode_tok_s $gen"
    [ -n "$pre" ] && echo "METRIC llm_prefill_tok_s $pre"
    echo "CHECK llm_decode_tok_s $([ -n "$gen" ] && echo ok || echo FAIL)"
    log "llama: prefill=${pre:-?} decode=${gen:-?} t/s"
else log "no model/llama ($MODEL / $LLAMA)"; fi

log "done"
