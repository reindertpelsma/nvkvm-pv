# Pre-release security & reliability audits

> **Read the status before the findings.** The list below is what the audits
> *found*, written in the present tense as the auditors wrote it. Most of it has
> since been fixed. Current state:
> **[8 of the 9 criticals are closed in code](#critical-status-8-of-9-closed--and-two-were-over-rated-see-the-correction-below)**,
> two of the remaining findings were later shown to be **over-rated**
> ([correction](#correction-2026-08-29--two-isolate-findings-were-over-rated)),
> and what is genuinely outstanding is listed under
> [What is genuinely still open](#what-is-genuinely-still-open). The unfixed
> items are named, not buried — see [`SECURITY.md`](../../SECURITY.md) for what
> that means for where you should run this.

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
2. **BAR1 VA leak** — **RESOLVED 2026-09-03, and it was not a leak.** With every
   host referrer dead, `va_capacity` reports MAXCONTIG 11340 vs CUDA_FREE 11370 —
   healthy, and higher than with the stack up. `status=0x23` is RM correctly
   declining to unmap for a dead guest client while live host referrers still
   hold the mapping. GPU containers return every byte on exit. See
   `docs/investigations/va-space-leak/FINDINGS.md` §31. The text below is kept
   for its measurements. Original wording: untouched: not root-caused,
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


## Second remediation pass — `close/vmm-guest-broker`, 2026-08-29

The VMM, guest and broker findings left open above, closed or explicitly not
closed. One branch, `close/vmm-guest-broker`, off
`integration/audit-2026-08-29`.

### The build gate got better, and that is the most reusable result here

`tests/qemu_syntax_check.sh` skips `virtio_nvgpu.c`, `virtio_nvgpu_pci.c` and
`nvkvm_mmap_host.c`, and — less obviously — its stubs do not define
`CONFIG_OPENGL`, so the **entire body of `nvkvm_present_egl.c` is preprocessed
away** and "ok" for that file means nothing at all. Every earlier VMM branch was
therefore reviewed rather than compiled on the display path.

The configured QEMU 11.1.1 tree at `/workspace/qemu-build-src` already has the
nvkvm sources patched in and a working `build.ninja`. Lifting the exact `ARGS`
line out of it and pointing `gcc -c` at the repo's own sources (repo headers
first, tree headers as fallback, output to `/tmp`) compiles any `src/qemu/*.c`
with production flags **without touching the shared tree**. All 11 translation
units now compile to real `.o` files: 0 errors, and the only warnings are the
pre-existing `nvkvm_isolate_uid.h` format-attribute suggestion. Worth wiring
into the syntax-check script as an optional second tier.

### Closed

| finding | what changed |
|---|---|
| VMM: `p->mode` un-latched on broker reconnect | The mode latch is now scoped to the present THREAD, not the connection. `relay_readback` still clears (that is the whole mirror fix); `mode` does not, once the present thread owns the EGL context, because a re-probe answering "GL" runs `nvkvm_present_gl()` on the main loop with no context current and concurrently with the present thread's use of `p->cache[]`. Also: `present_sync_relay_generation()` can only ever run with the relay ACTIVE, so clearing `mode` never helped either measured case. |
| VMM: BAR base accepted below the shm block | Refuse any firmware base below `gpa.ram_top`. A raw 128 GiB memslot placed under guest RAM shadows RAM, the 32-bit PCI hole, other BARs and the ROM. The shm/mmap overlap test was also rewritten without `base + sparse_size`, which wraps and then compares *below* `block_base`. |
| VMM: wrapping page round-up in the sparse allocator | `nvkvm_page_round_up()` returns 0 instead of wrapping, at alloc, free and release. **Unreachable from any current caller** — every one caps its length first — and the commit says so. |
| VMM: `nvkvm_tables.c` used a table index as a KVM memslot number | Takes a number from the `[64, 512)` pool `nvkvm_mmap_host.c` already carves out, and releases it on destroy. `.slot = s` would have re-pointed **guest RAM's memslot** at an 8-window memfd. No callers outside the unit test; fixed because dead code with a live landmine is how the next caller gets written. |
| VMM: used-ring length reported as `sizeof(out)` | All 16 sites pass `iov_from_buf()`'s return value. |
| VMM: unthrottled `error_report` per malformed request | Power-of-two backoff, running total on every line, no clock and no new dependency. |
| VMM: non-atomic present statistics | Only the two `dropped` counters actually cross threads; they are `qatomic_inc`/`qatomic_xchg` now. The rest are single-threaded *because* the mode-latch fix makes "never both paths in one run" true. |
| GUEST: `poll_armed` never cleared on failure | Cleared when the arm request fails. Same silent-permanent-fallback shape as the missing `tx_done` cases, which went unnoticed across three driver versions. |
| GUEST: `migrate_range` retypes any VMA | Admits only plain process memory (no `vm_file`, or shmem/ramfs) and refuses PFNMAP/IO/MIXEDMAP/HUGETLB. `get_user_pages_fast(FOLL_WRITE)` succeeds on a writable *file* mapping, so the pin loop was not the check anyone assumed. shmem is detected by superblock magic: the kernel does not export `vma_is_shmem()` to modules. |
| BROKER: detach inside dispatch never reached the exit check | One exit check at the bottom of the loop, with a sticky per-iteration flag so a replacement client accepted in the same poll cycle cannot swallow the loss. |
| BROKER: DRI3 1.0 stride truncated to `uint16_t` | Refused rather than truncated. 65535 bytes is a 16383-pixel row, twice this backend's maximum, so nothing displayable is turned away. |
| BROKER: `stride` unbounded independently of extent | Bounded against this buffer's own pixel row (twice it, plus a page), so a 1-pixel-wide buffer can no longer buy a 2 GiB `wl_shm` pool. |
| BROKER: the placeholder mapping is never dropped | It is now, after `wl_surface_commit()` — the ordering `wl_idle_make()` already argues for. The comment had claimed the drop for a long time. |
| BROKER: README vs `x11_open()` on clipboard modes | The README was the wrong one. X11 implements both directions and advertises both. |

### Half-closed, and the code says so at both ends

**The guest `ext_lock` / `mmap_lock` AB-BA.** The edge taken on *every* forwarded
ioctl is the VMA whitelist, and removing it costs nothing: **no code in QEMU or
the stub reads `vma_whitelist_nentries` or `vma_whitelist_slot`** — grep the
tree. It is `mmap_read_trylock()` now, sending an empty list on contention,
which is already an ordinary case.

Two narrower edges remain inside the same transaction, both documented at the
`ext_lock` acquisition: `cpu_pages_refresh() -> entry_live()` (`mmap_read_lock`,
and `get_user_pages_fast()` which takes it internally), and
`efault_resolve() -> cpu_page_migrate()` (`mmap_write_lock`). Neither can be a
trylock — one is *inside* GUP. Closing them means moving the blocking host round
trip out of `ext_lock` and re-establishing the mmap/ioctl mutual exclusion some
other way, which is a redesign of the UVM shadow transaction. **Needs an
owner's decision.**

### Not fixed, with the argument written into the code

**The false "no caller holds `mmap_lock`" invariant.** The comment enumerated
callers and missed one:

```
remove_vma()                    [mmap_write_lock HELD]
  -> nvkvm_vma_close()
  -> nvkvm_uvm_ext_release() -> ext_unwind()
  -> nvkvm_fd_ctx_put(ext_map_ctl) -> nvkvm_fd_ctx_destroy()
  -> nvkvm_cpu_pages_free() -> nvkvm_cpu_page_unmap_guest()
  -> mmap_write_lock(mm)        [SAME mm]
```

It does not fire only because that internal ctl context never accumulates
`cpu_pages`, and nothing enforces that. Every available fix is a design choice:
a trylock would have to skip the zap and reinstate the H-9 cross-process
aliasing; deferring the `ext_map_ctl` put adds a deferred-teardown lifetime to a
path whose whole point is that teardown must *not* be deferred; splitting the
ctl teardown changes the ordering `nvkvm_fd_ctx_destroy()` documents. The
comment now states the real path and the constraint — which is the part that was
actually dangerous, since it is the repo's own "comment describes the safe
behaviour, code does something else" class.

### Over-rated, in the same way the isolate criticals were

**Guest finding 12, `nvkvm_evt_deliver()` "matches on host-supplied ids with no
ownership check".** Both halves fail the backwards test:

- The ids are compared against `ctx->handle_id` and `ctx->session->isolate_id`,
  which are the **fd's own** values, set at open and never taken off the wire. A
  pair names exactly one handle in one isolate, so no fd can be reached with
  another fd's identifiers. **That match IS the ownership check.** What the host
  chooses is which of the guest's own fds to wake, not whose memory to touch.
- The "malicious VMM" in the premise is this guest's hypervisor. It owns the
  guest's RAM and its vCPUs. A spurious `poll` wakeup is not a capability it
  needs.

The one real gap is narrow and benign — an fd that never armed can be woken —
and it is deliberately left alone: dropping unarmed events would silently kill
the fast wakeup path if the host ever delivers one unsolicited, and that exact
failure mode already went unnoticed once. The reasoning is recorded above the
function so it is not re-raised.

### What builds, what ran, and what did not

- **VMM**: all 11 `src/qemu/*.c` compile to real `.o` with production flags
  against the configured QEMU 11.1.1 tree, 0 errors, warnings identical to
  baseline. `tests/qemu_syntax_check.sh` clean. **Never linked, never run.**
- **Guest**: real `nvkvm-guest.ko` against 7.0.0-29 headers from clean; same
  three pre-existing warnings, none new. `tests/guest_wiring_test.py` passes.
- **Broker**: builds clean; `make check` (82 checks + adopted-socket, clipboard,
  lifecycle) and `make check-sanitize` (ASan+UBSan) both pass. The ATTACH
  geometry guards are **compile-verified only** — the `--bad-*` cases live in
  `selftest.sh`, which needs a real display server.
- **Unit**: `make -C tests/unit check` from clean, 17/17 suites, 9 isolate cases,
  no known failures.
- **Nothing is runtime-tested.** No VM, no GPU, no compositor, no broker
  connection.
- One flake worth knowing about: `make -C src/broker check-sanitize` failed once
  with `ConnectionRefusedError` in `test_clipboard.py` and passed on two
  re-runs. It is a pre-existing harness race, not a code change — `nb_listen()`
  creates the socket path at `bind()` and then does `chmod`, `getgrnam` and
  `chown` before `listen()`, while the test only polls for the path to *exist*.
  ASan widens that window. Worth polling for a successful `connect()` instead.
