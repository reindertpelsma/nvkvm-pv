# Audit: memory safety, isolation and liveness across all three boundaries

**Date:** 2026-08-20. **Method:** static analysis only — no GPU, no VM, no
execution. Six parallel passes (guest-process→guest-kernel, guest-kernel→VMM,
guest→isolate + isolate→VMM, mapping/buffer-sharing, concurrency, and the
seccomp/sandbox layer), each finding then re-verified against the source by the
coordinator before being recorded here.

**Scope note.** This complements
[`audit-guest-pointers.md`](audit-guest-pointers.md), which covered one
invariant (no guest pointer reaches the host driver) and left its own gap list
in §7. That document's final item — *"The seccomp filter itself. Assumed
effective; not read line-by-line. Since it is what bounds every severity rating
in this report, it should be the next thing audited"* — is discharged here (§6).

**Like its predecessor, this document names locations, not techniques.** It
contains no working bypass procedure. The source is public, so it offers an
attacker nothing the code does not; it exists so a reader can judge the
boundary honestly rather than take a claim on trust.

**Threat model** (as scoped by the maintainer). In scope: guest process →
isolate / guest kernel / VMM; guest kernel → isolate / VMM; isolate → VMM.
Explicitly **not** findings, as safe by construction: isolate → guest process,
guest kernel → guest process, VMM → anything.

---

## Status at a glance

| # | severity | class | direction | status |
|---|---|---|---|---|
| A-1 | **critical** | blocking-under-lock | guest process → VMM | **fixed** — every sync wait deadlined; stub services its socket every 64 records |
| A-2 | **critical** | lifetime-race | isolate → VMM | **fixed** — `SO_RCVTIMEO`; `shutdown()` before `pthread_join`, `close()` after |
| A-3 | **critical** | uaf / cross-process | guest process → guest process | **fixed** — PTEs zapped before the extent is released; host-side quarantine as backstop |
| A-4 | **high** | oob read (double-fetch) | guest process → guest kernel | **fixed** — writeback bounds the second-fetch size against the extension actually allocated |
| A-5 | **high** | missing-validation | guest process → isolate | ⚠️ **PARTIAL — re-rated 2026-08-24.** Only the *guest-side* half landed, and guest code is not a control. The host-side reorder was never done, and a type-`'d'` record still bypasses every `'F'`-keyed gate. **See R-1 in [`audit-reconcile-2026-08-24.md`](audit-reconcile-2026-08-24.md).** |
| A-6 | **high** | cross-isolate | guest process → isolate | **partial — re-rated 2026-08-24.** mmap/present check session+isolate; munmap now refuses a token the *named* isolate does not own, but `req->isolate_id` is guest-supplied with no session anchor on the wire — see the KNOWN GAP comment at `nvkvm_isolate_handlers.c:3979-3994` and P-4 |
| A-7 | **high** | lost-wakeup | guest process → VMM | **fixed** — ENTER_LOOP moved to its own slot; a real one-at-a-time gate on the rest |
| A-8 | ~~high~~ **medium** | blocking-under-lock | guest process → VMM | **partial** — deadlined; re-rated 2026-08-23, see §9 |
| A-9 | ~~high~~ **medium** | lifetime-race | guest process → guest kernel | **open** — mitigated indirectly; re-rated 2026-08-23, see §9 |
| A-10 | medium | cache-confusion | isolate → isolate (display) | **fixed** — cache key carries the owning isolate; invalidated on isolate death |
| A-11 | medium | fd-confusion | guest → isolate | **fixed** — the command was `0x3d06`; `53dc238` punts it (and `0x3d08`) off the ring and translates the fd. See §8 |
| A-12 | medium | oob-map | guest kernel → VMM | **fixed** — handles record their size; offset+length checked overflow-safe |
| A-17 | medium | allowlist-bypass | guest kernel → VMM | **fixed** — gates select a denied sentinel instead of being skipped |
| A-18 | medium | missing-validation | guest kernel → VMM | **partial — re-rated 2026-08-24.** Validated pre- and post-export, but the size check short-circuits when `lseek` returns ≤ 0 (`nvkvm_isolate_handlers.c:1530` — **not** `nvkvm_present_egl.c`, which the reconciliation mis-cited) — the residual this document already disclosed in §4 |
| A-19 | low | oob read | guest kernel → VMM | **fixed** — guard corrected to `>= 52` |
| A-21 | medium | missing ownership check | guest kernel → VMM | **fixed** — slot release gated on a per-slot live bit; fails closed. See §9 |
| A-13 | medium | blocking-under-lock | guest process → VMM | **fixed** — EXIT sent outside `iso->lock`, `MSG_DONTWAIT` |
| A-14 | medium | lock-order | guest kernel → VMM | **fixed** — the slot wait takes `iso->lock` for the snapshot and the re-check |
| A-15 | medium | sandbox surface | isolate → host | **fixed** — `clone3` removed from the allowlist |
| A-16 | low ×6 | refcount / leak / hardening | various | **five of six done, one undetermined — resolved 2026-08-24.** Fixed: UVM handle-id offset, migration/mmap leaks (with the one documented residual), transaction-id leak, `nvkvm_ring_has_work`. Open by choice: unbounded pinning (no `RLIMIT_MEMLOCK` anywhere in `src/`). CANNOT DETERMINE: handle across a lock drop — not re-traced. See §5 |

