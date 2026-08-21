#!/bin/bash
# Firm up the TP=2/4 ratio: more warmups and more repetitions, because the
# three-rep runs showed a guest ramp (371 -> 588 -> 648 tok/s) that a single
# warmup did not absorb.
set -u
SIDE="${1:-side}"
VENV=/opt/llm/venv/bin/python
MODEL=/opt/llm/models/Qwen2.5-7B-Instruct
HERE="$(cd "$(dirname "$0")" && pwd)"
export VLLM_WORKER_MULTIPROC_METHOD=spawn HF_HUB_OFFLINE=1 TOKENIZERS_PARALLELISM=false
export OMP_NUM_THREADS=8
[ "${NVKVM_SHM:-on}" = "off" ] && export NCCL_SHM_DISABLE=1
PIN=""; [ -n "${NVKVM_PIN_CPUS:-}" ] && PIN="taskset -c $NVKVM_PIN_CPUS"
for tp in 2 4; do
  for g in eager graphs; do
    GF=""; [ "$g" = graphs ] && GF="--graphs"
    $PIN $VENV "$HERE/tp_bench.py" --model "$MODEL" --tp "$tp" $GF \
        --label "${SIDE}_${g}" --batch 8 --in-tokens 128 --out-tokens 128 \
        --reps 6 --warmups 3 2>&1 | grep -E "^(M\||WARM\|)"
  done
done
