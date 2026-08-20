# ABI profile boot matrix

Every row below is a guest actually booted on a host running that driver, with
`tests/validate.sh` run inside it. Values are the ones the suite printed, not
summaries of them.

Method: rent a box, change the host driver **in place** (apt + DKMS, exact
version pinned), re-run `scripts/make_host_bundle.sh`, re-run
`scripts/stage_guest_libs.sh` in the guest, then `tests/validate.sh`. The
driver version a vast.ai offer advertises is meaningless — all three boxes
rented for this run advertised 580.95.05 and all three came up on 575.51.03.

## Coverage against the eight-profile table

| profile | covers | booted here | previously booted | status |
|---|---|---|---|---|
| 515 | 515–520 | — | no | **not reached** (see blockers) |
| 525 | 525–530, 535.43.0x (<.08), 535.54.x | — | no | **not reached** (see blockers) |
| 535 | 535.43.08+, 535.86+ | — | yes (535.309.01) | already covered |
| 545 | 545, 550.40.0x (<.53) | **545.23.08** | no | **newly booted, 28/28** |
| 550 | 550.40.53–565 | **550.54.14** | no | **newly booted, 28/28** |
| 570 | 570–575 | — | yes (575.51.03) | already covered |
| 580 | 580–595 | **580.95.05, 595.84** | no | **newly booted** |
| 610 | 610+ | **610.43.02** | no | **newly booted, 28/28** (27/28 before the cmdType-60 fix) |

Four profile rows went from "measured but never booted" to booted: **545, 550,
580, 610**. The 580 row was exercised at both ends of its range (580.95.05 and
595.84) because the row now spans 580–595 and 590/595 were measured-identical
rather than assumed; booting 595 is what confirms the top of that range really
does take the 580 row.

The profile id QEMU selected was confirmed at realize on every boot:

```
nvkvm: host driver 545.23.08 → ABI profile 545
nvkvm: host driver 550.54.14 → ABI profile 550
nvkvm: host driver 580.95.05 → ABI profile 580
nvkvm: host driver 595.84    → ABI profile 580
nvkvm: host driver 610.43.02 → ABI profile 610
```

No profile's table values proved wrong in practice. Every row selected the
expected profile id, and on every row the full CUDA ladder — `cuInit` through a
verified kernel launch and a matmul checked against a CPU reference — passed.
That is the evidence the ABI struct sizes and field offsets are right for these
rows: a wrong `uvm_map_ext_size` or `uvm_map_ext_fd_off` does not fail loudly,
it forwards a truncated struct, and the failure surfaces as `cuCtxCreate -> 999`
or `-EBADF`. None of that appeared.

## Results

Host in every case: Ubuntu 22.04 container, kernel 6.8.0-59-generic, guest
Ubuntu 24.04 / kernel 6.8, 4 vCPU / 16 GB, repo on 9p at `/mnt/nvkvm`.

| check | 545.23.08 <br>RTX 3060 | 550.54.14 <br>RTX 3060 | 580.95.05 <br>RTX 3060 Ti | 595.84 <br>RTX 3060 Ti | 610.43.02 <br>RTX 3060 |
|---|---|---|---|---|---|
| `guest_module` | PASS 110592 | PASS 110592 | PASS 110592 | PASS 110592 | PASS 110592 |
| `dev_nodes` | PASS 5/5 | PASS 5/5 | PASS 5/5 | PASS 5/5 | PASS 5/5 |
| `nvidia_smi_gpu` | RTX 3060 | RTX 3060 | RTX 3060 Ti | RTX 3060 Ti | RTX 3060 |
| `nvidia_smi_driver` | 545.23.08 | 550.54.14 | 580.95.05 | 595.84 | 610.43.02 |
| `nvidia_smi_cuda` | 12.3 | 12.4 | 13.0 | 13.2 | 13.3 |
| `abi_profile` | 545 | 550 | 580 | 580 | 610 |
| `cuda_libcuda` | PASS | PASS | PASS | PASS | PASS |
| `cuda_init` | rc=0 | rc=0 | rc=0 | rc=0 | rc=0 |
| `cuda_driver_version` | 12.3 (12030) | 12.4 (12040) | 13.0 (13000) | 13.2 (13020) | 13.3 (13030) |
| `cuda_device_count` | 1 | 1 | 1 | 1 | 1 |
| `cuda_device_name` | RTX 3060 | RTX 3060 | RTX 3060 Ti | RTX 3060 Ti | RTX 3060 |
| `cuda_compute_cap` | sm_86 | sm_86 | sm_86 | sm_86 | sm_86 |
| `cuda_ctx_create` | rc=0 | rc=0 | rc=0 | rc=0 | rc=0 |
| `cuda_htod_dtoh_8mib` | 8 MiB byte-exact | 8 MiB byte-exact | 8 MiB byte-exact | 8 MiB byte-exact | 8 MiB byte-exact |
| `cuda_memset_d8` | verified | verified | verified | verified | verified |
| `cuda_ptx_jit` | rc=0 | rc=0 | rc=0 | rc=0 | rc=0 |
| `cuda_kernel_launch` | 1048576/1048576 | 1048576/1048576 | 1048576/1048576 | 1048576/1048576 | 1048576/1048576 |
| `cuda_matmul` | max abs delta 0 | max abs delta 0 | max abs delta 0 | max abs delta 0 | max abs delta 0 |
| `vk_loader` | PASS | PASS | PASS | PASS | PASS |
| `vk_instance` | rc=0 | rc=0 | rc=0 | rc=0 | rc=0 |
| `vk_physical_device` | NVIDIA + llvmpipe | NVIDIA + llvmpipe | NVIDIA + llvmpipe | NVIDIA + llvmpipe | NVIDIA + llvmpipe |
| `vk_device_is_nvidia` | 0x10DE | 0x10DE | 0x10DE | 0x10DE | 0x10DE |
| `vk_compute_dispatch` | 4096/4096 | 4096/4096 | 4096/4096 | 4096/4096 | 4096/4096 |
| `egl_loader` | PASS | PASS | PASS | PASS | PASS |
| `egl_context` | EGL 1.5 | EGL 1.5 | EGL 1.5 | EGL 1.5 | EGL 1.5 |
| `gl_renderer` | ES 3.2 NVIDIA 545.23.08 | ES 3.2 NVIDIA 550.54.14 | ES 3.2 NVIDIA 580.95.05 | ES 3.2 NVIDIA 595.84 | ES 3.2 NVIDIA 610.43.02 |
| `gl_renderer_is_nvidia` | PASS | PASS | PASS | PASS | PASS |
| `gl_draw_pixel_check` | PASS | PASS | PASS | **FAIL 0x8CDD** ‡ | **FAIL 0x8CDD** ‡ |
| **verdict** | **28/28 PASS** | **28/28 PASS** | **28/28 PASS** | **27/28** ‡ | **27/28** ‡ |

