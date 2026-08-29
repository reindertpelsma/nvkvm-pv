# Pre-release security & reliability audits

Five components, audited independently before release, re-run after every
substantive change. The gate is that an audit comes back **clean** — not that its
findings were triaged.

| component | file | verdict |
|---|---|---|
| guest kernel module | [2026-08-29-guest.md](2026-08-29-guest.md) | 12 findings — **1 critical**, 8 serious, 3 minor — **6 fixed on `fix/guest-lifetime`** |
| VMM | [2026-08-29-vmm.md](2026-08-29-vmm.md) | 13 findings — 6 serious, 7 minor |
| packaging / scripts / kata | [2026-08-29-packaging.md](2026-08-29-packaging.md) | 54 findings — **3 critical**, 21 serious, 30 minor |
| broker | [2026-08-29-broker.md](2026-08-29-broker.md) | 9 findings — 2 serious, 7 minor |
| isolate | [2026-08-29-isolate.md](2026-08-29-isolate.md) | 23 findings — **5 critical**, 12 serious, 6 minor |

## Totals: 111 findings, 9 critical

## The criticals

1. **Guest UAF** — `nvkvm_gem_resolve_fwd()` returns an `fd_ctx` after dropping
   the reference that kept it alive; a concurrent `GEM_CLOSE` frees it mid
   round-trip. `src/guest/nvkvm_drm.c:327`
2. **Root RCE on the sweep coordinator** — vast.ai API fields are `eval`'d as
   root. `scripts/sweep.sh:705` / `:892`
3. **Guest SSH on 0.0.0.0** with `ubuntu:ubuntu` + `NOPASSWD:ALL`, on every
   rented public-IP box. `scripts/run_test_vm.sh:308`
4. **Kata guest `modprobe` over plain HTTP**, unverified, run as guest root.
   `nvkvm-kata/scripts/prepare-guest-rootfs.sh:81-101`
5. **Guest-chosen `_IOC_SIZE` into a 256-byte stack buffer** in the stub's ring
   path — up to 16 KB written past a stack array. `src/stub/nvkvm_stub.c:3013`
6. **A pending ioctl unlinked then abandoned**, blocking a pool worker forever
   and eventually wedging block I/O VM-wide. `src/qemu/nvkvm_isolate.c:1027`
7. **`KILL_ISOLATE` destroys any isolate on a bare guest-supplied id** — no
   token, no session, no ownership check. `nvkvm_isolate_handlers.c:858`
8. **`CLOSE_HANDLE_ON_ISOLATE` tears down a neighbour's live mappings** and walks
   its refcount to zero. `nvkvm_isolate_handlers.c:1005`
9. **Sparse-GPA extents leaked unrecoverably** on three error paths; one request
   can consume the entire 128 GiB window. `nvkvm_isolate_handlers.c:4024`

**The isolate result is the one that matters most: four separate handlers take an
`isolate_id` off the wire and act on it with no ownership check, and the
`mmap_token` meant to protect the known gap is a round-robin index enumerable in
8191 requests. On the default rung isolates share QEMU's uid. Taken together the
cross-isolate boundary is not currently a boundary.**

## Two structural findings worth more than any single item

- **No lock exists in any of the three repos** — no `flock`, no lockfile, no
  `RefuseManualStart=`. Root of five separate race findings.
- **A recurring pattern where the comment describes the safe behaviour and the
  code does something else** (at least six instances across components). These
  are the most dangerous, because the comment stops anyone re-reading the code.

## Why the audits were weighted the way they were

Bounds checking is the easy class. The audits were explicitly re-weighted toward
**races, lock assumptions that cross file boundaries, ownership confusion
(silent dup, double free, UAF) and signedness**. That re-weighting is what found
the guest critical and, on a second pass, a VMM type confusion the first pass
missed. Keep that emphasis on re-runs.


## Remediation status

One branch per component, so each can be re-audited independently and a mistake
in one does not block another. Nothing is merged to `main`.

