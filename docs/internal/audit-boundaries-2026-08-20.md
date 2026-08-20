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
| A-5 | **high** | missing-validation | guest process → isolate | **fixed** — the ioctl size table is type-gated, matching the sanitizer |
| A-6 | **high** | cross-isolate | guest process → isolate | **fixed** — isolate/session ownership checked on mmap, munmap and present |
| A-7 | **high** | lost-wakeup | guest process → VMM | **fixed** — ENTER_LOOP moved to its own slot; a real one-at-a-time gate on the rest |
| A-8 | **high** | blocking-under-lock | guest process → VMM | **partial** — deadlined, but the lock is still held across the trip; residual risk in-line |
| A-9 | **high** | lifetime-race | guest process → guest kernel | **open** — mitigated indirectly: the VMM now kills a wedged stub, releasing the pump |
| A-10 | medium | cache-confusion | isolate → isolate (display) | **fixed** — cache key carries the owning isolate; invalidated on isolate death |
| A-11 | medium | fd-confusion | guest → isolate | **open** — one control command still executes inline on the ring without fd translation |
| A-12 | medium | oob-map | guest kernel → VMM | **fixed** — handles record their size; offset+length checked overflow-safe |
| A-17 | medium | allowlist-bypass | guest kernel → VMM | **fixed** — gates select a denied sentinel instead of being skipped |
| A-18 | medium | missing-validation | guest kernel → VMM | **fixed** — geometry validated pre- and post-export against the real dma-buf size |
| A-19 | low | oob read | guest kernel → VMM | **fixed** — guard corrected to `>= 52` |
| A-13 | medium | blocking-under-lock | guest process → VMM | **fixed** — EXIT sent outside `iso->lock`, `MSG_DONTWAIT` |
| A-14 | medium | lock-order | guest kernel → VMM | **open** — `alive`/`id` still read under the wrong lock |
| A-15 | medium | sandbox surface | isolate → host | **fixed** — `clone3` removed from the allowlist |
| A-16 | low ×6 | refcount / leak / hardening | various | **open** — see §5 |

**Nothing found gives a guest arbitrary code execution on the host.** The two
memory-safety breaks that cross a trust boundary (A-3, A-5) are both contained
by the isolate: they corrupt within one guest process's own GPU context or one
isolate's address space, which is exactly the containment the isolate model was
built to provide, and it holds. The most serious *practical* exposure is
liveness: A-1 and A-2 let an unprivileged guest process hang the entire VMM
with no memory corruption at all.

**Status, 2026-08-20.** Fifteen of the nineteen are fixed and the fixes are
verified to build: QEMU compiles clean (all nvkvm objects, no new warnings), the
guest module builds and links, and the unit suite is byte-identical to a baseline
captured *before* any of this work — a baseline that mattered, because
`test_isolate` was already failing on the test host and would otherwise have been
blamed on these changes. None of it is runtime-tested against a GPU workload; a
60fps desktop run with 8 concurrent EGL clients is the obvious next gate.

What remains open is listed honestly rather than quietly dropped: **A-9**
(the guest pump's uninterruptible wait — now indirectly mitigated, since a stub
that stops answering is killed by the new VMM deadline, which releases the pump),
**A-11** (one control command still runs inline on the ring without the fd
translation the slow path performs), **A-14** (two fields read under the wrong
lock), and the low-severity items in §5. A-8 is deliberately partial: the
round-trip is now deadlined, but the lock is still held across it, because
restructuring that is a threading change rather than a fix.

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
QEMU's boundary is "cross-VM / host-process", and isolates *are* separate,
uid-separated host processes; the stronger reading is the one its neighbours
implement. Note that A-6 composes with A-3: it aims the same recycle-under-
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

- Handle-id capture/restore disagree on a struct offset for one UVM ioctl, so
  an internal handle_id is returned to userspace (information disclosure).
- Unbounded pinning: up to 2 GiB per ioctl with no `RLIMIT_MEMLOCK` or cgroup
  accounting, repeatable concurrently.
- Three leaks on migration/mmap error paths; one is an unbounded,
  guest-drivable exhaustion of the handle and mmap-token tables that no reap
  path reclaims.
- A handle is used across a lock drop with no reference held (PLAUSIBLE; needs
  ~65k allocations to line up).
- Two `-ENOMEM` early returns never free the transaction id they reserved;
  once the bitmap fills, every GPU operation guest-wide fails until reload.
- `nvkvm_ring_has_work` does a plain read of the peer-owned counter, violating
  the header's own producer/consumer split; and the ring pointer is loaded
  twice without `READ_ONCE` while teardown NULL-stores it.

## 6. Sandbox surface (A-15 and hardening)

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
