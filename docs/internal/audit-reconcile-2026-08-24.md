# Audit reconciliation, 2026-08-24

**What this is.** A two-sweep reconciliation of the three audit documents against
the code at `main` (`68a35c0`), commissioned after four separate instances of
*the documentation and the code disagreeing* surfaced in a single day — every one
found incidentally.

- **P-2** — critical, marked "see below", never closed; had been fixed long ago.
- **P-3** — marked open; its writable-alias half was fixed months earlier by `073ece8`.
- **`handle_ioctl`** — dead code that read as a security control.
- **A-1** — a correct control keyed to the wrong ioctl number: the alloc-class
  allowlist ran under `if (nr == 0x2b)` while the dangerous route was `nr == 0x27`.

Those are not four mistakes. They are one failure mode — **decisions get made from
the documentation, and the documentation drifts** — and two mechanical sweeps catch
all four:

- **Sweep 1**: does every audit entry match the code today, in *both* directions?
- **Sweep 2**: does every gate's reachability condition cover *every* route to the
  thing it protects?

**Method.** Static analysis only, at `68a35c0`. No GPU, no VM, no execution, except
where a unit test was built and run (noted inline). Every claim cites `file:line` in
this tree. Where a question needs the vendor driver source or hardware to settle, it
is left open and said so — a confident wrong status is what created this task.

---

## PRIORITY 0 — read this before the tables

### R-1 (NEW, HIGH) — every host-side sanitiser is keyed on `_IOC_TYPE == 'F'`, and the guest chooses `_IOC_TYPE`

**This is A-1's shape, aimed at the one control the guest-pointer audit calls
`ENFORCED`.**

`audit-guest-pointers.md` §3 states:

> **`ENFORCED` total across the whole boundary: one.** `NV_ESC_RM_IDLE_CHANNELS`
> — an unconditional `memset(param_buf + 12, 0, 28)` gated only on
> `_IOC_TYPE=='F' && _IOC_NR==0x41 && param_size >= 40`, **none of which a guest
> can use to skip it.**

The final clause is false. `_IOC_TYPE` is a field of the command word the guest
supplies, and `0x41` is simultaneously `NV_ESC_RM_IDLE_CHANNELS` (frontend) and
`DRM_COMMAND_BASE + 0x01` = `GEM_IMPORT_NVKMS_MEMORY` (DRM) — a collision the tree
already documents at `src/guest/nvkvm_ioctl.c:134-138`.

A record with `_IOC_TYPE == 'd'`, `_IOC_NR == 0x41`, aimed at a handle whose device
is `/dev/nvidiactl` or `/dev/nvidiaN`:

| gate | site | keyed on | fires? |
|---|---|---|---|
| graphics gate | `nvkvm_isolate_handlers.c:2154` | `!nv->graphics && type=='d'` | only on compute-only VMs |
| DRM NR allowlist | `nvkvm_isolate_handlers.c:2170` | `type=='d'` | **passes** — `0x41` is allowed (`nvkvm_drm_allowlist.h:51`) |
| frontend NR allowlist | `nvkvm_isolate_handlers.c:2285-2287` | `type=='F'` | **skipped** |
| U-3 NVOS32 function gate | `:2344` | inside `if (type=='F')` | **skipped** |
| A-1 OS-descriptor gate | `:2410` | inside `if (type=='F')` | **skipped** |
| alloc-class allowlist | `:2484` | inside `if (type=='F')` | **skipped** |
| ctrl-cmd allowlist | `:2530` | `type=='F' && nr==0x2a` | **skipped** |
| DUP_OBJECT source gate | `:2562` | `type=='F'` | **skipped** |
| H-3 hClient gate | `:2594` | `type=='F'` | **skipped** |
| non-`'F'` default-deny (M-A) | `:2224` | `else if` — **after** the `'d'` branch | **never reached** |
| stub IDLE_CHANNELS neutralisation | `nvkvm_stub.c:1481-1484` | `((job.cmd>>8)&0xff)=='F'` | **skipped** |
| stub `clamp_inner_params_size` (U-5) | `nvkvm_stub.c:933` | `job_type != 'F'` → return | **skipped** |
| stub `zero_nvos64_rights` (U-7) | `nvkvm_stub.c:891` | `job_type != 'F'` → return | **skipped** |
| stub embedded-fd translation | `nvkvm_stub.c:1443` | `((job.cmd>>8)&0xff)=='F'` | **skipped** |

Nothing binds the ioctl's type to the target device. `nvkvm_req_ioctl_on_isolate`
forwards `req->isolate_id`, `req->handle_id` and `req->cmd` as three independent
guest-chosen values (`nvkvm_isolate_handlers.c:2756-2765`), and the stub resolves
the fd with `handle_lookup(job.handle_id)` and calls
`stub_ioctl(fd, job.cmd, job.param_buf)` (`nvkvm_stub.c:961`, `:1573`) with no
device/type consistency check.

**Why the audit did not catch it.** A-5 is the same observation, and it is marked
**fixed**. A-5's recommendation had two halves: *"make the guest's size table
type-aware, and reorder the host gate so the non-`'F'` deny is evaluated before the
per-type allowlists."* Only the first half landed
(`src/guest/nvkvm_ioctl.c:143`) — and that half is **guest code**, which
`audit-guest-pointers.md`'s own method statement excludes as a security control
(*"Guest-side sanitisation … is treated as not a security control throughout"*).
The host-side half was never done: the chain at `nvkvm_isolate_handlers.c:2164 /
2189 / 2224` still evaluates `'d'` first, NVKMS second, and the non-`'F'` deny
last. **A-5 is marked fixed on the strength of a fix in code the audit does not
count.**

#### R-1.1 — the exposed set is exactly four NRs, and the critical gates are out of reach

The `'d'` branch admits, as **absolute** NRs (`nvkvm_drm_allowlist.h:28-29` for the
two core ones, `:49-115` for the nvidia-private ones at base `0x40`):

`0x00 0x09 0x41 0x43 0x44 0x45 0x46 0x48 0x49 0x4b 0x4f 0x54 0x55 0x56 0x57`

Intersecting that with the frontend ioctl table (`src/abi/nvgpu.h:905-919`) gives
**four** collisions, not one:

| NR | as `'F'` (frontend) | as `'d'` (DRM, what the stub marshals for) |
|---|---|---|
| `0x41` | `NV_ESC_RM_IDLE_CHANNELS` | `GEM_IMPORT_NVKMS_MEMORY` |
| `0x4f` | `NV_ESC_RM_UNMAP_MEMORY` | `DMABUF_SUPPORTED` |
| `0x54` | `NV_ESC_RM_ALLOC_CONTEXT_DMA2` | `SEMSURF_FENCE_CTX_CREATE` |
| `0x57` | `NV_ESC_RM_MAP_MEMORY_DMA` | `SEMSURF_FENCE_ATTACH` |

**The most important result here is a negative one, and it is load-bearing.** The
following are **NOT** reachable through the `'d'` branch, because their NRs are
absent from the DRM allowlist — verified by enumerating it exhaustively:

| gate | its NR | in DRM allowlist? |
|---|---|---|
| **U-3** NVOS32 function gate | `0x4a` (`0x40+0x0a` = `GEM_MAP_OFFSET`) | **no** — G-3 excludes `0x0a` (`nvkvm_drm_allowlist.h:93-115`) |
| **A-1** OS-descriptor / `iso_mmap_covers` | `0x27` | **no** — below `0x40`, and only `0x00`/`0x09` are allowed there |
| **alloc-class allowlist** | `0x2b` | **no** — same reason |
| **ctrl-cmd allowlist** | `0x2a` | **no** — same reason |
| **DUP_OBJECT source gate** | `0x34` | **no** — same reason |

So the type confusion **cannot** be used to bypass U-3, A-1, the alloc-class
allowlist, the control allowlist, or the DUP gate. That bounds the finding hard and
is why this is HIGH and not CRITICAL.

All four exposed NRs are *already* on the frontend NR allowlist
(`nvkvm_fe_alloc_allowlist.h:32,35,36,38`), so the bypass grants **no new NR**. What
it bypasses is **per-NR parameter handling and the hClient gate**, both type-keyed.

