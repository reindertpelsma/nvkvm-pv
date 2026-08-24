# Audit: guest pointers at the host boundary

## Status at a glance

Severities below are **as first assessed**. Nine findings have since been fixed;
the rest are open. Read this table before the detail.

| finding | severity as found | status |
|---|---|---|
| U-1 | re-rated MEDIUM | **fixed** — the ring path applies the same default-deny gate (`src/qemu/nvkvm_ctrl_allowlist.h`) |
| U-2 | CRITICAL | **fixed** — rewrite sites fail closed (`nvkvm_stub.c`, `U-2` markers) |
| U-3 | CRITICAL | **fixed** — default-deny gate on `NVOS32.function`, only `ALLOC_SIZE` allowed |
| U-4 | CRITICAL | **fixed** — inner-pointer marshalling fails closed |
| U-5 | HIGH | **fixed** — the declared size is clamped to the aux blob on both stub paths (`nvkvm_stub.c`, `clamp_inner_params_size`); pinned by `tests/unit/test_stub_ptr_sanitize.c` |
| U-6 | HIGH | **fixed** — per-handle UVM VA-range ownership table; `semaphoreAddress` zeroed unconditionally |
| U-7 | HIGH | **fixed** — `pRightsRequested` zeroed unconditionally on both paths (`nvkvm_frontend.c`, `zero_nvos64_rights` in `nvkvm_stub.c`) |
| U-8 | MEDIUM | **open** — blocked on the ogkm struct layout, see the note in its section |
| U-9 | MEDIUM | **fixed** — the isolate now reserves a `PROT_NONE` guest-mapping window before it maps anything else and refuses every mapping outside it (`stub_window_contains`); pinned by `tests/unit/test_stub_window.c` |
| U-10 | MEDIUM | **fixed** — offset 16 decided from the cmd, offset 32 zeroed (`nvkvm_stub.c`) |
| U-11 | MEDIUM | open |
| U-12 | MEDIUM | **fixed** — `szName` cleared host-side via `aux_clear_ptr` (`nvkvm_stub.c`) |
| U-13 | ~~UNKNOWN~~ **not exploitable** | **closed** — RM refuses the whole control if the pointer is non-NULL and the client is not kernel-privileged. Traced 2026-08-24, evidence below |
| U-14 | by design | **no longer an exception** — `NVOS02.pMemory` is rewritten host-side to the window address, so no guest pointer reaches the driver on this path either |
| A-1 | **CRITICAL** | **fixed** — `NV_ESC_RM_ALLOC_MEMORY` + `hClass 0x71` pinned arbitrary stub memory; now gated on host-installed ranges |
| A-2 | not a control | the missing session gate on `ioctl_on_isolate` — analysed below, deliberately not added |
| U-15 | HIGH | **closed by revert** — QEMU-side UVM `mmap()` at a guest-chosen host address; layout oracle. Existed only between `c8ea92d` and `2406a3c`. See below. |

A separate defect found while fixing U-6 — the host driver writing past a
`g_malloc` on every `REGISTER_CHANNEL` — is in the addendum at the end, and is
also fixed.

### U-15 — the guest chose a host mmap address (introduced and reverted the same night)

`c8ea92d` made QEMU map `/dev/nvidia-uvm` at an address the guest supplied
(`nvkvm_isolate_handlers.c:3427,3447` on that commit):

```c
void *want = (void *)(uintptr_t)req->offset;   /* GUEST-SUPPLIED */
real = mmap(want, len, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED_NOREPLACE, h->fd, (off_t)req->offset);
```

`req->offset` is `vma->vm_pgoff << PAGE_SHIFT` forwarded verbatim from the guest
(`src/guest/nvkvm_mmap.c:150`), so a malicious guest names any address it likes
by mmap'ing the device with an arbitrary `pgoff`. That is a guest pointer used
host-side, against the invariant this audit is scoped against.

**Correctly prevented: overwrite.** `MAP_FIXED_NOREPLACE` returns `EEXIST`
rather than replacing, and the `real != want` guard catches kernels that ignore
the flag and relocate. A guest could not land a mapping over QEMU's heap, a
memslot, the KVM fd or an isolate socket.

**Not prevented: probing.** Request an address, observe whether the range got a
real UVM mapping or the announced anonymous-window fallback, repeat. That is an
address-space layout oracle over the privileged QEMU process — the same
primitive class as U-7, which was rated HIGH precisely because a distinguishable
outcome gave a guest an address probe, and whose severity rested on being a
stepping stone to U-1/U-2/U-4. This one is cheaper and more reliable: the signal
is a logged, unambiguous per-range outcome rather than a status code.

**Sharpening of U-6 found while checking this.** The U-6 write-up above describes
the pageable fallthrough as UVM migrating *"the CALLER'S OWN anonymous pages"*.
The driver's actual test is broader. `uvm_api_range_type_check()`
(`uvm_policy.c:59-106`) takes the pageable branch whenever **no** UVM range
covers the interval, gated on `uvm_is_valid_vma_range(mm, base, length)` — and
that helper (`uvm_policy.c:39-57`) just walks the caller's mm with
`find_vma_intersection()` and returns true if the interval is spanned by any run
of VMAs. **It tests no property of the VMA at all**: the sparse window, an RM
sysmem mapping, a memslot's backing and a mapped library all qualify equally.
The qualifying condition is *any mapping in QEMU*, not *anonymous memory*, and
the non-anonymous targets are the more interesting ones. U-6's containment check
blocks all of it, so nothing is open — but the control has to stay
unconditional, and the reason is broader than the original wording implies.

Two properties in the same function are worth recording because they corroborate
U-6's design rather than merely coexisting with it:

- **Full containment is the driver's own rule too.** A partially covered
  interval returns `UVM_API_RANGE_TYPE_INVALID`
  (`managed_range_last->va_range.node.end < last_address`), matching
  `uvm_va_covers()`'s deliberate refusal to stitch adjacent entries together.
- **A non-managed range is protective.** Any covering range makes
  `uvm_va_space_range_empty()` false, so the pageable branch is never reached;
  an external range over an address removes it from the "treat as the caller's
  CPU memory" path entirely.

**Status: closed.** `2406a3c` reverted the mapping, so `main` makes no UVM device
mmap and there is no guest-influenced host address left to probe. It is recorded
here rather than dropped because the functional gap that motivated it — managed
memory does not work without that mapping — is still open, and any future attempt
to close it must not reintroduce this. The measured reasons a VMM-chosen address
is *not* the way out are in
[UVM VA decoupling](uvm-va-decoupling.md).

### A-1 — an untrusted guest could pin and map arbitrary stub memory (CRITICAL, fixed)

`NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` (`hClass 0x71`) makes RM hand `pMemory`
straight to `os_lock_user_pages()` — `pin_user_pages()` **on the calling task**.
In the forwarded model the calling task is the stub, so an unvalidated `pMemory`
pins an arbitrary range of a host process's address space: its heap, its stack,
its libraries. `NV_ESC_RM_MAP_MEMORY` (nr `0x4e`) then maps that back to the
guest. The result is a read/write window into a process running as **the same
host uid as QEMU**, separated from it only by namespaces and seccomp.

The guest reaches it with one ioctl, `NV_ESC_RM_ALLOC_MEMORY` (nr `0x27`), and
needs nothing but a live `hClient` — which any legitimate prior allocation
provides.

**Why every existing gate missed it.** Four things lined up:

1. **The class allowlist does exclude `0x71`** — deliberately, following
   nvproxy (`nvkvm_fe_alloc_allowlist.h:12-15`). But it is applied under
   `if (nr == 0x2b)`, and `0x2b` is `NV_ESC_RM_ALLOC`, **a different ioctl**.
   Nr `0x27` is in the frontend NR allowlist and carries no class check at all.
2. **`nvkvm_dispatch.c`'s `p->p_memory = 0` looks exactly like the fix** and is
   dead code: its only caller, `handle_ioctl()`, had no callers of its own. It
   is easy to read that line and conclude the pointer is handled.
3. **The stub forwards it verbatim** — `nvkvm_stub.c` remaps only the embedded
   fd at offset 48; `p_memory` at offset 24 reaches `stub_ioctl` untouched.
4. **H-3's `hClient`-ownership gate applies but does not help** — it constrains
   *whose* client, never *which address*.

**The fix, and why it is not a default-deny.** Refusing `0x71` outright would
break U-14, which is a live feature (`cudaHostRegister` / host registration). So
the gate takes U-6's shape: an ownership test against ranges **the host itself
established in that isolate**. Every legitimate OS-descriptor range gets there
through the guest's page-migration path
(`src/guest/nvkvm_ioctl.c:409-440`), which installs it via `mmap_on_isolate` —
so QEMU already knows all of them, in `iso_mmap_tbl`, flagged `stub_mirrored`.
`iso_mmap_covers()` requires full containment in **one** such entry, deliberately
not a union (same reasoning as `uvm_va_covers()`).

**Updated by U-9 (2026-08-24).** The function is now `iso_mmap_translate()`: the chunks no longer
live at the guest's addresses, so covering the guest range is not enough — it also carries the
window-side end forward, refuses a step that is not window-adjacent to the previous one, and hands
back the translated base, which the gate writes into `NVOS02.pMemory`. The ownership property is
unchanged; what is added is that the answer is now an address rather than a yes/no, and that a
range which is contiguous in the guest but scattered in the isolate is refused rather than pinned
piecemeal. Anything else is refused
before `nvkvm_isolate_ioctl()`, never clamped, and logged the way the U-3 gate
logs.

One property worth stating: **the check does not depend on `req->session_id`
being truthful.** It is keyed on the isolate that will actually run the ioctl, so
a guest that forges an `isolate_id` is checked against that same isolate. There
is no ordering of guest-supplied values that makes the pin land somewhere the
host did not install.

Refusal is RM-shaped — the ioctl succeeds and `status` carries
`NV_ERR_INVALID_ADDRESS` — matching the alloc-class gate, because failing the
ioctl outright is a more severe signal than the real driver ever produces and
userspace treats it as fatal rather than falling back.

Also in this change: the dead `handle_ioctl()` is **removed** rather than left as
a decoy, and the `p_memory = 0` line that remains (the file is still built by
`tests/unit/test_dispatch.c`) is labelled as not being a control. A comment
asserting *"RM_ALLOC_MEMORY always allocates NV01_MEMORY_LOCAL_USER"* is
corrected — the guest chooses the class, and `0x71` is exactly the case; the
hardcoded `hClass = 0x40` was benign (it only selects whether to skip the
DUP_OBJECT grant) but it asserted something the code contradicted, which is the
species of false statement that hid this.

