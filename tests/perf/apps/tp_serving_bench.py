#!/usr/bin/env python3
"""tp_serving_bench.py — tensor-parallel serving throughput, host vs guest.

The 6x A4000 multi-GPU row in tests/BOOT_MATRIX.md needs a number that is
comparable across the host/guest boundary, so this script is deliberately
minimal and fully deterministic: the SAME file runs on both sides, the prompts
are fixed, sampling is greedy, and `ignore_eos` pins the generated token count
so tok/s is a rate over a known numerator rather than over whatever the model
felt like emitting.

It also prints a sha256 over the generated text. Throughput without that is not
enough: the point of the multi-GPU row is that the guest is *correct* as well as
fast, and a sharded model that quietly produces different text would otherwise
look like a win.

    python tp_serving_bench.py --model <dir> --tp 6 --enforce-eager
    python tp_serving_bench.py --model <dir> --tp 6            # CUDA graphs
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import time

PROMPTS = [
    "def quicksort(arr):",
    "def binary_search(a, target):",
    "class LRUCache:",
    "def fibonacci(n):",
    "def merge_sort(items):",
    "def matrix_multiply(a, b):",
    "def is_prime(n):",
    "def reverse_linked_list(head):",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--tp", type=int, default=6)
    ap.add_argument("--max-tokens", type=int, default=256)
    ap.add_argument("--gpu-mem-util", type=float, default=0.90)
    ap.add_argument("--max-model-len", type=int, default=2048)
    ap.add_argument("--enforce-eager", action="store_true")
    ap.add_argument("--tag", default="unknown")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    from vllm import LLM, SamplingParams

    t_load = time.time()
    llm = LLM(
        model=args.model,
        tensor_parallel_size=args.tp,
        dtype="bfloat16",
        gpu_memory_utilization=args.gpu_mem_util,
        max_model_len=args.max_model_len,
        enforce_eager=args.enforce_eager,
    )
    load_s = time.time() - t_load

    sp = SamplingParams(temperature=0.0, max_tokens=args.max_tokens,
                        ignore_eos=True)

    # Warm-up: the first generate pays graph capture / autotune costs that are
    # not part of steady-state serving and differ between the two sides.
    llm.generate(PROMPTS, SamplingParams(temperature=0.0, max_tokens=16,
                                         ignore_eos=True))

    t0 = time.time()
    outs = llm.generate(PROMPTS, sp)
    elapsed = time.time() - t0

    gen_tokens = sum(len(o.outputs[0].token_ids) for o in outs)
    texts = "".join(o.outputs[0].text for o in
                    sorted(outs, key=lambda o: PROMPTS.index(o.prompt)))
    digest = hashlib.sha256(texts.encode()).hexdigest()

    res = {
        "tag": args.tag,
        "tp": args.tp,
        "enforce_eager": args.enforce_eager,
        "load_s": round(load_s, 1),
        "gen_tokens": gen_tokens,
        "elapsed_s": round(elapsed, 3),
        "tok_s": round(gen_tokens / elapsed, 2),
        "sha256": digest,
        "nccl_shm_disable": os.environ.get("NCCL_SHM_DISABLE", "unset"),
    }
    print("RESULT " + json.dumps(res))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(res, f, indent=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