| branch | covers | state |
|---|---|---|
| `fix/guest-lifetime` | guest: the critical UAF + 5 serious | **builds**, reviewed, ready |
| `fix/isolate-boundary` | 6 isolate findings incl. 4 criticals | compiles + unit tests, reviewed — **one critical only half-closed** |
| `fix/vmm-lifetime` | 5 VMM findings | front-end compiles, reviewed, ready |
| `fix/broker-work-bounds` | all 5 broker findings | done — builds, **ASan+UBSan clean**, 79 selftests pass |
| `fix/packaging-supply-chain` | nvkvm-pv: dockerignore, action pinning, workflow injection, 9p | done, reviewed |
| `fix/kata-supply-chain` | nvkvm-kata: the HTTP modprobe critical | done, **verified live** |
| `fix/steamos-warn` | nvkvm-steamos: the undefined `warn()` | done, `make check` 17/17 |

### Critical status: 8 of 9 closed — and two were OVER-RATED, see the correction below

| # | critical | state |
|---|---|---|
| 1 | guest UAF on a borrowed `fd_ctx` | fixed, reviewed |
| 2 | sweep.sh `eval`s vast API fields as root | fixed on `main`, test proves it |
| 3 | test guest ssh on 0.0.0.0 | fixed on `main` |
| 4 | kata `modprobe` over plain HTTP | fixed, **verified live** (real fetch + digest match; forced mismatch dies) |
| 5 | ring `_IOC_SIZE` stack smash | fixed (oversize is PUNTed) |
| 6 | reader abandons a claimed pending entry | fixed (explicit `-ECONNRESET` wake) |
| 7 | `CLOSE_HANDLE_ON_ISOLATE` cross-isolate | fixed (both checks, before the reap) |
| 8 | sparse-GPA extents leaked ×3 | fixed |
| 9 | **`KILL_ISOLATE` unauthenticated** | **HALF-CLOSED — see below** |

### `KILL_ISOLATE` is still unauthenticated, and needs a protocol decision

`struct nvkvm_req_kill_isolate` is `{isolate_id, reserved}` and carries **no
caller identity at all**. The fix reads `reserved` as an optional caller
session_id:

```c
if (req->reserved != 0 && !session_has_isolate(nv, req->reserved, req->isolate_id))
```

Today's guest calls `nvkvm_virtio_kill_isolate(isolate_id)` and leaves `reserved`
zero, so **the check is skipped and any guest can still destroy any isolate in
the VM**. The fix is inert now and becomes real the moment the field is filled.

Closing it needs both halves, which is a protocol change across two components:
1. guest fills `reserved` with `session->id` (`src/guest/nvkvm_session.c:136` →
   `nvkvm_virtio_kill_isolate`), then
2. QEMU fails closed on `reserved == 0`.

Failing closed first would break every kill today, which is why it was not done
unilaterally.

### Review notes on the branches I checked by hand

- **`fix/vmm-lifetime`**: the present fix widens `p->lock` to cover the buffers,
  not just the indices — so I checked for self-deadlock. `nvkvm_stage_ensure()`
  takes the lock itself and both its call sites enter with a balance of 0;
  `nvkvm_stage_release()` documents "caller must hold" and its one external call
  site enters holding it. Correct. The agent also **deviated from its brief** —
  it was told to touch only the embedded-fd lines, and it added an `emb_fd[2]`
  array plus two close loops, because confining the change would have leaked an
  fd per UVM ioctl. It flagged the deviation; I verified the only escape path
  between the dup and the close closes them first. The deviation was right.
- **`fix/isolate-boundary`**: compiles to real `.o` with the exact flags from the
  existing build, diffed against baseline for new diagnostics; the stub compiles
  *and links*; unit tests pass. `CLOSE_HANDLE_ON_ISOLATE` trades a refcount
  underflow for a bounded leak when the stub never registered a handle QEMU
  thinks it holds — a good trade, disclosed in the commit.

Nothing is runtime-tested. No branch is merged.

### `fix/guest-lifetime` — reviewed 2026-08-29

The critical UAF fix was checked by hand rather than taken on trust, because a
wrong refcount fix is worse than the bug it replaces:

