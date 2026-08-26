# Supported drivers and GPUs

## Host driver

`nvkvm` replays guest ioctls against the NVIDIA driver installed on the host, so
the host driver version determines the ABI profile
(see [ABI profiles](abi-profiles.md)) and the version of the NVIDIA userspace the
guest must run (see [staging guest libraries](../howto/stage-guest-libraries.md)).

QEMU probes the version at device realize with `NV_ESC_CHECK_VERSION_STR`, using
`cmd = '2'` (`NV_RM_API_VERSION_CMD_QUERY`) rather than `0` — the strict form is
enforced by the open-source `nvidia.ko` and would reject a query
(`src/qemu/virtio_nvgpu.c:1159-1169`). If `/dev/nvidiactl` cannot be opened,
device realize fails outright (`src/qemu/virtio_nvgpu.c:1152-1158`).

### Profile status

Eight profiles cover every published open-driver branch. The version ranges
below are **measured**, not assumed: `tools/abi_derive.sh` compiled
`sizeof`/`offsetof` probes against open-gpu-kernel-modules at every tag named in
the "measured from OGKM" column, and `tests/abi_parity` asserts the table
against those numbers (`go test -count=1 ./tests/abi_parity`).

`UVM_REGISTER_GPU_PARAMS` is a separate universal invariant, not another
profile row. A compiled sweep run on **2026-08-26** over every one of the 216
official numeric OGKM tags from 515.43.04 through 610.57.04 measured the same
layout at every tag: size 40, `rmCtrlFd` at byte 24, `hClient` at 28,
`hSmcPartRef` at 32 and `rmStatus` at byte 36 — 216/216 tags, no clone failure,
no unmeasured cell. There are therefore no driver-version boundaries for that
structure. The sweep output is committed at
`tests/abi_parity/ogkm_register_gpu.tsv` (raw form in
`ogkm_abi_sweep_20260826.tsv`) and is asserted against the shipped constants by
`tests/abi_parity/ogkm_fixture_test.go`, so it is checkable from a clone.
Reproduce it with `tools/abi_derive.sh --all-published-supported`; the ordinary
matrix also prints those values for its representative tags. Coverage stops at
515 because that is OGKM's first release — 470 and earlier have no public
source and cannot be probed at all.

| profile | driver versions | measured from OGKM | booted in this repository |
|---|---|---|---|
| `NVKVM_ABI_515` | 515 – 520 | 515.43.04, 515.57, 515.105.01, 520.56.06, 520.61.07 | **no** |
| `NVKVM_ABI_525` | 525 – 530, plus 535.43.0x (< .08) and 535.54.x | 525.47.04, 525.85.05, 525.147.05, 530.30.02, 530.41.03, 535.43.02, 535.54.03 | **no** |
| `NVKVM_ABI_535` | 535.43.08+ and 535.86+ | 535.43.08, 535.43.28, 535.86.05, 535.129.03, 535.183.01, 535.309.01 (+ 10 more 535 tags) | **yes** — 535.309.01 |
| `NVKVM_ABI_545` | 545, plus 550.40.0x (< .53) | 545.23.06, 545.23.08, 545.29.02, 545.29.03, 545.29.06, 550.40.07 | **yes** — 545.23.08 |
| `NVKVM_ABI_550` | 550.40.53 – 565 | 550.40.53 … 550.163.01, 555.58.02, 560.35.03, 565.57.01, 565.77 | **yes** — 550.54.14 |
| `NVKVM_ABI_570` | 570 – 579 (incl. 575) | 570.86.15, 570.172.08, 570.211.01, 575.51.02, 575.64.05 | **yes** — 575.51.03 |
| `NVKVM_ABI_580` | 580 – 595 | 580.65.06, 580.95.05, 580.178.04, 590.44.01, 590.48.01, 595.44.02, 595.91.07 | **yes** — 580.95.05 and 595.84 |
| `NVKVM_ABI_610` | 610+ | 610.43.02, 610.43.03, 610.57.04 | **yes** — 610.43.02 |