### A-2 — the missing session gate on `ioctl_on_isolate` (analysed, deliberately not added)

`nvkvm_req_ioctl_on_isolate()` takes `req->isolate_id` from the guest and passes
it straight to `nvkvm_isolate_ioctl()`, with no check that the isolate belongs to
the calling session — while the sibling `nvkvm_req_mmap_on_isolate()` has exactly
such a check (S-2). The asymmetry is real and was worth examining.

**It should not be closed by adding the check, for three reasons.**

- **The tree already argues against it, with a concrete regression.** The comment
  at the head of the handler states that a session-ownership check "would wrongly
  reject a handle that the guest LEGITIMATELY shared into another isolate via a
  guest-commanded `COPY_HANDLE_TO_ISOLATE` (e.g. CUDA IPC)".
- **It would be trivially bypassed.** `nvkvm_req_copy_handle_to_isolate()`
  performs *no* session validation of its own — it forwards `isolate_id` and
  `handle_id` to `nvkvm_isolate_send_handle()` directly. A guest blocked by a
  session gate on the ioctl path would simply copy the handle first.
- **It is intra-VM, and intra-VM is not this boundary.** `nv->handles` and
  `nv->isolates` are per-`VirtIONvgpu`, i.e. per-VM. Directing an ioctl into
  another isolate of the *same* VM crosses no trust boundary QEMU defends:
  per-process access control inside the guest is emulated by the guest module,
  which is untrusted but authoritative for guest policy. A malicious guest
  *kernel* is already assumed able to do anything a guest process could.

**Independently exploitable? No.** It grants no reach a guest does not already
have by design, and it does not widen A-1 — A-1's severity is that it escapes the
*isolate* into host process memory, not which isolate it lands in, and the A-1
gate is keyed on the isolate that runs the ioctl rather than on session identity.

Adding the check would give the appearance of a control while changing nothing an
attacker can do, and would break CUDA IPC. Recorded here rather than silently
left alone, so the asymmetry with S-2 is a documented decision instead of an
oversight waiting to be re-found.

**This document names locations, not techniques.** It deliberately contains no
working bypass procedure for anything still open. The source is public, so it
offers an attacker nothing the code does not; it exists so a reader can judge the
boundary honestly rather than take a claim on trust.


**Scope.** The architectural invariant *"no guest pointer is ever forwarded to the host NVIDIA
driver."* Static analysis only — no GPU, no VM, no execution. Every classification below cites
`file:line` in this tree, or in NVIDIA open-gpu-kernel-modules tag `575.51.03` where the question
is "does the driver actually dereference this field".

**Method.** Guest-side sanitisation (`nvkvm_sanitize_ioctl_params`, `src/guest/nvkvm_ioctl.c:253`)
is treated as *not a security control* throughout — it is guest code, and a malicious guest skips
it. A field is `ENFORCED` only if a host-side handler (QEMU or the isolate stub) **unconditionally**
overwrites it on a path that actually executes.

---

## 1. Is there a categorical mechanism?

**No.** There is one *near*-categorical mechanism, and it is conditional on a guest-supplied value,
which disqualifies it.

`src/stub/nvkvm_stub.c:901-921`:

```c
} else if (job.aux_size > 0 && job.param_size >= 24) {
        uint64_t aux_ptr = (uint64_t)(uintptr_t)job.aux_buf;
        __builtin_memcpy((char *)job.param_buf + 16, &aux_ptr, sizeof(uint64_t));
}
```

Offset 16 is where `NVOS54.params` (RM_CONTROL) and `NVOS21`/`NVOS64.pAllocParms` (RM_ALLOC) live,
so this single line covers the three highest-traffic ioctls. But:

- **`job.aux_size` is guest-controlled.** It arrives in `nvkvm_req_ioctl_on_isolate.aux_size`
  straight off the virtqueue (`src/qemu/virtio_nvgpu.c:900-922`), and the only host check anywhere
  is a `> 1 MiB` denial for RM_CONTROL (`src/qemu/nvkvm_isolate_handlers.c:1762`). A guest that
  sends `aux_size = 0` takes the `else` and **no branch writes anything** — `param_buf + 16` reaches
  `stub_ioctl()` at `src/stub/nvkvm_stub.c:1372` exactly as the guest wrote it.
- It only ever covers **offset 16**. Every pointer field at another offset
  (`NVOS64.pRightsRequested@24`, `NVOS02.pMemory@24`, `NVOS33.pLinearAddress@32`,
  `NVOS56.pNewCpuAddress@24`, DRM `event_nvkms_params_ptr@32`) is untouched by design.
- It says nothing about pointers *inside* the aux blob (the inner control/alloc params), which is
  where most of the pointer surface actually is.

I searched for the alternative — a schema of `(cmd, offset)` pointer descriptors, a "zero every
`NvP64` before forwarding" sweep, a generated rewrite table — and found none.
`nvkvm_uvm_schema[]` (`src/qemu/nvkvm_isolate_handlers.c:599`) is the closest thing in the tree, but
it describes **fd** fields (`fd_off[2]`), not pointer fields, and its comment says the fd translation
was deliberately *not* generalised (`:541-544`). Everything else is hand-written and per-ioctl:
roughly 51 rewrite sites in `nvkvm_stub.c`, each gated on its own set of guest-supplied values.

**So the prior finding is confirmed, and it is worse than "hand-written per-ioctl": several of the
hand-written sites are themselves bypassable by choosing the right guest-supplied count.**

---

## 2. What is live in `src/qemu/nvkvm_dispatch.c`

**Nothing.** Not "mostly dead" — dead.

`nvkvm_dispatch.c` exports exactly two symbols. Both have exactly one non-test call site, and both
call sites are inside an `#if 0` block:

| symbol | sole non-test caller | status |
|---|---|---|
| `nvkvm_ioctl_expected_param_size` (`nvkvm_dispatch.c:25`) | `virtio_nvgpu.c:402` | inside `#if 0` |
| `nvkvm_dispatch_ioctl` (`nvkvm_dispatch.c:159`) | `virtio_nvgpu.c:444` | inside `#if 0` |

The `#if 0` opens at `src/qemu/virtio_nvgpu.c:236` and closes at `:588`
(`#endif /* legacy NVKVM_REQ_OPEN/CLOSE/IOCTL/MMAP/MUNMAP handlers */`), with the header comment
at `:230-235` stating the enclosed handlers are "dead code as of Step 3d.1 (guest module no longer
sends these request types)". `handle_ioctl` at `:360` — the function containing line 444 — is
`static` and has no other reference.

The file is still compiled and linked (`scripts/build_qemu.sh:169`), which is why it looks alive.
So the reassuring comment at `nvkvm_dispatch.c:383-392` — *"the boundary (not the untrusted guest)
must ensure no guest pointer is ever forwarded"* — decorates code that never runs, and the
`IDLE_CHANNELS` overflow hardening beneath it (`:393-403`, the 64-bit math and the 4096 cap) is
likewise unreachable. The file's own note at `:375-379` is accurate and should be believed.

**Two consequences beyond the comment being misleading:**

1. **There is no ABI param-size validation on the live path.** The size table
   (`nvkvm_ioctl_expected_param_size`) was the only place that checked `param_size == sizeof(struct)`
   per command. The live path (`nvkvm_req_ioctl_on_isolate`) has `min_size` floors for UVM only
   (`nvkvm_isolate_handlers.c:1284`) and, for `'F'` ioctls, nothing but ad-hoc
   `req->param_size >= N` guards before individual field reads. A guest may declare any
   `param_size` up to `MAX_PARAM_SIZE` (`nvkvm_stub.c:443`) for any command.
2. **`tests/unit/test_dispatch.c` does not test what it appears to — it does not build at all.**
   It declares `extern size_t nvkvm_ioctl_expected_param_size(unsigned int cmd);` at `:98` — one
   parameter — against the two-parameter definition (`nvkvm_dispatch.c:25-26`), which it has
   already seen via `virtio_nvgpu.h:369` included at `:27`. That is a constraint violation, not
   merely undefined behaviour: the compiler rejects it with *conflicting types for
   `nvkvm_ioctl_expected_param_size`*. Since `test_dispatch` is the first target of
   `tests/unit/Makefile:33`, a plain `make` in `tests/unit` fails there and builds nothing.
   Verified 2026-08-18 with gcc 15.2. Even if it did build it would be exercising dead code.

   Three of the other six targets do not build either, for reasons unrelated to this one:
   `test_frontend` and `test_handle` fail to link on `nvkvm_debug_enabled`, which is defined only
   in `src/qemu/virtio_nvgpu.c:1095` and is in no unit-test source list; `test_isolate` needs
   `-D_GNU_SOURCE` for `CLONE_NEWUSER` (the Makefile passes it only to `test_tables` and
   `test_open_scm`) and then still fails to link on `nvkvm_debug_enabled`,
   `nvkvm_gpa_to_vmm_va`, `nvkvm_sparse_gpa_alloc`, `nvkvm_sparse_gpa_free` and
   `nvkvm_virtio_push_evt`. Only `mock_stub`, `test_tables` and `test_open_scm` build.

### The live path, for the record

```
guest virtqueue
  └─ nvkvm_tx_handler                            virtio_nvgpu.c:886   NVKVM_REQ_IOCTL_ON_ISOLATE
       └─ nvkvm_ioctl_work_fn                    virtio_nvgpu.c:626   (thread pool; SHM snapshot, P2-2)
            └─ nvkvm_req_ioctl_on_isolate        nvkvm_isolate_handlers.c:1031
                 ├─ UVM branch  → ioctl() IN QEMU'S OWN PROCESS      :1143
                 └─ everything else → nvkvm_isolate_ioctl            nvkvm_isolate.c:1789  (pure transport)
                      └─ stub reader → job queue → worker_thread     nvkvm_stub.c:834
                           └─ stub_ioctl(fd, cmd, param_buf)         nvkvm_stub.c:1372

guest-mapped SPSC ring (parallel, no QEMU involvement at all)
  └─ ring_consumer_loop                          nvkvm_stub.c:2447
       └─ ring_exec_one                          nvkvm_stub.c:2342
            └─ stub_ioctl(fd, rq.cmd, param)     nvkvm_stub.c:2399
```

The second path is the one that matters most and is discussed as **U-1**.

---

