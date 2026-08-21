# clpeak on OpenBenchmarking — RTX 4070, bare metal both sides

**Published, third-party hosted, not editable by us:**
<https://openbenchmarking.org/result/2608219-NE-NVKVMPVRT27>

One result file with two identifiers, `host-bare-metal` and `guest-nvkvm-pv`,
so the comparison is side by side on Phoronix's own server rather than in a
table we wrote.

## Why clpeak

clpeak is a pure-OpenCL microbenchmark with no display dependency, which makes
it runnable on a headless host, and its subtests separate the two things a
forwarding layer affects differently: **sustained throughput** (compute and
memory bandwidth) versus **per-call cost** (kernel launch latency, buffer
transfer). That split is the whole question for nvkvm, and clpeak measures it
directly rather than by inference.

GLUT-based OpenCL tests (`pts/juliagpu`, `pts/mandelgpu`) were dropped: they
need an X display, and this host is someone's desktop — starting an X server on
it was not acceptable.

## The result

| subtest | host | guest | guest / host |
|---|---|---|---|
| Single-Precision Compute | 28309.73 GFLOPS | 28326.23 GFLOPS | **100.06%** |
| Double-Precision Compute | 526.15 GFLOPS | 525.25 GFLOPS | **99.83%** |
| Integer Compute | 14520.94 GIOPS | 14524.97 GIOPS | **100.03%** |
| Integer 24-bit Compute | 14560.98 GIOPS | 14594.20 GIOPS | **100.23%** |
| Global Memory Bandwidth | 435.86 GB/s | 435.79 GB/s | **99.98%** |
| Transfer Bandwidth, `enqueueReadBuffer` | 14.32 GB/s | 14.35 GB/s | **100.21%** |
| Transfer Bandwidth, `enqueueWriteBuffer` | 16.06 GB/s | 15.96 GB/s | **99.38%** |
| **Kernel Latency** | **3.84 us** | **8.18 us** | **2.13x worse** |

Seven of eight subtests are at parity — 99.4% to 100.2%, i.e. inside run-to-run
noise, with several fractionally above 1.00x. **The eighth is the whole story:
kernel launch latency is 2.13x the host's.**

Nothing here is a surprise given the design, and that is the point — it is the
first *third-party-hosted* measurement of the effect. nvkvm's premise is that
GPU work does not exit: a kernel launch is a store to a mapped doorbell page, so
sustained work costs nothing. But a launch is not free, and clpeak's kernel
latency test does nothing except pay that cost in a tight loop with a
synchronisation each time. 3.84 us to 8.18 us is +4.3 us of forwarding per
round trip.

This corroborates, on an independent harness, what [parity.md](parity.md)
already reports from the LLM rows: batched work reaches 0.99–1.00x, while
single-stream greedy decode — hundreds of tiny launches per token, each
synchronised — sits at 0.73–0.82x. Same mechanism, measured directly here
instead of inferred from an end-to-end score.

It is also the honest counterweight to the Geekbench headline. **99.6% is real
and 2.13x is real**; which one a workload feels depends entirely on whether it
batches its launches.

## Method

Both sides ran the **same PTS tarball**, verified rather than asserted:
phoronix-test-suite 10.8.4,
`sha256:7b5da7193c0190c648fc0c7ad6cdfbde5d935e88c7bfa5e99cd3a720cd5e2c5a`,
and both compiled `clpeak-1.1.0` from that identical source tree. PHP differs
between the sides (8.5.4 host, 8.3.6 guest) but PHP only orchestrates — it is
not in any measured path.

Guest allocation, stated rather than hidden: **4 vCPU and 15.62 GB against the
host's Ryzen 9 7900 (24 threads) and 30 GB.** clpeak is GPU-bound, so this
matters far less than it does for Blender's scene sync, but it is the same
handicap.

Guest clocksource is `kvm-clock` (`tsc` is not offered). Measured, not assumed:
`clock_gettime` costs 18.0 ns in the guest against 15.5 ns on the host — 1.16x,
so it stays in the vDSO and is not distorting the 8.18 us figure. A vDSO
fallback would have been ~13x and would have swamped a microsecond-scale
measurement.

Reproduce:

```bash
curl -LO https://github.com/phoronix-test-suite/phoronix-test-suite/archive/refs/tags/v10.8.4.tar.gz
tar xzf v10.8.4.tar.gz && cd phoronix-test-suite-10.8.4
./phoronix-test-suite batch-install pts/clpeak
# RunAllTestCombinations=TRUE in ~/.phoronix-test-suite/user-config.xml,
# otherwise batch mode loops forever on clpeak's test-option menu (see below)
TEST_RESULTS_NAME=<name> TEST_RESULTS_IDENTIFIER=<side> \
  ./phoronix-test-suite batch-benchmark pts/clpeak
./phoronix-test-suite upload-result <name>
```

### A PTS trap worth writing down

`pts/clpeak` has a test-option menu (`cl-test`). In batch mode with stdin at
EOF, PTS **re-prints that menu forever and spins at 100% CPU** — it never runs
the test and never exits. It looks like a slow benchmark, not a hang, and it
survives being backgrounded, so an orphan can sit there burning a core.
Setting `RunAllTestCombinations=TRUE` makes PTS select every subtest instead of
prompting, which both fixes the spin and is the coverage we wanted.

Note also that the PTS process renames itself to `Phoronix Test Suite`, so
`pkill -f phoronix` does not match it.
