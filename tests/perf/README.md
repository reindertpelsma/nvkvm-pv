# tests/perf — host-vs-guest parity harness (PLAN + building blocks)

Goal: a single repeatable runner that proves nvkvm runs real workloads at host
parity, with correct methodology baked in so we never again chase a phantom gap
(see the 2026-06-01 corrections in ../../docs/perf/forwarding_latency_decomposition.md).

## Building blocks already here (committed 2026-06-01)
- `launchstorm.c`     — separates pipelined submit (A) vs launch+sync RT (B) vs
                        empty cuCtxSynchronize (C). Driver API, embedded empty PTX.
- `cuda_api_prof.cpp` — LD_PRELOAD interposer over the CUDA *runtime* API
                        (cudaLaunchKernel/StreamSync/MemcpyAsync...), cycle-accurate,
                        buckets memcpy by kind×size. Build: `g++ -shared -fPIC -O2
                        -I/usr/local/cuda-12.0/include cuda_api_prof.cpp -o x.so -ldl`.
                        Use on real apps: `LD_PRELOAD=x.so <app>`.
- `htod_probe.c`      — HtoD by fresh-vs-reused source, anon-vs-file-backed.
- `dtoh_probe.c`      — DtoH cold/warm/new-buffer + byte-exact check.
(`sp_pingpong.c` predates this; transport ping-pong.)

## Methodology the harness MUST enforce (lessons paid for, do not drop)
1. Capture the HOST baseline in the SAME run — never compare to a remembered number.
2. Steady-state sampling only. For decode/LLM, GPU util must be sampled DURING
   generation, not model load (load shows ~0% GPU; that mistake faked a "14x gap").
3. Guest RAM >= model size + overhead, or model-load HtoD is disk-bound (the 17x
   was guest -m 4G < 4.4GB model; now -m 16G). Assert free RAM at start.
4. Byte-exact correctness alongside throughput (DtoH/HtoD/matmul), not just speed.
5. Warm caches before timing (gguf into page cache; one warm-up copy before loops).
6. Each metric asserts a threshold vs the host run -> regression FAILS loudly.

## Reference numbers — measured by run_parity.sh (RTX 3060, driver 580.159.04, 2026-06-01)
A full host-vs-guest run captured these in ONE invocation (guest/host ratio). The
harness thresholds are encoded in run_parity.sh; these are the current baseline:
- GEMM 2048^3 fp32 : host 452 / guest 452 GFLOP/s  = **1.00x**  (gate: >=0.90)
- LLM decode 7B    : host 67.6 / guest 65.5 t/s    = **0.97x**  (gate: >=0.90; authoritative)
- HtoD reused      : host 14.2 / guest 13.4 GB/s   = **0.95x**  (gate: >=0.80)
- DtoH warm cached : host 9.7  / guest 9.0  GB/s   = **0.93x**  (gate: >=0.80; was 0.07 pre-#94)
- empty cuCtxSync  : host 0.35 / guest 0.39 us     = **1.11x**  (gate: <=3x; guard WB-sysmem)
- launch+sync RTT  : host 6.5  / guest 12.0 us     = **1.85x**  (tripwire: <4x — known control tax)
- alloc+free RTT   : host 133  / guest 3863 us     = **29x**    (tripwire: <50x — KNOWN tax,
  the standing optimization target; never been at parity — see [[perf_host_vs_guest]])
- DtoH/HtoD byte-exact correctness: OK both sides (hard gate).
GPU util during decode is sampled (tail-median) but INFORMATIONAL only — guest
nvidia-smi util is unreliable, and util sampling is what produced this session's
phantom "14x gap"; tok/s is the parity signal, not util.

## Harness (built — 2026-06-01)
- `run_parity.sh`     — orchestrator. Runs on the dev box; uses ~/.ssh/config
                        aliases `vh` (host) and `vg` (guest, ProxyJump via vh).
                        Stages the self-contained probes to each target, runs the
                        SAME workloads HOST-THEN-GUEST (strictly serial — one
                        physical GPU, see [[remote_test_serialization]]), parses,
                        and prints the PASS/FAIL parity table above.
                        `--no-llm` skips the slow decode test. Exit 0 = all gates pass.
- `parity_remote.sh`  — runs ON a target; builds gpu_bench.c (self-contained
                        dlopen) + dtoh_probe.c + launchstorm.c, runs them + the LLM
                        decode, emits `M <key> <value>` lines. AUTO resolves model/
                        llama paths against the target's $HOME.
Methodology guards are enforced in code: in-run host baseline, RAM>=model assert,
warm page cache, byte-exact correctness, threshold/regression gates.

### LLM serving parity (2026-08-17) — `llm_parity.md`
End-to-end vLLM 0.11.0 + Qwen2.5-32B-Instruct-AWQ, host vs guest, on an RTX
6000 Ada: prefill 1.00x, decode 0.99x, 64-way concurrent batch 1.00x, and every
temperature-0 token bit-identical across the boundary.  Runner
`apps/llm_serving_bench.py`, comparator `compare_llm_parity.py`.
Two caveats live in that doc and should be read before quoting the table:
- the guest cannot allocate a pinned host buffer larger than **16 MiB**
  (`nvkvm_cpu_pages_migrate_range()` -> -E2BIG, `NVKVM_MIG_MAXCHK == 8`), so
  stock vLLM does not start; the run disables vLLM's pinned buffers on BOTH
  sides.  `apps/pinned_host_probe.py` bisects the cap.
- decode parity is contingent on CUDA graphs.  With `--enforce-eager` the guest
  drops to **0.82x** decode — the per-launch control tax, which graph capture
  removes 95% of.  The "control-RTT is only 1-2% of per-token time" comment is
  confirmed for graph-using stacks and wrong for launch-per-kernel ones.

### Next: real-app matrix
Point `cuda_api_prof.so` (LD_PRELOAD interposer) at real apps beyond llama.cpp:
PyTorch (a small train/infer step), a CUDA sample, vLLM, Stable Diffusion. Compare
guest-vs-host per-API cycle costs to confirm parity generalizes past the probes.
