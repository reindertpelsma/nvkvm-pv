# ABI profiles

NVIDIA guarantees no ioctl ABI stability across driver releases. The parameter
structs that `libcuda` passes to `/dev/nvidiactl` and `/dev/nvidia-uvm` are
private kernel/userspace contract, versioned only by the fact that the driver
and its userspace ship together. A forwarder sits in the middle of that contract
and therefore has to know, for each host driver version, exactly how big each
struct is and where its fields live.

`nvkvm` handles this with a **version-keyed profile table**: a small set of
struct sizes and field offsets, selected at runtime from the host driver's
version, consulted by every site whose layout is version-variant.

## The table

`src/common/nvkvm_abi.h:113-235`. Eight rows, one per distinct layout measured
across every published OGKM branch (515 → 610):

| id | covers | selected when |
|---|---|---|
| `NVKVM_ABI_515` | as 525, but `NV00DE` class absent from the tree | major ≤ 520 |
| `NVKVM_ABI_525` | pre-CC channel (304 B), pre-V545 mem/nv00de | major ≤ 530, **plus** 535.43.0x (`< .08`) and 535.54.x |
| `NVKVM_ABI_535` | CC channel (360 B), pre-V545 mem/nv00de | 535.43.08+ and 535.86+ |
| `NVKVM_ABI_545` | V545 mem/nv00de, still pre-V550 UVM (1200 B) | major 545, **plus** 550.40.0x (`< .53`) |
| `NVKVM_ABI_550` | V550 UVM (9264 B), V545 mem/nv00de, base channel | 550.40.53 … 565 |
| `NVKVM_ABI_570` | == 575 layouts: V550 UVM, V570 channel, pre-580 | 566 ≤ major ≤ 579 |
| `NVKVM_ABI_580` | V580 VASPACE + V580 NVOS46 (each +8 bytes) | 580 ≤ major ≤ 595 |
| `NVKVM_ABI_610` | V610 channel (376 B, `+hHandleVASpace`) | major ≥ 610 |

Selection is `nvkvm_abi_id_for_version(major, minor, patch)`
(`src/common/nvkvm_abi.h:311-368`). It takes the full version because **two
boundaries fall inside a branch** — 535 splits at the Confidential Computing
channel fields (535.54.03 measures 304, 535.86.05 measures 360) and 550 splits
at the V550 UVM array (550.40.07 measures 1200, 550.40.53 measures 9264). A
major-only lookup mis-keys 535.54.03 (the original 535 GA) and 550.40.07.
`nvkvm_abi_id_for_major()` (`:370`) is kept for callers that hold nothing else,
and is documented as lossy for exactly those two cases.

Every range above was produced by compiling probes at named tags with
`tools/abi_derive.sh`; `tests/abi_parity/abi_profile_test.go` replays the whole
measured matrix through the selector. Run it with `-count=1` — Go's build cache
tracks the cgo-included header, but the *test-result* cache can replay a stale
pass after a header edit.

Nine values per row (`struct nvkvm_abi_profile`, `src/common/nvkvm_abi.h:85-105`):

| field | what it pins |
|---|---|
| `uvm_map_ext_size` | `UVM_MAP_EXTERNAL_ALLOCATION` params size |
| `uvm_map_ext_fd_off` | byte offset of `rmCtrlFd` inside that struct |
| `uvm_sem_pool_size` | `UVM_ALLOC_SEMAPHORE_POOL` params size |
| `chan_alloc_size` | `{TURING,AMPERE,HOPPER}_CHANNEL_GPFIFO_A` alloc params |
| `vaspace_alloc_size` | `FERMI_VASPACE_A` alloc params |
| `mem_alloc_size` | `NV50_MEMORY_VIRTUAL` / `LOCAL_USER` / `SYSTEM` alloc params |
| `nv00de_alloc_size` | `RM_USER_SHARED_DATA` alloc params |
| `nvos46_size` | total size of `NVOS46` (`NV_ESC_RM_MAP_MEMORY_DMA`, NR `0x57`) |
| `nvos46_status_off` | offset of the status `u32` inside `NVOS46` |

`nv00de_alloc_size` is `0` in the `NVKVM_ABI_515` row alone. That records
*absent*, not a measured size of zero: `NV00DE` / `RM_USER_SHARED_DATA` does not
appear anywhere in the 515 or 520 source trees, so the class cannot be allocated
and there is no size to forward.

The header is shared by all three components — guest kernel module, QEMU device,
freestanding stub — so it uses only plain ints and `static inline`.

## How a profile reaches all three components

The profile has to be identical on the guest side (which sizes the copy-in), the
QEMU side (which validates the size) and the stub side (which indexes fields
before the real ioctl). Two independent mechanisms converge:

1. **Independent parse.** Both QEMU and the guest read the host driver version
   string from `NV_ESC_CHECK_VERSION_STR` and run the same
   `nvkvm_abi_parse_version()` / `nvkvm_abi_id_for_version()` (via
   `nvkvm_abi_for_version()`). QEMU probes it at
   device realize (`src/qemu/virtio_nvgpu.c:1152-1176`) and publishes it in the
   shared-memory control block (`src/qemu/virtio_nvgpu.c:1192-1199`,
   `struct nvkvm_shm_ctrl.driver_version`).
2. **Explicit stamp.** QEMU additionally writes the profile id into every
   `ISOLATE_CMD_IOCTL` header (`abi_profile`,
   `src/common/nvkvm_isolate_proto.h:107-109`; set at
   `src/qemu/nvkvm_isolate.c:1820`), so the stub does no parsing at all — it
   just calls `nvkvm_abi_by_id(job.abi_profile)`
   (`src/stub/nvkvm_stub.c:1114`, `src/stub/nvkvm_stub.c:1400`).

