# Nested nvkvm: an nvkvm guest hosting an nvkvm guest

**Measured 2026-08-30**, host `claude` (Ubuntu 26.04, AMD, RTX 4070, driver 595.84
open module, `kvm_amd.nested=1`), tree at `main` `e67cf2d` plus the two fixes
described below.

- **L0** — bare metal host, real NVIDIA driver.
- **L1** — `ghcr.io`-style container image built from this tree, Ubuntu 24.04
  cloud guest, 10 GB / 8 vCPU. Its `/dev/nvidia*` are nvkvm's synthesized nodes.
- **L2** — a second nvkvm guest, booted by `scripts/run_test_vm.sh` **inside L1**,
  4 GB / 2 vCPU, using a copy of L1's `/opt/qemu-nvkvm` and L1's own
  `host-libs-595.84` bundle.

Every L2 number below has an **L1 control taken on the same binary, the same
boot and the same GPU**. That control is what makes the deltas meaningful; the
rule in `CLAUDE.md` §2 exists because it has caught six retractions.

## Result

| | L1 (control) | L2 (nested) |
|---|---|---|
| `main` e67cf2d | **30 P / 0 F / 0 S** | 22 P / **1 F** / 7 S |
| + fix 1 (UVM 33) | 30 P / 0 F / 0 S | 22 P / 1 F / 7 S *(failure moves later)* |
| + fix 2 (UVM 27) | **30 P / 0 F / 0 S** | **26 P / 4 F / 0 S** |

Nesting works far further than expected. L2 boots, loads the guest module,
presents all five device nodes, and passes `nvidia-smi`, the full CUDA
bring-up, **Vulkan compute dispatch (verified on-GPU)**, EGL and OpenGL
(`GL_RENDERER='NVIDIA GeForce RTX 4070/PCIe/SSE2'`). The remaining four
failures are one bug, described at the bottom.

## What broke first, and why it was invisible

`cuCtxCreate_v2` returned **999 (CUDA_ERROR_UNKNOWN)** at L2 and 0 at L1 —
deterministically, in both orders, on repeat. `cuCtxCreate` **v1** succeeded at
L2 in the same process, immediately after v2 failed, which is what said the
fault was in a specific call sequence rather than in the GPU path.

`strace` found the first divergence:

```
L1: ioctl(9, _IOC(_IOC_NONE, 0, 0x21, 0), …) = 0
L2: ioctl(9, _IOC(_IOC_NONE, 0, 0x21, 0), …) = -1 EBADF (Bad file descriptor)
```

`0x21` = 33 = `UVM_MAP_EXTERNAL_ALLOCATION`. libcuda allocates an
`NV01_MEMORY_LOCAL_USER` (`hClass=0x0040`), tries to map it into UVM, is
refused, **frees the object it just allocated and gives up** — with no error
anywhere: no `DENY` line, no non-zero `nvstatus`, no failing RM ioctl. An
`LD_PRELOAD` dump showed the alloc's IN and OUT param blocks were byte-identical
at L1 and L2, which ruled the RM path out.

Instrumenting the **L1 guest module** (a `pr_warn` in `guest_fd_to_handle_id()`
and at the `nvkvm_ioctl()` exit) named it exactly:

```
nvkvmdiag: uvm33 comm=worker pid=2302 off=9248 size=9264 fd=2
nvkvmdiag: fd2hid fd=2 NOT OURS comm=worker pid=2302 name=nvkvm-l2-qemu.log
```

`comm=worker` is a **QEMU pool worker**, not the isolate stub. L1's QEMU
forwarded the embedded `rmCtrlFd` **untranslated**: the value in the field was
still the inner guest's `handle_id` (2), and QEMU handed the driver *fd 2* —
its own stderr, which the log line helpfully names.

## Root cause

`nvkvm_uvm_schema[]` (`src/qemu/nvkvm_isolate_handlers.c`) declares which UVM
commands carry an embedded frontend fd via `fd_off[]`. That set must equal the
set the **guest sanitizer** rewrites into a `handle_id`
(`nvkvm_sanitize_ioctl_params`, `src/guest/nvkvm_ioctl.c`) and the set the
**stub** translates back (`src/stub/nvkvm_stub.c`). It did not:

| cmd | guest rewrites | stub translates | QEMU `fd_off` |
|---|---|---|---|
| 25 `REGISTER_GPU_VASPACE` | yes | yes | `16` |
| 27 `REGISTER_CHANNEL` | yes | yes | **`0xffff` — missing** |
| 33 `MAP_EXTERNAL_ALLOCATION` | yes | yes | **`0xffff` — missing** |
| 37 `REGISTER_GPU` | yes | yes | `24` |
| 75 `MM_INITIALIZE` | yes | yes | `0` |

