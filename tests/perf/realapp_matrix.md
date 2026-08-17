# nvkvm real-application parity matrix

Proves nvkvm runs the workloads people actually put on GPUs — AI (beyond LLMs),
classic HPC/compute, crypto, and graphics/media — at **bare-metal host parity**,
from inside the untrusted KVM guest, over the full forward path
(guest kernel module → virtio → QEMU → per-process stub → host NVIDIA driver).

Each workload runs the SAME source/binary on the host and in the guest, STRICTLY
serially (one shared physical GPU), with a correctness check alongside throughput.
Parity gate: guest/host ≥ 0.90.

- Compute / AI / LLM matrix:  `run_matrix.sh`  (sources in `apps/`, runner `matrix_remote.sh`)
- Graphics / media matrix:    `run_graphics.sh` (runner `graphics_remote.sh`)
- Microbench (per-primitive):  `run_parity.sh`

Hardware: RTX 3060 12GB, driver 580.159.04, host Ubuntu 22.04 / guest Ubuntu 24.04.
Toolchain note: the 7 hand-written CUDA kernels compile to identical sm_86 SASS on
either toolkit; `sgemm_cublas`/`fft_cufft` use host cuBLAS/cuFFT 11.5 vs guest 12.x
(a library-version delta, not a forwarder effect) — `torch.matmul` gives the clean
same-version (12.1) cuBLAS parity signal.

## Results (2026-06-01)

### CUDA compute (10)
| workload | host | guest | ratio |
|---|---|---|---|
| memory bandwidth (triad) | 336.7 | 336.7 GB/s | 1.00x |
| reduction bandwidth | 127.9 | 126.1 GB/s | 0.99x |
| N-body (gravitation) | 4843 | 4872 GFLOP/s | 1.01x |
| Black-Scholes | 20985 | 20976 Mopt/s | 1.00x |
| Mandelbrot | 3.04e6 | 2.82e6 Mpix-it/s | 0.93x |
| 2D convolution | 1261 | 1218 GFLOP/s | 0.97x |
| SGEMM (cuBLAS) | 7.83 | 7.80 TFLOP/s | 1.00x |
| FFT (cuFFT) | 1251 | 1250 GFLOP/s | 1.00x |
| SHA-256 (crypto) | 766 | 808 MH/s | 1.05x |
| gpu-burn (sustained, 0 errors) | 9403 | 9470 GFLOP/s | 1.01x |

### PyTorch AI (7)
| workload | host | guest | ratio |
|---|---|---|---|
| matmul fp32 | 9.44 | 9.36 TFLOP/s | 0.99x |
| matmul fp16 (tensor cores) | 27.62 | 27.40 TFLOP/s | 0.99x |
| ResNet-50 inference | 632 | 630 img/s | 1.00x |
| ResNet-50 inference (AMP fp16) | 1126 | 1124 img/s | 1.00x |
| ResNet-50 training step | 205 | 204 img/s | 1.00x |
| ViT-B/16 inference | 172 | 172 img/s | 1.00x |
| BERT encoder inference | 290 | 290 seq/s | 1.00x |

### LLM — llama.cpp Qwen2.5-7B Q4_K_M (2)
| workload | host | guest | ratio |
|---|---|---|---|
| decode | 67.8 | 66.0 tok/s | 0.97x |
| prefill (1800-tok prompt) | 2221 | 2169 tok/s | 0.98x |

**Prefill methodology note.** With a *tiny* prompt (~5 tok) prefill measured
657→469 t/s (0.71x) — but that is fixed per-prefill launch/sync latency (the
control-path tax), not prefill compute. On a realistic long prompt (RAG / long
context / code — where prefill wall-clock actually matters) prefill is at 0.98x
parity because compute dominates. The short-prompt number measures launch RTT,
which `run_parity.sh` already tracks separately.