**Nothing found gives a guest arbitrary code execution on the host.** The two
memory-safety breaks that cross a trust boundary (A-3, A-5) are both contained
by the isolate: they corrupt within one guest process's own GPU context or one
isolate's address space, which is exactly the containment the isolate model was
built to provide, and it holds. The most serious *practical* exposure is
liveness: A-1 and A-2 let an unprivileged guest process hang the entire VMM
with no memory corruption at all.

**Status, 2026-08-20.** Fifteen of the nineteen were fixed and the fixes are
verified to build: QEMU compiles clean (all nvkvm objects, no new warnings), the
guest module builds and links, and the unit suite is byte-identical to a baseline
captured *before* any of this work — a baseline that mattered, because
`test_isolate` was already failing on the test host and would otherwise have been
blamed on these changes. None of it is runtime-tested against a GPU workload; a
60fps desktop run with 8 concurrent EGL clients is the obvious next gate.

**Update, 2026-08-21 (§8).** A-11 turned out to have been closed the same day by
`53dc238`, the NCCL cross-isolate work, which landed *after* this document was
written; A-14 is now fixed, and four of the six §5 items with it. That leaves
**A-9** open (the guest pump's uninterruptible wait — indirectly mitigated, since
a stub that stops answering is killed by the VMM deadline, which releases the
pump), **A-8** deliberately partial (the round-trip is deadlined, but the lock is
still held across it, because restructuring that is a threading change rather
than a fix), and two §5 items open by choice — see §5 and §8. Everything in §8
is build-verified only; none of it has been run against a GPU.

---

## 1. The liveness class — the most exposed surface (A-1, A-2, A-7, A-8, A-13)

This is the finding cluster that matters most, because it needs no bug in any
parser: it is purely structural, and the structure is documented in the code.

`virtio_nvgpu.c`'s header states the premise plainly:

> *"Currently we hold the QEMU BQL across dispatch for simplicity and will
> relax this later."*

Every synchronous VMM→stub round-trip is an **untimed** `pthread_cond_wait`.
There is no `pthread_cond_timedwait` anywhere in `nvkvm_isolate.c`, and the
`socketpair()` that carries the protocol is never given `SO_RCVTIMEO`. So the
VMM's liveness is unconditionally delegated to the stub's willingness to reply.

**A-1 (critical).** *Found independently by two of the six passes, which is worth recording: it is reachable from both the guest-process and the guest-kernel boundary.* The stub's `ring_consumer_loop` only reaches
`ring_loop_poll_socket()` when the ring is **empty** — a record present means
`continue`, unconditionally. A guest that keeps the ring continuously fed
therefore prevents the stub from ever servicing its control socket. Meanwhile
any of CLOSE_HANDLE / MMAP / MUNMAP / POLL / UNPOLL / COPY_HANDLE / SETUP_RING
dispatches **inline on the TX handler with the BQL held**, and blocks in the
untimed wait for a reply that cannot come. The BQL is then held indefinitely:
main loop, QMP, timers, every other device, and every vCPU at its next
BQL-taking exit. The guest needs no VM exits to keep feeding the ring, so the
stall is self-sustaining rather than self-limiting. Trigger: unprivileged guest
process.

**A-2 (critical).** The isolate reader thread blocks in an untimed `recv()` for
a blob whose length the *stub* declared. `nvkvm_isolate_kill` then `close()`s
the socket and `pthread_join`s that thread — from the TX handler, under the
BQL. `close()` does not wake a peer thread already inside `recv()` on the same
file description, so the join never returns. Trigger: compromised stub, which
the surrounding comments already treat as in-model.

**A-7 (high).** `struct nvkvm_isolate` carries a single `sync_done`/`sync_error`
slot documented as "one non-IOCTL command at a time", but `pthread_cond_wait`
releases `sync_lock` — so a second sender can enter while the first is parked.
A waiter's wakeup is then erased by the newcomer's `sync_done = false`, and
`sync_open_fd` / `sync_mmap_retval` / `sync_realize_*` / `sync_ring_probe` /
`sync_loop_head` have no correlation tag, so replies are delivered to the wrong
caller. Reachable whenever the ring is enabled, because ENTER_LOOP runs on the
thread pool while the other sync commands run on the TX thread. The existing
F-5 guard covers slot reuse *after kill*, not two live commands.

**A-8 (high).** `present_lock` / `xiso_lock` are held across the entire stub
round-trip, with untimed waits, inline on the TX handler under the BQL. The
code's own comment calls this "wedges the whole guest's GPU I/O"; that
understates it by one layer.

**A-13 (medium).** `nvkvm_isolate_kill` performs a blocking `send()` while
holding `iso->lock`, whose own contract comment says it is "Held briefly; never
during blocking I/O".

**Recommendation, in priority order.** (1) Give every sync wait a deadline —
`pthread_cond_timedwait` plus an alive check, treating expiry as isolate death.
This alone converts every finding in this section from "hangs the VMM" to
"kills one isolate", and it is a mechanical change. (2) Make the stub's ring
loop service its socket on a bounded cadence rather than only at ring-empty.
(3) Move the inline sync dispatches off the BQL-holding path. (1) and (2) are
small and independently valuable; (3) is the real fix the header comment
already anticipates.

---

## 2. The isolation class (A-3, A-6, A-10)

