# The gamescope scheduling controls

SteamOS's own session never starts on the physical PC. It gets Vulkan, selects
`connector Virtual-1` and mode `1920x1080@60Hz`, then issues five RM control
commands, is denied every one of them, and retries — until systemd's start
timeout kills the unit and restarts it, forever.

The five are not error recovery. They are gamescope configuring **interleaved
and realtime runlist scheduling** for low-latency compositing;
`RESTART_RUNLIST` is how a scheduling-policy change is committed.

This page is the audit of whether nvkvm may allow them, and what was done. It
is a companion to [`allowlists.md`](allowlists.md), which describes the nine
gates themselves; this is one worked decision at the eighth of them.

## Verdict

| cmd | name | OGKM `accessRight` | verdict |
|---|---|---|---|
| `0x20801109` | `NV2080_CTRL_CMD_FIFO_GET_INFO` | `0x0` — none | **allowed** (forwarded) |
| `0x20801115` | `NV2080_CTRL_CMD_FIFO_RUNLIST_SET_SCHED_POLICY` | `0x2` — `RS_ACCESS_NICE` | **allowed as no-op** |
| `0xa06f0111` | `NVA06F_CTRL_CMD_RESTART_RUNLIST` | `0x2` — `RS_ACCESS_NICE` | **allowed as no-op** |
| `0xa06f0109` | `NVA06F_CTRL_CMD_SET_INTERLEAVE_LEVEL` | `0x2` — `RS_ACCESS_NICE` | **allowed as no-op** |
| `0xa06c0110` | `NVA06C_CTRL_CMD_MAKE_REALTIME` | `0x2` — `RS_ACCESS_NICE` | **allowed as no-op** |

