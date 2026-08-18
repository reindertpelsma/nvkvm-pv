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
- **keep your existing container workflow** — Docker with
  `nvidia-container-toolkit` works unmodified inside the guest, so
  `docker run --gpus all` behaves as it does on the host
- **pass several GPUs to one guest** — multiple devices are autodetected and
  independently usable, verified concurrently on two cards

It is fast because the guest is not in a hot path. Control calls are forwarded;
the work itself is not. A kernel launch reaches the GPU as a store to a mapped
doorbell page, so there is no per-operation cost to pay:

> **A 32-billion-parameter model served through vLLM runs at 0.99-1.00x of host
> speed inside the guest, and produces token-identical output at temperature 0.**
> Seventeen other workloads — PyTorch, ResNet-50, BERT, Vulkan compute — land
> between 0.95x and 1.00x. [See the numbers](#tested-applications).

## What it is not

- **Not a hardened multi-tenant sandbox.** The guest/host boundary is not yet a
  security boundary you should rely on. It also runs in **containers**, where
  Linux namespaces are usually blocked — the isolate falls back to UID separation,
  which is weaker; see [the isolate model](docs/internal/isolate-model.md). We audited it ourselves and published what
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

**Guest pointers are not meant to cross, and the host is what enforces it.** The
guest sanitiser zeroes pointer-sized fields carrying a guest VA, but that runs in
the guest and is therefore not a control — a malicious guest simply skips it. The
host boundary overwrites those fields itself. Enforcement today is per-ioctl and
hand-written rather than categorical, and is **not yet complete**: we audited it
and published what we found, open items included, in
[the pointer audit](docs/internal/audit-guest-pointers.md).

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
| PyTorch matmul fp16 (tensor cores) | 26.03 | 26.03 TFLOP/s | 1.00x |
| ResNet-50 training step | 199.5 | 199.6 img/s | 1.00x |
| BERT encoder | 277.6 | 277.3 seq/s | 1.00x |
| Vulkan compute (vkpeak) | 8947.2 | 8975.3 GFLOP/s | 1.00x |
| OpenGL offscreen (EGL) | 1.8 | 1.8 Mtri/s | 1.00x |
| reduction bandwidth | 122.0 | 119.7 GB/s | 0.98x |
| Mandelbrot | 2.90e6 | 2.76e6 | 0.95x |

Eleven more — N-body, Black-Scholes, SHA-256, 2D convolution, ResNet-50
inference and AMP, ViT-B/16, PyTorch fp32 — all land at 1.00x. Full table and
method: [`tests/perf/realapp_matrix.md`](tests/perf/realapp_matrix.md).

Where the guest is measurably slower it is latency-bound control paths, never
sustained compute or bandwidth.

### Containers

Docker with `nvidia-container-toolkit` works inside the guest with no special
handling — standard install, `docker run --gpus all`, and the GPU appears:

```
$ docker run --rm --gpus all nvidia/cuda:12.9.0-base nvidia-smi
NVIDIA-SMI 575.51.03   Driver Version: 575.51.03   CUDA Version: 12.9
0  NVIDIA GeForce RTX 3070   Off   00000000:00:07.0
```

Process enumeration is namespace-correct too — `nvidia-smi` inside a container
sees only that container's GPU processes, never a host PID. See
[device nodes](docs/reference/device-nodes.md).

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

### Fine-tuning

A real Kaggle ARC-AGI notebook runs unmodified in the guest on an RTX 5090
(driver 580.178.04): Unsloth 2025.9.7 LoRA fine-tuning **Qwen3**, with
`embed_tokens` and `lm_head` trained in mixed precision, then decode + scoring
inference over the augmented puzzle set.

```
Unsloth 2025.9.7: Fast Qwen3 patching. Transformers: 4.55.4
NVIDIA GeForce RTX 5090. Num GPUs = 1. Max memory: 31.356 GB
Torch: 2.13.0+cu130. CUDA Toolkit: 13.0. Triton: 3.7.1. Bfloat16 = TRUE
[Rank 0] allocated 13059MB for training
TrainOutput(global_step=128, train_runtime=118.57, train_steps_per_second=1.079)
[Rank 0] allocated 14767MB for inference
[Rank 0] finished 0934a4d8 in 177.2s
```

Host parity: **—** (not measured yet — no host-side baseline has been run for
this workload, which is not a statement that parity was missed). What this run
does establish is that it works: training as well as inference, multi-GB
allocation churn between the training and inference phases, and the Triton /
xformers paths, all through the forwarder.

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
| RTX 5090 | **Blackwell GB202** | 580.178.04 | 580 | 28/28 |
| 2x RTX 4070 | Ada AD104 | 575.51.03 | 570 | 28/28, `cuda_device_count 2` |
| RTX 3050 Laptop | Ampere GA107 mobile | 580.173.02 | 580 | 28/28 |

\* Both read 27/28 until the NVKMS allowlist was fixed on 2026-08-17, and the
595.84 row has no 28/28 measurement — its box was re-provisioned without
`libnvidia-ptxjitcompiler`. Full detail in
[`tests/BOOT_MATRIX.md`](tests/BOOT_MATRIX.md).

NVIDIA guarantees no ioctl ABI stability across driver releases, so `nvkvm` keys
struct layouts off the host driver version. **Eight profiles** cover every
open-kernel-modules release from 515 to 610, measured by compiling `sizeof` /
`offsetof` probes against 61 upstream tags — and keyed on the full
`major.minor.patch`, because two widely deployed branches change layout *inside*
the branch. **Six of the eight have been booted**; no profile's values proved
wrong in practice. See [ABI profiles](docs/reference/abi-profiles.md) and
[supported drivers](docs/reference/supported-drivers.md).

Verify any host yourself, inside a guest:

```bash
sudo bash /mnt/nvkvm/tests/validate.sh
```

28 checks — device nodes, the full CUDA ladder, Vulkan compute, offscreen GL.
Needs only a C compiler, and a software-rasteriser fallback is an explicit
**FAIL**, not a pass.

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
