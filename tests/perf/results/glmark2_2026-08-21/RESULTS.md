# glmark2 host-vs-guest, re-measured 2026-08-21

**The README's "glmark2 6857 guest vs 21571 host (~32%)" does not reproduce.**
Measured end-to-end on one box with one binary, the off-screen full-suite ratio
is **0.73x on a default guest and 0.89x with `clocksource=tsc`**. The "32%" is
what you get from a *single-scene* glmark2 invocation, because a fresh process's
**first** scene in the guest runs at ~0.37x while every scene after it runs at
0.88-0.93x.

## Setup

| | |
|---|---|
| box | vast.ai, RTX 3060, driver 575.51.03, 19 host cores |
| host | Ubuntu 22.04, kernel 6.8.0-59 (itself a KVM guest — nested) |
| guest | Ubuntu 24.04 cloud image, `VM_SMP=8`, `VM_MEM=12G`, nvkvm @ `11c385a` |
| glmark2 | 2023.01, built once **on the host**, run on both sides |
| | `sha256 27b70639…ae82  glmark2-wayland` — byte-identical both sides |
| compositor | headless weston, GL renderer, EGL vendor NVIDIA, both sides |
| glmark2 mode | `--off-screen` (no presentation at all) unless stated |

Every number below is host and guest on the same box, same binary, serially.

## Headline

| glmark2 run | host | guest | ratio |
|---|---|---|---|
| full default suite, off-screen | 32102 / 30246 | 22685 (kvm-clock) | **0.73x** |
| full default suite, off-screen | 32102 / 30246 | 27677 (`clocksource=tsc`) | **0.89x** |
| full default suite, windowed on headless weston | 725 | 531 | 0.73x |
| single scene `-b build`, off-screen | 44768 | 14027 | 0.31x ← **misleading** |

## Why the single-scene number is misleading: the first scene is cold

Same scene, four times, in **one** process (`-b build -b build -b build -b build`):

| repeat | host | guest | ratio |
|---|---|---|---|
| 1 | 44858 | 16582 | **0.37x** |
| 2 | 50981 | 45066 | 0.88x |
| 3 | 51585 | 45244 | 0.88x |
| 4 | 50082 | 46387 | **0.93x** |

It is per-**process**, not per-boot: every fresh glmark2 invocation pays it
again. `--reuse-context` does not change it (18895 / 46885 / 47288 / 47369), so
it is not GL context re-creation.

VM-exit histogram (`perf kvm stat`, host-side, on the QEMU pid) over 9 s of the
cold scene vs 9 s of the warm scene of the *same* process:

| exit | cold scene (17783 fps) | warm scene (44844 fps) |
|---|---|---|
| EPT_VIOLATION | 18768 @ 6.85 us | **absent** |
| EPT_MISCONFIG | 3875 @ 145.32 us | **absent** |
| MSR_WRITE | 17487 @ 4.80 us | 9812 @ 3.26 us |
| EXTERNAL_INTERRUPT | 6187 | 9087 |

Both EPT exit classes disappear once warm — the signature of on-demand
population of the GPU mapping window. They account for ~0.69 s of the 9 s cold
window directly; the rest of the cold penalty is **not yet accounted for**.

The same cold ramp is visible in a bare draw-call loop with no glmark2 at all
(`gl_drawrate`, wall = C + N x per_draw):

| N draw calls | host wall | guest wall |
|---|---|---|
| 100k | 0.014 s | 0.115 s |
| 400k | 0.034 s | 0.141 s |
| 1.6M | 0.117 s | 0.224 s |
| 8M | 0.543 s | 0.600 s |

Fitted: fixed cost **host ~10 ms, guest ~130 ms**; steady-state per draw call
**host 66.6 ns, guest 58.8 ns** — the guest is marginally *faster* once warm.

## Second cause: the guest's clock calls are not vDSO

| | host | guest (kvm-clock) | guest (tsc) |
|---|---|---|---|
| `clock_gettime(MONOTONIC)` | 49.2 ns | **644.7 ns** | 31.9 ns |
| `clock_gettime(REALTIME)` | 35.4 ns | 520.6 ns | — |
| `gettimeofday` | 35.3 ns | 491.2 ns | — |

Under kvm-clock the guest leaves the vDSO fast path and every clock read is a
real syscall: `strace -c` on a glFinish loop shows **192031 `gettimeofday` +
64026 `clock_gettime` for 30000 syncs** in the guest and **zero** on the host
(they never enter the kernel there). NVIDIA's GL driver spin-waits on a *timed*
loop, and glmark2 itself times every frame, so short-frame scenes pay this
heavily. Switching the guest to `tsc` recovers most of it:

| scene, off-screen, single run | host | guest kvm-clock | guest tsc |
|---|---|---|---|
| texture | 49381 | 32262 (0.65x) | 43228 (0.88x) |
| shading:phong | 41597 | 26962 (0.65x) | 37238 (0.90x) |
| terrain | 1525 | 1378 (0.90x) | 1442 (0.95x) |