## 3. Classification counts

| surface | entries | `NO_POINTERS` | `ENFORCED` | `UNENFORCED` | `UNKNOWN` |
|---|---:|---:|---:|---:|---:|
| Frontend ioctl NRs | 23 | 15 | 1 | 7 | 0 |
| RM control cmds (allowlist) | 166 | 135 | 0 | 18 | 13 |
| RM control cmds (rule-based) | 2 rules | — | 0 | unbounded | — |
| RM_ALLOC classes | 89 | 81 | 0 | 5 | 3 |
| UVM schema rows | 31 | 16 | 0 | 15 | 0 |
| DRM ioctls | 14 | 11 | 0 | 3 | 0 |
| NVKMS inner cmdTypes | 7 | 6 | 0 | 0 (wrapper: 1) | 1 |
| Isolate control commands | 16 | 16 | 0 | 0 | 0 |

The 16 isolate control commands are the `case ISOLATE_CMD_*` arms of `stub_dispatch_cmd`.
`ISOLATE_CMD_IOCTL` is counted under the surfaces above, not here; the entry that used to be
`UNENFORCED` — `ISOLATE_CMD_MMAP`/`_MUNMAP` (U-9, counted once) — is now gated on the guest-mapping
window, so there are no unenforced arms left. The rest carry
only handle ids, uuids and scalars — including the `REALIZE_UVM_FD` state snapshot
(`src/stub/nvkvm_stub.c:2043-2057`), which is handles and UUIDs throughout.

Counts from the prior docs pass all verified: 166 control entries
(`grep -c` on `nvkvm_ctrl_allowlist.h`), 89 alloc classes, 23 frontend NRs, 31 UVM rows,
14 DRM, 7 NVKMS.

The NVKMS row moved from 6 to 7 on 2026-08-17 when `cmdType=60` was added
(`src/qemu/nvkvm_nvkms_allowlist.h:61`). It is counted `UNKNOWN`, not
`NO_POINTERS`: all that is measured about it is that the 595+/610 ICD issues it
once per offscreen context with a 32-byte params block, between REGISTER_SURFACE
and UNREGISTER_SURFACE. The `NvKmsIoctlCommand` enum that would name it ships in
no header, so whether those 32 bytes contain a pointer cannot be determined from
anything public. The same caveat has always applied to 61/62, which this table
classified `NO_POINTERS`; that classification was never better evidenced than
this one.

**`ENFORCED` total across the whole boundary: one.** `NV_ESC_RM_IDLE_CHANNELS`
(`src/stub/nvkvm_stub.c:1280-1283`) — an unconditional
`memset(param_buf + 12, 0, 28)` gated only on `_IOC_TYPE=='F' && _IOC_NR==0x41 && param_size >= 40`,
none of which a guest can use to skip it. That is what enforcement looks like, and it is the only
instance of it in the tree. (It exists because this exact hole was found on the live path after the
fact — see `nvkvm_stub.c:1269-1279`.)

The 7 `UNENFORCED` frontend NRs are `0x27`, `0x2a`, `0x2b`, `0x4a`, `0x4e`, `0x4f`, `0x5e`; the
single `ENFORCED` one is `0x41`. Frontend NRs deserve one credit: **`NV_ESC_IOCTL_XFER_CMD`
(`NV_IOCTL_BASE + 11` = 211 = `0xd3`) is correctly excluded.** That ioctl is
`{ u32 cmd; u32 size; NvP64 ptr; }` — a generic "here is a user pointer and a length, go copy it"
escape for oversized ioctls (ogkm `kernel-open/common/inc/nv-ioctl.h:40-52`). Had it been
allowlisted it would be a categorical arbitrary-pointer forwarding primitive that no amount of
per-struct rewriting could contain. The allowlist jumps `0xd2 → 0xd4` around it. This is what
default-deny is for and it worked here.

The 13 `UNKNOWN` control commands are IDs in `nvkvm_ctrl_allowlist.h` that do not appear anywhere in
open-gpu-kernel-modules 575.51.03, in any file, in any padding:
`0x0080028b 0x20800145 0x20800146 0x20808159 0x20808162 0x2080852e 0x2080852f 0x2080a612
0x2080a618 0x2080a0d1 0x20810107 0x20810108 0xc36f0101`.
These are presumably closed-source/GSP-side commands. Their params layout — and therefore whether
they carry pointers — cannot be determined from the open tree. They are allowlisted and forwarded.
Note `0x20808159`, `0x20808162`, `0x2080852e`, `0x2080852f` also match the
`cmd & 0x8000` rule-based passthrough, so they would be forwarded even if removed from the table.

The two rule-based passthroughs (`nvkvm_isolate_handlers.c:822-827`) —
`cmd & 0x8000` and `(cmd >> 16) == 0x2081` — admit an **unbounded** set of control commands, none
of which can be pointer-audited because the set is not enumerable. The comment justifying them
(`nvkvm_ctrl_allowlist.h:15-18`) asserts both are "GSP-routed, no app pointers". That assertion is
not verifiable from the open tree and is not verified here.

---

## 4. The `UNENFORCED` list, ranked

Severity accounts for the Phase 0 mitigation: the stub is unprivileged and
`clone()`d into fresh user/pid/net/ipc/uts namespaces with a seccomp allowlist
(`src/stub/nvkvm_stub.c:2610-2692`, `:2830-2845`), so a guest VA that reaches the driver corrupts
*within one isolate*. That bounds blast radius; it does not remove the bug. **U-6 is the exception —
it lands in QEMU, where the mitigation does not apply.**

---

### U-1 — FIXED 2026-08-19 — RE-RATED: MEDIUM — see maintainer note — The SPSC ring path forwarded `NVOS54.params` with no host mediation whatsoever

> **Fixed.** The bounded fix the maintainer note below prescribes is the one
> that landed: the default-deny control gate moved from
> `nvkvm_isolate_handlers.c` into `src/qemu/nvkvm_ctrl_allowlist.h`, next to the
> table it reads, and `ring_ctrl_must_punt()` in `src/stub/nvkvm_stub.c` now
> calls it. A command the gate refuses is **punted**, not answered in the stub,
> so the slow path makes the denial and reports it the way RM does — one policy,
> one decision point, no second copy to drift.
>
> The inner control cmd is now read before the `aux_size == 0` early return,
> which is what let a record with no inner params skip the block entirely.
>
> `tests/unit/test_ctrl_gate.c` pins the policy: every table entry allowed, both
> rule-based passthroughs allowed, unlisted commands denied. Measured on an RTX
> 3050 guest with `ring_enable=1`, `tests/validate.sh` is 28/28 and
> `opencl_correctness` passes — nothing legitimate was relying on the ungated
> path.


> **Maintainer note (re-rating).** This entry over-rated the allowlist's role. The
> ring bypasses the *control* allowlist, but object **allocation** still passes
> through QEMU, so a guest can only issue controls against handles it legitimately
> allocated — and RM's own per-client scoping is what bounds their effect. The
> reachable surface is unprivileged commands acting on the guest's own channel and
> context objects. The control allowlist is defence-in-depth here, not the primary
> control.
>
> This property is not specific to nvkvm: gVisor's `nvproxy` relies on the same RM
> client scoping. Treat the allowlist as hardening against RM bugs and against
> commands with effects beyond the calling client, not as the boundary itself.
>
> **Re-rated twice.** CRITICAL was too high: object allocation still passes through
> QEMU, so a guest can only issue controls against handles it legitimately
> allocated, and RM's per-client scoping bounds their effect. But LOW was too low,
> and this is the settled rating at MEDIUM. The ring is *ours* — nvkvm creates it
> for every isolate and MAP_FIXEDs it into guest-visible GPA — so the allowlist
> simply was never wired into that path. A defence-in-depth layer present on every
> path except one is an omission, not a trade-off, and the cost of the gap is
> precisely that a deny-listed command would skip its mitigation if RM ever had a
> bug in one.
>
> **The fix is bounded:** the allowlist tables already exist; the ring path does not
> consult them. Applying the same default-deny check stub-side closes it without
> new infrastructure. Deleting the ring is also defensible — it is off by default
> and measured no improvement to LLM decode, where control-RTT is only 1-2% of
> per-token time.
>
> The original analysis below is kept because the *mechanism* it documents is
> accurate and worth understanding.

**Field:** `NVOS54_PARAMETERS.params` (offset 16), plus `paramsSize` (offset 24).
**Driver dereferences it:** yes, `rmapiParamsAcquire` at
`src/nvidia/src/kernel/rmapi/control.c:262` with `PARAM_LOCATION_USER` — `copy_from_user` on entry
and `copy_to_user` on exit, `paramsSize` bytes.

`ring_exec_one` (`src/stub/nvkvm_stub.c:2342`) executes guest-authored ring records inline. The
pointer rewrite is at `:2293`:

```c
if (rq.aux_size) {
        uint64_t aux_ptr = (uint64_t)(uintptr_t)aux;
        __builtin_memcpy(param + 16, &aux_ptr, sizeof(aux_ptr));
}
```

and `ring_ctrl_must_punt` (`:2309`) explicitly **accepts** `aux_size == 0`:

```c
if (aux_size == 0)
        return 0;                 /* no inner params → trivially flat   */
```

So a record with `cmd = _IOWR('F', 0x2a, 32)`, `param_size = 32`, `aux_size = 0`, and
`param[16..23]` set to any 64-bit value is executed with that value intact.

**This path has no allowlist at all.** `grep -n allowlist src/stub/nvkvm_stub.c` returns only
seccomp comments. `nvkvm_ctrl_cmd_allowed`, `nvkvm_client_allow_has`, the `DUP_OBJECT` source gate,
and the `'F'`-type gate all live in `nvkvm_req_ioctl_on_isolate`, which the ring never enters. The
guest therefore reaches control commands QEMU denies, including
`NV83DE_CTRL_CMD_DEBUG_WRITE_MEMORY` (`0x83de0316`) and `NV83DE_CTRL_CMD_DEBUG_READ_MEMORY`
(`0x83de0315`) — which carry their own embedded `buffer` `NvP64` that the driver copies in *and*
out (`src/nvidia/src/kernel/rmapi/embedded_param_copy.c:435`, `:448`) — and
`NV2080_CTRL_CMD_GPU_EXEC_REG_OPS` (`0x20800122`, `:506`).

