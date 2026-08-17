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
for the per-check values and for the one failure they surfaced (offscreen
framebuffer objects are unusable on 595 and 610).

Turing is the floor in practice, not by an explicit check: the alloc-class
allowlist (`src/qemu/nvkvm_fe_alloc_allowlist.h:53-142`) carries the Volta,
Turing, Ampere, Ada, Hopper and Blackwell channel/compute/DMA-copy classes, and
the channel-alloc sizing in the guest handles `TURING_CHANNEL_GPFIFO_A`,
`AMPERE_CHANNEL_GPFIFO_A` and `HOPPER_CHANNEL_GPFIFO_A`
(`src/guest/nvkvm_main.c:1709-1713`). A pre-Volta card would need its classes
added to the allowlist.

Multi-GPU hosts are enumerated — QEMU scans `/dev/nvidia0..15`
(`src/qemu/nvkvm_isolate_handlers.c:121-129`) and the guest creates that many
`/dev/nvidiaN` nodes — but the tested configurations above are single-GPU.

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