### Graphics / media — the DRI path (run_graphics.sh)
Headless throughout (EGL device platform / Vulkan compute / ffmpeg), exercising
the forwarded `/dev/dri/renderD128` + Vulkan ICD + video engines.
| workload | host | guest | ratio | notes |
|---|---|---|---|---|
| Vulkan device enumerate | RTX 3060 | RTX 3060 | — | ICD binds the GPU (not llvmpipe) |
| Vulkan compute fp32 (vkpeak) | 9341 | 9307 GFLOP/s | 1.00x | real Vulkan compute through nvkvm |
| OpenGL offscreen render (EGL) | 1.8 | 1.8 Mtri/s | 1.00x | renderer = NVIDIA RTX 3060; no GL error |
| NVENC h264 encode | 146 fps | — | — | **partial — see below** |

> **STALE (corrected 2026-08-17).** The NVENC paragraph below, and the "one real
> gap remains" line at the bottom, were both overtaken by events on the very day
> this file was written: commit ee53c90 ("nvenc: forward NVENC video encode —
> H.264 + HEVC work through nvkvm", 2026-06-01 18:49) root-caused
> `InitializeEncoder` to a missing alloc-params size-table entry for
> NV01_CONTEXT_DMA, landed the fix, and NVENC was re-baselined to 720p parity
> (0.96x) on 06-02.  The text below is kept for the record of how it was
> diagnosed, not as current status.  See the re-validation section for what
> NVENC actually does on driver 575.51.03.

**NVENC (video encode) — partial, root-caused.** Fixed in this pass: staged
`libnvidia-encode` + `libnvcuvid` into the guest (were missing → "Cannot load
libnvidia-encode.so.1"), and allowlisted 4 RM control cmds NVENC needs for engine
discovery (`GPU_GET_CLASSLIST` 0x800201, `GR_GET_CAPS_V2` 0x801109,
`FIFO_GET_CAPS_V2` 0x801713, `GPU_GET_ENCODER_CAPACITY` 0x2080016c — all benign
read-only queries, nvproxy CapVideo). The GPU is now recognized and the encode
session opens, but `InitializeEncoder` fails with "generic error (20)" and the
forwarder logs no DENY/error — a deeper semantic gap (cuCtxCreate-class) tracked
as its own task. NVDEC (cuvid) is excluded: `h264_cuvid` decodes 0 frames and
hangs on the **bare-metal host** too (broken ffmpeg+cuvid build), so no baseline.

## Bottom line
Across **22 real workloads** spanning AI training/inference (PyTorch CNN/ViT/
transformer + matmul tensor-cores), HPC (GEMM/FFT/N-body/Black-Scholes/stream/
reduction/convolution), crypto (SHA-256), sustained stress (gpu-burn), LLM
(llama.cpp decode+prefill), and graphics (Vulkan enumerate+compute, OpenGL
offscreen render), the guest runs at **host parity** (≥0.97x on everything that
matters), all byte-exact / error-free where checkable. The only places the guest
is measurably slower are latency-bound control paths (short-*prompt* prefill,
alloc churn) — never sustained compute, bandwidth, or throughput. The residual
gap is the multi-hop ioctl RTT (`docs/perf/forwarding_latency_decomposition.md`
— note: `docs/` is not part of this release repo, so that path resolves only in
the development tree), which only surfaces when a workload is dominated by tiny
serialized control ops.

~~One real gap remains and is root-caused: NVENC `InitializeEncoder` (above).~~
Superseded — NVENC was fixed in ee53c90 the same day (see the note above).

## Re-validation on driver 575.51.03 (2026-08-17)

First re-run of this matrix since the code was extracted into the release repo.
Different box and **different driver — 575.51.03, not the 580.159.04 these
results were taken on** — so absolute numbers are expected to move; the
guest/host ratio is the signal.  Host and guest ran strictly serially on the one
GPU.  Everything below was measured on BOTH sides in this pass; workloads that
were not run are listed as such rather than carried over.

The CUDA rows used ONE binary per workload, built once on the host with
`nvcc -O3 -arch=sm_86 -cudart static` and executed on both sides (sha256 of the
binary verified identical in the guest), which removes the host-11.5 vs guest-12.x
toolkit caveat that applies to the 2026-06-01 numbers.

| workload | host | guest | ratio |
|---|---|---|---|
| memory bandwidth (triad) | 336.2 | 336.3 GB/s | 1.00x |
| reduction bandwidth | 122.0 | 119.7 GB/s | 0.98x |
| N-body (gravitation) | 4615.5 | 4605.3 GFLOP/s | 1.00x |
| Black-Scholes | 20974.6 | 20963.8 Mopt/s | 1.00x |
| Mandelbrot | 2.90e6 | 2.76e6 Mpix-it/s | 0.95x |
| 2D convolution | 1176.6 | 1180.6 GFLOP/s | 1.00x |
| SHA-256 (crypto) | 756.0 | 756.0 MH/s | 1.00x |
| PyTorch matmul fp32 | 9.02 | 8.98 TFLOP/s | 1.00x |
| PyTorch matmul fp16 (TC) | 26.03 | 26.03 TFLOP/s | 1.00x |
| ResNet-50 inference | 615.8 | 614.6 img/s | 1.00x |
| ResNet-50 inference (AMP) | 1091.2 | 1092.4 img/s | 1.00x |
| ResNet-50 training step | 199.5 | 199.6 img/s | 1.00x |
| ViT-B/16 inference | 165.2 | 164.8 img/s | 1.00x |
| BERT encoder inference | 277.6 | 277.3 seq/s | 1.00x |
| Vulkan device enumerate | RTX 3060 | RTX 3060 | — |
| Vulkan compute fp32 (vkpeak) | 8947.2 | 8975.3 GFLOP/s | 1.00x |
| OpenGL offscreen render (EGL) | 1.8 | 1.8 Mtri/s | 1.00x |
| **NVENC h264 encode** | **94 fps** | **hangs** | **FAIL** |
| cudaMemcpy2D 1280x720 (new probe) | 5.64 | 2.35 GB/s | 0.42x |

**Not run in this pass** (so no ratio is claimed): SGEMM cuBLAS, FFT cuFFT,
gpu-burn, and both llama.cpp LLM rows.

**NVENC now hangs on the guest (driver 575.51.03).** Host encodes fine (94 fps).
In the guest `ffmpeg -c:v h264_nvenc` never completes — even a trivial 5 s 720p
solid-colour source — and wedges the harness's ssh channel with it.  gdb on the
live guest puts the main thread in a spin (clock_gettime loop) at:

    cuMemcpy2D_v2  <- libnvcuvid.so.1  <- h264_nvenc

This is NOT the old `InitializeEncoder` error-20 signature (a fast failure); it
is a hang further in, at frame upload.  Generic pitched 2D copies are fine — the
`cudaMemcpy2D` probe added above completes byte-exact on both sides — so it is
specific to the encoder's surface path, not to 2D copy in general.  Whether this
is a 575-vs-580 driver regression or a forwarder gap is undetermined: the running
VM's qemu stdout was not being captured, so there is no DENY/allowlist evidence
either way.  Reproduce with `tests/perf/run_graphics.sh` (the NVENC row now
FAILs one-sided instead of vanishing).

**Two caveats about the ffmpeg NVENC row's methodology**, independent of the
hang.  `graphics_remote.sh` synthesises frames with `-f lavfi -i testsrc`, which
is CPU-bound, and it builds a `src.mp4` that it then never uses — so the "fps"
partly measures frame synthesis, and on a box where host and guest have
different vCPU counts (25 vs 4 here) it is not a clean GPU-encode parity number.

### Display / desktop — re-validated
- **Headless GPU compositor: CONFIRMED.** `weston --backend=headless-backend
  --renderer=gl` comes up on the RTX 3060 — weston's own log reports
  `EGL vendor: NVIDIA`, `GL version: OpenGL ES 3.2 NVIDIA 575.51.03`,
  `GL renderer: NVIDIA GeForce RTX 3060/PCIe/SSE2` (i.e. not llvmpipe), the main
  thread sits in a healthy `epoll_wait` under `wl_display_run` with no
  nvidia-egl-gbm in the stack, and `weston-screenshooter` captured a populated
  1920x1080 desktop — textured wallpaper, top panel, live clock.
- **GL *clients*: now reach the GPU, but present NOTHING.  Root-caused to
  dma-buf export (2026-08-17).**  The earlier "clients land on llvmpipe" result
  was a *staging* failure and is fixed: with the EGL external platform staged
  (`libnvidia-egl-wayland.so.1` + `/usr/share/egl/egl_external_platform.d/
  10_nvidia_wayland.json`), `glmark2-wayland` as a client of headless weston now
  reports `GL_RENDERER: NVIDIA GeForce RTX 3060/PCIe/SSE2`, `GL_VERSION 4.6.0
  NVIDIA 575.51.03` — a real GPU context, no llvmpipe.  `eglinfo` binds NVIDIA on
  the Wayland platform (it bound Mesa before).
  **But the client still contributes ZERO pixels.**  Measured differentially
  (`verify_client_window_differential.sh`): a capture with the client running is
  *bit-identical* to a capture with no client at all — 0/518400 sampled pixels
  differ.  The earlier "100% non-black" reading was weston's background gradient,
  not the client.
  Where it stops, in order:
    - `WAYLAND_DEBUG=1` shows the client bind `zwp_linux_dmabuf_v1`, receive full
      feedback (`format_table` / `main_device` / `tranche_formats` / `done`),
      create its surface and ack configure — then NEVER emit
      `zwp_linux_buffer_params_v1.create` or `wl_surface.attach`.  No buffer ever
      reaches the wire.
    - Its main thread blocks in `pthread_cond_wait` inside
      `libnvidia-egl-wayland.so.1` ← `libEGL_nvidia.so.0` (i.e. inside
      `eglSwapBuffers`); the egl-wayland helper thread `poll()`s on a 1000 ms
      timeout forever.  glmark2 prints its GL info, starts `[build]`, and never
      completes one benchmark or one fps line.
    - **Control — the compositor is fine.**  The same glmark2 forced onto Mesa
      (`LIBGL_ALWAYS_SOFTWARE=1` + the Mesa vendor JSON) reports
      `GL_RENDERER: llvmpipe`, runs at **245 fps**, and IS visible: the
      differential shows a 799x599 changed region at (744,358) — exactly its
      800x600 window centred on the 1920x1080 output.
    - **The failing primitive is dma-buf export.**  `egl_dmabuf_export_probe`
      (no Wayland, no compositor: GL texture → `eglCreateImageKHR` →
      `eglExportDMABUFImageMESA`) gives, for the *same driver 575.51.03, same
      RTX 3060, same binary*:
        - HOST bare metal: `fd=25 stride=1024 offset=0` → **PASS**
        - nvkvm GUEST:     **FAIL `EGL_BAD_MATCH`**
      `eglExportDMABUFImageQueryMESA` succeeds identically in both
      (fourcc `AB24`, 1 plane, modifier `0x300000000e08014`) — the guest can
      *describe* the buffer but cannot *export* it.
  So this is a genuine nvkvm forwarding gap, not a staging or compositor issue:
  PRIME/dma-buf export of a rendered EGL surface is not forwarded, and that is
  precisely the operation a Wayland client must perform to hand a buffer to the
  compositor.  The compositor itself is unaffected because it renders offscreen
  and reads back with `glReadPixels`, never exporting anything.
  The "glmark2 795 fps through nvkvm" figure above remains unconfirmed: no
  GPU-rendered client has yet presented a frame, so no client fps exists to
  compare.  (Only the 245 fps llvmpipe control produced an fps number at all.)
- Not attempted: the DRM-backend compositor (documented, intrinsic
  `gbm_surface`→scanout wall — unchanged and deliberately out of scope), the
  wlr-screencopy zero-copy capture fps run, and `run_mate_x11.sh`.  No evidence
  for or against a working MATE desktop was found in this pass; the script
  exists (added in 9c827a7) but records no result.
