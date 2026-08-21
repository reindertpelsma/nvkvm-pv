# tests/perf/tp — tensor-parallel scaling, host vs guest

Answers one question: **how does the guest/host throughput ratio move as the
tensor-parallel world size grows?** Everything here runs the *same* file on
both sides — host and guest mount one read-only ext4 image holding the venv,
the interpreter and the weights, so the software stack is not a variable.

| script | what it measures |
|---|---|
| `tp_bench.py` | vLLM throughput at a fixed batch/prompt/output length, every repetition printed |
| `run_matrix.sh` | the TP=1/2/4 x eager/graphs matrix; `NVKVM_SHM=off` reproduces the old `NCCL_SHM_DISABLE=1` configuration |
| `run_tp4_reps.sh`, `tp4_graphs_spread.sh` | more warmups / more *separate processes*, because the variance that matters is between engine instantiations |
| `determinism.py`, `run_determinism.sh`, `det_tp1_graphs.sh` | is a temperature-0 decode reproducible **on one side**, before anyone compares two sides |
| `rtt_contend.c` | per-call forwarding round-trip as a *distribution*, under 1/2/4-way concurrency |
| `sync_micro.py` | synchronised (all-reduce / barrier) vs independent multi-GPU work |
| `paths_probe.py` | which fast paths a side actually gets (peer matrix, CUDA IPC, vLLM's P2P cache) |
| `results/` | the raw output the numbers in `../../BOOT_MATRIX.md` were read off |

Methodology notes that are not optional:

* **Both sides pinned to the same CPU count.** `NVKVM_PIN_CPUS=0-31` on the host
  against a 32-vCPU guest; a 98-core host versus a 4-vCPU guest would put CPU
  oversubscription in the answer without saying so.
* **Timing is rdtscp in `rtt_contend.c`, not `clock_gettime`.** Under kvm-clock
  the guest leaves the vDSO and a timestamp costs ~645 ns, which is 5% of a
  12 us round trip.
* **Cold passes are discarded** (`--warmups`), and every repetition is printed
  rather than averaged, because the guest's TP=4 numbers are bimodal and a mean
  hides that.