Two of those ranges split **inside** a branch, which is why selection takes the
full `major.minor.patch` and not just the major
(`nvkvm_abi_id_for_version()`):

- **535** — `NV_CHANNEL_ALLOC_PARAMS` gained the Confidential Computing fields
  (`encryptIv`, `decryptIv`, `hmacNonce`, +56 B) between 535.54.03 (measured
  304) and 535.86.05 (measured 360). 535.54.03 is the *original 535 GA*, so it
  is on the old side. The split is not monotonic in the version string: the
  long-lived 535.43.x maintenance train crosses over at 535.43.08 (2023-08-17),
  which is newer in wall-clock time than 535.54.03 (2023-06-14) despite sorting
  older.
- **550** — the UVM per-GPU attribute array grew 1200 → 9264 B between
  550.40.07 (measured 1200) and 550.40.53 (measured 9264).

Say the booting status plainly. Six of the eight rows have now had a VM brought
up on them: `535` and `570` previously, and `545`, `550`, `580` and `610` in the
boot-matrix run recorded in [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md).
Each of those booted rows was verified with `tests/validate.sh` inside the guest
— the whole ladder from the five device nodes through `cuCtxCreate`, a byte-exact
8 MiB round trip, a PTX-JIT kernel launch checked element by element, a matmul
against a CPU reference, a Vulkan compute dispatch and offscreen EGL. The `580`
row was exercised at both ends of its range (580.95.05 and 595.84) since it now
spans two branches.

`515` and `525` remain **unbooted**, and are hard to reach: the only drivers
that select them (515.x, 525.x, 535.43.0x, 535.54.x) do not build against a
kernel 6.8 host, which is what every KVM-capable rental box ran. 535.54.03 —
the most interesting target in the `525` row, being the old side of the
non-monotonic 535 split — fails with 22 compile errors. Reaching those two rows
needs a host on roughly kernel ≤6.5. Their values are measured from source and
asserted by the test suite, but no VM here has run on them: they are expected to
work; they are not evidence. Deriving a layout is not booting one.

