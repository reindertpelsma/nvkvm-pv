# Tested platforms — the full matrix

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
| 6x RTX A4000 | **Ampere GA104** (workstation) | 570.124.06 | 570 | 28/28, `cuda_device_count 6` |


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
Four such pairs now exist, 98.0–99.9%, and two of them are bare metal on both
sides: [all four, with what each does and does not establish](parity.md).
All eleven workloads land between **93.2% and 100.1%**, two of them at or above
parity (Particle Physics 100.1%, Face Tracking 100.0%); the weakest is Video
Filter at 93.2%.

The guest was given less machine than the host, and this box is itself a VM
with the A100 passed through — so 98.0% is nvkvm's cost measured while nested a
level deeper than usual, which makes it a stronger result rather than a weaker
one. [Why, and why the 3050 reads higher](parity.md).

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

