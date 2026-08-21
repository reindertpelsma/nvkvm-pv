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

## Where it's useful

- **A workstation you don't want to give up** — GPU work inside a VM while the
  host keeps the display and the same card.
- **One GPU, several VMs** — a homelab or shared dev box where every VM gets
  GPU access, with no card each and no `vfio-pci` rebind between starts.
- **Disposable environments** — CI runners or per-task VMs that come and go;
  attaching costs under a second and needs no device reset.
- **Kernel and driver work** — boot an experimental guest kernel without
  rebooting the host or losing the GPU.
- **Consumer GeForce** — cards that vGPU does not cover at all, with no licence.

Not yet for untrusted multi-tenant hosting — see below.

## What it is not

- **Not a hardened multi-tenant sandbox.** The guest/host boundary is not yet a
  security boundary you should rely on. It also runs in **containers**, where
  Linux namespaces are usually blocked — the isolate falls back to UID separation,
  which is weaker; see [the isolate model](docs/internal/isolate-model.md). We
  audit it ourselves and publish what we find, fixed or not: the
  [pointer audit](docs/internal/audit-guest-pointers.md) (14 unenforced paths,
  5 since fixed) and the
  [boundary audit](docs/internal/audit-boundaries-2026-08-20.md) (19 findings
  across all three trust boundaries, 15 fixed, 4 named as open). The most
  exposed surface in the second was liveness, not memory safety: an
  unprivileged guest could hang the whole VMM without corrupting anything.
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
| Guest | Linux, kernel 5.15 – 7.0 (built on every LTS in range; run-tested on Ubuntu 24.04, kernel 6.8) — see [guest kernels](docs/reference/guest-kernels.md) |
| GPU | Turing or newer — Pascal enumerates but `cuInit` fails, and the open kernel module will not probe it at all (see [Tested platforms](#tested-platforms)) |
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

### With Docker

```bash
docker compose up --build       # builds QEMU, prepares a guest, boots it
ssh -p 2222 ubuntu@127.0.0.1    # into the guest -- nvidia-smi already works
```

That is the whole setup. The image builds from this repository alone (QEMU 9.2
and the Ubuntu cloud image are fetched during the build and on first run), and
the guest comes up with the module loaded and the GPU present — no manual
staging step.

- **Requirements:** `/dev/kvm` and, for the GPU, the
  [NVIDIA container runtime](https://github.com/NVIDIA/nvidia-container-toolkit).
  Both are already in [`docker-compose.yml`](docker-compose.yml). No
  `--privileged`, no added capabilities.
- **The driver userspace is mounted read-only, not copied in.** The container
  hands the guest the host's NVIDIA libraries over a read-only 9p share and the
  guest links against them, so `apt upgrade` inside the guest cannot replace a
  driver library — there is none in its filesystem to replace.
- **`./data` is shared with the guest** at `/data`, read-write, for moving work
  in and out.
- **The guest disk is a named volume**, so rebuilding the image does not
  re-download it, and the GPU comes back on every boot (a systemd unit rebuilds
  the module against the running kernel).
- **Sizing and image:** `VM_MEM`, `VM_SMP`, and `NVKVM_GUEST_IMAGE_URL` to bring
  your own cloud image — any cloud-init capable Linux that can build an
  out-of-tree module should work, though only the default Ubuntu 24.04 is tested.

Verified end to end on an RTX 3060: cold `docker compose up` to `nvidia-smi`
inside the guest, surviving a guest reboot and a guest `apt` install of a
conflicting NVIDIA package.

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

**Graphics: Wayland works. Native Xorg does not, and that part is not ours.**
A full desktop runs on the GPU inside the guest and is interactive in a host
window at 60.0 frames/s with zero dropped frames — a browser rendering real
pages, eight concurrent EGL clients. X11 clients under `weston --xwayland` work
too; the earlier "X11 clients get no window" was a signal-restart bug in the
guest module and is fixed.

What does not work is a stock distro's own Xorg session driving nvkvm's display
head, because that path uses `modesetting` + glamor, and glamor imports its
scanout pixmap with `eglCreateImageKHR(..., EGL_NATIVE_PIXMAP_KHR, gbm_bo)`.
NVIDIA's EGL returns `EGL_BAD_PARAMETER` for that call **on bare metal too** —
we measured host and guest side by side ([the repro](tests/repro/gbm_egl_import.c)),
and dma-buf export itself survives the round trip perfectly. It is the
long-standing reason `modesetting` + glamor is not the supported combination on
NVIDIA's proprietary driver anywhere.

The combination that *is* supported on real NVIDIA hardware is NVIDIA's own X
driver, and we tried it: staged by hand, `nvidia_drv.so` and the GLX server
module both load and the stock `OutputClass` even matches nvkvm's head, but it
then fails at `Failed to initialize the NVIDIA graphics device!` because it
wants PCI BARs that nvkvm deliberately does not expose. Closing that gap would
not be enough on its own — the NVIDIA X driver drives outputs through
nvidia-modeset on the *real* GPU's display engine and physical connectors, so
it is aimed at the host's display hardware rather than at nvkvm's virtual head.
What a guest X driver should scan out to is an open design question, not a
missing forward. [Detail](docs/internal/mint-guest-desktop.md).

**A compositor in the guest can land on llvmpipe without saying so.** The VM
boots with an emulated VGA so GRUB and the early kernel have somewhere to draw,
which means the guest sees two DRM devices — `card0 -> bochs-drm` and
`card1 -> nvidia`. A compositor that takes the first one it finds renders
correctly, animates, and screenshots fine, entirely in software. Select the DRM
node **by driver name, never by index**, and check `GL renderer` says NVIDIA
before trusting any graphics number — indices move between configurations.
[How](docs/howto/run.md#running-the-guest-desktop-in-a-window).

**`kvm run failed Bad address`** — a GL client taking the guest down 10–60 s in
— has **not reproduced** since: 150 s+ of continuous glmark2, a full 20-scene
run, a five-minute soak at 190,013 frames, and an interactive desktop session.
It was originally seen on different hardware and several unrelated fixes have
landed, so treat it as open-and-unreproduced rather than closed.
[Detail](docs/internal/known-limitations.md).

**28/28 is not evidence that your workload computes correctly.** The suite
covers bring-up, the CUDA ladder, Vulkan compute and offscreen GL — it does not
cover everything, and a real correctness bug has passed it before
([what it was](docs/reference/correctness.md)). Check your own results against a
host run.

Recently closed, in case you read an older copy of this file: Vulkan compute on
Hopper was a defect in the 570 driver branch, not in nvkvm — the same part is
28/28 on 580
([the A/B](docs/reference/correctness.md#vulkan-compute-on-hopper--resolved-2026-08-21-and-it-was-never-an-nvkvm-bug)).
Guest kernels 6.12 and newer could not open the DRM nodes at all; also fixed.

## Tested applications

An independent third-party benchmark first, because it is the easiest to check:
**Geekbench 7 GPU (OpenCL) scores 99.9% of bare metal** — 48335 in the guest vs
48395 on the host, with all eleven workloads between 98.4% and 101.2%, on one
RTX 3050 Laptop GPU and one driver. Bare metal here is literal: the host side is
a physical ASUS TUF F17 laptop, not another VM
([side by side](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862)).
Both runs are public; neither is ours to edit.

The rest is guest vs host, one statically-linked binary run on both sides, RTX
3060, host driver 575.51.03, strictly serial on one GPU.

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

### Desktop graphics

A Wayland compositor runs on the GPU inside the guest, and its desktop can be
displayed and driven in a window on the host — see
[running a guest desktop in a window](docs/howto/run.md#running-the-guest-desktop-in-a-window).
Measured on an RTX 4070, driver 595.84, guest `weston --backend=drm
--renderer=gl`:

```
Using rendering device: /dev/dri/renderD128
EGL vendor: NVIDIA
GL renderer: NVIDIA GeForce RTX 4070/PCIe/SSE2
GL version: OpenGL ES 3.2 NVIDIA 595.84
```

The guest's composited frame reaches the host window as a dma-buf with no
readback (`NVKVM_PRESENT_MODE=gl`). At 1920x1080 the guest's KMS head flips at
59.9 Hz and **every one of those frames reaches the host window: 60.0 swaps/s,
zero dropped**, holding with 8 concurrent EGL clients. Measured with per-frame
counters compiled into the present path, over 60 consecutive one-second samples
of which every sample was exactly 60 frames.

The pipeline is display-refresh-bound, not overhead-bound: the per-present
PRIME export costs 0.07 ms. (An earlier figure of ~637 frames/s here came from
an unthrottled configuration and is superseded by the counter-based
measurement above, which is the one to quote.)

Graphics is the one area where the guest is well short of the host rather than
at parity: `glmark2-wayland` scores **6857 in the guest vs 21571 on the host**
on the same box (~32%). Compute and bandwidth are at 1.00x, as above; the
graphics present path has real overhead and is the honest number to quote.

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
| RTX 4070 | Ada AD104 | 595.84 | 580 | 28/28 on kernel 7.0 (26/28 before the UVM_FREE fix **) |
| RTX 4070 Ti SUPER | Ada AD103 | 595.84 | 580 | 28/28 (reproduced + fixed the above) |
| RTX 3080 | Ampere GA102 | 595.84 | 580 | 28/28 |
| RTX 3060 | Ampere GA106 | 610.43.02 | 610 | 28/28 * |
| RTX 5090 | **Blackwell GB202** | 580.178.04 | 580 | 28/28 |
| 2x RTX 4070 | Ada AD104 | 575.51.03 | 570 | 28/28, `cuda_device_count 2` |
| GTX 1660 Ti | Turing TU116 | 575.51.03 | 570 | 28/28 |
| H100 PCIe | **Hopper GH100** | 570.124.06 | 570 | 27/28 (`vk_compute_dispatch` — a 570-branch driver bug, see below) |
| H100 PCIe | **Hopper GH100** | 580.126.09 | 580 | 28/28 |
| RTX 3050 Laptop | Ampere GA107 mobile | 580.173.02 | 580 | 28/28 |
| RTX 2080 Ti | Turing TU102 | 575.51.03 | 570 | 28/28 |
| RTX 3080 | Ampere GA102 | 580.95.05 | 580 | 28/28 |
| RTX 3090 | Ampere GA102 | 580.95.05 | 580 | 28/28 |
| RTX 4060 | Ada AD107 | 580.95.05 | 580 | 28/28 |
| RTX 4090 | Ada AD102 | 580.95.05 | 580 | 28/28 |
| RTX 5070 | Blackwell GB205 | 580.95.05 | 580 | 28/28 |
| A100 80GB PCIe | **Ampere GA100** (datacenter) | 580.126.09 | 580 | 28/28 \*\*\* |

\*\*\* First datacenter GA100, and it took two fixes that had been silently
wrong on every card before it — a 3B LLM now generates in the guest at 5.75 GiB
VRAM. [What they were](docs/reference/correctness.md#two-bugs-that-only-a100-exposed).

Host/guest parity measured on that A100 with identical scripts, identical torch
(2.13.0+cu130), same box:

| workload | host | guest | ratio |
|---|---|---|---|
| **Geekbench 7 GPU (OpenCL)** | **207234** | **203098** | **98.0%** |
| fp16 matmul 8192x8192 | 246.8 TFLOP/s | 249.1 TFLOP/s | **1.01x** |
| Qwen2.5-3B generate, batch 1, greedy | 15.4 tok/s | 11.3 tok/s | **0.73x** |

The Geekbench row is the one to check first, because it is the only line here a
reader can verify without taking our word for it — both runs are published:
[side by side](https://browser.geekbench.com/v7/gpu/compare/85389?baseline=85405).
All eleven workloads land between **93.2% and 100.1%**, two of them at or above
parity (Particle Physics 100.1%, Face Tracking 100.0%); the weakest is Video
Filter at 93.2%.

The guest was given less machine than the host, and this box is itself a VM
with the A100 passed through — so 98.0% is nvkvm's cost measured while nested a
level deeper than usual, which makes it a stronger result rather than a weaker
one. [Why, and why the 3050 reads higher](docs/reference/parity.md).

Sustained compute is at parity; the 1.01x is noise. Single-stream greedy
decoding is 27% slower, which is the honest number for that shape — hundreds of
tiny launches per token with a sync between each. Anything that batches (vLLM,
llama.cpp) pays close to nothing.

OpenCL and Vulkan were checked on the same card, guest and host, and both
enumerate the A100 identically (`OpenCL 3.0 CUDA`, driver 580.126.09; Vulkan
`deviceName = NVIDIA A100 80GB PCIe`). Enumeration is not the interesting part:
OpenCL through nvkvm once returned *wrong answers* rather than failing, so
`tests/repro/opencl_correctness.c` is the check that matters. It passes on GA100
in the guest exactly as on bare metal — `ALLOC_HOST_PTR`, `USE_HOST_PTR`, 20
map/unmap cycles and an `UNORM_INT8` image, 6.3M values verified, 0 failed.

\*\* nvkvm rejected a legitimate `UVM_FREE`, which poisoned every later CUDA
call in that context. Fixed —
[detail](docs/reference/correctness.md#two-cuda-checks-failed-on-ada--driver-59584--fixed-2026-08-19).

On the H100 every CUDA and bring-up check passes — `sm_90`, PTX JIT, kernel
launch, matmul, byte-exact transfers — and OpenGL renders through the forwarder.
`vk_compute_dispatch` failed on driver 570.124.06 and **passes on 580.126.09**
(28/28); the A/B that pins that on the driver rather than on nvkvm is in
[Correctness and known issues](docs/reference/correctness.md#vulkan-compute-on-hopper--resolved-2026-08-21-and-it-was-never-an-nvkvm-bug).

Host/guest parity on that H100, same box, same scripts:

| workload | host | guest | ratio |
|---|---|---|---|
| **Geekbench 7 GPU (OpenCL)** | **265071** | **261901** | **98.8%** |
| bf16 matmul 8192x8192 | 487.3 TFLOP/s | 495.5 TFLOP/s | **1.02x** |
| Qwen2.5-7B generate, batch 1, greedy | 40.0 tok/s | 32.9 tok/s | **0.82x** |

Both Geekbench runs are published:
[side by side](https://browser.geekbench.com/v7/gpu/compare/85619?baseline=85612).
Greedy generations were byte-identical between host and guest.

Same caveats as the A100 row, and they cut the same way: this box is also a VM
underneath, and the guest got fewer cores. The 0.82x decode row is the control
path rather than the data path — [what that means](docs/reference/parity.md).

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

**Will my GPU work?**
If it is **Turing or newer** (GTX 16xx, RTX 20/30/40/50, and the datacenter
parts), yes — that is a hard requirement, not a guess. Pascal and older are out:
the open kernel module will not even probe them, and NVIDIA's 580 branch is the
last to support them at all. A card of the same architecture as one in
[Tested platforms](#tested-platforms) is expected to behave the same — the
forwarded interface is per-architecture, not per-die — so an untested RTX 4080
should match the tested RTX 4070. See
[supported drivers](docs/reference/supported-drivers.md) for the reasoning.

**Which host driver versions are covered?**
Eight ABI profiles span every published open-driver release; six of them have
been booted here, including all the ones in common use (535 LTS, 545, 550–565,
570/575, 580–595, 610). The two unbooted ones cover 515–530, whose drivers no
longer build against a modern kernel. Full matrix, and what "unbooted" means for
your risk, in [supported drivers](docs/reference/supported-drivers.md).

**Does the guest need an NVIDIA driver?**
No kernel driver — the guest loads `nvkvm-guest.ko`, which presents `/dev/nvidia*`
itself. It does need the matching userspace libraries, staged from the host by
[`stage_guest_libs.sh`](scripts/stage_guest_libs.sh).

**Which guest distros are supported?**
The distro does not matter; the **kernel version** does. `nvkvm-guest.ko` is
built against the guest's own headers, and it builds on **5.15, 6.1, 6.6, 6.8,
6.12, 6.14, 6.19 and 7.0** — every LTS in the range NVIDIA's driver supports,
plus current stable — in both the graphics and compute-only variants. Verify any
kernel yourself with `bash tests/kernel_matrix.sh`, which needs Docker and
nothing else. Table and the API differences it papers over:
[guest kernels](docs/reference/guest-kernels.md).

Caveat worth stating: those are **build** results. Ubuntu 24.04 (6.8) is the
one that gets booted and run through `validate.sh`; a compile pass says the API
surface matches, not that the module behaves. Windows guests are not
supported.

**Does the host driver version have to match the guest's?**
Yes. The libraries staged into the guest come from the host, so they are the same
build by construction. See [ABI profiles](docs/reference/abi-profiles.md).

**Can several VMs share one GPU?**
Yes — each guest process gets its own isolate on the host, so they are separate
address spaces sharing the device the same way host processes do.

**What's the performance cost?**
Close to nothing on throughput, and a real cost on latency. Sustained compute
and bandwidth measure at parity (1.00x) on every workload in
[Tested applications](#tested-applications), and Geekbench 7 GPU — an
independent benchmark, both runs public — scores
[99.9% of bare metal](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862).

What costs is any workload dominated by small serialized control calls, because
each one is a forwarded round trip. The sharpest measured case is LLM prefill on
a *tiny* (~5-token) prompt: 0.71x, which is launch latency rather than prefill
compute — on a realistic long prompt the same measurement is 0.98x. Alloc churn
behaves the same way. If your workload is a stream of tiny GPU calls rather than
sustained work, budget for that; otherwise you will not notice.

**Is it safe to run untrusted guests?**
Not yet — treat it as experimental. The ioctl and alloc-class gates are
default-deny and the guest kernel module is untrusted by design, but the code
has had no *external* security review. It has had two internal ones, both
published with their open findings rather than only their fixed ones: the
[pointer audit](docs/internal/audit-guest-pointers.md) and the
[boundary audit](docs/internal/audit-boundaries-2026-08-20.md). Read the second
before deciding: it found that an unprivileged guest could hang the entire VMM
without corrupting a single byte, which is the kind of thing a "no known memory
bugs" summary hides. See also
[the isolate model](docs/internal/isolate-model.md).

**Can nvkvm itself run inside a container?**
Yes, and much of the testing is done that way — there is a
[`Dockerfile`](Dockerfile) and a [`docker-compose.yml`](docker-compose.yml) for
exactly this. A default container is enough:

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
Two causes, and the second is easy to miss. Either the NVIDIA userspace
libraries did not stage — see
[staging guest libraries](docs/howto/stage-guest-libraries.md) — or the user
running the client is not in the guest's `video` and `render` groups, so opening
the render node returns `EACCES` and the stack falls back silently. Nothing
errors; you simply get software rendering that looks like a working GPU until
you check the renderer string. `scripts/setup_guest.sh` puts the default user in
both groups.

**Does CUDA give bit-identical results to the host?**
On everything measured, yes — including token-identical LLM output at
temperature 0, and Geekbench 7 GPU at 99.9% of bare metal with every workload
validating. Verify your own workload against a host run all the same; see
[Known issues](#known-issues).

## Documentation

| | |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The request path end to end, and how the hard problems are solved |
| [`docs/howto/`](docs/howto/) | Building, running, staging guest libraries, adding a driver version |
| [`docs/reference/`](docs/reference/) | ABI profiles, allowlists, virtio protocol, device nodes |
| [Correctness](docs/reference/correctness.md) | What is known to be wrong, how far it is traced, how to reproduce it |
| [Reading the parity numbers](docs/reference/parity.md) | What the host/guest ratios do and do not establish |
| [Guest kernels](docs/reference/guest-kernels.md) | Which guest kernels the module builds on, measured, and why the range is narrow |
| [`docs/internal/`](docs/internal/) | Design rationale, forwarding model, isolate model, known limitations |
| [Security audits](docs/internal/audit-boundaries-2026-08-20.md) | Both audits, findings and status — locations, not techniques |

## Status

Experimental — a research artifact, not a supported product. It runs real
workloads at host parity on six GPU architectures (Turing, Ampere, Ada,
Blackwell, and the GA100 and Hopper datacenter parts), including multiple GPUs
in one guest.

The largest open item is that a stock distro's own Xorg session cannot drive the
display head; everything else is tracked in
[known limitations](docs/internal/known-limitations.md). Several entries that
used to sit here have been re-tested and no longer hold — Wayland GL, the NVENC
hang, Vulkan compute on Hopper, and DRM on guest kernels 6.12+.

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