Full suite: 22685 -> 27677, i.e. **0.73x -> 0.89x**.

## What is NOT wrong

| probe | host | guest | ratio |
|---|---|---|---|
| GL fill rate, 1920x1080 quads | 67.618 Gpix/s | 67.639 Gpix/s | **1.000x** |
| draw-call submission, warm | 66.6 ns | 58.8 ns | 1.13x (guest faster) |
| `glFinish` with nothing queued | 0.116 us | 0.135 us | 0.86x |
| `glMapBufferRange`+memcpy, 8 MB | 10.801 GB/s | 9.553 GB/s | 0.88x |
| `glBufferSubData`, 8 MB | 9.508 GB/s | 7.937 GB/s | 0.84x |
| `glReadPixels` 1920x1080 | 9.912 GB/s | 7.567 GB/s | 0.76x |
| `glTexSubImage2D` 1024x1024 | 3.711 GB/s | 1.629 GB/s | 0.44x |
| CUDA submit+`cuCtxSynchronize` | 91.58 us | 91.34 us | **1.00x** |
| `cuCtxSynchronize`, nothing queued | 0.49 us | 0.49 us | 1.00x |

- **Not a frame-rate cap.** Reproduced with `--off-screen`, which presents
  nothing; scores are tens of thousands of fps, never near 60.
- **Not cacheability.** Mapped-buffer CPU writes run at 9.5 GB/s in the guest
  (memcpy speed, not UC), the idle `glFinish` semaphore read is at parity, and
  the guest's `pat_memtype_list` shows write-combining honoured as requested.
- **Not ioctl forwarding.** A 200k-draw-call loop makes 3139 syscalls on the
  host and 2974 in the guest; a 100k-`glFinish` loop makes 1922 ioctls on the
  host and 1885 in the guest.
- **Not per-frame VM exits.** In steady state a `glFinish` loop at 26800/s
  produces 7844 exits/s = 0.29 exits per sync.

## Which scenes carry the remaining gap

Per-scene, off-screen, one run each, guest on kvm-clock. The gap tracks
*frame time*, not pixels — GPU-heavy scenes are at parity:

| scene | host FPS | guest FPS | ratio | host frame time |
|---|---|---|---|---|
| terrain | 1525 | 1460 | 0.96x | 0.656 ms |
| refract | 3394 | 2949 | 0.87x | 0.295 ms |
| desktop:blur | 3297 | 2515 | 0.76x | 0.303 ms |
| build:use-vbo=false | 4944 | 4687 | 0.95x | 0.202 ms |
| texture:nearest | 49381 | 41119 | 0.83x | 0.020 ms |
| build:use-vbo=true | 50659 | 43079 | 0.85x | 0.020 ms |

(These are the full-suite numbers, i.e. every scene except the first is warm.)

Raising the off-screen resolution moves the ratio the same way — same draw
calls, more GPU work per frame:

| scene | 800x600 | 1920x1080 | 3840x2160 |
|---|---|---|---|
| build (guest/host) | 0.31x | 0.37x | 0.57x |
| texture | 0.69x | 0.70x | 0.83x |
| terrain | 0.96x | 0.95x | 0.98x |

(Single-scene runs, so `build` here is the cold-scene number.)

## Reproduce

```bash
# on the host, once: build one glmark2 for both sides
bash tests/perf/build_glmark2.sh          # -> /opt/glmark2, share it into the guest
# then, on each side:
bash tests/perf/glmark_remote.sh host     # / guest
gcc -O2 tests/perf/apps/gl_decompose.c    -o gl_decompose    -lEGL -lGLESv2 -lm
gcc -O2 tests/perf/apps/gl_drawrate.c     -o gl_drawrate     -lEGL -lGLESv2
gcc -O2 tests/perf/apps/gl_finishrate.c   -o gl_finishrate   -lEGL -lGLESv2
gcc -O2 tests/perf/apps/cuda_sync_latency.c -o cuda_sync_latency -ldl
```

## Methodology traps hit while measuring this

1. **Never quote a single-scene glmark2 run.** The guest's first scene is 2.5x
   slow; a one-scene invocation reports that and nothing else.
2. **`nvidia-smi` polling during a micro-benchmark destroys it.** A 1 Hz
   `nvidia-smi` loop took `gl_finishrate` from 10.5 us to 174.8 us on the *host*.
3. **`strace` hides the interesting case.** vDSO calls are invisible to it, so
   "the host makes no clock syscalls" and "the host makes 8.5 per sync via the
   vDSO" look identical. Time the call instead.
4. **Check the clocksource before comparing anything timed.** Half of this gap
   was a `kvm-clock`/vDSO effect that has nothing to do with the GPU.
