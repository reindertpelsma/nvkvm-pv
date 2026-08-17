#!/usr/bin/env python3
"""compare_llm_parity.py — diff two llm_serving_bench.py result JSONs.

    python compare_llm_parity.py host.json guest.json

Prints the parity table (guest/host ratio per metric) and the temperature-0
output-equality verdict.  Exit code 1 if any throughput gate fails or if the
generated text diverges.
"""
from __future__ import annotations

import difflib
import json
import sys

GATE = 0.90   # guest/host ratio required, same gate the rest of tests/perf uses

THROUGHPUT = [
    ("prefill_tok_s", "prefill (long ctx)", "tok/s"),
    ("decode_tok_s", "decode (steady state)", "tok/s"),
]
BATCH = [
    ("out_tok_s", "batch output", "tok/s"),
    ("total_tok_s", "batch total (in+out)", "tok/s"),
]


def main() -> int:
    host = json.load(open(sys.argv[1]))
    guest = json.load(open(sys.argv[2]))
    failed = []

    print("== environment ==")
    for key in ("nvidia_smi", "vllm", "torch", "python", "kernel", "nproc",
                "sched_affinity", "ram_avail_gb", "model_gb",
                "page_cache_warm_s"):
        print(f"  {key:18s} host={host['env'].get(key)!s:45s} "
              f"guest={guest['env'].get(key)}")
    print(f"  {'corpus_sha256':18s} host={host.get('corpus_sha256','')[:16]!s:45s} "
          f"guest={guest.get('corpus_sha256','')[:16]}")
    same_corpus = host.get("corpus_sha256") == guest.get("corpus_sha256")
    print(f"  corpus identical   : {same_corpus}")
    if not same_corpus:
        failed.append("corpus differs between sides — comparison invalid")

    for key in ("prefill_prompt_ids_sha256_16", "prefill_prompt_tokens",
                "decode_prompt_tokens"):
        if host.get(key) != guest.get(key):
            failed.append(f"{key} differs: {host.get(key)} vs {guest.get(key)}")

    print("\n== model load (NOT an inference metric) ==")
    print(f"  host {host['load_s']}s   guest {guest['load_s']}s")

    print("\n== throughput ==")
    print(f"  {'metric':26s} {'host':>10s} {'guest':>10s} {'ratio':>7s}  gate")
    rows = []
    for key, label, unit in THROUGHPUT:
        rows.append((label, unit, host[key], guest[key]))
    for key, label, unit in BATCH:
        rows.append((label, unit, host["batch"][key], guest["batch"][key]))
    for label, unit, h, g in rows:
        ratio = g / h if h else 0.0
        ok = ratio >= GATE
        print(f"  {label:26s} {h:10.1f} {g:10.1f} {ratio:6.2f}x  "
              f"{'PASS' if ok else 'FAIL'}  ({unit})")
        if not ok:
            failed.append(f"{label} ratio {ratio:.2f} < {GATE}")

    print("\n== batch detail ==")
    for side, blob in (("host", host), ("guest", guest)):
        b = blob["batch"]
        print(f"  {side:5s} n={b['n_requests']} in={b['input_tokens']} "
              f"out={b['output_tokens']} wall={b['wall_s']}s")

    print("\n== output equality at temperature 0 ==")
    for name in sorted(host["quality"]):
        h = host["quality"][name]
        g = guest["quality"].get(name)
        if g is None:
            failed.append(f"quality/{name} missing on guest")
            continue
        exact = h["output_ids_sha256"] == g["output_ids_sha256"]
        if exact:
            print(f"  {name:20s} IDENTICAL  ({h['output_tokens']} tokens, "
                  f"sha {h['output_ids_sha256'][:16]})")
            continue
        hi, gi = h["token_ids"], g["token_ids"]
        common = 0
        for a, b in zip(hi, gi):
            if a != b:
                break
            common += 1
        sim = difflib.SequenceMatcher(None, h["text"], g["text"]).ratio()
        print(f"  {name:20s} DIVERGES   host={len(hi)} guest={len(gi)} tokens, "
              f"identical prefix={common} tok, text similarity={sim:.4f}")
        failed.append(f"quality/{name} diverged after {common} tokens")

    print("\n== verdict ==")
    if failed:
        for f in failed:
            print(f"  FAIL: {f}")
        return 1
    print("  all throughput gates pass and all temperature-0 outputs identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
