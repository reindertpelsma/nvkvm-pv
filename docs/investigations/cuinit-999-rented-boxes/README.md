# `cuInit` returned 999 in nvkvm guests — FIXED (regression, `0a5527a` → `6f2e37d`)

**Status: CLOSED.** Root-caused, fixed, and verified by a full sweep at
`30P/0F/0S` on three GPU architectures.

## What it was

`0a5527a` ("isolate: the RM_CONTROL deny sentinel never denied") added a
validity check on the fd embedded in a UVM ioctl's param blob, and hardcoded the
expected device to `NVKVM_DEV_CTL` on the stated premise that "the only two rows
that set fd_off ... name an /dev/nvidiactl fd".

There are **three** such rows, and they do not all name the same device:

| cmd | field | device |
|---|---|---|
| 25 `UVM_REGISTER_GPU_VASPACE` @16 | `rmCtrlFd` | `/dev/nvidiactl` |
| 37 `UVM_REGISTER_GPU` @24 | `rmCtrlFd` | `/dev/nvidiactl` |
| **75 `UVM_MM_INITIALIZE`** @0 | **`uvmFd`** | **`/dev/nvidia-uvm`** |

So every `UVM_MM_INITIALIZE` (cmd `0x4b`) was refused with `-EBADF` and
`cuInit()` returned 999.

Two things made it expensive to find:

1. **`nvidia-smi` kept working**, because NVML never touches UVM. The guest
   looked healthy — GPU enumerated, memory, driver version, utilisation — while
   CUDA was completely dead. That asymmetry is the tell: *NVML fine + CUDA dead
   ⇒ suspect UVM.*
2. **It was silent.** That one DENY used `NVKVM_DBG()` (gated on `NVKVM_DEBUG`)
   while every sibling DENY in the same function uses unconditional
   `fprintf(stderr, ...)`, and `nvkvm_log.h` states security DENY decisions must
   always be visible. Nothing appeared in the guest or host logs.

## The fix (`6f2e37d`)

The expected device now comes from `nvkvm_uvm_embedded_fd_dev()`, per command.
The security property `0a5527a` was added for is unchanged: type must still be
`NVKVM_HANDLE_TYPE_NVIDIA` (a memfd is still refused), the generation check
still rejects a recycled slot, and an unlisted command returns `-1` and is
**denied**, so a future ioctl that grows an embedded fd must be added
deliberately rather than admitted by default. The DENY is also promoted to
unconditional `fprintf`.

## Verification

- **Local, PC:** `cuInit rc=0`; `scripts/e2e/vecadd.c` → `VECADD OK`, 1048576
  elements, 0 mismatches (real kernel launch, arithmetic checked).
- **Negative control:** forcing the helper to expect the wrong device for cmd 75
  reproduces `cuInit rc=999` *and* prints the DENY with `NVKVM_DEBUG` unset —
  proving the gate still discriminates by device and that the logging fix works.
- **Sweep, rented hardware, 2026-08-30** — every row `30P/0F/0S`:

| arch | control | 545 | 550 | 570 | 580 | 610 |
|---|---|---|---|---|---|---|
| ampere (RTX 3060) | pass | pass | pass | pass | pass | pass |
| turing (GTX 1660 S) | pass | pass | pass | pass | pass | pass |
| ada (RTX 4070S Ti) | pass | pass | pass | pass | pass | pass |

  Before the fix the identical matrix read `17P/1F/12S` on every row.

## How it was found, and the mistake worth not repeating

The regression was first misdiagnosed **as an environmental limit** — a doc in
this directory concluded CUDA "fails at nesting depth L2" and marked that the
surviving hypothesis. It was wrong, and the evidence refuting it was already
committed in this repo: `sweep-runs/audrem-{ampere,ada}/sweep.jsonl`, recorded
at tree `bbe7010` on 2026-08-27, show `30P/0F/0S` on the same vast host image
and default guest, for the same drivers that later failed.

**An environmental explanation for a new failure must be checked against the
last known-good run of the same thing before it is written down.** The passing
rows were sitting in the tree the whole time.

Bisect: `bbe7010` (good) .. `54269c5` (bad), pinned to `0a5527a` by an adjacent
pair — `d8553ee` → `rc=0`, `0a5527a` → `rc=999`. A second, unrelated prior
(`2a44489`, the UVM fd-dup commit) looked compelling from the diff and was
**excluded by measurement**: it is commit 29, and commit 54 still passed.

## Blackwell

Still uncovered, for an unrelated reason — its boxes wedge during driver purge.
See `../blackwell-purge-wedge/`. That is not this bug.
