# Tested platforms — the full matrix

## Current 30-check managed-memory suite

These are the first rows rebuilt after the suite gained real
`cuMemAllocManaged` allocation and CPU↔GPU coherence checks. Each row passed
all 30 checks with zero failures and zero skips, including three 4 MiB managed
allocations, three verified migration/coherence cycles, CUDA kernels and
matmul, Vulkan compute, and EGL pixel verification.

| GPU | architecture | host driver | ABI profile | tree | `validate.sh` |
|---|---|---|---:|---|---:|
| GTX 1660 SUPER | Turing TU116 | 580.105.08 (control) | 580 | `9311bcb` | 30/30 |
| GTX 1660 SUPER | Turing TU116 | 580.95.05 | 580 | `9311bcb` | 30/30 |
| RTX 3060 | Ampere GA106 | 580.105.08 (control) | 580 | `2dc9465` | 30/30 |
| RTX 3060 | Ampere GA106 | 535.309.01 | 535 | `2dc9465` | 30/30 |
| RTX 3060 | Ampere GA106 | 570.124.06 | 570 | `2dc9465` | 30/30 |
| RTX 3060 | Ampere GA106 | 610.43.02 | 610 | `2dc9465` | 30/30 |
| RTX 4070 | Ada AD104 | 580.105.08 (control) | 580 | `0b48bb5` | 30/30 |
| RTX 4070 | Ada AD104 | 535.309.01 | 535 | `0b48bb5` | 30/30 |
| RTX 4070 | Ada AD104 | 610.43.02 | 610 | `0b48bb5` | 30/30 |

The Turing evidence is retained at
`/workspace/nvkvm-sweep-rr09-9311bcb/`; the Ampere evidence is retained at
`/workspace/nvkvm-sweep-ampere-2dc9465/`; the Ada evidence is retained at
`/workspace/nvkvm-sweep-ada-0b48bb5/`. All paid instances were destroyed and
verified absent. The numeric control denials retained on the passing rows are
recorded in [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md); none was
allowed merely to make this matrix green.

> ## ⚠ HISTORICAL. This matrix is being rebuilt (2026-08-23).
>
> **Every row below is real, and every row is narrower than it reads.** Two
> reasons, and the second one alone forces a reset:
>
> **1. The suite never exercised UVM.** `tests/validate.sh` checks that
> `/dev/nvidia-uvm` *exists* (it is in `$NODES`) and never uses it — no
> `cuMemAllocManaged`, no managed-memory allocation, no touch-and-verify. So a
> `28/28` here says nothing whatsoever about unified memory, **on any part in this
> table, ever**. That gap was found on 2026-08-23 after `cuMemAllocManaged` failed
> in a guest on Blackwell while succeeding on the host in 177–190 µs; whether UVM
> is broken generally is under investigation. Either way, this table never
> measured it.
>
> **2. A matrix is only meaningful if every row ran the same suite.** These rows
> span 17 driver versions from 535 to 610 and many revisions of the code. Once
> `validate.sh` gains a UVM test, "28/28" means something different from what it
> meant when these were recorded — so old and new rows are not comparable, no
> matter what the UVM investigation concludes.
>
> **What these rows DO still establish**, and it is not nothing: that on each of
> these parts and drivers, the ABI profile resolved correctly, the guest booted,
> the device nodes appeared, CUDA initialised, and the workloads the suite did
> cover passed. That is genuine coverage of the forwarding path. It is simply not
> the "everything works on this GPU" the number implies.
>
> Rows re-run against the current suite will move to a new matrix above this
> banner, each stating the suite revision it ran. Nothing here is deleted — a
> result is not worthless for having been narrower than its header.


Every row below reached a real CUDA kernel launch through the forwarder. The
README carries a condensed view organised by architecture; this is the complete
list, including near-duplicate rows that are evidence rather than reading
material.

Coverage here is a function of what someone happened to rent, so it is uneven by
construction. Reports from hardware not listed are welcome — see
[contributing](../../CONTRIBUTING.md). A **failure** report is worth more than a
success.

