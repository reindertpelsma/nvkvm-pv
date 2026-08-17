# Raw results — LLM serving parity, 2026-08-17

Produced by `tests/perf/apps/llm_serving_bench.py`; analysed in
[`../../llm_parity.md`](../../llm_parity.md).  RTX 6000 Ada 48 GB, driver
575.51.03, vLLM 0.11.0 / torch 2.8.0+cu128, Qwen2.5-32B-Instruct-AWQ.
Host and guest ran strictly serially on the one physical GPU.

| file pair | configuration |
|---|---|
| `*_llm.json`   | Pass A — default: 8245-tok prefill, 512-tok decode, 32 concurrent, CUDA graphs on |
| `*_hard.json`  | Pass B — 16384-tok prefill context, 1024-tok decode, 64 concurrent, CUDA graphs on |
| `*_eager.json` | Pass C — `--enforce-eager` (CUDA graphs OFF), 256-tok decode, 8 concurrent |

Each file carries the environment block (driver, vCPU affinity, available RAM,
page-cache warm time), the per-repetition timings behind every median, the
corpus and prompt-token-id hashes that prove both sides saw identical input,
and the full generated text plus token-id sha256 for the three
temperature-0 quality tasks.

Compare a pair with:

    python tests/perf/compare_llm_parity.py host_llm.json guest_llm.json
