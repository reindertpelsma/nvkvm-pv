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
| 610 | 610+ | **610.43.02** | no | **newly booted, 27/28** |

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
| `gl_draw_pixel_check` | PASS | PASS | PASS | **FAIL 0x8CDD** | **FAIL 0x8CDD** |
| **verdict** | **28/28 PASS** | **28/28 PASS** | **28/28 PASS** | **27/28** | **27/28** |

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

`gl_draw_pixel_check` allocates its own 64×64 FBO, clears it, draws a triangle
over the lower-left half, and asserts the triangle colour inside and the clear
colour outside. On 595.84 and 610.43.02 the FBO never becomes complete:

```
gl_draw_pixel_check  FAIL  FBO incomplete: status=0x8CDD
```

`0x8CDD` is `GL_FRAMEBUFFER_UNSUPPORTED`. A standalone probe in the guest
confirmed it is not an attachment-format issue — **every** configuration fails
identically, with `glGetError() == GL_NO_ERROR` at every step:

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
purging its packages.

Everything else about GL is healthy on the broken drivers — the EGL device
binds, the context is current, and `GL_RENDERER` is the NVIDIA GPU. Only
render-target allocation fails. Note this is a *different* thing from the
known-open "GL clients under a Wayland compositor do not present" issue: there
is no compositor here, and the failure is at `glCheckFramebufferStatus`, before
anything is presented anywhere.

Worth knowing because a suite that stopped at `GL_RENDERER` — which is what
"offscreen EGL works" has meant so far — reports these two drivers as fully
working.

## Control commands denied by the allowlist

Non-fatal on every row (all workloads still passed), but new branches issue
control commands the allowlist was generated without:

| driver | denied |
|---|---|
| 545.23.08 | `0x00730102`, `0x00730138` |
| 550.54.14 | `0x00730102` |
| 580.95.05 | none observed |
| 595.84 | none observed |
| 610.43.02 | `0x00730102`, `0x2080019f`, `0x2080220b`, nvkms `cmdType=60` |

610 is the notable one: three distinct RM control commands plus an NVKMS
command type. None of them broke CUDA, Vulkan or GL bring-up.

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

The suite caught all three by checking values rather than exit codes: the
llvmpipe fallback showed up as
`vk_device_is_nvidia FAIL: SOFTWARE RASTERISER: 'llvmpipe ...' (vendorID 0x10005)`,
and the 803 showed up as `cuda_init FAIL` with every downstream CUDA check
reported SKIP rather than passed or omitted.