**Not derivable:** 470 and earlier predate open-gpu-kernel-modules (515 was
NVIDIA's first open release), so there is no source to compile probes against
and no profile for them. That is a gap, not a zero.

If you boot one, the interesting artefacts are: the `nvkvm: host driver <v> →
ABI profile <n>` line QEMU prints at realize
(`src/qemu/virtio_nvgpu.c:1177-1178`), whether `cuInit` and `cuCtxCreate`
succeed, and whether any `nvkvm: DENY ctrl cmd 0x...` lines appear (a driver
branch may issue control commands the allowlist was generated without — see
`src/qemu/nvkvm_ctrl_allowlist.h:218-239` for exactly that happening on 535).

## GPUs

Every row below reached the full ladder — `cuInit` → `cuCtxCreate` → an 8 MiB
host↔device round trip verified byte-exact → `cuMemsetD8` verified → a real CUDA
kernel launch.

| GPU | architecture | compute capability | host driver | ABI profile |
|---|---|---|---|---|
| RTX 3060 | Ampere GA106 | 8.6 | 575.51.03 | 570 |
| RTX 4000 Ada | Ada AD104 | 8.9 | 575.51.03 | 570 |
| GTX 1660 SUPER | Turing TU116 | 7.5 | 575.51.03 | 570 |
| GTX 1660 SUPER | Turing TU116 | 7.5 | 535.309.01 | 535 |
| RTX 3060 | Ampere GA106 | 8.6 | 545.23.08 | 545 |
| RTX 3060 | Ampere GA106 | 8.6 | 550.54.14 | 550 |
| RTX 3060 Ti | Ampere GA104 | 8.6 | 580.95.05 | 580 |
| RTX 3060 Ti | Ampere GA104 | 8.6 | 595.84 | 580 |
| RTX 3060 | Ampere GA106 | 8.6 | 610.43.02 | 610 |

The last five rows additionally cleared the PTX JIT path, a matmul checked
against a CPU reference, a Vulkan compute dispatch and offscreen EGL, via
`tests/validate.sh` — see [`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md)
for the per-check values and for the one failure they surfaced: offscreen
framebuffer objects came back `GL_FRAMEBUFFER_UNSUPPORTED` on 595.84 and
610.43.02. That was **nvkvm's own bug, and it is fixed** — the NVKMS inner-
cmdType allowlist, captured on a 575-era session, denied the `cmdType=60` that
branches 595+ issue per offscreen surface. With 60 allowed both branches pass
`gl_draw_pixel_check` and 610.43.02 is a clean 28/28.

## Turing is the floor, and we are not moving it

The alloc-class allowlist (`src/qemu/nvkvm_fe_alloc_allowlist.h:59-149`) carries
the Volta, Turing, Ampere, Ada, Hopper and Blackwell channel/compute/DMA-copy
classes, and the guest's channel-alloc sizing handles
`TURING_CHANNEL_GPFIFO_A`, `AMPERE_CHANNEL_GPFIFO_A` and
`HOPPER_CHANNEL_GPFIFO_A` (`src/guest/nvkvm_main.c:1709-1713`). Pascal's classes
are absent, so a pre-Volta card would need them added.

That is deliberate, and it is a supported claim rather than an untested
assumption — a GTX 1080 was measured, and
[what it did](#the-gtx-1080-measurement) is below. Three reasons not to chase
pre-Turing:

- **The open kernel module cannot drive Pascal at all.** It refuses to probe:
  `not supported by open nvidia.ko because it does not include the required GPU
  System Processor (GSP)`. Pascal predates GSP. Any host on the open module —
  increasingly the default — has no driver for the card, with or without nvkvm.
- **Upstream support ends anyway.** NVIDIA has confirmed the 580 branch is the
  last to support Maxwell, Pascal and Volta; 590 moves them to
  maintenance/security only. Work here would target a frozen ABI.
- **nvproxy draws the same line.** gVisor's nvproxy lists T4 (Turing), A100/A10G,
  L4 and H100 as supported, with no Pascal or Volta. Our allowlists track
  nvproxy for parity, so keeping the same floor keeps that parity meaningful.

### The GTX 1080 measurement

Measured 2026-08-19 rather than assumed, which is why the floor is stated as a
fact rather than a guess.

On a stock KVM rental box the open module refuses the card outright:

```
NVRM: The NVIDIA GPU 0000:00:07.0 (PCI ID: 10de:1b80) installed in this system
NVRM: is not supported by open nvidia.ko because it does not include the
NVRM: required GPU System Processor (GSP).
```

Swapping to the proprietary module (`nvidia-driver-580-server`, 580.173.02) the
card initialises and forwarding partly works — the guest's `nvidia-smi` reports
the GTX 1080 and `validate.sh` gives **15 pass / 3 fail / 10 skip**:

| check | result |
|---|---|
| device/NVML/bring-up checks | pass (15) |
| `cuda_init` | **FAIL** — `cuInit rc=999 (CUDA_ERROR_UNKNOWN)` |
| the ten CUDA checks behind it | skipped, `cuInit failed` |
| `vk_compute_dispatch` | FAIL — `vkCreateDevice rc=-3` |
| `gl_draw_pixel_check` | FAIL — `FBO incomplete: status=0x8CDD` |

No allowlist denies anything during the attempt, so this is not default-deny
refusing a Pascal class — the remaining work is of unknown depth, for hardware
whose driver line ends at 580. Not worth it.

## Multi-GPU

**Up to six GPUs in one guest, measured.** The largest run is 6x RTX A4000 on
driver 570.124.06 (`validate.sh` 28/28 with `cuda_device_count 6`, six
concurrent isolates each driving their own card, all six busy at once); 4x RTX
5060 and 2x RTX 4070 are also on the
[tested platforms](tested-platforms.md) list, and the per-check output is in
[`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md). NVLink/P2P *between* guest
GPUs is still unexercised, and tensor-parallel serving is the one shape below
parity — [about 0.86x](parity.md#multi-gpu-tensor-parallel-serving).

The section below is the two-GPU bring-up that got it working, kept because it
names the three guest-side bugs and what each looked like.

**Two GPUs work, measured 2026-08-18** on a 2x RTX 4070 host (driver 575.51.03,
host BDFs `0000:00:07.0` and `0000:00:09.0`). Not merely enumerated: a PTX-JIT
`vec_add` ran on each device with all 1,048,576 elements checked, and the host's
own `nvidia-smi` attributed the load to the matching physical GPU (device 1 busy
=> host GPU 1 at ~50%, host GPU 0 at 0%, and the mirror image for device 0). Two
processes pinned with `CUDA_VISIBLE_DEVICES=0` and `=1` ran concurrently at full
speed, ~2.26M and ~2.30M launches in 30 s, both results verified. `validate.sh`
is 28/28 with `cuda_device_count 2`, and Vulkan enumerates both GPUs.

Getting there needed three guest-side fixes (the QEMU side was already
multi-GPU-correct — it scans `/dev/nvidia0..15`, keeps a sorted host-BDF list
and resolves per-GPU file requests against it):

- **`num_gpus` now autodetects.** It was a module parameter defaulting to `1`,
  so a stock guest exposed only `/dev/nvidia0` and `cuDeviceGetCount` returned
  **1** on a 2-GPU host — the second GPU was invisible, not broken. The default
  is now `-1`, which probes the host and exposes exactly what it has. A positive
  value still overrides.
- **`gpu_index` is no longer hardcoded to 0.** Every per-GPU host-file read
  (`information`, `registry`) asked for GPU 0 regardless of which GPU's file was
  being read, so GPU 1's procfs would have reported GPU 0's UUID and VBIOS.
- **`/proc/driver/nvidia/gpus/<bdf>/` is per-device and correctly named.** There
  was one hardcoded directory, `0000:00:07.0` — the guest slot the identity PCI
  device sits at. `strace` on guest `nvidia-smi` shows the lookup actually uses
  the BDF **RM** reports, i.e. the *host* BDF: on this box it probed
  `gpus/0000:00:07.0/` **and** `gpus/0000:00:09.0/`, and nothing in the guest
  lives at `00:09.0`. The old name therefore matched only by coincidence, on
  hosts whose first GPU also happens to sit at `00:07.0`. The tree is now built
  from BDFs discovered from the host, one directory per GPU.

Known gaps, neither of which blocks CUDA:

- `numa_status` is still `ENOENT` for every GPU (it was before this work too —
  only `information` and `registry` are synthesized). Guest `nvidia-smi` probes
  it and tolerates its absence. QEMU already whitelists
  `NVKVM_HFILE_NVIDIA_NUMA_STATUS`, so adding it is a one-line change if
  something ever needs it.
- Only **one** identity PCI device is created (`-device nvkvm-gpu,addr=7` in
  `scripts/run_test_vm.sh`), so the guest PCI bus shows a single NVIDIA card
  regardless of GPU count. CUDA, NVML and Vulkan all go through RM and are
  unaffected; anything that counts GPUs by walking guest PCI would see one.
- NVLink/P2P *between* guest GPUs has not been exercised. (Six GPUs in one
  guest has since been measured — see the head of this section — so "untested
  beyond two" no longer applies to device count.)

## Host CPU

**This is the first hardware requirement nvkvm has that is about the CPU rather
than the GPU.** It exists because the GPA windows have to live somewhere the
host's page tables can actually address.

nvkvm places three guest-physical windows above guest RAM — a 16 MiB shared
memory region, a 16 GiB legacy mmap window and a 128 GiB sparse window, 145 GiB
of GPA span in total. Those used to sit at fixed addresses (1 TB, 1.5 TB and
2 TB), which needs **41-42 physical address bits**. Every host this project was
developed on was a server part, so it always fit:

| host | CPU | physical address bits | max GPA |
|---|---|---|---|
| laptop | Core i7-11800H (Tiger Lake-H) | 39 | 512 GiB |
| rental | Xeon E5-2673 v4 (Broadwell) | 46 | 64 TiB |
| rental | EPYC 8224P (Zen 4) | 52 | 4 PiB |

39 bits is standard for consumer **mobile** Intel, and on such a host KVM
rejected the very first window at device realize:

```
qemu-system-x86_64: kvm_set_user_memory_region: KVM_SET_USER_MEMORY_REGION failed,
                    slot=6, start=0x10000000000, size=0x1000000: Invalid argument
kvm_set_phys_mem: error registering slot: Invalid argument
```

and QEMU died before the guest booted. (Note the failing slot is the 16 MiB
*shm* region, not the big sparse window — the whole fixed layout was affected.)

### What the requirement actually is

The window base is no longer a constant. At device realize nvkvm reads

- the **host** MAXPHYADDR from CPUID leaf `0x80000008` EAX bits 7:0
  (`/proc/cpuinfo`'s "address sizes" line is only a fallback — it can be
  filtered by container runtimes), and
- the **guest** MAXPHYADDR from the CPU's `phys-bits` QOM property, which is
  what `-cpu host` copies from the host and what QEMU may cap below it,

takes the **narrower of the two**, and places the whole 145 GiB block against
the top of that space, above guest RAM. `base + span <= 2^bits` is checked, not
just `base`.

The practical requirement is therefore:

> **The host needs enough physical address bits that 145 GiB of window fits
> above the guest's RAM.** In round numbers: **39 bits (512 GiB) is enough for
> any guest up to ~230 GiB of RAM**, which covers every consumer laptop. 46+
> bits, as on any server part, is enough for a 1 TB-RAM guest with room to
> spare.

Measured placements:

| host bits | guest RAM | resulting window base | sparse window |
|---|---|---|---|
| 39 | 6 GiB | `0x5bc0000000` (367 GiB) | 128 GiB, full |
| 46 | 16 GiB | `0xffbc0000000` (63.86 TiB) | 128 GiB, full |
| 46 | 1 TB (hypothetical) | 63.86 TiB | 128 GiB, full |

### When it does not fit

If the block will not fit, the sparse window is **halved** — deliberately, with
a `warn_report` naming the new size — down to a floor of **16 GiB**. The window
is large because a single `cuCtxCreate` fires >1500 individual mmaps and the
single-memslot window is what keeps those off KVM's ~509-memslot budget; what
matters is the window's total byte capacity, and 16 GiB still admits >1500
mappings averaging ~10 MiB. A shrink is logged as:

```
nvkvm: sparse GPA window 128 GiB does not fit below the 512 GiB GPA limit
       with 260 GiB of guest RAM — halving to 64 GiB
```

Below that floor realize **fails with a clear error** rather than starting with
a window that would break at the 1500th mmap:

```
nvkvm: the GPA windows do not fit in this VM's physical address space.
       host MAXPHYADDR 36 bits, guest MAXPHYADDR 36 bits -> usable GPA limit
       64 GiB. Guest RAM top is 12 GiB, so the windows must start above 32 GiB,
       and the smallest layout nvkvm supports needs 33 GiB (1 GiB shm + 16 GiB
       mmap window + 16 GiB sparse window) — 1 GiB more than there is room for.
       Reduce guest RAM to at most 10 GiB, or run on a host with more physical
       address bits.
```

A 36-bit host (64 GiB of GPA, the x86-64 architectural minimum) cannot run
nvkvm at any useful guest size. No such host has been tested — no part that old
carries a supported GPU.

### Checking your host

```bash
grep -m1 'address sizes' /proc/cpuinfo     # "39 bits physical, 48 bits virtual"
```

The chosen layout is logged at realize on every run, so the decision is always
auditable:

```
nvkvm: GPA width: host MAXPHYADDR 39 bits, guest MAXPHYADDR 39 bits -> using 39 bits (limit 0x8000000000, 512 GiB)
nvkvm: GPA windows: guest RAM top ~0x280000000 (10 GiB), floor 0x4000000000 -> block base 0x5bc0000000 size 145 GiB [shm 0x5bc0000000, mmap 0x5c00000000 +16 GiB, sparse 0x6000000000 +128 GiB]
```

### Interaction with the reservation BAR

The sparse window is also advertised as a 64-bit prefetchable reservation BAR
(`#55`), and it is the **firmware-assigned** BAR address that the window's KVM
memslot is actually installed at; the computed `sparse_base` is the fallback
when there is no BAR, and the override when firmware picks something unusable.

Where firmware puts it is **not** predictable from the address width alone.
Measured, SeaBIOS 1.16.3, same 128 GiB BAR:

| host bits | GPA limit | computed `sparse_base` | BAR actually assigned |
|---|---|---|---|
| 39 | 512 GiB | `0x6000000000` (384 GiB) | `0x6000000000` — same address |
| 46 | 64 TiB | ~63.86 TiB | `0x380000000000` (56 TiB) — 8 TiB lower |

So do not assume the BAR lands at `2^bits - window`; on the 39-bit host it did
and on the 46-bit host it did not. What matters is that both are inside the
limit and neither overlaps the shm/mmap regions, which nvkvm now checks
explicitly: if firmware places the BAR so that it crosses the GPA limit, or on
top of the shm/legacy-mmap regions, nvkvm logs that and uses its own computed
base rather than handing KVM a range it will reject or letting the window
shadow the shm slots.

One residual, untriggered risk is worth naming: the computed block is placed at
the top of the addressable space, which on some machines is inside the range
firmware treats as the 64-bit PCI hole. Nothing else large enough to reach it
has been observed (firmware allocated only our own BAR up there, and lower than
our block on the 46-bit host), and both verified hosts booted clean — but the
block is not itself PCI-reserved, so a machine that filled its 64-bit hole to
the top could in principle collide. Making the shm/mmap regions reservation
BARs too would close it.

## Host kernel

The QEMU-side isolate spawn uses `close_range(2)` where available (Linux 5.9+),
falling back to a `getdents64` loop over `/proc/self/fd`
(`src/qemu/nvkvm_isolate.c:276-308`). Isolate hardening uses unprivileged
`CLONE_NEWUSER` plus `CLONE_NEWPID|NEWNET|NEWIPC|NEWUTS|NEWNS`
(`src/qemu/nvkvm_isolate.c:128-133`) and `pivot_root`
(`src/qemu/nvkvm_isolate.c:205`), so unprivileged user namespaces must not be
disabled on the host. `NVKVM_ISOLATE_NO_HARDEN=1` skips all of it
(`src/qemu/nvkvm_isolate.c:795`) — a debugging hatch, not a supported mode.

## Guest kernel

The guest module is an out-of-tree kernel module built against the running guest
kernel's headers (`src/guest/Makefile:1`). It was developed and tested against
Ubuntu 24.04 / kernel 6.8 (`scripts/setup_guest.sh:16`, `:80`).

Two guest-side kernel dependencies are worth knowing about:

- `remap_pfn_range` on a prefetchable-BAR GPA range is silently downgraded to
  `UC-` on x86; the module rewrites the leaf PTEs back to write-back itself
  (`src/guest/nvkvm_mmap.c:68-109`). This is x86-specific code.
- On `CONFIG_VMAP_STACK` kernels (Ubuntu's default) a virtio request built on
  the stack produces a bogus page for `virt_to_page()`, so every request buffer
  is `kmalloc`'d (`src/guest/nvkvm_mmap.c:706-715`).
