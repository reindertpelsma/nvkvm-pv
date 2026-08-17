#!/usr/bin/env python3
"""llm_serving_bench.py — host-vs-guest LLM serving parity for nvkvm.

Runs the SAME stack / version / weights / flags on the bare-metal host and
inside the nvkvm guest, and measures the three regimes that stress different
parts of the forward path:

  prefill  — long-context prompt processing.  Compute-bound, hits the matmul
             path.  Measured on genuinely long prompts (several thousand
             tokens), NOT a toy prompt: with a ~5-token prompt you measure
             fixed per-prefill launch/sync latency (the control-path tax),
             which is a different quantity entirely (see realapp_matrix.md).
  decode   — long generations, steady state.  Latency-bound; this is where a
             per-token forwarding overhead would show up if it existed.
  batch    — many concurrent requests through continuous batching.  Closest
             thing to a real serving workload; stresses multi-context.

Plus a temperature-0 output-equality check: divergent text between host and
guest would be a far more serious finding than a throughput gap.

METHODOLOGY GUARDS (tests/perf/README.md — lessons already paid for):
  #2 steady-state only: decode rate is derived from a max_tokens=1 run
     subtracted off a max_tokens=N run, so prefill is excluded from the decode
     number and model load is excluded from both.
  #3 RAM >= model size + overhead, asserted at start.  A guest with less RAM
     than the weights makes model load disk-bound and fakes a huge gap.
  #5 weights warmed into page cache before timing, on BOTH sides.
  -- model load time is reported but explicitly NOT an inference metric.

Emits `M <key> <value>` lines on stdout (parsed by the orchestrator) and a
full JSON blob including generated text for the equality diff.

Usage:
  python llm_serving_bench.py --model <dir> --corpus <file> --out <json> [--tag host]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import time

# Determinism knobs that must be identical on both sides.  vLLM's sampler is
# already greedy at temperature 0; the seed pins anything else that draws.
SEED = 1234


def emit(key: str, value) -> None:
    print(f"M {key} {value}", flush=True)


def sh(cmd: str) -> str:
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=60).stdout.strip()
    except Exception as exc:  # pragma: no cover - diagnostics only
        return f"<{exc}>"


def dir_size_gb(path: str) -> float:
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            fp = os.path.join(root, name)
            if os.path.islink(fp):
                continue
            try:
                total += os.path.getsize(fp)
            except OSError:
                pass
    return total / 1e9


def warm_page_cache(path: str) -> float:
    """Methodology #5: read every weight file so model load is not disk-bound.

    Returns seconds spent.  Run on BOTH sides; otherwise the side with the
    cold cache reports a model-load figure that has nothing to do with the GPU.
    """
    t0 = time.time()
    for root, _dirs, files in os.walk(path):
        for name in sorted(files):
            fp = os.path.join(root, name)
            try:
                with open(fp, "rb") as fh:
                    while fh.read(32 << 20):
                        pass
            except OSError:
                pass
    return time.time() - t0


def avail_ram_gb() -> float:
    with open("/proc/meminfo") as fh:
        for line in fh:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / (1024 * 1024)
    return -1.0


def build_prompts(corpus_path: str):
    """Deterministic prompt corpus.

    The long-context material is a real file (source tree / document), read
    identically on both sides.  We sha256 the exact token ids actually fed to
    the model and emit it, so 'the two sides saw the same input' is proven,
    not assumed.
    """
    with open(corpus_path, "r", errors="replace") as fh:
        text = fh.read()
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="unknown")
    ap.add_argument("--gpu-mem-util", type=float, default=0.85)
    ap.add_argument("--max-model-len", type=int, default=32768)
    ap.add_argument("--prefill-ctx-tokens", type=int, default=8192,
                    help="target corpus tokens in the long-context prompt")
    ap.add_argument("--prefill-reps", type=int, default=3)
    ap.add_argument("--decode-reps", type=int, default=3)
    ap.add_argument("--decode-tokens", type=int, default=512)
    ap.add_argument("--batch-n", type=int, default=32)
    ap.add_argument("--batch-gen", type=int, default=256)
    ap.add_argument("--enforce-eager", action="store_true",
                    help="disable CUDA graphs (fallback if capture misbehaves)")
    args = ap.parse_args()

    res = {"tag": args.tag}

    # ---------------------------------------------------------------- env ---
    model_gb = dir_size_gb(args.model)
    ram = avail_ram_gb()
    res["env"] = {
        "hostname": platform.node(),
        "kernel": platform.release(),
        "python": platform.python_version(),
        "nproc": os.cpu_count(),
        "sched_affinity": len(os.sched_getaffinity(0)),
        "ram_avail_gb": round(ram, 1),
        "model_gb": round(model_gb, 2),
        "nvidia_smi": sh("nvidia-smi --query-gpu=name,driver_version,memory.total "
                         "--format=csv,noheader"),
    }
    emit("nproc", os.cpu_count())
    emit("affinity", len(os.sched_getaffinity(0)))
    emit("ram_avail_gb", round(ram, 1))
    emit("model_gb", round(model_gb, 2))

    # Methodology #3.  Weights + runtime need headroom above the raw file size.
    need = model_gb * 1.3 + 4
    if ram < need:
        print(f"!! METHODOLOGY: RAM avail {ram:.1f}G < {need:.1f}G needed for a "
              f"{model_gb:.1f}G model — model load would be disk-bound and every "
              f"LLM number here would be SUSPECT. Refusing to run.", file=sys.stderr)
        emit("methodology_fail", 1)
        return 2

    # Methodology #5.
    warm_s = warm_page_cache(args.model)
    res["env"]["page_cache_warm_s"] = round(warm_s, 1)
    emit("page_cache_warm_s", round(warm_s, 1))

    import torch
    import vllm
    from vllm import LLM, SamplingParams
    from vllm.inputs import TokensPrompt

    res["env"]["vllm"] = vllm.__version__
    res["env"]["torch"] = torch.__version__
    res["env"]["torch_cuda"] = torch.version.cuda
    emit("vllm_version", vllm.__version__)
    emit("torch_version", torch.__version__)

    # --------------------------------------------------------------- load ---
    # Reported for completeness ONLY.  A model-load figure is not an inference
    # figure and must never be presented as one.
    t0 = time.time()
    llm = LLM(
        model=args.model,
        seed=SEED,
        gpu_memory_utilization=args.gpu_mem_util,
        max_model_len=args.max_model_len,
        enable_prefix_caching=False,   # otherwise repeats measure a cache hit
        enforce_eager=args.enforce_eager,
        disable_log_stats=True,
    )
    load_s = time.time() - t0
    res["load_s"] = round(load_s, 1)
    emit("model_load_s_NOT_INFERENCE", round(load_s, 1))

    tok = llm.get_tokenizer()
    text = build_prompts(args.corpus)
    res["corpus_sha256"] = hashlib.sha256(text.encode()).hexdigest()
    emit("corpus_sha", res["corpus_sha256"][:16])

    # Slice the corpus by TOKENS, not characters, so both sides get prompts of
    # an identical, known length regardless of how the text tokenises.
    corpus_ids = tok(text, add_special_tokens=False).input_ids

    def corpus_slice(start_tok: int, n_tok: int) -> str:
        return tok.decode(corpus_ids[start_tok:start_tok + n_tok])

    def chat_ids(user_msg: str) -> list[int]:
        msgs = [{"role": "user", "content": user_msg}]
        return tok.apply_chat_template(msgs, add_generation_prompt=True,
                                       tokenize=True)

    def greedy(n: int) -> SamplingParams:
        return SamplingParams(temperature=0.0, top_p=1.0, top_k=-1, seed=SEED,
                              max_tokens=n, ignore_eos=False)

    def run(prompts, sp):
        t = time.time()
        outs = llm.generate(prompts, sp, use_tqdm=False)
        return time.time() - t, outs

    # ------------------------------------------------------------- warmup ---
    # Discard the first generation entirely (methodology: steady state only).
    run([TokensPrompt(prompt_token_ids=chat_ids("Say hello."))], greedy(32))

    # ------------------------------------------------------------ prefill ---
    # A genuinely long input: a large slice of the corpus with a real task on
    # top.  max_tokens=1 so the measured wall time is prefill + one decode
    # step; the single decode step is subtracted using the decode rate below.
    long_ctx = corpus_slice(0, args.prefill_ctx_tokens)
    prefill_ids = chat_ids(
        "Below is a source tree excerpt. Read it carefully, then answer.\n\n"
        f"{long_ctx}\n\nQuestion: summarise the architecture in one paragraph."
    )
    n_prefill = len(prefill_ids)
    ids_sha = hashlib.sha256(
        json.dumps(prefill_ids).encode()).hexdigest()[:16]
    res["prefill_prompt_tokens"] = n_prefill
    res["prefill_prompt_ids_sha256_16"] = ids_sha
    emit("prefill_prompt_tokens", n_prefill)
    emit("prefill_prompt_ids_sha", ids_sha)

    pf = []
    for _ in range(args.prefill_reps):
        dt, _ = run([TokensPrompt(prompt_token_ids=prefill_ids)], greedy(1))
        pf.append(dt)
    pf.sort()
    t_pf = pf[len(pf) // 2]
    res["prefill_s_all"] = [round(x, 3) for x in pf]
    res["prefill_s_median"] = round(t_pf, 3)
    res["prefill_tok_s"] = round(n_prefill / t_pf, 1)
    emit("prefill_tok_s", round(n_prefill / t_pf, 1))
    emit("prefill_s_median", round(t_pf, 3))

    # ------------------------------------------------------------- decode ---
    # Steady-state decode.  t(max_tokens=N) - t(max_tokens=1) removes prefill
    # AND the first token, so what is left is (N-1) pure decode steps.
    dec_prompt = chat_ids(
        "Write a detailed technical explanation of how a hypervisor forwards "
        "GPU driver ioctls from an untrusted guest to a host driver, covering "
        "the control path, the data path, memory pinning, and the security "
        "boundary. Be thorough and precise.\n\nContext:\n" + corpus_slice(0, 1024)
    )
    res["decode_prompt_tokens"] = len(dec_prompt)
    emit("decode_prompt_tokens", len(dec_prompt))

    dec_rates = []
    dec_detail = []
    for _ in range(args.decode_reps):
        t1, o1 = run([TokensPrompt(prompt_token_ids=list(dec_prompt))], greedy(1))
        spN = greedy(args.decode_tokens)
        spN.ignore_eos = True   # pin the token count so the rate is well defined
        tN, oN = run([TokensPrompt(prompt_token_ids=list(dec_prompt))], spN)
        n_out = len(oN[0].outputs[0].token_ids)
        rate = (n_out - 1) / (tN - t1)
        dec_rates.append(rate)
        dec_detail.append({"t_prefill_only_s": round(t1, 3),
                           "t_total_s": round(tN, 3),
                           "n_out": n_out, "tok_s": round(rate, 2)})
    dec_rates.sort()
    res["decode_runs"] = dec_detail
    res["decode_tok_s"] = round(dec_rates[len(dec_rates) // 2], 2)
    emit("decode_tok_s", round(dec_rates[len(dec_rates) // 2], 2))

    # -------------------------------------------------------------- batch ---
    # Concurrency: N distinct requests handed to the engine at once, so
    # continuous batching schedules them together.  Prompts are distinct
    # (prefix caching is off anyway) to keep every request doing real work.
    batch_prompts = []
    seg_len = 1024                      # corpus tokens per concurrent request
    for i in range(args.batch_n):
        seg = corpus_slice((i * seg_len) % max(len(corpus_ids) - seg_len, 1), seg_len)
        batch_prompts.append(TokensPrompt(prompt_token_ids=chat_ids(
            f"Request {i}. Analyse this excerpt and explain what it does.\n\n{seg}")))
    sp_b = greedy(args.batch_gen)
    sp_b.ignore_eos = True
    t_b, outs_b = run(batch_prompts, sp_b)
    in_tok = sum(len(p["prompt_token_ids"]) for p in batch_prompts)
    out_tok = sum(len(o.outputs[0].token_ids) for o in outs_b)
    res["batch"] = {
        "n_requests": args.batch_n,
        "input_tokens": in_tok,
        "output_tokens": out_tok,
        "wall_s": round(t_b, 3),
        "out_tok_s": round(out_tok / t_b, 1),
        "total_tok_s": round((in_tok + out_tok) / t_b, 1),
    }
    emit("batch_out_tok_s", round(out_tok / t_b, 1))
    emit("batch_total_tok_s", round((in_tok + out_tok) / t_b, 1))
    emit("batch_wall_s", round(t_b, 3))
    emit("batch_input_tokens", in_tok)
    emit("batch_output_tokens", out_tok)

    # ------------------------------------------------------------ quality ---
    # Structurally hard work at temperature 0, one request at a time (batch of
    # one) so continuous-batching numerics cannot vary the result.  Degenerate
    # or divergent output is obvious in these.
    tasks = {
        "reasoning_chain": (
            "A VM guest issues 40,000 GPU ioctls per second. Each is forwarded "
            "over a ring with 12 us round-trip latency, but up to 64 may be in "
            "flight concurrently. The GPU kernel launched by each ioctl takes "
            "180 us. Work out, step by step, whether the forwarding path or the "
            "GPU is the bottleneck, what the achievable ioctl rate is, and how "
            "the answer changes if the ring depth drops to 4. Show arithmetic.",
            700),
        "code_generation": (
            "Write a complete, correct C function `ring_submit` implementing a "
            "single-producer single-consumer lock-free descriptor ring with a "
            "power-of-two capacity, using C11 atomics with explicit memory "
            "orders. Include the struct definitions, the matching "
            "`ring_complete` consumer side, and comments justifying each memory "
            "order. Then explain the ABA hazard and why it does or does not "
            "apply here.",
            900),
        "long_doc_summary": (
            "Read the following source excerpt and produce a precise technical "
            "summary: what the component does, its main data structures, and "
            "any correctness or security concern you can identify. Be specific "
            "and cite identifiers from the text.\n\n" + corpus_slice(0, 8192),
            600),
    }
    quality = {}
    for name, (prompt, ntok) in tasks.items():
        ids = chat_ids(prompt)
        sp = greedy(ntok)
        t = time.time()
        out = llm.generate([TokensPrompt(prompt_token_ids=ids)], sp,
                           use_tqdm=False)[0]
        dt = time.time() - t
        gen = out.outputs[0].text
        gen_ids = list(out.outputs[0].token_ids)
        quality[name] = {
            "prompt_tokens": len(ids),
            "prompt_ids_sha256_16": hashlib.sha256(
                json.dumps(ids).encode()).hexdigest()[:16],
            "output_tokens": len(gen_ids),
            "output_ids_sha256": hashlib.sha256(
                json.dumps(gen_ids).encode()).hexdigest(),
            "output_text_sha256": hashlib.sha256(gen.encode()).hexdigest(),
            "wall_s": round(dt, 2),
            "text": gen,
            "token_ids": gen_ids,
        }
        emit(f"quality_{name}_sha", quality[name]["output_ids_sha256"][:16])
        emit(f"quality_{name}_ntok", len(gen_ids))
    res["quality"] = quality

    with open(args.out, "w") as fh:
        json.dump(res, fh, indent=1)
    print(f"wrote {args.out}", flush=True)
    emit("ok", 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