**The ring is not opt-in at the boundary.** `nvkvm_isolate_ring_setup` runs for every isolate unless
the *host* sets `NVKVM_RING_DISABLE` (`src/qemu/nvkvm_isolate.c:988`), and it `MAP_FIXED`s the ring
memfd into the guest-visible sparse GPA window (`:1330-1345`), handing the GPA to the guest via
`NVKVM_REQ_SETUP_RING` (`src/qemu/nvkvm_isolate_handlers.c:449`). The guest-side
`ring_enable` module parameter defaults to false (`src/guest/nvkvm_main.c:424`) — but that is guest
code, exactly the class of "control" this audit exists to reject. A malicious guest sets it, or
writes ring records directly.

The stub's own comment at `:2240-2242` states the trust model correctly — *"the request ring is
producer-writable by the (untrusted) guest … a malformed ring is fatal to THIS isolate only (DoS,
never OOB)"* — and the "never OOB" half is what fails: the record framing is validated, the
*contents* are not.

---

### U-2 — FIXED — fail-closed — `RM_CONTROL`, `RM_ALLOC` and the NVKMS wrapper forward their pointer verbatim when `aux_size == 0` (socket path)

**Fields:** `NVOS54.params@16`; `NVOS21.pAllocParms@16`; `NVOS64.pAllocParms@16`;
`NvKmsIoctlParams.address@8` (`src/common/nvkvm_proto.h:112`).
**Driver dereferences them:** yes for all three (control.c:262; `alloc_free.c` param copy;
NVKMS `copy_from_user` on `address`/`size`).

Same defect as U-1 on the socket path, at `src/stub/nvkvm_stub.c:901-921`. Both branches require
`job.aux_size > 0`. Nothing upstream forces it: `virtio_nvgpu.c:917-922` takes `aux_size` from the
guest and only nulls `aux_buf` if the slot lookup fails; `nvkvm_req_ioctl_on_isolate` never checks
it except for the 1 MiB cap.

The NVKMS case is the sharpest: `NvKmsIoctlParams` is 16 bytes, so `job.param_size >= 24` is false
and the second branch can never rescue it — with `aux_size == 0` the wrapper's `address` field is a
raw guest VA handed to `/dev/nvidia-modeset`, which is a **host-global** device. The 6-entry inner
`cmdType` allowlist (`nvkvm_nvkms_allowlist.h`) still applies (it reads `param_buf+0`), so the
reachable NVKMS surface stays small, but the pointer is not sanitised.

*(Aside, since the tree's comments have been wrong before: `nvkvm_nvkms_allowlist.h:19-21`
speculates that cmdTypes 61/62 are "query-class". They are `NVKMS_IOCTL_ENABLE_VBLANK_SEM_CONTROL`
and `NVKMS_IOCTL_DISABLE_VBLANK_SEM_CONTROL` — vblank semaphore-control registration, not queries.
Neither inner struct carries a pointer, so this does not change the classification, but the comment
invites a wrong conclusion and asks to be checked "before relying on this long-term".)*

---

### U-3 — FIXED — function gate — `NV_ESC_RM_VID_HEAP_CONTROL` with `function = NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR` pins a guest VA

**Field:** `NVOS32_PARAMETERS.data.AllocOsDesc.descriptor` (`NvP64`, inside the 144-byte union at
offset 40), with `data.AllocOsDesc.limit` as the length.
**Driver dereferences it:** yes, and hardest of anything in this report. `escape.c:450` dispatches
`NV_ESC_RM_VID_HEAP_CONTROL`; `:462` tests `pApi->function == NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR`
(value 27, `nvos.h:643`) and calls `RmCreateOsDescriptor` at `:463`, which at
`src/nvidia/arch/nvalloc/unix/src/escape.c:162`:

```c
rmStatus = os_lock_user_pages(pDescriptor, pageCount, &pPageArray, flags);
```

`os_lock_user_pages` is `pin_user_pages()`. `pageCount` is `(limit + 1) / PAGE_SIZE`, also guest
controlled. The only validation the driver performs is page alignment (`:142-146`) and a `limit+1`
overflow check (`:150`). On failure it falls through to `os_lookup_user_io_memory` (`:170`).

**Host-side coverage: none.** `nr == 0x4a` is allowlisted (`nvkvm_fe_alloc_allowlist.h`). The stub
has no case for it. The generic `+16` rewrite does not reach offset 40+. QEMU forwards it opaquely.

The guest declines to sanitise it on an explicit — and incorrect — premise
(`src/guest/nvkvm_ioctl.c:456-460`):

> `NVOS32`: the legacy `ALLOC_SIZE` path libGLX uses has no embedded input pointer (its `address`
> field is `[OUT]`-only), so there is nothing to translate. Forward opaquely.

That is true of `function == NVOS32_FUNCTION_ALLOC_SIZE` (2) and false of `function == 27`, and
nothing constrains `function` to 2. gVisor's nvproxy handles exactly this by rejecting every NVOS32
whose `Function != NVOS32_FUNCTION_ALLOC_SIZE`; nvkvm has no equivalent gate. `src/abi/nvgpu.h:311`
records the same premise (*"We forward NVOS32 opaquely, so only the fixed prefix matters here"*).

Two further union members carry pointers under other `function` values —
`data.HwAlloc.bindResultFunc` and `data.HwAlloc.pHandle` (`nvos.h:832-833`, reached under
`NVOS32_FUNCTION_HW_ALLOC` = 19). I could not establish from the open tree whether the Linux RM
path consumes `bindResultFunc`; it is listed here as a reason to gate `function` rather than to
enumerate offsets.

---

### U-4 — FIXED — fail-closed — the inner-pointer marshalling for all 15 handled control commands is bypassable by a guest-chosen count

**Fields:** `grInfoList` / `fbInfoList` / `busInfoList` / `biosInfoList` / `surfaceInfoList` /
`capsTbl` / `engineList` / `classList` (offset 8 of the inner params); the three
`GET_BUILD_VERSION` string pointers (offsets 8/16/24); `FIFO_GET_CHANNELLIST`'s two list pointers
(offsets 8/16).
**Driver dereferences them:** yes, and this is the authoritative list —
`src/nvidia/src/kernel/rmapi/embedded_param_copy.c` exists solely to walk them. `GR_GET_INFO`
(`:400-409`) is `RMAPI_PARAM_COPY_INIT(..., grInfoList, grInfoList, grInfoListSize,
sizeof(NV0080_CTRL_GR_INFO))` with **no** `SKIP_COPYIN` flag — copy in **and** out, i.e. an
arbitrary read *and* write of `grInfoListSize * 8` bytes at the supplied address.

The stub reconstructs these pointers at `src/stub/nvkvm_stub.c:986-1160`. The guard is `:1030`:

```c
uint32_t ls = 0;
__builtin_memcpy(&ls, job.aux_buf, sizeof(uint32_t));
if (ls > 0 && (size_t)ls * list_esz < job.aux_size) {
        uint32_t base = (uint32_t)(job.aux_size - (size_t)ls * list_esz);
        if (base >= 16) {
                ... write the stub pointer at aux_buf + 8 ...
        }
}
```

`ls` is the count field the guest wrote at `aux_buf + 0`; `job.aux_size` is the guest's declared aux
length. A guest that sets `ls` such that `ls * list_esz >= aux_size` (or such that `base < 16`)
falls out of the `if` and **the pointer at `aux_buf + 8` is never written** — it stays whatever the
guest put there. The driver then sees a non-zero list size and a guest VA, and copies
`ls * entry_size` bytes in and out at that address.

Concretely: `aux_size = 16` (the exact size of `NV0080_CTRL_GR_GET_INFO_PARAMS`), `ls = 1000`,
`aux_buf+8 = <target>`. `1000 * 8 = 8000 >= 16` → skip → the driver copies 8000 bytes from and to
`<target>` in the stub's address space.

The same shape applies to `GET_BUILD_VERSION` (`:1144`, guarded on
`sz > 0 && sz <= 512 && aux_size >= 40 + sz*3`) and `FIFO_GET_CHANNELLIST` (`:1100`, guarded on
`nc > 0 && nc <= 4096 && aux_size >= 24 + nc*8`). In every case the guard's failure mode is
*"leave the guest's pointer in place"* rather than *"zero it"* or *"reject"*.

**These are the 15 commands the tree considers handled**, and none of them is `ENFORCED`:

`0x00000101` `0x00410110` `0x00800201` `0x00801102` `0x00801104` `0x00801301` `0x00801401`
`0x00801701` `0x00801b01` `0x0080170d` `0x00801c02` `0x20800123` `0x20800802` `0x20801201`
`0x20801802`

The ring path punts all of them to the socket path (`ring_ctrl_must_punt`,
`src/stub/nvkvm_stub.c:2332-2339`), so U-4 is socket-path only — but U-1 makes that moot for the
subset the ring accepts.

---

### U-5 — FIXED 2026-08-23 — HIGH — `paramsSize` / `allocParmsSize` are never bounded against `aux_size`

**Fields:** `NVOS54.paramsSize@24`; `NVOS64.allocParmsSize@32`.

