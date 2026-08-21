#!/bin/bash
# run_matrix.sh — the tensor-parallel scaling matrix, run identically on the
# bare-metal host and inside the guest.  Both sides read the SAME read-only
# payload image (venv + weights), so the software stack is not a variable.
#
# Usage: run_matrix.sh <side-label> [tp-list] [extra tp_bench args...]
set -u
SIDE="${1:-side}"; shift || true
TPS="${1:-1 2 4}"; shift || true

VENV=/opt/llm/venv/bin/python
MODEL=/opt/llm/models/Qwen2.5-7B-Instruct
HERE="$(cd "$(dirname "$0")" && pwd)"

# NCCL's shared-memory transport: on by default now that the guest can use it.
# NVKVM_SHM=off reproduces the old crippled configuration on BOTH sides.
if [ "${NVKVM_SHM:-on}" = "off" ]; then export NCCL_SHM_DISABLE=1; fi
export VLLM_WORKER_MULTIPROC_METHOD=spawn
export HF_HUB_OFFLINE=1
export TOKENIZERS_PARALLELISM=false
export VLLM_LOGGING_LEVEL=INFO
export OMP_NUM_THREADS=8

# Host runs are pinned to the same vCPU count the guest was given, so "the host
# had 98 cores and the guest 32" is not silently part of the answer.
PIN=""
if [ -n "${NVKVM_PIN_CPUS:-}" ]; then PIN="taskset -c $NVKVM_PIN_CPUS"; fi

for tp in $TPS; do
  for g in eager graphs; do
    GF=""; [ "$g" = graphs ] && GF="--graphs"
    echo "### RUN side=$SIDE tp=$tp mode=$g $(date -Is)"
    $PIN $VENV "$HERE/tp_bench.py" --model "$MODEL" --tp "$tp" $GF \
        --label "${SIDE}_${g}" --batch 8 --in-tokens 128 --out-tokens 128 \
        --reps 3 --warmups 1 --dump-sample "$@" 2>&1 \
      | grep -E "^(M\||LOAD\||WARM\||SAMPLE\||ERROR|Traceback)" 
    echo "### END side=$SIDE tp=$tp mode=$g rc=$?"
  done
done
