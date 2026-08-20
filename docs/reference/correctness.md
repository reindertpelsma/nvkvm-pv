# Correctness and known issues

What is known to be wrong, how far each one has been traced, and how to
reproduce it yourself. Every claim here was measured by running the **same
binary** on the host and in the guest; a difference is the finding, agreement
means the behaviour belongs to the GPU or the driver rather than to nvkvm.

**Silent wrong results after a mapped buffer was freed** *(fixed 2026-08-19,
commit `ca6d496`).* The guest CPU and the GPU stopped seeing the same memory:
the CPU read back its own writes correctly while the GPU read the *previous*
buffer's contents, so a kernel computed from stale input and no error was
reported anywhere. It was never OpenCL-specific — plain CUDA
(`cuMemHostAlloc` → write → `cuMemcpyHtoD`) reached it too.

*Root cause.* Both CPU-memory migration paths in
[`src/guest/nvkvm_mmap.c`](../../src/guest/nvkvm_mmap.c) remembered a guest
virtual address and treated a later request for the same address as already
done. A virtual address is not a stable name for a buffer. Free a pinned host
buffer and allocate another, and libcuda hands back the same address — at which
point the tracking entry named a mapping belonging to a buffer that no longer
existed, and the isolate still had the *old* buffer's memory mapped there. The
dedup fired, no migration happened, and the GPU read the dead buffer. The guest
meanwhile read its own fresh anonymous pages and saw exactly what it had just
written, which is why the two views disagreed silently.

*Fix.* Stop trusting the address; ask the guest page tables whether the mapping
we recorded is still installed — the leaf PTE still on the memfd's GPA for a
range entry, the address still translating to the pinned page for a per-page
entry — and reap the entries that fail before either dedup runs. A reaped entry
releases its isolate mapping and its memfd, so the next access re-migrates.
Entries created under a different address space are left alone: an fd shared
with another process gives an `mm` in which those addresses mean nothing.

*Measured*, same guest and libraries, modules swapped back to back on an RTX
3050 laptop (Ampere, host driver 580.173.02):

| check | before | after |
|---|---|---|
| `tests/repro/cuda_host_churn.c 3 20` | 1048576/1048576 wrong (`-999.0`) | clean |
| `tests/repro/opencl_input_visibility.c 3 20 1 1` | GPU view 1048576 wrong (`0.0`) | clean |
| `tests/repro/opencl_correctness.c` | FAIL (1 failed) | PASS |
| `tests/validate.sh` | 28/28 | 28/28 |

Confirmed on a second architecture and a second driver branch: on an **RTX 2080
Ti (Turing TU102, host driver 575.51.03)**, the same A/B with the pre-fix module
returns `1048576/1048576 WRONG, got -999.0` and the fixed module is clean, with
`validate.sh` 28/28 on both the ring and virtqueue paths. So this was never
specific to the laptop's 39-bit physical address space, which was the standing
suspicion while it was unsolved.

