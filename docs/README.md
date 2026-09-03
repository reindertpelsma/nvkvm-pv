# nvkvm documentation

Start with [`ARCHITECTURE.md`](../ARCHITECTURE.md) at the repository root. It
explains the request path end to end and how the five hard problems — mmap,
nested guest pointers, ABI versioning, isolation, and the data plane — are
actually solved, with `file:line` citations throughout.

Everything here is either a procedure (how to do a thing), a reference (what a
value is), or rationale (why it is that way).

## How-to

| | |
|---|---|
| [Install a release](howto/install.md) | the `/dev/kvm` preflight, the container knobs, the tarball's runtime dependencies and glibc floor, the source build |
| [Build](howto/build.md) | the isolate stub, the patched QEMU, the guest kernel module |
| [Run a guest](howto/run.md) | image preparation, the QEMU command line, first bring-up, what to check |
| [Stage the guest NVIDIA userspace](howto/stage-guest-libraries.md) | `make_host_bundle.sh` on the host, `stage_guest_libs.sh` in the guest, and why the failures here are silent |
| [Add a driver version](howto/add-a-driver-version.md) | measuring a new ABI profile row and what else may need to change |

## Reference

**What is true, and what it was measured on.** The first four are the ones a
user reads; the last four go as deep as the subject needs and are written for
someone changing that code.

| | |
|---|---|
| [Correctness and known issues](reference/correctness.md) | what is known to be wrong, how far it is traced, and how to reproduce it |
| [Tested platforms, full matrix](reference/tested-platforms.md) | every box, driver and footnote — the README carries a condensed view |
| [Reading the parity numbers](reference/parity.md) | what the host/guest ratios do and do not establish, and which are bare metal on both sides |
| [Quoting numbers publicly](reference/quoting-numbers.md) | a short checklist: which numbers are safe to quote, which are not, and the scope each one needs |
| [Supported drivers and GPUs](reference/supported-drivers.md) | which profile rows have actually been booted, which have only been measured, and the host-CPU address-bit requirement |
| [Guest kernels](reference/guest-kernels.md) | which guest kernels the module builds on, which have been *run*, and why the range is narrow |
| [Guest userspace libraries](reference/guest-userspace-libraries.md) | every library the guest needs, what breaks without it, and the version-matching traps |
| [Device nodes](reference/device-nodes.md) | what appears in the guest, major/minor numbers, and why each one exists |
| [ABI profiles](reference/abi-profiles.md) | the version-keyed table, how it is measured, how it reaches all three components |
| [Allowlists](reference/allowlists.md) | all nine gates in the order an ioctl meets them — six static default-deny tables plus three code checks |
| [Virtio protocol](reference/virtio-protocol.md) | virtqueues, shared memory, request types, GPA windows |
| [Display broker protocol](reference/broker-protocol.md) | the 40-byte command and 24-byte packet, backpressure, and the rule it enforces |

## Audits

**Published in full, findings open or fixed.** The 2026-08-29 round audited five
components independently; the summary carries the totals, the criticals, and the
per-component remediation state.

| | |
|---|---|
| [Audit summary and remediation state](audit/README.md) | 111 findings across five components, 9 critical — what each was, and which are closed in code |
| [Guest kernel module](audit/2026-08-29-guest.md) | 12 findings, 1 critical — the `nvkvm_gem_resolve_fwd()` UAF |
| [VMM](audit/2026-08-29-vmm.md) | 13 findings, 6 serious |
| [Isolate](audit/2026-08-29-isolate.md) | 23 findings, 5 critical — the cross-isolate ownership results, and why they matter most |
| [Broker](audit/2026-08-29-broker.md) | 9 findings, 2 serious |
| [Packaging, scripts, kata](audit/2026-08-29-packaging.md) | 54 findings, 3 critical — including the sweep coordinator's root RCE |
| [Release-blocker audit, v0.2.0](audit/release-blockers-2026-09-02.md) | what was checked before tagging, and what was accepted |
| [Branch triage](audit/branch-triage-2026-08-29.md) | which fix branches carried which findings |

