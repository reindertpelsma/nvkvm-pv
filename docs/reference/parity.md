# Reading the parity numbers

The README quotes host/guest ratios for several cards. This page explains what
those measurements do and do not establish, why they differ between GPUs, and
which of them are bare metal on both sides. If you are here to check one number,
the four Geekbench rows below are the only ones you can verify without trusting
us.

## Where the numbers come from

Every ratio is the same binary, the same script and the same machine, run once
on the host and once inside the guest. Geekbench rows are published to
Geekbench's own browser so a reader can check them without trusting us:

| card | guest | host | ratio | published |
|---|---|---|---|---|
| RTX 4070 (Ada) | 181346 | 182134 | **99.6%** | [compare](https://browser.geekbench.com/v7/gpu/compare/87004?baseline=87011) |
| RTX 3050 Laptop | 48335 | 48395 | **99.9%** | [compare](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862) |
| A100 80GB PCIe | 203098 | 207234 | **98.0%** | [compare](https://browser.geekbench.com/v7/gpu/compare/85389?baseline=85405) |
| H100 PCIe | 261901 | 265071 | **98.8%** | [compare](https://browser.geekbench.com/v7/gpu/compare/85619?baseline=85612) |

On the A100 all eleven workloads land between 93.2% and 100.1%, two of them at
or above parity (Particle Physics 100.1%, Face Tracking 100.0%); the weakest is
Video Filter at 93.2%.

## Two caveats, and both cut in nvkvm's favour

**Two of these are bare metal on both sides, two are not.** The RTX 4070 and the
RTX 3050 Laptop were measured against real hardware — the 4070's host row is an
MS-7E26 desktop running Ubuntu 26.04, not a hypervisor guest. Those are the rows
to quote for a bare-metal claim.

**The "host" side is itself a VM on the datacenter cards.** Both the A100 and
the H100 were rented as "dedicated" machines and both turned out to be
hypervisor guests — the H100 reports `Hypervisor vendor: KVM` and DMI
`Standard PC (Q35 + ICH9)`. So the topology is L0 bare metal → L1 the rented VM
(the "host" column) → **L2 the nvkvm guest**, and the ratio measures nvkvm's
forwarding cost *while the guest is nested a level deeper than usual*.

That is worth more than a plain parity figure. Nesting is the hostile case for
any design that leans on VM exits, because an L2 exit has to traverse
L2 → L1 → L0. nvkvm's premise is that GPU work does **not** exit — a kernel
launch is a store to a mapped doorbell page — and these numbers are direct
evidence the premise holds where it would be punished hardest. Running nvkvm
inside an ordinary cloud VM is not a degraded configuration.

What it is *not* is a bare-metal number. The RTX 3050 row is the one to quote
for that. We did not measure L1 against L0, so these figures do not separate
nvkvm's cost from nesting's.

**The guest was given less machine**, on every row and by a wide margin. On the
RTX 4070 the guest had 4 cores and 15.62 GB against a Ryzen 9 7900 with 30 GB,
and it still reached 99.6% with eight of eleven workloads at or above parity
(Particle Physics 102.6%, Video Filter 102.3%; the weakest is RAW at 90.4%). On the A100, 8 cores and 15.62 GB against
the host's 16 and 94.38 GB; on the H100, 16 cores and 62.79 GB against 20 and
125.88 GB. Geekbench's GPU workloads still do CPU-side work, so the guest
carries a handicap unrelated to forwarding.

## Published on OpenBenchmarking: where the cost actually is

clpeak on the same RTX 4070, both sides, one result file on Phoronix's server:
[**openbenchmarking.org/result/2608219-NE-NVKVMPVRT27**](https://openbenchmarking.org/result/2608219-NE-NVKVMPVRT27)

Seven of its eight subtests land between 99.4% and 100.2% — single- and
double-precision compute, integer compute, global memory bandwidth, and both
buffer-transfer directions are all at parity. The eighth is **kernel launch
latency: 3.84 us on the host, 8.18 us in the guest, 2.13x.**

That is the clearest statement of the trade this design makes. Sustained GPU
work does not exit and costs nothing; each *launch* costs about +4.3 us. Every
other row on this page follows from that one number — which is why batched
serving reaches 1.00x and single-stream eager decode does not. Details and
method in [clpeak on OpenBenchmarking](openbenchmarking-clpeak.md).

## A second opinion: Blender Cycles

Geekbench is one vendor's harness. Blender Open Data on the same RTX 4070,
bare metal on both sides, splits the guest's cost in a way Geekbench does not:

| metric | ratio |
|---|---|
| GPU render time (`render_time_no_sync`) | **99.95%** (range 99.86–100.07%) |
| total render time, what Open Data scores | **93.10%** (range 90.63–94.22%) |

The GPU work is at parity; the whole visible deficit is Cycles' scene-sync
phase, which costs the guest a roughly constant +2.0 to +4.1 s per scene — CPU
work on 4 vCPUs instead of 24 threads, plus host-to-device upload. That is the
same CPU handicap noted above, showing up as a measurable line item rather than
being blended into a single score.

These are self-reported, not third-party hosted: launcher 3.x has no anonymous
submission path. Full method, per-run timings, and a bimodality trap that
`monster` sets for small samples are in
[Blender Open Data](blender-opendata.md).

## Why 98.0% on an A100 and 99.9% on an RTX 3050

The obvious guess — the CPU handicap — is probably wrong. The 3050 guest ran
under a *larger* relative handicap (4 cores against 8 cores / 16 threads, 5.79
GB against 15.34 GB) and still reached 99.9%.

The likelier explanation is the GPU. An A100 finishes each workload roughly four
times faster, so the same per-call forwarding cost occupies a correspondingly
larger share of a shorter run. This is the same effect as the slow
token-generation rows below, at much smaller scale, and it predicts that parity
will look slightly worse the faster the card gets.

## Why some LLM rows are far below 1.00x

Two shapes of LLM workload give very different answers, and both are honest:

| shape | example | ratio |
|---|---|---|
| batched | vLLM, Qwen2.5-32B-AWQ | **0.99–1.00x** |
| single-stream greedy decode | HF transformers eager, batch 1 | **0.73–0.82x** |

Greedy batch-1 decoding is hundreds of tiny kernel launches per token with a
synchronisation between each — exactly the latency-bound control path where a
forwarding layer costs something. It measures per-call overhead almost directly.

Anything that batches its launches is bandwidth- or compute-bound instead, and
pays close to nothing: vLLM reaches 0.99–1.00x, llama.cpp 0.97x. Sustained
compute is at parity outright — the occasional 1.01–1.02x matmul row is
measurement noise, not a speedup.

So the number that matters depends on what you run. A serving stack sees ~1.00x.
An interactive single-stream chat loop in eager mode sees the control path.

## Multi-GPU tensor-parallel serving

This is the one shape where the honest number is neither ~1.00x nor a control-path
story, and it is also the row that has been quoted wrongly the most.

| measurement | host | guest | ratio |
|---|---|---|---|
| vLLM TP=6, 6x RTX A4000, SHM transport on both sides | 163.57 | 140.15 tok/s | **~0.86x** |
| world=6 all-reduce, 64 MiB sustained, guest aggregate payload | — | 1.52 GB/s | — |

**The older 0.12–0.37x figures are superseded and should not be quoted.** They
were measured with `NCCL_SHM_DISABLE=1`, which was a *workaround*, not a
setting: NCCL's shared-memory transport failed in the guest inside
`cuMemImportFromShareableHandle`, so every world≥2 job needed it and the host's
fastest path was simply unavailable. That bug was fixed on 2026-08-21 — the CUDA
VMM shareable-handle fd is now brokered across isolates — and NCCL world=6
passes in the guest with **default settings**. The workaround hurt the guest far
worse than the host (host 115.7 → 47.9 tok/s without SHM; guest → 10.0), which
is why removing it moves the end-to-end ratio so much further than it moves
collective bandwidth (0.83 → 1.52 GB/s, roughly double).

Two caveats, both load-bearing:

- That is **one** datapoint, not a re-measured matrix. TP=1/2/4 scaling on the
  fixed stack has not been measured.
- In that run the **generated-text hashes differed between host and guest**,
  where the earlier `NCCL_SHM_DISABLE=1` comparison had been byte-identical.
  That is plausibly reduction-order nondeterminism in tensor-parallel
  collectives, which is common and not specific to nvkvm — but it has not been
  shown either way, so **output parity on this path is not claimed**.

Single-GPU serving is unaffected by any of this. Mechanism and the fd-brokering
fix: [cross-isolate sharing](../internal/cross-isolate-sharing.md); the raw runs
are in [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md).