## Consumption sites

| site | file:line | uses |
|---|---|---|
| guest ioctl size table | `src/guest/nvkvm_ioctl.c:85`, `:113`, `:187` | `uvm_map_ext_size`, `uvm_sem_pool_size`, `nvos46_size` |
| guest sanitiser fd offset | `src/guest/nvkvm_ioctl.c:313` | `uvm_map_ext_fd_off` |
| guest alloc-param sizing | `src/guest/nvkvm_main.c:1659`, `:1662`, `:1701`, `:1712` | `nv00de_alloc_size`, `vaspace_alloc_size`, `mem_alloc_size`, `chan_alloc_size` |
| QEMU expected-size table | `src/qemu/nvkvm_dispatch.c:61`, `:88` | UVM sizes |
| QEMU UVM schema floor override | `src/qemu/nvkvm_isolate_handlers.c:1036-1039` | UVM sizes |
| QEMU dispatch fd offset | `src/qemu/nvkvm_dispatch.c:274` | `uvm_map_ext_fd_off` |
| stub UVM fd offset | `src/stub/nvkvm_stub.c:1114` | `uvm_map_ext_fd_off` |
| stub NVOS46 status offset | `src/stub/nvkvm_stub.c:1400` | `nvos46_status_off` |

## Values are measured, never derived

`tools/abi_derive.sh` compiles a probe program against NVIDIA
open-gpu-kernel-modules (OGKM) at each version tag and prints `sizeof` /
`offsetof` for exactly the nine fields:

```bash
tools/abi_derive.sh                       # full matrix + distinct-layout summary
tools/abi_derive.sh --reference-check     # just the five long-cited rows
tools/abi_derive.sh --tags "610.57.04" --format csv
tools/abi_derive.sh --jobs 4 --work /var/tmp/ogkm --keep
```

It blobless-sparse-clones each tag (only the four header trees the probes need,
~30 MB per tag instead of ~1 GB), compiles one probe **per field** against
`kernel-open/nvidia-uvm`, `kernel-open/common/inc`,
`src/common/sdk/nvidia/inc` and `src/common/inc`, and runs each one.

One translation unit per field is deliberate: a struct that does not exist at a
tag then costs one cell, printed literally as `MISSING`, instead of killing the
whole row and tempting someone to fill it in from a neighbouring branch. The
compiler error is kept under the work dir. A value suffixed `*` was measured
through a documented rename (pre-525 spells the channel struct
`NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS` in `nvos.h`) — still a measurement, not
a substitution.

The default tag set is every OGKM branch 515 → 610, with an early **and** a late
tag inside each branch, because two layout boundaries turned out to fall inside a
branch rather than between branches. The script finishes by printing the
distinct-layout summary: consecutive tags sharing a 9-tuple need one profile
between them, and every change in the tuple is a real, measured boundary.

The script's own header explains why it exists:

> Those numbers were originally DERIVED by arithmetic ("V550 grew the array
> +9180 bytes, so 535 must be 9264-9180 = 84") rather than measured. Three of
> the five 535-row values were wrong -- `uvm_map_ext_size` is really 1200, not
> 84 -- and the error is SILENT: a wrong size does not fail to compile, it
> forwards a truncated struct and the kernel reads past the buffer.
>
> So: never hand-derive a row. Run this and paste what it prints.
>
> — `tools/abi_derive.sh:6-12`

That failure mode is the whole argument for the table. A wrong size is not a
compile error and not a runtime error either: the guest copies fewer bytes than
the struct really has, forwards them, and the host kernel's `copy_from_user`
reads whatever follows in the stub's buffer. The offset variant is just as
quiet — `src/guest/nvkvm_ioctl.c:301-312` records what a wrong
`uvm_map_ext_fd_off` actually looked like from the outside:

> `struct uvm_map_external_allocation_params` hardcodes V550, so dereferencing
> `p->rm_ctrl_fd` reads offset 9248 on EVERY driver. On a 535 host libcuda's
> struct is only 1200 bytes, so that read lands 8 KiB past the real field, the
> garbage fails `guest_fd_to_handle_id()`, and the ioctl returns `-EBADF` from
> inside the guest — nothing ever reaches QEMU, so the QEMU debug log shows no
> error at all. Observed on GTX 1660 SUPER / 535.309.01 as `cuCtxCreate -> 999`.

`tests/abi_parity` asserts the compiled-in table against measured values
(`src/common/nvkvm_abi.h:23`).

## The default-row hazard

`nvkvm_abi_by_id()` (`src/common/nvkvm_abi.h:279-303`) looks its fallback up
*by id* rather than by array index. It used to be `&nvkvm_abi_profiles[1]` with
the comment "570/575 default" — but index 1 is the 550 row. The header keeps the
post-mortem:

> On a 570/575 host the old fallback happened to be harmless: the only fields
> the stub reads through this path (`uvm_map_ext_fd_off`, `nvos46_status_off`)
> are identical in the 550 and 570 rows. It is NOT harmless elsewhere — on a
> 535 host an unstamped job would have taken `uvm_map_ext_fd_off = 9248`
> instead of the measured 1184, i.e. an embedded-fd fixup ~8 KiB past the real
> field.
>
> — `src/common/nvkvm_abi.h:281-291`

## Adding a row

See [`docs/howto/add-a-driver-version.md`](../howto/add-a-driver-version.md).

## What is actually booted

Two of the eight rows have been exercised end to end in this repository. See
[`docs/reference/supported-drivers.md`](supported-drivers.md).