- `nvkvm_gem_resolve_fwd()` now returns an **owned** reference; the contract is
  written at the definition and at both call sites, which is what the two
  disagreeing sites lacked.
- Three `get` sites (same-isolate, cached cross-isolate under `xiso_lock`,
  broker) against four `put` sites in the single caller, one immediately before
  each of its four late returns.
- The two early returns cannot leak: one precedes `resolve_fwd` entirely, and on
  every failure path `resolve_fwd` returns before taking any reference.
- The broker branch deliberately takes **two** references — one for the
  single-entry cache, one for the returned pointer — so the caller's put cannot
  tear down the cache entry. On failure it takes none.
- `nvkvm_fb_stub_handle()` stays borrowed, which is correct under the framebuffer
  reference the KMS path holds, and that contract is now stated too.

It builds against real kernel headers, and the warning set is identical to
pristine `main` (three pre-existing warnings, none new). **Not runtime-tested** —
no VM, no GPU. The reference balance under real eviction traffic (a two-compositor
workload) is the one thing review cannot settle.

Still open in the guest, deliberately not attempted because both are
lock-ordering changes rather than patches: the `ext_lock`/`mmap_lock` AB-BA
deadlock, and the false "no caller holds mmap_lock" invariant.


## Corrections the remediation made to the audit itself

Two, both worth more than a fix, and both the result of telling agents to verify
in the code rather than trust the finding.

**The `warn()` crash loop was half wrong.** The audit said an undefined `warn()`
turns the documented image fallback into a `set -e` abort and a restart loop. It
does not, at `steamos-container-entrypoint.sh:136-137`: those calls sit inside
`$(latest_recovery_url)`, and **bash does not inherit `errexit` into a command
substitution** unless `shopt -s inherit_errexit` is set, which nothing in the
repo sets. The fallback did run — just silently, emitting two spurious
"command not found" lines instead of its explanation. The `:537` call **is** in
the main flow and does abort, so that half stands. `warn()` is defined either way.

**A kata function that could never have succeeded.** Fixing the HTTP fetch
surfaced that `fetch_deb_kmod`'s parse is

```sh
printf '%s' "$pkgs" | awk '... { print fn; exit }'
```

under `set -euo pipefail`. `kmod` sorts early in a Packages index, so awk exits
with most of the ~7 MB still unwritten, `printf` takes SIGPIPE, and the installer
dies with a bare exit 141. Reproduced here exactly: the same construct under the
same shell options exits 141. This is a plain functional bug, unrelated to
security, that the audit did not look for and that no test covered — the guest
rootfs build was broken for anyone whose mirror served a full index.


## All five components remediated — 2026-08-29

| repo | branch | build evidence |
|---|---|---|
| nvkvm-pv | `fix/guest-lifetime` | real `.ko`, warnings identical to `main` |
| nvkvm-pv | `fix/isolate-boundary` | real `.o` with production flags, stub links, unit suites pass |
| nvkvm-pv | `fix/vmm-lifetime` | front-end compile only (installed QEMU tree is stale 9.2.0) |
| nvkvm-pv | `fix/broker-work-bounds` | builds all backends, `make check` + ASan/UBSan clean |
| nvkvm-pv | `fix/packaging-supply-chain` | CI tests + shellcheck + YAML parse |
| nvkvm-kata | `fix/kata-supply-chain` | **verified live** — real fetch, digest match, forced mismatch dies |
| nvkvm-steamos | `fix/steamos-warn` | `make check` 17/17 |

**Nothing merged. Nothing runtime-tested against a VM or GPU.**

### What is genuinely still open

1. **`KILL_ISOLATE` is unauthenticated** until the guest fills `reserved` and
   QEMU fails closed on zero. Protocol change across two components; needs a
   decision, not more code.
2. **BAR1 VA leak** — the original release blocker. Untouched: not root-caused,
   no fix on any branch.
3. Guest lock-ordering: the `ext_lock`/`mmap_lock` AB-BA deadlock and the false
   "no caller holds mmap_lock" invariant. Design changes, deliberately skipped.
4. VMM: `p->mode` un-latched on broker reconnect, letting both present paths run.
   State-machine change, deliberately skipped.
