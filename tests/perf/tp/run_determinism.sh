#!/bin/bash
# run_determinism.sh — is tensor-parallel decode reproducible?  Two separate
# processes on the SAME side, so "the host disagrees with itself" is
# distinguishable from "the guest disagrees with the host".
set -u
SIDE="${1:-side}"
VENV=/opt/llm/venv/bin/python
MODEL=/opt/llm/models/Qwen2.5-7B-Instruct
HERE="$(cd "$(dirname "$0")" && pwd)"
export VLLM_WORKER_MULTIPROC_METHOD=spawn HF_HUB_OFFLINE=1 TOKENIZERS_PARALLELISM=false
[ "${NVKVM_SHM:-on}" = "off" ] && export NCCL_SHM_DISABLE=1
PIN=""; [ -n "${NVKVM_PIN_CPUS:-}" ] && PIN="taskset -c $NVKVM_PIN_CPUS"
for tp in ${2:-1 4}; do
  for g in graphs eager; do
    GF=""; [ "$g" = graphs ] && GF="--graphs"
    for proc in A B; do
      $PIN $VENV "$HERE/determinism.py" --model "$MODEL" --tp "$tp" $GF \
          --label "${SIDE}_${g}_p${proc}" --reps 3 ${NVKVM_DET_EXTRA:-} 2>&1 | grep -E "^DET"
    done
  done
done
