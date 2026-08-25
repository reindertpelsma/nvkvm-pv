# Full security, reliability and race audit — 2026-08-25

**Revision audited:** `ce4200121639058b2efed158e5b217722b08a5ea`
(`nvkvm-pv` `main` after the request-completion consolidation).

**Scope:** the guest module, QEMU device and isolate handlers, isolate/stub,
shared protocol and ABI descriptions, scripts and CI, and the new display
broker including its Wayland, X11 and test backends. This is a source and
lifecycle audit under [`SECURITY.md`](../../SECURITY.md), plus compilation,
unit, sanitizer and adversarial broker tests. The physical GPU host was off, so
no GPU or SteamOS end-to-end test is claimed here.

This report does not silently supersede the earlier audits. The inherited open
items are reconciled near the end; findings prefixed `SR-`, `RR-`, `BR-` and
`CI-` below are new in this pass.

---

## Executive verdict

No new path to host code execution was established. The audit did, however,
find four high-severity boundary failures:

1. A split or inherited managed-fallback VMA can free its shared backing while
   another VMA still maps it. This is both a guest-kernel use-after-free and,
   after the finite GPA quarantine is drained, a stale-GPA cross-process alias.
2. The managed-fallback UUID collector walks a list under a different mutex
   from the writer. A guest process can race the guest kernel into traversing a
   partially linked node.
3. Several UVM ioctls allocate persistent guest-kernel bookkeeping before the
   host accepts the operation, with no cap, deduplication or rollback.
4. The broker's `--no-peercred` safety check is based on a configured pathname
   mode that is irrelevant for an adopted descriptor. The broker accepts an
   adopted TCP listener and then accepts every peer on it.

There are also three high-priority lifecycle/reliability defects: DRM hotplug
is invoked from the virtqueue callback although the DRM helper requires process
context; KMS state and its workqueue can outlive the DRM device; and QEMU's
display-relay disconnect path races between the main loop and a worker and can
schedule two closes for one descriptor.

The tree should not be described as ready for mutually untrusted production
tenants. That is consistent with `SECURITY.md`; these findings make some of the
remaining reasons concrete.

| ID | Severity | Boundary / effect | Result |
|---|---:|---|---|
| SR-01 | **high** | guest process → guest kernel / another guest process | managed VMA split/fork UAF and stale GPA alias |
| SR-02 | **high** | guest process → guest kernel | `registered_gpus` list race under mismatched mutexes |
| SR-03 | **high** | guest process → guest kernel | unbounded persistent UVM state allocations |
| SR-04 | **high, conditional** | unauthorized peer → privileged broker | adopted socket bypasses the premise of `--no-peercred` |
| RR-01 | **high** | guest kernel reliability | process-context DRM helper called from virtqueue callback |
| RR-02 | **high** | guest kernel reliability | KMS global/workqueue outlive DRM teardown |
| RR-03 | **high** | VMM reliability | display-relay disconnect data race and double-close |
| RR-04 | **high compatibility** | NVIDIA ABI profiles 515–535 | managed fallback rejects every pre-545 allocation layout |
| RR-05 | **high build regression** | compute-only guest module | every kernel-matrix `graphics=0` build fails to link |
| RR-06..08 | medium | display relay | broker-down fallback and reconnect/clipboard state defects |
| BR-01..06 | medium/low | privileged broker | cross-client clipboard state and input/backend correctness |
| CI-01 | medium | regression prevention | broker has no blocking CI job |

---

## Method and trust model

The VMM and host kernel are trusted by the main nvkvm model; the guest kernel,
guest processes and isolate are not. Guest-process separation inside one guest
is preserved rather than dismissed. The broker is assessed against its
stronger local promise in `src/broker/README.md`: its VMM-side client is hostile
while the broker owns the compositor connection, keyboard grab and host
clipboard integration.

The audit used four passes:

1. Trace lifetimes and ownership through open/close, fork/split, remove,
   disconnect/reconnect and persistent-client transitions.
2. Compare every reader's synchronization with every writer's synchronization.
3. Follow untrusted counts, pointers, handles, command types and descriptors to
   allocations and privileged APIs.
