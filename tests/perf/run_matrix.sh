#!/usr/bin/env bash
# run_matrix.sh — real-application host-vs-guest parity MATRIX.
#
# Runs a broad set of real GPU workloads (CUDA compute, PyTorch AI, gpu-burn,
# LLM) on the bare-metal host and inside the nvkvm guest, STRICTLY SERIALLY
# (one shared physical GPU), checks correctness, and prints a categorized
# parity table with PASS/FAIL per app vs a parity gate.
#
# Companion to run_parity.sh (microbench). This one is the "real apps" matrix.
# Graphics (DRI/Vulkan) apps live in run_graphics.sh (separate display path).
#
# Usage:   tests/perf/run_matrix.sh [--no-llm]
# Env:     HOST_SSH=vh  GUEST_SSH=vg   GATE=0.90   (guest/host parity threshold)
set -uo pipefail
HOST_SSH="${HOST_SSH:-vh}"; GUEST_SSH="${GUEST_SSH:-vg}"; GATE="${GATE:-0.90}"
RUN_LLM=1; for a in "$@"; do [ "$a" = "--no-llm" ] && RUN_LLM=0; done

REPO="$(realpath "$(dirname "$0")/../..")"; PERF="$REPO/tests/perf"
STAGE=/tmp/matrix_src
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

stage_and_run(){ # $1 alias  $2 tag
    local al="$1" tag="$2"
    echo "==> [$tag] staging matrix to $al:$STAGE"
    ssh -o LogLevel=ERROR "$al" "rm -rf $STAGE && mkdir -p $STAGE/apps" || { echo "ssh $al failed"; return 1; }
    for f in "$PERF"/apps/*; do ssh -o LogLevel=ERROR "$al" "cat > $STAGE/apps/$(basename "$f")" < "$f"; done
    ssh -o LogLevel=ERROR "$al" "cat > $STAGE/matrix_remote.sh" < "$PERF/matrix_remote.sh"
    local llm="'' '' ''"; [ "$RUN_LLM" = 1 ] && llm="AUTO AUTO AUTO"
    echo "==> [$tag] running matrix (serial; shared GPU) — this takes a few min"
    ssh -o LogLevel=ERROR "$al" "bash $STAGE/matrix_remote.sh $STAGE AUTO $tag $llm" \
        >"$OUT/$tag.m" 2>"$OUT/$tag.err"
    grep -E '^\[' "$OUT/$tag.err" | sed 's/^/    /'
}

stage_and_run "$HOST_SSH"  host  || exit 1
stage_and_run "$GUEST_SSH" guest || exit 1

# parse METRIC <k> <v> and CHECK <k> <ok|FAIL>
declare -A hv gv hc gc
while read -r t k v; do [ "$t" = METRIC ]&&hv[$k]="$v"; [ "$t" = CHECK ]&&hc[$k]="$v"; done < "$OUT/host.m"
while read -r t k v; do [ "$t" = METRIC ]&&gv[$k]="$v"; [ "$t" = CHECK ]&&gc[$k]="$v"; done < "$OUT/guest.m"

echo ""; echo "================= nvkvm real-app parity matrix ================="
printf "%-26s %12s %12s %7s %6s %s\n" "workload" "host" "guest" "ratio" "ok" "verdict"
echo "----------------------------------------------------------------------"
PASS=0; FAIL=0; N=0; NORUN=0
row(){ # $1 label  $2 key  $3 unit
    local label="$1" k="$2" u="$3" h="${hv[$2]:-}" g="${gv[$2]:-}"
    # A workload that produced no metric on EITHER side used to `return` here —
    # so it vanished from the table AND from the N/PASS/FAIL counts, and the run
    # still ended in "RESULT: PASS".  A missing nvcc, a build failure on both
    # sides, or a typo'd metric key were all indistinguishable from a clean
    # sweep: the report simply got shorter.  Print it as DID-NOT-RUN instead and
    # count it, so "did not run" can never again read as "passed".
    if [ -z "$h$g" ]; then
        NORUN=$((NORUN+1))
        printf "%-26s %12s %12s %7s %6s %s\n" "$label" "—" "—" "" "-" "DID-NOT-RUN"
        return
    fi
    N=$((N+1))
    local ratio="-" v="FAIL" okc="-"
    local hck="${hc[$2]:-na}" gck="${gc[$2]:-na}"
    okc="$gck"
    if [ -n "$h" ] && [ -n "$g" ]; then
        ratio=$(awk -v h="$h" -v g="$g" 'BEGIN{if(h+0==0){print"-";exit}printf"%.2f",g/h}')
        local meets; meets=$(awk -v r="$ratio" -v t="$GATE" 'BEGIN{print(r>=t)?1:0}')
        if [ "$meets" = 1 ] && [ "$gck" != FAIL ] && [ "$hck" != FAIL ]; then v="PASS"; else v="FAIL"; fi
    fi
    [ "$v" = PASS ] && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
    printf "%-26s %12s %12s %7s %6s %s\n" "$label" "${h:-—}" "${g:-—}" "${ratio:+x$ratio}" "$okc" "$v"
}
echo "── CUDA compute ──"
row "memory BW (triad)"    stream_triad_GBs        GB/s
row "reduction BW"         reduce_GBs              GB/s
row "N-body (gravity)"     nbody_GFLOPs            GFLOP/s
row "Black-Scholes"        blackscholes_Mopts      Mopt/s
row "Mandelbrot"           mandelbrot_Mpix_iter_s  Mpix-it/s
row "2D convolution"       conv2d_GFLOPs           GFLOP/s
row "SGEMM (cuBLAS)"       sgemm_cublas_TFLOPs     TFLOP/s
row "FFT (cuFFT)"          fft_cufft_GFLOPs        GFLOP/s
row "SHA-256 (crypto)"     sha256_MHs              MH/s
row "cudaMemcpy2D (pitched)" memcpy2d_GBs          GB/s
row "gpu-burn (sustained)" gpu_burn_GFLOPs         GFLOP/s
echo "── PyTorch AI ──"
row "matmul fp32"          torch_matmul_fp32_TFLOPs TFLOP/s
row "matmul fp16 (TC)"     torch_matmul_fp16_TFLOPs TFLOP/s
row "ResNet-50 infer"      resnet50_infer_imgs      img/s
row "ResNet-50 infer AMP"  resnet50_infer_amp_imgs  img/s
row "ResNet-50 train"      resnet50_train_imgs      img/s
row "ViT-B/16 infer"       vit_b16_infer_imgs       img/s
row "BERT enc infer"       bert_infer_seqs          seq/s
[ "$RUN_LLM" = 1 ] && { echo "── LLM (llama.cpp 7B) ──"
row "Qwen2.5-7B decode"    llm_decode_tok_s         tok/s
row "Qwen2.5-7B prefill"   llm_prefill_tok_s        tok/s; }
echo "----------------------------------------------------------------------"
echo "apps compared: $N   PASS: $PASS   FAIL: $FAIL   DID-NOT-RUN: $NORUN   (parity gate guest/host >= $GATE)"
echo "================================================================"
if [ "$FAIL" != 0 ]; then echo "RESULT: $FAIL below gate (see FAIL rows)"
elif [ "$NORUN" != 0 ]; then echo "RESULT: INCOMPLETE — $NORUN workload(s) produced no metric on either side"
else echo "RESULT: PASS"; fi
# Non-zero for both "below gate" and "did not run": an incomplete matrix is not
# a passing matrix.
[ "$FAIL" = 0 ] && [ "$NORUN" = 0 ]