`—` in the `validate.sh` column means the suite was not run on that box, not
that it failed; those are early rows that predate the suite.

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
| 2x RTX PRO 6000 WS | **Blackwell GB202** `10de:2bb1` | 580.95.05 | 580 | 28/28, `multi_gpu` 2/2, `peer_gpu` 9/9 P2P byte-exact; llama.cpp decode 0.95x host, prefill 0.93x |
| 2x RTX PRO 6000 WS | **Blackwell GB202** `10de:2bb1` | 590.48.01 | 580 | 28/28 — the seam case: inside ABI_580's 580..595 struct row yet past the NVKMS 590 boundary, both tables behaved, no NVKMS DENY |
| 2x RTX PRO 6000 WS | **Blackwell GB202** `10de:2bb1` | 610.57.04 | 610 | 28/28, crosses into ABI_610 (V610 channel, 376B). Offscreen GL holds — the 595/610 breakage fixed 2026-08-17 does not recur |
| 2x RTX 5090 | **Blackwell GB202** `10de:2b85` | 580.105.08 | 580 | 28/28, `multi_gpu` 2/2, `peer_gpu` 7 pass / 1 skip (`p2p=no` — bare metal gives a byte-identical matrix, so hardware, not nvkvm) |
| 2x RTX 4070 | Ada AD104 | 575.51.03 | 570 | 28/28, `cuda_device_count 2` |
| GTX 1660 Ti | Turing TU116 | 575.51.03 | 570 | 28/28 |
| H100 PCIe | **Hopper GH100** | 550.54.14 | 550 | 28/28 |
| H100 PCIe | **Hopper GH100** | 565.57.01 | 550 | 27/28 before the `0xc661` fix; not re-run after |
| H100 PCIe | **Hopper GH100** | 570.124.06 | 570 | 28/28 (27/28 before the `0xc661` fix) |
| H100 PCIe | **Hopper GH100** | 570.148.08 | 570 | 27/28 before the `0xc661` fix; not re-run after |
| H100 PCIe | **Hopper GH100** | 575.57.08 | 570 | 28/28 (27/28 before the `0xc661` fix) |
| H100 PCIe | **Hopper GH100** | 580.65.06 | 580 | 28/28 |
| H100 PCIe | **Hopper GH100** | 580.126.09 | 580 | 28/28 |
| RTX 3050 Laptop | Ampere GA107 mobile | 580.173.02 | 580 | 28/28 |
| RTX 2080 Ti | Turing TU102 | 575.51.03 | 570 | 28/28 |
| RTX 3080 | Ampere GA102 | 580.95.05 | 580 | 28/28 |
| RTX 3090 | Ampere GA102 | 580.95.05 | 580 | 28/28 |
| RTX 4060 | Ada AD107 | 580.95.05 | 580 | 28/28 |
| RTX 4090 | Ada AD102 | 580.95.05 | 580 | 28/28 |
| RTX 5070 | Blackwell GB205 | 580.95.05 | 580 | 28/28 |
| A100 80GB PCIe | **Ampere GA100** (datacenter) | 580.126.09 | 580 | 28/28 \*\*\* |
| 4x RTX 5060 | Blackwell GB206 | 580.95.05 | 580 | 28/28, `cuda_device_count 4` |
| 6x Tesla T4 | **Turing TU104** (datacenter) | 580.178.04 | 580 | 28/28, `cuda_device_count 6` \*\*\*\* |
| 6x RTX A4000 | **Ampere GA104** (workstation) | 570.124.06 | 570 | 28/28, `cuda_device_count 6` |