The stub points `NVOS54.params` at a `blob_alloc(aux_size)` buffer (`nvkvm_stub.c:901-921`,
allocation at `:1944-1945`) but forwards the guest's `paramsSize` unchanged. The driver allocates and
copies `paramsSize` bytes from that address (`control.c:262`) and copies the same count back out.
Nothing in QEMU or the stub relates the two: `grep` for a `params_size` clamp finds only
`nvkvm_stub.c:1891` (the stub's generic transport cap) and the 1 MiB `aux_size` cap at
`nvkvm_isolate_handlers.c:1762`.

A guest sending `aux_size = 8` and `paramsSize = 0x100000` gets a ~1 MiB over-read and over-write
of the stub's heap, adjacent to whatever `mmap` placed after the aux blob. This is a heap
corruption primitive that does not require any pointer field at all — it is the same class of bug
and it is why per-ioctl hand-marshalling is the wrong shape.

In gVisor's nvproxy the two are the same variable by construction; here they were decoupled when the
buffer moved into the aux slot, and nothing re-tied them.

**Correction, 2026-08-23 — this was understated when written.** The finding
describes the socket path only (`blob_alloc`, so a heap over-read/over-write).
The **ring path has the same hole and is worse**: `ring_exec` copies the record
into fixed stack buffers (`uint8_t aux[NVKVM_RING_MAX_AUX]`), rewrites
`param + 16` to point at them, and forwards `paramsSize` from the guest
untouched — so the same record runs off the **reader thread's own stack**, not
the heap. `ring_ctrl_must_punt()` bounds `param_size`/`aux_size` against
`NVKVM_RING_MAX_*`, but never relates either to the declared size.

**FIXED 2026-08-23.** `clamp_inner_params_size()` in `src/stub/nvkvm_stub.c`
clamps the declared size down to `aux_size` at both rewrite sites — the worker
path (after the U-2 pointer rewrite) and the ring path. The rule is the U-2 rule
applied to the size field: *the stub owns the pointer, so it owns the size that
describes it; never leave the guest's bytes in a field that describes our
memory.*

Clamp-down only, deliberately. A **smaller** declared size is the caller's
business, and for a probe-guessed alloc window the guest is intentionally
forwarding the caller's own `0` (`nvkvm_alloc_parms_probe_len()`). On the
ordinary path the two already agree exactly — the guest sets
`aux_size = ctrl->params_size` for RM_CONTROL (`nvkvm_main.c:1534`) and syncs
`alloc->alloc_parms_size = ap_size` for RM_ALLOC (`nvkvm_main.c:2068`) — so the
clamp is a no-op on every legitimate record and a hard bound on the rest. That
is also why it clamps rather than rejecting: fail-closed would turn any future
legitimate size disagreement into a broken workload, where clamping can only
ever shrink a copy to the buffer that actually exists.

`NVOS21` (the 32-byte `RM_ALLOC` variant) carries no size field at all — its
size comes from `hClass` via the alloc-param table — hence the `param_size >= 36`
discriminator that selects `NVOS64` only.

Pinned by `tests/unit/test_stub_ptr_sanitize.c` (12 cases, including the exploit
record itself and the four layouts that must be left untouched). The function is
**extracted from the stub source at build time** rather than copied into the
test, so the suite cannot drift from the code it pins.

---

### U-6 — FIXED — VA ownership table — UVM virtual-address ranges are interpreted in **QEMU's** address space, unvalidated

**Fields:** `base`/`length` or `requestedBase`/`length` on 15 of the 31 schema rows; plus
`UVM_MIGRATE_PARAMS.semaphoreAddress`.
**Driver dereferences them:** yes — that is what UVM is. These are VAs in the calling task's mm,
and `semaphoreAddress` is one the driver **writes** `semaphorePayload` to on async completion.

`nvkvm_req_ioctl_on_isolate` handles `dev_id == NVKVM_DEV_UVM` by calling
`ioctl(tfd, req->cmd, param_buf)` **in QEMU's own process** (`nvkvm_isolate_handlers.c:1471`), for
the reason given at `:1037-1046` (UVM binds `nvfp` to the calling task's mm at `UVM_INITIALIZE`, and
the matching `mmap` must come from the same mm). The schema
(`:545-591`) validates `cmd` (default-deny), a `min_size` floor, and translates up to two **fd**
fields. It does not look at a single VA field.

Rows carrying an unvalidated guest-supplied VA range:
`27 REGISTER_CHANNEL`, `31 SET_RANGE_GROUP`, `33 MAP_EXTERNAL_ALLOCATION`, `34 FREE`,
`42 SET_PREFERRED_LOCATION`, `43 UNSET_PREFERRED_LOCATION`, `44 ENABLE_READ_DUPLICATION`,
`45 DISABLE_READ_DUPLICATION`, `46 SET_ACCESSED_BY`, `47 UNSET_ACCESSED_BY`, `51 MIGRATE`,
`65 MAP_DYNAMIC_PARALLELISM_REGION`, `66 UNMAP_EXTERNAL`, `68 ALLOC_SEMAPHORE_POOL`,
`72 VALIDATE_VA_RANGE`, `73 CREATE_EXTERNAL_RANGE`.

Severity is bounded — UVM resolves most `base`/`length` pairs against its own `va_space` and returns
`NV_ERR_INVALID_ADDRESS` for ranges it does not own, so this is not a naive arbitrary-unmap. It is
still (a) an intra-QEMU cross-range confusion surface with no host-side ownership check, and (b) in
`UVM_MIGRATE`'s case a driver-performed write to a guest-named address. **And it is the one finding
in this report that the isolate does not contain**: the target process is QEMU, which holds the KVM
fd, the memslots, every isolate's socket, and the per-VM handle table.

Secondary note on the same table: its comment at `:546-556` states that rows 44/45/53/65/66 are
"NOT in our ABI" and therefore carry `min_size 0` (accept any size). Four of those five
(44/45/65/66) do have `UVM_*_PARAMS` structs in ogkm 575.51.03 with `base`/`length`, so the "no
verified layout" justification for skipping the size floor is stale.

---

### U-7 — FIXED 2026-08-23 — HIGH — `NVOS64.pRightsRequested` is never overwritten by anything

**Field:** `NVOS64_PARAMETERS.pRightsRequested`, offset 24 (`src/abi/nvgpu.h:184`).
**Driver dereferences it:** yes. `src/nvidia/src/kernel/rmapi/alloc_free.c:155-180`:
if non-NULL, `rmapiParamsCopyIn("RightsRequested", pMaskBuffer, pRightsRequested,
sizeof(RS_ACCESS_MASK), ...)` — a 16-byte `copy_from_user`.

The guest zeroes it (`src/guest/nvkvm_ioctl.c:351`). No host-side site writes offset 24 of an
`RM_ALLOC` param buffer: the generic rewrite is offset 16 only, and there is no `nvos64` case
anywhere in `nvkvm_stub.c` or `nvkvm_isolate_handlers.c`.

Read-only, 16 bytes, and the copied value only becomes the requested access mask — it is not
returned to the guest. So the direct impact is an address-probing oracle: `copy_from_user` failure
surfaces as a distinguishable `nvstatus`, letting a guest map out the stub's address space and
defeat its ASLR from inside the isolate. That is a stepping stone for U-1/U-2/U-4, not an end in
itself.

**Correction, 2026-08-23.** The claim above — *"No host-side site writes offset
24 of an `RM_ALLOC` param buffer"* — is true exactly as scoped (`nvkvm_stub.c`,
`nvkvm_isolate_handlers.c`) but misses a third path. `nvkvm_frontend.c`'s
`nvkvm_handle_rm_alloc()` **did** write the field — but only under
`if (h_class == NV01_ROOT_CLIENT)`, as an unrelated privilege check. For every
other class it saved the guest's value, forwarded it verbatim to `host_ioctl`,
and restored it afterwards. So the finding held on that path too; it was one
`if` away from being invisible.

**FIXED 2026-08-23** on both paths:

- `nvkvm_frontend.c` — the zeroing is now unconditional rather than gated on
  `NV01_ROOT_CLIENT`. The save/restore around the ioctl is unchanged, so the
  guest still sees its own bytes come back.
