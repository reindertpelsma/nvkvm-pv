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
NVIDIA driver's own ioctl interface over virtio to the host driver.

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
- **boot an experimental guest kernel** without rebooting the host or losing
  the GPU
- **keep your existing container workflow** — Docker with
  `nvidia-container-toolkit` works unmodified inside the guest, so
  `docker run --gpus all` behaves as it does on the host
- **pass several GPUs to one guest** — multiple devices are autodetected and
  independently usable, verified on up to six cards in one guest

It is fast because the guest is not in a hot path. Control calls are forwarded;
the work itself is not — launching a kernel is a write to memory the guest
already has mapped, and nvkvm is not in that path at all:

> **Geekbench 7 GPU (OpenCL) runs at 98.0–99.9% of bare metal**, on four
> machines, published to Geekbench's own servers where neither we nor you can
> edit them: [RTX 4070 99.6%](https://browser.geekbench.com/v7/gpu/compare/87004?baseline=87011)
> · [RTX 3050 Laptop 99.9%](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862)
> · [H100 PCIe 98.8%](https://browser.geekbench.com/v7/gpu/compare/85619?baseline=85612)
> · [A100 80GB 98.0%](https://browser.geekbench.com/v7/gpu/compare/85389?baseline=85405).
> Guest on one side, the same physical box on the other.

Our own numbers agree: a 32-billion-parameter model through vLLM runs at
**0.99–1.00x** of host speed and produces token-identical output at temperature
0, and fifteen other workloads land at 1.00x. [See the
numbers](#tested-applications).

That is one GPU, with CUDA graphs, on throughput. Three shapes cost more, all
measured: single-stream greedy decode without graphs (0.73–0.82x), tensor-parallel
serving across several GPUs ([0.89–1.06x, with one
configuration at 0.52x](#multi-gpu-serving)), and NVENC video encode, which
hung once and has not reproduced since.

## What it is not

- **Not a hardened multi-tenant sandbox.** The guest/host boundary is not yet a
  security boundary you should rely on — read [`SECURITY.md`](SECURITY.md)
  before you decide where to run this. We audit it ourselves and publish what we
  find, fixed or not: the
  [pointer audit](docs/internal/audit-guest-pointers.md) (14 unenforced paths,
  5 since fixed) and the
  [boundary audit](docs/internal/audit-boundaries-2026-08-20.md) (19 findings
  across all three trust boundaries, 15 fixed, 4 named as open).
  **Do not put untrusted tenants behind it.**
- **One virtual display, not a multi-monitor setup.** There *is* a scanout path:
  the guest gets a virtual KMS head, and its composited frames reach a QEMU
  window by dma-buf (zero-copy on an NVIDIA-rendered host desktop, readback
  otherwise). That head does not need a monitor, or even a display server, on
  the host -- which is what makes a headless cloud GPU usable as a workstation.
  What you do **not** get is more than one head: no multi-monitor guest, and no
  guest-side control over modes, because forwarding NVKMS modesetting would hand
  the guest the *host's* displays. See
  [Known limitations](docs/internal/known-limitations.md).
- **Not vGPU.** No SR-IOV, no hardware partitioning, no MIG. Sharing is
  cooperative, at the driver interface.
- **Not a Windows guest solution.** Linux guests only.
- **Pinning host memory runs at 250-350 MB/s against 12-17 GB/s native** (~40x),
  and one registration is capped at 2 GiB. Stock vLLM starts and runs; see
  [known limitations](docs/internal/known-limitations.md#pinned-host-memory) for
  the numbers.

## Requirements

| | |
|---|---|
| Host | Linux with **working KVM** (see below), an NVIDIA GPU, and the proprietary/open NVIDIA driver installed |
| Guest | Linux, kernel 5.15 – 7.0 (built on every LTS in range; run-tested on Ubuntu 24.04, kernel 6.8) — see [guest kernels](docs/reference/guest-kernels.md) |
| GPU | Turing or newer — Pascal enumerates but `cuInit` fails, and the open kernel module will not probe it at all (see [Tested platforms](#tested-platforms)) |
| Size | ~16 GB RAM and 4 vCPUs free for the guest (the defaults), plus ~40 GB disk for the guest image and QEMU |
| Driver | See [supported drivers](docs/reference/supported-drivers.md) |
| Install | A [container image](#docker-start-here) or a [prebuilt tarball](#prebuilt-tarball-on-a-bare-host); [from source](#from-source-if-you-want-to-hack-on-it) QEMU 9.2 is built by the provided script |

**Check you can actually open `/dev/kvm` before anything else.** That is the
whole test — not CPU flags, which a container inherits from its host, so
`grep vmx /proc/cpuinfo` reads healthy inside one that has no `/dev/kvm`:

```bash
exec 3<>/dev/kvm && echo "KVM usable" && exec 3>&-
```

That opens the device rather than asking about it, which is the only test that
counts: `test -w` consults permission bits, and permission bits are not the
whole story — group membership does not take effect in a shell you were already
in when it was granted.

If it fails with permission denied, you are almost certainly not in the group
that owns the node (`crw-rw---- root kvm` on most distributions):

```bash
sudo usermod -aG kvm "$USER"    # then log out and back in, or: newgrp kvm
```

Containers are fine **provided the device is passed in** — the
[`docker-compose.yml`](docker-compose.yml) here does exactly that. What does not
work is a container without it, or a VM whose host has nested virtualisation
switched off.

Without `/dev/kvm`, QEMU silently falls back to software emulation (TCG): it
appears to work and is unusably slow, which is the most expensive way to find
out.

## Install

Three ways in, in the order most people should try them. The container is
first because it is the only one that does not make you wait for a QEMU build.

Everything below is published from a tag by
[`.github/workflows/release.yml`](.github/workflows/release.yml) on a
GitHub-hosted runner and carries a
[build-provenance attestation](#verifying-what-you-downloaded) — so "did this
come from this repository?" is a question you can answer rather than assume.

### Docker (start here)

```bash
docker run --rm -it --device /dev/kvm --gpus all \
    -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics,display,video \
    -p 127.0.0.1:2222:2222 -v nvkvm-guest:/opt/nvkvm-guest \
    ghcr.io/reindertpelsma/nvkvm-pv:v0.0.1-rc2
ssh -p 2222 ubuntu@127.0.0.1    # into the guest -- nvidia-smi already works
```

`NVIDIA_DRIVER_CAPABILITIES` is not optional decoration: `--gpus all` alone
gives the container `compute,utility`, and the guest then gets a compute-only
driver with no GL or Vulkan libraries — which does not fail, it silently falls
back to llvmpipe.

[`docker-compose.yml`](docker-compose.yml) is the same thing with those
capabilities, a tightened `cap_drop`/`cap_add` set, the shared folder and the
named volume already spelled out, and is the better starting point for anything
you intend to keep:

```bash
docker compose pull && docker compose up   # the published image
docker compose up --build                  # or build QEMU here instead
```

The guest comes up with the module loaded and the GPU present — no manual
staging step.

- **Requirements:** `/dev/kvm` and, for the GPU, the
  [NVIDIA container runtime](https://github.com/NVIDIA/nvidia-container-toolkit).
  Both are already in [`docker-compose.yml`](docker-compose.yml). No
  `--privileged`, no added capabilities.
- **The isolation trade differs from a bare host, and is not obviously worse.**
  Containers block user namespaces, so the isolate falls back to UID separation —
  but most of our audit findings target the VMM, which a container confines and a
  bare host does not. Neither is ready for untrusted tenants:
  [the trade in full](docs/internal/isolate-model.md),
  [`SECURITY.md`](SECURITY.md).
- **The guest survives reboots and `apt upgrade`.** The driver userspace is
  mounted read-only rather than copied in, so nothing in the guest can replace
  it, and a systemd unit rebuilds the module against the running kernel on every
  boot. `./data` is shared at `/data` for moving work in and out.
- **Knobs:** `VM_MEM`, `VM_SMP`, and `NVKVM_GUEST_IMAGE_URL` to bring your own
  cloud image — anything cloud-init capable that can build an out-of-tree module
  should work; only Ubuntu 24.04 is tested.

### Prebuilt tarball, on a bare host

The bare-host install — stronger isolation than a container, and no QEMU build.
From the [releases page](https://github.com/reindertpelsma/nvkvm-pv/releases):

```bash
# runtime dependencies -- the tarball ships QEMU, not what QEMU links against
sudo apt install -y libglib2.0-0t64 libpixman-1-0 libslirp0 \
                    qemu-utils genisoimage sshpass

tar xzf nvkvm-<version>-linux-x86_64.tar.gz && cd nvkvm-<version>
sudo cp -a qemu-nvkvm /opt/qemu-nvkvm
sudo install -Dm755 src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
```

```bash
bash scripts/setup_guest.sh         # fetches an Ubuntu 24.04 cloud image
```

x86-64 only, and the QEMU in it is headless (no GTK/SDL window — build from
source for that). It is a real binary built on Ubuntu 24.04, so it needs
**glibc 2.38 or newer**: on an older host (Ubuntu 22.04 is glibc 2.35) use the
container, which brings its own userspace. Exact floor and runtime package list
are in `RELEASE.md` inside the tarball
([detail](docs/howto/build.md#installing-the-tarball)).

**The guest kernel module is not in there and cannot be** — it is compiled
against *your* guest's kernel. That is why `src/guest/` ships with the tarball:
`scripts/run_test_vm.sh` exports the unpacked directory to the guest over 9p at
`/mnt/nvkvm`, and cloud-init builds the module there on first boot.

Building it there needs the export to be writable, and that is opt-in:

```bash
sudo NVKVM_DEV_HARNESS_INSECURE_RW=1 bash scripts/run_test_vm.sh
```

Read the banner it prints. A writable export gives guest root a path to host
root, so the harness is for guests you trust completely — see
[CONTRIBUTING.md, "The dev VM harness is not a sandbox"](CONTRIBUTING.md#the-dev-vm-harness-is-not-a-sandbox).
Without the flag the VM still boots and the export is read-only; only the
in-guest module build needs it.

### From source, if you want to hack on it

```bash
git clone https://github.com/reindertpelsma/nvkvm-pv.git nvkvm && cd nvkvm
bash scripts/build_qemu.sh          # builds the isolate stub, then QEMU 9.2 with the nvkvm device
bash scripts/setup_guest.sh         # fetches an Ubuntu 24.04 cloud image and prepares a disk
```

Most of the wall clock is QEMU. The script is a convenience, not the
mechanism: everything it changes in upstream QEMU is nine patch files in
[`patches/`](patches/) — 631 lines, applied with `git apply` — plus a copy of the
device sources into `hw/misc/`.
[`docs/howto/build.md`](docs/howto/build.md) lists the whole delta and walks the
same build by hand, command by command, if you would rather not run a script
over your QEMU tree; [`CONTRIBUTING.md`](CONTRIBUTING.md) has the three traps in
this build, including which changes need a `--force` rebuild.

### Staging the guest driver libraries

Needed for the tarball and source paths, not for the container — the container
assembles this itself on start-up from what the NVIDIA runtime injects.

The guest needs NVIDIA userspace libraries version-matched to your **host**
driver. Assemble the bundle and stage it:

```bash
V=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
bash scripts/make_host_bundle.sh    # collects host-libs-$V/ from the installed driver
```

Inside the guest, `scripts/stage_guest_libs.sh` puts them in place — see
[First result](#first-result) below. Guest setup is three things installed, not
two: the same script also writes an `/etc/X11/xorg.conf`, which is what makes a
stock distro's own Xorg session come up on the nvkvm head. It will not overwrite
one you wrote yourself, and `NVKVM_STAGE_XORG=0` tells it to leave X alone
([why the file is needed, and the two cases where you have to think about
it](docs/howto/run.md#the-guests-own-xorg-session-a-stock-distro-desktop)).

### Verifying what you downloaded

The release workflow signs a statement about each artifact — which repository
built it, from which commit, in which workflow run — and
[`gh`](https://cli.github.com/) checks it against the artifact in your hand:

```bash
# the image, resolved straight from the registry
gh attestation verify oci://ghcr.io/reindertpelsma/nvkvm-pv:v0.0.1-rc2 \
    --repo reindertpelsma/nvkvm-pv

# the tarball, on disk
gh attestation verify nvkvm-<version>-linux-x86_64.tar.gz \
    --repo reindertpelsma/nvkvm-pv
```

`--repo` is the point of the exercise: without it, `verify` will accept an
attestation from any repository, which makes it a signature check rather than a
provenance check. The `.sha256` file beside each download only proves the bytes
arrived intact. Add `--signer-workflow` to pin *which* workflow was allowed to
produce it, and see [verify before you run
either](docs/howto/build.md#verify-before-you-run-either) for the offline
variant.

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

Then run the suite the tables on this page are quoting, still inside the guest:

```bash
bash /mnt/nvkvm/tests/validate.sh
```

```
 TOTAL 28   PASS 28   FAIL 0   SKIP 0

 VERDICT: PASS (all 28 checks passed)
```

It exits 0 on a full pass, 1 on a failure and 2 if anything was skipped, so it
is usable in a script. Every `28/28` below is this command on that hardware.

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

Two things follow from that shape, and they are the two claims on this page that
sound too good to be true.

**The guest never gets the device**, and that is something PCIe passthrough
cannot offer: with passthrough the GPU keeps DMA access to host RAM, so the
isolation boundary is weaker than the VM boundary suggests.

**The work itself is never forwarded.** Setting a job up crosses the boundary;
running it does not — launching a kernel is a write to memory the guest already
has mapped, and nvkvm is not in that path. What you pay for is the *number* of
setup calls, which is why most numbers below are 1.00x and a few are not.

**And the VMM does not have to hold your display.** A separate privileged
process — the *display broker* — owns the window and the display-server
connection; QEMU relays the guest's dma-buf to it over a unix socket and imports
nothing. In that mode QEMU needs no EGL, no GL, no `libnvidia-eglcore` and no
`/dev/dri/renderD*` for display, and no X11 or Wayland socket at all, so the
socket is its only interface to your desktop. Verified by running it: QEMU at
uid 1000 with an empty capability set, putting a Linux Mint desktop on a 4K
monitor. [`src/broker/README.md`](src/broker/README.md).

The request path end to end, the five hard problems it runs into, and what the
boundary does with a guest pointer: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Known issues

- **NVIDIA's own X driver — the DDX — cannot be used inside the guest.** It
  reaches the GPU fine, then asks about the **host's** physical displays — a
  question nvkvm will not forward, and could not answer honestly if it did. This
  costs you something only if you specifically need that driver — chiefly
  `nvidia-settings`, and the professional-application features that depend on
  it. Ordinary desktops are unaffected: a stock distro's own Xorg session comes
  up on the nvkvm head, and GL, Vulkan and CUDA are accelerated on all of them
  ([guest setup](docs/howto/run.md#the-guests-own-xorg-session-a-stock-distro-desktop),
  [mechanism and measurements](docs/internal/mint-guest-desktop.md)).
- **One rare crash is unexplained.** A GL client once took a guest down within a
  minute. It has not reproduced since — through a five-minute soak at 190,013
  frames and a full interactive desktop session — but several unrelated things
  changed in between, so treat it as open rather than fixed
  ([detail](docs/internal/known-limitations.md)).
- **`28/28` is not proof that your workload is correct.** The test suite covers
  bring-up, CUDA, Vulkan compute and offscreen GL. It does not cover everything,
  and a real correctness bug has passed it before. Check your own results
  against a host run ([what that bug was](docs/reference/correctness.md)).
- **Multi-GPU tensor-parallel serving is correct, and 0.89–1.06x of host** at
  TP=1/2/4 — except TP=4 with CUDA graphs, which is bimodal and lands at 0.52x
  for reasons not yet established. Output differences at TP>1 are reduction
  order, not a guest defect: the host disagrees with itself there too
  ([numbers](#multi-gpu-serving)).
- **Frameworks that pin large host buffers pay a penalty.** Registering pinned
  memory is slower than native and a single registration is capped at 2 GiB —
  noticeable in data loaders and serving stacks that pin aggressively. Stock
  vLLM starts and runs
  ([numbers](docs/internal/known-limitations.md#pinned-host-memory)).

## Tested applications

An independent third-party benchmark first, because it is the easiest to check:
**Geekbench 7 GPU (OpenCL) scores 99.9% of bare metal** — 48335 in the guest vs
48395 on the host, with all eleven workloads between 98.4% and 101.2%, on one
RTX 3050 Laptop GPU and one driver. Bare metal here is literal: the host side is
a physical ASUS TUF F17 laptop, not another VM
([side by side](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862)).
Both runs are public; neither is ours to edit.

A second third-party-hosted result names the cost instead of averaging it away.
**clpeak**, published to OpenBenchmarking, puts seven of its eight subtests at
**99.4-100.2%** of bare metal -- several fractionally above 1.00x -- and the
eighth is the whole story: **kernel launch latency is 2.13x the host's**, 3.84 us
against 8.18 us, or **+4.3 us per launch**
([result](https://openbenchmarking.org/result/2608219-NE-NVKVMPVRT27)). That is
the design working exactly as designed, with its price stated: sustained GPU work
never exits the guest, but every launch does. It is also why throughput
workloads sit at parity while something that issues many tiny launches -- an
interactive desktop, say -- feels slower.

**Blender Open Data** (Cycles, RTX 4070, bare metal on both sides) splits that
cost a third way: the **GPU render is 99.95% of bare metal**, with one scene
fractionally above 1.00x, while *total* render time is 93.10%. The entire gap is
Cycles' scene-sync phase -- a roughly constant +2.0 to +4.1 s per scene of CPU
work on 4 vCPUs against 24 threads, plus host-to-device upload -- not a
per-kernel forwarding tax. Unlike the two above, **this one is ours and is not
third-party verifiable**: launcher 3.x requires a Blender ID login and offers no
anonymous submission path
([detail](docs/reference/blender-opendata.md)).

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

Eight more — N-body, Black-Scholes, SHA-256, 2D convolution, ResNet-50
inference and AMP, ViT-B/16, PyTorch fp32 — all land at 1.00x. **Two rows in
that table are not at parity, and they are not hidden**: a pitched 2D copy probe
at 0.42x, and **NVENC h264 encode, which hung in the guest** on 2026-08-17 while
the host encoded at 94 fps. A re-test on the same driver three days later did
**not** reproduce it, on different hardware and a much-changed tree — so treat
it as unreproduced rather than fixed, exactly as we do the crash above. Full
table and method: [`tests/perf/realapp_matrix.md`](tests/perf/realapp_matrix.md),
[the re-test](docs/internal/known-limitations.md).

Where the guest is measurably slower it is latency-bound control paths, never
sustained compute or bandwidth.

### Desktop graphics

A full desktop runs on the GPU inside the guest and can be displayed and driven
in a window on the host — including the stock Linux Mint Cinnamon desktop
([how](docs/howto/run.md#running-the-guest-desktop-in-a-window)).

That window can be owned by QEMU, or by the **display broker** — a small
privileged process that holds the display connection so the VMM does not have
to. With `-display nvkvm-broker` the guest's buffer reaches the screen with no
copy and QEMU links no graphics library at all. On GNOME/Wayland the compositor
scans the guest's own buffer out directly when fullscreen
(`wp_presentation` reports `KIND_ZERO_COPY`; the host DRM plane holds the
guest's NVIDIA block-linear modifier, unscaled, covering the CRTC).
[How to run it](src/broker/README.md).

On an RTX 4070 the guest's display flips at 59.9 Hz and **every one of those
frames reaches the host window: 60.0 swaps/s, zero dropped**, holding with a
browser rendering real pages and eight concurrent GPU clients. Measured with
per-frame counters over 60 consecutive one-second samples, every one of which
was exactly 60 frames. The pipeline is limited by display refresh, not by
forwarding overhead.

Graphics is the one area below parity. Measured on an RTX 3060 with one
`glmark2` binary, sha256-identical on both sides: the full suite off-screen
runs at **0.73x of host**, and **0.89x** with `clocksource=tsc` in the guest. The GPU itself is at
parity — GL fill rate is 1.000x and draw-call submission is slightly *faster*
in the guest.

The remaining cost is not the present path. It is a cold first scene — the
guest's first scene in a process runs ~0.37x and every one after it 0.88-0.93x,
so a single-scene run measures the cold path and nothing else — plus much
slower clock reads in the guest under `kvm-clock`, which a benchmark that times
every frame pays for directly. [Full decomposition and what was ruled
out](tests/perf/results/glmark2_2026-08-21/RESULTS.md).

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

**That number is contingent on CUDA graphs, and the hinge is measurable.** Run
the identical benchmark with `--enforce-eager` and decode drops to **0.82x**: the
forwarding tax is per *launch*, and a graph collapses a decode step into one.
Every modern serving stack uses graphs, which is why the headline is 0.99x — but
a stack that launches kernels one at a time pays about 20% on decode.
[Both passes, side by side](tests/perf/llm_parity.md).

At temperature 0 the guest produced **token-id identical** output to the host on
all three tasks (long-chain reasoning, C11 lock-free ring codegen, 8k-token
summarisation).

### Multi-GPU serving

vLLM tensor-parallel on 4x RTX 4090, Qwen2.5-7B. Host and guest mount **one
read-only image** holding the interpreter, venv and weights, so both sides run
byte-identical bytes by construction. Medians, NCCL at defaults on both sides:

| TP | eager | CUDA graphs |
|----|-------|-------------|
| 1  | 0.97x | 0.99x |
| 2  | 0.91x | 1.06x |
| 4  | 0.89x | [0.52x](docs/reference/parity.md#tp124-scaling-and-the-one-cell-that-does-not-fit) |

Eager scaling is flat and orderly. **One cell is not: TP=4 with CUDA graphs** —
and it is not a trend, because TP=2 with graphs is *faster* in the guest. That
cell is bimodal where the host's is not: across 40 samples the guest's best run
(795 tok/s) **beats the host's best** (775), but it keeps falling into a slow
mode, so the median lands at 0.52x. It is a mode, not a ceiling. Cause not
established — the guest carries a long launch+sync tail (p99 38–54 us against
6.6–8.6) and whether that tail *causes* the slow mode is assumed, not shown.
What has been ruled out (forwarder serialisation under N-way load, a different
NCCL algorithm, channel count, CUDA graph capture failure) and the measurement
that would settle the tail question are in
[the full writeup](docs/reference/parity.md#tp124-scaling-and-the-one-cell-that-does-not-fit).

Separately, on six RTX A4000s a 38 GB model that fits on none of them serves at
**0.86x**.

**Output parity on this path is resolved, and it clears the guest.** The *host*
disagrees with itself at TP=4, in both modes, while being 20/20 identical at
TP=1 on the same binary. A host/guest text difference at TP>1 is reduction order
inside the collective, not a guest defect. One residue stays open and we are not
smoothing it over: at TP=1 with CUDA graphs — no collective at all — the host is
20/20 and the guest 19/20. The odd output is a plausible near-tie flip, not
garbage; cause unknown.
[How the numbers got there](docs/reference/parity.md).

### Fine-tuning

A real Kaggle ARC-AGI notebook runs unmodified in the guest on an RTX 5090:
Unsloth LoRA fine-tuning **Qwen3** (128 steps, 13 GB allocated), then decode and
scoring inference at 14.7 GB — a full puzzle in 177 s. Training as well as
inference, multi-GB allocation churn between the two phases, and the Triton and
xformers paths, all through the forwarder.

Host parity: **—**, not measured yet for this workload; that is a gap in the
measurements, not a known shortfall.

## Tested platforms

Every row reached a real CUDA kernel launch through the forwarder, and `28/28`
means `tests/validate.sh` passed in full. Grouped by architecture here; the
[full matrix](docs/reference/tested-platforms.md) has every individual box,
driver and footnote.

| GPU | architecture | host drivers | `validate.sh` |
|---|---|---|---|
| GTX 1660 SUPER, GTX 1660 Ti, RTX 2080 Ti | Turing TU116 / TU102 | 535, 575 | 28/28 |
| RTX 3060, 3060 Ti, 3080, 3090, 3050 Laptop | Ampere GA10x | 545 → 610 | 28/28 |
| RTX 4060, 4070, 4070 Ti SUPER, 4090, RTX 4000 Ada | Ada AD10x | 575 → 595 | 28/28 |
| RTX 5070, RTX 5090 | **Blackwell** GB205 / GB202 | 580 | 28/28 |
| A100 80GB PCIe | **Ampere GA100** (datacenter) | 580 | 28/28 |
| H100 PCIe | **Hopper GH100** | 550 → 580, five versions | 28/28 |
| 2x RTX 4070 | Ada AD104 | 575 | 28/28, `cuda_device_count 2` |
| 4x RTX 5060 | Blackwell GB206 | 580 | 28/28, `cuda_device_count 4` |

Six architectures, and multiple GPUs in one guest work — six concurrent
isolates on a 6x A4000 box each drove their own GPU, every result still
element-wise correct, with all six busy at once
([the run](tests/BOOT_MATRIX.md)).

Several of those rows cost a bug fix to reach, and two of them looked like
NVIDIA's bugs until the same test was run on bare metal:
[what broke and how it was found](docs/reference/correctness.md).

Coverage is a function of what someone happened to rent, so it is uneven by
construction — reports from hardware not listed are genuinely wanted, and a
**failure** is worth more than a success. See [contributing](CONTRIBUTING.md).

## FAQ

**Is this vGPU or SR-IOV?**
No. There is no hardware partitioning and no vendor licence. nvkvm forwards the
driver interface; the GPU is shared cooperatively, the way two processes on one
machine share it.

**Will my GPU work?**
Turing (GTX 16xx / RTX 20xx) or newer. Six architectures are tested, from GTX
1660 to H100 — see [tested platforms](#tested-platforms). Pascal and older do
not work.

**Does the guest need a special driver?**
No. The guest runs stock NVIDIA userspace — its own `libcuda`, unmodified. What
it does need is a small kernel module, and userspace libraries matching your
**host** driver version, which `scripts/` stages for you.

**Can several VMs share one GPU?**
Yes, and the host keeps using it at the same time.

**What's the performance cost?**
Close to nothing for work that batches — a 32B model through vLLM runs at
0.99–1.00x of host speed. Single-stream, launch-bound workloads pay more; see
[reading the parity numbers](docs/reference/parity.md).

**Is it safe to run untrusted guests?**
Not yet. Read [`SECURITY.md`](SECURITY.md) before deciding where to run this.

[More questions](docs/faq.md) — driver version coverage, guest distros,
container support, llvmpipe, bit-identical results.

## Documentation

| | |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The request path end to end, and how the hard problems are solved |
| [`docs/howto/`](docs/howto/) | Building, running, staging guest libraries, adding a driver version |
| [`docs/reference/`](docs/reference/) | ABI profiles, allowlists, virtio protocol, device nodes |
| [FAQ](docs/faq.md) | The full set — driver coverage, guest distros, containers, llvmpipe |
| [Correctness](docs/reference/correctness.md) | What is known to be wrong, how far it is traced, how to reproduce it |
| [Reading the parity numbers](docs/reference/parity.md) | What the host/guest ratios do and do not establish |
| [Tested platforms, full matrix](docs/reference/tested-platforms.md) | Every box, driver and footnote |
| [Guest kernels](docs/reference/guest-kernels.md) | Which guest kernels the module builds on, measured, and why the range is narrow |
| [`.github/workflows/`](.github/workflows/) | What CI checks on every push, how a release is built and attested, and why each job exists |
| [`docs/internal/`](docs/internal/) | Design rationale, forwarding model, isolate model, known limitations |
| [`src/broker/README.md`](src/broker/README.md) | The display broker: threat model, wire protocol, and running the VMM with no display-server connection |
| [`SECURITY.md`](SECURITY.md) | Threat model, what is known broken, how to report a vulnerability |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | What is most useful to send, and three traps in the build |
| [Security audits](docs/internal/audit-boundaries-2026-08-20.md) | Both audits, findings and status — locations, not techniques |

## Status

Experimental — a research artifact, not a supported product. It runs real
workloads at host parity on six GPU architectures (Turing, Ampere, Ada,
Blackwell, and the GA100 and Hopper datacenter parts), including multiple GPUs
in one guest.

The largest open item is [NVIDIA's own X driver](#known-issues); everything
else is tracked in [known
limitations](docs/internal/known-limitations.md). Issues and measurements from
hardware and driver branches this repository has not exercised are welcome —
see [contributing](CONTRIBUTING.md).

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