‡ **superseded 2026-08-17.** These two cells are what the suite printed on the
day, and they are left as printed. The cause was nvkvm's own NVKMS allowlist,
not the driver; with `cmdType=60` allowed, both rows PASS
`gl_draw_pixel_check` and 610.43.02 is a clean **28/28**. See
[Attribution](#attribution-nvkvm-not-nvidia--measured-2026-08-17).

## Multi-GPU — 2x RTX 4070, 575.51.03 (measured 2026-08-18)

Every row above is single-GPU (`cuda_device_count 1`). This is the first
two-GPU boot. Host: 2x RTX 4070, driver 575.51.03, host BDFs `0000:00:07.0`
and `0000:00:09.0`; guest Ubuntu 24.04 / kernel 6.8, 4 vCPU / 16 GB.

`validate.sh` only ever touches device 0 (`cuDeviceGet(&dev, 0)`), so it cannot
answer this on its own. `tests/multi_gpu.c` was added for it: it walks every
device libcuda reports and, on each, creates a context, round-trips 8 MiB,
JIT-compiles PTX and launches a kernel whose every element is checked — with
per-device operands, so a launch routed to the wrong GPU cannot compare equal.

| check | before (stock) | after (this branch) |
|---|---|---|
| `/dev/nvidia*` nodes | `nvidia0` only | `nvidia0`, `nvidia1` |
| guest `nvidia-smi -L` | 1 GPU | 2 GPUs, both host UUIDs |
| `cuDeviceGetCount` | **1** | **2** |
| `gpus/` procfs dirs | `0000:00:07.0` only | `0000:00:07.0`, `0000:00:09.0` |
| GPU 1 kernel launch | unreachable — device never enumerated | **1048576/1048576 correct** |
| `vk_physical_device` | NVIDIA + llvmpipe | **2x NVIDIA + llvmpipe** |
| `validate.sh` | 28/28 | **28/28** |

The stock failure was *invisibility*, not breakage: `num_gpus` was a module
parameter defaulting to 1, so the guest never created `/dev/nvidia1` and libcuda
had nothing to enumerate. Loading the **unmodified** module with `num_gpus=2`
was already enough to get 2/2 devices fully usable — the forwarding path was
correct all along, because `dev_id = NVKVM_DEV_GPU(iminor(inode))` in
`nvkvm_open` already routes `/dev/nvidiaN` to the host's `/dev/nvidiaN`.

Both GPUs, verified per device (autodetect build):

```
CHECK|device_count|INFO|2
CHECK|gpu0|INFO|handle=0 name='NVIDIA GeForce RTX 4070' pci=0000:00:07.0 sm_89 smcount=46 totalmem=11874MiB
CHECK|gpu0|PASS|memcpy 8 MiB HtoD/DtoH byte-exact on device 0
CHECK|gpu0|PASS|vec_add kernel on device 0: 1048576/1048576 elements correct (bias=1000003)
CHECK|gpu1|INFO|handle=1 name='NVIDIA GeForce RTX 4070' pci=0000:00:09.0 sm_89 smcount=46 totalmem=11874MiB
CHECK|gpu1|PASS|memcpy 8 MiB HtoD/DtoH byte-exact on device 1
CHECK|gpu1|PASS|vec_add kernel on device 1: 1048576/1048576 elements correct (bias=2000006)
CHECK|summary|PASS|2/2 device(s) fully usable
```

Distinct UUIDs, matching the host's two cards exactly:

```
GPU 0: NVIDIA GeForce RTX 4070 (UUID: GPU-3e8efc0d-cad9-65a7-0316-f004d91d936b)
GPU 1: NVIDIA GeForce RTX 4070 (UUID: GPU-654a09ff-ce7b-db05-05b6-970a1bcdb91e)
```

### The kernel really ran on the second physical GPU

Enumeration proves nothing, so this was checked from outside the guest. With the
guest spinning a checked kernel on device 1, the **host's** `nvidia-smi`:

```
index, utilization.gpu, memory.used
0, 0 %, 4 MiB
1, 50 %, 173 MiB
```

and the mirror image with the guest on device 0:

```
0, 51 %, 173 MiB
1, 0 %, 4 MiB
```

1,935,010 launches over 25 s on device 1, final result still element-wise
correct.

### Both at once

Two guest processes, one pinned to each GPU, running concurrently:

```
CVD=0 -> pci=0000:00:07.0  spin 30.0s: 2260128 launches, final result still correct
CVD=1 -> pci=0000:00:09.0  spin 30.0s: 2301615 launches, final result still correct
```

Host during the overlap — both cards busy, so the two isolates are genuinely
parallel rather than serialized behind one device:

```
0, 46 %, 173 MiB
1, 50 %, 173 MiB
```

Throughput per process matches the solo run (~2.3M launches / 30 s either way),
i.e. no contention penalty from the one-isolate-per-guest-process model.

`CUDA_VISIBLE_DEVICES=1` correctly resolves to `pci=0000:00:09.0`, so the
ordinal remapping libcuda does on top of nvkvm's device list is consistent.

### Override behaviour

| `num_gpus=` | exposed | `cuDeviceGetCount` | result |
|---|---|---|---|
| unset (`-1`, autodetect) | 2 | 2 | 2/2 usable |
| `1` | 1 | 1 | 1/1 usable — single-GPU behaviour preserved |
| `4` (overshoot) | 4 nodes, 2 procfs dirs | 2 | 2/2 usable, extra nodes inert |

### Not covered

Three or more GPUs; NVLink/P2P between guest GPUs; mixed GPU models in one
host; `numa_status`, which stays `ENOENT` (as it was before this work — guest
`nvidia-smi` probes it and tolerates its absence). Only one identity PCI device
is created, so the guest PCI bus shows one NVIDIA card whatever the GPU count;
CUDA/NVML/Vulkan go through RM and do not care.

Exact strings, since a renderer check that does not print its renderer is not
worth much:

- `gl_renderer` on 610: `GL_RENDERER='NVIDIA GeForce RTX 3060/PCIe/SSE2'`
  `GL_VENDOR='NVIDIA Corporation'` `GL_VERSION='OpenGL ES 3.2 NVIDIA 610.43.02'`
- `vk_device_is_nvidia` on 610: `NVIDIA GeForce RTX 3060 (vendorID 0x10DE)`
- `vk_physical_device` everywhere: two devices are enumerated — the NVIDIA GPU
  and Mesa `llvmpipe`. The suite selects the NVIDIA one and would FAIL if only
  llvmpipe were present. That is not hypothetical; see the llvmpipe incident
  below.

## Finding: offscreen GL rendering is broken on 595 and 610

> **RESOLVED 2026-08-17 — this was ours, and it is fixed.** The cause is
> nvkvm's NVKMS inner-cmdType allowlist, not the NVIDIA driver. The same probe
> passes on bare metal on 595.84 and 610.43.02; it failed only inside a guest,
> because branches 595+ issue NVKMS `cmdType=60` and
> `src/qemu/nvkvm_nvkms_allowlist.h` — captured on a 575-era session — denied
> it. Allowing 60 takes the 610 guest from 27/28 to **28/28**. Full evidence in
> [Attribution](#attribution-nvkvm-not-nvidia--measured-2026-08-17) below; the
> original observation is kept as written.

`gl_draw_pixel_check` allocates its own 64×64 FBO, clears it, draws a triangle
over the lower-left half, and asserts the triangle colour inside and the clear
colour outside. On 595.84 and 610.43.02 the FBO never becomes complete:

```
gl_draw_pixel_check  FAIL  FBO incomplete: status=0x8CDD
```

`0x8CDD` is `GL_FRAMEBUFFER_UNSUPPORTED`. A standalone probe in the guest
(since committed as `tests/integration/fbo_formats_probe.c`) confirmed it is not
an attachment-format issue — **every** configuration fails identically, with
`glGetError() == GL_NO_ERROR` at every step:

| colour attachment | `glCheckFramebufferStatus` |
|---|---|
| `GL_RGBA` / `GL_UNSIGNED_BYTE` texture | `0x8CDD` |
| `GL_RGBA8` renderbuffer | `0x8CDD` |
| `GL_RGB565` renderbuffer | `0x8CDD` |
| `GL_RGBA4` renderbuffer | `0x8CDD` |
| `GL_RGB5_A1` renderbuffer | `0x8CDD` |

This is **not** an ABI-row problem and not GPU-specific:

- 595.84 and 580.95.05 select the *same* profile row (580), on the *same box*
  and the *same GPU*, and 580.95.05 passes while 595.84 fails.
- 610.43.02 and 550.54.14 ran on the *same box*, *same GPU (RTX 3060)* and the
  *same guest image*; 550 passes, 610 fails.

So it tracks the driver version, not the profile and not the hardware:

| driver | offscreen FBO |
|---|---|
| 545.23.08 | works |
| 550.54.14 | works |
| 580.95.05 | works |
| 595.84 | **broken** |
| 610.43.02 | **broken** |

590.48.01 would have narrowed the 580.95.05 → 595.84 window further. It was
attempted on the same box and abandoned: purging 595 left its DKMS modules in
`/lib/modules/.../updates/dkms`, so `modprobe` kept loading 595.84 no matter
what apt had installed, and forcing them out left the box with no buildable
driver. Not pursued further — it is a nice-to-have for bisecting, not a profile
row. Anyone bisecting this should `dkms remove` the outgoing driver *before*
purging its packages. (That advice is correct and was followed on 2026-08-17;
the swap works cleanly now. 590 is still not reachable, for an unrelated
packaging reason — see "590.48.01: DID NOT RUN" below.)

Everything else about GL is healthy on the broken drivers — the EGL device
binds, the context is current, and `GL_RENDERER` is the NVIDIA GPU. Only
render-target allocation fails. Note this is a *different* thing from the
known-open "GL clients under a Wayland compositor do not present" issue: there
is no compositor here, and the failure is at `glCheckFramebufferStatus`, before
anything is presented anywhere.

Worth knowing because a suite that stopped at `GL_RENDERER` — which is what
"offscreen EGL works" has meant so far — reports these two drivers as fully
working.

## Attribution: nvkvm, not NVIDIA — measured 2026-08-17

The obvious reading of the table above is "NVIDIA broke offscreen GL in 595".
That reading is wrong, and one cheap test kills it: **run the same probe on
bare metal, no VM, on a host running the suspect driver.**

### The decisive test

Ubuntu 22.04 box, RTX 3060, kernel 6.8.0-59-generic. Host driver swapped in
place to 610.43.02 (`nvidia-driver-610-open=610.43.02-0ubuntu0.22.04.1`;
`nvidia-smi` on 610 renames its banner fields to `KMD Version` / `CUDA UMD
Version`, which breaks hardcoded parsers). `tests/validate.sh` run **on the
host**, no guest anywhere:

```
NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.02  Release Build

  PASS gl_renderer            GL_RENDERER='NVIDIA GeForce RTX 3060/PCIe/SSE2' GL_VENDOR='NVIDIA Corporation' GL_VERSION='OpenGL ES 3.2 NVIDIA 610.43.02'
  PASS gl_draw_pixel_check    64x64 FBO: inside(16,16)=RGBA(255,127,0,255)==triangle, outside(48,48)=RGBA(0,0,0,255)==clear

 TOTAL 28   PASS 26   FAIL 1   SKIP 1
```

The one FAIL is `guest_module` (`nvkvm_guest not in lsmod`), which is the
correct answer outside a guest, and `abi_profile` SKIPs for the same reason.
The five-format probe from the table above
(`tests/integration/fbo_formats_probe.c`), on that same bare-metal host:

```
FBO| GL_RGBA/GL_UNSIGNED_BYTE texture   status=0x8CD5 GL_FRAMEBUFFER_COMPLETE  glGetError alloc=0x0 attach=0x0 after=0x0
FBO| GL_RGBA8 renderbuffer              status=0x8CD5 GL_FRAMEBUFFER_COMPLETE  glGetError alloc=0x0 attach=0x0 after=0x0
FBO| GL_RGB565 renderbuffer             status=0x8CD5 GL_FRAMEBUFFER_COMPLETE  glGetError alloc=0x0 attach=0x0 after=0x0
FBO| GL_RGBA4 renderbuffer              status=0x8CD5 GL_FRAMEBUFFER_COMPLETE  glGetError alloc=0x0 attach=0x0 after=0x0
FBO| GL_RGB5_A1 renderbuffer            status=0x8CD5 GL_FRAMEBUFFER_COMPLETE  glGetError alloc=0x0 attach=0x0 after=0x0
SUMMARY| 0/5 configurations incomplete
```

595.84 on bare metal (second box, also RTX 3060, also 22.04/6.8.0-59): same,
`gl_draw_pixel_check PASS`, `SUMMARY| 0/5 configurations incomplete`. 575.51.03
on bare metal before the swap: same. **Every driver passes on bare metal.**

Then the guest was booted on that first box — same box, same GPU, same
610.43.02, same probe source compiled from the same file:

```
SUMMARY| 5/5 configurations incomplete
```

Bare metal 0/5, guest 5/5, one machine, one driver, one afternoon. That is the
A/B, and it says the guest path is what breaks it.

### Root cause

The guest's QEMU log during the failing probe contains exactly one refusal:

```
nvkvm: DENY nvkms cmdType=60
```

That is `src/qemu/nvkvm_nvkms_allowlist.h`, whose allowed set was *captured
from a live 575-era Vulkan/EGL session*. A list captured on one driver branch
expires when the branch moves. An instrumented QEMU build, logging every NVKMS
wrapper (allowed or denied) rather than only the denials, shows what the 610
ICD actually issues for one offscreen context:

```
NVKMS cmdType=0  size=1440 paramsz=16     (ALLOC_DEVICE)
NVKMS cmdType=17 size=152  paramsz=16     (REGISTER_SURFACE)
NVKMS cmdType=60 size=32   paramsz=16     <-- denied
NVKMS cmdType=18 size=16   paramsz=16     (UNREGISTER_SURFACE)
```

Three things worth reading off that trace. `60` lands *between* register and
unregister, so it acts on a just-registered surface. It carries a 32-byte
params block. And `61`/`62` — the two "query-class (captured)" entries the
allowlist was built around — are **not issued at all** by the 610 ICD, so 60 is
less an addition to that pair than its replacement.

Denying it returns `-EACCES` / `NV_ERR_NOT_SUPPORTED`. The ICD's response is to
unregister the surface and report every colour format unrenderable — which is
precisely the observed symptom, including the clean `glGetError()`: from GL's
point of view nothing errored, there is simply no renderable format.

The command is **not named** here because it cannot be. The DKMS tree ships
only `nvidia-modeset/nvkms-ioctl.h` (the wrapper struct); the
`NvKmsIoctlCommand` enum that would name 60 is not in any shipped header.
`grep -rn NVKMS_IOCTL_ALLOC_DEVICE /usr/src/nvidia-610.43.02/` returns nothing.

### The fix, and what it measures

One line — `case 60:` in `nvkvm_nvkms_allowlist.h`. Rebuild QEMU, reboot the
guest, nothing else changed:

| | 610.43.02 guest | 595.84 guest |
|---|---|---|
| five-format probe, before | 5/5 incomplete | 5/5 incomplete |
| five-format probe, after | **0/5 incomplete** | **0/5 incomplete** |
| `gl_draw_pixel_check`, before | FAIL 0x8CDD | FAIL 0x8CDD |
| `gl_draw_pixel_check`, after | **PASS** | **PASS** |
| `validate.sh`, after | **28/28 PASS** | `gl_draw_pixel_check` PASS † |

† the 595 box scores 25/28 for an unrelated reason: its host was built from a
component package subset that omits `libnvidia-ptxjitcompiler`, so
`cuda_ptx_jit` FAILs `rc=221 CUDA_ERROR_JIT_COMPILER_NOT_FOUND` and the two
kernel checks SKIP behind it. That is a defect in how that box was provisioned,
not in nvkvm, and it is orthogonal to GL. The 610 box, provisioned from the
full driver metapackage, is a clean 28/28.

So both "broken" rows in the table above are now **28/28-capable**, and the
610 row's `27/28` is stale.

### Correction to the denied-commands table

The 595.84 row below reads "none observed". That is wrong — it was measured
before the GL probe ran, and NVKMS denials only appear once something creates
an offscreen context. Re-measured on a 595.84 guest, the *only* denial during
the failing probe is `nvkms cmdType=60`, and after the fix 595.84 shows the
same three RM control denials 610 does (`0x00730102`, `0x2080019f`,
`0x2080220b`). Those three are genuinely harmless: on 610 the guest scores
28/28 *with them still being denied* — the DENY count goes 12 → 4 and every
remaining one is an RM control.

### 590.48.01: DID NOT RUN — not installable from the Ubuntu archive

The bisection between 580.95.05 (works) and 595.84 (broke) was attempted and
abandoned again, for a different reason than last time. The DKMS trap noted
below is real and is now handled — `dkms remove <module>/<version> --all`
before purging the packages, and the outgoing driver goes away cleanly. The
wall is packaging: in jammy, `nvidia-driver-590-open` (590.48.01-0ubuntu0.22.04.4)
is a metapackage whose *only* dependency is `nvidia-driver-595-open`, and every
590 component package pulls its 595 counterpart alongside itself. Installing
the components explicitly and pinned still ends with both trees on disk, 595
winning the vendor links, and `nvidia-smi` reporting **595.84**:

```
Depends: nvidia-driver-595-open
...
NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  595.84
```

A standalone 590.48.01 needs NVIDIA's `.run` installer, not apt. Since the
attribution no longer depends on it — the bug is ours, and it is fixed on both
595 and 610 — this was dropped rather than pursued. Anyone who wants the exact
NVIDIA release that introduced cmdType 60 should start from the `.run`.

## Control commands denied by the allowlist

Non-fatal on every row (all workloads still passed), but new branches issue
control commands the allowlist was generated without:

| driver | denied |
|---|---|
| 545.23.08 | `0x00730102`, `0x00730138` |
| 550.54.14 | `0x00730102` |
| 580.95.05 | none observed |
| 595.84 | ~~none observed~~ → `0x00730102`, `0x2080019f`, `0x2080220b`, nvkms `cmdType=60` |
| 610.43.02 | `0x00730102`, `0x2080019f`, `0x2080220b`, nvkms `cmdType=60` |

The 595.84 row was re-measured 2026-08-17; "none observed" was taken before
anything created an offscreen GL context, which is the only thing that reaches
NVKMS. 595 and 610 in fact deny the same four things.

The three RM control commands are non-fatal — 610.43.02 scores 28/28 with all
three still denied. **The NVKMS `cmdType=60` denial was not**: it is what broke
offscreen GL on both branches, and it is now allowed. See
[Attribution](#attribution-nvkvm-not-nvidia--measured-2026-08-17). The general
lesson stands and is worth stating plainly: *an allowlist captured on one
driver branch is a time bomb on the next one*, and its failures do not
necessarily look like denials — this one surfaced four layers up as
`GL_FRAMEBUFFER_UNSUPPORTED` with `glGetError()` clean.

## Profiles not reached, and why

**515 and 525 are unreachable on a kernel-6.8 host.** The 525 row is only
exercised by 525.x, 535.43.0x (<.08) or 535.54.x — all 2022/2023 drivers whose
DKMS modules do not build against kernel 6.8. `535.54.03` (the version that
would exercise the non-monotonic 535 split, and the most interesting single
target in that row) fails with **22 compile errors**. That is too much to patch
without changing driver behaviour, and a heavily patched driver is weak evidence
about an ABI.

For contrast, 545.23.08 failed with exactly **one** error — `DRM_UNLOCKED`
undeclared, a flag removed in kernel 6.8 that has been a no-op since 4.x. That
was safe to shim (`#define DRM_UNLOCKED 0`), and the shim is in `nvidia-drm`,
which touches no RM or UVM ioctl struct, so the ABI under test is unaffected.
That one-line fix is what made the 545 row reachable at all.

To reach 515/525 someone needs a host whose kernel is old enough for those
drivers to build — roughly ≤6.5, i.e. a stock Ubuntu 22.04 GA kernel (5.15)
rather than the 6.8 HWE kernel every KVM-capable box rented here was running.
The 550.40.0x half of the 545 row is a separate matter: it is not packaged at
all (the oldest 550 in NVIDIA's apt repo is 550.54.14), so exercising the
intra-550 split needs a `.run` installer, not apt.

## Bugs this run found

Three, all in repo scripts rather than the ABI table, and all of the same shape
— a script reporting success while leaving the guest in a broken state.

1. **`stage_guest_libs.sh`: `ldconfig` re-pointed the GL/Vulkan vendor links at
   a stale driver.** NVIDIA libraries carry the same SONAME regardless of
   version, so with two versions present `ldconfig` version-sorts and picks the
   higher-numbered — the stale one. Re-staging a guest from 610 down to 550 left
   `libEGL_nvidia.so.0` and `libGLX_nvidia.so.0` pointing at the 610 files, and
   EGL and Vulkan both fell back to Mesa **llvmpipe** while the script printed
   "staged 22 libs" and "done". CUDA was unaffected, so 12/12 CUDA checks passed
   and the guest looked healthy. Fixed by sweeping differently-versioned NVIDIA
   libraries before `ldconfig` and re-asserting the vendor links after it.

2. **`stage_guest_libs.sh`: bundle chosen alphabetically.** The default was
   `ls -d host-libs-* | head -1`. A box taken from 580.95.05 to 595.84 has both
   bundles, and `580.95.05` sorts first, so the guest was staged with the old
   userspace against the new kernel driver — `cuInit rc=803`,
   `CUDA_ERROR_SYSTEM_DRIVER_MISMATCH`. Fixed by selecting the bundle matching
   the host driver version the guest module reports, and refusing to guess when
   it cannot be determined.

3. **`setup_guest.sh`: the 9p mount was not persistent.** cloud-init `runcmd`
   runs once per instance, so any later boot of the same image came up with
   `/mnt/nvkvm` empty — no module source, no staging script, no test suite.
   Added an fstab entry.

Both `stage_guest_libs.sh` bugs only bite when a guest is re-staged across a
driver change, which is exactly what building this matrix required, and exactly
what anyone adding a new driver version will do.

Two more turned up on 2026-08-17 while chasing the FBO failure, both in the
rebuild loop rather than in nvkvm itself:

4. **`run_remote_test.sh rebuild` cannot actually rebuild QEMU.** It copies
   `src/qemu/*.c`/`*.h` into `/opt/qemu-src/hw/misc/` and runs `ninja`, but
   skips steps 4–5 of `build_qemu.sh` — staging `src/abi` + `src/common`
   headers into `hw/misc/nvkvm_inc/` and rewriting the `../../src/{common,abi}/`
   includes to point at them. The build stops at
   `fatal error: ../../src/common/nvkvm_proto.h: No such file or directory`.
   Anyone iterating on the QEMU side must replicate those two steps, or the
   ninja run fails and (worse, if the failure is not read) the *old* binary
   stays installed and the experiment silently measures nothing.

5. **`pkill -f '[q]emu-system-x86_64'` kills the shell that runs it** whenever
   the rest of that command line also mentions the binary — e.g. the
   `cp /opt/qemu-src/build/qemu-system-x86_64 ...` that usually follows it. The
   bracket trick only hides the *pattern* from itself; the literal path later
   in the same command line still matches, pkill sees its own remote shell, and
   everything after the pkill silently never runs. Put the pkill on a line of
   its own, in its own SSH invocation.

The suite caught all three by checking values rather than exit codes: the
llvmpipe fallback showed up as
`vk_device_is_nvidia FAIL: SOFTWARE RASTERISER: 'llvmpipe ...' (vendorID 0x10005)`,
and the 803 showed up as `cuda_init FAIL` with every downstream CUDA check
reported SKIP rather than passed or omitted.


## Blackwell (RTX 5090, sm_120) — driver 580.178.04, 2026-08-18

> **RESOLVED, same day.** The section below records the first run (20/28) and is
> kept because the diagnostic trail is the useful part. Blackwell is now **28/28**
> — see "The fix" at the end.
>
> **One correction to my own analysis below:** I attributed `gl_draw_pixel_check`
> to an sRGB framebuffer, with arithmetic showing 128 linear encodes to 188 sRGB.
> The arithmetic is correct and the conclusion was wrong — that check passes
> unchanged once a CUDA context can be created, so it was cascading from the same
> root cause, not an independent test-expectation bug. A plausible mechanism with
> supporting numbers is still a guess if you do not test it.

First run on a fourth GPU architecture. **Enumeration works end to end; context
creation does not.** No code changes were made for this run — the guest module,
QEMU device and ABI profile selection were used exactly as shipped.

```
TOTAL 28   PASS 20   FAIL 3   SKIP 5      VERDICT: FAIL
```

Working (12 checks, unmodified):

```
guest_module          nvkvm_guest loaded
dev_nodes             5/5  nvidia0 nvidiactl nvidia-uvm nvidia-uvm-tools nvidia-modeset
nvidia_smi_gpu        NVIDIA GeForce RTX 5090
nvidia_smi_driver     580.178.04
nvidia_smi_cuda       13.0
abi_profile           host driver 580.178.04 -> profile selected, slot_size=65536
cuda_init             rc=0
cuda_driver_version   CUDA 13.0 (raw 13000)
cuda_device_count     1
cuda_device_name      NVIDIA GeForce RTX 5090
cuda_compute_cap      sm_120
vk/egl/gl             loader, instance, physical device, context, renderer all NVIDIA
```

### The single blocker

```
FAIL  cuda_ctx_create   rc=1 (CUDA_ERROR_INVALID_VALUE)
```

Everything downstream cascades from it — five CUDA checks SKIP (correctly recorded
as SKIP, not PASS), and `vk_compute_dispatch` fails `vkCreateDevice rc=-3`
(`VK_ERROR_INITIALIZATION_FAILED`) for the same reason: no context.

**This is the fourth instance of one recurring bug class**, and the previous three
were all resolved the same way:

| symptom | cause | fix |
|---|---|---|
| Ada `cuInit -> 801` | `NV50_THIRD_PARTY_P2P` allowed but absent from the guest size switch, so `alloc_parms_size=0` forwarded 0 bytes | add the size |
| NVENC `InitializeEncoder` bails | `NV01_CONTEXT_DMA` had no entry in the per-class alloc-params size table | add `NV_CONTEXT_DMA_ALLOCATION_PARAMS` |
| offscreen GL dead on 595/610 | NVKMS allowlist captured on 575 never saw `cmdType 60` | allow 60 |

The expected cause here is a Blackwell-specific alloc class or control that is
either absent from an allowlist, or allowed but with an unknown params size. The
documented diagnostic is a host-vs-guest ioctl trace diff (`LD_PRELOAD` on the
host, QEMU `DENY` log plus `rmdump` in the guest). **Not yet investigated.**

### Two results that are NOT nvkvm defects

`gl_draw_pixel_check` fails with `inside=RGBA(255,188,0,255)`, expected ~128 green.
That is an **sRGB framebuffer**: 128/255 = 0.502 linear encodes to 0.735 sRGB =
187.6 ≈ 188. The draw is correct; the suite's expectation assumes a linear
framebuffer, which held on Ampere/Ada/Turing and does not here. Fix belongs in
`tests/validate.sh`.

`vk_compute_dispatch` is a cascade of `cuda_ctx_create`, not an independent Vulkan
defect — `vk_instance`, `vk_physical_device` and `vk_device_is_nvidia` all pass.

### Host-side note worth recording

The RTX 5090 **requires the NVIDIA open kernel modules**. A box provisioned with
the proprietary `nvidia-driver-575` shows the GPU on the PCI bus and then fails:

```
NVRM: The NVIDIA GPU 0000:00:07.0 (PCI ID: 10de:2b85)
NVRM: installed in this system requires use of the NVIDIA open kernel modules.
NVRM: GPU 0000:00:07.0: RmInitAdapter failed! (0x22:0x56:884)
```

Installing `nvidia-driver-580-open` resolves it. This is a host provisioning
requirement, unrelated to nvkvm, but it will bite anyone bringing up Blackwell.

### The fix

**`BLACKWELL_CHANNEL_GPFIFO_A` (0xC96F) was allowlisted but unsized** — the silent
variant of the class, which is why the QEMU log showed no `DENY`. Instrumenting the
guest forwarder to log every `RM_ALLOC` isolated it to the last alloc of
`cuCtxCreate`:

```
before:  alloc hClass=0xc96f ap_size=0    origsz=0  status=0x1f   (NV_ERR_INVALID_ARGUMENT)
after:   alloc hClass=0xc96f ap_size=368            status=0x0
         alloc hClass=0xcab5 ap_size=8              status=0x0
```

libcuda passes a real params pointer but leaves `alloc_parms_size=0`, expecting the
driver to size by hClass. Neither size-by-hClass switch had a case for 0xC96F, so
0 bytes of params reached the host RM. Blackwell reuses `NV_CHANNEL_ALLOC_PARAMS`
unchanged, so it shares the existing size (368 on the 580 profile, confirmed by
compiling `sizeof`). Added 0xC96F and `_B` 0xCA6F to **both** the nvos21 and nvos64
switches.

**A second, latent bug surfaced while verifying.** `BLACKWELL_DMA_COPY_A` was
recorded as `0xCBB5` — not a class NVIDIA ships. There is no `clcbb5.h` in
open-gpu-kernel-modules, and 0xCBB5 is absent from QEMU's alloc allowlist while
0xC9B5/0xCAB5 are present. Corrected to 0xC9B5 and added `_B` 0xCAB5. This was not
speculative: with the channel fixed, the trace shows `hClass=0xcab5 ap_size=8
status=0x0` immediately after every channel alloc — the 5090 uses the id that did
not exist in the tree at all.

All four ids confirmed against OGKM 580.95.05 (`clc96f.h`, `clca6f.h`, `clc9b5.h`,
`clcab5.h`). No QEMU change was needed.

**Result: 28 PASS / 0 FAIL / 0 SKIP**, reproduced twice. Blackwell is the fourth
architecture fully working, after Ampere, Ada and Turing.

### Why this keeps happening

This is the fourth instance of "class reachable but params size unknown" in one
month. The failure is always silent — nothing is denied, 0 bytes are forwarded, and
the error surfaces several layers away as `INVALID_VALUE` or `801`. A guest-side
warning when a *known-allowed* hClass hits the default arm of a size switch would
have turned every one of these into a one-line log message instead of a bisect.

## Hopper (sm_90) — wired, not testable on vast.ai

Checked 2026-08-18 while adding Blackwell. **No work is needed and none was done.**

`HOPPER_CHANNEL_GPFIFO_A` (0xC86F) and `HOPPER_DMA_COPY_A` (0xC8B5) are already
defined in `src/abi/nvgpu.h` and already present in **both** size-by-hClass
switches, alongside the Ampere and Blackwell classes. So the trap that broke
Blackwell — allowlisted but unsized — does not apply to Hopper.

That is safe by construction rather than by luck: `alloc_channel.h` in
open-gpu-kernel-modules defines a single `NV_CHANNEL_ALLOC_PARAMS`, aliased as
`NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS` for every GPFIFO class. `clc86f.h` defines
only Hopper's *control* struct, not its own alloc params. Hopper therefore shares
the same `chan_alloc_size` the group already uses.

**It has not been booted, and cannot be on vast.ai.** This is not a Hopper-specific
shortage — datacenter GPUs on vast do not offer VM support at all. Measured across
620 single-GPU offers on 2026-08-18:

| class | offers | `vms_enabled` | |
|---|---|---|---|
| consumer (other) | 314 | 24 | 7.6% |
| consumer Blackwell | 129 | 9 | 7.0% |
| consumer Ada | 108 | 7 | 6.5% |
| **datacenter Ampere/Ada** (A100, L40, V100) | **52** | **0** | **0%** |
| **datacenter Hopper/Blackwell** (H100, H200, B200) | **17** | **0** | **0%** |

Zero of 69 datacenter offers, across two independent categories, against a ~7%
consumer base rate — under which you would expect about five. So the KVM template
this matrix depends on is simply not offered on datacenter hardware.

Testing Hopper needs nested virtualisation plus an H100 from somewhere else: a
bare-metal cloud instance (AWS `.metal`, GCP with nested virt enabled) or physical
hardware.

Testing Hopper needs a host with nested virtualisation and an H100 from somewhere
other than vast — a bare-metal cloud instance, or hardware access. Recorded as an
open gap rather than an assumed pass: the classes being present means the *known*
failure mode is covered, not that Hopper works.

**Where the H100 row actually came from: Spheron.** Worth recording, because the
paragraph above suggests bare metal is the only route and it is not. Spheron's
GPU instances are VMs, but *some flavors expose `/dev/kvm`* — the H100 that
produced the 27/28 row was one of them.

Not all of them do, and the console does not tell you which. An A100 80GB PCIe
in `Canada 1` (2026-08-20) is a VM **without** nested virt, confirmed four ways:
`svm`/`vmx` absent from `/proc/cpuinfo`, `kvm-ok` reporting no KVM extensions,
`kvm_amd: SVM not supported by CPU` in dmesg, and `modprobe kvm_amd` failing
with `Operation not supported`. The listing showed `Dedicated` and `PCIE`, both
of which are true *of the GPU* and say nothing about the host.

**So probe before committing.** Boot, run `ls /dev/kvm`, and destroy if it is
missing — that costs a few cents and is the only reliable signal. A `VM` badge
means *ask*, not *no*.