- `nvkvm_stub.c` — `zero_nvos64_rights()` zeroes offset 24 for `RM_ALLOC`
  alongside the U-5 clamp. `NVOS21` is exactly 32 bytes and its offset 24 is
  `{ status, pad }` — real data, not a pointer — so the discriminator is
  `param_size > 32` (`>= 36` in the code, matching the U-5 clamp's).

No-op on the live path: the guest already zeroes the field
(`src/guest/nvkvm_ioctl.c:371`) and restores the caller's original VA on the way
back (`nvkvm_main.c:2290`), so nothing legitimate ever depended on the host
forwarding it. Guest code is not a security control, which is the whole reason
this had to be done host-side as well.

Pinned by `tests/unit/test_stub_ptr_sanitize.c` (5 U-7 cases, including the two
layouts that must be left untouched).

---

### U-8 — OPEN — MEDIUM — `NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS` (0x127) writes `gpuCount²` words to a guest VA

**Fields:** `busPeerIds`, `busEgmPeerIds` (both `NvP64`).
**Driver dereferences them:** yes, and writes. `embedded_param_copy.c:570-604`: two
`RMAPI_PARAM_COPY_INIT` entries sized `gpuCount * gpuCount` × `sizeof(NvU32)`, both flagged
`RMAPI_PARAM_COPY_FLAGS_SKIP_COPYIN` — i.e. **copy-out only**. The driver writes
`gpuCount² × 4` bytes to each address. `gpuCount` is guest-supplied in the same params struct.

Allowlisted (`nvkvm_ctrl_allowlist.h`). Not in `nvkvm_ctrl_list_entry_size`
(`src/stub/nvkvm_stub.c:456-478`), not in any explicit inner-cmd block (`:952-1076`). The generic
`+16` rewrite retargets the *outer* `NVOS54.params` at the aux blob; these two pointers live inside
that blob and are forwarded exactly as the guest wrote them.

This is the cleanest write primitive in the report: no guard to bypass, no bug to trigger — it is
simply not handled. Ranked below U-1..U-4 only because the written content is P2P peer-ID data
rather than attacker-chosen bytes, and because reaching it requires the socket path (the ring
accepts it, but `must_punt` does not exclude it either — see U-1).

**Still open 2026-08-23, deliberately — blocked on a struct layout we do not
have locally.** Clearing these two fields needs the byte offsets of `busPeerIds`
and `busEgmPeerIds` inside `NV0000_CTRL_SYSTEM_GET_P2P_CAPS_PARAMS`. That struct
is not in `src/abi/nvgpu.h`, and the ogkm checkout on this machine holds only a
handful of headers, not `embedded_param_copy.c`.

Hand-transcribing the offsets from the audit's prose would be exactly the
fragility this project has already ruled out: offsets that are typed rather than
derived can shift between driver branches and fail silently on the ones nobody
tested. The fix wants the layout taken from ogkm source for every supported ABI
profile, the same way the other struct sizes are, and then the same
`aux_clear_ptr` treatment as U-12.

Note also that `gpuCount` is guest-supplied and the write is `gpuCount² × 4`
bytes per pointer, so clearing the pointers is necessary but a bound on
`gpuCount` is worth adding in the same pass.

---

### U-9 — FIXED — MEDIUM — `ISOLATE_CMD_MMAP` / `ISOLATE_CMD_MUNMAP` took a raw guest VA

**Field:** `isolate_cmd_mmap.gva`, `isolate_cmd_munmap.gva` (both now `win_va`).

What it was:

```c
uint32_t flags = cmd->map_flags | MAP_FIXED;
void *addr = stub_mmap((void *)(uintptr_t)cmd->gva, (size_t)cmd->length,
                       (int)cmd->prot, (int)flags, fd, (off_t)cmd->offset);
```

`gva` originated in `nvkvm_req_mmap_on_isolate.gva` off the virtqueue — the guest process's own
`vma->vm_start` — and reached `nvkvm_isolate_mmap` untouched: stored and passed, never bounded.
`length` and `prot` *were* bounded (audit M-1/N-2) and seccomp blocks `PROT_EXEC`, so it was a
corruption/unmap primitive rather than a code-execution one.

**Why a per-call check was the wrong fix.** The isolate's address space is *intentionally* shared
with the guest in places — the memfd `MAP_FIXED` aliasing is what lets `pin_user_pages()` on the
stub's task find pages that alias guest userspace (U-14). So a guest-directed mapping existing in
the stub is not the bug. The bug was that **nothing distinguished the region the guest is supposed
to reach from the isolate's own text, stack and allocations.** QEMU has had a sparse GPA window
(`nvkvm_sparse_init`) for exactly this reason since long before this audit; the stub had no
equivalent reservation at all.

**The fix: a reserved window.**

- **The reservation.** `stub_window_init()` (`src/stub/nvkvm_stub.c`) takes one 1 TiB
  `PROT_NONE | MAP_NORESERVE` region as the first act of `main()`, immediately after
  `apply_relocations()` and before any other mapping this process makes. Freestanding
  (`-nostdlib -ffreestanding`) is what makes "before anything else" exact: there is no malloc arena
  and no C runtime constructors, so the only things already placed are what the ELF loader and
  `fexecve` set up. From that point the kernel's mmap allocator routes every later stub allocation
  (ring region, job blobs, worker stacks, the realize intent buffer) *around* the reservation,
  because it is a live VMA. **Measured**, on the built stub with a command socket on fd 3:

  ```
  00400000-0044b000  ...          image + early anon
  7dac77080000-7dac770a0000 rw-p  worker stack   <- placed BELOW the window
  7dac771a0000-7dac771c0000 rw-p  worker stack
  7dac772c0000-7dac772e0000 rw-p  worker stack
  7dac773e0000-7dac77400000 rw-p  worker stack
  7dac77400000-7eac77400000 ---p  the window, 0x100_0000_0000 = 1 TiB
  ```

  Failure is **fatal**: an isolate that cannot bound guest-directed mappings must not run.

- **Placement is the kernel's, deliberately.** `addr = NULL`, so the window inherits the stub's own
  mmap-region entropy rather than sitting at a constant. Hardcoding a base would have discarded
  exactly the ASLR that making the stub a genuine static-PIE (`sec-easy-batch`) was for. QEMU
  learns the base over the command socket (`ISOLATE_CMD_WINDOW_INFO`) and never returns a stub VA
  to the guest — `nvkvm_resp_mmap_on_isolate` carries a token, a GPA and a length, and that is
  deliberate.

- **Size, and why it cannot be the binding constraint.** Live guest-directed bytes in one isolate
  are capped by the VM-wide sparse GPA window (128 GiB) — nothing is mapped into an isolate without
  a GPA extent behind it — plus up to `NVKVM_MIG_MAX_RANGE` (2 GiB) of reservation per in-flight
  OS-descriptor registration. 1 TiB clears both, so "window full" can never be a denial the
  existing 128 GiB ceiling does not already impose. Cost: one VMA, no memory, and 1 TiB out of the
  128 TiB the stub's own allocator draws from — 0.011 bits of ASLR entropy.

- **Who chooses the address.** No longer the guest. QEMU runs a per-isolate window allocator
  (`win_place_locked`, `src/qemu/nvkvm_isolate_handlers.c`), bump pointer plus free list, the same
  shape as `nvkvm_sparse_gpa_alloc`. `req->gva` survives only as the identity recorded in
  `iso_mmap_tbl` and as the adjacency key that keeps a migration's 2 MiB chunks contiguous.

- **Enforcement, reject-never-clamp.** `stub_window_contains()` gates `handle_mmap`,
  `handle_munmap_cmd`, and the `host_va_hint` arm of `handle_realize_uvm_fd`, logging
  `nvkvm: DENY ... (U-9)`. In `handle_mmap` the address check runs **before** the
  handle lookup, deliberately: it does not depend on the fd, and a control sitting behind another
  check is one a later edit to that other check can bypass — which is exactly A-1's shape.

**Evidence.** Two suites, neither needing a GPU.

- `tests/unit/test_stub_window.c` (27 cases) pins the predicate and the allocator, both extracted
  from real source at build time. Verified to fail when reverted: a naive
  `addr >= base && addr + len <= base + size` scores 23/27 (the u64-wrap and zero-length cases);
  the gate removed outright scores 12/27; the run reservations removed score 26/27 — the
  interleaved registration fails while the *non*-interleaved one still passes, which is the same
  shape as the 2 MiB A-1 probe that sailed past its bug.
- `tests/security/u9_window_gate_test.c` (12 probes) spawns the **real built stub** and sends real
  `ISOLATE_CMD_MMAP` / `_MUNMAP` / `_WINDOW_INFO` messages over the real socket, because neither
  unit suite can see whether the predicate is actually *wired into* the handlers — a stub with
  `stub_window_contains()` defined and never called passes the whole unit suite. Against a
  gate-removed build it scores 4/12, and the probe at `0x401000` makes that stub **unmap its own
  text and die**, which is U-9 demonstrated rather than described.

Two things that suite got wrong first, recorded because both are the shape of a green result that
measures nothing. (1) A hardcoded "obviously outside" address of `0x7f1122200000` was *accepted*,
and the gate was right — the window is 1 TiB and that run had placed it at `0x7e46fb400000`. Guest
VAs and the window are drawn from the same band; every probe is now derived from the reported base.
(2) The MMAP probe first asserted only `retval != 0` and got `-EBADF`, because the handle lookup ran
first and the address was never examined — it would have passed with the gate deleted. That is what
prompted the reordering above, and the probe now asserts the exact errno only the window produces.

**What it does not cover.** `REALIZE_UVM_FD` with `host_va_hint == 0` — which is what QEMU passes
today — maps at a **kernel-chosen** address, outside the window. That address is not
guest-influenced, so it is not a U-9 exposure; but it means "every mapping the stub makes on the
guest's behalf is in the window" is *not* a true statement, only "every mapping made at an address
anything outside the stub supplied" is. Moving it inside would require MAP_FIXED at a window VA
with the offset rewritten to match, because `uvm_mmap()` pins `vm_start == (vm_pgoff << PAGE_SHIFT)`
(ogkm `kernel-open/nvidia-uvm/uvm.c:791-795`) — a driver-behaviour change with no test short of a
GPU run, so it was not made.

**Relationship to `iso_mmap_covers()` / `iso_mmap_translate()` — both are still needed.** The window
answers *where a mapping may be*; the table answers *what was installed, for whom, and whether it is
still live*. Inside the window a guest can still name another handle's extent, a freed extent, or a
hole between two extents, and the window cannot see any of that. Conversely the table cannot be the
window: it only knows what QEMU installed, so a stub-side path that maps without telling QEMU is
invisible to it. The window is necessary, the table sufficient; what the window buys the A-1 class
is that a *forgotten* check now fails inside guest-directed memory instead of on stub-private
memory.

---

### U-10 — FIXED 2026-08-23 — MEDIUM — DRM `PRIME_FENCE_CONTEXT_CREATE` (nr 0x45) has two pointers; neither is handled

**Fields:** `import_mem_nvkms_params_ptr` (offset 16) and `event_nvkms_params_ptr` (offset 32) in
`drm_nvidia_prime_fence_context_create_params` (ogkm `kernel-open/nvidia-drm/nvidia-drm-ioctl.h:199-213`).

The stub's DRM branch (`src/stub/nvkvm_stub.c:906`) handles only `job_nr == 0x54` and
`job_nr == 0x49`. NR 0x45 falls to the generic branch, which writes the aux pointer at offset 16 —
accidentally covering `import_mem_nvkms_params_ptr`, but only when `aux_size > 0`, and never
covering offset 32. `event_nvkms_params_ptr` is forwarded verbatim in every case.

Reachable only when `nv->graphics` is set (`nvkvm_isolate_handlers.c:1557-1564`); compute-only VMs
are unaffected.

**FIXED 2026-08-23.** Both pointers are now handled in `nvkvm_stub.c`:

- **Offset 16** gets its own branch in the `ptr_off` decision, keyed on
  `job_type == 'd' && job_nr == 0x45`, so it is written unconditionally — the
  aux pointer, or an explicit 0. Previously it fell to the generic branch, which
  only fires when `aux_size > 0`, so an `aux_size == 0` record left the guest's
  own bytes in a pointer field: the same fail-open shape as U-2.
- **Offset 32** is zeroed outright. Only one aux blob exists per job, so there is
  nothing to retarget the second pointer at; the driver null-checks it and the
  event params are not on any path nvkvm serves.

---

### U-11 — OPEN — MEDIUM — frontend pointer fields at offsets the generic rewrite does not reach

All of these are zeroed by the guest and by nothing on the host. Grouped because the analysis is
identical.

| ioctl | field | offset | guest zeroes at | host overwrite |
|---|---|---|---|---|
| `NV_ESC_RM_UNMAP_MEMORY` (0x4f) | `NVOS34.pLinearAddress` | 16 | `nvkvm_ioctl.c:436` | only if `aux_size>0`; this ioctl has no aux → none |
| `NV_ESC_RM_MAP_MEMORY` (0x4e) | `NVOS33.pLinearAddress` | 32 | `nvkvm_ioctl.c:418` | none (the `p_linear_address = 0` at `nvkvm_dispatch.c:353` is dead code) |
| `NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO` (0x5e) | `NVOS56.pOldCpuAddress` | 16 | `nvkvm_ioctl.c:451` | only if `aux_size>0` → none |
| `NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO` (0x5e) | `NVOS56.pNewCpuAddress` | 24 | `nvkvm_ioctl.c:452` | none |

Severity is genuinely lower here: `NVOS33.pLinearAddress` is `[OUT]`, and `NVOS34`/`NVOS56` addresses
are looked up in RM's own per-client mapping list before use, so a value that matches no live
mapping is rejected rather than dereferenced. They are `UNENFORCED` because nothing on the host
guarantees that, not because a walk is demonstrated.

---

### U-12 — FIXED 2026-08-23 — MEDIUM — `NV0000_CTRL_CMD_GPU_GET_ID_INFO` (0x202) `szName`

**Field:** `NV0000_CTRL_GPU_GET_ID_INFO_PARAMS.szName` (`NvP64`).

Output-only: the driver writes the GPU name string to it. Allowlisted; the guest zeroes it rather
than marshalling (per `ARCHITECTURE.md`, `src/guest/nvkvm_main.c:1628-1639`); the host does nothing.
A short, low-entropy, attacker-positioned write inside the isolate.

**FIXED 2026-08-23.** `aux_clear_ptr(job.aux_buf, job.aux_size, 16)` on
`inner_cmd == 0x202`, alongside the existing inner-pointer handling for `0x101`,
`0x3d05`, `0x3d06`/`0x3d08` and `0x80170d`. The name is optional — `cuInit` does
not need it and `nvidia-smi` gets the model elsewhere — and the driver
null-checks `szName`, so clearing it host-side costs nothing and matches what the
guest was already trying to do. The point is that the guest was the *only* thing
doing it, and guest code is not a security control.

---

### U-13 — CLOSED 2026-08-24 — NOT EXPLOITABLE — `NV2080_CTRL_CMD_FIFO_DISABLE_CHANNELS` (0x2080110b) `pRunlistPreemptEvent`

`NvP64`, documented in the header as "KEVENT handle for Async HW runlist preemption". It is not in
`embedded_param_copy.c`, which suggests RM treats it as an opaque handle rather than a user pointer —
but I could not follow it to a definite consume-or-ignore in the open tree. Allowlisted, unhandled
host-side. Listing it as `UNENFORCED` with severity `UNKNOWN` rather than dropping it.

**Traced. The severity is not UNKNOWN, it is none, and the reason is a check in the driver.**

Reachability, in the order it has to be established:

1. **Is the command allowlisted?** Yes — `src/qemu/nvkvm_ctrl_allowlist.h:132`, a bare
   `0x2080110bu` with no justifying comment. Neither rule-based passthrough admits it
   (`0x110b & 0x8000 == 0`, and `cmd >> 16` is `0x2080`, not `0x2081`), so the explicit row is the
   only thing letting it through. So it does **not** close as unreachable here.

2. **Does nvkvm's pointer rewrite reach the field's offset?** No. The generic rewrite (U-2) replaces
   `NVOS54.params` at outer offset 16 with the address of the stub's aux blob; it does nothing to
   fields *inside* that blob. Everything inside is per-command, and in `src/stub/nvkvm_stub.c` the
   per-command list is exactly `0x101`, `0x202`, `0x3d05`, `0x3d06`/`0x3d08` and `0x80170d`, plus the
   `nvkvm_ctrl_list_entry_size()` InfoList family. `0x2080110b` is in none of them, so the guest's
   64 bits at aux offset 16 are forwarded to RM **verbatim**. (Offset confirmed by compiling the
   struct: `pRunlistPreemptEvent@16`, `hClientList@24`, `hChannelList@280`, `sizeof == 536`.)

3. **Does the driver dereference it?** **No — it refuses the entire control first.**
   `subdeviceCtrlCmdFifoDisableChannels_IMPL`, `src/nvidia/src/kernel/gpu/fifo/kernel_fifo_ctrl.c`,
   opens with:

   ```c
   // Validate use of pRunlistPreemptEvent to allow use by Kernel clients only
   if ((pDisableChannelParams->pRunlistPreemptEvent != NULL) &&
       (pCallContext->secInfo.privLevel < RS_PRIV_LEVEL_KERNEL))
   {
       return NV_ERR_INSUFFICIENT_PERMISSIONS;
   }
   ```

   It is the first statement of the handler, before the `NV_RM_RPC_CONTROL` forward and before any
   use of the field. And no ioctl client can satisfy it: the escape path sets
   `secInfo.privLevel = osIsAdministrator() ? RS_PRIV_LEVEL_USER_ROOT : RS_PRIV_LEVEL_USER`
   (`src/nvidia/arch/nvalloc/unix/src/escape.c:375`), and `RS_PRIV_LEVEL_KERNEL` is the **largest**
   value of the enum (`src/common/sdk/nvidia/inc/nvsecurityinfo.h:36-38`, ordered `USER`,
   `USER_ROOT`, `KERNEL`). `RS_PRIV_LEVEL_KERNEL` is set only for RM-internal contexts
   (`rmapi/deprecated_context.c:184`), never for anything arriving through `/dev/nvidiactl`. The
   stub is an ordinary userspace RM client, so a non-NULL value there is a guaranteed
   `NV_ERR_INSUFFICIENT_PERMISSIONS`, not a dereference.

   The `NV_RM_RPC_CONTROL` forward that follows copies `pParams` to GSP **as data**, so the pointer
   value crosses as bytes and is not dereferenced there either.

**Checked across the whole version span nvkvm supports**, because a guard that appeared late would
close nothing on the older branches. Byte-identical at **515.105.01, 575.51.03, 580.95.05,
580.159.04 and 610.43.02** — i.e. present since the first open-source release, comfortably before
535, the oldest driver nvkvm supports.

**No code change.** A defensive `aux_clear_ptr(job.aux_buf, job.aux_size, 16)` on `inner_cmd ==
0x2080110b` was considered — it is the U-12 treatment and it would cost nothing on the live path,
since a legitimate userspace caller must pass NULL anyway. It was **not** added, deliberately:
clearing the field would convert a request RM *refuses* into one it *accepts*, silently changing the
driver's verdict for a hostile caller. U-7's clear does not have that property (the field there is
one the driver would otherwise happily dereference); this one does. Where the driver's own check is
unconditional, first-statement, and present in every supported version, masking it is a downgrade,
not hardening.

**Not closed by this**, and it is a different field in the same struct: the *"Suspected — cross-VM
`hClient` blind spot"* item in
[`audit-prerelease-2026-08-21.md`](audit-prerelease-2026-08-21.md) cites `0x2080110b`'s
`hClientList[64]` (offset 24, above) as a second client handle the per-VM gate does not see, because
that gate reads param offset 0 only. That is a handle-scoping question, not a pointer-dereference
one, and it stays open. U-13 was only ever about `pRunlistPreemptEvent`.

**An UNKNOWN that turns out to be nothing is a result.** Recorded in full so the next reader does
not re-derive it — the work here was three greps and a struct offset, and it had been sitting open
for the want of them.

---

### U-14 — WAS BY DESIGN, NO LONGER AN EXCEPTION — `NVOS02.pMemory` for `hClass == 0x71`

`NV_ESC_RM_ALLOC_MEMORY` with `hClass == NV01_MEMORY_SYSTEM_OS_DESCRIPTOR` **used to** forward the
guest VA deliberately: the guest migrates the range onto memfds and the stub `MAP_FIXED`ed them at
the same VA (`src/guest/nvkvm_ioctl.c:396-409`), so `RmAllocOsDescriptor` → `pin_user_pages` found
pages aliasing guest userspace. That made this the one place where a guest pointer reaching the
driver was intended, and `ARCHITECTURE.md`'s invariant had to be read with an asterisk.

**U-9's window removes the asterisk.** The chunks now live at an address the *host* chose, so QEMU
rewrites `NVOS02.pMemory` to the translated window base at the A-1 gate before forwarding. This is
invisible to the guest — `pMemory` is IN-only for this class; the caller gets back an `hMemory`,
never this field — and the physical aliasing the feature depends on is untouched: same memfds, same
pages, only a different virtual address in a process the guest cannot observe. What made it
possible is that "the stub maps at the guest's VA" was an *implementation* of the aliasing, never a
requirement of it: RM only needs `pMemory` to be a valid user VA **in the calling task**, and the
calling task is the stub.

Two things had to hold for the rewrite to be safe, and `iso_mmap_translate()` establishes both:
the guest range must be spanned by live mirrored entries **of this isolate** (the A-1 property,
unchanged), and those entries must be **contiguous in the window**, because RM pins one range. The
second is why the window allocator has run reservations at all — see the note on
`win_place_locked`.

Note that `hClass` is **not** gated for nr 0x27 — the alloc-class allowlist applies only to nr 0x2b
(`nvkvm_isolate_handlers.c`) — and `nvkvm_fe_alloc_allowlist.h:13-16` states OS_DESCRIPTOR (0x71) is
deliberately omitted from that allowlist, which is true and irrelevant on this path.

**Unverified.** The rewrite has not been exercised on hardware. `tests/security/a1_span_test.c` is
the test that would say so — its "inside passes the gate" case now additionally depends on the run
reservation keeping a 16 MiB registration's eight chunks window-contiguous. The unit suite pins
that property against the real allocator source
(`tests/unit/test_stub_window.c`, "a 16 MiB registration lands contiguously" and the interleaved
case), but the end-to-end `cudaHostRegister` path needs a GPU.

---

## 5. RM_ALLOC classes (89)

Alloc params travel in the aux blob and the outer `pAllocParms@16` is covered by the generic
rewrite (subject to U-2). The residual question is whether any allowlisted class's *alloc-params
struct* contains a pointer the driver walks. Resolved against
`src/nvidia/src/kernel/rmapi/resource_list.h` (the authoritative class → alloc-param-struct table)
plus the `class/cl*.h` and `nvos.h` bodies.

**81 of 89 carry no pointer or address-like field. 8 do:**

| class | name | struct | field | host handling |
|---|---|---|---|---|
| 0x0000 | `NV01_ROOT` | `NV0000_ALLOC_PARAMETERS` | `pOsPidInfo` (`NvP64`) | none — see below |
| 0x0005 | `NV01_EVENT` | `NV0005_ALLOC_PARAMETERS` | `data` (`NvP64`, an **fd**) | partial, `nvkvm_stub.c:1327-1355` |
| 0x0079 | `NV01_EVENT_OS_EVENT` | `NV0005_ALLOC_PARAMETERS` | `data` (`NvP64`, an **fd**) | partial, same site |
| 0x003e | `NV01_MEMORY_SYSTEM` | `NV_MEMORY_ALLOCATION_PARAMS` | `address` (`NvP64`, `[OUT]`) | none |
| 0x0040 | `NV01_MEMORY_LOCAL_USER` | `NV_MEMORY_ALLOCATION_PARAMS` | `address` (`NvP64`, `[OUT]`) | none |
| 0x50a0 | `NV50_MEMORY_VIRTUAL` | `NV_MEMORY_ALLOCATION_PARAMS` | `address` (`NvP64`, `[OUT]`) | none |
| 0x00f1 | `NV_IMEX_SESSION` | `NV00F1_ALLOCATION_PARAMETERS` | `pOsEvent` (`NvP64`) | none |
| 0x00fd | `NV_MEMORY_MULTICAST_FABRIC` | `NV00FD_ALLOCATION_PARAMETERS` | `pOsEvent` (`NvP64`) | none |

Notes on each, because the severities differ a lot:

- **`NV0005.data` (0x05 / 0x79)** is an fd, not a VA — the driver resolves it with
  `osUserHandleToKernelPtr`. It **is** translated, at `nvkvm_stub.c:1335-1352`, but only for the
  nvos64 form: the gate at `:1238-1241` requires `job.param_size > 32`, deliberately (audit G-5), so
  that a raw nvos21-form fd is not misread as a handle_id. The consequence is that on the nvos21
  path a raw *guest* fd number reaches the driver. That is an fd-confusion issue rather than a
  pointer-deref one, and it is already documented in-tree; it is not counted in the `UNENFORCED`
  totals for this audit's property.
- **`NV_MEMORY_ALLOCATION_PARAMS.address` (0x3e / 0x40 / 0x50a0)** is `[OUT]` in the common path
  (`src/abi/nvgpu.h:722`). Classified `UNENFORCED` — nothing on the host guarantees it — but no walk
  is demonstrated, so severity sits with U-11's group.
- **`NV0000_ALLOC_PARAMETERS.pOsPidInfo` (0x0000)** appears to be RM-internal: the client's
  `pOsPidInfo` is set by RM itself from `osGetPidInfo()` (`rmapi/client.c:113`) and only ever read
  back through `osFindNsPid` (`rmapi/client_resource.c:1826-1827`). I found no read of the
  *user-supplied* alloc-param field anywhere in the open tree. Severity `UNKNOWN`, leaning benign.
- **`pOsEvent` (0x00f1 / 0x00fd)** is documented `[IN]` (`ctrl/ctrl00fd.h:141`). The consuming
  constructors are in `kernel/mem_mgr`, outside the sparse checkout used here, so I could not
  establish whether it is dereferenced as a VA or resolved as an fd handle (the naming and the
  `NV0005.data` precedent suggest the latter). Severity `UNKNOWN`. Both classes are fabric /
  multi-node primitives with no role in this deployment and are strong candidates for removal from
  the allowlist regardless.

**Independently: the host has no per-class alloc-params size bound at all.** The size-by-hClass
tables live only in the guest — two of them, `src/guest/nvkvm_main.c:1651-1743` (nvos21) and
`:1777-1869` (nvos64, used only as a fallback when userspace passed `alloc_parms_size == 0`), with a
flat `NVKVM_SHM_SLOT_DEFAULT_SIZE` ceiling at `:1871`. Neither the stub nor QEMU has an equivalent:
`nvkvm_stub.c:1891` is a generic transport cap, and the QEMU-side gates
(`virtio_nvgpu.c:435`, `nvkvm_isolate.c:549`) are slot-capacity checks. The 1 MiB inner-params cap at
`nvkvm_isolate_handlers.c:1762` applies to `RM_CONTROL` only, not `RM_ALLOC`. So `aux_size` for an
alloc is guest-chosen and host-unverified — this is the `RM_ALLOC` half of U-5.

---

## 6. Recommendation

**Yes — build the schema-driven default-deny for pointer fields. It is the only fix that scales,
and the evidence for it is that this codebase has now failed the same way five times.**

The pattern is consistent across U-1 through U-4: someone identifies a pointer, writes a bespoke
rewrite, guards it with a condition derived from guest-supplied values, and the guard's failure mode
is *forward the guest's bytes* rather than *reject*. `IDLE_CHANNELS` is the single site that got it
right, and it only got it right on the second attempt, after the first fix landed in dead code
(`nvkvm_stub.c:1269-1279`).

Concretely, in priority order:

1. **Fix the fail-open direction first — this is cheap and it is most of the win.** Every
   conditional rewrite site should zero the field when its guard fails, instead of leaving it. Turn
   `if (guard) { p = stub_ptr; }` into `if (guard) { p = stub_ptr; } else { p = 0; }` at
   `nvkvm_stub.c:901-921`, `:1030-1045`, `:1100-1125`, `:1144-1160`, and `:2392-2396`. This alone
   closes U-2, U-4, and the `aux_size == 0` half of U-1, without any new infrastructure.
2. **Route the ring through the same gates as the socket path, or delete it.** U-1 is not a pointer
   bug so much as an entire unmediated channel; it also bypasses the control allowlist, the hClient
   allowlist and the DUP gate. Given `docs/design/command_buffer.md`'s own measured conclusion that
   the ring does not improve throughput on the target workload
   (`src/guest/nvkvm_main.c:414-423`), deleting it is a defensible answer and is strictly the
   cheapest one.
3. **Build the schema.** A table of `(cmd, [pointer offsets], [size-field offset])` covering the
   frontend NRs and the allowlisted control/alloc inner params, applied as a sweep in the stub
   *before* any bespoke rewrite runs: zero every listed offset unconditionally, then let the
   per-ioctl code opt a field back in by pointing it at aux. Default-deny becomes structural — an
   unlisted command with pointers gets them zeroed, and a *new* command added to an allowlist without
   a schema row fails safe rather than fails open. This is what gVisor's nvproxy does and what the
   nestrilabs rewrite table does; `ARCHITECTURE.md:373-377` currently frames "nvkvm does something
   different" as a virtue, and the difference is precisely the coverage gap this report enumerates.
4. ~~**Add the size-consistency check** (U-5): reject any `RM_CONTROL` where
   `paramsSize != aux_size`, and any `nvos64 RM_ALLOC` where `allocParmsSize > aux_size`. One
   comparison each, in QEMU, where the values are already in hand.~~ **DONE 2026-08-23**, with two
   departures from this recommendation, both deliberate: it clamps rather than rejecting (a reject
   on `!=` would break the legitimate `aux_size > paramsSize` extension records that U-4's writeback
   creates), and it lives in the **stub** rather than QEMU — QEMU never sees the ring path, which is
   the site where an over-read runs off the reader thread's stack. See U-5.
5. **Gate `NVOS32.function`** to the set actually needed (U-3), matching nvproxy. If
   `ALLOC_OS_DESCRIPTOR` is genuinely required, it needs the same memfd-aliasing treatment as the
   `NVOS02` path, not opaque forwarding.
6. **Validate UVM VA ranges against QEMU's own mapping table** (U-6), or move UVM into the isolate.
   This is the only finding the isolate does not contain, and it deserves its own design pass rather
   than a spot fix.
7. **Resolve or drop the 13 unresolvable control IDs and the two rule-based passthroughs.** An
   allowlist entry whose params layout cannot be determined is not an allowlist entry, it is a
   passthrough with extra steps.

**On the isolate mitigation.** It is real and it is load-bearing: Phase 0 `clone()`s the stub into
fresh user/pid/net/ipc/uts namespaces, unprivileged, under a seccomp allowlist that blocks `execve`,
`ptrace`, `fork`, and `PROT_EXEC` mappings (`src/stub/nvkvm_stub.c:2610-2692`, `:2830-2845`). Every
finding above except U-6 corrupts *within one isolate* — one guest process's own GPU context — and
not QEMU, not the host, not another VM. That is the difference between "critical" and "catastrophic"
and it should be stated plainly. It is not a reason to leave the bugs: an isolate still holds live
RM client handles, mapped device memory, and the ring shared with the guest, and the escape surface
from a corrupted isolate is the whole point of the seccomp filter, which has not been audited here.

---

## 7. What this audit did not settle

- **The 13 unresolvable control IDs (§3)** and the unbounded rule-based passthroughs. Not
  determinable from the open tree.
- **`pOsEvent` on classes 0x00f1 / 0x00fd, and `pOsPidInfo` on class 0x0000** (§5) — consuming code
  outside the sparse checkout, or apparently RM-internal.
- **Whether `NVOS32.data.HwAlloc.bindResultFunc` is consumed** by the Linux RM path (U-3).
- ~~**`pRunlistPreemptEvent`** (U-13).~~ **Settled 2026-08-24** — RM rejects the whole control with
  `NV_ERR_INSUFFICIENT_PERMISSIONS` before touching the field, for every client that is not
  kernel-privileged, in every driver version from 515 to 610. See U-13 above.
- **The seccomp filter itself.** Assumed effective; not read line-by-line. Since it is what bounds
  every severity rating in this report, it should be the next thing audited.
  **Done 2026-08-20** — see [`audit-boundaries-2026-08-20.md`](audit-boundaries-2026-08-20.md) §6.
  The filter and the namespace sandbox around it hold up: the BPF was verified
  instruction by instruction (the jump offsets are correct, and an off-by-one in
  one of them would have been a clean allowlist bypass). Three hardening gaps
  were found and fixed, the substantive one being a process-creation syscall left
  in the allowlist that nothing calls after the filter is applied.

---

## Addendum — a live heap overflow found while fixing U-6

Not a guest-pointer finding, and not in the original audit. It was found while
building the U-6 fix and is recorded here because it is the most severe defect
the audit work surfaced, and because it required **no malicious guest at all**.

`src/qemu/virtio_nvgpu.c:644` allocated the parameter buffer at the size the
*guest* declared:

```c
priv_param = g_malloc(w->req.param_size);
```

The host NVIDIA driver, however, reads and writes **its own** struct size. The two
disagree, and the guest's size is the smaller one. Measured:

| UVM command | guest `param_size` | driver struct | consequence |
|---|---|---|---|
| `UVM_REGISTER_CHANNEL` (27) | 48 | 56 | driver writes `rmStatus` **4 bytes past** a `g_malloc` allocation |
| `UVM_MIGRATE` (51) | 48 | 80 | **32-byte heap over-read**, which included the semaphore address |

A cooperative guest running ordinary CUDA triggered the write on **every**
`REGISTER_CHANNEL` call. This was not a latent attack surface; it was happening
continuously in normal operation, corrupting QEMU's heap by four bytes each time.

**Fix:** the ioctl now runs on an over-sized zeroed bounce buffer sized to the
driver's struct, not the guest's declaration, with the result copied back into the
guest's buffer at the guest's length.

**The general lesson**, which applies well beyond this call site: a guest-declared
size must never determine a host-side allocation that the *driver* will write into.
The two sizes are independent facts — the guest's is a claim about its own struct,
the driver's is ground truth about the kernel's — and any place they are conflated
is a heap bug waiting for a struct to grow. This is the same root cause as the
version-keyed ABI table (`docs/reference/abi-profiles.md`): NVIDIA changes struct
sizes between driver releases, and anything assuming otherwise breaks silently.
