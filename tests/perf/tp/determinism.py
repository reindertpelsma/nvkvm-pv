#!/usr/bin/env python3
"""
determinism.py — is a tensor-parallel decode reproducible at all?

A host-vs-guest text mismatch only means something if the host agrees with
ITSELF.  Tensor-parallel all-reduce is a floating-point reduction whose order
NCCL is free to vary with the transport, the channel count and the ring it
builds, so the same binary on the same machine can legitimately disagree run to
run.  This measures that directly:

  * three repetitions inside ONE process (same engine, same graphs)
  * the whole run is repeatable across processes by invoking it twice

Greedy sampling, fixed prompt token ids, ignore_eos -> any difference is the
model's numerics, not sampling.
"""
import argparse, hashlib, json, os, sys


def h(ids):
    return hashlib.sha256(json.dumps(list(ids)).encode()).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--tp", type=int, default=1)
    ap.add_argument("--graphs", action="store_true")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--batch", type=int, default=4)
    ap.add_argument("--in-tokens", type=int, default=64)
    ap.add_argument("--out-tokens", type=int, default=64)
    ap.add_argument("--label", default="run")
    ap.add_argument("--real", action="store_true",
                    help="natural-language prompts instead of synthetic token ids; "
                         "random ids sit near logit ties and exaggerate any numeric "
                         "difference, so the realistic case has to be checked too")
    a = ap.parse_args()

    from vllm import LLM, SamplingParams
    llm = LLM(model=a.model, tensor_parallel_size=a.tp, dtype="bfloat16",
              max_model_len=2048, gpu_memory_utilization=0.85,
              enforce_eager=not a.graphs, trust_remote_code=True,
              disable_log_stats=True, seed=0)

    if a.real:
        prompts = [
            "Explain in two sentences what a race condition is.",
            "def quicksort(arr):\n",
            "Write a Python function that reverses a linked list.\n",
            "Explain what a GPU tensor-parallel all-reduce does.\n",
        ][:a.batch]
    else:
        prompts = [{"prompt_token_ids": [1000 + ((i * 37 + j * 13) % 5000)
                                         for j in range(a.in_tokens)]}
                   for i in range(a.batch)]
    sp = SamplingParams(temperature=0.0, max_tokens=a.out_tokens, ignore_eos=True)

    for rep in range(a.reps):
        outs = llm.generate(prompts, sp, use_tqdm=False)
        per = [h(o.outputs[0].token_ids) for o in outs]
        allh = hashlib.sha256("".join(per).encode()).hexdigest()[:16]
        print("DET|%s|tp=%d|graphs=%d|rep=%d|all=%s|per=%s"
              % (a.label, a.tp, int(a.graphs), rep, allh, ",".join(per)), flush=True)
        if rep == 0:
            print("DETIDS|%s|tp=%d|%s"
                  % (a.label, a.tp, json.dumps(list(outs[0].outputs[0].token_ids))),
                  flush=True)


if __name__ == "__main__":
    main()
