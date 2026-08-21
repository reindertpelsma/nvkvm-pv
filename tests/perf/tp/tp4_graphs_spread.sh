#!/bin/bash
# TP=4 + CUDA graphs, FOUR separate engine instantiations.  Two three-rep
# sessions on the guest disagreed by ~1.7x (588-648 vs 338-407 tok/s) with
# nothing changed between them, so the variance that matters is BETWEEN
# processes, not between repetitions inside one.
set -u
SIDE="${1:-side}"
VENV=/opt/llm/venv/bin/python
MODEL=/opt/llm/models/Qwen2.5-7B-Instruct
HERE="$(cd "$(dirname "$0")" && pwd)"
export VLLM_WORKER_MULTIPROC_METHOD=spawn HF_HUB_OFFLINE=1 TOKENIZERS_PARALLELISM=false
export OMP_NUM_THREADS=8
PIN=""; [ -n "${NVKVM_PIN_CPUS:-}" ] && PIN="taskset -c $NVKVM_PIN_CPUS"
for proc in A B C D; do
  $PIN $VENV "$HERE/tp_bench.py" --model "$MODEL" --tp 4 --graphs \
      --label "${SIDE}_p${proc}" --batch 8 --in-tokens 128 --out-tokens 128 \
      --reps 4 --warmups 2 2>&1 | grep -E "^M\|"
done