#### R-1.2 — per-NR consequence

The stub's `ptr_off` decision (`nvkvm_stub.c:1017-1046`) has a `'d'` arm for
`0x54`/`0x49`/`0x41` that sets `ptr_off = NVKVM_NVKMS_ADDR_OFF` = **8**
(`src/common/nvkvm_proto.h:112`); `0x4f` and `0x57` have no `'d'` arm and fall to
the generic `aux_size > 0 && param_size >= 24` case, `ptr_off = 16`.

| NR | struct under `'F'` | what `'d'` writes | sanitisation skipped | verdict |
|---|---|---|---|---|
| **`0x41`** | `NVOS30` (`src/abi/nvgpu.h:412-424`): `h_channel@8`, `num_channels@12`, `p_clients@16`, `p_devices@24`, `p_channels@32` | host VA at **offset 8** → clobbers `h_channel` **and `num_channels`** | the IDLE_CHANNELS memset (`nvkvm_stub.c:1481-1484`) that zeroes `[12,40)` | **the real one — see below** |
| `0x4f` | `NVOS34` (`:320-329`): `p_linear_address@16` (`NvP64`) | aux VA at offset 16, *overwriting* the guest's pointer with stub-owned memory (or nothing at all when `aux_size == 0`) | **H-3 hClient gate** (`0x4f` *is* in H-3's list, `nvkvm_isolate_handlers.c:2601`); the status readback (`nvkvm_stub.c:1670`) | no new pointer exposure beyond U-11's already-open state; the delta is the hClient bypass |
| `0x54` | `nv_ioctl_alloc_context_dma2` (`:522-534`) — **no `NvP64` at all** | host VA at offset 8 → clobbers `h_memory@8` / `h_class@12` | nothing pointer-shaped; `0x54` is *already* missing from H-3's list (see R-5) | low — RM rejects garbage handles |
| `0x57` | `NVOS46` (`:372-384`) — **no `NvP64`**; `offset@16` is a u64 object offset | aux VA at offset 16 → corrupts `offset` | **H-3 hClient gate** (`0x57` *is* in H-3's list, `:2600`) | low for the params; the delta is the hClient bypass |

**Checked and cleared: no host-address disclosure.** The writeback at
`nvkvm_stub.c:1605-1620` mirrors the `ptr_off` decision and zeroes offset 8 (for
`'d'` `0x41`/`0x54`) or offset 16 before the param buffer goes back to the guest.
And when `aux_size == 0` the inbound write stores an explicit `0`, not an address
(`:1048-1053`). So this is not a U-7-class address oracle in either direction.

**`0x41` is where the severity lives.** With `aux_size > 0`:

- the stub writes an 8-byte stub-mapped address at offset 8, so `num_channels@12`
  becomes the **high half** of that address — on x86-64 Linux userspace,
  `0x00007fxx`, i.e. of order 32,700;
- `p_clients@16`, `p_devices@24`, `p_channels@32` are left **exactly as the guest
  wrote them**, because the one control that would have cleared them is
  `'F'`-keyed;
- the NVIDIA frontend dispatches on NR ignoring type — an assertion this tree makes
  itself, in the comment that justifies A-5's guest-side fix
  (`src/guest/nvkvm_ioctl.c:137`) — so it executes `NV_ESC_RM_IDLE_CHANNELS` and
  walks three guest-named `NvP64` arrays for that many entries in the stub's mm.

That is an unbounded-length read of three fully guest-chosen addresses inside the
isolate, i.e. **precisely the G-2 defect the tree records as already found and
fixed** (`src/abi/nvgpu.h:402-410`: *"caused the host driver to read 56B from a 40B
buffer (OOB read of stub heap) and to interpret guest scalar fields as the
phClients/phDevices/phChannels pointers, then dereference those guest-controlled
values"*), reachable again through the sibling type value.

One further precondition, for accuracy rather than as a bar: the `'d'`/`0x41` arm
at `nvkvm_stub.c:1133-1145` reads a handle id from the first 4 bytes of aux and
returns `-EBADF` unless it resolves, so the attacker must place one of its own
handle ids there. Any legitimate handle satisfies it.

**Bounds.** Requires `nv->graphics` — compute-only VMs are unaffected, because the
graphics gate denies all `'d'` ioctls there (`nvkvm_isolate_handlers.c:2154-2162`)
and `nvkvm_req_open_nvidia_handle:311` refuses to open DRM/modeset devices at all.
Blast radius is the stub's address space: isolate-contained, the same bound as
U-1..U-5 — not QEMU, not the host, not another VM. The primitive is a read /
fault-oracle / DoS, not a demonstrated write.

**Severity: HIGH.** It defeats the only control the guest-pointer audit classifies
`ENFORCED`, on the specific claim that it *cannot* be defeated; it reopens a defect
the project already found and fixed once; and its entry condition is a single field
of the command word.

#### R-1.3 — proposed fix (specified, deliberately not implemented)

The missing control is a cross-check between the ioctl's type and the target
handle's device — a gate keyed on the real discriminator rather than a proxy for it,
which is exactly the A-1 correction. In `nvkvm_req_ioctl_on_isolate`, after the
handle lookup and **before** the `'d'` / NVKMS / `'F'` chain at
`nvkvm_isolate_handlers.c:2164`:

```
h = nvkvm_handle_get(&nv->handles, req->handle_id);   /* already done at :1824 */

reject unless one of:
    h->dev_id == NVKVM_DEV_UVM                       and the UVM branch is taken
    h->dev_id in [NVKVM_DEV_DRM_RD(0), NVKVM_DEV_DRM_RD(16))
                                                     and _IOC_TYPE(req->cmd) == 'd'
    h->dev_id == NVKVM_DEV_MODESET                   and req->cmd == NVKVM_NVKMS_IOCTL_CMD
    h->dev_id == NVKVM_DEV_CTL || h->dev_id in [NVKVM_DEV_GPU(0), ...)
                                                     and _IOC_TYPE(req->cmd) == 'F'
```

Notes for the implementer:

- **Fail closed, and refuse the ioctl** (`-EPERM` / `NV_ERR_NOT_SUPPORTED`), not
  the RM-shaped "succeed with a status" signalling used by the alloc-class and A-1
  gates. Those two use that shape because userspace treats a hard failure as fatal
  and they sit on paths a legitimate workload reaches; a type/device mismatch is
  never legitimate, so there is nothing to fall back gracefully to.
- **Also do what A-5 actually asked**: move the non-`'F'` default-deny above the
  `'d'` and NVKMS branches. The two changes are independent, and the ordering fix
  alone does not close R-1 — `'d'` is a *recognised* type, so it never reaches the
  deny arm no matter where the arm sits. The cross-check is the one that closes it.
- **`NVKVM_DEV_EVENTFD` (`0xFF`) needs a decision.** It is a handle type the stub
  accepts (`nvkvm_stub.c:1921`); establish which ioctl types, if any, are legitimate
  against it rather than letting it fall through either arm.
- **The stub should assert the same invariant independently.** QEMU's check is the
  boundary, but the stub already re-derives `job_type`/`job_nr` and is the process
  that actually issues the syscall; a matching check there makes the property hold
  even if a future request type reaches the stub without passing `:2164`, which is
  how R-4 and R-7 arose.
- **Regression test.** `tests/integration/test_adversarial.py:98-105` tests an
  *unrecognised* type (`'Z'`) and passes; it needs the *valid sibling* cases —
  `_IOWR('d', 0x41, …)` on a `/dev/nvidiactl` handle and `_IOWR('F', 0x4a, …)` on a
  render-node handle — which is exactly why the suite is green today.

### R-2 (NEW, MEDIUM — the reverse direction) — `'F'` on a DRM handle

The mirror case: `_IOC_TYPE == 'F'` with an NR in the DRM private range, aimed at a
render-node handle. It passes the frontend NR allowlist and skips
`nvkvm_drm_nr_allowed()` entirely — the same missing cross-check, opposite
direction. Two frontend-allowlisted NRs land on DRM ioctls the DRM allowlist
**deliberately denies** (audit G-3, `nvkvm_drm_allowlist.h:93-115`, for carrying raw
guest VAs with no marshalling):

| `_IOC_NR` | frontend meaning (allowlisted) | DRM meaning (G-3 denied) |
|---|---|---|
| `0x4a` | `NV_ESC_RM_VID_HEAP_CONTROL` | `DRM_NVIDIA_GEM_MAP_OFFSET` |
| `0x4e` | `NV_ESC_RM_MAP_MEMORY` | `DRM_NVIDIA_GEM_IDENTIFY_OBJECT` |

Both are `DRM_RENDER_ALLOW`, so the render node's own permission model does not
exclude them.

**Status: STRONGLY SUSPECTED BLOCKED, NOT CONFIRMED.** `drm_ioctl()` is believed to
reject a mismatched type outright (`DRM_IOCTL_TYPE(cmd) != DRM_IOCTL_BASE →
-ENOTTY`) before dispatching on `_IOC_NR`, which would make R-2 inert. **That could
not be verified from this repository** and must be checked against the kernel source
for the target kernel before anyone relies on it.

Two reasons not to close it on the strength of that belief:

1. It would make the boundary depend on an **out-of-tree** control the project does
   not own — the same anti-pattern flagged against `sec-easy-batch`'s U-13 closure.
2. The tree simultaneously asserts the *NVIDIA frontend* **does** ignore type
   (`src/guest/nvkvm_ioctl.c:137`) and relies on that assertion to justify A-5's
   guest-side fix. Exactly one of "the frontend ignores type" and "R-1 is not
   exploitable" can be true. If the frontend assertion is wrong, A-5's guest-side
   fix was unnecessary; if it is right, R-1 is live. **Settling R-1 and R-2 means
   reading both dispatchers, and it is the same afternoon's work.**

The R-1 cross-check closes R-2 as a side effect, which is the argument for doing the
structural fix rather than the targeted one.

---

## Newly discovered, most severe first

| # | severity | finding | where |
|---|---|---|---|
| **R-1** | **HIGH** | branch selection keys on attacker-controlled `_IOC_TYPE` with nothing tying it to the handle's `dev_id`. **Four** NR collisions, not one (`0x41`, `0x4f`, `0x54`, `0x57`); `0x41` as type `'d'` skips 14 gates including the one `ENFORCED` control and reopens G-2. U-3/A-1/alloc-class/ctrl-cmd/DUP are **out of reach** — verified | above |
| **DEAD-1** | HIGH (review hazard) | all 562 lines of `nvkvm_frontend.c` are unreachable and advertise three security invariants; U-7's "fixed on both paths" counts one of them | below |
| **R-3** | MEDIUM | alloc-class allowlist still keyed to `nr==0x2b` alone; the deliberate `0x3f` exclusion is unreachable on `nr==0x27` | below |
| **R-5** | MEDIUM | H-3's hClient gate under-covers its own stated scope by five allowlisted NRs | below |
| **R-2** | MEDIUM (unconfirmed) | the reverse direction — `'F'` on a DRM handle reaches the two G-3-excluded ioctls. Probably blocked by `drm_ioctl()`, but that is an out-of-tree control and could not be confirmed here | above |
| **R-7** | LOW–MEDIUM | `REALIZE_UVM_MAPPING` carries a guest-supplied `h_client` past H-3 | below |
| **R-4** | LOW–MEDIUM | `REALIZE_UVM_MAPPING` is a second route to UVM ioctls, gated by neither the UVM schema nor U-6 | below |
| **R-8** | LOW | the ring applies the ctrl-cmd allowlist but not the hClient gate (promotes a "Suspected" item to established) | below |
| **R-6** | LOW | H-3 is inert while `client_allow_n == 0` | below |

---

## Sweep 1 — reconciliation

Statuses below are **verified against the code at `68a35c0`**. "Doc" is what the
document said before this reconciliation; "Actual" is what the code does.
Corrections have been applied to the three source documents in the same commit as
this one.

### The two directions, and which one actually bit

**Direction 1 — "doc says fixed, code says otherwise."** The dangerous direction,
because nobody looks again. **One instance found, and it is R-1's origin:**

- **A-5** is marked **fixed**. Its host-side half was never written, and the half
  that *was* written is guest code the audit's own methodology excludes as a
  control. Everything else marked fixed is genuinely fixed.

Two more are *overstatements* rather than errors — the finding is closed but the
evidence is not what the doc says:

- **U-7** — "fixed on both paths"; one of the two paths is dead code (DEAD-1).
- **A-6**, **A-18** — flat "fixed" where the code itself documents a residual. Both
  are re-rated PARTIAL here.

**Direction 2 — "doc says open, code says fixed."** Nobody re-checks this direction
either, and it wastes the next reader's time or gets work redone. **Four instances,
all in `audit-boundaries-2026-08-20.md`:**

- **A-15** — the document **contradicts itself**: the status table says *fixed*, §6's
  prose still describes `clone3` as sitting in the allowlist and the `apply_seccomp()`
  comment as wrongly claiming to block "fork". The code matches the table:
  `clone3` is absent (`nvkvm_stub.c:2940-2988`) and the comment was corrected
  (`:3241-3246`, tagged F6-1). **§6's prose is stale.**
- **§6's three hardening items**, all listed as outstanding, all in fact fixed:
  `umount2` return now checked and fail-closed (`nvkvm_isolate.c:357-358`, R4-L2);
  the `sock_filter[96]` `EMIT` is bound-checked with a fail-closed `overflow` flag
  (`nvkvm_stub.c:2901-2907`, F8-1 — ~57 of 96 slots used); the `closefrom` fallback
  returns −1 and both call sites `_exit(126)` (`nvkvm_isolate.c:1826`, `:1940`).

Two more of the same shape outside that document:

- **P-8** — "partly fixed"; all five reverted items *and* the CI job are now
  restored.
- **P-9** — described as one-of-five heredocs escaped; all six were fixed by
  `929f467`, one day after the document was written.

The lesson is symmetric with the one that opened this file: a status is a claim
about code, and it decays in both directions.

### `audit-guest-pointers.md`

| # | doc | actual | evidence |
|---|---|---|---|
| U-1 | fixed | **CONFIRMED FIXED** | `ring_ctrl_must_punt()` calls `nvkvm_ctrl_cmd_allowed(inner)` at `nvkvm_stub.c:2551-2553`, **before** the `aux_size==0` return at `:2555`. One gate definition (`nvkvm_ctrl_allowlist.h:299`), two call sites. `test_ctrl_gate` built and run: PASS. |
| U-2 | fixed | **CONFIRMED FIXED** | `ptr_off` is decided from the cmd, not from `aux_size` (`nvkvm_stub.c:1017-1051`); when `ptr_off >= 0` it always writes the aux pointer or an explicit 0 (`:1048-1053`). Ring path likewise (`:2632-2636`). |
| U-3 | fixed | **CONFIRMED FIXED** on the `'F'` route | `nvkvm_isolate_handlers.c:2344-2352`; `fn` defaults to `0xffffffff` so a short param denies. Ring cannot reach `nr 0x4a` (`nvkvm_stub.c:2541`). **Subject to R-1.** |
| U-4 | fixed | **CONFIRMED FIXED** | every guard now has an `else aux_clear_ptr(...)`: `nvkvm_stub.c:1194-1206`, `:1281-1319`, `:1333-1360`. |
| U-5 | fixed | **CONFIRMED FIXED** | `clamp_inner_params_size()` `nvkvm_stub.c:897-925`, called on the worker path `:1058` and the ring path `:2643`. `test_stub_ptr_sanitize` built and run: 12/12 U-5 cases pass. |
| U-6 | fixed | **CONFIRMED FIXED** | all 16 VA-bearing schema rows carry a `va_mode`/`va_off` (`nvkvm_isolate_handlers.c:724-757`); `uvm_va_covers()` called `:1955`, `uvm_va_add()` `:2113`; `semaphoreAddress` zeroed unconditionally `:2054`. **See R-4 for a second route.** |
| U-7 | fixed **on both paths** | **PARTIAL — the live path only** | `zero_nvos64_rights()` (`nvkvm_stub.c:888-895`, called `:1063`) is live, unconditional, and unit-pinned (5/5 cases pass). The `nvkvm_frontend.c:126-129` half is correct as written but **unreachable** — see DEAD-1. The finding is closed; the evidence is half what the doc claims. |
| U-8 | open | **CONFIRMED OPEN** | `0x127` still allowlisted (`nvkvm_ctrl_allowlist.h:53`); no handling, no `gpuCount` bound anywhere in `src/`. Still blocked on the struct layout, as the doc says. |
| U-9 | open | **CONFIRMED OPEN** | `stub_mmap((void *)(uintptr_t)cmd->gva, ...)` `nvkvm_stub.c:2193`; `stub_munmap(...)` `:2201`; `gva` unbounded through `nvkvm_isolate_handlers.c:3513` → `nvkvm_isolate.c:3477`. `iso_mmap_tbl` records mappings, it does not constrain them. |
| U-10 | fixed | **CONFIRMED FIXED** | offset 16 written unconditionally for `'d'`/`0x45` (`nvkvm_stub.c:1027-1029`, `:1049-1053`); offset 32 zeroed `:1074-1080`. |
| U-11 | open | **CONFIRMED OPEN** | no host-side handling of `NVOS34.pLinearAddress`, `NVOS33.pLinearAddress`, `NVOS56.pOld/pNewCpuAddress` in either `nvkvm_stub.c` or `nvkvm_frontend.c`. Guest-side only, which is not a control. |
| U-12 | fixed | **CONFIRMED FIXED** | `nvkvm_stub.c:1330-1331`, `aux_clear_ptr(job.aux_buf, job.aux_size, 16)` on `inner_cmd == 0x202`. |
| U-13 | open | **CONFIRMED OPEN on `main`** | `0x2080110bu` still allowlisted (`nvkvm_ctrl_allowlist.h:132`), no handling. `sec-easy-batch` closes it by a vendor-source reachability argument with no code change — see the branch section. |
| U-14 | by design | **DOC TEXT STALE — corrected** | the section said *"`hClass` is **not** gated for nr 0x27"*. Since A-1 landed (`b26c56f`) `hClass 0x71` on `nr 0x27` **is** gated, by `iso_mmap_covers()` (`nvkvm_isolate_handlers.c:2410-2459`). Every *other* class on `nr 0x27` is still ungated — see R-3. |
| U-15 | closed by revert | **CONFIRMED CLOSED** | no `MAP_FIXED_NOREPLACE` call in `src/qemu/` (only an explanatory comment at `nvkvm_isolate_handlers.c:3469`); the surviving guest-influenced mmap maps at a QEMU-chosen sparse-window address (`:3391`). |
| **GP-**A-1 | fixed | **CONFIRMED FIXED — DOC MECHANISM STALE, corrected** | the gate is real (`nvkvm_isolate_handlers.c:2410`), checks `stub_mirrored` (`:114`) and wraps twice (`:99`, `:119`). But the doc said it *"requires full containment in **one** such entry, deliberately not a union"*. The code deliberately does the **opposite** — a union walk over adjacent host-installed chunks — and its own comment (`:66-80`) explains why: *"An earlier version of this function demanded containment in a single entry and would have refused every registration above 2 MiB."* The doc describes the superseded version. Security property (full unbroken coverage by host-installed ranges only) holds either way. |
| **GP-**A-2 | analysed, deliberately not added | **CONFIRMED UNCHANGED** | no `session_has_isolate` anywhere in `nvkvm_req_ioctl_on_isolate` (`:1794-3198`); `nvkvm_req_copy_handle_to_isolate` (`:612-620`) still performs no session validation, so the doc's bypass argument still holds. |

### `audit-boundaries-2026-08-20.md`

| # | doc | actual | evidence |
|---|---|---|---|
| **BD-**A-1 | fixed | **CONFIRMED FIXED** | `NVKVM_ISO_SYNC_TIMEOUT_MS` `nvkvm_isolate.c:604`; all non-ENTER_LOOP sync waits deadlined via `nvkvm_iso_slot_wait_or_die()` (`:2397,2427,2459,3141,3217,3304,3589`); ENTER_LOOP is slice-woken (`:756`). Stub polls its socket every 64 records (`nvkvm_stub.c:2473`, `:2735-2739`). |
| **BD-**A-2 | fixed | **CONFIRMED FIXED** | `SO_RCVTIMEO` `nvkvm_isolate.c:1584`; `shutdown()` `:2178` before `pthread_join`, `close()` `:2202-2211` after. |
| A-3 | fixed | **CONFIRMED FIXED** | guest zaps before release (`nvkvm_mmap.c:654` ahead of `:656-660`; zap at `:519-575`); host quarantine FIFO `nvkvm_mmap_host.c:693-800`. |
| A-4 | fixed | **CONFIRMED FIXED** | `nvkvm_main.c:2483-2489` bounds the second fetch against `aux_size`, not the ABI max. |
| **A-5** | **fixed** | **PARTIAL — the host-side half was never done. See R-1.** | The doc's fix had two halves. Guest half done (`nvkvm_ioctl.c:142-144`) — but that is **guest code, which this audit's own method excludes as a control**. Host half *not* done: the chain at `nvkvm_isolate_handlers.c:2164 / 2189 / 2224` still evaluates `'d'` first and the non-`'F'` deny last, so a type-`'d'` record still bypasses every `'F'`-keyed gate. **This is the dangerous direction and it is the origin of R-1.** |
| A-6 | fixed | **PARTIAL** | `MMAP_ON_ISOLATE` checks session + isolate (`:3276-3283`); `MUNMAP_ON_ISOLATE` now refuses a token the named isolate does not own (`:3623-3625`). But the code's own comment (`:3605-3620`) flags a residual the doc's table does not: `req->isolate_id` is guest-supplied with no session anchor on the wire, so naming a neighbour's isolate *and* its token still passes. Same residual as P-4. |
| A-7 | fixed | **CONFIRMED FIXED** | dedicated `loop_lock`/`loop_cond`/`loop_done` (`nvkvm_isolate.h:179-184`) separate from `sync_*` (`:155-169`); `sync_cmd_lock` held across the whole round-trip (`nvkvm_isolate.c:2380-2402`). |
| A-8 | partial | **CONFIRMED PARTIAL** | deadlined (`nvkvm_isolate.c:3214`, `:3302`) but `present_lock` (`:3190`→`:3223`) and `xiso_lock` (`:3284`→`:3309`) are still held across the full stub round-trip. Exactly as the doc says. |
| A-9 | open | **CONFIRMED OPEN** | the guest pump's ENTER_LOOP `wait_for_completion(&inf->done)` (`nvkvm_virtio.c`, ~`:477-531`) is still uninterruptible; release depends on the VMM's deadline. |
| A-10 | fixed | **CONFIRMED FIXED** | cache key carries `owner` (`nvkvm_present_egl.c:643-646`, set `:361`, `:1375`); invalidation on isolate death `:1389-1417` → `:892-933`, wired from `nvkvm_isolate_handlers.c:501` and `nvkvm_isolate.c:2350`. |
| A-11 | fixed | **CONFIRMED FIXED** | `0x3d05`/`0x3d06`/`0x3d08` all punted (`nvkvm_stub.c:2563-2566`) and all three have guest-side fd translation. The `aux_size==0`-before-check shape is still there and still benign, as §8 says. |
| A-12 | fixed | **CONFIRMED FIXED** | `nvkvm_handle.h:41` records `size`; check is subtraction-based and wrap-safe (`nvkvm_isolate_handlers.c:3288-3300`). |
| A-13 | fixed | **CONFIRMED FIXED** | EXIT sent under `write_lock` trylock, not `iso->lock`, with `MSG_DONTWAIT` (`nvkvm_isolate.c:2151-2160`). |
| A-14 | fixed | **CONFIRMED FIXED** | snapshot and re-check both under `iso->lock` (`nvkvm_isolate.c:737-741`, `:764-770`). |
| A-15 | table: fixed / §6: open | **CONFIRMED FIXED — §6 prose was stale, corrected** | no `clone3` in the filter (`nvkvm_stub.c:2940-2988`); the `apply_seccomp()` comment no longer claims to block "fork" (`:3241-3246`, tagged F6-1). The document contradicted itself; the table was right. |
| A-16 | partial (4 of 6) | **RESOLVED per item** | (1) UVM handle-id offset — **FIXED**, both sides read the same profile field (`nvkvm_ioctl.c:330-341`, `nvkvm_main.c:1331-1376`, `:2305-2307`). (2) unbounded pinning — **OPEN by choice**, `grep -rn RLIMIT_MEMLOCK src/` is empty. (3) migration/mmap leaks — **FIXED with the one documented residual still present** (`nvkvm_mmap.c:958-978`, bounded by `nvkvm_isolate_handlers.c:3683`). (4) handle across a lock drop — **CANNOT DETERMINE**, not re-traced; needs a ~65k-allocation repro, left as found. (5) transaction-id leak — **FIXED**, 14+ paired `nvkvm_txn_id_free()` sites. (6) `nvkvm_ring_has_work` — **FIXED**, both counters acquire-loaded (`nvkvm_ring.h:259-260`), ring pointer `READ_ONCE` (`nvkvm_main.c:339-345`). Net: **five of six done, one undetermined.** |
| A-17 | fixed | **CONFIRMED FIXED** | both gates default to a sentinel and always evaluate: ctrl `:2526-2530`, alloc-class `:2484-2488`. The same S-5 idiom is also on the DUP gate (`:2562-2571`) and the H-3 gate (`:2612-2624`). |
| A-18 | fixed | **PARTIAL — matches its own residual note** | pre-export `nvkvm_present_geom_ok(req, 0)` `:1272-1280`, post-export with `lseek` size `:1332-1340`. The residual holds: `if (buf_size && need > buf_size)` (`:1230-1237`) short-circuits the size check when `lseek` returns ≤ 0. |
| A-19 | fixed | **CONFIRMED FIXED** | guard is `>= 52` (`nvkvm_isolate_handlers.c:3040`). |
| A-20 | open | **CONFIRMED OPEN** | `virtio_nvgpu_device_unrealize()` (`virtio_nvgpu.c:1347-1375`) still has no cancel/drain; the two `thread_pool_submit_aio` sites (`:791`, `:849`) are uncoordinated with it. |
| A-21 | open | **CONFIRMED OPEN on `main`** | `nvkvm_kvm_slot_release()` (`nvkvm_mmap_host.c:947-958`) range-checks only, then pushes unconditionally (`:953`). **Fixed on `sec-easy-batch`** — see the branch section. |
| §6 ×3 | listed as open hardening gaps | **ALL THREE FIXED — doc stale, corrected** | `umount2` return now checked and fail-closed (`nvkvm_isolate.c:357-358`, R4-L2; caller `_exit(126)`). `EMIT` is bound-checked with a fail-closed `overflow` flag (`nvkvm_stub.c:2901-2907`, F8-1; ~57 of 96 slots used). `closefrom` fallback returns −1 and both call sites `_exit(126)` (`nvkvm_isolate.c:1826`, `:1940`). Safe direction — the doc *understated* progress — but it should not have stayed wrong. |

### `audit-prerelease-2026-08-21.md`

Five entries carried no closing verdict ("see below"). All five are resolved here.

| # | doc | actual | evidence |
|---|---|---|---|
| P-1 | fixed | **CONFIRMED FIXED** (with the residual the doc's own body already names) | all five sites fail closed: `nvkvm_main.c:1507-1512`, `:1541-1560`, `:1580-1600`; `nvkvm_drm.c:779-789`, `:857-867`. Residual, still open: `nvkvm_xrm_materialise` (`nvkvm_isolate_handlers.c:1694-1723`) derives `owner_iso` from `h->session_id` and then checks `session_has_isolate(nv, h->session_id, owner_iso)` — tautological, as its own comment concedes (*"confinement in depth rather than the only check"*) — and never validates the **importer** `iso_id` against any session. No `nv->graphics` gate. Confined to the VM (`nv->isolates` is per-VM), so this is the intra-VM boundary GP-A-2 argues is out of scope. |
| P-2 | fixed | **CONFIRMED FIXED** | row gone; only the EXCLUSIONS comment remains (`nvkvm_ctrl_allowlist.h:19-39`). Checked the wildcards explicitly: `0x20800513 & 0x8000 == 0` and `0x20800513 >> 16 == 0x2080 ≠ 0x2081` (`:301-304`), so **neither passthrough re-admits it**. It is the only exclusion in the block. |
| **P-3** | *see below* | **RESOLVED: the security half FIXED, the ASLR half OPEN** | Writable alias closed by `073ece8` (*"seal the stub memfd so the stub can't rewrite its own text"*, an ancestor of `68a35c0`): `MFD_ALLOW_SEALING` + `F_SEAL_SEAL\|SHRINK\|GROW\|WRITE` applied before exec, **fail-closed** if sealing fails (`nvkvm_isolate.c:1713-1743`); on-disk path opens `O_RDONLY` (`:1882-1892`). Non-PIE remains true, but `src/stub/Makefile:8-15` no longer *claims* PIE. A real static-PIE conversion exists on `sec-easy-batch` (`91d49d0`), unmerged. |
| **P-4** | *see below* | **RESOLVED: PARTIAL** | `nvkvm_req_realize_uvm_mapping` — **FIXED** (`nvkvm_isolate_handlers.c:4117-4125`, checks `fh->session_id == req->session_id` *and* `session_has_isolate`); `fd_handle_id` is now read (`:4116`), so the doc's *"grep returns nothing"* no longer holds. `nvkvm_req_munmap_on_isolate` — **STILL OPEN**, and the doc was right that it needs a protocol field: `struct nvkvm_req_munmap_on_isolate` is still `{isolate_id, mmap_token}` (`src/common/nvkvm_proto.h:484-487`). Partial mitigation landed (token must belong to the named isolate, `:3623-3625`) with an explicit KNOWN GAP comment (`:3605-3620`). |
| P-5 | open, needs a decision | **STILL OPEN on `main`; the comment half is done** | `nvkvm_iso_auto_select` still returns `NS \| SECCOMP` with no UID layer (`src/qemu/nvkvm_isolate_uid.h:537-548`). The false claim *was* corrected, by `d28201e`: `nvkvm_isolate_handlers.c:3266-3277` now says plainly that isolates are **not** uid-separated. The decision itself is recorded on `sec-easy-batch` (`2889c2e`) as "by design" — a decision not to fix, not a fix. |
| P-6 | fixed | **CONFIRMED FIXED** | `nvkvm_nvkms_cmd_allowed_ver()` `nvkvm_nvkms_allowlist.h:222-247`, fails closed on unknown version (`:233`), called with major+minor at `nvkvm_isolate_handlers.c:2200`. |
| P-7 | open, harness only | **CONFIRMED OPEN on `main`** | `scripts/run_test_vm.sh:264` still exports `$REPO_ROOT` with no `readonly=on`. Gated behind an opt-in flag on `sec-easy-batch` (`44be77f`), unmerged. |
| **P-8** | **partly fixed** | **RESOLVED: all five items and the CI job are now done** | (1) `.gitignore:34-35`, `git ls-files \| grep __pycache__` empty — `2d0a214`. (2) heredoc escaping — `2d0a214` then `929f467`. (3) README trim — present at HEAD. (4) `nvkvm_alloc_parms_probe_len()` `src/guest/nvkvm_main.c:1087`, used `:1886`, `:2036` — `c9e3875`. (5) `build_qemu.sh` patch series — `4b9e7c9` + `dcaff60`, `patches/` holds 10 files. `abi-parity` job at `.github/workflows/ci.yml:151`. **"partly fixed" is stale; this is fixed.** |
| P-9 | build correctness | **CONFIRMED FIXED** (post-dates the doc) | all six nested heredocs in `scripts/setup_mint_guest.sh:125-375` now escape every `$`, including the "worst instance" `\$(uname -r)`. Fixed by `929f467`, one day after the audit was written. |
| **P-10** | *see below* | **RESOLVED: (a) FIXED, (b) OPEN, (c) OPEN by design** | (a) fd leak — fixed: `reader_signal_sync_open` (`nvkvm_isolate.c:894-904`) and `reader_signal_present` (`:908-918`) now close the previous fd before overwriting (`6087e1b`). (b) prefault SIGBUS — **open**: the size check exempts `TYPE_NVIDIA` handles (`nvkvm_isolate_handlers.c:3299-3311`), the prefault loop at `:3436-3441` is unguarded, and there is no SIGBUS handler in `src/qemu/`. (c) `loop_lock` across a deadline-less wait — **open, deliberately**: `nvkvm_isolate.c:3064` → `:3076` (`timeout_ms = 0`) → `:3090`, with a design-rationale comment at `:3057-3060`. |
| **P-11** | *see below* | **RESOLVED: FIXED** | `struct nvos57_parameters` is now 24 bytes with `status` at offset 20 (`src/abi/nvgpu.h:273-283`); the stub's offset table has `case 0x35: off = 20` (`nvkvm_stub.c:1685`) and the guard `(off+4) <= param_size` is now `24 <= 24` → true (`:1704`). The guest sizes the buffer from the same struct (`nvkvm_ioctl.c:202-203`). The silent `NV_OK`-regardless-of-RM behaviour on `RM_SHARE` is gone. |
| **P-12** | *see below* | **RESOLVED: FIXED** | `xrm_handles`/`xrm_n` reset in `alloc_isolate_slot()` under `xrm_lock` (`nvkvm_isolate.c:1539-1540`), with a comment explaining why slot-claim is the right place rather than `nvkvm_isolate_kill()`. |
| P-13 | fixed | **CONFIRMED FIXED** | `nvkvm_fd_clear_of_stdio()` (`nvkvm_isolate.c:455-463`, `F_DUPFD_CLOEXEC` min 3) called in both spawn paths before `dup2(sv[1], STDIN_FILENO)`: `:1758-1766` and `:1888-1890`. |
| P-14 | open | **CONFIRMED OPEN** | `nvkvm_isolate_ring_setup()` (`nvkvm_isolate.c:2823`) → `sync_sendmsg_recv()` (`:2923`) → `nvkvm_iso_slot_wait_or_die()` (`:788-795`), which still calls `nvkvm_isolate_declare_dead()` on `-ETIMEDOUT`. The graceful-degradation comment is still wrong. |
| P-15 | fixed | **CONFIRMED FIXED, exhaustively** | `nvkvm_isolate_table_init()` sets all five descriptor fields to −1: `sock_fd` `:1409`, `sync_open_fd` `:1410`, `ring_memfd` `:1411`, `ring_kvm_slot` `:1412`, `present_fd` `:1434`. Cross-checked against every fd-typed field declared in `nvkvm_isolate.h` (`:101`, `:161`, `:199`, `:250`, `:255`) — none missing. |

---

## `sec-easy-batch` — what is pending, and what it does not close

The branch (`a886044`) is a **linear, unmerged descendant of `main`** — merge-base is
`68a35c0` itself, `merge-tree` reports no conflicts, and
`git merge-base --is-ancestor a886044 main` is false, so none of it has landed. Six
commits, +1077/−68 across 19 files.

| finding | what the branch does | closes it? |
|---|---|---|
| **P-3** | `63ad43b` records that the writable-alias half was already fixed by `073ece8`; `91d49d0` converts the stub to a genuine `static-pie` (`-static-pie -Wl,-z,relro,-z,now`, a `verify-pie` target that fails the build if `readelf -h` is not `DYN`, and a `_DYNAMIC` self-relocation fix). 4 runs, 4 different load bases. | **Yes, both halves.** Not GPU-tested. |
| **P-5** | `2889c2e` is **docs only** — flips the row to "by design / decided", records that namespaces not uid are the boundary. `nvkvm_iso_auto_select` is unchanged. | **No.** This is a decision *not* to fix, presented in the table as `**decided**`. A casual reader will misread it as resolution. It also leaves P-5's second consequence (RM's `osValidateClientTokens` not separating isolates) open — which is what R-5's severity rests on. |
| **P-7** | `44be77f` makes the 9p export `readonly=on` by default and puts read-write behind `NVKVM_DEV_HARNESS_INSECURE_RW=1`, with a banner naming the exploit chain; `run_remote_test.sh` forwards the flag, defaulting to 0. | **Narrows, does not remove.** The harness still needs RW for the first-boot module build, and setting the flag restores the exact chain. Honestly described as "gated", not "fixed". |
| **A-21** | `9daf4b6` adds a per-slot `kvm_slot_live[]` bitmap under `kvm_slot_lock`; `nvkvm_kvm_slot_release()` refuses and logs a slot that is not live; `nvkvm_kvm_slot_alloc()` refuses an already-live freelist entry; `kvm_remove_memory_region()` gains a range check. New `tests/unit/test_kvm_slot.c` (12 cases) extracted from production source via `NVKVM_KVM_SLOT_POOL_BEGIN/_END` markers so it cannot drift. | **Yes.** Fails closed, logged, pinned by an extracted test. |
| **U-13** | `a886044` is **docs + one comment**. The command stays allowlisted; no `aux_clear_ptr`. The argument: `subdeviceCtrlCmdFifoDisableChannels_IMPL` refuses the whole control with `NV_ERR_INSUFFICIENT_PERMISSIONS` when the field is non-NULL and privLevel < `RS_PRIV_LEVEL_KERNEL`, which no ioctl-path client can reach. | **By argument only.** It rests entirely on closed-source driver behaviour the project does not control, with no defence-in-depth — unlike its sibling U-12, which got a host-side clear. If either upstream check regresses, this reopens silently. |

The branch also flags, without fixing, the `hClientList[64]` cross-VM blind spot in
the same struct — which is the same family as R-5.

---

## Sweep 2 — gate coverage

For each gate: what it protects, its reachability condition, and whether every route
to the protected thing is covered.

| gate | site | reachability condition | coverage verdict |
|---|---|---|---|
| frontend NR allowlist | `nvkvm_isolate_handlers.c:2285` | `_IOC_TYPE=='F'` | **GAP — R-1/R-2.** Guest chooses the type. |
| alloc-class allowlist | `:2484` | `type=='F' && nr==0x2b` | **GAP — R-3** (below). `nr==0x27` also allocates a guest-chosen class. |
| ctrl-cmd allowlist | `:2530` (QEMU), `nvkvm_stub.c:2553` (ring) | `type=='F' && nr==0x2a` | **COVERED** on both routes since U-1's fix. One definition, two call sites (`nvkvm_ctrl_allowlist.h:299`). |
| NVKMS inner-cmdType allowlist | `:2200` | `req->cmd == NVKVM_NVKMS_IOCTL_CMD` (exact) | **COVERED** for the NvKmsIoctl entry. Any other `'m'` cmd falls to the non-`'F'` deny at `:2224`. |
| NVOS32 function gate (U-3) | `:2344` | `type=='F' && nr==0x4a` | **COVERED** on the `'F'` route (fails closed on short param). Reachable only via `nvkvm_req_ioctl_on_isolate`; the ring rejects everything but `nr==0x2a` (`nvkvm_stub.c:2541`). Subject to R-1's type caveat. |
| UVM schema table | `:1829` (`nvkvm_uvm_lookup`) | `h->dev_id == NVKVM_DEV_UVM` | **GAP — R-4** (below). `REALIZE_UVM_MAPPING` is a second route to UVM ioctls. |
| U-6 VA ownership (`uvm_va_covers`) | `:868`, called `:1955` | UVM schema rows with `va_mode` USE/FREE | **COVERED** for the schema route. Same R-4 caveat. |
| A-1 `iso_mmap_covers` | `:96`, called `:2433` | `type=='F' && nr==0x27 && (hClass==0x71 \|\| unclassifiable)` | **COVERED for `0x71`.** See R-3 for the residue. |
| H-3 hClient ownership | `:2594` | `type=='F' && nv->client_allow_n > 0` && NR in an explicit 11-entry list | **GAP — R-5, R-6, R-7** (below). |
| S-2 session/isolate checks | `:3279` (mmap), `:4117` (realize), `:1306` (close-on-isolate), `:1511` (xiso import), `:1722` (mapva) | per-handler | **COVERED** for the five handlers that carry a `session_id`. `MUNMAP_ON_ISOLATE` cannot (`:3606-3623`) — see P-4. |
| DRM NR allowlist | `:2170` | `_IOC_TYPE=='d'` | **GAP — R-1/R-2.** |
| graphics gate | `:2154` (ioctl), `:311` (handle open) | ioctl-side keyed on type; **open-side keyed on `dev_id`** | **COVERED** — `nvkvm_req_open_nvidia_handle:311` is the authoritative one and is device-keyed, as its own comment says. The ioctl-side copy is defence-in-depth only. |
| seccomp | `nvkvm_stub.c:2897`+ | applied after worker spawn | **COVERED** — see A-15/§6 rows in Sweep 1. `clone3` removed, `EMIT` bound-checked (F8-1, `:2885-2891`). |

### R-3 (NEW, MEDIUM) — the alloc-class allowlist is still keyed to `nr == 0x2b` alone

A-1 fixed the *instance* (`hClass 0x71` on `nr 0x27`) and left the *structure*.
`nvkvm_alloc_class_allowed()` is called only under `if (nr == 0x2b)`
(`nvkvm_isolate_handlers.c:2484`). `NV_ESC_RM_ALLOC_MEMORY` (`nr 0x27`) also takes a
guest-chosen class at param offset 12 (`struct nv_ioctl_nvos02_parameters_with_fd`,
`src/abi/nvgpu.h:287-291`), and the gate at `:2410` inspects it **only to test for
`0x71`**. Every other class value on `nr 0x27` is forwarded with no class check at
all.

The allowlist header states two deliberate exclusions following nvproxy
(`nvkvm_fe_alloc_allowlist.h:12-15`): privileged memory `0x3f` and OS_DESCRIPTOR
`0x71`. `0x71` is now covered on both routes. **`0x3f` is not** — it is excluded on
`nr 0x2b` and unreachable-as-a-check on `nr 0x27`.

This is the same sentence the A-1 write-up used about itself: *"a deliberate
exclusion … unreachable on the one route that used it."*

**CANNOT DETERMINE** from this tree whether `RmAllocMemory` honours `hClass 0x3f`,
or which other classes `nr 0x27` can instantiate — that needs
`open-gpu-kernel-modules`, which is not checked out on this machine. The structural
mismatch is not in doubt; the exploitability is. **Recommended:** apply
`nvkvm_alloc_class_allowed()` to `nr 0x27` as well, with `0x71` continuing to route
to the `iso_mmap_covers()` test instead of a flat deny (U-14 is a live feature).

### R-4 (NEW, LOW–MEDIUM) — `REALIZE_UVM_MAPPING` is a second route to UVM ioctls, gated by neither the UVM schema nor U-6

`nvkvm_req_realize_uvm_mapping` (`nvkvm_isolate_handlers.c:4092`) sends
`ISOLATE_CMD_REALIZE_UVM_FD` to the stub, which opens its own `/dev/nvidia-uvm`
(`nvkvm_stub.c:2313-2314`) and replays five UVM ioctls from a guest-supplied state
snapshot: `UVM_INITIALIZE`, `UVM_REGISTER_GPU`, `UVM_REGISTER_GPU_VASPACE`,
`UVM_CREATE_RANGE_GROUP`, `UVM_ALLOC_SEMAPHORE_POOL` (`nvkvm_stub.c:2327-2390`).
None passes `nvkvm_uvm_lookup()` or `uvm_va_covers()`.

Severity is genuinely lower than U-6's, and for the reason U-6 gives: these run in
the **stub's** mm, not QEMU's, so the isolate contains them. The QEMU handler does
bound the shape — `n_gpus`/`n_va_spaces`/`n_range_groups` caps (`:4146-4151`), exact
`intent_size` match (`:4159-4162`), `p->base == req->gva && p->length == req->length`
(`:4165-4168`), page-aligned length ≤ 1 TiB (`:4185-4189`). Recorded because "the UVM
schema is the gate on UVM ioctls" is a claim a reader will make from `:1829` and it
is not true of this route.

### R-5 (NEW, MEDIUM) — H-3's hClient gate under-covers its own stated scope by five NRs

The gate's comment (`nvkvm_isolate_handlers.c:2586-2592`) claims it is authoritative
for **"EVERY forwarded RM 'F' ioctl that carries an hClient at param offset 0"**, then
enumerates eleven NRs (`:2596-2603`). Five frontend-allowlisted NRs carry `h_client`
at offset 0 and are **not** in that list:

| NR | struct | `h_client` at offset 0 | in fe allowlist | in H-3 list |
|---|---|---|---|---|
| `0x41` `NV_ESC_RM_IDLE_CHANNELS` | `nv_ioctl_idle_channels` | `src/abi/nvgpu.h:413` | `nvkvm_fe_alloc_allowlist.h:32` | **no** |
| `0x54` `NV_ESC_RM_ALLOC_CONTEXT_DMA2` | `nv_ioctl_alloc_context_dma2` | `src/abi/nvgpu.h:524` | `:36` | **no** |
| `0x5e` `NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO` | `nvos56_parameters` | `src/abi/nvgpu.h:333` | `:39` | **no** |
| `0xce` `NV_ESC_ALLOC_OS_EVENT` | `nv_ioctl_alloc_os_event` | `src/abi/nvgpu.h:476` | `:48` | **no** |
| `0xcf` `NV_ESC_FREE_OS_EVENT` | `nv_ioctl_free_os_event` | `src/abi/nvgpu.h:483` | `:49` | **no** |

So a guest can name a foreign VM's RM client on five ioctls that a control existing
precisely to stop that does not inspect. `NV_ESC_ALLOC_OS_EVENT` is the sharpest —
it registers an OS event against `(hClient, hDevice)`.

Residual risk is bounded by RM's own client scoping, and P-5 records that
`osValidateClientTokens` rejects only when euid *and* pid both differ — so on a host
running two VMs under the same uid, RM's check does not separate them either. That
combination is the reason H-3 exists. **The comment is also wrong as written and
should be corrected whether or not the list is extended.**

### R-6 (NEW, LOW) — H-3 is inert while `client_allow_n == 0`

`if (_IOC_TYPE(req->cmd) == 'F' && nv->client_allow_n > 0)`
(`nvkvm_isolate_handlers.c:2594`). Before the VM's first root-client allocation the
gate does not run at all, so any `hClient` value passes. This is presumably the
bootstrap allowance, but the `hc != 0 && hc != (uint32_t)-1` and `is_root_alloc`
tests at `:2632-2640` already handle bootstrap, so the outer predicate looks
redundant as well as widening. Worth a look; not obviously exploitable.

### R-7 (NEW, LOW–MEDIUM) — `REALIZE_UVM_MAPPING` carries a guest-supplied `h_client` past H-3

`struct` field `h_client` in the UVM state snapshot (`src/common/nvkvm_proto.h:641`)
is passed to the stub and forwarded into `STUB_UVM_REGISTER_GPU_VASPACE`
(`nvkvm_stub.c:2355-2358`). `grep` for `h_client` in
`nvkvm_isolate_handlers.c` past line 4092 returns nothing — QEMU never checks it
against `nvkvm_client_allow_has()`. H-3 is keyed on `_IOC_TYPE=='F'` inside
`nvkvm_req_ioctl_on_isolate`; `NVKVM_REQ_REALIZE_UVM_MAPPING` is a different request
type and never enters that function.

Same bound as R-5: RM's own `rmCtrlFd`-scoped validation is the real backstop, and
H-3 is defence-in-depth. Recorded because the gate claims a scope it does not have.

### R-8 (NEW, LOW) — the ring path applies the ctrl-cmd allowlist but not the hClient gate

`ring_ctrl_must_punt()` calls `nvkvm_ctrl_cmd_allowed()` (`nvkvm_stub.c:2553`) —
U-1's fix, and correct. It does **not** apply `nvkvm_client_allow_has()`, which the
slow path applies to every `NV_ESC_RM_CONTROL` (`nvkvm_isolate_handlers.c:2594`).
`audit-prerelease-2026-08-21.md`'s "Suspected" section already names this
(*"The ring path skips the `hClient` gate"*); this reconciliation **confirms it against
the code** and promotes it from suspected to established. Residual risk low for the
reason the U-1 re-rating gives, but it is a documented gate that is not on one route.

---

## Dead or unreachable code that resembles a control

### DEAD-1 (NEW, HIGH as a review hazard) — all of `nvkvm_frontend.c` is unreachable, and it advertises three security invariants

`handle_ioctl` was removed for exactly this reason, and the removal note
(`src/qemu/virtio_nvgpu.c:371-382`) says why: *"A reviewer (and an auditor) can find
that line, conclude the pointer is handled, and be wrong; that is how A-1
survived."* The removal did not follow the call graph one step further.

- `nvkvm_dispatch_ioctl()` (`src/qemu/nvkvm_dispatch.c:159`) now has **zero**
  callers. `grep -rn nvkvm_dispatch_ioctl src/` returns only its declaration
  (`virtio_nvgpu.h:453`), its definition, and a comment.
- Every exported function of `nvkvm_frontend.c` — `nvkvm_handle_rm_alloc` (`:86`),
  `_rm_free` (`:225`), `_rm_control` (`:322`), `_rm_dup_object` (`:380`),
  `_register_fd` (`:409`), `_alloc_os_event` (`:436`), `_free_os_event` (`:471`),
  `_simple_ioctl` (`:482`) — is called **only** from `nvkvm_dispatch.c:165-417`.

So the whole 562-line file is dead at runtime. It is still compiled and linked,
which is what makes it look alive, and its header comment
(`src/qemu/nvkvm_frontend.c:18-22`) reads as a specification of live controls:

> *Security invariants: We verify all handles in ALLOC/FREE/CONTROL requests
> against the per-client object graph before invoking the real driver. We ensure
> `NV01_ROOT_CLIENT` allocations are unprivileged (no admin caps). We do not allow
> handles from one session to appear in another session's requests.*

None of those three executes. This is the `handle_ioctl` species at five times the
size, and it is load-bearing for at least one recorded status:

**U-7's "FIXED on both paths" is really "fixed on one path".** The
`nvkvm_frontend.c` half (`:126-129`, the zeroing made unconditional) is in dead
code. The `nvkvm_stub.c` half (`zero_nvos64_rights`, `:888-895`) is on the live path
and is genuinely correct and unit-pinned — so the finding *is* closed, but the doc
overstates the evidence, and the next reader will count two paths where there is one.

**This is the third security fix known to have landed in `nvkvm_dispatch.c` and been
dead**, after:
- the `IDLE_CHANNELS` neutralisation (`nvkvm_stub.c:1470-1472`: *"The earlier
  dispatch.c fix was dead code (never wired into the IOCTL_ON_ISOLATE path)"*), and
- the `MAP_MEMORY`/`UNMAP_MEMORY` VA table (`nvkvm_isolate_handlers.c:1560-1564`:
  *"The only code that wrote the field host-side sits inside nvkvm_dispatch.c's
  `#if 0` block, so every unmap reached the driver with VA 0"*),
- plus `p_memory = 0` itself (A-1).

Four for four. **Recommendation:** delete `nvkvm_dispatch.c` and `nvkvm_frontend.c`,
or move them out of the QEMU link and under `tests/` where `test_dispatch.c` can
still build against them. Keeping them linked is what sustains the illusion. At
minimum, put the same "not a control, nothing reaches this" banner at the top of
`nvkvm_frontend.c` that `virtio_nvgpu.c:371` now carries.

### DEAD-2 (checked, benign) — the remaining `#if 0` blocks

Three exist and none resembles a control:
- `src/qemu/virtio_nvgpu.c:66-77` — `host_dev_path()`, a device-path table, labelled
  a tombstone.
- `src/qemu/virtio_nvgpu.c:248-514` — the legacy `handle_open`/`_close`/`_mmap`/
  `_munmap`, labelled dead as of Step 3d.1. `handle_ioctl` is excised from this block
  entirely, not merely disabled.
- `src/guest/nvkvm_virtio.c:555+` — the legacy guest `nvkvm_virtio_ioctl`.

Recorded so the next sweep does not re-derive it.

---

## Numbering disambiguation — a recommendation, not a change

Four findings share two numbers:

| id | document | subject |
|---|---|---|
| A-1 | `audit-guest-pointers.md:95` | OS-descriptor pin (`hClass 0x71`), CRITICAL, fixed |
| A-1 | `audit-boundaries-2026-08-20.md` | blocking-under-lock / BQL stall, critical, fixed |
| A-2 | `audit-guest-pointers.md:155` | the missing session gate on `ioctl_on_isolate`, deliberately not added |
| A-2 | `audit-boundaries-2026-08-20.md` | lifetime-race on isolate teardown, critical, fixed |

These IDs appear in commit messages (`b26c56f`, `ac87dfa`) and in code comments
(`nvkvm_isolate_handlers.c:57`, `:2382`, `:4101`), so they **must not be renumbered
unilaterally**.

**Recommended:** a per-document prefix, adopted going forward and applied to *new*
references only, with a disambiguation table (this one) in each document's header:

- `GP-` for `audit-guest-pointers.md` → `GP-A-1`, `GP-A-2`, `GP-U-1`…
- `BD-` for `audit-boundaries-2026-08-20.md` → `BD-A-1`, `BD-A-2`…
- `PR-` for `audit-prerelease-2026-08-21.md` → `PR-P-1`…
- `R-` for this document's own new findings → `R-1`…`R-8`, already applied.

Existing bare `A-1`/`A-2` references stay valid and resolve through the table. The
`U-*` and `P-*` series are already unambiguous and need no prefix; the stray
`S-*`/`G-*`/`H-*`/`C-*`/`F*-*`/`M-*`/`N-*`/`R*-L*` markers used in code comments are
not defined in any of the three documents and should get a glossary — see "What this
could not settle".

---

## What this reconciliation could not settle

Left open on purpose rather than guessed at:

- **R-2** — whether `drm_ioctl()` checks `_IOC_TYPE`. Needs the kernel source.
- **R-3** — which classes `NV_ESC_RM_ALLOC_MEMORY` can instantiate, and whether
  `hClass 0x3f` is meaningful on that route. Needs `open-gpu-kernel-modules`, which
  is not checked out on this machine.
- **U-8** — `busPeerIds` / `busEgmPeerIds` offsets inside
  `NV0000_CTRL_SYSTEM_GET_P2P_CAPS_PARAMS`. Same reason; the finding's own note says
  so and it is still true.
- **U-13** — whether RM consumes `pRunlistPreemptEvent`. `sec-easy-batch` closes it
  on a vendor-source reachability argument (see the branch section below); that
  argument cannot be re-derived from this tree.
- **The 13 unresolvable control IDs and the two rule-based passthroughs.** Unchanged
  since `audit-guest-pointers.md` §3. `nvkvm_ctrl_cmd_allowed()` still admits
  `cmd & 0x8000` and `(cmd>>16)==0x2081` unconditionally
  (`nvkvm_ctrl_allowlist.h:301-304`), an unbounded and unauditable set.
- **The stray marker glossary.** `S-1`, `S-2`, `S-5`, `G-1`…`G-8`, `H-1`…`H-3`,
  `M-1`, `M-A`, `N-1`, `N-2`, `F5-1`, `F6-1`, `F8-1`, `FF-1`, `P2-1`, `P2-2`,
  `R2-L1`, `R4-L1`, `R4-L2`, `C7` and `#NN` issue numbers all appear in code
  comments as though they were audit IDs. None is defined in any of the three audit
  documents, so a reader cannot check any of them. This reconciliation's own
  findings are numbered `R-1`…`R-8` **specifically to avoid** the pre-existing
  `N-1`/`N-2` markers in `nvkvm_isolate_handlers.c:3818` and `nvkvm_stub.c`; that
  near-miss is itself evidence that the marker space needs a registry.
- **Everything requiring hardware.** No claim in this document was executed against
  a GPU. Two unit suites were built and run (`test_stub_ptr_sanitize`,
  `test_ctrl_gate`); everything else is static.
