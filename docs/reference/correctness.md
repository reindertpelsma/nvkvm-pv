# Correctness and known issues

What is known to be wrong, how far each one has been traced, and how to
reproduce it yourself. Every claim here was measured by running the **same
binary** on the host and in the guest; a difference is the finding, agreement
means the behaviour belongs to the GPU or the driver rather than to nvkvm.

**Silent wrong results after a mapped buffer is freed.** The guest CPU and the
GPU stop seeing the same memory: the CPU reads back its own writes correctly,
while the GPU reads zeros, so a kernel computes from an all-zero input and no
error is reported anywhere. Geekbench 7 `--gpu` fails validation on 11 workloads
in the guest while the identical binary on the same GPU and driver is clean on
the host ([guest](https://browser.geekbench.com/v7/gpu/79890) vs
[host](https://browser.geekbench.com/v7/gpu/79862)). `validate.sh` does not
cover it — 28/28 passes on a guest that computes this wrong — so **verify your
own results against a host run**.

*Root cause, bisected 2026-08-19 on an RTX 3060:* the trigger is an RM memory
object being **freed while nvkvm still holds its window mapping**. Releasing a
buffer produces no unmap: every window extent in a run is torn down at process
exit, never when the object is freed. The driver then recycles that device
memory for the next allocation, and the guest keeps writing into the stale
extent — which is why the CPU sees its own writes and the GPU does not.

`tests/repro/opencl_input_visibility.c` isolates it to one variable: churn
buffers that are mapped, written and *released* corrupt the next buffer
(`./clvis 3 20 1 1`); the same buffers *not* released do not (`./clvis 3 20 1 3`).

Eliminated by experiment, so that the next attempt does not re-tread them:

- **not** the read-only page substitution below — removing it entirely leaves
  the corruption unchanged
- **not** GPA extent recycling — no window GPA is reused during a failing run
- **not** the ioctl/alloc allowlists — no gate denies anything during the run
- **not** pinned vs device memory (`ALLOC_HOST_PTR` and plain device buffers
  fail identically), and **not** the buffer's `CL_MEM_READ_ONLY`/`READ_WRITE` flags

*Fix direction:* window mappings carry no RM memory handle today, so a free
cannot find them. The `fd` recorded by `RM_MAP_MEMORY` bridges the two; teaching
the mapping table that association would let `NV_ESC_RM_FREE` tear the extent
down, so the next mapping is established against the new object.

**OpenCL is off by default** for that reason — staging it requires
`NVKVM_STAGE_OPENCL=1`. Without it, OpenCL programs fail loudly with "unknown
OpenCL platform" rather than returning wrong answers quietly.

**A driver-managed read-only page is mishandled** *(fixed 2026-08-19 for the
common case).* In-window device mappings used to be forced to `PROT_READ|
PROT_WRITE` regardless of what the guest asked for. When the guest requested a
read-only mapping the driver honoured it with a read-only VMA, the forced
writable `mprotect` probe then failed, and the page was replaced with anonymous
zeroes — so whatever the driver publishes there never reached the guest. nvkvm
now honours the requested protection and only probes for writability when the
guest actually asked to write.

What remains is the narrower conflict: the guest requests **write** and the
driver returns a read-only VMA anyway. That still falls back to anonymous
memory, because leaving the driver's mapping in place causes an unrecoverable
`EFAULT` that kills the guest. The fix for that case is a `KVM_MEM_READONLY`
sub-slot, so reads are served and writes become resumable MMIO exits.

**Vulkan compute fails on Hopper** (`vk_compute_dispatch`) — see the note under
[Tested platforms](#tested-platforms).

## Vulkan compute on Hopper

On the H100 every CUDA and bring-up check passes (`sm_90`, PTX JIT, kernel
launch, matmul, byte-exact 8 MiB round trips) and OpenGL renders through the
forwarder, but `vk_compute_dispatch` fails: `vkCreateDevice` returns `-4`
(`VK_ERROR_DEVICE_LOST`) in the guest while the *same binary* returns `0` on the
host with the same driver. Vulkan enumeration works and both sides see the same
5 queue families — only device creation differs. Minimal reproducer:
[`tests/repro/vk_create_device.c`](tests/repro/vk_create_device.c).

Traced, with two hypotheses eliminated by experiment:

* **Not the allowlist.** The QEMU log showed the default-deny gate rejecting
  `alloc class 0x0000a083` (`NVA083_GRID_DISPLAYLESS`) and `ctrl cmd 0x20803401`
  (`NV2080_CTRL_CMD_ECC_GET_VOLATILE_COUNTS`, a read-only ECC query that only
  appears on ECC-capable datacenter parts). Temporarily allowing `0xa083`
  changed nothing — `vkCreateDevice` still returned `-4` — so it was never the
  cause, and allowing it would have widened a security boundary for no gain.
  The entry was reverted.
* **Not class discovery either.** The failure is a channel/compute mismatch: the
  userspace driver creates its channel as `AMPERE_CHANNEL_GPFIFO_A` (`0xc56f`)
  on this Hopper part, then allocating `HOPPER_COMPUTE_A` (`0xcbc0`) under that
  parent fails with `NV_ERR_OBJECT_NOT_FOUND` (`0x57`). The obvious suspect was
  a truncated or filtered class list, but instrumenting
  `NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2` shows the guest receives it intact —
  48 classes in a 100-entry array, including both `HOPPER_CHANNEL_GPFIFO_A`
  (`0xc86f`) and `HOPPER_COMPUTE_A` (`0xcbc0`). The driver has the Hopper
  channel class available and picks the Ampere one anyway, so the remaining
  question is why, not what it was told. Run with `NVKVM_DEBUG=1` to see the
  `CLASSLIST_V2` line.

**This does not affect CUDA.** All 19 CUDA checks pass on the H100 over that
same Ampere channel, with byte-exact verification, and the class list reaching
the guest is complete — nvkvm is not withholding Hopper classes from the
driver.

\* Both read 27/28 until the NVKMS allowlist was fixed on 2026-08-17, and the
595.84 row has no 28/28 measurement — its box was re-provisioned without
`libnvidia-ptxjitcompiler`. Full detail in
[`tests/BOOT_MATRIX.md`](tests/BOOT_MATRIX.md).

NVIDIA guarantees no ioctl ABI stability across driver releases, so `nvkvm` keys
struct layouts off the host driver version. **Eight profiles** cover every
open-kernel-modules release from 515 to 610, measured by compiling `sizeof` /
`offsetof` probes against 61 upstream tags — and keyed on the full
`major.minor.patch`, because two widely deployed branches change layout *inside*
the branch. **Six of the eight have been booted**; no profile's values proved
wrong in practice. See [ABI profiles](docs/reference/abi-profiles.md) and
[supported drivers](docs/reference/supported-drivers.md).

Verify any host yourself, inside a guest:

```bash
sudo bash /mnt/nvkvm/tests/validate.sh
```

28 checks — device nodes, the full CUDA ladder, Vulkan compute, offscreen GL.
Needs only a C compiler, and a software-rasteriser fallback is an explicit
**FAIL**, not a pass.

## Reproducing

See [`tests/repro/`](../../tests/repro/) — each program is small, self-validating,
and meant to be run on both sides of the boundary.
