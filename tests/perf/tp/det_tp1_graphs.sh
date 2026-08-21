#!/bin/bash
# Narrow follow-up: TP=1 + CUDA graphs only, many repetitions.  TP=1 has no
# collective at all, so anything non-reproducible here is NOT tensor-parallel
# reduction order.
set -u
SIDE="${1:-side}"
VENV=/opt/llm/venv/bin/python
MODEL=/opt/llm/models/Qwen2.5-7B-Instruct
HERE="$(cd "$(dirname "$0")" && pwd)"
export VLLM_WORKER_MULTIPROC_METHOD=spawn HF_HUB_OFFLINE=1 TOKENIZERS_PARALLELISM=false
PIN=""; [ -n "${NVKVM_PIN_CPUS:-}" ] && PIN="taskset -c $NVKVM_PIN_CPUS"
for proc in A B C D; do
  $PIN $VENV "$HERE/determinism.py" --model "$MODEL" --tp 1 --graphs --real \
      --label "${SIDE}_p${proc}" --reps 5 2>&1 | grep -E "^DET"
done
