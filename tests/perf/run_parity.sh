#!/usr/bin/env bash
# run_parity.sh — host-vs-guest GPU parity benchmark + correctness harness.
#
# Runs the SAME self-contained workloads on the bare-metal host and inside the
# nvkvm guest VM, captures both in ONE invocation (never compares to a
# remembered number), checks byte-exact correctness, and prints a parity table
# with PASS/FAIL vs the thresholds in README.md.
#
# Methodology baked in (lessons paid for — see README.md §"Methodology"):
#   1. Host baseline captured in the SAME run.
#   2. Steady-state sampling: LLM tok/s is llama's own eval rate; GPU util is the
#      median of the decode tail (first 40% of samples dropped = load+prompt).
#   3. Guest RAM >= model: asserted before the LLM test; FAIL loudly otherwise.
#   4. Byte-exact correctness (dtoh_probe) alongside throughput.
#   5. Warm caches: gguf is cat'd into page cache before the timed load.
#   6. Each metric asserts a threshold vs the host run -> regression FAILS.
#
# The host and guest share ONE physical GPU (guest forwards to the host driver),
# so the two runs are STRICTLY serialized (see memory [[remote_test_serialization]]).
#
# Usage:
#   tests/perf/run_parity.sh                 # full run (compute + DMA + LLM)
#   tests/perf/run_parity.sh --no-llm        # skip the LLM decode test (fast)
# Env:
#   HOST_SSH (default "vh")   GUEST_SSH (default "vg")   — ~/.ssh/config aliases
set -uo pipefail

HOST_SSH="${HOST_SSH:-vh}"
GUEST_SSH="${GUEST_SSH:-vg}"
RUN_LLM=1
for a in "$@"; do [ "$a" = "--no-llm" ] && RUN_LLM=0; done

REPO="$(realpath "$(dirname "$0")/../..")"
PERF="$REPO/tests/perf"
INTEG="$REPO/tests/integration"
STAGE=/tmp/parity_src           # staging dir on each remote
OUT=$(mktemp -d)                # local: per-target metric files
trap 'rm -rf "$OUT"' EXIT

# Files the remote runner needs (self-contained probes + the runner itself).
FILES=("$INTEG/gpu_bench.c" "$PERF/dtoh_probe.c" "$PERF/launchstorm.c" "$PERF/parity_remote.sh")

stage_and_run() {  # $1=ssh-alias  $2=tag(host|guest)  -> writes $OUT/$2.m
    local alias="$1" tag="$2"
    echo "==> [$tag] staging probes to $alias:$STAGE"
    ssh -o LogLevel=ERROR "$alias" "rm -rf $STAGE && mkdir -p $STAGE" || { echo "  ssh $alias failed"; return 1; }
    # Pipe each source file's bytes directly into the staging dir (robust over
    # the ProxyJump'd guest connection; avoids 9p and rsync assumptions).
    for f in "${FILES[@]}"; do ssh "$alias" "cat > $STAGE/$(basename "$f")" < "$f"; done
    local llm_args="'' '' ''"
    [ "$RUN_LLM" = 1 ] && llm_args="AUTO AUTO AUTO"
    echo "==> [$tag] running workloads (serialized; shared GPU)"
    ssh -o LogLevel=ERROR "$alias" "bash $STAGE/parity_remote.sh $STAGE $llm_args $tag" \
        >"$OUT/$tag.m" 2>"$OUT/$tag.err"
    sed 's/^/    /' "$OUT/$tag.err"
}

# ── Serialized: host fully, then guest (never concurrent on the shared GPU) ──
stage_and_run "$HOST_SSH"  host  || { echo "host run failed"; exit 1; }
stage_and_run "$GUEST_SSH" guest || { echo "guest run failed"; exit 1; }

# ── Parse "M <key> <value>" lines into hv[]/gv[] ──
declare -A hv gv
while read -r m k v; do [ "$m" = M ] && hv[$k]="$v"; done < "$OUT/host.m"
while read -r m k v; do [ "$m" = M ] && gv[$k]="$v"; done < "$OUT/guest.m"

g(){ printf '%s' "${1:-}"; }   # safe getter

echo ""
echo "================ nvkvm host-vs-guest parity ================"
echo "host : ${hv[gpu]:-?}  driver ${hv[driver]:-?}  RAM avail ${hv[ram_avail_g]:-?}G"
echo "guest: ${gv[gpu]:-?}  driver ${gv[driver]:-?}  RAM avail ${gv[ram_avail_g]:-?}G"
echo ""