**A-3 (critical) — the guest frees a GPA extent it still maps.** The CPU-page
migration path converts an *anonymous* VMA to `VM_PFNMAP` and `remap_pfn_range`s
it onto window GPAs. On teardown, `nvkvm_cpu_pages_free` sends MUNMAP + CLOSE
and — for range entries — does nothing else: no `zap_vma_ptes`, no unmap. The
only zap call site in the tree is on the *install* path. QEMU then returns the
extent to a VM-global free list, and first-fit reuse hands the same GPA to the
next requester, whose mapping is installed at the same window VA. The first
process's surviving PTEs now read and write the second process's GPU or pinned
memory.

The design invariant that makes this safe elsewhere — "a VMA holds the file
open, so teardown runs after the last VMA" — is true for `/dev/nvidia*` mmaps
and **false** for this path, whose VMAs are ordinary anonymous memory and hold
no reference on the driver file. There is a second, deterministic route that
does not even require closing the fd: the liveness probe tests only a chunk's
first page, so overmapping that one page makes a multi-page entry look dead and
the reaper frees the whole extent under live PTEs.

Fix: zap the range before releasing the extent, and/or quarantine extents on
the QEMU free list until the guest has acknowledged teardown. This is the one
finding that breaks intra-VM isolation, which `tests/security/` otherwise
covers — a regression test belongs with the fix.