4. Build and run the available tests with normal compilers, sanitizers and a
   live malicious broker client. Negative results are listed separately from
   untested hardware behavior.

---

## New security findings

### SR-01 — HIGH — managed-fallback VMA split/fork frees live backing

**Code:** `src/guest/nvkvm_mmap.c:113-158`,
`src/guest/nvkvm_uvm_ext.c:424-428,602-606`,
`src/qemu/nvkvm_isolate_handlers.c:4190-4227`,
`src/qemu/nvkvm_mmap_host.c:724-736`.

`nvkvm_vm_ops.open` is installed but `nvkvm_vma_open()` does nothing. Every VMA
copy therefore shares the same `vm_private_data` pointer without acquiring a
reference. The first `close` of an `ext_backed` region removes the object from
the fd list, tears down the entire isolate/QEMU mapping and frees the region.

That assumption is incompatible with the kernel VMA contract. Linux invokes
`.open` when a VMA is copied or split and invokes `.close` for each resulting
VMA; the upstream contract is documented in
[`struct vm_operations_struct`](https://github.com/torvalds/linux/blob/master/include/linux/mm.h).
A guest process can reach the mismatch with `fork()` or a partial `munmap()`.

Consequences:

- The surviving VMA retains a dangling `vm_private_data`; its later close
  dereferences freed guest-kernel memory.
- Its existing PTEs still name the old GPA after QEMU restores anonymous
  backing and releases that extent.
- The GPA quarantine is deliberately finite: 64 extents / 64 MiB. A process
  can drain it. When the allocator gives the GPA to another process's mapping,
  the stale PTE aliases that mapping. This reopens the same cross-process class
  the quarantine was intended to make non-deterministic.

`VM_DONTEXPAND` prevents expansion; it does not prevent fork inheritance or all
VMA splits. `VM_DONTCOPY` is not set.

**Required correction:** give `nvkvm_mmap_region` a real VMA reference count,
increment it from `.open`, and release the backing only after the last VMA
closes. Decide explicitly whether fork inheritance is supported; if not, add
`VM_DONTCOPY`, but retain split-safe accounting because partial unmap still
exists. Add fork plus partial-unmap tests that force GPA reuse beyond both
quarantine limits.

### SR-02 — HIGH — `registered_gpus` writer and reader use different locks

**Code:** `src/guest/nvkvm_main.c:1317-1327`,
`src/guest/nvkvm_uvm_ext.c:437-453,491`.

`UVM_REGISTER_GPU` appends to `st->registered_gpus` while holding `st->lock`.
`ext_collect_uuids()` traverses the same list while its caller holds only
`st->ext_lock`. The same fd can service an ioctl and an mmap concurrently, so
the two paths are concurrent. `list_add_tail()` changes multiple links; a
reader may observe an incompletely linked node and walk an invalid pointer.

This is a guest-process-triggered guest-kernel crash/corruption path, not merely
a stale-value race.

**Required correction:** snapshot or traverse the list under `st->lock`. If
the mmap synthesis must also hold `ext_lock`, document and enforce one lock
order (`ext_lock` then `lock`, or copy under `lock` before taking `ext_lock`).
Exercise concurrent register/mmap in KCSAN and lockdep builds.

### SR-03 — HIGH — pre-forward UVM recording is an unbounded kernel allocator

**Code:** `src/guest/nvkvm_main.c:1294-1405,773-803`.

Each accepted-shaped `UVM_REGISTER_GPU`, `UVM_REGISTER_GPU_VASPACE`,
`UVM_CREATE_RANGE_GROUP` and `UVM_ALLOC_SEMAPHORE_POOL` allocates a new persistent
list entry. Semaphore-pool entries also duplicate the parameter buffer. There
is no per-fd count limit, uniqueness check, corresponding removal, or rollback
when the forwarded host ioctl rejects the operation. Recording occurs before
forwarding, and entries remain until the fd context is destroyed.

An unprivileged guest process can repeat these ioctls and consume guest kernel
memory. Forwarded round trips limit throughput but do not bound total memory,
so this is still a reliable denial of service under the project's
guest-process → guest-kernel boundary.

**Required correction:** impose small, semantics-derived limits; replace
duplicates by key rather than appending; record only after host success or
rollback on failure; and remove state on the matching unregister/destroy
operations. Failed registrations need a regression test because they are the
cheapest amplification path.

### SR-04 — HIGH (conditional) — adopted sockets invalidate `--no-peercred`

**Code:** `src/broker/nvkvm_broker.c:1594-1618,1711-1727,2115-2140`.

Startup rejects `--no-peercred` only when the *configured*
`cfg.socket_mode` has other-write or group-write bits. For `--socket-fd` and
socket activation, that mode is neither read from nor applied to the adopted
socket. `nb_adopt_fd()` verifies only that the descriptor is a listening
socket; it does not require `AF_UNIX`, `SOCK_STREAM`, a pathname, or safe actual
ownership/mode. `nb_peer_allowed()` then returns true unconditionally.

This was verified dynamically at the audited revision: the broker test backend
was handed an `AF_INET` TCP listener through `--socket-fd` with the default
configured mode `0600` and `--no-peercred`. A TCP client received the 24-byte
`HELLO`; the broker logged uid/gid `4294967295` and pid 0 as accepted. The
configured `0600` protected nothing.

On a TCP listener SCM_RIGHTS frames are unavailable, but the endpoint still
accepts protocol commands and clipboard/control traffic. On a broadly
accessible adopted UNIX socket an unauthorized local peer also has descriptor
passing. This defeats the advertised authentication boundary around a process
that owns the host display and clipboard.

**Required correction:** adopted descriptors must be `AF_UNIX` +
`SOCK_STREAM`. With `--no-peercred`, require a named filesystem socket and
validate its *actual* owner and mode, or simply refuse `--no-peercred` for every
adopted/unnamed socket. Log actual, not configured, protection.

The shipped SteamOS Compose deployment is not affected: it creates a UNIX
socket itself and keeps SO_PEERCRED enabled.

---

## New guest and VMM reliability/race findings

### RR-01 — HIGH — DRM hotplug helper is called from the virtqueue callback

**Code:** `src/guest/nvkvm_virtio.c:469-489`,
`src/guest/nvkvm_kms.c:288-315`.

`NVKVM_EVT_TYPE_UI_INFO` directly calls `nvkvm_kms_set_host_size()`, which calls
`drm_kms_helper_hotplug_event()`. There is no workqueue boundary. The event path
is a virtqueue callback (the surrounding transport describes event delivery as
softirq-context), while the upstream DRM helper explicitly
[`must be called from process context with no mode-setting locks held`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/drm_probe_helper.c).

The helper can lock and allocate. Calling it from interrupt/softirq context can
sleep in atomic context, deadlock, or crash when a broker resize arrives.

**Required correction:** store the latest size atomically and queue one
coalescing work item. The work item performs the mode update and hotplug from
process context. Teardown must cancel/flush it before the DRM object disappears.

### RR-02 — HIGH — KMS teardown is disconnected from object lifetime

**Code:** `src/guest/nvkvm_kms.c:730-798`,
`src/guest/nvkvm_drm.c:1224-1234`, `src/guest/nvkvm_main.c:2824-2830`.

`nvkvm_kms_fini()` only clears the global head pointer and has no call site.
`nvkvm_drm_fini()` unregisters and puts the DRM device without calling it.
Device removal destroys DRM before virtqueues, so a late UI event can read the
published pointer after its drmm-managed object has been freed.

The ordered present workqueue allocated during KMS init is never destroyed.
Several later init failures also return without destroying it. Clearing the
global pointer alone would not be sufficient: a callback may already hold the
old pointer, and queued present work may still name drmm-owned data.

**Required correction:** add an explicit KMS shutdown sequence: stop new UI
events, unpublish with synchronization, cancel the timer, cancel/flush present
and resize work, destroy the workqueue, then release DRM. Register cleanup for
every post-workqueue init failure. Test repeated probe/remove under KASAN and
forced resize/present traffic.

Two independent `READ_ONCE`/`WRITE_ONCE` operations on width and height can
also expose a mixed pair. This is low severity once the callback is moved; use
one locked structure or packed value.

### RR-03 — HIGH — display-relay disconnect can double-close a reused fd

**Code:** `src/qemu/nvkvm_display_relay.c:78-88,238-280,282-403,678-713`.

The virtio worker holds `r->lock` while sending and may call `relay_drop()`.
The main-loop read callback calls `relay_drop()` without that lock. The drop
function reads and writes `sock`, dimensions and retry state and schedules a
deferred close of the copied descriptor.

Two callers can observe the same non-negative descriptor, both store `-1`, and
both schedule a close. After the first deferred close, the descriptor number
may be reused before the second callback, allowing the second callback to close
an unrelated QEMU fd. Independent unsynchronized accesses to `sock`, receive
state and reconnect state are also C data races.

**Required correction:** make the main loop the sole owner of connection state
and descriptor registration/closure. Worker failures should queue a generation-
tagged drop request. If a mutex design is retained, every access must obey it
and deferred closes must be idempotent by connection generation, not fd number.

### RR-04 — HIGH compatibility — managed fallback rejects profiles 515–535

**Code:** `src/guest/nvkvm_uvm_ext.c:462-488`,
`src/common/nvkvm_abi.h:116-180`.

The fallback casts the allocation buffer to
`nv_memory_allocation_params_v545` and rejects
`ap_size < sizeof(*ap)`. That type is 128 bytes. ABI profiles 515, 525 and 535
correctly advertise the measured pre-545 size of 120 bytes, so the guard
returns `-EINVAL` for every one of those profiles by construction. The fields
used by this synthesis are in the common prefix, but the code requires the
newest full struct.

This finding is **NVIDIA-driver-specific ABI logic**. No ABI code was modified
during this audit. A correction must use profile-aware field access and be
checked/generated against the corresponding OGKM tags; changing the cast or
guard globally without that tag matrix is not acceptable.

### RR-05 — HIGH build regression — compute-only module no longer links

**Code:** `src/guest/nvkvm_virtio.c:483-489`, `src/guest/Kbuild:19-26`,
`tests/kernel_matrix.sh`.

`NVKVM_GRAPHICS=0` omits `nvkvm_kms.o`, but `nvkvm_virtio.o` unconditionally
references `nvkvm_kms_set_host_size()`. Local reproduction on kernel 7.0 ends
at modpost with:

```
ERROR: modpost: "nvkvm_kms_set_host_size" [nvkvm-guest.ko] undefined!
```

The hosted matrix reproduced it on Ubuntu 22.04/24.04/25.04/26.04, Debian
12/13 and Fedora 42. Every `graphics=1` build passed; every `graphics=0` build
failed. This predates the request-completion change and is not caused by type
27.

The matrix's error extraction searches lowercase `error:` and therefore prints
only `make: Leaving directory` for this uppercase `ERROR:`. Fix the compile
guard or provide a compute-only no-op, and make the diagnostic grep
case-insensitive.

### RR-06 — MEDIUM — broker-down frame retention is unreachable

**Code:** `src/qemu/nvkvm_display_relay.c:91-101,162-167,282-320`,
`src/qemu/nvkvm_isolate_handlers.c:1733-1764`.

`nvkvm_display_relay_submit()` returns `false` immediately when `sock < 0`.
The later locked branch that says it retains the newest frame while disconnected
can therefore never execute. Its caller falls through to the legacy GL present
path even though broker mode says QEMU imports nothing; without GL it closes the
frame and reconnect has nothing to replay.

The initial socket-absent Compose startup is the ordinary trigger. Reject only
a missing relay or invalid dma-buf before the lock; consume and retain the fd
when enabled but disconnected.

### RR-07 — MEDIUM — reconnect inherits partial protocol state and ignores replay failure

**Code:** `src/qemu/nvkvm_display_relay.c:246-280,678-713,833-849,970-977`
and `src/qemu/nvkvm_display_relay.c:1000-1078`.

`relay_drop()` does not reset the packet accumulator (`rxlen`) or inbound
clipboard accumulator. A fresh connection consumes its `HELLO` into a local
object, then appends ordinary packets to any fragment left by the dead socket.
Clipboard chunks can similarly cross connection generations.

`relay_send_caps()` discards send errors. Replay logs attach/commit failure but
does not drop the new link or re-arm a retry, so a reconnect can remain blank
and capability-less until unrelated traffic arrives.

Reset all per-connection receive state when a generation ends. Treat fatal
replay/capability sends as a dropped connection; queue retryable sends rather
than declaring reconnect complete.

### RR-08 — MEDIUM — multi-packet clipboard sends have no transaction abort

**Code:** `src/qemu/nvkvm_display_relay.c:853-889`,
`src/broker/nvkvm_broker.c:1214-1260`.

QEMU sends each clipboard chunk independently on a nonblocking stream. If the
socket reaches `EAGAIN` after one or more chunks, QEMU abandons the transfer.
The broker ignores the `chunk` index and retains the prefix. The next transfer
is appended; its `LAST` may commit concatenated text to the host clipboard.

Validate chunk zero and monotonic indices, reset on a new transaction, and add
an explicit abort or connection generation. On the sender, queue the complete
bounded transaction or do not begin it.

---

## New broker findings

### BR-01 — MEDIUM confidentiality — clipboard state crosses persistent clients

**Code:** `src/broker/nvkvm_broker.c:336-440`,
`src/broker/nvkvm_broker.h:195-224`,
`src/broker/nb_session_wl.c:1198-1255,2834-2863`.

Attach resets the old framing, window and input fields, but not the newly added
command/clipboard rate windows, clipboard accumulator and notices,
`clip_want_paste`, `clip_held_key`, or `caps_seen`. Detach does not cancel an
in-flight Wayland selection fetch.

Under `--persist`, client B can therefore inherit client A's advertised agent,
partial guest clipboard, or held paste state. More seriously, a host clipboard
fetch explicitly triggered for A can complete after B attaches and deliver the
host selection to B. This requires clipboard consent/full mode and a timing
window around a user paste, so it is medium rather than high, but it is a real
cross-client confidentiality failure under the hostile-client model.

Reset every connection-scoped field in one helper used by attach and detach.
Tag asynchronous backend operations with a client generation and close/cancel
the selection pipe on detach.

### BR-02 — MEDIUM — asynchronous paste loses key-up and modifier ordering

**Code:** `src/broker/nvkvm_broker.c:532-567,698-712`.

The physical paste-trigger keydown is held without setting `key_down`. If its
physical keyup arrives before the asynchronous Wayland fetch completes, the
duplicate-edge check drops it because the guest key state is already false.
Completion later emits only keydown and marks it down. There is no future keyup,
so V/Insert remains stuck. Modifiers may already have been released, in which
case the final keydown does not paste at all.

Capture a coherent chord and replay balanced edges after the clipboard is
queued, or retain/release all relevant physical edges in order. Add a delayed
fetch test with keyup and modifier-up before completion.

### BR-03 — MEDIUM correctness — the 16 KiB clipboard contract is impossible

**Code:** `src/common/nvkvm_broker_proto.h:129-144`,
`src/broker/nvkvm_broker.c:722-764`,
`src/broker/nb_session_wl.c:1224-1255`.

The outbound ring has 512 entries and reserves four during whole-transfer
preflight. At 15 payload bytes per event it can accept at most 507 chunks from
an empty ring. The current formula also overcounts exact multiples. The largest
payload admitted from an otherwise empty ring is 7,604 bytes, not 16,384.

The protocol comment claiming 16 KiB is too small to fill the ring is false.
Wayland separately rejects exactly 16 KiB because reaching the nominal buffer
limit is treated as overflow. Either reduce and document the real bound or
stream/queue transfers without requiring the entire nominal maximum to fit the
event ring.

### BR-04 — MEDIUM correctness — rejected clipboard transfers poison later transfers

**Code:** `src/broker/nvkvm_broker.c:1214-1260`.

Focus and rate rejection set `clip_in_bad` and return before processing `LAST`,
so at least the next otherwise valid transfer is discarded. The size/chunk cap
is worse: once exceeded, every later chunk—including `LAST`—returns through the
cap branch, so the accumulator remains permanently wedged until connection
reset. The connection-reset omission in BR-01 can carry that state further.

Separate structural transaction advancement from policy. Always consume/reset
on `LAST`; reset on chunk zero; validate chunk order; and make every rejection
leave a defined next-transfer state.

### BR-05 — MEDIUM — advertised clipboard modes and backend behavior diverge

**Code:** `src/broker/nvkvm_broker.h:79`,
`src/broker/nvkvm_broker.c:837,888-899,2061`,
`src/broker/nb_session_x11.c:1075-1087`,
`src/broker/nb_session_test.c:232-244`.

X11 and test backends have no set/fetch clipboard operations, yet the core can
recognize a paste trigger and set `clip_want_paste`. Only the Wayland dispatch
loop consumes it, so on X11 the key may be swallowed indefinitely instead of
clipboard being cleanly reported unavailable.

`NB_CLIP_FULL` is parsed and named but nowhere has behavior distinct from
`consent`; automatic bidirectional synchronization is not implemented. Reject
unsupported modes per backend or provide explicit capability gates. Do not
advertise `full` until its semantics exist.

### BR-06 — LOW–MEDIUM — Wayland offer lifetime and event-loop blocking

**Code:** `src/broker/nb_session_wl.c:1048-1131,1198-1255`.

- Every `data_offer`, including drag-and-drop, is assigned as the clipboard
  offer before a `selection` event identifies it. A text drag can replace the
  actual selection source.
- Superseded and NULL-selection `wl_data_offer` proxies are not destroyed, so
  clipboard churn leaks client-side Wayland objects.
- `dsrc_send()` performs a blocking write to a requester-owned fd in the single
  display/input loop. The text cap bounds bytes but not blocking time.
- `wl_fetch_pump()` ignores failure from `nb_sink_send_clipboard()` and then
  releases the paste key, so ring pressure can paste stale guest content.

Track candidate offers separately until `selection`, destroy superseded
proxies, make source writes nonblocking, and release the paste key only with an
explicit success/failure behavior visible to the user.

### BR-07 — LOW hardening items

- `nb_tx_coalesce()` performs signed `int32_t` addition on unbounded accumulated
  relative deltas (`nvkvm_broker.c:201-204`), which can overflow with undefined
  C behavior. Use saturating or wider arithmetic.
- Return values from `PR_SET_DUMPABLE` and `PR_SET_NO_NEW_PRIVS` are ignored
  (`nvkvm_broker.c:1764-1767`). A hardening promise should fail closed or log a
  visible failure.
- `--allow-gid` compares only the primary gid in `SO_PEERCRED`, not
  supplementary group membership. Clarify this in the CLI documentation or
  implement the intended group authorization another way.

---

## CI and test reliability

### CI-01 — MEDIUM — the broker is absent from blocking CI

`.github/workflows/ci.yml:55-163` runs the core unit suite, QEMU syntax checks,
shellcheck and ABI parity, but never builds or tests `src/broker`. The broker
has a substantial 43-case selftest, 20-case UTF-8 suite and sanitizer-clean
build, yet regressions—including a recent stale-object failure—can merge without
running any of them.

Add a blocking broker job that starts from `make clean`, runs `make check`, and
periodically or on every push runs ASAN+UBSAN. Include at least one adopted-fd
authentication test and persistent-client lifecycle tests from this report.

The kernel matrix is valuable because it caught RR-05, but its error parser
should preserve the first case-insensitive `error`/`undefined` line so failures
are actionable from the summary.

---

## Inherited open and deliberately accepted items

These were rechecked for continued presence, but are not new discoveries. The
detailed analysis remains in
[`audit-reconcile-2026-08-24.md`](audit-reconcile-2026-08-24.md),
[`audit-guest-pointers.md`](audit-guest-pointers.md),
[`audit-prerelease-2026-08-21.md`](audit-prerelease-2026-08-21.md), and
[`audit-boundaries-2026-08-20.md`](audit-boundaries-2026-08-20.md).

| Existing ID | Current disposition |
|---|---|
| U-8 | open, medium: control `0x127` P2P capability nested output pointer |
| U-11 | open, medium: frontend nested pointers outside the generic rewrite |
| R-3 | open, medium: allocation-class gate does not cover the `nr 0x27` route |
| R-5 | open, medium: hClient gate omits five allowlisted frontend NRs |
| R-4 | open, low–medium: `REALIZE_UVM_MAPPING` is a secondary UVM route outside the normal schema |
| R-7 | open, low–medium: guest-supplied `h_client` in `REALIZE_UVM_MAPPING` bypasses H-3 |
| R-6 / R-8 | open, low: empty-population hClient gate and ring control path coverage |
| P-4 / BD-A-6 | partial: MUNMAP wire request still lacks a caller session anchor |
| P-10(b) | open: NVIDIA-handle prefault can take unhandled SIGBUS |
| P-10(c) | open by design: `loop_lock` spans a deadline-less ENTER_LOOP wait |
| P-14 | open, medium: ring setup still kills the isolate despite the graceful-degradation comment |
| A-20 | open: QEMU unrealize does not cancel/drain two async thread-pool jobs |
| A-16 | unbounded pinning accepted for now; one handle-after-lock-drop race remains undetermined |
| GP-A-2 | deliberately outside the present tenant model; intra-VM sharing is not a claimed boundary |

The request-completion/type-27 fix at this revision is not a culprit and did
not widen the NVIDIA control surface. Its signal counter and exhaustive enum
switch are nvkvm's own wire bookkeeping.

---

## Verification performed

| Check | Result |
|---|---|
| `bash tests/unit/run_tests.sh` | pass; all core and isolate expected cases |
| `bash tests/qemu_syntax_check.sh` | pass on default GCC and GCC 14 |
| `go test -count=1 ./...` in `tests/abi_parity` | pass |
| broker `make clean check` | pass: UTF-8 20/20, selftest 43/43 |
| broker ASAN+UBSAN check | pass: selftest 43/43 and UTF-8 suite |
| broker GCC `-fanalyzer` | no production warning; one test-client CMSG false positive |
| shellcheck at blocking severity | pass |
| adversarial adopted-socket check | fail as designed by SR-04: TCP peer received HELLO under `--no-peercred` |
| guest module, `NVKVM_GRAPHICS=1` | hosted kernel matrix passes every tested kernel |
| guest module, `NVKVM_GRAPHICS=0` | fails every hosted kernel and local 7.0 with undefined KMS symbol |
| hosted fast CI at `ce42001` | pass |

Sanitizer success does not clear kernel or QEMU concurrency findings: those
paths are outside the broker process. No result here asserts Vulkan, CUDA,
display-broker frame pacing, SteamOS boot, suspend/remove behavior, or GPU
correctness on hardware.

---

## Recommended repair order

1. Fix SR-01 before any multi-process managed-memory test; add forced GPA-reuse
   coverage.
2. Fix SR-02 and SR-03 together so UVM state has one lock/lifetime model and
   explicit bounds.
3. Move UI resize to process context and implement complete KMS teardown
   (RR-01/RR-02); then fix the compute-only build (RR-05).
4. Make display-relay connection state main-loop-owned and generation-tagged;
   this resolves RR-03 and gives RR-06..08 a sound base.
5. Close SR-04 before documenting adopted sockets as safe with
   `--no-peercred`.
6. Centralize broker per-client reset and clipboard transaction state, then add
   broker CI for persistent disconnects, delayed selection fetches and ring
   pressure.
7. Address the inherited U-8/U-11/R-3/R-5 pointer and authorization work before
   revisiting the stated tenant model.

No fix in this report should be treated as a reason to hand-allow new NVIDIA
control commands. The control allowlist was not changed during this audit.