The independent check is Geekbench 7 `--gpu` (OpenCL), which is what surfaced
the bug at scale in the first place: the guest used to fail validation on
**11 workloads** — Photo Filter, Face Tracking, Super Resolution, Horizon
Detection, Feature Matching, Path Tracer, Particle Physics, Fluid Simulation
among them — while the identical binary on the same GPU was clean on the host
([old guest](https://browser.geekbench.com/v7/gpu/79890) ·
[host](https://browser.geekbench.com/v7/gpu/79862)). The same run on the same
guest with this fix completes **every workload with zero validation failures**,
and lands at **99.9% of the host score** — 48335 guest vs 48395 host, with each
of the eleven workloads between 98.4% and 101.2%
([side by side](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862)).
That parity is the useful part: it says the workloads are not merely returning
*valid* answers now, they are returning them at the speed the bare-metal host
does, so nothing was traded away to fix them.

`validate.sh` passing 28/28 on *both* sides is still the point worth keeping:
the suite never covered this, so a green 28/28 was not evidence of correct
results and still is not. Check your own workload against a host run.

Three earlier suspects were eliminated by experiment and are recorded so nobody
re-treads them: it was not the read-only page substitution (removing it changed
nothing), not GPA extent recycling (no window GPA is reused during a failing
run), and not the ioctl/alloc allowlists (no gate denies anything during the
run). A fourth — re-`gup`ing the address and comparing `struct page` — is part
of the fix but was not sufficient alone: the buffers that corrupt take the
*range* path, whose entries have no `struct page` to compare, and are
identified by their PTE instead.

*Host side, also done:* window mappings now record the frontend handle they
were made from, and closing that handle tears the extent down (`REAP_HANDLE` in
a `NVKVM_DEBUG=1` log) — a freed object's device memory no longer stays mapped
into the guest's GPA space. That was a real hole, though it was not this bug.

**The SPSC ring fast path now applies the allowlist too** *(fixed 2026-08-19).*
Flat RM_CONTROLs ride a shared-memory ring consumed directly by the isolate
stub, which is how they used to reach the host driver without passing QEMU's
default-deny control gate — [U-1](../internal/audit-guest-pointers.md). The gate
now lives in the header next to its table and both host-side components call it;
a refused command is punted to the slow path, which makes the denial. Measured
with `ring_enable=1`: 28/28 and `opencl_correctness` PASS, so nothing legitimate
was relying on the ungated path.

An earlier note here claimed the ring also skipped
`nvkvm_cpu_pages_writeback()`. That was wrong — the `goto forwarded` lands in
front of it. What the ring skips is guest-side pre-forward work.

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

## Two CUDA checks failed on Ada + driver 595.84 — FIXED 2026-08-19

*Root cause: nvkvm refused a legitimate `UVM_FREE`. Fix below.*

First seen on an RTX 4070, host driver **595.84**, current `main`, inside the
shipped container: `tests/validate.sh` was **26/28**: `cuda_kernel_launch` and `cuda_matmul` both fail `setup rc=1`.
Every other check passes, including `cuda_ptx_jit` immediately before them and
the 8 MiB byte-exact round trip.

The same code is 28/28 on 580.173.02 (RTX 3050) and 580.95.05 (GTX 1660
SUPER) the same day, so this is specific to 595.84 (or to that GPU/host).

**The old explanation for this driver row was wrong.** It blamed a box
provisioned without `libnvidia-ptxjitcompiler`. That library is present — in
the host bundle *and* staged in the guest — and the failure happens anyway.

**Root cause.** `UVM_FREE` (UVM cmd 34) names the range to free by its **base
alone** — libcuda sends `length = 0` and the driver looks the range up by start
address. The U-6 ownership check called `uvm_va_sane(base, len)`, which rejects
`length == 0` outright, so nvkvm refused the free with `NV_ERR_INVALID_ADDRESS`.
The range was never freed, the UVM address space stayed wedged, and **every
later CUDA call in that context returned `INVALID_VALUE`** — which is why all
six setup calls failed rather than one, and why it read as a dead context.

The fix (`src/qemu/nvkvm_isolate_handlers.c`) substitutes the length nvkvm
itself recorded for that base instead of trusting the caller's zero. The
security property is unchanged and still fails closed: a base that was never
recorded for this handle yields 0 and is refused exactly as before.
`tests/security/u3_u6_gate_test.c` still passes, `MIGRATE(unowned)` included.

*Measured*, RTX 4070 Ti SUPER on 595.84, same box, QEMU rebuilt between runs:

| | `tests/validate.sh` |
|---|---|
| before | 26/28, 27/28, 27/28 |
| after | **28/28, 28/28, 28/28** |

Then confirmed on the GPU that found it — an **RTX 4070 (AD104), driver 595.84,
kernel 7.0**: 28/28 three times over, and a `LD_PRELOAD` interposer in the guest
shows the path is genuinely exercised rather than merely absent. libcuda issues
**five `UVM_FREE` calls per run, every one with `length=0x0`**, and all five now
succeed where each was previously refused:

```
3x UVM_FREE base=0x7e411ac00000 length=0x0 rc=0 status=0x0
1x UVM_FREE base=0x7e411b400000 length=0x0 rc=0 status=0x0
1x UVM_FREE base=0x7e411b000000 length=0x0 rc=0 status=0x0
```

That matters for how seriously to take this: **595.84 is what a stock Ubuntu
26.04 install selects for an Ada card**, no driver chosen by hand. Anyone
installing the current Ubuntu LTS on a 4070 and running nvkvm would have hit
it.

**Why it hid for so long**, since each of these misled an earlier attempt:

- The guest sees **no failing ioctl** — a refused UVM command comes back as a
  *status*, so the ioctl itself succeeds.
- It is **not reproducible standalone**. A probe replicating the suite's CUDA
  sequence — warmup, the same PTX load, the identical allocations — passes
  every call, because it never frees a UVM range.
- It is **not deterministic**: one or both checks fail depending on whether a
  by-base free happens in that run.
- An `LD_PRELOAD` ioctl interposer in the guest showed nothing, because it
  traced RM controls (`'F' 0x2a`) and this is a **UVM** ioctl on another
  device. The silence was from looking at the wrong surface.
- The old footnote blamed a missing `libnvidia-ptxjitcompiler`. That library is
  staged; the explanation was simply wrong.

Ada + 595.84 is the combination that exposed it — RTX 3080 on the same driver
is 28/28, and the same Ada part on 580.95.05 is 28/28 — but **nothing in the
check is architecture-specific**. It was latent for any guest whose libcuda
takes the by-base free path.

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
595.84 row had no 28/28 measurement.  **That footnote used to blame a box
re-provisioned without `libnvidia-ptxjitcompiler`; that explanation is wrong**
— see below. Full detail in
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