**A-6 (high) — mapping handlers lack the ownership checks their siblings have.**
`MUNMAP_ON_ISOLATE` frees by bare token index into a VM-global 8192-entry array
and never compares the entry's `isolate_id` against the requester's, which the
request itself carries. `MMAP_ON_ISOLATE` never checks the handle's
`session_id`. The adjacent handlers already do the stronger thing —
`nvkvm_req_present` checks `session_id`, and `nvkvm_req_xiso_import` checks
`session_has_isolate` for *both* pairings — so this is an omission, not a
policy. The design note arguing QEMU should not do intra-VM policy also states
QEMU's boundary is "cross-VM / host-process", and isolates *are* separate host
processes; the stronger reading is the one its neighbours implement. (This
sentence said "uid-separated host processes" when written. They are not — see
P-5 in [`audit-prerelease-2026-08-21.md`](audit-prerelease-2026-08-21.md),
where that is now a recorded decision. The argument here does not depend on it:
separate *processes* is enough to make this QEMU's boundary.) Note that A-6 composes with A-3: it aims the same recycle-under-
live-PTE primitive at a victim rather than at yourself.

**A-10 (medium) — the present import cache cannot tell isolates apart.**
`NvkvmPresent` is one per-device struct shared by every isolate, and the cache
key is the *stub's own* GEM handle — which starts at 1 in every isolate — plus
geometry the virtual head fixes at one mode and two modifiers for everyone. Two
isolates collide on key 1 with identical geometry as a matter of course. A
compositor exiting and another starting means the second one's frames hit the
first one's cached texture, and nothing invalidates on isolate death, so the
dead isolate's dma-buf and VRAM stay pinned for the life of the VM. Disclosure
is host-display-only, never back to the guest, which is why this is medium.
Fix: put the isolate/session id in the key and invalidate on teardown.

---

## 3. The parsing class (A-4, A-5, A-12)

**A-4 (high) — a double-fetch on the RM_CONTROL writeback path.**
`size_of_strings` is read from the kernel's own copy of the params struct to
size the allocation, and then **re-read from userspace** on the writeback path
after the host round-trip. The writeback guard bounds it against the ABI
maximum but never against the buffer that was actually allocated, so a
shrink-then-grow yields three copy-outs of up to 512 bytes each from an
allocation sized for far less — adjacent kernel slab memory delivered to an
unprivileged caller. The two sibling writeback paths in the same function carry
exactly the missing check, which is what makes this an oversight rather than a
design choice. Fix: one comparison against `aux_size`.

**A-5 (high) — the sanitizer and the size table disagree about ioctl type.**
`nvkvm_sanitize_ioctl_params` returns early for any `_IOC_TYPE != 'F'`, a gate
added for a good reason (bare UVM NRs collide with frontend NRs). But
`nvkvm_ioctl_param_size` dispatches on `_IOC_NR` **alone**, with no type check.
The result is asymmetric: the size table says "this is a frontend struct" while
the sanitizer says "not my type", so the struct is accepted and forwarded with
its embedded guest-VA pointer fields intact.

Host-side, the backstop does not fire. The `'d'` branch is taken first and
checks only the DRM allowlist; the frontend allowlist below it is itself
guarded by `_IOC_TYPE == 'F'` and is skipped; and the `else if (type != 'F') →
DENY` arm is never reached. The NR spaces overlap numerically, so a frontend NR
can be presented as an allowlisted DRM NR. The NVIDIA frontend dispatches on NR
and ignores type — a fact this file's own comment states.

Worth stating plainly: the comment at the top of that host-side chain describes
*this exact attack* and believes it is mitigated. The mitigation has a hole,
and the hole is branch ordering. Requires `nv->graphics`; compute-only VMs are
unaffected. Fix: make the guest's size table type-aware, and reorder the host
gate so the non-`'F'` deny is evaluated before the per-type allowlists.

> ⚠️ **RE-RATED PARTIAL, 2026-08-24. This was marked `fixed` and should not have
> been.** Of the two halves prescribed above, only the first landed:
> `nvkvm_ioctl_param_size` is now type-gated (`src/guest/nvkvm_ioctl.c:142-144`).
> That is **guest code**, and the companion audit's method statement excludes guest
> sanitisation as a security control — so the half that shipped is the half that
> does not count. The host-side reorder was never done: the chain at
> `src/qemu/nvkvm_isolate_handlers.c:2164 / 2189 / 2224` still evaluates `'d'`
> first, NVKMS second, and the non-`'F'` deny last.
>
> The consequence is live and is written up as **R-1** in
> [`audit-reconcile-2026-08-24.md`](audit-reconcile-2026-08-24.md): because *every*
> host-side sanitiser keys on `_IOC_TYPE == 'F'` and the guest supplies the type, a
> record with `_IOC_TYPE == 'd'` and `_IOC_NR == 0x41` passes the DRM allowlist,
> skips fourteen gates — including the `memset` that `audit-guest-pointers.md` §3
> calls the tree's **only** `ENFORCED` control, on the explicit claim that a guest
> cannot skip it — and reopens G-2. Reordering alone does **not** close it (`'d'` is
> a *recognised* type, so it never reaches the deny arm wherever that arm sits); the
> fix is a cross-check between `_IOC_TYPE(req->cmd)` and the target handle's
> `dev_id`, specified in R-1.3.
>
> Bounding it, and worth recording here because the bound is the good news: `0x4a`,
> `0x27`, `0x2b`, `0x2a` and `0x34` are **absent** from the DRM allowlist, so U-3,
> the A-1 OS-descriptor gate, the alloc-class allowlist, the control allowlist and
> the DUP gate are **not** reachable this way. The exposed set is exactly four NRs:
> `0x41`, `0x4f`, `0x54`, `0x57`.

**A-12 (medium) — a memory handle's size is never recorded**, so no
`(offset, length)` pair can be bounds-checked against the object. The existing
guards correctly bound *length* against the window, but "length fits in the
window" is a different question from "offset+length fits in the object", and
the second cannot be asked at all. QEMU then prefaults every page of the
mapping it just made, so an out-of-range mapping becomes a SIGBUS that kills
the VMM. Normal guests always pass offset 0 with the full size, which is why
this has never fired. Fix: store the size at handle creation and check it.

**A-17 (medium) — the default-deny gates fail *open* on a short declared size.**
The RM control-command gate is guarded by `req->param_size >= 12` and the
RM_ALLOC class gate by `>= 16`. A guest that declares a *shorter* size skips the
gate entirely — the allowlist function is never called — and the stub, which has
no control allowlist of its own on that path, zero-pads the buffer up to
`_IOC_SIZE` and forwards it to the driver. That is a clean bypass of the only
gate on the path.

What makes this unambiguous is that the *sibling* gates on the same path already
use the correct idiom: the NVKMS one defaults its discriminator to `0xffffffff`
when the buffer is too short, and the VID_HEAP one does the same — both are then
denied. These two were written as "read the field if it's there", which is the
fail-open shape. Practical damage is limited because the zero pad forces a null
inner-params pointer, so parameterised controls fail inside RM; it is a bypass of
the gate rather than a demonstrated capability. Fix: too short to contain the
discriminator must mean *reject*.

**A-18 (medium) — PRESENT geometry reaches QEMU's allocator unchecked.**
`nvkvm_req_present` validates `graphics`, the handle's session, and the device
type — then passes the guest's `width`/`height`/`pitch`/`format`/`modifier`
straight through. On the main loop these reach `qemu_console_resize()`, where the
`uint32_t` values convert to `int` and `qemu_create_displaysurface()` computes
`width * 4` as `int` and asserts on pixman failure: a huge or sign-flipped
dimension **aborts the whole QEMU process**. The handler's own comment says "this
is the host/cross-VM boundary, so we validate hard" — and it does, but entirely
about *which handle* may be exported; the geometry rides alongside as opaque
metadata. Note the existing `surface_width(ds) != (int)w` check in the present
path runs *after* the resize, i.e. after the allocation that aborts. The handler
already measures the real buffer with `lseek(SEEK_END)` and uses it only for
logging, so the bound is available at the point it is needed.

**A-19 (low) — an off-by-four on a diagnostic read.** A block guarded by
`param_size >= 48` reads a four-byte field *at* offset 48 out of an allocation of
exactly `param_size` bytes; the field sits at offset 48 of a 56-byte struct, so
the guard must be `>= 52`. The memcpy is unconditional — only the debug print
that follows it is gated. Under glibc a 48-byte request has 56 usable bytes so it
does not fault in practice, but it is a genuine out-of-bounds read and a
hardened allocator or ASAN will trap it into a VMM abort.

---

## 4. What was checked and found sound

Recorded because a negative result from a careful pass is worth as much as a
finding, and because these are the assumptions the rest of the design rests on.

- **The SPSC ring is not double-fetched.** This was the single highest-risk
  item going in. The stub copies the record header into a private stack struct
  before parsing, and every subsequent size check reads that private copy; the
  param and aux blobs are copied into private fixed buffers under
  already-validated bounds. Length and type fields are fetched exactly once in
  `nvkvm_ring_peek`, and every offset is bounded with the caller's **trusted**
  capacity rather than the peer-writable `r->size` (the FF-1 fix). The guest
  may scribble on the ring freely after the copy; nothing the stub acts on
  changes. The guest-side consumer does the same.
- **The ring's lost-wakeup claim holds.** Both halves of the documented
  exit-edge invariant are implemented: the consumer re-checks `has_work()` once
  more after deciding to exit, and the guest is level-triggered — it
  re-evaluates after every `enter_loop` return — which is strictly stronger
  than the edge-triggered protocol the header describes. All four interleavings
  were enumerated and are safe.
- **The seccomp BPF is correct.** Verified instruction by instruction: the
  `fcntl` macro's false-jump lands on the nr-reload rather than on `RET_ALLOW`
  (an off-by-one there would be a clean allowlist bypass); the no-exec macro's
  non-match path never clobbers the accumulator, so the following block's
  comparison is still valid; denying `PROT_EXEC` outright rather than W^X is
  the right call and the stated reasoning is sound; failure to apply is fatal;
  TSYNC's positive-return-means-failure trap is handled; `--no-seccomp` fails
  closed on a mangled argv.
- **The sandbox around it is real.** user/pid/net/ipc/uts/mount namespaces,
  `pivot_root` into a minimal tmpfs, capability bounding set dropped, ambient
  caps cleared, `setgroups: deny`, `PR_SET_DUMPABLE 0`, and the root remounted
  `RDONLY|NOSUID|NOEXEC` with the return checked.
- **The export/import brokers are correctly gated** — both `(isolate, handle)`
  pairings are verified, and the stub resolves every GEM handle in its own DRM
  file, so a forged pair fails closed.
- **No lock-order inversion** among the three documented VMM locks, and none in
  the guest beyond A-14. The guest's one nested pair is consistently ordered.
- **Refuted after investigation:** a suspected guest VMA use-after-free (the
  VMA holds `vm_file`, so release cannot run under a live VMA — and this is
  precisely the invariant that *fails* for A-3's anonymous VMAs); a suspected
  GPA recycle on stub crash (unexpected death leaks the extent rather than
  recycling it); integer overflow in the migration path (the wrap checks and
  the 2 GiB cap cover every construction attempted); session refcount
  resurrection (`idr_remove` is in the same critical section as the final drop).

---

## 5. Low-severity items

Status added 2026-08-21; the wording of each item is as first written.

- **fixed** — Handle-id capture/restore disagree on a struct offset for one UVM
  ioctl, so an internal handle_id is returned to userspace (information
  disclosure). *The ioctl is `UVM_MAP_EXTERNAL_ALLOCATION`, whose `rm_ctrl_fd`
  is version-variant; capture/restore now read it at the same profile offset
  the sanitizer writes.*
- **open, by choice** — Unbounded pinning: up to 2 GiB per ioctl with no
  `RLIMIT_MEMLOCK` or cgroup accounting, repeatable concurrently. *A resource
  policy to decide, not a defect to patch; left for a deliberate decision.*
- **fixed, with one residual** — Three leaks on migration/mmap error paths; one
  is an unbounded, guest-drivable exhaustion of the handle and mmap-token
  tables that no reap path reclaims. *Four contained leaks were found and
  closed, not three. The unbounded one is `nvkvm_cpu_page_migrate`'s
  MMAP_ON_ISOLATE failure: the handle already carries an isolate reference by
  then, and `nvkvm_handle_close()` refuses with `-EBUSY` while that is held —
  a return the guest does not check — so the entry survived until session
  teardown, and nothing reaps `nv->handles`. A FIFTH leak was found and
  deliberately left: `nvkvm_efault_resolve()`'s GPU-VMA branch discards the
  token its re-`MMAP_ON_ISOLATE` returns, and fixing it means moving the old
  token's release ahead of the new mapping and carrying the region pointer
  across an `mmap_read_lock` drop — restructuring, not a contained fix. It is
  bounded by `nvkvm_iso_mmap_reap_handle()` at handle close.*
- **open** — A handle is used across a lock drop with no reference held
  (PLAUSIBLE; needs ~65k allocations to line up). *Not re-traced; left as
  found rather than guessed at.*
- **fixed** — Two `-ENOMEM` early returns never free the transaction id they
  reserved; once the bitmap fills, every GPU operation guest-wide fails until
  reload. *Fourteen such returns across twelve call sites, not two. The
  correct idiom was already present at two of the twelve.*
- **fixed** — `nvkvm_ring_has_work` does a plain read of the peer-owned
  counter, violating the header's own producer/consumer split; and the ring
  pointer is loaded twice without `READ_ONCE` while teardown NULL-stores it.
  *Both counters are acquire-loaded now (the helper serves both roles, so
  either counter can be the peer's), and the pump loads the ring pointer once
  through `READ_ONCE`.*

## 6. Sandbox surface (A-15 and hardening)

> **STALE — all four items below are FIXED in code; reconciled 2026-08-24.** This
> section was never updated and it *contradicts its own status table*, which has
> recorded A-15 as fixed since 2026-08-21. "Doc says open, code says fixed" is the
> direction nobody re-checks, so it is recorded explicitly rather than quietly
> corrected:
>
> - **`clone3` (A-15)** — gone from the allowlist (`src/stub/nvkvm_stub.c:2940-2988`),
>   and the `apply_seccomp()` comment no longer claims to block "fork"
>   (`:3241-3246`, tagged F6-1). The table row was right; the prose below is not.
> - **`umount2(".", MNT_DETACH)`** — return now checked and fail-closed
>   (`src/qemu/nvkvm_isolate.c:357-358`, tagged R4-L2); the caller `_exit(126)`s.
> - **The `sock_filter[96]` array** — `EMIT` is bound-checked and sets a fail-closed
>   `overflow` flag (`src/stub/nvkvm_stub.c:2901-2907`, tagged F8-1); `apply_seccomp()`
>   refuses on `overflow || n <= 0`. ~57 of 96 slots used.
> - **`closefrom` fallback** — returns −1 rather than succeeding silently, and both
>   call sites `_exit(126)` (`src/qemu/nvkvm_isolate.c:1826`, `:1940`).
>
> The wording below is left as first written, for the record.

- **`clone3` remains in the seccomp allowlist but is never called after the
  filter is applied** — the only call site is the worker-spawn loop, which
  completes before `apply_seccomp()`. It is the one process-creation primitive
  left to a compromised stub, and there is no `RLIMIT_NPROC` or pids cgroup
  anywhere in the tree, so process count is unbounded (a PID namespace does not
  cap it). The `R2-L1` cleanup immediately below already removed exactly this
  class of vestigial entry; `clone3` was kept because "clone3 is used"
  conflates *used at startup* with *needed after the filter*. Fix: delete one
  line. Relatedly, the comment above `apply_seccomp()` claims the allowlist
  blocks "execve, ptrace, **fork**" — it does not, `clone3` is a superset of
  fork, and that claim should be corrected.
- **`umount2(".", MNT_DETACH)`'s return is unchecked.** If it fails the old
  host root stays overmounted and path resolution reaches the host filesystem.
  This is the same fail-open class the *sibling* call in the same function
  already fixed under `R4-L1`. It is probably backstopped incidentally by the
  checked remount that follows, but incidental is not designed.
- **The BPF filter is built into a fixed `sock_filter[96]` with an unchecked
  `n++`.** 59 instructions used, 37 headroom, no static assert — adding ~19
  more allowlist entries silently smashes a stack array in the function that
  builds the sandbox.

---

## 7. What this audit did not settle

- Whether the ring's punt list is a complete superset of every allowlisted
  control carrying an inner pointer. The slow path enumerates pointer-bearing
  families case-by-case; the ring replaces all of it with a hand-maintained
  four-entry list. Completeness could not be established from this tree.
  **Recommendation: derive the punt list mechanically from the same table the
  slow path special-cases, rather than maintaining it by hand.** This is the
  same structural argument the predecessor audit's §6.3 made for the pointer
  schema, and it applies here for the same reason.
- Whether the host NVIDIA driver independently bounds an oversized declared
  buffer size at GEM import, and whether its EGL importer bounds stride×height
  against the dma-buf extent. Not determinable without the closed driver.
- The `U-5`/`U-7`…`U-13` items from the predecessor audit remain open and were
  not re-examined here.

---

## 8. Follow-up, 2026-08-21 — what was closed, and what was not

Static analysis and build verification only. **Nothing below has been run
against a GPU.** `tests/validate.sh` needs one and could not be executed, so
every claim here is "the code now does X", never "X was observed working".

### A-11 — already fixed, by `53dc238`, and this document was right when written

The command was `NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD` (`0x3d06`). At
the time of writing, `ring_ctrl_must_punt()` punted `0x3d05` and nothing else in
that family, so `0x3d06` — which *was* in the control allowlist — satisfied the
punt check and executed inline on the ring, carrying a raw guest fd NUMBER into
a stub where it means something else. `0x3d08` was not a second instance
because it was not allowlisted, so the punt function's allowlist gate caught it;
"one control command" was exactly right.

`53dc238` (the NCCL SHM work, committed hours after this document) added both
`0x3d06` and `0x3d08` to the punt list, added the guest-side fd translation for
them, and excluded them from the ring at the caller via `have_import_fd`. Both
halves are now covered, and the punt list matches the guest's translation set:
`guest_fd_to_handle_id()` is called for exactly three inner control commands —
`0x3d05`, `0x3d06`, `0x3d08` — and all three are punted. Every other embedded-fd
translation in the tree is on a frontend or NVKMS ioctl, and the ring only
accepts `NV_ESC_RM_CONTROL`, so none of them can reach it.

One shape worth recording for the next reader: a record with `aux_size == 0`
returns "no inner params → trivially flat" from `ring_ctrl_must_punt()` *before*
the `0x3d0*` check, so a `0x3d06` with no inner params does still run inline.
That is not an instance of A-11 — with no inner params there is no embedded fd
to confuse, and the stub zeroes the params pointer on that path anyway.

**No code change was made for A-11.** §7's first open question — whether the
punt list is a complete superset of every allowlisted control carrying an inner
*pointer* — is untouched by this; only the fd-bearing subset was enumerated.

### A-14 — fixed

`nvkvm_iso_slot_wait()` snapshotted `iso->id` and re-checked `in_use`/`id`/
`alive` with no lock, under a comment arguing that a stale read only costs one
more slice. Those fields are documented in `nvkvm_isolate.h` as protected by
`iso->lock` and are written under it from three places, and the predicate is the
*only* exit for the ENTER_LOOP wait, which has no deadline at all. Both reads
now take `iso->lock`.

Not a lock-order change: `mtx → iso->lock` already existed, because
`nvkvm_iso_slot_wait_or_die()` calls `nvkvm_isolate_declare_dead()` with the slot
mutex held. The reverse order does not occur anywhere, and this helper is never
called with `mtx == &iso->lock`.

### §5 — four of six

See the annotated list in §5. The two left open are the pinning-accounting
policy question and the PLAUSIBLE handle-across-lock-drop, neither of which is a
contained code fix. A fifth migration/mmap leak was found during the work and is
recorded there as deliberately unfixed, with the reason.

### Left alone on purpose

- **A-8.** This document already calls the fix a threading change rather than a
  fix. That has not become less true the night before a release.
- **A-9.** Indirectly mitigated, and the direct fix is the same threading
  surgery.
- Everything in §6 and §7.

---

## 9. Re-rating and two later findings, 2026-08-23

### A-8 and A-9 were rated against code that has since changed

Both were **high** when written, and correctly so: the round-trip wait was an
*untimed* `pthread_cond_wait`, so an unprivileged guest process could park the
VMM forever. Three things have changed since, and together they make **medium**
the honest rating:

1. **The wait is deadlined** (`NVKVM_ISO_SYNC_TIMEOUT_MS`), so "forever" became
   a bounded stall ending in the isolate being killed.
2. **The ioctl hot path is not affected at all.** `NVKVM_REQ_IOCTL_ON_ISOLATE`
   is offloaded via `thread_pool_submit_aio` and the TX handler returns
   immediately — its own comment says running it inline "would block the single
   virtio TX thread, so a second concurrent guest process ... is starved". What
   remains inline under the BQL is the per-frame **display** round-trip
   (`NVKVM_REQ_PRESENT`, `NVKVM_REQ_XISO_IMPORT`), whose stub side is a single
   `DRM_IOCTL_PRIME_HANDLE_TO_FD` that does not wait on GPU work.
3. **Reaching it now requires guest `CAP_SYS_ADMIN`.** Present is driven by a
   flip on the virtual head, and the primary DRM node is gated (#124), so an
   unprivileged guest process cannot drive the path at all.

Reaching the deadline therefore needs a stub that has stopped answering — which
guest userspace cannot cause, since ioctls run on the stub's worker pool and a
faulting worker is caught. That is a broken or compromised stub, not an attack
primitive. Worst case is a bounded VM freeze followed by the isolate being
killed: availability only, no corruption, no escape.

What genuinely remains is the **throughput** ceiling the code already names:
`ISOLATE_REQ` calls the handler inline, and its comment reads *"TODO(perf):
offload to the thread pool like NVKVM_REQ_IOCTL_ON_ISOLATE if per-frame TX stall
matters for a high-fps desktop."* That is a performance item, not a security
one, and it should not be carried in this document at high severity.

### A-20 — `virtio_nvgpu_device_unrealize` has no cancel/drain (OPEN)

`src/qemu/virtio_nvgpu.c` submits work at two `thread_pool_submit_aio` sites
(the `IOCTL_ON_ISOLATE` offload and the ENTER_LOOP worker). `unrealize` does not
cancel or drain either, so device teardown with work in flight can complete
while a worker still holds `nv`, `vq` and the `VirtQueueElement` it was handed
ownership of. Use-after-free on teardown. Found by reading, not yet reproduced;
recorded here because it was previously written down nowhere at all.

### A-21 — `nvkvm_kvm_slot_release()` does not validate the slot (**FIXED**, 2026-08-24)

It does not check that the slot being released was ever allocated. Same
provenance as A-20: found by reading, recorded so it is not lost.

**What was missing, precisely.** The range check was already there —
`slot < NVKVM_KVM_SLOT_BASE || slot >= BASE + COUNT` returns early. What was
absent is any notion of *ownership*: a slot released twice, or a slot number
that was never handed out, was pushed onto `kvm_slot_free_stack` regardless.

**Why that is a boundary bug and not an accounting one.** The freelist then
holds N copies of one slot number, so the next N `nvkvm_kvm_slot_alloc()` calls
return the **same KVM memslot** to N unrelated mappings. Each calls
`KVM_SET_USER_MEMORY_REGION` on it with its own GPA and its own host VA, and
the last writer wins — so a guest physical range ends up backed by a host VA
belonging to a *different isolate's* device memory. That is cross-isolate
memory aliasing arranged entirely inside the slot allocator, underneath every
ownership gate above it (A-6's `session_has_isolate` checks, A-12's extent
checks). Teardown compounds it: whichever mapping unmaps first deletes the
memslot the other still believes it owns, and releases the number again.

**Fixed** with a per-slot live bit (`kvm_slot_live[]`, under `kvm_slot_lock`),
and both gates **fail closed** — reject and log, never clamp, never repair the
count. Same shape and same `nvkvm: DENY …(A-21)` logging as the A-1 and U-3
gates in `nvkvm_isolate_handlers.c`:

- **release, range** — already present, now *says so*. Silence is how a
  sentinel that should have been filtered (`NVKVM_IN_WINDOW_SLOT` is `-2`) or
  an uninitialised `kvm_slot` stays invisible.
- **release, ownership** — the gate this finding is about. A slot that is not
  currently live is refused and stays out of the freelist. Leaking one number
  out of 448 is bounded and observable; putting one in twice is not.
- **alloc, ownership** — belt and braces. If a duplicate ever reaches the
  freelist by a route this does not anticipate, the second hand-out is refused
  rather than silently aliased, and the duplicate is dropped rather than pushed
  back.
- `nvkvm_kvm_slot_rejects()` exposes the refusal count. Non-zero means somebody
  is double-releasing, which nothing could previously observe.

**One adjacent hazard fixed with it, and it was worse than A-21 itself.**
`kvm_remove_memory_region()` issued `KVM_SET_USER_MEMORY_REGION` with
`memory_size = 0` — which **deletes** the named memslot — before calling
release, and checked nothing. Every teardown path is written
`if (kvm_slot >= 0)`, and `struct nvkvm_mmap_region` is `g_new0`'d, so its
`kvm_slot` is **0** until `nvkvm_mmap_map_to_guest()` fills it in. Slot 0 is
guest RAM. The range check inside `release` would have caught the release but
not the ioctl, so the check now sits before the ioctl too. **Not reachable at
HEAD** — a region only reaches a session's mmap list after `map_to_guest` has
set a real slot or `-1` — so this is a latent hazard, recorded rather than
claimed as a live bug. It is the P-15 pattern from
[`audit-prerelease-2026-08-21.md`](audit-prerelease-2026-08-21.md) exactly: a
zero-initialised field with a `>= 0` sentinel test.

**Pinned by `tests/unit/test_kvm_slot.c`** (12 cases), and the property it
measures is deliberately *not* "release logs something" — a test that counted
refusals would pass against a build that logs and then aliases anyway, which is
the false pass the A-1 work hit twice. What it asserts is the invariant the
finding is actually about: **no two live allocations ever share a slot number,
for any sequence of releases, valid or not.** Every hostile-release case ends
by allocating and checking the results are pairwise distinct and in range.

The allocator's own code is **extracted from `nvkvm_mmap_host.c` at build
time** between `NVKVM_KVM_SLOT_POOL_BEGIN/_END` markers rather than copied into
the test — the same technique as `test_stub_ptr_sanitize.c`, for the same
reason. Lose the markers and the `.inc` is empty and the test fails to link.

**Proved to fail with the fix reverted.** Compiled against the pre-fix pool
verbatim, with only the two new symbols added so the difference under test is
the gate and not a compile error: **6/12, exit 1**, and the headline case reads

```
FAIL a double release cannot alias two allocations   released 64 twice, then got 64 and 64
```

which is the aliasing primitive, printed. With the fix: 12/12.

Whole suite green, zero failures — at the time this landed that read
84 named PASS + 618 allowlist + 17 sanitize + 12 slot; the current standing count is
in [`docs/howto/build.md`](../howto/build.md). Not runtime-tested against a GPU; the
change is confined to the slot bookkeeping and adds no ioctl.

**Verified at `65aa69f`**, after the `sec-easy-batch` merge: `kvm_slot_live[]` at
`src/qemu/nvkvm_mmap_host.c:961`, the alloc-side ownership gate at `:990-1008`, the
release-side range gate at `:1020-1035` and the ownership gate at `:1040-1055`,
`nvkvm_kvm_slot_rejects()` at `:1092`, and the pre-ioctl range check in
`kvm_remove_memory_region()` at `:1161-1177`. The extraction markers
`NVKVM_KVM_SLOT_POOL_BEGIN`/`_END` bracket `:943`–`:1100`.

*Reconciliation note.* The 2026-08-24 sweep read this finding as still open on `main`
and fixed only on the then-unmerged `sec-easy-batch` (`9daf4b6`). That branch has since
merged; the writeup above is the current state.

---

## 10. Reconciliation, 2026-08-24

A full two-sweep reconciliation of this document against `main` **as it stood at
`68a35c0`** is in
[`audit-reconcile-2026-08-24.md`](audit-reconcile-2026-08-24.md). It was merged
forward onto `65aa69f`, which had since taken `sec-easy-batch` (P-3, P-5, P-7, A-21,
U-13) and `stub-window` (U-9, U-14, `iso_mmap_translate`); where those merges closed a
finding the sweep recorded as open, **the post-merge status is the one carried below**
and the sweep's reading is kept as a dated note. Summary of what it changed here:

- **A-5** re-rated `fixed` → **PARTIAL**; the host-side half was never written, and
  the live consequence is R-1 (HIGH).
- **A-6**, **A-18** re-rated `fixed` → **partial**, each for a residual the code
  already documents.
- **A-16** resolved to five of six done, one (`handle across a lock drop`)
  undetermined.
- **§6** and the **A-15 table-vs-prose contradiction** resolved: all four items are
  fixed in code; the prose was stale.
- **A-21** confirmed open at `68a35c0`, fixed on the then-unmerged `sec-easy-batch`.
  **Now merged and verified fixed at `65aa69f`** — see the A-21 entry above.
- Everything else — A-1, A-2, A-3, A-4, A-7, A-8 (partial), A-9 (open), A-10, A-11,
  A-12, A-13, A-14, A-17, A-19, A-20 (open) — verified to match its recorded status.

### The numbering hazard, and the recommendation

This document's **A-1** and **A-2** are *different findings* from
[`audit-guest-pointers.md`](audit-guest-pointers.md)'s A-1 and A-2. Same ids, same
`A-` prefix, two documents, four findings. A reader who follows a cross-reference
without checking which file it points at will read the wrong finding, and both
documents cross-reference each other.

**Recommendation, carried forward from the reconciliation:** give each document its
own id prefix — **`BD-`** for this one (boundaries), **`GP-`** for
`audit-guest-pointers.md`, **`PR-`** for
[`audit-prerelease-2026-08-21.md`](audit-prerelease-2026-08-21.md).

**Deliberately not done yet, and this is not an oversight.** The existing ids appear
in commit messages and in code comments (`grep -rn 'A-21\|A-1 ' src/` finds them in
`nvkvm_mmap_host.c` and `nvkvm_isolate_handlers.c`), and a rename breaks every one of
those references without a mechanical way to fix history. The prefixes should be
adopted for **new** findings, with the old ids left alone. Until then, a bare `A-n`
in prose means *the A-n of the document it appears in*.

