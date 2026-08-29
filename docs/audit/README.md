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
| `fix/isolate-boundary` | isolate criticals | in progress |
| `fix/vmm-lifetime` | VMM serious | in progress |
| `fix/broker-work-bounds` | broker serious | in progress |
| `fix/packaging-supply-chain` | remaining packaging critical + serious | in progress |

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
