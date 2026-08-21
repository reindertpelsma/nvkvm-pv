# Reading the parity numbers

The README quotes host/guest ratios for several cards. This page explains what
those measurements do and do not establish, and why they differ between GPUs.

## Where the numbers come from

Every ratio is the same binary, the same script and the same machine, run once
on the host and once inside the guest. Geekbench rows are published to
Geekbench's own browser so a reader can check them without trusting us:

| card | guest | host | ratio | published |
|---|---|---|---|---|
| RTX 3050 Laptop | 48335 | 48395 | **99.9%** | [compare](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862) |
| A100 80GB PCIe | 203098 | 207234 | **98.0%** | [compare](https://browser.geekbench.com/v7/gpu/compare/85389?baseline=85405) |
| H100 PCIe | 261901 | 265071 | **98.8%** | [compare](https://browser.geekbench.com/v7/gpu/compare/85619?baseline=85612) |

On the A100 all eleven workloads land between 93.2% and 100.1%, two of them at
or above parity (Particle Physics 100.1%, Face Tracking 100.0%); the weakest is
Video Filter at 93.2%.

## Two caveats, and both cut in nvkvm's favour

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

**The guest was given less machine.** On the A100, 8 cores and 15.62 GB against
the host's 16 and 94.38 GB; on the H100, 16 cores and 62.79 GB against 20 and
125.88 GB. Geekbench's GPU workloads still do CPU-side work, so the guest
carries a handicap unrelated to forwarding.

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
