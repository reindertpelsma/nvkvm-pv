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
(`src/common/nvkvm_abi.h:311-382`). It takes the full version because **two
boundaries fall inside a branch** — 535 splits at the Confidential Computing
channel fields (535.54.03 measures 304, 535.86.05 measures 360) and 550 splits
at the V550 UVM array (550.40.07 measures 1200, 550.40.53 measures 9264). A
major-only lookup mis-keys 535.54.03 (the original 535 GA) and 550.40.07.
`nvkvm_abi_id_for_major()` (`:392`) is kept for callers that hold nothing else,
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
   `src/qemu/nvkvm_isolate.c:1836`), so the stub does no parsing at all — it
   just calls `nvkvm_abi_by_id(job.abi_profile)`
   (`src/stub/nvkvm_stub.c:1202`, `src/stub/nvkvm_stub.c:1488`).

## Consumption sites

| site | file:line | uses |
|---|---|---|
| guest ioctl size table | `src/guest/nvkvm_ioctl.c:85`, `:113`, `:187` | `uvm_map_ext_size`, `uvm_sem_pool_size`, `nvos46_size` |
| guest sanitiser fd offset | `src/guest/nvkvm_ioctl.c:313` | `uvm_map_ext_fd_off` |
| guest alloc-param sizing | `src/guest/nvkvm_main.c:1659`, `:1662`, `:1701`, `:1712` | `nv00de_alloc_size`, `vaspace_alloc_size`, `mem_alloc_size`, `chan_alloc_size` |
| QEMU expected-size table | `src/qemu/nvkvm_dispatch.c:61`, `:88` | UVM sizes |
| QEMU UVM schema floor override | `src/qemu/nvkvm_isolate_handlers.c:1280-1283` | UVM sizes |
| QEMU dispatch fd offset | `src/qemu/nvkvm_dispatch.c:274` | `uvm_map_ext_fd_off` |
| stub UVM fd offset | `src/stub/nvkvm_stub.c:1202` | `uvm_map_ext_fd_off` |
| stub NVOS46 status offset | `src/stub/nvkvm_stub.c:1488` | `nvos46_status_off` |

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
(`src/common/nvkvm_abi.h:28`).

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

## The other two version-keyed tables, and why they are not rows here

The profile table above answers *one* question — the RM/UVM struct sizes and
offsets the forwarder needs. Two other surfaces are just as version-variant and
each has its **own** table, because their boundaries fall in different places
and a profile id cannot express them:

| table | file | keyed on | why not `enum nvkvm_abi_id` |
|---|---|---|---|
| NVKMS command numbering | `src/qemu/nvkvm_nvkms_allowlist.h:199` | major **and** minor | the enum renumbers at **570.207**, i.e. *inside* the `NVKVM_ABI_570` bucket **and** inside a single major |
| nvidia-drm `GET_DEV_INFO` params layout | `src/guest/nvkvm_drm_abi.h` | major **and** minor | the layout changes at **575**, and `NVKVM_ABI_570` deliberately means "570 == 575 layouts" — its 566..579 bucket straddles exactly that boundary |

Borrowing the RM/UVM profile for either would be right by luck at best. Both are
pinned by their own unit suite (`tests/unit/test_nvkms_allowlist.c`,
`tests/unit/test_drm_devinfo.c`).

### nvidia-drm `GET_DEV_INFO` (nr 0x03)

`struct drm_nvidia_get_dev_info_params` is **four** structs, not one, and two of
the three changes were made by **inserting a field in the middle** rather than
appending — so every field after the insertion point moves. `nvkvm_drm.c`
hardcoded the newest of the four.

Measured from vendor source at **all 216 published OGKM tags**, 515.43.04 →
610.57.04 (`kernel-open/nvidia-drm/nvidia-drm-ioctl.h`; renamed to
`kernel-open/nvidia-drm/nv_drm_common_ioctl.h` at 590, contents unchanged). Each
boundary below is adjacent-tag exact — the two named tags are consecutive
releases whose headers disagree:

| first tag | last tag | size | change |
|---|---|---|---|
| 515.43.04 | 535.309.01 | 20 B | the original: `gpu_id`, `primary_index`, `generic_page_kind`, `page_kind_generation`, `sector_layout` |
| 545.23.06 | 545.23.08 | 28 B | `supports_sync_fd`, `supports_semsurf` **appended** |
| 545.29.02 | 570.211.01 | 32 B | `supports_alloc` **inserted at word 2** |
| 575.51.02 | 610.57.04 | 36 B | `mig_device` **inserted at word 1** |

Two notes on the boundaries:

- **575 is a clean major boundary.** All 25 published 570 tags are 32 bytes; the
  first 575 tag (575.51.02) is already 36. No 571..574 branch was ever
  published, so nothing sits in the gap.
- **545 splits mid-branch**, at 545.23.08 → 545.29.02 — same vendor habit that
  put the NVKMS split inside 570. A major-only key mis-reads the two 545.23.x
  releases, which is why the selector takes major *and* minor.
- **610 is byte-identical to 580** — checked against the vendor tree
  (`610.43.02`, `610.57.04`), not assumed. Assuming was how the original bug
  survived.
- Anything above **610.57.04** is extrapolation. Read the new branch's header
  and add a row.

