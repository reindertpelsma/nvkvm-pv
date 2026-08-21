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
| [Build](howto/build.md) | the isolate stub, the patched QEMU, the guest kernel module |
| [Run a guest](howto/run.md) | image preparation, the QEMU command line, first bring-up, what to check |
| [Stage the guest NVIDIA userspace](howto/stage-guest-libraries.md) | `make_host_bundle.sh` on the host, `stage_guest_libs.sh` in the guest, and why the failures here are silent |
| [Add a driver version](howto/add-a-driver-version.md) | measuring a new ABI profile row and what else may need to change |
| [The QEMU patch series](../patches/README.md) | the four patches applied to upstream QEMU 9.2, and why each one exists |

## Reference

| | |
|---|---|
| [ABI profiles](reference/abi-profiles.md) | the version-keyed table, how it is measured, how it reaches all three components |
| [Supported drivers and GPUs](reference/supported-drivers.md) | which profile rows have actually been booted, and which have only been measured |
- [Correctness and known issues](reference/correctness.md) — what is known to be wrong, how far it is traced, and how to reproduce it
| [Allowlists](reference/allowlists.md) | all six default-deny gates, their contents and provenance |
| [Virtio protocol](reference/virtio-protocol.md) | virtqueues, shared memory, request types, GPA windows |
| [Device nodes](reference/device-nodes.md) | what appears in the guest, major/minor numbers, and why each one exists |
| [Guest userspace libraries](reference/guest-userspace-libraries.md) | every library the guest needs, what breaks without it, and the version-matching traps |

## Internal

| | |
|---|---|
| [Design rationale](internal/design-rationale.md) | the choices that shaped the architecture, and what was tried and rejected |
| [The forwarding model](internal/forwarding-model.md) | sanitiser, aux slot, handle translation, per-command special cases |
| [The isolate model](internal/isolate-model.md) | why one process per guest process, what the sandbox is, what the trust boundary is |
| [Known limitations](internal/known-limitations.md) | open bugs, intrinsic limits, and numbers that should not be quoted |

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
`docs/audits/*`, `SECURITY_MODEL.md`, `REFACTOR_PLAN.md`,
`STATE_MACHINE_PLAN.md`, `HARDENING_PLAN.md`. They live in the development tree.
Where their content mattered, it has been folded into the pages above.
