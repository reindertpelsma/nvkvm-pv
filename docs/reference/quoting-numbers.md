# Quoting numbers in public copy

A checklist, not an essay. If a number isn't listed here, don't quote it —
check [`known-limitations.md#numbers-you-should-not-quote`](../internal/known-limitations.md#numbers-you-should-not-quote)
and [`parity.md`](parity.md) first, or ask.

## Safe to quote — with the scope attached

Every row below needs its scope stated alongside the number, not dropped.

| number | required scope | source |
|---|---|---|
| Geekbench GPU **99.6%** (RTX 4070) | bare metal both sides; published, independently checkable | [parity.md](parity.md), [compare](https://browser.geekbench.com/v7/gpu/compare/87004?baseline=87011) |
| Geekbench GPU **99.9%** (RTX 3050 Laptop) | bare metal both sides; published | [parity.md](parity.md) |
| clpeak compute/bandwidth **~100%**, 7 of 8 subtests | bare metal, RTX 4070 only; published on OpenBenchmarking | [openbenchmarking-clpeak.md](openbenchmarking-clpeak.md) |
| clpeak kernel launch latency **2.13x** (cost, not a win) | same run as above — quote as a cost, never drop the "x" or imply it's a throughput number | [openbenchmarking-clpeak.md](openbenchmarking-clpeak.md) |
| Blender Cycles GPU render time **99.95%** | bare metal both sides, RTX 4070; self-reported (no anonymous submission path for this launcher) | [blender-opendata.md](blender-opendata.md) |
| vLLM serving **0.99–1.00x** (prefill/decode/batch) | single GPU, TP=1, RTX 6000 Ada, nested (both sides sit inside a rented VM); pinned host buffers were off on both sides at measurement time | [`tests/perf/llm_parity.md`](../../tests/perf/llm_parity.md) |
| Multi-GPU TP=1/2/4 eager **0.89–0.97x** | 4x RTX 4090, no NVLink; flat/orderly, the safe part of that table | [parity.md](parity.md) |

## Do not quote

| number | why not |
|---|---|
| **"795 fps glmark2"** | uncited, from a document with a known transcription error, explicitly unconfirmed on the current driver |
| **2026-06-01 perf matrix** (any row) | different toolkit versions per side (host cuBLAS/cuFFT 11.5 vs guest 12.x) — not apples to apples. Use the 2026-08-17 re-validation instead |
| **SGEMM / FFT / gpu-burn / llama.cpp LLM rows** | not re-run in the re-validation pass; only valid for driver 580.159.04, which we no longer stand behind as current |
| **A100 / H100 Geekbench numbers as "bare metal"** | both rented boxes turned out to be hypervisor guests themselves — the "host" column is already a VM. Fine to quote as nested-guest evidence, never as a bare-metal claim |
| **TP=4 with CUDA graphs, 0.52x** (as a headline) | real, disclosed, but bimodal and unexplained — quoting it alone without the "mode not a ceiling" context misrepresents it either direction |
| **NVENC hardware encode "works"** | most recent re-test didn't reproduce the original hang, but that's "doesn't reproduce here," not "fixed" — do not claim it works |
| **Any multi-GPU output-parity claim above TP=1** | host does not reproduce its own output at TP=4 either — a host/guest text difference here is expected noise, but don't claim byte-identical output at TP>1 |
| **"Multi-tenant" anything** | not ready today by design intent, not by accident — see [`known-limitations.md#security`](../internal/known-limitations.md#security) |

## Before you ship a number

1. Does it name **which sides were bare metal**? If either side was itself a VM, say so.
2. Does it name **the driver and GPU**? Numbers do not transfer across driver versions without re-measurement.
3. Is it from the **most recent re-validation**, not an older matrix it superseded?
4. If it's a ratio below 1.0x, is the **cause understood or explicitly marked unexplained** in the copy — not silently rounded away?
