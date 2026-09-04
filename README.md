# nvkvm

Run CUDA, PyTorch, Vulkan or a graphical display inside a KVM guest, on the same GPU your host is
still using.

![nvkvm booting a guest and driving the host GPU from inside it](docs/img/boot.gif)

<sub>Real recording on an RTX 3050 Laptop GPU: cold boot to `nvidia-smi` inside
the guest, with the host still holding the card. Idle time is fast-forwarded;
nothing printed was cut. ([asciinema cast](docs/img/boot.cast) ·
[how it was made](tools/demo/README.md))</sub>

`nvkvm` gives a virtual machine real, driver-level access to an NVIDIA GPU
without handing the card over to it. It feels like a CUDA container upgraded with actual VM support.

`/dev/nvidia0` and friends, backed by a small kernel module that forwards the
NVIDIA driver's own API interface over virtio to the host driver. Unmodified
NVIDIA userspace runs inside the guest — not a CUDA shim, not an API remoting
layer, not a shared kernel container.

## Why it exists

This project fixes the limitations of the alternatives:
- VFIO gives up the entire card to the VM, you cannot share it with multiple VMs, if you have a display on it then your host desktop is unrenderable
- vGPU is only for licensed datacenter parts. Even the community attempt vgpu_unlock does not work on most recent nvidia cards. This only requires vanilla nvidia access and KVM without any drastic host kernel/OS changes. 
- cuda containers do not give your a VM, no stock OS, no VM boundary, limited root, no docker-in-docker with GPU without severely weakining the container, many desktop apps failing.

It provides you
- give a VM GPU access without PCIe passthrough, so the host keeps the card.
- run several VMs against one GPU, and the host desktop alongside them
- attach a guest to a GPU **in under a second** — no device reset, no
  `vfio-pci` rebind
- run consumer GeForce hardware, with no vGPU licence and no datacenter SKU
- pass several GPUs to one guest - autodetected and independently usable,
  verified on up to six cards
- Get the actual GPU accelerated display in the VM on your desktop - zero copy native, no PCIe round trips when not needed.
- Run QEMU in a container for additional isolation and environment consistency.

This project is both intended for compute workload VMs (e.g deep learning) and for graphics VMs (e.g gaming)

## Requirements

| | |
|---|---|
| Host | Linux with working KVM (confirm virtualiziation technology is enabled in BIOS), an NVIDIA GPU, and the NVIDIA driver - either module flavour, proprietary or open ([which](docs/reference/supported-drivers.md)) |
| Guest | Linux, kernel 5.15 – 7.0 ([guest kernels](docs/reference/guest-kernels.md)) |
| GPU | Turing or newer - Volta/Pascal/Maxwell support is in the making |
| Size | Recommended 16 GB RAM and 4 vCPUs for the guest and 40 GB disk |

Check you can open `/dev/kvm` before anything else - not CPU flags, which a
container inherits from its host:

```bash
exec 3<>/dev/kvm && echo "KVM usable" && exec 3>&-
```

Without `/dev/kvm`, QEMU silently falls back to software emulation: it appears
to work and is unusably slow.
Permissions, containers and nested virt can be found here: [install guide](docs/howto/install.md).

## Quickstart

```bash
docker run --rm -it --device /dev/kvm --gpus all \
    -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics,display,video \
    -p 127.0.0.1:2222:2222 -v nvkvm-guest:/opt/nvkvm-guest \
    ghcr.io/reindertpelsma/nvkvm-pv:v0.2.4
ssh -p 2222 ubuntu@127.0.0.1    # password: ubuntu -- nvidia-smi already works
```

Keep the `127.0.0.1:` in the `-p` flag. The guest is `ubuntu:ubuntu` with
`NOPASSWD:ALL`; published as a bare `-p 2222:2222` it is a root shell with your
GPU attached, offered to your whole network.

`NVIDIA_DRIVER_CAPABILITIES` is not optional decoration: `--gpus all` alone
gives the container `compute,utility`, and the guest then gets a compute-only
driver with no GL or Vulkan — which does not fail, it silently falls back to
llvmpipe.