Commit `6f2e37d` (2026-08-30) fixed the same class of bug for 75 and its message
says *"There are THREE such rows"*. There are **five**. 33 is missed twice over:
its offset is version-variant (1184 pre-V550, 9248 from 550.54.14), so the
static `fd_off[]` cannot express it at all — it has to come from the ABI profile
at the call site, the way `min_size` already does.

**Why bare metal never noticed.** On L0 the same untranslated integer goes to
the *real* NVIDIA UVM driver, which does not dereference `rmCtrlFd` on this
path, so L1 read 30/30 with the bug present the whole time. Nested, the "driver"
is another nvkvm guest module — which *does* validate the field, correctly, and
returns `-EBADF`. **Nesting did not introduce this bug; it made an existing one
observable.** That is the useful thing nesting bought.

## The fixes

Both in `src/qemu/nvkvm_isolate_handlers.c`:

1. row 27 gains `fd_off = { 16, 0xffff }`, and `nvkvm_uvm_embedded_fd_dev()`
   gains `case 27 -> NVKVM_DEV_CTL`;
2. row 33 keeps `0xffff` (it cannot be static) and the translation loop takes
   its offset from `prof->uvm_map_ext_fd_off`, mirroring the `min_size`
   override eight lines above; `nvkvm_uvm_embedded_fd_dev()` gains
   `case 33 -> NVKVM_DEV_CTL`.

The security property of the gate is unchanged and in fact extended: two more
commands now go through the type/device/generation check instead of past it.
Gates run: `tests/unit/run_tests.sh` (17 suites), `tests/qemu_syntax_check.sh`
under cc and gcc-14, `abi_parity` `go test -count=1`, and the **full
`tests/validate.sh` at L1: 30/30 before and after**.

## Still open at L2: device memory reads back as zeros, silently

With both fixes, L2's four remaining failures are one bug. Plain `cuMemAlloc`
device memory is written and read back as **zeros**, with every ioctl returning
0 and no diagnostic anywhere:

```
L2:  size=      16  htod=0 dtoh=0 roundtrip_bad=16/16        memset=0 readback_bad=16
     size= 8388608  htod=0 dtoh=0 roundtrip_bad=8355840/8388608 memset=0 readback_bad=8388608
L1:  size=      16  htod=0 dtoh=0 roundtrip_bad=0/16         memset=0 readback_bad=0
     size= 8388608  htod=0 dtoh=0 roundtrip_bad=0/8388608    memset=0 readback_bad=0
```

Size-independent, from 16 B to 8 MiB. Note what still **passes** at L2:
`cuda_managed_coherence` — 1,048,576 elements written through a managed
pointer, `vec_add<<<4096,256>>>` run on them, every element verified, three
CPU↔GPU migration cycles. So the GPU executes correct work on UVM-managed
memory; it is the plain RM VIDMEM + copy path that returns zeros. Vulkan
compute (its own buffers, its own dispatch) also verifies byte-exact.

This is the `correctness.md` failure class — a silent wrong answer that a
passing suite would not have caught. Not root-caused here. The next step is to
compare the `cuMemcpyHtoD` staging path's GPA mapping at L1 and L2; the two GPA
window blocks are nested (L1's QEMU logs `block base 0xdfdbc0000000 size 145 GiB
[shm …, mmap … +16 GiB, sparse … +128 GiB]` inside a guest whose own window L0
already placed), which is the obvious first place to look and has not yet been
measured.

## Two things that are NOT findings

- **The DENY sets are identical at both boundaries.** Whole-lifetime, at L1 and
  at L2: `0x00730102` (`NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`), `0x2080019f`
  (`NV2080_CTRL_CMD_GPU_GET_SKYLINE_INFO`), `0x2080220b`
  (`NV2080_CTRL_CMD_RC_ENABLE_WATCHDOG`) — one each, all three already known and
  benign. **No control was denied at L2 that is not also denied at L1.** The
  double-allowlist traversal cost nothing.
- **A guest oops during this work was self-inflicted.** `rmmod nvkvm_guest`
  after a CUDA process had used the module oopsed L2 in
  `__kmalloc_node_track_caller` (`[last unloaded: nvkvm_guest(OE)]`). That was
  the instrumentation cycle, not the product path; the same rmmod immediately
  after boot is clean. It is not the teardown deadlock of
  `docs/investigations/guest-deadlock-gpu-under-load/`, which did **not**
  reproduce here.