> **Two failures reproduce on every Blackwell box tested, and on `main` as well as
> on the branch — so they are pre-existing nvkvm bugs, not Blackwell regressions.**
> Both are recorded here because a 28/28 row above would otherwise read as "all
> good on this part", and it is not.
>
> - **NCCL fails in-guest** (`Cuda failure 999`) while the same test passes on the
>   host, same driver, same torch, same NCCL, same GPUs. `DENY ctrl 0x00003d0b`
>   appears **only** during the failing run — `NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECTS_TO_FD`,
>   the plural sibling of the `0x3d08` that caused the previous NCCL bug. `3d0b` is
>   absent from the entire tree. Seen on 590.48.01, 610.57.04 and 580.105.08, across
>   two Blackwell SKUs. Correlation plus a source argument, **not proven cause**.
> - **`cuMemAllocManaged` failed `err=1`** via `DENY UVM cmd=0x48`
>   (`UVM_VALIDATE_VA_RANGE`, refused by the U-6 VA-ownership gate). **Fixed
>   2026-08-23** — see [Unified memory](#unified-memory-was-broken-everywhere-and-nothing-noticed)
>   below. It was **not** Blackwell-specific and not really a U-6 bug: QEMU never
>   made the `/dev/nvidia-uvm` mapping that creates a managed range, so no range
>   existed for U-6 to have recorded or for the driver to have found. Every
>   architecture was affected.


\* Both rows read 27/28 until the NVKMS inner-`cmdType` allowlist was fixed on
2026-08-17: branches 595+ issue `cmdType=60` per offscreen surface and a
575-era capture did not carry it, so `gl_draw_pixel_check` came back
`GL_FRAMEBUFFER_UNSUPPORTED`. With 60 allowed, 610.43.02 is a clean 28/28; the
595.84 row has only the one check re-measured, not a full re-run. Per-check
values in [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md).

\*\*\* First datacenter GA100, and it took two fixes that had been silently
wrong on every card before it — a 3B LLM now generates in the guest at 5.75 GiB
VRAM. [What they were](correctness.md#two-bugs-that-only-a100-exposed).

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
Five such pairs now exist, 96.2–99.9%, and three of them are bare metal on both
sides: [all four, with what each does and does not establish](parity.md).
All eleven workloads land between **93.2% and 100.1%**, two of them at or above
parity (Particle Physics 100.1%, Face Tracking 100.0%); the weakest is Video
Filter at 93.2%.

The guest was given less machine than the host, and this box is itself a VM
with the A100 passed through — so 98.0% is nvkvm's cost measured while nested a
level deeper than usual, which makes it a stronger result rather than a weaker
one. [Why, and why the 3050 reads higher](parity.md).

\*\*\*\* First Turing datacenter part, and the first **non-nested** multi-GPU
box: six T4s on bare metal, one guest, no configuration beyond the usual boot.
The guest saw all six (`/dev/nvidia0`..`nvidia5`, `nvidia-smi -L` 6,
`cuda_device_count` 6, Vulkan enumerating six T4s), sustained **133.7 TFLOPS
fp16 aggregate** with all six under concurrent GEMM load, verified five
device-to-device copies byte-exact, and returned every card to its 4-10 MiB
baseline afterwards with no lingering compute apps.

It also exercised the paths a display-less card forces, which no consumer GPU
here can: presentation had to run through **readback** rather than native
scanout, and hardware **NVENC** encode was driven from inside the guest — a
full XFCE desktop on the virtual head, captured and H.265-encoded to a browser
at 1080p, 58 fps, 26 ms end-to-end (25-28% encoder utilisation on the host,
one session, so genuinely the T4's encoder and not a software fallback).

Two things this box exposed that a desktop host hides:

- **`nvidia_drm` is not loaded by default** on a headless datacenter install,
  so there are no NVIDIA render nodes at all and QEMU dies with `egl: no drm
  render node available`. `modprobe nvidia-drm` creates them. The present path
  needs a render node; a datacenter host may not have one until asked.
- **`GL_RENDERER` is not a vendor string.** A T4 reports `Tesla T4/PCIe/SSE2`
  with no "NVIDIA" in it, where consumer parts say `NVIDIA GeForce ...`.
  `validate.sh` matched on it and failed the card as non-NVIDIA, taking
  `gl_draw_pixel_check` down as an unexpected skip: 26/28 on a stack that was
  working perfectly. Every Tesla, A-series and H-series row above was scored
  by that same check, so it now matches `GL_VENDOR` too.

#### Host/guest parity on the T4

[Published side by side](https://browser.geekbench.com/v7/gpu/compare/90743?baseline=90729)
— Geekbench 7 GPU (OpenCL), bare metal on both sides:

| | host | guest | ratio |
|---|---|---|---|
| **GPU Score** | **65851** | **63358** | **96.2%** |

The guest was given half the cores (8 vs 16) and a twelfth of the memory
(15.6 GB vs 187.6 GB), so 96.2% is measured while handicapped, not on equal
footing. Five of eleven workloads land at or above parity — Feature Matching
102.2%, Face Tracking 101.6%, Super Resolution 100.3%, Path Tracer 100.2%,
Fluid Simulation 100.1%.

**Video Filter is the weakest workload on this card (87.5%) and was also the
weakest on the A100 (93.2%)** — the same workload, two architectures and two
die families apart. Every other workload varies by card; this one does not.
That consistency makes it a lead rather than noise: something in that
workload's path costs more through the forwarder than the rest, and it is
reproducible on demand.

### First RHEL-family guest (CentOS Stream 9)

Also brought up on the same box, as a second guest alongside the Ubuntu one.
`nvkvm-guest.ko` had **never compiled on a RHEL kernel** — seven compat guards
fired at once, because RHEL reports `LINUX_VERSION_CODE` 5.14 and backports
large parts of 6.x underneath it (`class_create`, `vm_flags_clear`,
`vma_start_write`, const `devnode`, the maple-tree VMA iterator, `pde_data`,
and `drm_driver.date`). The guards now consult `RHEL_RELEASE_CODE`. Five
unchecked `copy_to_user()` calls also had to go: RHEL builds modules with
`-Werror` and the return is `__must_check`.

With those fixed, **CentOS Stream 9 scores 28/28** — the same as every Ubuntu
row above it, on 5.14.0-737.el9:

```
nvkvm: host reports 6 GPU(s); exposing 6
nvkvm: virtual KMS head ready (1920x1080, 1 connector/crtc)
TOTAL 28   PASS 28   FAIL 0   SKIP 0
```

Getting from "module will not compile" to 28/28 took two more portability
fixes, both of which had been invisible because the suite had only ever run on
Debian-family guests:

- **`stage_guest_libs.sh` hardcoded the Debian multiarch path**
  (`/usr/lib/x86_64-linux-gnu`), which is not on RHEL's linker search path
  (`/usr/lib64`), so only 6 of 24 libraries staged and `nvidia-smi` could not
  find NVML. It also only looked for the bundle on the 9p share, which a RHEL
  guest cannot have.
- **`validate.sh` trusted PATH for `nvidia-smi`.** The suite runs under sudo,
  and RHEL's sudo `secure_path` is `/sbin:/bin:/usr/sbin:/usr/bin` — no
  `/usr/local/bin`, which is where the binary is staged. Debian's secure_path
  includes it, so this could only ever appear on RHEL. All three `nvidia_smi_*`
  checks recorded SKIP while the binary worked perfectly from a normal shell,
  and an unexpected skip scores as not-a-pass: 25/28 INCOMPLETE on a working
  stack. The same shape of false negative as the `GL_RENDERER` check above,
  and found the same way — by running the suite somewhere it had never run.

One RHEL gap remains, and it is not in the forwarder: **the kernel package
ships no 9p at all** — not built in, not a module. The repo cannot reach the
guest the way it does everywhere else; it was scp'd in for this run. virtiofs
is the replacement RHEL does support.

Weston on CentOS is packaged without the DRM backend (`unknown backend "drm"`),
so the compositor path was not exercised there; the EGL/GL/Vulkan rungs that
`validate.sh` covers all pass, including `gl_draw_pixel_check` rendering a real
triangle into an FBO.

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
[detail](correctness.md#two-cuda-checks-failed-on-ada--driver-59584--fixed-2026-08-19).

On the H100 every CUDA and bring-up check passes — `sm_90`, PTX JIT, kernel
launch, matmul, byte-exact transfers — and OpenGL renders through the forwarder.
`vk_compute_dispatch` used to fail on every host driver **older than 580**
(565.57.01, 570.124.06, 570.148.08, 575.57.08) while passing on 580.65.06 and
580.126.09, and passed on the bare-metal host on all of them — which is how it
was identified as nvkvm's bug rather than the driver's, and then fixed
(`HOPPER_USERMODE_A` was allocated with a NULL parameter block). The 28/28 rows
above were measured on the tree that carried that fix — **which is not `main`
today**; it was reverted by `4fece85` and has not been restored. Read
[the driver sweep, the trace and the fix](correctness.md#vulkan-compute-on-hopper--root-caused-it-was-ours-and-it-was-never-a-driver-bug)
before quoting a current Hopper number.

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
path rather than the data path — [what that means](parity.md).

## The display path

The table above is about compute — every row reached a CUDA kernel launch. The
display path is a different code path and needs its own matrix, because *how the
guest's frame reaches a human* varies more than the GPU does, and each route
exercises different code:

- **native** — the host desktop is rendered by the *same* NVIDIA GPU, so QEMU
  imports the guest's dma-buf straight into its window. Zero copies. **Detected
  automatically** since the present path probes the host UI's own GL renderer;
  the log says `display mode = GL zero-copy (host UI renders on NVIDIA: native
  import)` and names the renderer. `NVKVM_PRESENT_MODE=gl|readback` still forces
  it either way. (Before that probe existed the mode was hardcoded to readback
  and `=gl` had to be set by hand, which is what older logs and notes show.)
- **readback** — the host's window cannot import an NVIDIA dma-buf (a hybrid
  laptop where the display is on the iGPU, an Xvfb or software X server, a
  non-NVIDIA compositor). QEMU reads the pixels back to a CPU surface and
  re-uploads them. Correct everywhere, and the default for exactly that reason.
- **headless** — no host display or display server at all. The guest still gets
  a virtual KMS head; frames leave by whatever runs *inside* the guest, e.g. a
  remote-desktop or streaming server. This is the cloud-workstation case and the
  one no other open stack covers.

| GPU | driver | host session | present path | what was exercised |
|---|---|---|---|---|
| RTX 4070 (Ada AD104) | 595.84 | GNOME/Wayland, real monitor | **readback**, then **native** | Mint desktop; Minecraft at max settings, 60 fps, chunk generation smooth, 35% GPU util; `nvtop` (NVML); GL zero-copy A/B against readback |
| RTX 3050 (Ampere GA107) | 595.84 | GNOME/Wayland, real monitor | **native** | Mint desktop; pointer lock; keyboard grab (Super to guest); configurable head verified at 1600x900; OpenGL 4.6 reported by the guest |
| 6x Tesla T4 (Turing TU104, datacenter) | 580.178.04 | **none — headless bare metal** | **headless** | Ubuntu guest: XFCE on the virtual head, weston+Xwayland, NVENC H.265 to a browser over an SSH tunnel; Minecraft 54 fps in-guest, 48 fps delivered, 25 ms end-to-end. CentOS Stream 9 guest: Xorg + openbox on the same head. |
| RTX 3050 **Laptop** (Ampere GA107, mobile) | 580.173.02 | **Xvfb + llvmpipe, inside a Docker container** | **readback** | Ubuntu guest: weston on the virtual head → readback into `Xvfb :99` → x11vnc → noVNC → browser. 52-61 fps sustained, `present_mean` 1.0-1.4 ms, 12,303 frames, `failed=0`. Verified by pixel, not by counter: the guest's exported dma-buf reads `nonzero_px=2073512/2621440`, and the Xvfb root window holds 8,916 unique colours with no black frame. |

The two real-monitor rows are on driver **595.84** and were verified by watching
the screen, not by reading a log. Two GPU generations (Ada and Ampere) and two
CPU vendors (Ryzen 9 7900 and Core i5-4460).

The **RTX 3050 Laptop** row is the software-host case: no NVIDIA-rendered host
UI anywhere, so the window is Mesa/llvmpipe on `Xvfb` and readback is the only
correct path. It is also the row that needed two fixes before it worked at all,
and each failed in a way worth knowing about:

- without the threaded present path, `egl_init()` cannot get a context on that
  host (the UI already holds one on the main loop) and nvkvm displays **nothing
  at all** while reporting a healthy 54.8 fps;
- without `patches/0010`, weston on the virtual head kills the guest within
  10-60 s with `kvm run failed Bad address` -- the NVIDIA fault handler answers
  "come back later" while it reinstates a revoked GPU mapping, and KVM turns
  that into a fatal EFAULT. It clears after ~1.5 s if you let it.

Both are mobile-GPU-friendly in the sense that matters for r/VFIO: this is the
consumer-laptop shape, and it is the only row where either bug reproduces.

The T4 row is the headless case in its strongest form: that card has **no
display outputs at all**, so there is no host display server, no monitor, and
nothing for a native path to import into. Frames leave by NVENC inside the
guest and arrive in a browser. It also covers a second guest OS on the same
head — CentOS Stream 9 running Xorg, where weston could not be used because
EPEL packages it without the DRM backend.

**Still not in this table, and it should be:** a container with Xvfb, which is
a readback row by construction. That was done by the author but is not recorded
here with its driver version, so it is named rather than tabulated until
someone writes down what they ran.

### What the display path is fragile to

Not CPU architecture — DRM, dma-buf and EGL are arch-neutral, and the x86-64
constraint lives in the isolate and the GPA-window logic instead.

It is fragile to **driver branch**. `enum NvKmsIoctlCommand` is unvalued, so
position is the wire value, and NVIDIA renumbers it *mid-branch*: 570.195.03 and
570.207 disagree. A gate written against one branch silently admits a different
command on another — which is exactly what happened, and why the allowlist is
now keyed on major *and* minor across 28 vendor tags. Assume a new driver branch
needs the enum re-read, not that it will just work.

### Pointer lock needs SDL, and that is a host-side constraint

On a Wayland host, QEMU's **GTK** backend cannot take a pointer lock at all: it
emulates one with `gdk_seat_grab()` plus `gdk_device_warp()`, and an X11 client
under Xwayland can do neither, because the compositor owns the pointer. Measured
— the cursor left the "grabbed" window and `evtest` in the guest saw zero
relative events. The **SDL** backend asks properly
(`SDL_SetRelativeMouseMode()`, which on Wayland is `zwp_pointer_constraints_v1`
plus `zwp_relative_pointer_v1`), and that is the configuration both rows above
used for grab mode.

## Unified memory was broken everywhere, and nothing noticed

Every `28/28` in the table above was **silent on unified memory**. `validate.sh`
mentioned UVM in exactly one place — `/dev/nvidia-uvm` in the device-node list
(`tests/validate.sh:219`) — and a node existing says nothing about managed
memory working. No check ever called `cuMemAllocManaged`. So on every GPU ever
tested here, `cuMemAllocManaged` returned `CUDA_ERROR_INVALID_VALUE` in the
guest and the suite reported a clean sweep.

This is the failure mode the suite's own design rules exist to prevent, and it
went unnoticed for the same reason every time: *the check did not exist*, so
there was nothing to go yellow.

### It was never Blackwell-specific

The bug was first seen on 2x RTX 5090 and recorded as "scope beyond Blackwell
not established". It is universal, and it was checked rather than argued:
`main` @ `10a0a03`, the same guest image, the same host driver, three
architectures, one command each.

| GPU | architecture | `cuMemAllocManaged` on `main` | host log |
|---|---|---|---|
| RTX 5090 | Blackwell GB202 | `rc=1 CUDA_ERROR_INVALID_VALUE` | `DENY UVM cmd=0x48 ... (U-6)` |
| RTX 4070 | Ada AD104 | `rc=1 CUDA_ERROR_INVALID_VALUE` | `DENY UVM cmd=0x48 ... (U-6)` |
| RTX 3080 | Ampere GA102 | `rc=1 CUDA_ERROR_INVALID_VALUE` | `DENY UVM cmd=0x48 ... (U-6)` |

Which is what the code said it would be: nothing on the path reads a GPU model,
an architecture or a PCI ID. `nvkvm_req_mmap_on_isolate` branches on
`h->dev_id != NVKVM_DEV_UVM` and nothing else, the orphaned `mmap_win` GPA
region is a property of the window layout, and the guest's UVM size table is a
`switch` on a command number. **Read every `28/28` row in the table above as
"28/28, unified memory not exercised".**

### The fix, on the same three boxes

Same boxes, same guests, one QEMU rebuild and a guest-module rebuild apart:

| GPU | architecture | `main` @ `10a0a03` | with the fix |
|---|---|---|---|
| RTX 5090 | Blackwell GB202 | `cuda_managed_alloc` **FAIL** — 28 PASS / 1 FAIL / 1 SKIP | **30 PASS / 0 FAIL / 0 SKIP** |
| RTX 3080 | Ampere GA102 | `cuda_managed_alloc` **FAIL** — 28 PASS / 1 FAIL / 1 SKIP | **30 PASS / 0 FAIL / 0 SKIP** |
| RTX 4070 | Ada AD104 | `cuda_managed_alloc` **FAIL** | both managed checks **PASS**, no UVM `DENY` \* |

\* That guest has no Vulkan loader or EGL installed, so the graphics half of
the suite skips and the run reads INCOMPLETE rather than PASS. The CUDA half is
complete: 20 PASS / 0 FAIL.

On the RTX 5090, beyond the suite:

- `tests/integration/cuda_micro.c` case 5 (`uvm_alloc`, 50 managed alloc+touch)
  and case 6 (`uvm_migrate`, 20 GPU↔CPU migration cycles over 4 MiB) both run.
  Case 5 previously printed `cuMemAllocManaged FAILED (err=1) — skipping UVM`
  and case 6 could not start.
- Two concurrent CUDA processes both get working managed memory — the case the
  QEMU-side UVM mapping was removed for, when a collision was fatal
  (`cuCtxCreate` 304) rather than a fallback.
- Over a full run: 153 real UVM mappings, 142 stale-mapping reclaims, 1 announced
  fallback, and **zero** `DENY UVM` from legitimate traffic. The only eight
  `DENY UVM` lines in the log are the four adversarial probes in
  `tests/security/u3_u6_gate_test.c`, twice — which still reports
  `GATE_TEST PASS (0 accepted)`.

### When it broke

Not on 2026-08-21, and not in any commit that can be bisected. The UVM branch of
`nvkvm_req_mmap_on_isolate` already reads *"The old approach mmap'd the UVM fd
MAP_FIXED at req->offset ... Instead allocate the GPA from the sparse window"*
at `ebdfb30`, the first commit in this history, and the only commit to touch
that function since (`e5e6dc3`) does not go near the UVM branch. The three
2026-08-21 UVM commits are not candidates either: `a276035` hardens
`REALIZE_UVM_MAPPING`, which the guest never reaches
(`src/guest/nvkvm_mmap.c:295` disables it outright), and `74769e8` / `ff98af9`
only *widen* the guest's size table.

So the comment is the only surviving record that an earlier design did make this
mapping, and it predates the published history. On `main` as published, unified
memory has never worked.

Two checks now cover it, and the suite is **30** rather than 28:

| check | what it proves |
|---|---|
| `cuda_managed_alloc` | `cuMemAllocManaged` succeeds and returns a usable pointer on a device reporting `MANAGED_MEMORY=1` |
| `cuda_managed_coherence` | the CPU writes the inputs **through the managed pointer** (no `cuMemcpy` anywhere), a real kernel runs over them, and the CPU reads every output element back — repeated over three CPU↔GPU migration cycles with different inputs each time |
| RTX 3060 | Ampere GA106 | 565.57.01 / 570.124.06 / 580.95.05 / 590.48.01 / 595.84 / 610.57.04 | 550-610 | `9a98116` | 34/34 |
| RTX 2060S | Turing TU106 | 565.57.01 / 570.124.06 / 580.95.05 / 590.48.01 / 595.84 / 610.57.04 | 550-610 | `9a98116` | 34/34 |
| RTX 4070 Ti | Ada AD104 | 565.57.01 / 570.124.06 / 580.95.05 / 590.48.01 / 595.84 / 610.57.04 | 550-610 | `9a98116` | 34/34 |
| RTX 5070 | Blackwell GB205 | 580.95.05 / 590.48.01 / 595.84 / 610.57.04 (all applicable; floor 580) | 580-610 | `9a98116` | 34/34 |
| RTX 3050 Laptop | Ampere GA107 | 580.173.02 | 580 | `9a98116` | 34/34 |
| RTX 4070 | Ada AD104 | 595.84 | 580 | `9a98116` | 34/34 |

The second one is the point. Managed memory is a different code path from
everything else in the suite, not a variation on one: the range is created by
`mmap()` on `/dev/nvidia-uvm` rather than by an ioctl, the pages migrate on
fault rather than being copied, and the CPU dereferences the device pointer
directly. Only a value check across both directions can tell "it works" from
"the allocation returned a pointer".

Measured on the same box, same guest, same driver, one QEMU rebuild apart:

```
main @ 10a0a03   TOTAL 30   PASS 28   FAIL 1   SKIP 1
                 FAIL cuda_managed_alloc  cuMemAllocManaged(4194304) rc=1
                      (CUDA_ERROR_INVALID_VALUE) on a device that reports
                      MANAGED_MEMORY=1 -- unified memory is unavailable in this guest
with the fix     TOTAL 30   PASS 30   FAIL 0   SKIP 0
```

That `PASS 28` is the historic result, reproduced exactly. The suite was not
wrong before; it was not looking.
