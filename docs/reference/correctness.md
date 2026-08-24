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

**Vulkan compute failed on Hopper** (`vk_compute_dispatch`) *(root-caused
2026-08-21; the fix is not on `main` today).* An unsized alloc class made nvkvm
forward a NULL parameter block —
[the driver sweep, the trace and the fix](#vulkan-compute-on-hopper--root-caused-it-was-ours-and-it-was-never-a-driver-bug).

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

## Vulkan compute on Hopper — root-caused; it was ours, and it was never a driver bug

**Status: root-caused, fixed once, and the fix is not on `main` today.**
`HOPPER_USERMODE_A` (`0xc661`) had no entry in the guest module's
alloc-parameter size-by-`hClass` tables, so nvkvm forwarded a **NULL parameter
block** for it and the host RM built a different object than the caller asked
for. On an H100 that cost Vulkan its compute queue.

> **The fix is currently absent from this tree.** It landed in `0492685`
> (merged as `14e7511`) and was removed again by `4fece85`, a single-parent
> commit that reverted several unrelated things it did not conflict with — see
> [What a clean merge can still revert](#what-a-clean-merge-can-still-revert)
> below. `src/guest/nvkvm_main.c` has no `nvkvm_alloc_parms_probe_len()` on
> `main` as of `f9f187f`. The **diagnosis** below is measured and stands; the
> **28/28 rows** were measured on a tree that carried the fix. Restore
> `0492685`'s `src/guest/nvkvm_main.c` hunk before quoting a current Hopper
> result.

### The driver sweep that found it (2026-08-21)

Measured on one H100 PCIe box by swapping only the host NVIDIA driver. The nvkvm
QEMU binary and `nvkvm-guest.ko` were **byte-identical across every row**; the
guest libraries were re-bundled with `scripts/make_host_bundle.sh` and re-staged
with `scripts/stage_guest_libs.sh` after each swap, as they must be. Drivers
installed from NVIDIA's official `.run` with `--kernel-module-type=open`.

| host driver | ABI profile nvkvm selected | HOST `vk_compute_dispatch` | GUEST `validate.sh` (before fix) |
|---|---|---|---|
| 550.54.14  | 550 | **PASS** | measured after the fix only — 28/28 |
| 565.57.01  | 550 | **PASS** | **27/28** — `vk_compute_dispatch` FAIL (not re-run after the fix) |
| 570.124.06 | 570 | **PASS** | **27/28** — `vk_compute_dispatch` FAIL |
| 570.148.08 | 570 | **PASS** | **27/28** — `vk_compute_dispatch` FAIL (not re-run after the fix) |
| 575.57.08  | 570 | **PASS** | **27/28** — `vk_compute_dispatch` FAIL |
| 580.65.06  | 580 | **PASS** | 28/28 |
| 580.126.09 | 580 | **PASS** | 28/28 |

Every profile selection above matches what
[ABI profiles](abi-profiles.md) predicts for that version, on Hopper as
elsewhere: 550.54.14 → 550, 565.57.01 → 550, 570.x and 575.57.08 → 570, 580.x →
580. **535.86.05 could not be booted here at all** — its open kernel module does
not compile against Linux 6.8 (`crypto_tfm_ctx_aligned` and `DRM_UNLOCKED` are
both gone), so the 535 profile still has no Hopper measurement. That is the same
"no longer builds against a modern kernel" wall the 515–530 profiles hit, now
reached one branch higher on a 6.8 host.

**The host column is the whole argument.** Every driver, 570.124.06 included,
runs the probe correctly on bare metal. A driver that works on the host and
fails through nvkvm is an nvkvm bug, by definition — which is why this table was
worth the box it took to measure.

Two earlier claims died here. The write-up that said this was **"a defect in the
570.124.06 driver branch"** was reached by *elimination* — no 570 machine was
available, so "the only remaining variable is the host driver" was treated as
naming the culprit. It names the *trigger*. A bug of ours that only some drivers
expose fits that evidence identically, and that is what it was. And the affected
range was never "the 570 branch": 565 fails and 575 fails too, spanning **two**
different ABI profiles (550 and 570), so the profile table is not the
discriminator either.

### Root cause

The probe's own status codes, from the guest on 575.57.08 with `NVKVM_DEBUG=1`:

```
nvkvm: TRACE ALLOC hParent=0xbeef0004 hNew=0xbeefc360 hClass=0xc661 aps=0 aux=0 nvstatus=0x0
nvkvm: A100DBG FREE hRoot=0xc1d00067 hParent=0xbeef0003 hObject=0xbeee0100    nvstatus=0x0
nvkvm: RM_ALLOC failed: hParent=0xbeee0100 hObjNew=0xbeee90c0 hClass=0xcbc0
       alloc_parms_size=0 aux_size=0 nvstatus=0x57
```

`0x57` is **`NV_ERR_OBJECT_NOT_FOUND`**, confirmed against `nvstatuscodes.h` at
upstream tag `570.124.06` (`0x55` `NOT_READY`, `0x56` `NOT_SUPPORTED`, `0x57`
`OBJECT_NOT_FOUND`). The object not found is the **parent**: the channel
`0xbeee0100` is freed one ioctl earlier and the compute-class allocation then
names it as `hParent`. That free is the userspace driver giving up, not the
fault — so the fault is upstream of it.

Finding the upstream point took a host/guest ioctl diff — the same `LD_PRELOAD`
shim ([`tools/nv_ioctl_trace.c`](../../tools/nv_ioctl_trace.c)) run on **both**
sides, logging every `NV_ESC_RM_ALLOC` / `RM_CONTROL` / `RM_FREE` with its
parameter bytes. The two sequences are **identical for 46 consecutive
allocations** and every control's in/out bytes match, until this one control
answers differently:

```
NV0000_CTRL_CMD_CLIENT_GET_ADDR_SPACE_TYPE (0xd01) on object 0xbeefc360
  HOST   params = 60c3efbe 00000000 02000000   addrSpaceType = 2  VIDMEM
  GUEST  params = 60c3efbe 00000000 03000000   addrSpaceType = 3  REGMEM
```

Object `0xbeefc360` is a **`HOPPER_USERMODE_A` (`0xc661`)** — the usermode
doorbell aperture. It is allocated with `pAllocParms` set and `paramsSize = 0`,
the ordinary "let RM size the struct by class" convention. `0xc661` is defined in
`src/abi/nvgpu.h:54` and is on the alloc-class allowlist
(`src/qemu/nvkvm_fe_alloc_allowlist.h:111`) — but **had no case in either
size-by-`hClass` switch** in `src/guest/nvkvm_main.c`, so:

1. the guest module computed `ap_size = 0` and copied **no** parameter bytes;
2. the stub, which sets `pAllocParms = aux_size > 0 ? aux_buf : NULL`, forwarded
   a **NULL** parameter block;
3. RM allocated the usermode object from all-defaults — no `bBar1Mapping` — so
   its aperture came back **REGMEM** instead of **VIDMEM**;
4. the userspace driver mapped it the way REGMEM requires, concluded the channel
   it had just built was unusable, freed it, and then allocated
   `HOPPER_COMPUTE_A` under the dead handle;
5. `NV_ERR_OBJECT_NOT_FOUND` → `vkCreateDevice` → `VK_ERROR_DEVICE_LOST` → 27/28.

Nothing was ever denied and no forwarded ioctl returned a non-zero status until
the very last one. This is the **fifth** instance of the same silent failure mode
in this codebase — a class that is *reachable* but *unsized*, so zero bytes are
forwarded and the damage surfaces layers away (#84 twice, #99, #101, now this).
The four earlier ones are tabulated in
[Add a driver version → Check the alloc-class size table](../howto/add-a-driver-version.md#7-check-the-alloc-class-size-table).
[`tests/BOOT_MATRIX.md`](../../tests/BOOT_MATRIX.md) predicted it in writing:
*"A guest-side warning when a known-allowed hClass hits the default arm of a
size switch would have turned every one of these into a one-line log message
instead of a bisect."*

### The fix

`src/guest/nvkvm_main.c` no longer forwards a NULL parameter block for a class it
cannot size. For an unsized `hClass` it copies a **page-bounded window** of the
caller's buffer (256 bytes, clipped at the end of the page holding the pointer,
so it can never fault into an unmapped neighbour) and — in the NVOS64 path —
**leaves the caller's `alloc_parms_size` at 0**, because that is precisely what a
native caller passes and what makes RM size the struct by class itself. Guessing
a size there would be the one thing RM does reject. It also emits the warning
`BOOT_MATRIX.md` asked for:

```
nvkvm: RM_ALLOC hClass=0xc661 has no alloc-param size entry; forwarding a 256-byte window
```

The window is deliberately not a new table row: a table entry can be *wrong*, and
being wrong here is silent. Letting RM size its own struct cannot be.

### Verified

| host driver | ABI profile | before | after |
|---|---|---|---|
| 570.124.06 | 570 | 27/28, `vk_compute_dispatch` FAIL | **28/28 PASS** |
| 575.57.08  | 570 | 27/28, `vk_compute_dispatch` FAIL | **28/28 PASS** |
| 550.54.14  | 550 | not measured | **28/28 PASS** |
| 580.126.09 | 580 | 28/28 | **28/28** (no regression) |

The 550.54.14 row is there because it is the high side of the 550 intra-branch
ABI boundary, and this is the only Hopper part the project has had — a
consumer card cannot stand in for it. GA100 needed two fixes that had been
silently wrong on every consumer Ampere die, so cross-architecture substitution
is not a safe assumption for anything architecture-specific.

Host-side unit suite (`tests/unit`): `test_ctrl_gate`, `test_objects`,
`test_handle` all pass. (Read at the time as `test_dispatch` / `test_frontend`;
those two targeted the unreachable dispatch/frontend pair and went with it on
2026-08-24 — DEAD-1. `test_dispatch` is now `test_objects`.)

**Not established:** *why* the 580 userspace was immune. It issues the same
`0xc661` allocation and got the same NULL parameter block, and still worked — so
something in the 580 Vulkan driver tolerates the default usermode object on this
path. That was not chased, because the guest is correct on every driver either
way once the fix is applied. Do not record a reason for it that has not been
measured; that mistake is what produced the first two versions of this section.

**This never affected CUDA.** All 19 CUDA checks passed on the H100 on every
driver in the sweep, before and after the fix.

### What a clean merge can still revert

The fix, the corrected write-up on this page, and the `abi-parity` CI job added
by `5036b0f` were all removed by `4fece85` — a commit whose stated subject is a
DDX investigation handoff and which conflicted with none of them. It has a single
parent, so nothing about it reads as a merge; the losses are visible only by
diffing against the merge that introduced them:

```bash
git diff 14e7511 HEAD -- src/guest/nvkvm_main.c docs/reference/correctness.md
git log --oneline 5036b0f -1 && grep -c abi_parity .github/workflows/ci.yml
```

Three other casualties of the same commit *were* caught and restored — the
trimmed README (`b54b38f`), the `.gitignore` `!go.mod` negation and a
heredoc-escaping fix (`2d0a214`), and the re-tracked `__pycache__` (`a6ef02b`) —
which is the tell that the revert was mechanical rather than intended. After
merging any long-lived branch here, check the things that branch could not have
known about; a clean auto-merge is not evidence that nothing was lost.

### The old "channel/compute class mismatch" diagnosis, for the record

The failure was once written up as a *class mismatch* — an
`AMPERE_CHANNEL_GPFIFO_A` (`0xc56f`) channel being an illegitimate parent for a
`HOPPER_COMPUTE_A` (`0xcbc0`) object. It never was: the bare-metal host does the
same pairing and succeeds, on every driver tested. Hopper reuses the Ampere
GPFIFO channel class and `HOPPER_CHANNEL_GPFIFO_A` (`0xc86f`) is requested by
neither side. The status was never `INVALID_CLASS`; it was `OBJECT_NOT_FOUND`,
which is a statement about the parent handle. Minimal reproducer:
[`tests/repro/vk_create_device.c`](../../tests/repro/vk_create_device.c).

### What the earlier 580-only A/B did and did not show

Before a pre-580 box existed, the driver and ~20 nvkvm commits had moved
together, so they were separated by A/B on 580.126.09 — where, as the sweep above
now explains, *nothing* fails:

| arm | QEMU | guest module | driver | `vkCreateDevice` |
|---|---|---|---|---|
| shipping | `e8e472f` | `e8e472f` | 580.126.09 | rc=0 OK |
| map-VA fill-in disabled | `e8e472f` + gate | `e8e472f` | 580.126.09 | rc=0 OK |
| **full original stack** | **`a996386`** | **`a996386`** | 580.126.09 | rc=0 OK |

That A/B was correctly executed and its narrow result stands — no nvkvm change
between `a996386` and `e8e472f` fixed this, and the 2026-08-20 map-VA /
`UNMAP_MEMORY` work is not involved. Its mistake was the inference drawn from it.
Every arm ran on the one driver that cannot exhibit the bug, so the experiment
could only ever return "OK", and "the only remaining variable is the host driver"
turned that into a verdict about NVIDIA's code. The missing measurement was one
bare-metal run of the probe on 570 — about a minute, once a 570 machine exists.

## Why the ABI profile table exists, and how to check a host

NVIDIA guarantees no ioctl ABI stability across driver releases, so `nvkvm` keys
struct layouts off the host driver version. **Eight profiles** cover every
open-kernel-modules release from 515 to 610, measured by compiling `sizeof` /
`offsetof` probes against 61 upstream tags — and keyed on the full
`major.minor.patch`, because two widely deployed branches change layout *inside*
the branch. **Six of the eight have been booted**; no profile's values proved
wrong in practice. See [ABI profiles](abi-profiles.md) and
[supported drivers](supported-drivers.md).

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

## Two bugs that only A100 exposed

Both had been silently wrong on every card before the first datacenter GA100,
and neither was visible on any consumer die.

**The host half of the `RM_UNMAP_MEMORY` address contract was never
implemented.** Every unmap reached the driver with virtual address 0. Consumer
cards tolerated it; GA100 did not.

**`UVM_MAP_DYNAMIC_PARALLELISM_REGION` was unhandled.** `libcuda` calls it during
context creation on GA100, so `cuCtxCreate` returned 999 (`CUDA_ERROR_UNKNOWN`)
and every CUDA check below it skipped.

The second one is the more interesting failure, because it had been *silently
misrouted* rather than rejected. Bare UVM ioctl numbers share a number space
with the NVIDIA frontend ioctls, and 65 == 0x41 == `NV_ESC_RM_IDLE_CHANNELS` —
so the call had been forwarded for a long time as a completely different ioctl.
Adding a type gate to the sanitiser turned that into an honest `ENOTTY`, which
is what made the missing handler visible at all.

With both fixed the A100 reaches 28/28, and a 3B LLM generates in the guest at
5.75 GiB VRAM.

Confirmed independently of the A100 afterwards: a three-way A/B on one Turing
box, one variable at a time, reproduced both failures and both fixes —
`cuda_ctx_create rc=999` without the UVM handler, `vk_probe`/`gl_probe` dying on
SIGBUS with an earlier version of the unmap fix that overwrote map-VA table
entries instead of treating them as a stack, and 28/28 with both correct.