A [prebuilt tarball](https://github.com/reindertpelsma/nvkvm-pv/releases) runs
on a bare host, and `bash scripts/build_qemu.sh --install-deps` builds from
source. Both, plus the container knobs and the attestation check:
[install guide](docs/howto/install.md).

## First result

Inside the guest:

```bash
nvidia-smi                              # a guest enumerating a GPU the host has not given up
bash /mnt/nvkvm/tests/validate.sh
```

```
 TOTAL 28   PASS 28   FAIL 0   SKIP 0
 VERDICT: PASS (all 28 checks passed)
```

Exits 0 on a full pass, 1 on failure, 2 if anything was skipped. Every `28/28`
below is this command on that hardware.

## Performance

It is fast because the guest is not in a hot path. Control calls are forwarded;
the work itself is not - launching a kernel is a write to memory the guest
already has mapped, and nvkvm is not in that path at all.

> Geekbench 7 GPU (OpenCL) runs at 98.0–99.9% of host, on four
> machines, published to Geekbench's own servers
> · [RTX 4070 99.6%](https://browser.geekbench.com/v7/gpu/compare/87004?baseline=87011)
> · [RTX 3050 Laptop 99.9%](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862)
> · [H100 PCIe 98.8%](https://browser.geekbench.com/v7/gpu/compare/85619?baseline=85612)
> · [A100 80GB 98.0%](https://browser.geekbench.com/v7/gpu/compare/85389?baseline=85405).
> The RTX rows are bare metal on both sides; the datacenter rentals' "host" is
> itself a VM, so those two measure nvkvm nested a level deeper
> — [why that's still meaningful](docs/reference/parity.md).

A 32B model through vLLM runs at 0.99–1.00x of host and produces
token-identical output at temperature 0; fifteen other workloads land at 1.00x.
Three shapes cost more, all measured: single-stream greedy decode without CUDA
graphs (0.73–0.82x), tensor-parallel serving (0.89–1.06x, one configuration at
0.52x), and NVENC encode. [All the numbers](docs/reference/parity.md).

## What it is not

- **Not a hardened multi-tenant sandbox.** The guest/host boundary is not yet a
  security boundary you should rely on - read [`SECURITY.md`](SECURITY.md)
  before deciding where to run this. The code is intended to become secure against multi tenant use in the future so everything is written defensive,
  but some issues are still open (to be fixed) and a project with a single author cannot be claimed to be battle tested without external review.
  **Do not put untrusted tenants behind it.**
- One virtual display, not a multi-monitor setup. The guest gets a virtual
  KMS head that needs no monitor on the host - which is what makes a headless
  cloud GPU usable as a workstation - but only one, and no guest-side mode
  control. [Known limitations: display and graphics](docs/internal/known-limitations.md#display-and-graphics).
- Not vGPU No SR-IOV, no hardware partitioning, no MIG. Sharing is
  cooperative, at the driver interface.
- Not a Windows guest solution. Linux guests only.
- CUDA DMA pinned memory bypasses the VM ram limit. Known limitation, its currently an unbounded DoS target.

## How it fits together

```
  GUEST                                  HOST
  ──────────────────────────────         ──────────────────────────────
  CUDA / PyTorch / Vulkan / OpenGL
    │  ioctl(/dev/nvidia*)
    ▼
  nvkvm-guest.ko ─── virtio ────►  QEMU: virtio-nvgpu device
                                     │
                                     ▼
                                   one sandboxed process per guest
                                   process ──► NVIDIA driver ──► GPU
```

**The guest never gets the device** — something PCIe passthrough cannot offer,
since a passed-through GPU keeps DMA access to host RAM. **The work itself is
never forwarded**: setting a job up crosses the boundary, running it does not.
**And the VMM does not have to hold your display** — a separate display broker
owns the window, so QEMU needs no GL and no X11 or Wayland socket at all.

The request path end to end, and what the boundary does with a guest pointer:
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Tested platforms

| GPU | architecture | host drivers | `validate.sh` |
|---|---|---|---|
| GTX 1660 SUPER / Ti, RTX 2080 Ti | Turing | 535, 575 | 28/28 |
| RTX 3060 → 3090, 3050 Laptop | Ampere GA10x | 545 → 610 | 28/28 |
| RTX 4060 → 4090, RTX 4000 Ada | Ada AD10x | 575 → 595 | 28/28 |
| RTX 5070, RTX 5090 | **Blackwell** | 580 | 28/28 |
| A100 80GB, H100 PCIe | **GA100 / Hopper** | 550 → 580 | 28/28 |

Six architectures; multiple GPUs in one guest work, up to six concurrent
isolates each driving their own card. [Full matrix, every box and
footnote](docs/reference/tested-platforms.md). Coverage is a function of what
someone happened to rent, so it is uneven by construction — reports from
hardware not listed are wanted, and a **failure** is worth more than a success.

## Known issues

- **NVIDIA's own X driver (the DDX) cannot be used in the guest** — it asks
  about the *host's* physical displays. Ordinary desktops are unaffected.
- **One rare crash is unexplained.** A GL client took a guest down once and has
  not reproduced since; treat it as open rather than fixed.
- **`28/28` is not proof your workload is correct.** A real correctness bug has
  passed it before — check against a host run
  ([what that bug was](docs/reference/correctness.md)).
- **Frameworks that pin large host buffers pay a penalty** (250–350 MB/s vs
  12–17 GB/s, 2 GiB cap per registration). Stock vLLM starts and runs.

[All of them, with numbers](docs/internal/known-limitations.md).

## Documentation

| | |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | the request path and the five hard problems |
| [SECURITY.md](SECURITY.md) | the trust model, and what is not yet a boundary |
| [Install](docs/howto/install.md) | container, tarball, source; `/dev/kvm`; attestation |
| [Parity](docs/reference/parity.md) | every measurement behind the numbers above |
| [Tested platforms](docs/reference/tested-platforms.md) | the full hardware matrix |
| [FAQ](docs/faq.md) | including why not VFIO, vGPU or virtio-gpu |

Everything else: [`docs/README.md`](docs/README.md).

## Status

Experimental — a research artifact, not a supported product. It runs real
workloads at host parity on six GPU architectures, including multiple GPUs in
one guest. The largest open item is NVIDIA's X driver; everything else is in
[known limitations](docs/internal/known-limitations.md). Issues and
measurements from hardware this repository has not exercised are welcome — see
[contributing](CONTRIBUTING.md).

## Credits

`nvkvm` derives substantially from **gVisor's `nvproxy`** (Apache-2.0) for the
ioctl allowlist model, object tracking and frontend handling, and from
**NVIDIA's open-gpu-kernel-modules** for ABI struct definitions. Per-file
attribution is in [`CREDITS`](CREDITS).

Two other public non-vendor efforts at driver-level NVIDIA GPU virtualization
are worth reading: [`nestrilabs/virtio-nvgpu`](https://github.com/nestrilabs/virtio-nvgpu)
and [`straylight-software/isospin-microvm`](https://github.com/straylight-software/isospin-microvm).

## Licence

Apache-2.0, except the guest kernel module (`src/guest/`), which is GPL-2.0 as
required for kernel symbol access. See [`LICENSE`](LICENSE).
