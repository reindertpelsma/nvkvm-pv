# Correctness and known issues

What is known to be wrong, how far each one has been traced, and how to
reproduce it yourself. Every claim here was measured by running the **same
binary** on the host and in the guest; a difference is the finding, agreement
means the behaviour belongs to the GPU or the driver rather than to nvkvm.

**Silent wrong results in repeated map/unmap after allocation churn.** An OpenCL
program that repeatedly maps and unmaps a pinned buffer, after other buffers
have been allocated and freed, reads zeros where the kernel's output should be —
no error, no crash, just wrong data. Geekbench 7 `--gpu` fails validation on 11
workloads in the guest while the identical binary on the same GPU and driver is
clean on the host
([guest](https://browser.geekbench.com/v7/gpu/79890) vs
[host](https://browser.geekbench.com/v7/gpu/79862)). Reproducers and the full
bisection are in [`tests/repro/`](tests/repro/). `validate.sh` does not cover
this: its checks pass on the same guest, so **28/28 is not evidence that a given
workload computes correctly**. Verify your own results against a host run.

**OpenCL is off by default** for that reason — staging it requires
`NVKVM_STAGE_OPENCL=1`. Without it, OpenCL programs fail loudly with "unknown
OpenCL platform" rather than returning wrong answers quietly.

**A driver-managed read-only page is mishandled.** Where the NVIDIA driver hands
back a page with `VM_WRITE` cleared, nvkvm currently substitutes anonymous
memory. Leaving the driver's mapping in place instead causes an unrecoverable
`EFAULT` that kills the guest, so both current behaviours are wrong; the fix is
a `KVM_MEM_READONLY` memslot for those pages, so reads are served and writes
become resumable MMIO exits. This is the leading suspect for the corruption
above, though removing the substitution does not by itself fix it.

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
