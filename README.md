# nvkvm

Run CUDA, PyTorch and Vulkan inside a KVM guest — on the same GPU your host is
still using.

![nvkvm booting a guest and driving the host GPU from inside it](docs/img/boot.gif)

<sub>Real recording on an RTX 3050 Laptop GPU: cold boot to `nvidia-smi` inside
the guest, with the host still holding the card. Idle time is fast-forwarded;
nothing printed was cut. ([asciinema cast](docs/img/boot.cast) ·
[how it was made](tools/demo/README.md))</sub>

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

## Known issues

**A guest can return silently wrong results.** An OpenCL program that repeatedly
maps and unmaps a pinned buffer, after other buffers have been freed, reads
zeros instead of its results — no error, no crash. Geekbench 7 `--gpu` fails
validation on 11 workloads in the guest while the identical binary is clean on
the host ([guest](https://browser.geekbench.com/v7/gpu/79890) ·
[host](https://browser.geekbench.com/v7/gpu/79862)). `validate.sh` passes on that
same guest, so **28/28 is not evidence that your workload computes correctly** —
check your own results against a host run.

OpenCL is therefore off by default, and Vulkan compute fails on Hopper. Full
detail, bisection and reproducers:
[Correctness and known issues](docs/reference/correctness.md).

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

A real Kaggle ARC-AGI notebook runs unmodified in the guest on an RTX 5090:
Unsloth LoRA fine-tuning **Qwen3** (128 steps, 13 GB allocated), then decode and
scoring inference at 14.7 GB — a full puzzle in 177 s. Training as well as
inference, multi-GB allocation churn between the two phases, and the Triton and
xformers paths, all through the forwarder.

Host parity: **—**, not measured yet for this workload; that is a gap in the
measurements, not a known shortfall.

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
| GTX 1660 Ti | Turing TU116 | 575.51.03 | 570 | 28/28 |
| H100 PCIe | **Hopper GH100** | 570.124.06 | 570 | 27/28 (`vk_compute_dispatch`, see below) |
| RTX 3050 Laptop | Ampere GA107 mobile | 580.173.02 | 580 | 28/28 |

On the H100 every CUDA and bring-up check passes — `sm_90`, PTX JIT, kernel
launch, matmul, byte-exact transfers — and OpenGL renders through the forwarder.
The one failure is `vk_compute_dispatch`, and it does **not** affect CUDA; the
trace, and the two hypotheses ruled out by experiment, are in
[Correctness and known issues](docs/reference/correctness.md#vulkan-compute-on-hopper).

## FAQ

**Is this vGPU or SR-IOV?**
No. There is no hardware partitioning and no vendor licence. nvkvm forwards the
driver's ioctl interface, so it runs on consumer cards that have no vGPU support
at all.

**So how are resources divided between guests?**
They are not. Nothing is partitioned: guests share VRAM, SMs and bandwidth
dynamically, exactly as GPU containers on one card do today. There is no
per-guest VRAM reservation and no quota, so one guest can exhaust the card for
the others. If you need hard partitioning, MIG sits *below* the interface nvkvm
forwards and should compose with it — but that is untested.

**Does the guest need an NVIDIA driver?**
No kernel driver — the guest loads `nvkvm-guest.ko`, which presents `/dev/nvidia*`
itself. It does need the matching userspace libraries, staged from the host by
[`stage_guest_libs.sh`](scripts/stage_guest_libs.sh).

**Does the host driver version have to match the guest's?**
Yes. The libraries staged into the guest come from the host, so they are the same
build by construction. See [ABI profiles](docs/reference/abi-profiles.md).

**Can several VMs share one GPU?**
Yes — each guest process gets its own isolate on the host, so they are separate
address spaces sharing the device the same way host processes do.

**What's the performance cost?**
Sustained compute and bandwidth measure at parity (1.00x) on every workload in
[Tested applications](#tested-applications). Where the guest is slower it is
latency-bound control paths, not throughput.

**Is it safe to run untrusted guests?**
Not yet — treat it as experimental. The ioctl and alloc-class gates are
default-deny and the guest kernel module is untrusted by design, but the code
has not had an external security review. See
[the isolate model](docs/internal/isolate-model.md).

**Can nvkvm itself run inside a container?**
Yes, and much of the testing is done that way. A default container is enough:

```bash
docker run --gpus all --device /dev/kvm ...
```

No `--privileged`, no added capabilities, default seccomp and AppArmor. Rootless
Docker works on the same terms, as long as your user can open `/dev/kvm`.

This is a useful way to run it today: the isolates are weaker inside a container
(namespaces are usually blocked, so they fall back to UID separation), but the
container boundary sits *outside* the VMM, so breaking out of the VMM lands the
attacker in the container rather than on the host.

**Why is my GPU showing as llvmpipe?**
The guest is falling back to software rendering because the NVIDIA userspace
libraries did not stage. See
[staging guest libraries](docs/howto/stage-guest-libraries.md).

**Does CUDA give bit-identical results to the host?**
On everything measured, yes — including token-identical LLM output at
temperature 0. But read [Known issues](#known-issues) first: there is a path
that returns wrong results silently.

## Documentation

| | |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The request path end to end, and how the hard problems are solved |
| [`docs/howto/`](docs/howto/) | Building, running, staging guest libraries, adding a driver version |
| [`docs/reference/`](docs/reference/) | ABI profiles, allowlists, virtio protocol, device nodes |
| [Correctness](docs/reference/correctness.md) | What is known to be wrong, how far it is traced, how to reproduce it |
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