"Allowed as no-op" means: QEMU accepts the command, returns `NV_OK`, and never
forwards it. **The guest is being lied to.** What that costs is
[below](#what-the-lie-costs).

Every row was checked at **all 216 supported OGKM tags**, 515.43.04 through
610.57.04, not just the branch in front of us. The per-tag evidence is
committed at
[`tests/abi_parity/ogkm_gamescope_ctrl_audit.tsv`](../../tests/abi_parity/ogkm_gamescope_ctrl_audit.tsv)
and re-derivable with `tools/ogkm_ctrl_audit.sh`.

## Why the flags word is not the test

The obvious place to look is the `RMCTRL_FLAGS` word in the NVOC export table,
and it gives the wrong answer. **All five carry
`RMCTRL_FLAGS_NON_PRIVILEGED` (`0x8`) and none carries
`RMCTRL_FLAGS_PRIVILEGED` (`0x4`)**, on every tag. Judged on flags alone, all
five would be allowed.

The authority is the neighbouring `accessRight` field, and the thing to know
about it is that **it is not an index — it is a one-limb access mask**:

> ```c
> // Copy from NVOC exportedEntry
> pRmCtrlExecuteCookie->cmd       = exportedEntry->methodId;
> pRmCtrlExecuteCookie->ctrlFlags = exportedEntry->flags;
> // One time initialization of a const variable
> *(NvU32 *)&pRmCtrlExecuteCookie->rightsRequired.limbs[0]
>                                 = exportedEntry->accessRight;
> ```
>
> — `src/nvidia/src/kernel/rmapi/resource.c:170-175` (595.84)

So `accessRight = 0x2` is `NVBIT(1)`, and access right 1 is `RS_ACCESS_NICE`
(`src/common/sdk/nvidia/inc/rs_access.h:60` — the value is `1` on all 216
tags). Read as an index it would decode to `RS_ACCESS_DEBUG` and mislead;
read correctly it is the GPU equivalent of `CAP_SYS_NICE`.

## The privilege chain, at 595.84

Four of the five are privileged, by this path:

1. **The mask is required.** `rmControlCmdExecute()` checks it against the
   invoking client before dispatch —
   `rsAccessCheckRights(pRmCtrlParams->pResourceRef, ..., &pRmCtrlExecuteCookie->rightsRequired)`
   at `src/nvidia/src/kernel/rmapi/control.c:748-751`. Insufficient rights
   return `NV_ERR_INSUFFICIENT_PERMISSIONS`
   (`src/nvidia/src/libraries/resserv/src/rs_access_map.c:203-206`).
2. **Owning the channel does not help.** `RS_ACCESS_NICE`'s metadata is
   `RS_ACCESS_FLAG_ALLOW_PRIVILEGED | RS_ACCESS_FLAG_UNCACHED_CHECK` and
   notably *not* `ALLOW_OWNER`
   (`src/nvidia/src/libraries/resserv/src/rs_access_rights.c:46-49`).
   `UNCACHED_CHECK` re-runs the check on every call, so it cannot be cached
   into existence either.
3. **It is granted to root, or to `CAP_SYS_NICE`, and to nobody else.**
   `ALLOW_PRIVILEGED` grants it at `privLevel >= RS_PRIV_LEVEL_USER_ROOT`
   (`src/nvidia/src/libraries/resserv/src/rs_access_map.c:505-511`), and
   `privLevel` for an ioctl client is
   `osIsAdministrator() ? RS_PRIV_LEVEL_USER_ROOT : RS_PRIV_LEVEL_USER`
   (`src/nvidia/arch/nvalloc/unix/src/escape.c:375`). Otherwise the resource's
   access callback runs, which for `RS_ACCESS_NICE` is
   `osCheckAccess(RS_ACCESS_NICE)`
   (`src/nvidia/src/kernel/rmapi/client_resource.c:152-157`) →
   `capable(CAP_SYS_NICE)` (`kernel-open/nvidia/os-interface.c:397-400`).

So the owner's test — *could an unprivileged user issue this successfully on
bare metal?* — is answered **no** for all four.

NVIDIA also says so in prose, which is worth quoting because it is
unambiguous:

> Since this is a global setting, only privileged clients will be allowed to
> set it. Regular clients will get NV_ERR_INSUFFICIENT_PERMISSIONS error.
>
> — `src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fifo.h:626-627`
>   (`SET_SCHED_POLICY`)

> This command interacts with the scheduler and may cause certain low priority
> channels to starve under certain circumstances. Therefore, it is only
> available to privileged clients.
>
> — `src/common/sdk/nvidia/inc/ctrl/ctrla06f/ctrla06fgpfifo.h:188-190`
>   (`RESTART_RUNLIST`)

> For safety reasons, setting this property requires PRIVILEGED user level.
>
> — `src/common/sdk/nvidia/inc/ctrl/ctrla06c.h:254` (`SET_INTERLEAVE_LEVEL`)

## The "lowering is free, like `nice`" hypothesis is false

The intuition worth testing was that dropping your own scheduling priority
might be unprivileged the way `nice(2)` is, while raising it needs root — which
would make a *value*-restricted allow possible.

It does not hold. `rightsRequired` is a **per-`methodId` constant** read from
the export table and checked in `control.c` **before the handler is ever
entered**, so the parameter value is not in scope at the point the decision is
made. Setting `NVA06C_CTRL_INTERLEAVE_LEVEL_LOW` is gated exactly as hard as
setting `_HIGH`. The value is only validated later, in
`kchangrpSetInterleaveLevel_IMPL`
(`src/nvidia/src/kernel/gpu/fifo/kernel_channel_group.c:667-694`), which
switches on LOW/MEDIUM/HIGH purely to reject out-of-range input.

The GPU rule is therefore *stricter* than `nice(2)`, not looser: there is no
value-dependent path to carve out, and by the owner's rule 2 a value-gated
command would have been a NO anyway.

## The precedent cuts the other way

Five siblings on these same interfaces are already allowed —
`0xa06c0101`/`0103`/`0105` and `0xa06f0103`/`0104`. They are a good precedent,
and what they establish is the *opposite* of an argument for adding these:

| cmd | name | `accessRight` |
|---|---|---|
| `0xa06c0101` | `GPFIFO_SCHEDULE` | `0x0` |
| `0xa06c0103` | `SET_TIMESLICE` | `0x0` |
| `0xa06c0105` | `PREEMPT` | `0x0` |
| `0xa06f0103` | `GPFIFO_SCHEDULE` | `0x0` |
| `0xa06f0104` | `BIND` | `0x0` |

All five are `accessRight = 0x0` on all 216 tags. **nvkvm's control allowlist
has never contained a command requiring an access right**, and these four would
be the first.

`SET_TIMESLICE` is the sharpest comparison: it is a scheduling knob, on the same
object, and it is free. NVIDIA itself draws the line between *how long your own
channel runs* (unprivileged) and *where you sit relative to everyone else*
(privileged). `0xa06c0107`, the TSG twin of `SET_INTERLEAVE_LEVEL`, is
`accessRight = 0x2` and is likewise not allowlisted.

## Two of them are cross-VM hazards

This gate is a host/cross-VM control, and two of the four are squarely in its
remit even setting privilege aside.

`SET_SCHED_POLICY` is **GPU-global and lockable**. Per `ctrl2080fifo.h:629-631`,
once set, "that policy cannot be changed to a different one unless all clients
which set it have either restored the policy ... or died". A guest that set a
policy and held it would pin the scheduler for the host and every other VM on
that GPU until it exited — a cross-VM denial of service with no fault injection
required.

`MAKE_REALTIME` promotes a TSG above every non-realtime channel on the runlist
and forces the others preemptible:

> A realtime TSG will have the highest interleave level when the scheduling
> policy is CHANNEL_INTERLEAVED, and will also precede any non-realtime
> channel/TSG in the order channels are added to the corresponding runlist.
>
> Whenever a realtime TSG is added to a runlist, all non-realtime channels/TSGs
> are made preemptible by setting a COMPUTE preemption mode to CTA.
>
> — `src/common/sdk/nvidia/inc/ctrl/ctrla06c.h:370-377`

## Why no-op rather than deny

Correctness does not depend on any of the four. They are scheduling **priority**
and preemption **latency** hints: the same work runs, in the same order within a
channel, to the same result — it may just start a timeslice later. That is
precisely the case where returning success and doing nothing is legitimate, so
that is what is done.

Not forwarding is also **strictly safer than forwarding**, which is the part
worth being clear about:

- In the default `namespace` and `uid` isolation rungs the stub has no
  capabilities and a non-root uid (`src/qemu/nvkvm_isolate.c:99-125`), so
  `osIsAdministrator()` is false and `capable(CAP_SYS_NICE)` is false. A
  forwarded command would come back `NV_ERR_INSUFFICIENT_PERMISSIONS` from the
  host driver and gamescope would still spin. **Allowlisting these would not
  have fixed anything.**
- In the weaker `seccomp` and `none` rungs
  (`src/qemu/nvkvm_isolate_uid.h:107-116`) the stub keeps QEMU's uid and
  capabilities. Against a root QEMU a forwarded `SET_SCHED_POLICY` would
  *succeed*, handing the guest the GPU-global scheduler lock described above.
  Not forwarding removes that rung dependency entirely.

## What the lie costs

Stated plainly: we return `NV_OK` and do nothing. gamescope believes it
configured `CHANNEL_INTERLEAVED` scheduling, promoted its compositing TSG to
realtime, and kicked a runlist restart. None of that happened; its channels keep
the default policy and default interleave level.

**The cost is compositing latency, and nothing else.** Under GPU contention a
frame may be scheduled a timeslice later than gamescope intends. No rendering is
wrong, no frame is dropped, no state is corrupted, and nothing outside the
guest's own scheduling preference is affected. On a single-guest desktop with no
competing GPU load there may be no observable difference at all.

The lie is also not observable through this interface. All four parameter
structs are input-only — there is no output field to forge. `FIFO_GET_INFO`, the
one query in the sequence that *is* forwarded, has no index that reports
scheduling policy or interleave level (indices `0x0`..`0xa`,
`ctrl2080fifo.h:140-150`). The two readbacks that would expose it,
`GET_INTERLEAVE_LEVEL` `0xa06c0108` and `0xa06f0110`, are not in the allowlist
and stay denied; `tests/unit/test_ctrl_gate.c` asserts that.

## Why `FIFO_GET_INFO` is a real allow, not a no-op

`0x20801109` is the one command in the sequence that is genuinely unprivileged
— `accessRight = 0x0` on all 216 tags, so `rsAccessCheckRights()` returns
`NV_OK` at its first line (`rs_access_map.c:193-194`) without consulting any
capability. It is a read-only query. So it is forwarded for real.

The thing to get right is that **despite the name it is not the `GET_INFO`
pointer shape**. `GR_GET_INFO`, `FB_GET_INFO` and `BUS_GET_INFO` are
`{ u32 count@0; pad; NvP64 ptr@8 }` and need the marshalling in
`nvkvm_ctrl_list_entry_size()` (`src/stub/nvkvm_stub.c:627`, mirrored in
`src/guest/nvkvm_main.c:1068`) so that a guest VA never reaches the host driver.
FINN inlines this one:

```c
typedef struct NV2080_CTRL_FIFO_GET_INFO_PARAMS {
    NvU32                 fifoInfoTblSize;
    NV2080_CTRL_FIFO_INFO fifoInfoTbl[NV2080_CTRL_FIFO_GET_INFO_MAX_ENTRIES];
    NvU32                 engineType;
} NV2080_CTRL_FIFO_GET_INFO_PARAMS;
```

4 + 256×8 + 4 = 2056 bytes, flat, no pointer anywhere in it, far under the 1 MiB
aux cap. That struct body is **byte-identical on all 216 tags** and
`MAX_ENTRIES` is 256 on every one (swept column `fifo_get_info_params`). So it
needs no marshalling entry, and adding one would be wrong.

## Where this lives in the code

The no-op set is deliberately **not** in `nvkvm_ctrl_allowlist[]`. That table
means exactly one thing — "this command may be forwarded to the host driver" —
and these must not be. They are a separate, smaller table,
`nvkvm_ctrl_noop[]`, with its own predicate `nvkvm_ctrl_cmd_noop()`, in
`src/qemu/nvkvm_ctrl_allowlist.h` beside the gate it belongs to (the same "one
definition, one place to change" rule that put the gate there). The two sets are
disjoint, and `tests/unit/test_ctrl_gate.c` fails if they ever overlap.

The answer is given in `src/qemu/nvkvm_isolate_handlers.c`, immediately before
the DENY branch, mirroring it exactly except for the status: `nvstatus = 0`
(`NV_OK`) rather than `0x56` (`NV_ERR_NOT_SUPPORTED`), nothing written back,
nothing forwarded. The 1 MiB aux bound is repeated on the no-op path so it
cannot be used to smuggle an oversized blob past the cap.

**Nothing is needed in the stub.** `ring_ctrl_must_punt()`
(`src/stub/nvkvm_stub.c:2891-2911`) punts any command for which
`nvkvm_ctrl_cmd_allowed()` is false, and these are false there by construction,
so the ring bounces them to the virtqueue and QEMU's single answer is the only
one. Punt means "not executed", so there is no path on which one of these both
no-ops and runs.

## What this does not establish

**Whether gamescope actually starts.** This removes five denials from its
startup path; it does not prove there is not a sixth thing failing behind them.
The sequence was observed looping on these five, but a loop stops at its first
failure, so anything gamescope would have done *after* them is unobserved. This
needs a run on the physical PC to confirm, and that has not been done.

**What physical RM does with three of them.** `SET_SCHED_POLICY`,
`RESTART_RUNLIST` and `MAKE_REALTIME` have **no implementation body in OGKM at
all** — only an export-table entry pointing at a symbol defined in closed
physical RM, reached via `RMCTRL_FLAGS_ROUTE_TO_PHYSICAL`. Their privilege
gating is fully visible (it is enforced in `control.c`, before dispatch, in open
code) but their *behaviour* is not auditable from source. That is an independent
reason not to forward them, and it is why the header quotes above are cited as
the description of what they do.

**The `RS_ACCESS_NICE` claim is source-derived, not measured.** Nobody ran an
unprivileged `SET_INTERLEAVE_LEVEL` on bare metal and observed
`NV_ERR_INSUFFICIENT_PERMISSIONS`. The chain is short and entirely in open code,
but it is a reading.

## The strongest argument against this

Not the security call — that one is not close. It is the **no-op** that is
arguable, on two grounds.

First, **a silent lie is a bad failure mode**. nvkvm's own allowlist doc draws
this lesson from NVKMS `cmdType` 60: "an allowlist captured on one driver branch
expires on the next, and its failures do not necessarily surface as denials" —
that bug cost real time precisely because it did not announce itself. A no-op is
that shape on purpose. If gamescope ever gains a code path that *depends* on
realtime scheduling having taken effect — a deadline it misses, a fence it waits
on assuming preemption — the symptom will be a hang or a stutter with no `DENY`
line anywhere, and the next person will not think to look here. The mitigation
is that it is logged (`NVKVM_DBG`, "NO-OP ... not forwarded") and documented in
three places, but a debug-level log is weaker than a denial.

Second, **"correctness is preserved" is an inference from the headers, not a
measurement.** It rests on these being pure priority hints, which is what NVIDIA
documents and what their parameter structs support — but `RESTART_RUNLIST` in
particular has a real side effect (it preempts the running channel), and the
claim that skipping it is always safe assumes no caller treats it as a barrier.
gamescope is the only caller here and it is using it to commit a policy change
that also is not happening, so the two lies are consistent with each other. That
is an argument, not a proof.

The honest alternative, if either concern bites, is to deny the four loudly and
accept that SteamOS's session does not start — which is the status quo this
audit was asked to change.