FAILED=0
# row <label> <key> <unit> <dir> <thresh> <note>
#   dir: hi = higher-is-better (guest/host ratio), lo = lower-is-better (guest/host),
#        eq = byte-exact string match against "OK"
#   thresh: for hi -> min ratio guest/host ; for lo -> max ratio guest/host
row() {
    local label="$1" key="$2" unit="$3" dir="$4" thr="$5" note="${6:-}"
    local h="${hv[$key]:-}" gg="${gv[$key]:-}"
    local verdict ratio="-"
    if [ -z "$h" ] || [ -z "$gg" ]; then
        verdict="SKIP"; [ -z "$h$gg" ] && verdict="n/a"
    elif [ "$dir" = eq ]; then
        if [ "$gg" = OK ] && [ "$h" = OK ]; then verdict="PASS"; else verdict="FAIL"; FAILED=1; fi
    elif [ "$dir" = info ]; then
        ratio=$(awk -v h="$h" -v g="$gg" 'BEGIN{if(h+0==0){print"-";exit} printf "%.2f", g/h}')
        verdict="info"
    else
        ratio=$(awk -v h="$h" -v g="$gg" 'BEGIN{if(h+0==0){print"-";exit} printf "%.2f", g/h}')
        if [ "$dir" = hi ]; then
            verdict=$(awk -v r="$ratio" -v t="$thr" 'BEGIN{print (r>=t)?"PASS":"FAIL"}')
        else
            verdict=$(awk -v r="$ratio" -v t="$thr" 'BEGIN{print (r<=t)?"PASS":"FAIL"}')
        fi
        [ "$verdict" = FAIL ] && FAILED=1
    fi
    printf "  %-22s host=%-9s guest=%-9s %-6s %-5s  %s\n" \
        "$label" "${h:-—}" "${gg:-—}" "${ratio:+x$ratio}" "$verdict" "$note"
}

echo "--- correctness (must pass) ---"
row "DtoH byte-exact"    correct       ""        eq 0   "HtoD->DtoH round-trip"
echo "--- compute / data path (expect ~parity) ---"
row "GEMM throughput"    gemm_gflops   GFLOP/s   hi 0.90 "2048^3 fp32"
row "DtoH bandwidth"     dtoh_gbs      GB/s      hi 0.80 "warm, cached (guard #94)"
row "HtoD bandwidth"     htod_gbs      GB/s      hi 0.80 "reused src"
[ "$RUN_LLM" = 1 ] && {
echo "--- end-to-end LLM decode (expect ~parity) ---"
row "LLM decode"         llm_tok_s     tok/s     hi 0.90 "Qwen2.5-7B Q4_K_M -ngl 99 (authoritative)"
row "  GPU util (decode)" llm_util_p50 %         info 0  "informational; guest nvidia-smi util unreliable [[get_pid_info_findings]]"
}
# Control path is a KNOWN forwarding tax (multi-hop ioctl RTT) — never been at
# parity. Thresholds here are REGRESSION TRIPWIRES set above the current
# baseline, not parity goals: they fire only if the tax roughly doubles.
echo "--- control path (known multi-hop ioctl tax; regression tripwire, not parity) ---"
row "launch+sync RTT"    launch_us     us        lo 4.0  "~1.8x baseline; trip if >4x"
row "alloc+free RTT"     alloc_us      us        lo 50.0 "~28x KNOWN tax — optimization target [[perf_host_vs_guest]]"
row "empty cuCtxSync"    empty_sync_us us        lo 3.0  "~parity; guard WB-sysmem #c5d5d8a"

# ── RAM methodology guard ──
echo ""
if [ "$RUN_LLM" = 1 ]; then
    model_g=5
    if [ -n "${gv[ram_avail_g]:-}" ] && [ "${gv[ram_avail_g]}" -lt "$model_g" ]; then
        echo "  !! METHODOLOGY: guest RAM avail ${gv[ram_avail_g]}G < ~${model_g}G model — model-load was disk-bound, LLM numbers SUSPECT"
        FAILED=1
    fi
fi

echo ""
if [ "$FAILED" = 0 ]; then
    echo "RESULT: PASS — guest at host parity within thresholds"
else
    echo "RESULT: FAIL — see rows marked FAIL above"
fi
echo "============================================================"
exit $FAILED