**How the mistake presented, in both directions.** The handler writes two
fields, `primary_index` (the guest's own DRM card minor) and `supports_sync_fd`
(forced to 0), and a wrong layout puts both on their neighbours:

- On a **570** host with the 36-byte struct — measured, RTX 4090 / 570.133.20,
  SteamOS guest — `p->primary_index = dev->primary->index` landed on
  **`supports_alloc`** and `p->supports_sync_fd = 0` cleared
  **`supports_semsurf`**. The guest got `supports_alloc = 0`, so
  `libnvidia-allocator`'s GBM backend refused the device, Mesa fell back to
  llvmpipe, the compositor never got an NVIDIA EGL display and nothing was ever
  scanned out — while `supports_sync_fd` stayed advertised as 1, which is the
  exact eglSwapBuffers hang that line exists to prevent. Reading the reply as 8
  words instead of 9 gives `supports_alloc = 1` and `generic_page_kind = 6` (the
  correct generic kind for Turing+); as 9 it gives 6 and 2, neither a legal
  value.
- Pinning the **32-byte** layout instead (which an earlier fix did) reintroduces
  the same llvmpipe fallback on 575/580/590/595/610, where the guest's
  `supports_alloc` read lands on `primary_index`.

There is no single correct layout. Hence the table.

**Why not derive the size from `_IOC_SIZE(cmd)`.** Tempting, because DRM sizes
its bounce buffer as `max(caller _IOC_SIZE, registered size)` and the four
layouts happen to have four distinct sizes today. Three reasons it is the wrong
key: a `drm_ioctl_desc::func` never sees `cmd` (it takes `dev, data, file`), so
it would need our own `.unlocked_ioctl` interposed just to recover a value the
module already holds; size is a *proxy* for layout, and the failure mode this
whole table exists to stop is a field moving — a same-size reorder, exactly what
the NVKMS enum did at 570.207, would pass a size-keyed decoder silently; and
`_IOC_SIZE` reports what the **guest's** userspace expects, while the offsets
the handler writes at must match what the **host** driver wrote. The good half
of the idea is kept: the handler fills only the fields the host version actually
has (offset `-1` means "absent in that release"), rather than carrying four
parallel struct definitions.

### The sweep: is `GET_DEV_INFO` the only one?

Every other `drm_nvidia_*_params` struct `nvkvm_drm.c` defines was swept against
the same 216 tags. **`GET_DEV_INFO` is the only one that moves a field**, and
the only one where nvkvm's compiled-in definition is wrong on a supported host.
One other struct changed at all, and it changed benignly:

| struct | nvkvm | vendor history | verdict |
|---|---|---|---|
| `semsurf_fence_ctx_create` | 32 B | 32 B at every tag since it appeared (545.23.06) | match |
| `semsurf_fence_create` | 24 B | 24 B since 545.23.06 | match |
| `get_drm_file_unique_id` | 8 B | 8 B since 555.42.02 | match |
| `gem_identify_object` | 8 B | 8 B since 515.43.04; the `drm_nvidia_gem_object_type` values (0/1/2/0x7fffffff) never changed either | match |
| `gem_export_nvkms_memory` | 24 B | 24 B since 515.43.04 | match |
| `gem_import_nvkms_memory` | 32 B | 32 B since 515.43.04 | match |
| `gem_alloc_nvkms_memory` | 24 B | **16 B** at 515.43.04..515.105.01; **24 B** from 520.56.06 on | mismatch on 515/520 only — see below |

**The `gem_alloc_nvkms_memory` finding, stated honestly.** `flags` was
**appended** at 520.56.06 (515: `handle@0, block_linear@4, compressible@5,
__pad@6, memory_size@8` = 16 B). Because it is an append and not an insertion,
nothing shifts: `handle@0` and `memory_size@8` — the only fields
`nvkvm_drm_fwd_gem_alloc_nvkms_memory()` reads or writes — are at the same
offsets in both. The consequence of nvkvm's 24-byte definition on a 515/520 host
is that eight extra bytes are forwarded and returned untouched, not that a field
lands on its neighbour. That plus the fact that no 515/520 driver builds against
kernel 6.8 (`NVKVM_ABI_515` and `NVKVM_ABI_525` are the two profile rows that
have never been booted) is why this is recorded rather than fixed. If a 515 host
ever *is* brought up with graphics, this is the row to key.

The `DRM_NVIDIA_*` command numbers were swept too: nothing was ever renumbered
across 515..610 — the only changes are additions (530 added the prime-fence and
permissions verbs, 545 the semsurf ones, 555 `GET_DRM_FILE_UNIQUE_ID`, 595 the
ROI ones) and the removal of the pre-530 `FENCE_CONTEXT_CREATE` /
`GEM_FENCE_ATTACH` pair at 0x05/0x06, whose numbers were then reused by
`PRIME_FENCE_CONTEXT_CREATE` / `GEM_PRIME_FENCE_ATTACH`; nvkvm wires neither.
The `drm_nvidia_gem_object_type` enum values (0/1/2/0x7fffffff) are unchanged at
every tag. `tests/unit/test_drm_devinfo.c` §5 pins that result against the
production definitions, extracted from `nvkvm_drm.c` at build time so the test
cannot drift from the code it pins.

## What is actually booted

**Six of the eight rows** have been exercised end to end in this repository:
`535` (535.309.01) and `570` (575.51.03) earlier, then `545` (545.23.08), `550`
(550.54.14), `580` (at both ends of its range, 580.95.05 and 595.84) and `610`
(610.43.02). `515` and `525` remain unbooted — the drivers that select them do
not build against kernel 6.8. See
[`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md) and
[`docs/reference/supported-drivers.md`](supported-drivers.md).
