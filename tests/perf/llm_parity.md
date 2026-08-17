# nvkvm LLM serving parity — host vs guest

**Headline: a 32B model served by vLLM runs at host parity inside the nvkvm
guest — prefill 1.00x, decode 0.99x, 64-way concurrent batch 1.00x — and every
generated token at temperature 0 is bit-identical between the two sides.**

Measured 2026-08-17 on an RTX 6000 Ada. This is the first end-to-end LLM
serving number published for nvkvm; the existing
[`realapp_matrix.md`](realapp_matrix.md) LLM rows are llama.cpp decode/prefill
on a 7B, not a serving stack, and were not re-run on this driver.

Two things had to be fixed before vLLM would start in the guest at all, and one
of them is a real forwarder limitation. Both are documented below — see
[Blockers](#blockers-found-standing-this-up). Read that section before quoting
the throughput table: the guest numbers were taken with vLLM's pinned host
buffers disabled *on both sides*, because the guest cannot allocate them.

---

## 1. What was run

| | host | guest |
|---|---|---|
| GPU | NVIDIA RTX 6000 Ada Generation, 48 GB (49140 MiB) | same physical GPU |
| driver (`nvidia-smi`, both sides) | 575.51.03 / CUDA 12.9 | 575.51.03 / CUDA 12.9 |
| OS / kernel | Ubuntu 22.04.5, 6.8.0-59-generic | Ubuntu 24.04, 6.8.0-137-generic |
| vCPU **used by the benchmark** | **16** (`taskset -c 0-15`; box has 122) | **16** (`VM_SMP=16`) |
| RAM available at start | 157.5 GB | 61.7 GB (`VM_MEM=64G`) |
| model store | local NVMe | guest-local disk (NOT 9p) |

The box is a vast.ai instance (contract 47962564, machine 8296) and is itself a
KVM VM, so the nvkvm guest is **nested**. Absolute throughput should be read
with that in mind; the guest/host ratio is the signal and both sides sit inside
the same outer VM.

### Stack — identical on both sides, and proven so

| | version |
|---|---|
| serving stack | **vLLM 0.11.0** |
| torch | 2.8.0+cu128 |
| transformers | 4.56.2 (pinned; 5.x breaks vLLM 0.11's tokenizer wrapper) |
| Python | 3.12.14 (uv-managed CPython at `/opt/uvpy`, same path both sides) |
| model | **Qwen/Qwen2.5-32B-Instruct-AWQ** — 32.5 B params, 4-bit AWQ (`awq_marlin` on sm_89), 19.34 GB on disk |

The venv was built **once on the host** and copied wholesale to the guest at the
identical path (`/opt/llmvenv`), so "same version" is actually "same bytes":

```
$ find /opt/llmvenv -type f -name '*.py' -o -type f -name '*.so' | sort | xargs sha256sum | sha256sum
796a28d25914db69f0792f049b22c8dc741ae6aec1e6f0922fb13538b1110cff  -   # host
796a28d25914db69f0792f049b22c8dc741ae6aec1e6f0922fb13538b1110cff  -   # guest
```

Weights likewise — every shard hashed on both sides, `diff` clean:

```
548f5c7078e297088c74bec4443bfe9e3b4183ee7457f328107c37a5eb861ea1  model-00001-of-00005.safetensors
735d1b5aa8ef01420f2079b355d84cfa7a4b37571d44715276b3b903c06d65d8  model-00002-of-00005.safetensors
f158db037c405677ea6ca21a5cb67a800ddc256217c722c0dee0eb31f3f75fb8  model-00003-of-00005.safetensors
ece82d2cd5ecc9691572aae55cf2d66fa52edd2c199d5a6dccb8182145bae59c  model-00004-of-00005.safetensors
1af3bf12ce80cf2f85bacb57a7d2fd584712a945f481cf394f0c7b17983269e5  model-00005-of-00005.safetensors
2bf6e9840c77e5ca4c5f8cada21b5530f4ebd36b2788dd2e2f0fe7ac62ec4284  config.json
c0382117ea329cdf097041132f6d735924b697924d6f6fc3945713e96ce87539  tokenizer.json
```

Engine flags, identical: `max_model_len=32768`, `gpu_memory_utilization=0.85`,
`enable_prefix_caching=False`, CUDA graphs on (vLLM default),
`seed=1234`, `temperature=0.0, top_p=1.0, top_k=-1`.

### Workload

The long-context material is a real 400 000-char slice of this repo's own
source tree, assembled deterministically by
[`apps/make_llm_corpus.py`](apps/make_llm_corpus.py) and copied to both sides:
`corpus sha256 = 5288922c1b3ec3786b873021801098cbc1185c77baea0f7f48b32b23575f78ba`
on host and guest. Prompts are sliced by **token**, not character, and the
harness hashes the exact token-id list it feeds the model — the 8245-token
prefill prompt hashed `5254a00e907a4726` on both sides, so "the two sides saw
the same input" is measured, not assumed.

---

## 2. Results

Runner: [`apps/llm_serving_bench.py`](apps/llm_serving_bench.py), compared with
[`compare_llm_parity.py`](compare_llm_parity.py). Host and guest ran **strictly
serially** — one physical GPU.

### Pass A — default configuration (CUDA graphs on)

8245-token prefill prompt · 512-token decode · 32 concurrent requests

| metric | host | guest | ratio |
|---|---|---|---|
| **prefill, 8245-tok prompt** | 1527.9 | 1532.9 tok/s | **1.00x** |
| **decode, steady state** | 41.68 | 41.08 tok/s | **0.99x** |
| **batch x32, output** | 244.2 | 244.3 tok/s | **1.00x** |
| **batch x32, total (in+out)** | 1262.8 | 1263.4 tok/s | **1.00x** |

Batch detail: 32 requests, 34 165 input + 8 192 output tokens, wall
33.542 s host / 33.528 s guest.

Per-repetition spread — the runs are tight enough that the decode ratio is a
real effect, not noise:

```
prefill wall s   host [5.282, 5.396, 5.464]   guest [5.265, 5.379, 5.452]
decode  tok/s    host [41.57, 41.71, 41.68]   guest [41.05, 41.10, 41.08]
```

### Pass B — harder operating point

16384-token prefill context · 1024-token decode · **64** concurrent requests

| metric | host | guest | ratio |
|---|---|---|---|
| prefill, 16k-tok context | 1374.0 | 1378.1 tok/s | **1.00x** |
| decode, steady state (1024 tok) | 41.4 | 41.0 tok/s | **0.99x** |
| batch x64, output | 253.6 | 253.7 tok/s | **1.00x** |
| batch x64, total (in+out) | 1311.4 | 1311.9 tok/s | **1.00x** |

Batch detail: 64 requests, 68 340 input + 16 384 output tokens, wall
64.607 s host / 64.580 s guest.

### Output equality at temperature 0 — **identical**

Three structurally hard tasks, each generated one-at-a-time (batch of one, so
continuous-batching numerics cannot vary the result), compared by sha256 of the
**token-id sequence**, not just the text:

| task | tokens | host sha (first 16) | guest sha | verdict |
|---|---|---|---|---|
| long-chain reasoning (bottleneck arithmetic) | 666 | `626b5c20df50f150` | `626b5c20df50f150` | **IDENTICAL** |
| code generation (lock-free SPSC ring, C11 atomics + memory-order justification) | 900 | `2bf24a9a00cdedad` | `2bf24a9a00cdedad` | **IDENTICAL** |
| long-document summary (8192-token source excerpt) | 600 | `f62ad0fd0a628214` | `f62ad0fd0a628214` | **IDENTICAL** |

Host-vs-guest identical in **every** pass: Pass A, Pass B, the eager pass
below, and additionally an earlier host run taken with pinned buffers *enabled*
(which produced the same three hashes as the no-pin host run). Output is
coherent, not degenerate — the reasoning task produces correctly-formatted
step-by-step arithmetic, the code task produces a compiling-shaped C ring
buffer with per-atomic memory-order commentary.

The one hash that moves is `long_doc_summary` in the eager pass
(`6ebb2bc027541dc9` instead of `f62ad0fd0a628214`) — but it moves **on both
sides together**, so it is an eager-vs-CUDA-graph numerics difference, not a
host-vs-guest divergence. Within each pass the two sides always match.

---

## 3. Where the residual 1% comes from — and the control-ring prediction

`src/`'s standing comment says the control-path ring is off by default because
*"control-RTT is only ~1-2% of per-token time (the bottleneck is GPU compute +
the mapped doorbell/fence launch path)"*.

**That prediction is confirmed — but it is contingent on CUDA graphs.** Re-run
identically with `enforce_eager=True` (graphs off, so every kernel is launched
individually):

### Pass C — CUDA graphs OFF (`--enforce-eager`)

| metric | host | guest | ratio |
|---|---|---|---|
| prefill, 8245-tok prompt | 1607.4 | 1534.8 tok/s | 0.95x |
| **decode, steady state** | **30.0** | **24.8 tok/s** | **0.82x** |
| batch x8, output | 101.0 | 91.4 tok/s | 0.90x |
| batch x8, total | 942.4 | 853.0 tok/s | 0.91x |

Put the two decode numbers side by side:

| decode | host | guest | gap per token |
|---|---|---|---|
| CUDA graphs **on** (default) | 41.68 t/s = 23.99 ms/tok | 41.08 t/s = 24.34 ms/tok | **+0.35 ms** (1.4%) |
| CUDA graphs **off** (eager) | 30.03 t/s = 33.30 ms/tok | 24.80 t/s = 40.32 ms/tok | **+7.02 ms** (21%) |

The forwarding tax is real and it is exactly what the comment says it is: a
**per-launch** cost. Turn CUDA graphs on and the entire decode step collapses
into one graph launch, which removes **95%** of that tax (7.02 ms → 0.35 ms)
and lands decode at 0.99x. Nothing else about the two runs differs.

> Order-of-magnitude sanity check, *not* a measurement from this session: a
> 64-layer AWQ decode step in eager mode issues on the order of a thousand
> kernel launches, and `README.md`'s previously-published launch+sync RTT delta
> is ~5.5 us (host 6.5 vs guest 12.0 us). ~1 300 launches x 5.5 us ≈ 7 ms,
> which matches the 7.02 ms observed. That RTT figure was taken on **different
> hardware** (RTX 3060, driver 580.159.04) and was **not** re-measured here, so
> treat the arithmetic as consistency, not proof.

So: leaving the control ring off is the right default *for a stack that uses
CUDA graphs*, which every modern serving stack does. The 1-2% estimate is
accurate for that case. A workload that launches kernels one at a time will pay
~20% on decode, and for that class of workload the comment would be wrong.

Prefill is at parity in both modes (1.00x with graphs, 0.95x eager) because a
long prefill is a small number of very large kernels — compute dominates and
there is nothing for a per-launch tax to bite on. This matches the existing
prefill methodology note in `realapp_matrix.md`.

---

## 4. Model load — 1.27x, and it is NOT weight upload

Model load is reported separately and is **not** an inference metric. The
harness never lets it leak into one (decode is derived as
`t(max_tokens=N) − t(max_tokens=1)`, so both prefill and load are subtracted
off).

With Inductor caches warm on both sides, `VLLM_LOGGING_LEVEL=INFO`:

| stage | host | guest | ratio |
|---|---|---|---|
| **loading weights (18.14 GiB HtoD)** | **5.75 s** | **5.55 s** | **1.04x** |
| model loading total | 7.89 s | 9.44 s | 0.84x |
| torch.compile | 13.57 s | 15.31 s | 0.89x |
| CUDA graph capture | 17 s | 27 s | 0.63x |
| init engine (profile + KV cache + warmup) | 48.84 s | 62.55 s | 0.78x |
| **total `LLM()` construction** | **63.0 s** | **80.3 s** | **0.78x (1.27x slower)** |

Getting 18 GiB of weights onto the GPU is at parity — slightly *faster* in the
guest, within noise. The whole load gap is one-time control-heavy startup:
graph **capture** (which is thousands of small control ops, the exact thing the
forwarder taxes) and torch.compile. This is the same per-launch tax as §3,
paid once at startup instead of per token.

> The very first guest run showed 147.4 s vs 64.1 s. That was a cold Inductor
> compile cache in the guest, not a forwarder effect — with caches warm on both
> sides it is 80.3 s vs 63.0 s. Reported here rather than quietly dropped.

---

## 5. Blockers found standing this up

### 5a. Hard 16 MiB cap on pinned host memory — vLLM will not start without a workaround

> **RESOLVED (2026-08-17, branch `fix-pin-cap`).** The cap is gone and stock,
> unmodified vLLM now starts in the guest with pinned buffers enabled. Jump to
> [5a-fix](#5a-fix--cap-removed) for the before/after measurements. The
> `CUDA_ERROR_INVALID_VALUE` loose end flagged at the bottom of this section is
> also root-caused there — it was a *second*, separate limit, not noise.
>
> Everything below this line is the original finding, kept as written.

vLLM's first act is to allocate an ~80 MiB pinned `inputs_embeds` staging
buffer. In the guest that fails:

```
torch.AcceleratorError: CUDA error: OS call failed or operation not supported on this OS
  ... vllm/v1/worker/gpu_model_runner.py:448 _make_buffer -> CpuGpuBuffer -> torch.zeros(..., pin_memory=True)
```

Bisected exactly (`apps/pinned_host_probe.py`):

| | largest successful pinned host allocation |
|---|---|
| host | ≥ 2 GiB (2 147 483 648 B — the probe's search ceiling, not a limit) |
| **guest** | **exactly 16 MiB — 16 777 216 B succeeds, 16 777 217 B fails** |

**Root-caused in our own source.** `nvkvm_cpu_pages_migrate_range()` in
[`src/guest/nvkvm_mmap.c`](../../src/guest/nvkvm_mmap.c) rejects the
registration:

```c
#define NVKVM_MIG_MAXCHK  8          /* 16MB cap / 2MB = 8 chunks max */
...
if (end - start > (16ULL << 20))     /* sanity: 16 MB max per call */
    return -E2BIG;
```

The cap is structural: the chunk descriptor array is a fixed 8-entry stack
array of 2 MiB chunks. Guest `dmesg` confirms it end-to-end — the largest
successful registration is exactly the 8-chunk maximum, and every larger one
returns `-7` (`-E2BIG`):

```
nvkvm DIAG: migrate_range(bulk) 4096 pages, 8 chunks in 516848 us
nvkvm: OS_DESCRIPTOR migrate 7ebe62000000+2000000 failed: -7      (32 MiB)
nvkvm: OS_DESCRIPTOR migrate 757fd0000000+4000000 failed: -7      (64 MiB)
nvkvm: OS_DESCRIPTOR migrate 757f94000000+40000000 failed: -7     (1 GiB)
```

**Workaround used for the measurements above**, applied *identically to both
sides* so the comparison stays honest: a one-line switch in
`vllm/platforms/interface.py` making `is_pin_memory_available()` return `False`
under `VLLM_NO_PIN_MEMORY=1`, with the env var set on host and guest. The
patched file hashed identically on both sides
(`ef07ba37af33d8ce0c5fa55e1602fb16e48bb9cc95c13ab9a2ed34c108d97c84`).

**This is a caveat on the result, stated plainly:** the throughput table is
vLLM-without-pinned-host-buffers on both sides. It is a valid host-vs-guest
comparison, but it is not the stock vLLM configuration, and stock vLLM does not
currently run in the guest. Raising `NVKVM_MIG_MAXCHK` (or chunk-looping the
registration) is the fix; it was not attempted here.

Registration is also *slow* in the guest — the module's own DIAG counters
during the vLLM run:

| registration | guest (kernel DIAG) | host (`cuMemHostAlloc` wall clock) |
|---|---|---|
| 2 MiB (512 pages, 1 chunk) | 66–80 ms | 1.8 ms |
| 16 MiB (4096 pages, 8 chunks) | 517 ms | 13.7 ms |

> Loose end, flagged rather than hidden: a direct `ctypes` `cuMemHostAlloc`
> probe in the guest returned `CUDA_ERROR_INVALID_VALUE` (1) for 4/8/16 MiB and
> `CUDA_ERROR_OPERATING_SYSTEM` (304) for 32/64 MiB, even though torch's
> `cudaHostAlloc` path succeeds at 16 MiB. The 304 boundary matches the cap;
> the 1s at 4–16 MiB are unexplained and were not chased.
>
> **Root-caused in [5a-fix](#5a-fix) (2026-08-17).** The `1`s were a genuinely
> separate limit: `remap_pfn_range()` returning `-EINVAL` because it refuses a
> sub-VMA remap on a copy-on-write mapping. Private memory hit it; the memory
> behind torch's `cudaHostAlloc` did not, which is exactly why the two probes
> disagreed at the same size. Both limits are fixed.

H2D bandwidth itself is fine — the cap is about *registration size*, not
throughput:

| H2D source | host | guest | ratio |
|---|---|---|---|
| pageable (anon) | 11.33 | 10.79 GB/s | 0.95x |
| file-backed `mmap` (how weights arrive) | 7.27 | 7.24 GB/s | 1.00x |
| pinned | 26.62 (512 MiB buf) | 25.23 (16 MiB buf) GB/s | not comparable — buffer sizes forced apart by the cap |

<a name="5a-fix"></a>
### 5a-fix. Cap removed — stock vLLM starts with pinned buffers enabled

Measured 2026-08-17 on an RTX 3060 / driver 575.51.03 guest (Ubuntu 24.04,
6.8.0-137-generic), `VM_MEM=48G VM_SMP=16`. Both modules built from the same
tree; the loaded module was confirmed by `srcversion` each time
(`2077C65BFF2E2D7529252CC` pre-fix, `DFC4446C299D3AC91C229E1` fixed).

**There were three limits, not the two the cap advertised.** The third is the
one that had gone unnoticed, and it explains the loose end above:

| # | limit | error | who hits it |
|---|---|---|---|
| 1 | `end - start > 16 MiB` sanity check | `-E2BIG` → `CUDA_ERROR_OPERATING_SYSTEM` (304) | any registration > 16 MiB |
| 2 | `nck >= NVKVM_MIG_MAXCHK` (8-entry array) | `-E2BIG` | never reached — (1) fires first |
| 3 | **`remap_pfn_range()` refuses any sub-VMA remap on a copy-on-write mapping** | `-EINVAL` → `CUDA_ERROR_INVALID_VALUE` (1) | **any** multi-chunk registration on ordinary `malloc`/`MAP_PRIVATE` memory |

`is_cow_mapping(flags)` is `(VM_SHARED|VM_MAYWRITE) == VM_MAYWRITE`, and for
such a VMA `remap_pfn_range()` returns `-EINVAL` unless the remap spans the
whole VMA exactly. Ordinary user memory is exactly that. Controlled experiment —
identical sizes, one registration per process, only the mmap flag differs:

```
== pre-fix, MAP_PRIVATE ==                 == pre-fix, MAP_SHARED ==
 1MiB   FAIL rc=304                         1MiB   OK
 2MiB   OK                                  2MiB   OK
 4MiB   FAIL rc=1  CUDA_ERROR_INVALID_VALUE 4MiB   OK
 8MiB   FAIL rc=1  CUDA_ERROR_INVALID_VALUE 8MiB   OK
16MiB   FAIL rc=1  CUDA_ERROR_INVALID_VALUE 16MiB  OK          <- the parity run's 16 MiB
16MiB+1 FAIL rc=304 CUDA_ERROR_OPERATING_SYSTEM  16MiB+1 FAIL rc=304
```

Guest `dmesg` for the MAP_PRIVATE column shows `-22` (`-EINVAL`), a different
error from the `-7` (`-E2BIG`) above 16 MiB:

```
nvkvm: migrate_range(bulk) failed ret=-22 at chunk 2
nvkvm: OS_DESCRIPTOR migrate 744346600000+400000 failed: -22      (4 MiB)
nvkvm: migrate_range(bulk) failed ret=-22 at chunk 8
nvkvm: OS_DESCRIPTOR migrate 787027000000+1000000 failed: -22     (16 MiB)
nvkvm: OS_DESCRIPTOR migrate 7b5cd8000000+2000000 failed: -7      (32 MiB)
```

So torch's `cudaHostAlloc` reached 16 MiB because libcuda backs it with a
non-COW mapping, while the `ctypes cuMemHostAlloc` probe on private memory did
not. **That is the `CUDA_ERROR_INVALID_VALUE` at 4-16 MiB, fully explained.**

#### Stock vLLM: before and after

Identical script, identical model, no `VLLM_*` env vars, no source edits
(`vllm/platforms/interface.py` sha256 `ff142312173c173dae20a8303cd10f9c29fa255862396655b6888d2fdb73fae5`,
`NO_PIN_MEMORY` absent), `is_pin_memory_available() == True` asserted before the
engine is constructed. vLLM 0.11.0 / torch 2.8.0+cu128 / Qwen2.5-1.5B-Instruct,
whose `inputs_embeds` staging buffer is 32 MiB — past the old cap.

```
PRE-FIX  module 2077C65BFF2E2D7529252CC
  PYTHON_EXIT=1
  torch.AcceleratorError: CUDA error: OS call failed or operation not supported on this OS
  RuntimeError: Engine core initialization failed.
  dmesg: nvkvm: OS_DESCRIPTOR migrate 7d12ce000000+2000000 failed: -7     (32 MiB, -E2BIG)

FIXED    module DFC4446C299D3AC91C229E1
  is_pin_memory_available() = True
  ENGINE_READY_IN 117.8s
  OUTPUT: A hypervisor is a software program that allows multiple operating systems
          to run on a single computer, creating a virtualized environment.
  GENERATION_OK
  PYTHON_EXIT=0
  dmesg: nvkvm DIAG: migrate_range(bulk) 8192 pages, 16 chunks, dup_peak=2097152 B in 152997 us
```

#### Registration size and cost, after the fix

`cuMemHostRegister`, MAP_PRIVATE, one registration per process
(`tests/perf/run_pin_ladder.sh`). Every size succeeds; 2 GiB + 1 byte is the
first failure, which is the new sanity check doing its job.

| size | guest ms | guest MB/s | host ms |
|---|---|---|---|
| 16 MiB | 82.2 | 195 | 1.3 |
| 17 MiB | 108.3 | 157 | 1.3 |
| 32 MiB | 155.4 | 206 | 2.5 |
| 80 MiB | 319.9 | 250 | 5.9 |
| 256 MiB | 1147.1 | 223 | 26.4 |
| 1 GiB | 3787.4 | 270 | 68.5 |
| 2 GiB | 7313.4 | 280 | 123.3 |
| **2 GiB + 1** | **FAIL rc=304** (`-E2BIG`) | — | OK |

Registration is ~250-350 MB/s in the guest and roughly **linear** in size, so
the cost is predictable rather than cliff-shaped. It is still ~15-60x the host
and is a separate problem from the cap — most likely the slot-batched upload
loop, not the per-chunk loop. Not addressed here.

For context, the parity run measured 517 ms for 16 MiB on an RTX 6000 Ada box;
this run measures 82 ms for the same size on an RTX 3060 box. Different
hardware, so the two are not directly comparable, but the 65-second
extrapolation for 2 GiB that the old rate implied did not materialise — the
measured figure is 7.3 s.

#### Bounded duplication

The property the old cap was protecting is that data does not sit in two places
at once for long. It is now measured directly: the module reports `dup_peak`,
the largest number of bytes that existed in **both** the pinned guest pages and
a memfd at any single instant.

```
512 pages,       1 chunk,  dup_peak=2097152 B
8192 pages,     16 chunks, dup_peak=2097152 B
65536 pages,   128 chunks, dup_peak=2097152 B
262144 pages,  512 chunks, dup_peak=2097152 B
524288 pages, 1024 chunks, dup_peak=2097152 B      <- 2 GiB
```

Flat at one chunk (2 MiB) from 2 MiB to 2 GiB — and tighter than the 16 MiB the
old batched Phase 1 held. Corroborated from outside the kernel: guest `MemFree`
sampled every 200 ms across a 2 GiB registration dips by ~2.09 GB (the buffer
itself, allocated before the call) and then **climbs back steadily during the
registration** as each chunk's pins are released, rather than staying flat until
the end:

```
36083712 -> 33996712 (buffer allocated) -> 34139080 -> 34490476 -> 34833508
-> 35182984 -> 35482808 -> 35762208 -> 35944716 -> ... -> 36248436   (kB)
```

If the migration held a second full copy, the trough would be ~4 GB below
baseline rather than ~2 GB.

Note the memfd is **not** transient and its total does scale with the request:
it is the mapping's backing store for the lifetime of the registration — the
data in its final home, not overhead.

#### No VMA fragmentation

Chunked migration does not split the caller's mapping. `vm_flags_set()` /
`vm_flags_clear()` take no address range, so they cannot split a VMA; the
conversion is done once for the whole range and the per-chunk loop only touches
PTEs. Measured from `/proc/self/maps` after registration, plus a full byte-wise
readback of the pattern written before it:

| registration | chunks | VMAs overlapping the buffer | process VMAs total | bytes wrong |
|---|---|---|---|---|
| 16 MiB | 8 | **1** | 106 | 0 / 16777216 |
| 256 MiB | 128 | **1** | 106 | 0 / 268435456 |
| 1 GiB | 512 | **1** | 107 | 0 / 1073741824 |

#### Regression

`tests/validate.sh` in the guest on the fixed module: **28 PASS, 0 FAIL,
0 SKIP, exit code 0** — same as the pre-fix baseline taken in the same session.

### 5b. Missing `libcuda.so` linker symlink — fixed in this branch

Triton (and therefore vLLM, `torch.compile`, anything on Inductor) shells out
at runtime to build its `cuda_utils` shim:

```
/usr/bin/gcc /tmp/.../cuda_utils.c -O3 -shared -fPIC -lcuda -L/usr/local/nvidia-guest/lib ...
/usr/bin/ld: cannot find -lcuda
```

`-lcuda` resolves only through the unversioned `libcuda.so` symlink. The host
has it from the driver package; `stage_guest_libs.sh` staged only
`libcuda.so.1`. It surfaces wrapped in an `InductorError`, which reads as a
compiler problem rather than a missing symlink. Fixed by staging the bare
linker name too.

### 5c. Guest sizing — the methodology trap, avoided

`run_test_vm.sh` defaulted to `-m 16G -smp 4` with a 20 GB guest disk. A 19.34 GB
model does not fit in any of the three. Per `README.md` methodology #3 this is
precisely the configuration that produced the historical phantom "17x gap", so:

- `VM_MEM`, `VM_SMP` and `GUEST_DISK_GROW` are now env-overridable; this run used
  `VM_MEM=64G VM_SMP=16 GUEST_DISK_GROW=140G`.
- The harness **asserts** `MemAvailable >= 1.3 x model + 4 GB` and refuses to run
  otherwise, emitting `M methodology_fail 1`.
- Weights are warmed into page cache on both sides before timing (host 15.8–16.6 s,
  guest 16.1–16.6 s of warming — comparable, so neither side started cold).
- Weights live on **guest-local disk**, never 9p.

---

## 6. Methodology controls applied

1. **Host baseline captured in the same session**, on the same GPU, strictly
   serially — never compared to a remembered number.
2. **vCPU counts equalised.** The box has 122 vCPUs and the guest has 16; the
   host side runs under `taskset -c 0-15`. Both sides report
   `sched_affinity 16`. Without this the host would have 7.6x the CPU for
   tokenisation, sampling and the scheduler loop.
3. **RAM >= model + overhead**, asserted in code, guest 61.7 GB available for a
   19.34 GB model.
4. **Page cache warmed on both sides** before any timing.
5. **Model load can never leak into an inference number** — decode is
   `t(max_tokens=N) − t(max_tokens=1)`, which subtracts prefill, the first
   token, and everything before it. Load is reported under a key literally named
   `model_load_s_NOT_INFERENCE`.
6. **First generation discarded** as warmup before any measurement.
7. **Prefix caching disabled**, so repeated prefills measure prefill, not a
   cache hit.
8. **Correctness alongside throughput** — token-id sha256 equality at
   temperature 0, not just tok/s.
9. **Input identity proven, not assumed** — corpus sha256 and prefill token-id
   sha256 emitted by both sides and compared.
10. **Same bytes, not just same version** — venv and weights hash-verified
    identical across the boundary.

## 7. What was NOT run

Stated rather than implied:

- **Stock vLLM (pinned host buffers enabled) in the guest** — impossible today, §5a.
- **vLLM's OpenAI HTTP server / `benchmark_serving.py`.** Concurrency was driven
  through the in-process engine API instead, deliberately: hitting an HTTP
  endpoint from outside the guest would have put QEMU's slirp NAT in the
  measurement path, which is not what this test is about.
- **Tensor parallelism / multi-GPU.** Single GPU only, `TP=1`.
- **A rerun of the llama.cpp 7B rows** from `realapp_matrix.md` on this driver.
- **Any fix for the 16 MiB cap.** Root-caused, not repaired.

---

## Reproducing

```bash
# both sides, same flags; run HOST first, then GUEST, never concurrently
export HF_HUB_OFFLINE=1 VLLM_NO_PIN_MEMORY=1 TOKENIZERS_PARALLELISM=false
python tests/perf/apps/make_llm_corpus.py <repo> /opt/corpus.txt 400000   # copy to both sides
python tests/perf/apps/llm_serving_bench.py \
    --model /opt/models/Qwen2.5-32B-Instruct-AWQ \
    --corpus /opt/corpus.txt --out side.json --tag host|guest
python tests/perf/compare_llm_parity.py host.json guest.json   # exit 0 = all gates pass
python tests/perf/apps/pinned_host_probe.py --tag host|guest   # the 16 MiB cap
```

The host side must be wrapped in `taskset -c 0-<VM_SMP-1>` to match the guest's
vCPU count.
