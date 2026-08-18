# nvkvm

Run CUDA, PyTorch and Vulkan inside a KVM guest — on the same GPU your host is
still using.

`nvkvm` gives a virtual machine real, driver-level access to an NVIDIA GPU
without handing the card over to it. The host keeps the GPU. The guest gets
`/dev/nvidia0` and friends, backed by a small kernel module that forwards the
NVIDIA Resource Manager ioctl surface over virtio to the host driver.

Unmodified NVIDIA userspace runs inside the guest. Not a CUDA shim, not an API
remoting layer, not a container: the guest's own `libcuda` talks to what it
believes is a real driver, and the work executes on real hardware.

From a user's perspective, this is what it buys you:

- give a VM GPU access **without PCIe passthrough**, so the host keeps the card
- run **several VMs against one GPU**, and the host desktop alongside them
- attach a guest to a GPU **in under a second** — no driver re-initialisation,
  no device reset, no `vfio-pci` rebind
- run **consumer GeForce hardware**, with no vGPU licence and no datacenter SKU
- keep a **real VM boundary** — the guest never receives the physical device,
  gets no MMIO window to it, and has no DMA path to host memory
- **bring your own guest image** — a stock NVIDIA driver, not a licensed
  vGPU guest driver bound to one vendor's cloud

It is fast because the guest is not in a hot path. Control calls are forwarded;
the work itself is not. A kernel launch reaches the GPU as a store to a mapped
doorbell page, so there is no per-operation cost to pay:

> **A 32-billion-parameter model served through vLLM runs at 0.99-1.00x of host
> speed inside the guest, and produces token-identical output at temperature 0.**
> Seventeen other workloads — PyTorch, ResNet-50, BERT, Vulkan compute — land
> between 0.95x and 1.00x. [See the numbers](#tested-applications).

## What it is not

- **Not a hardened multi-tenant sandbox.** The guest/host boundary is not yet a
  security boundary you should rely on. We audited it ourselves and published what
  we found — 14 unenforced paths with severities and containment, four since fixed
  — in [the pointer audit](docs/internal/audit-guest-pointers.md).
  **Do not put untrusted tenants behind it.**
- **Not a virtual monitor.** There is no scanout path. A GPU-accelerated desktop
  runs inside the guest and frames leave by *capture*, not by a virtual display.
  See [Known limitations](docs/internal/known-limitations.md).
- **Not vGPU.** No SR-IOV, no hardware partitioning, no MIG. Sharing is
  cooperative, at the driver interface.
- **Not a Windows guest solution.** Linux guests only.
- **Pinning host memory is slower than native**, and one registration is capped
  at 2 GiB. Stock vLLM starts and runs; see
  [known limitations](docs/internal/known-limitations.md#pinned-host-memory) for
  the numbers.

## Requirements

| | |
|---|---|
| Host | Linux with KVM, an NVIDIA GPU, and the proprietary/open NVIDIA driver installed |
| Guest | Linux (tested on Ubuntu 24.04, kernel 6.8) |
| GPU | Turing or newer (see [Tested platforms](#tested-platforms)) |
| Driver | See [supported drivers](docs/reference/supported-drivers.md) |
| Build | QEMU 9.2 is built from source by the provided script |

## Build

```bash
git clone https://github.com/reindertpelsma/nvkvm-pv.git nvkvm && cd nvkvm
bash scripts/build_qemu.sh          # builds the isolate stub, then QEMU 9.2 with the nvkvm device
bash scripts/setup_guest.sh         # fetches an Ubuntu 24.04 cloud image and prepares a disk
```

The guest also needs NVIDIA userspace libraries version-matched to your **host**
driver. Assemble the bundle and stage it:

```bash
V=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
bash scripts/make_host_bundle.sh    # collects host-libs-$V/ from the installed driver
```

## First result

```bash
bash scripts/run_test_vm.sh
```

Then, inside the guest (`ssh ubuntu@localhost -p 2222`, password `ubuntu`):

```bash
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh
nvidia-smi
```

```
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 575.51.03              Driver Version: 575.51.03      CUDA Version: 12.9     |
|   0  NVIDIA GeForce RTX 3060        Off |   00000000:00:07.0                            |
|  0%   44C    P8              6W /  170W |       1MiB /  12288MiB |      0%      Default |
+-----------------------------------------------------------------------------------------+
```

That is a guest enumerating a GPU the host has not given up.

## How it fits together

```
  GUEST                                        HOST
  ─────────────────────────────────────        ──────────────────────────────
  CUDA / PyTorch / Vulkan / OpenGL
    │  ioctl(/dev/nvidia*)
    ▼
  nvkvm-guest.ko                               QEMU: virtio-nvgpu device
    │  sanitise: zero guest VAs,                 │  validate against allowlist
    │  stage params in the aux slot              │  translate handles + fds
    │  virtio                                    ▼
    └──────────────────────────────────────►  per-process isolate  ──► NVIDIA driver ──► GPU
                                                (own RM client, own address space)
```

Three properties fall out of that shape:

**The guest never gets the device.** No BAR is mapped to it, no MMIO window is
handed over, and there is no DMA path from the guest to host memory. Compare
PCIe passthrough, where the GPU retains DMA access to host RAM and the isolation
boundary is weaker than the VM boundary suggests.

**No guest pointer is ever forwarded.** The guest sanitiser zeroes every
pointer-sized field carrying a guest VA before the call crosses, and the host
boundary overwrites those fields unconditionally rather than trusting the guest
to have done it.

**In steady state there is no forwarded call at all.** Control operations cross
the boundary; work does not. A kernel launch reaches the GPU as a write-combining
store to a mapped BAR doorbell page — there is no doorbell interception anywhere
in this codebase. The project measured control-RTT at only 1–2% of per-token LLM
decode time, which is why the numbers below are ratios near 1.00 rather than a
fraction of host speed.

Full detail: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Tested applications

Guest vs host, one statically-linked binary run on both sides, RTX 3060, host
driver 575.51.03, strictly serial on one GPU.

| workload | host | guest | ratio |
|---|---|---|---|
| memory bandwidth (triad) | 336.2 | 336.3 GB/s | 1.00x |
| N-body | 4615.5 | 4605.3 GFLOP/s | 1.00x |
| Black-Scholes | 20974.6 | 20963.8 Mopt/s | 1.00x |
| 2D convolution | 1176.6 | 1180.6 GFLOP/s | 1.00x |
| SHA-256 | 756.0 | 756.0 MH/s | 1.00x |
| reduction bandwidth | 122.0 | 119.7 GB/s | 0.98x |
| Mandelbrot | 2.90e6 | 2.76e6 | 0.95x |
| PyTorch matmul fp32 | 9.02 | 8.98 TFLOP/s | 1.00x |
| PyTorch matmul fp16 (tensor cores) | 26.03 | 26.03 TFLOP/s | 1.00x |
| ResNet-50 inference | 615.8 | 614.6 img/s | 1.00x |
| ResNet-50 inference (AMP) | 1091.2 | 1092.4 img/s | 1.00x |
| ResNet-50 training step | 199.5 | 199.6 img/s | 1.00x |
| ViT-B/16 inference | 165.2 | 164.8 img/s | 1.00x |
| BERT encoder | 277.6 | 277.3 seq/s | 1.00x |
| Vulkan compute (vkpeak) | 8947.2 | 8975.3 GFLOP/s | 1.00x |
| OpenGL offscreen (EGL) | 1.8 | 1.8 Mtri/s | 1.00x |

### LLM serving

vLLM 0.11.0 serving **Qwen2.5-32B-Instruct-AWQ** (32.5B params, 19.34 GB) on an
RTX 6000 Ada, driver 575.51.03. Same venv on both sides, byte-identity proven by
sha256 over every `.py`/`.so`.

| | host | guest | ratio |
|---|---|---|---|
| prefill, 8245-token prompt | 1527.9 | 1532.9 tok/s | 1.00x |
| prefill, 16384-token context | 1374.0 | 1378.1 tok/s | 1.00x |
| decode, steady state | 41.68 | 41.08 tok/s | 0.99x |
| batch x32, output | 244.2 | 244.3 tok/s | 1.00x |
| batch x64, total | 1311.4 | 1311.9 tok/s | 1.00x |

At temperature 0 the guest produced **token-id identical** output to the host on
all three tasks (long-chain reasoning, C11 lock-free ring codegen, 8k-token
summarisation).

Two caveats, both stated in [the full results](tests/perf/llm_parity.md):
pinned host buffers were disabled **on both sides identically**, because at the
time the guest could not pin more than 16 MiB; and with CUDA graphs disabled
(`--enforce-eager`) guest decode falls to **0.82x**, because the per-launch
forwarding cost is real and graph capture is what hides it.

The first caveat no longer applies to the code — the 16 MiB cap has since been
removed and stock vLLM starts in a guest with pinned buffers enabled — but the
throughput table above **has not been re-measured** with pinning on, so it is
still a no-pinned-buffers comparison on both sides. Re-running it is future
work.

A Wayland compositor also runs on the GPU inside the guest — headless weston
reporting `GL renderer: NVIDIA GeForce RTX 3060`, compositing its own shell at
1920x1080. Its GL **clients** do not render: they reach the GPU but never present
a frame. See [known limitations](docs/internal/known-limitations.md).

Where the guest is measurably slower it is latency-bound control paths, never
sustained compute or bandwidth. Reproduce with `tests/perf/run_matrix.sh`.

## Tested platforms

Every row below reached a real CUDA kernel launch through the forwarder.

| GPU | architecture | host driver | ABI profile | `validate.sh` |
|---|---|---|---|---|
| RTX 3060 | Ampere GA106 | 575.51.03 | 570 | — |
| RTX 4000 Ada | Ada AD104 | 575.51.03 | 570 | — |
| GTX 1660 SUPER | Turing TU116 | 575.51.03 | 570 | — |
| GTX 1660 SUPER | Turing TU116 | 535.309.01 | 535 | — |
| RTX 3060 | Ampere GA106 | 545.23.08 | 545 | 28/28 |
| RTX 3060 | Ampere GA106 | 550.54.14 | 550 | 28/28 |
| RTX 3060 Ti | Ampere GA104 | 580.95.05 | 580 | 28/28 |
| RTX 3060 Ti | Ampere GA104 | 595.84 | 580 | `gl_draw_pixel_check` PASS * |
| RTX 3060 | Ampere GA106 | 610.43.02 | 610 | 28/28 * |

\* These two read 27/28 until 2026-08-17: `gl_draw_pixel_check` failed with
`GL_FRAMEBUFFER_UNSUPPORTED` on every attachment format. The cause was nvkvm's
own NVKMS allowlist, which was captured on a 575-era session and denied a
`cmdType` that branches 595+ need for offscreen render targets — not a driver
regression; the same probe passes on bare metal on both drivers. After the fix
610.43.02 is a clean 28/28. **There is no 28/28 measurement for 595.84**: the
box it ran on was re-provisioned from a component-package subset that omits
`libnvidia-ptxjitcompiler`, so `cuda_ptx_jit` now FAILs there and the run scores
25/28 for a reason unrelated to nvkvm. What was re-measured on 595.84 after the
fix is `gl_draw_pixel_check` PASS and `0/5 configurations incomplete`. See
[`tests/BOOT_MATRIX.md`](tests/BOOT_MATRIX.md).

NVIDIA guarantees no ioctl ABI stability across driver releases, so `nvkvm`
keys struct layouts off the host driver version. **Eight profiles** cover every
open-kernel-modules release from 515 to 610, derived by compiling `sizeof` /
`offsetof` probes against 61 upstream tags (`tools/abi_derive.sh`).

Selection is keyed on the full `major.minor.patch`, not the major alone, because
two widely deployed branches change layout *inside* the branch — and 535 does so
non-monotonically: the long-lived 535.43.x maintenance train picked up the
Confidential Computing channel fields at 535.43.08 (2023-08-17), which is newer
in wall-clock time than 535.54.03 (2023-06-14) despite sorting older.

The table is derived for all eight; **six of the eight have been booted** and put
through the validation suite. `515` and `525` are underived-by-boot only because
the drivers selecting them do not build on kernel 6.8, which every KVM-capable
test host ran — they need a host on kernel <= 6.5. No profile's table values
proved wrong in practice.

Run the suite yourself inside a guest:

```bash
sudo bash /mnt/nvkvm/tests/validate.sh
```

28 checks covering device nodes, the full CUDA ladder (including a real kernel
launch and the PTX JIT path), Vulkan compute and offscreen GL. It needs only a C
compiler — the probes `dlopen` their libraries. A software-rasteriser fallback
(llvmpipe/lavapipe/swrast) is an explicit **FAIL**, not a pass. Results per
driver are in [`tests/BOOT_MATRIX.md`](tests/BOOT_MATRIX.md); see also
[supported drivers](docs/reference/supported-drivers.md).

## Documentation

| | |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The request path end to end, and how the hard problems are solved |
| [`docs/howto/`](docs/howto/) | Building, running, staging guest libraries, adding a driver version |
| [`docs/reference/`](docs/reference/) | ABI profiles, allowlists, virtio protocol, device nodes |
| [`docs/internal/`](docs/internal/) | Design rationale, forwarding model, isolate model, known limitations |

## Status

Experimental. It runs real workloads at host parity on three GPU architectures,
and it is a research artifact rather than a supported product. Known open items
are tracked in [known limitations](docs/internal/known-limitations.md); notably
hardware video encode (NVENC) and GL clients under Wayland are not currently
working and are under investigation.

Issues and measurements from other hardware are welcome — particularly boots on
driver branches this repository has not exercised.

## Credits

`nvkvm` derives substantially from **gVisor's `nvproxy`** (Apache-2.0) for the
ioctl allowlist model, object tracking and frontend handling, and from
**NVIDIA's open-gpu-kernel-modules** for ABI struct definitions. Per-file
attribution is in [`CREDITS`](CREDITS).

Two other public non-vendor efforts at driver-level NVIDIA GPU virtualization
are worth reading: [`nestrilabs/virtio-nvgpu`](https://github.com/nestrilabs/virtio-nvgpu)
(virtio transport, libkrun, KVM memslot mmap) and
[`straylight-software/isospin-microvm`](https://github.com/straylight-software/isospin-microvm)
(vsock transport, Firecracker workers borrowing a GPU-owning VM).

## Licence

Apache-2.0, except the guest kernel module (`src/guest/`), which is GPL-2.0 as
required for kernel symbol access. See [`LICENSE`](LICENSE).