Earlier single-file rounds are listed under [Internal](#internal). All of them
name **locations, not techniques** — no working bypass procedure appears in any
of them.

## Investigations

**Root-cause narratives, including the theories that turned out to be wrong.**
Each of these is a log of an actual hunt: what was measured, what was ruled out,
and what it finally was. The disproven sections are kept deliberately — knowing
what a bug *is not* is most of the value when it comes back.

| | |
|---|---|
| [Host GPU VA-space exhaustion](investigations/va-space-leak/FINDINGS.md) | the BAR1 "leak" — **resolved, and it was not a leak**: deferred cleanup while live host referrers hold the mapping |
| [Shared-mapping desync](investigations/shared-mapping-desync/README.md) | registering a shared mapping with CUDA silently desynchronises it |
| [Nested nvkvm (L2)](investigations/nested-nvkvm-l2/README.md) | an nvkvm guest hosting an nvkvm guest — plus [the zeros, confirmed](investigations/nested-nvkvm-l2/CONFIRMED.md) and [what they actually were](investigations/nested-nvkvm-l2/ZEROS.md) |
| [Guest deadlock under GPU + I/O load](investigations/guest-deadlock-gpu-under-load/README.md) | **open** |
| [`cuInit` returned 999 on rented boxes](investigations/cuinit-999-rented-boxes/README.md) | fixed — a regression, bisected to the commit |
| [`GET_DEV_INFO` returns EBADF](investigations/get-dev-info-ebadf/README.md) | why the guest silently fell back to llvmpipe |
| [KWin could not drive the nvkvm head](investigations/kwin-atomic-modeset/README.md) | solved; [System Settings wedging on the Wayland GL path](investigations/kwin-atomic-modeset/systemsettings-wayland-hang.md) is still open |
| [The OOBE gamescope session](investigations/gamescope-oobe/README.md) | why gamescope never reached the display |
| [register-then-fork `MAP_PRIVATE`](investigations/register-then-fork-private-not-fixed/README.md) | `fork_mapping_semantics.c` fails, and it is not fixed |
| [Two processes registering the same shared buffer](investigations/shared-registration-two-processes/README.md) | what the second registration does |

## Internal

**Depth is the point here.** These are mechanism, root-cause narratives and
audit detail: `file:line` citations, RM class numbers, ioctl command values,
struct offsets. They are written to be read in full — by a person changing that
code, or by an agent with the context budget for it — not skimmed. A user-facing
page should link here rather than recite them.

| | |
|---|---|
| [Design rationale](internal/design-rationale.md) | the choices that shaped the architecture, and what was tried and rejected |
| [The forwarding model](internal/forwarding-model.md) | sanitiser, aux slot, handle translation, per-command special cases |
| [The isolate model](internal/isolate-model.md) | why one process per guest process, what the sandbox is, what the trust boundary is |
| [Cross-isolate sharing](internal/cross-isolate-sharing.md) | dma-buf and CUDA-VMM handle brokering between isolates — the NCCL shared-memory path |
| [Display broker design](internal/broker-design.md) | why the broker exists, its threat model, and the hardware verification log |
| [Present backpressure](internal/present-backpressure.md) | why the guest lapped its scanout ring, why deeper buffering was rejected, and what happens in every case where the host never releases a buffer |
| [Known limitations](internal/known-limitations.md) | open bugs, intrinsic limits, retracted findings, and numbers that should not be quoted |
| [The Mint guest desktop](internal/mint-guest-desktop.md) | the DDX investigation end to end, including what `fake-bars` measured before it was removed from the tree |
| [Narrow-MAXPHYADDR GPA windows](internal/gpa-window-narrow-maxphyaddr.md) | why the window base is computed rather than constant, and what happens when it does not fit |
| [Boundary audit, 2026-08-20](internal/audit-boundaries-2026-08-20.md) | 19 findings across all three in-scope trust boundaries; 15 fixed, 4 named as open |
| [Pre-release audit, 2026-08-21](internal/audit-prerelease-2026-08-21.md) | 12 findings, plus what was suspected and what was checked and found clean. The critical one was in code a day old and is fixed |
| [Guest pointer audit](internal/audit-guest-pointers.md) | 14 unenforced paths against one invariant, 5 since fixed |
| [Release readiness, 2026-09-01](internal/release-readiness-2026-09-01.md) | the state of every component the day before tagging |
| [Sweep plan](internal/sweep-plan.md) | how the rented-hardware matrix is chosen and run |

Both audits name **locations, not techniques**: they contain no working bypass
procedure. See [`SECURITY.md`](../SECURITY.md) for the threat model they are
audited against.

## Measurements and matrices

These live under `tests/` because they are outputs of a harness, not prose:

| | |
|---|---|
| [`tests/BOOT_MATRIX.md`](../tests/BOOT_MATRIX.md) | every boot, per-check, with the raw output — the evidence behind the platform tables |
| [`tests/perf/realapp_matrix.md`](../tests/perf/realapp_matrix.md) | the real-application host-vs-guest table and its method |
| [`tests/perf/llm_parity.md`](../tests/perf/llm_parity.md) | vLLM serving parity, both the CUDA-graph and `--enforce-eager` passes |
| [`tests/perf/results/`](../tests/perf/results/) | dated result sets, each with what was ruled out |
| [`tests/repro/`](../tests/repro/) | self-validating reproducers — run the same binary on both sides of the boundary |

## The other two repositories

nvkvm-pv is the base: the guest module, the QEMU patches, the isolate. Two
integration layers build on it, each in its own repository.

| | |
|---|---|
| [nvkvm-steamos](https://github.com/reindertpelsma/nvkvm-steamos) | SteamOS in a guest, Game Mode on the host's GPU — the gaming path, with screenshots |
| [nvkvm-kata](https://github.com/reindertpelsma/nvkvm-kata) | Kata Containers with GPU access through nvkvm |

## For agents

[`CLAUDE.md`](../CLAUDE.md) at the repository root is the orientation and
trap list written for LLM agents working in this tree: what the components are,
which docs to read before touching what, the build and measurement traps that
have each cost hours, and the rule that has retracted six bug reports.

## Reading the source

Roughly 26,000 lines. The entry points:

| | |
|---|---|
| `src/guest/nvkvm_main.c` | module init, device nodes, the ioctl path |
| `src/guest/nvkvm_ioctl.c` | parameter size table and the sanitiser |
| `src/guest/nvkvm_mmap.c` | `remap_pfn_range`, cacheability, CPU-page migration |
| `src/guest/nvkvm_virtio.c` | virtqueues, shm slots, inflight tracking |
| `src/qemu/virtio_nvgpu.c` | the QEMU device, TX dispatch, realize |
| `src/qemu/nvkvm_isolate_handlers.c` | every allowlist, `MMAP_ON_ISOLATE`, `IOCTL_ON_ISOLATE` |
| `src/qemu/nvkvm_isolate.c` | isolate spawn, sandbox, the QEMU↔stub protocol |
| `src/stub/nvkvm_stub.c` | the isolate itself |
| `src/common/nvkvm_abi.h` | the ABI profile table |
| `src/common/nvkvm_proto.h` | the guest↔QEMU wire protocol |

The comments in this codebase are unusually good — many of them record a failure
that was actually hit, with the symptom and the measurement. When a doc here
quotes a comment, the quote is the authority and the doc is the index.

## Things that do not exist

Several source comments reference documents that are not in this repository:
`docs/design/command_buffer.md`, `docs/design/virtual_modeset.md`,
`docs/design/gpa_window_pci_bar.md`, `docs/perf/forwarding_latency_decomposition.md`,
`SECURITY_MODEL.md`, `REFACTOR_PLAN.md`,
`STATE_MACHINE_PLAN.md`, `HARDENING_PLAN.md`. (`docs/audits/*`, plural, is also referenced by some comments — the real
directory is [`docs/audit/`](#audits), singular, indexed above.) They live in
the development tree.
Where their content mattered, it has been folded into the pages above.
