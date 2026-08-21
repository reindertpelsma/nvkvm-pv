# Blender Open Data — RTX 4070, bare metal both sides

Blender's Cycles renderer is a very different workload from Geekbench's OpenCL
kernels: it is a long-running, single-context GPU job with a substantial
CPU-side setup phase in front of it. That makes it a useful second opinion on
the [parity numbers](parity.md), and it splits the guest's cost into two parts
that the Geekbench figure blends together.

## Setup

| | host | guest |
|---|---|---|
| machine | MS-7E26 desktop, bare metal | nvkvm-pv guest on that host |
| OS | Ubuntu 26.04, kernel 7.0.0-30 | Ubuntu 24.04 |
| CPU visible | Ryzen 9 7900, 24 threads | 4 vCPU |
| RAM | 30 GB | 15.62 GB |
| clocksource | `tsc` | `kvm-clock` |
| GPU | RTX 4070, driver 595.84 | same GPU, same driver |

Both sides ran the **same launcher binary**, verified by checksum rather than
asserted — `benchmark-launcher-cli` 3.3.0,
`sha256:c52cd088936d7cd8fac9b72f0d14055744e5d4ddd40f85ba299892350c494515`,
copied host→guest. Blender 4.5.0 LTS, scenes `monster`, `junkshop`,
`classroom`, device type **CUDA on both sides**.

CUDA and not OptiX because the launcher only offers OptiX on the host: the
guest enumerates the 4070 as a `CUDA` device and the host as an `OPTIX` one, so
OptiX is not a like-for-like pair today. That asymmetry is itself a finding and
is tracked in [known limitations](../internal/known-limitations.md).

The first pass on each side was discarded as a warm-up. Numbers below are from
the passes after it.

## The result

The launcher reports two timings per scene: `total_render_time`, and
`render_time_no_sync` which excludes Cycles' scene-sync phase (BVH upload,
mesh and texture transfer — CPU-side work plus host-to-device copies).
Separating them is what makes this measurement interesting.

| scene | metric | host | guest | ratio |
|---|---|---|---|---|
| monster (slow mode) | GPU render | 34.117 s | 34.163 s | **99.86%** |
| monster (fast mode) | GPU render | 30.027 s | 30.028 s | **100.00%** |
| junkshop | GPU render | 33.172 s | 33.150 s | **100.07%** |
| classroom | GPU render | 30.225 s | 30.265 s | **99.87%** |
| | | | **geomean** | **99.95%** |
| monster (slow mode) | total | 35.359 s | 37.529 s | 94.22% |
| monster (fast mode) | total | 31.303 s | 33.354 s | 93.85% |
| junkshop | total | 39.419 s | 43.495 s | 90.63% |
| classroom | total | 30.650 s | 32.691 s | 93.76% |
| | | | **geomean** | **93.10%** |

**The GPU render itself is at parity — 99.95%, and one scene lands fractionally
above 1.00x.** Everything the guest loses, it loses in scene sync:

| scene | host sync | guest sync | delta |
|---|---|---|---|
| monster | 1.24 s | 3.37 s | +2.12 s |
| junkshop | 6.25 s | 10.34 s | +4.10 s |
| classroom | 0.43 s | 2.43 s | +2.00 s |

The delta is roughly constant per scene rather than proportional to render
length, which is what you would expect from a fixed upload cost plus CPU-side
work done on 4 vCPUs instead of 24 threads — not from a per-kernel forwarding
tax. junkshop, the scene with by far the heaviest geometry (its host sync alone
is 6.2 s), pays the largest absolute penalty and posts the worst total ratio.

So the honest summary is two numbers, not one: **93.1% on the metric Open Data
would score, 99.95% on the GPU work itself.** Quoting only the first would
understate the forwarding path; quoting only the second would hide a real cost
a user of a 4-vCPU guest would actually feel.

## The bimodality trap

`monster` renders in one of two clearly separated modes on this GPU — about
30.03 s or about 34.15 s of GPU time, with nothing in between. Over five runs:

```
host  render_time_no_sync : 34.149  34.217  34.051  34.052  30.027
guest render_time_no_sync : 30.050  34.143  34.208  30.018  30.011
```

**The two modes are the same on both sides**, to within 0.03 s. It is a
property of the scene on this card, not of virtualisation.

This is worth recording because it is exactly the shape of artifact that has
burned this project before. An early three-pass sample happened to catch the
guest mostly in the slow mode and the host entirely in it, which read as a
clean "guest is 6% slower on monster" — a result that survives a
stability check, because each individual mode is stable to ±0.05 s. Only
re-running monster on its own, five times per side, exposed it. Any comparison
that samples `monster` fewer than about five times per side should not be
trusted to better than 6%.

`junkshop` and `classroom` are unimodal and repeat to within ±0.03 s.

## Clocksource

The guest runs `kvm-clock` — `tsc` is not offered (`available_clocksource` is
`kvm-clock hpet acpi_pm`). This project has previously seen `kvm-clock` push
`clock_gettime` out of the vDSO and cost a benchmark suite 22%. **That is not
happening here**, measured rather than assumed:

| | clocksource | `clock_gettime(CLOCK_MONOTONIC)` |
|---|---|---|
| host | `tsc` | 15.5 ns |
| guest | `kvm-clock` | 18.0 ns |

1.16x, not the ~13x of a vDSO fallback. Cycles does not time individual
kernels anyway, but the timing path is not distorting these numbers.

## Reproducing

```bash
curl -LO https://download.blender.org/release/BlenderBenchmark2.0/launcher/benchmark-launcher-cli-3.3.0-linux.tar.gz
tar xzf benchmark-launcher-cli-3.3.0-linux.tar.gz
./benchmark-launcher-cli blender download 4.5.0
for s in monster junkshop classroom; do
  ./benchmark-launcher-cli scenes download --blender-version 4.5.0 $s
done
# discard the first pass, then:
./benchmark-launcher-cli benchmark --blender-version 4.5.0 \
  --device-type CUDA --json monster junkshop classroom
```

Raw per-run timings are in
[`data/benchmarks/blender-4070.json`](../../data/benchmarks/blender-4070.json).

## Not published to opendata.blender.org

Unlike the Geekbench rows, these are **not** on a third-party server. Adding
`--submit` to the launcher requires a token from `benchmark-launcher-cli
authenticate`, which is an OAuth device flow against a Blender ID account
(`id.blender.org`, email and password). There is no anonymous submission path
in launcher 3.x. Publishing these therefore needs an account holder to run:

```bash
./benchmark-launcher-cli authenticate      # opens/prints a verification URL
./benchmark-launcher-cli benchmark --blender-version 4.5.0 \
  --device-type CUDA --json --submit monster junkshop classroom
```

on each side, which prints a public `opendata.blender.org/benchmarks/<uuid>/`
URL. Until that happens these numbers are self-reported and should be labelled
as such.