5. Broker: detach-inside-dispatch never reaching the exit check. Control-flow
   restructuring, deliberately skipped.

### A third correction to the audit, from the broker work

Finding 4 said a refused X11 grab leaves the title reading "GRABBED" **and the
cursor hidden**. Only the title half is real: `x11_show_cursor()` ignores its
argument and re-runs `x11_cursor_policy()`, which keys off whether a guest frame
is up, not off the grab state. The fix restores both anyway, for symmetry with
the ungrab path.

### The bound values are reasoned, not measured

The broker's new limits — 4 X round trips and 64 MB of copy per wakeup — are
derived from the 8192-dimension cap and documented honest-client rates, **not**
measured against a real VMM. Whether a legitimate client ever imports more than
four new buffers in one wakeup is unmeasured; the cost if it does is dropped
frames recovered within the 100 ms pacing watchdog, not a stall. Worth
confirming on hardware before this is treated as tuned.


## CORRECTION 2026-08-29 — two isolate findings were over-rated

Challenged by the owner and checked in the code. **The audit was wrong about
reachability on two of its five isolate criticals**, and an inflated critical
gating a release costs more than a missed minor.

### What the audit assumed, and why it was wrong

It rated `KILL_ISOLATE` and `CLOSE_HANDLE_ON_ISOLATE` as *guest*-reachable
because their handlers take an id straight off the wire. That is true of the
handler and irrelevant to the threat, because **guest userspace never composes
those messages**:

- `nvkvm_virtio_kill_isolate()` has exactly one caller,
  `nvkvm_session.c:136`, on the session-teardown path. The id comes from
  `session->isolate_id` of the session being destroyed — reached only via
  `nvkvm_session_put(ctx->session)` (`nvkvm_main.c:744`, `:805`), i.e. the fd's
  *own* context, and only when the refcount hits zero on last close.
- `nvkvm_virtio_close_handle_on_isolate()` is called with `ctx->handle_id`
  (`nvkvm_main.c:759`) — again the fd's own handle.

An unprivileged guest process therefore cannot name another isolate. It can only
cause its own session's isolate to be torn down, by closing its own fd.

And the blast radius is one VM: **cross-VM is always cross-process here**, so
another VM is another VMM process with its own `t->isolates` table. The
"VM-global id space" the audit flagged is global *within one VM*, which is the
VM's own property.

**Re-rated: `KILL_ISOLATE` critical → low.** It needs a compromised guest
*kernel*, and what it can then destroy is its own VM's isolates — self-DoS.
`CLOSE_HANDLE_ON_ISOLATE` likewise, kept slightly higher only because it moves
host-side refcounts.

The fixes on `fix/isolate-boundary` stay: they are correct defence-in-depth and
cost nothing. But **the `KILL_ISOLATE` protocol decision is no longer urgent**
and should not gate the release.

### What this does NOT rescue

The ring `_IOC_SIZE` stack smash **remains critical**, and the same check is why:
`nvkvm_session_ring_try(ctx, cmd, ...)` is called from the ioctl path
(`nvkvm_main.c:2849`) with the **userspace-supplied ioctl command number**. So an
unprivileged guest process picks `cmd`, `_IOC_SIZE(cmd)` rides it into the ring,
and the stub reads up to 16 KB into a 256-byte stack buffer **in a host
process**. Userspace-reachable, guest→host, unchanged.

### Still to re-rate by the same test

`POLL`/`UNPOLL_ON_ISOLATE`, the sparse-GPA leak and the reader-wake bug were all
rated on handler reachability. Each needs the same question asked — *can guest
userspace compose this message, or only the guest kernel?* — before its severity
is trusted. The reader-wake bug likely stands regardless: it wedges QEMU's shared
aio pool, which is host state.

### The lesson for the next audit round

Every auditor was told the guest is hostile, and each correctly traced its
component's handler. None traced **backwards into the guest module** to ask who
can actually compose the request. That is the question that separates
"unprivileged guest process" from "compromised guest kernel", and the two
deserve different severities. Put it in the next audit brief.
