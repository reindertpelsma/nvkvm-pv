# Guest kernel support

`nvkvm-guest.ko` is an out-of-tree module built against the guest's own kernel
headers. That makes the **distro** irrelevant and the **kernel version**
decisive.

Until 2026-08-19 the module carried no version guards at all, so it built only
on a narrow band around 6.8 — excluding both widely deployed LTS kernels below
it and everything current above it. `src/guest/nvkvm_compat.h` now holds one
guard per API that actually broke a build; nothing is shimmed speculatively.

Reproduce the whole table in one command — no GPU, no VM, nothing to install
but Docker:

```bash
bash tests/kernel_matrix.sh                 # default image set
bash tests/kernel_matrix.sh debian:13       # or specific images
```

## Builds

Both variants (`NVKVM_GRAPHICS=1` and `0`), each against that distro's own
kernel headers and toolchain:

| kernel | LTS | source used to test | builds |
|---|---|---|---|
| 5.15 | yes | Ubuntu 22.04 | **yes** |
| 6.1 | yes | Debian 12 | **yes** |
| 6.6 | yes | mainline headers | **yes** |
| 6.8 | — | Ubuntu 24.04 | **yes** |
| 6.12 | yes | Debian 13 | **yes** |
| 6.14 | — | Ubuntu 25.04 | **yes** |
| 6.19 | — | Fedora 42 | **yes** |

That covers every LTS in the range NVIDIA's driver supports, plus current
stable.

**A build is not a run**, so two of them were run:

| guest | kernel | `tests/validate.sh` |
|---|---|---|
| Ubuntu 24.04 | 6.8 | **28/28** |
| Debian 12 | 6.1 | **28/28** |

and the **host** side has now run on kernel 7.0 as well (Ubuntu 26.04, RTX
4070) — previously 7.0 was a compile result only.

The Debian run is the one that matters — 6.1 could not be compiled at all
before this work, and it exercises the shimmed paths (`zap_vma_ptes`,
`pud_leaf`, the `vm_next` VMA walk, `class_create`) on real hardware rather
than in a compiler. The other five kernels are compile results only: a compile
failure is conclusive, a compile pass only says the API surface matches.

Getting Debian to 28/28 took two fixes that had nothing to do with kernel APIs
and would have bitten any non-Ubuntu guest:

- **`libxext6`.** `libGLX_nvidia.so.0` *is* the NVIDIA Vulkan ICD, and it links
  against libXext, which Debian's minimal cloud image does not ship. The Vulkan
  loader then cannot load the ICD, says nothing, and falls back to llvmpipe —
  `vk_device_is_nvidia` FAILs as "SOFTWARE RASTERISER" while everything else
  passes. Now in the guest package list.
- **`drm_shmem_helper`.** `drm_gem_shmem_dumb_create` lives in a separate module
  where a distro builds it modular, and `insmod` — unlike `modprobe` — pulls no
  dependencies, so the load failed with "Unknown symbol". The guest unit now
  modprobes it first.

## How old is worth supporting

The floor is a judgement call, so here is the reasoning rather than just the
number. In nvkvm the guest kernel is paired with **`nvkvm-guest.ko`**, not with
NVIDIA's driver — the guest never loads NVIDIA's kernel module, which runs only
on the host. So supporting guest kernels older than NVIDIA's own floor buys
nothing: the equivalent bare-metal setup would not have worked either.

That puts the useful range at "every LTS someone would pair with a current
driver", which is where it now sits. Below 5.15 is 5.4 (Ubuntu 20.04, past
standard support) and 4.15 (18.04, ESM only), and each step down costs more
shims — no `vm_flags` accessors, no maple tree, older DRM — for distros nobody
runs a modern CUDA workload on. Not worth it unless someone turns up with a
real need.

Note also that guest kernel and **host driver version are orthogonal**. The
ABI profiles key on driver version, but those describe ioctl struct layouts,
not kernel APIs. A 6.12 guest works against host driver 535 exactly as against
580.

## What had to change

Most of it is spelling — an API renamed or a parameter added — and those shims
are mechanical:

| API | changed in | note |
|---|---|---|
| `class_create()` | 6.4 | lost its `owner` argument |
| `devnode()` | 6.2 | `struct device *` became `const` |
| `vm_flags_set/clear()` | 6.3 | `vma->vm_flags` became `__private` |
| `vma_start_write()` | 6.4 | arrived with `CONFIG_PER_VMA_LOCK` |
| `pde_data()` | 5.17 | lowercased from `PDE_DATA()` |
| `VMA_ITERATOR` / `for_each_vma` | 6.1 | maple tree; before it, a `vm_next` walk |
| `pud_large()` / `pmd_large()` | 6.11 | became `pud_leaf()` / `pmd_leaf()` |
| `virtio_find_vqs()` | 6.12 | callbacks/names arrays became `struct virtqueue_info[]` |
| `drm_driver.date` | 6.14 | field removed |
| `hrtimer_init()` | 6.15 | became `hrtimer_setup()` |

Two are worth more than a table row.

**`vma_start_write()` compiles to nothing below 6.4.** That is correct there,
not a silent downgrade: those kernels have no per-VMA locks, so `mmap_lock`
alone is the entire exclusion the code needs.

**The range zap had no portable spelling.** Three exist and only one is
exported to modules everywhere:

| function | availability |
|---|---|
| `zap_page_range()` | removed in 6.4 |
| `zap_page_range_single()` | exported in a **narrow window** — measured *absent* from `Module.symvers` on 6.6 and 6.19, present on 6.8 and 6.12 |
| `zap_vma_ptes()` | exported on all seven kernels above |

So the driver calls `zap_vma_ptes()`. It returns `void` and does nothing at all
unless the VMA is `VM_PFNMAP`/`VM_MIXEDMAP`, so
`nvkvm_cpu_pages_migrate_range()` checks that itself rather than trusting a
silent no-op — a skipped zap would leave the guest reading its old anonymous
pages while the GPU reads the memfd, which is exactly the silent divergence
this driver [already shipped once](correctness.md).

## Kernels before 5.18

They build modules as `gnu89`, where a declaration inside a `for()` initialiser
is an error, and the tree uses them throughout. `src/guest/Kbuild` asks for
`-std=gnu11`; on newer kernels that matches what they already pass.
