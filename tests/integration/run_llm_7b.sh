#!/usr/bin/env bash
# run_llm_7b.sh — #27 milestone: 7B+ LLM inference fully on the GPU through the
# nvkvm forwarder, from inside the untrusted guest VM.
#
# Proven 2026-05-30 on an RTX 3060 (12 GB): Qwen2.5-7B-Instruct Q4_K_M, all
# layers offloaded (-ngl 99), generated a coherent answer at ~20 tok/s
# (prompt ~280 tok/s) — vs ~2-4 tok/s on CPU, so the throughput is real GPU
# compute carried over the guest→virtio→QEMU→isolate→host-driver path. VRAM
# returned to 0 MiB on exit (no leak); a follow-up matmul stayed green.
#
# Prereqs in the guest (already set up on the test image):
#   - CUDA-enabled llama.cpp at ~/llm/llama-cli with libs in ~/llm/lib
#   - the nvkvm-guest.ko module loaded
# Model is pulled guest-local (NOT via 9p — a 9p read of the 4.4 GB file hits
# EIO; download direct or scp to local disk). Needs ~5 GB free on the rootfs.
set -euo pipefail

MODEL="${1:-$HOME/q7.gguf}"
URL=https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf
PROMPT="${2:-Explain in two sentences why GPU virtualization is useful for cloud computing.}"

if [ ! -s "$MODEL" ]; then
    echo "fetching 7B model to $MODEL ..."
    curl -fL "$URL" -o "$MODEL"
fi

export LD_LIBRARY_PATH="$HOME/llm/lib"
# -ngl 99: all transformer layers in VRAM.  -st: single-turn (this build's
# llama-cli does not accept -no-cnv; -st gives non-interactive completion).
exec "$HOME/llm/llama-cli" -m "$MODEL" -ngl 99 -c 2048 -n 120 -st -p "$PROMPT"
