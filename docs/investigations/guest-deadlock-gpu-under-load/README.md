# Guest deadlocks when the GPU is active under heavy I/O — OPEN

**Status: OPEN, reproducible.** Measured on the physical PC, 2026-08-30, on a
SteamOS guest. This is the real bug behind what was reported as "the OTA freezes
at 1 sec remaining" and "the VM is frozen".

## Signature

Run `steamos-update` (a ~1 GB rauc install) **with the GPU path live** — Game
Mode up, gamescope compositing. 60–90 seconds in, the guest stops:

| observation | value |
|---|---|
| QMP `query-status` | `running` (NOT suspended, not S3) |
| `/proc/<qemu>/io` | frozen at 6,558,060,544 read / 9,344,331,776 written, **unchanged across 5 minutes** |
| vCPUs | 7 of 8 in HLT at `ffffffffb76f1532`; **CPU#3 spinning at `ffffffffb86e6148`** |
| stability of that state | identical twice 5 s apart, and again 2 minutes later |
| guest sshd | accepts TCP, then resets at banner/key exchange |
| guest kernel log | no OOM, no hung-task, no soft-lockup, no filesystem error |
| host | 1.2 TB free, 19 GB RAM available, host load ~0.2 |

One CPU spinning while the rest halt is the shape of a spinlock whose holder
never releases — on KVM, contended spinlock waiters halt until kicked.

## It also reproduces at SHUTDOWN, with no OTA involved

Measured 2026-08-30, second reproduction, triggered by a plain `systemctl
reboot` while Game Mode was running:

| observation | value |
|---|---|
| `/proc/<qemu>/io` | 145072128 read / 2846859264 written, **identical 5 s apart** |
| QEMU CPU | 1.0% |
| vCPUs | **all 8 at RIP `ffffffffb9ed7aef`**, identical across two samples 5 s apart |
| QMP | `running` |

So the trigger is not the OTA and not sustained disk writes -- a reboot with the
GPU active is enough. What the OTA and a shutdown share is **GPU teardown /
activity**, which is the common factor.

(The RIP differs from the OTA capture only because KASLR relocates the kernel
each boot; compare symbols, never raw addresses, across boots.)

## The discriminating control

**The same OTA on the same guest with the GPU path dead completes normally**
("Update completed"). So the hang tracks **GPU activity**, not the OTA's disk
writes, and not rauc.

That control is what makes this a GPU-path bug rather than an I/O or update bug,
and it should be the first thing re-run if anyone doubts the framing.

## What it is NOT

- **Not the no-op/DENY gate.** `NVA06F_CTRL_CMD_RESTART_RUNLIST` (0xa06f0111)
  denials looked like a smoking gun — 23,568 of them in the baseline, ending
  right as the guest died. They were an artifact of an OLD build that had no
  `nvkvm_ctrl_noop[]` table. Rebuilding the current tree with the noop fix
  REVERTED still yields zero denials (345 no-ops), and the deadlock still
  happens either way. See `../../src/qemu/nvkvm_isolate_handlers.c`.
- **Not S3 suspend.** QMP reports `running`; vCPU state is HLT-in-guest, not a
  paused VM.
- **Not memory or disk exhaustion.** Both measured, both fine.
- **Not the guest kernel dying.** No oops, no panic, no distress of any kind in
  the log.

## Also seen in the same runs

New denial classes worth checking against the allowlist, not yet linked to the
hang: `0x0080170f` (4 occurrences) and `0x2080220b` (6, also present in
baselines that did NOT hang).

## Next steps

1. Get the guest kernel symbol for `ffffffffb86e6148` — that names the lock the
   spinning CPU is on and likely names the bug. The guest is SteamOS
   `6.16.12-valve24.4-1-neptune-616`; its `System.map` is in the image.
2. Bisect the *workload*, not the code: does GPU activity alone deadlock under
   sustained disk I/O from any source (e.g. `dd` to the slot-B partition), or
   only under rauc? That separates "GPU + heavy I/O" from "GPU + rauc".
3. Attach a serial console BEFORE reproducing (`SERIAL=socket` in
   `run_steamos_nvkvm.sh`, then
   `socat -,raw,echo=0 UNIX-CONNECT:.../serial.sock`). Every diagnosis so far has
   been forensic because the only console was a write-only log file, and sshd is
   gone by the time anyone looks. Note SteamOS's kernel cmdline has no
   `console=ttyS0`, so add it for the kernel to talk to that socket at all.
