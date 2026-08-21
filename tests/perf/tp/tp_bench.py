#!/usr/bin/env python3
"""
tp_bench.py — vLLM tensor-parallel throughput, identical script on host and
guest, with the methodology guards this project has already paid for:

  * a cold pass is discarded (a guest's first pass can be 2.7x its steady state)
  * prompts are fixed *token* sequences, so prefill is byte-identical on both
    sides and not a tokenizer difference
  * ignore_eos, so every sequence emits exactly --out-tokens and the token count
    is not itself a variable
  * every repetition is printed, not just an average, so a single slow rep is
    visible instead of averaged in

Emits one machine-readable M| line per repetition.
"""
import argparse, json, os, sys, time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--tp", type=int, default=1)
    ap.add_argument("--graphs", action="store_true")
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--in-tokens", type=int, default=128)
    ap.add_argument("--out-tokens", type=int, default=128)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--warmups", type=int, default=1)
    ap.add_argument("--label", default="run")
    ap.add_argument("--max-model-len", type=int, default=2048)
    ap.add_argument("--gpu-mem-util", type=float, default=0.85)
    ap.add_argument("--disable-custom-ar", action="store_true")
    ap.add_argument("--dump-sample", action="store_true")
    a = ap.parse_args()

    from vllm import LLM, SamplingParams

    t0 = time.time()
    kw = dict(model=a.model, tensor_parallel_size=a.tp, dtype="bfloat16",
              max_model_len=a.max_model_len, gpu_memory_utilization=a.gpu_mem_util,
              enforce_eager=not a.graphs, trust_remote_code=True,
              disable_log_stats=True)
    if a.disable_custom_ar:
        kw["disable_custom_all_reduce"] = True
    llm = LLM(**kw)
    load_s = time.time() - t0
    print("LOAD|%s|tp=%d|seconds=%.1f" % (a.label, a.tp, load_s), flush=True)

    # Deterministic synthetic prompts of an exact token length.  Token ids are
    # chosen from a fixed low range that every tokenizer in scope maps to real
    # tokens, so the two sides feed the model the same thing without depending
    # on tokenizer version.
    prompts = [{"prompt_token_ids": [1000 + ((i * 37 + j * 13) % 5000)
                                     for j in range(a.in_tokens)]}
               for i in range(a.batch)]
    sp = SamplingParams(temperature=0.0, max_tokens=a.out_tokens, ignore_eos=True)

    for w in range(a.warmups):
        t = time.time()
        llm.generate(prompts, sp, use_tqdm=False)
        print("WARM|%s|tp=%d|rep=%d|seconds=%.3f" % (a.label, a.tp, w, time.time() - t),
              flush=True)

    for rep in range(a.reps):
        t = time.time()
        outs = llm.generate(prompts, sp, use_tqdm=False)
        dt = time.time() - t
        ntok = sum(len(o.outputs[0].token_ids) for o in outs)
        print("M|%s|tp=%d|graphs=%d|batch=%d|in=%d|out=%d|rep=%d|seconds=%.4f|tokens=%d|tok_per_s=%.3f"
              % (a.label, a.tp, int(a.graphs), a.batch, a.in_tokens, a.out_tokens,
                 rep, dt, ntok, ntok / dt), flush=True)

    if a.dump_sample:
        ids = list(outs[0].outputs[0].token_ids)
        print("SAMPLE|%s|tp=%d|%s" % (a.label, a.tp, json.dumps(ids)), flush=True)


if __name__ == "__main__":
    main()
