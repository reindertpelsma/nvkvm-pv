# Audit: pre-release review, 2026-08-21

**Method:** static analysis only — no GPU, no VM, no execution. Adversarial read
of the boundaries named in [`SECURITY.md`](../../SECURITY.md), against the
vendor source in a local `open-gpu-kernel-modules` checkout where the host
driver's own behaviour mattered.

**Scope note.** This follows
[`audit-boundaries-2026-08-20.md`](audit-boundaries-2026-08-20.md), which
covered the same boundaries the day before. The most serious finding here is in
code that **did not exist** when that audit was written: the cross-isolate fd
relay landed on 2026-08-21 with the NCCL shared-memory fix.

**Like its predecessor, this names locations, not techniques.**

---

## Status at a glance

| # | severity | class | direction | status |
|---|---|---|---|---|
| P-1 | **critical** | fail-open validation | guest process → another guest process | **fixed** — `71a490f` |
| P-2 | **critical** | allowlist too permissive | guest process → host kernel | see below |
| P-3 | high | sandbox self-modification | isolate → isolate | see below |
| P-4 | high | missing ownership check | guest kernel → VMM | see below |
| P-5 | high | design vs documentation | isolate → isolate | **open, needs a decision** |
| P-6 | high | version drift in a gate | guest process → host NVKMS | **open** |
| P-7 | high | dev harness | guest → host root | **open, harness only** |
| P-8 | high | process | — | **partly fixed** — see "the revert" |
| P-9 | medium | shell quoting | — | build correctness |
| P-10 | medium–high | liveness ×3 | isolate → VMM | see below |
| P-11 | medium | struct size disagreement | guest → host | silent wrong answer |
| P-12 | medium | state reuse | guest process → VM-wide DoS | see below |

---

## P-1 — the fd relay accepted a guessed integer (FIXED, `71a490f`)

The relay's stated security argument is that entitlement comes from **fd
possession**, "which cannot be forged the way a guessable cookie could". Five
translation sites made it exactly a guessable cookie:

```c
__s32 hid = guest_fd_to_handle_id(gfd);
if (hid >= 0)
        memcpy(aux_buf, &hid, sizeof(hid));   /* no else */
```

`guest_fd_to_handle_id()` returns `-EBADF` for any integer that is not an open
nvkvm fd, so an unprivileged guest process could pass `17` and have `17`
forwarded in the field QEMU reads as a **VM-global handle_id**.
`nvkvm_xrm_prepare()` accepts anything `> 0`; `nvkvm_xrm_materialise()` confirms
only that the handle exists and belongs to a *different* isolate, then relays a
dup of that isolate's host fd.

Amplifiers: handle ids are sequential from 1, so enumeration is trivial; ctrl
`0x3d08` returns `deviceInstance` on success, giving an oracle; and
`nvkvm_xrm_prepare` has **no `nv->graphics` gate**, unlike PRESENT and
XISO_IMPORT — so it was reachable on the default headless compute configuration.

Three tells that this was an oversight rather than a decision, all of which also
mean the fix cannot break anything:
- all **seven** sibling sites in `nvkvm_ioctl.c` already failed closed;
- the legitimate path never takes the branch (a real SCM_RIGHTS nvkvm fd always
  translates), so NCCL is unaffected;
- the one check present was tautological — `session_has_isolate()` called with an
  owner derived from the handle being checked.

Fixed by matching the siblings at all five sites. A sixth translation
(`rm_ctrl_fd`) already failed safe by substituting 0, which the relay rejects.

**Note for whoever revisits the relay:** `audit-boundaries-2026-08-20.md` §4
says *"the export/import brokers are correctly gated — both (isolate, handle)
pairings are verified"*. That was true of the only broker that existed on
2026-08-20. It is **not** true of the one added on 2026-08-21, which verifies no
pairing at all beyond "different isolate". The dma-buf broker
`nvkvm_req_xiso_import` remains correctly gated and is the model to follow.

---

## P-2 — `0x20800513` in the ctrl allowlist

`NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2`, allowed with no justifying comment
and **not present in gVisor nvproxy's set**, which that header otherwise tracks.
In the host driver it runs a guest-controlled `NvU32` loop over a fixed
`instructionList[0x20]` **before the version check**, in a `NON_PRIVILEGED`
control, with a second read/write loop at the same indices. nvkvm applies the
allowlist and a 1 MiB aux cap and no per-command parameter validation.

**Honest framing:** this is not a bare-metal divergence — a local unprivileged
user on the host can call it too. The nvkvm delta is that it is a hand-added
entry handing the guest a host-kernel primitive the allowlist exists to withhold.

---

## P-3 — the stub holds a writable fd to its own executable, and is not PIE

`nvkvm_memfd_create("nvkvm_stub", MFD_CLOEXEC)` — **no `MFD_ALLOW_SEALING`**, and
memfds are always `O_RDWR`. It is `dup2`'d to fd 3 (which clears `FD_CLOEXEC`),
`closefrom(keep)` starts at 4, and the stub never closes it. The seccomp filter
constrains `prot` on `mmap`, so `MAP_SHARED, PROT_WRITE` on fd 3 is permitted —
a writable alias of the running text, defeating the `PROT_EXEC` denial that the
previous audit says bounds every other severity rating.

Compounding: the shipped stub is `ET_EXEC`, not PIE, while `src/stub/Makefile`
claims *"a fully static, position-independent executable"*. Fixed addresses, no
ASLR.

Fix is one line — `close(3)` in the stub, or seal before exec.

---

## P-4 — two handlers accept the caller's self-declared identity

Of the five handlers taking an `isolate_id`, three cross-check it with
`session_has_isolate` and two do not:

- **`nvkvm_req_munmap_on_isolate`** — ownership is tested against
  `req->isolate_id`, which the guest supplies. The request struct carries no
  `session_id` to check against, so closing this needs a protocol field.
- **`nvkvm_req_realize_uvm_mapping`** — the request **already carries**
  `session_id` and `fd_handle_id`, and `grep -rn "fd_handle_id" src/qemu/`
  returns nothing. Both are on the wire and never read. Cheap to fix.

Reachability: `isolate_id` is set by the guest module, so this needs a malicious
guest *kernel*, not an unprivileged guest process. That boundary is in scope.

---

## P-5 — isolates are not uid-separated on the default rung

`nvkvm_iso_auto_select` picks `NS | SECCOMP` when namespaces are available — the
common case. **`NVKVM_ISO_LAYER_UID` is only the fallback rung.** On the NS rung
every isolate maps to the same host euid.

Two consequences: `nvkvm_isolate_handlers.c` states the boundary as *"separate
**uid-separated** host processes"*, which is untrue on the default
configuration; and RM's own `osValidateClientTokens` rejects only when euid *and*
pid both differ, so a shared euid means the driver's cross-client guard does not
separate isolates either.

Mitigating: distinct PID and mount namespaces still block naming or ptracing a
peer. This is the removal of a layer the code claims, not a demonstrated
cross-isolate takeover.

**This needs a decision, not a patch** — either add UID to the default rung or
stop claiming it. The comment should be corrected either way.

---

## P-6 — the NVKMS gate compares raw enum indices that shift between branches

`nvkvm_nvkms_cmd_allowed(uint32_t)` is a plain switch with **no ABI-profile
argument**, and the file asserts the vendor enum *"grows by APPENDING, so old
values are stable"*. Read at vendor tags, it does not:

| tag | 17 | 18 | 60 |
|---|---|---|---|
| 535 / 545 / 550 / 565 / 570.86 / 570.172 | UNREGISTER_SURFACE | **GRANT_SURFACE** | ENABLE_VBLANK_SEM |
| 570.211 / 575 / 580 | REGISTER_SURFACE | UNREGISTER_SURFACE | SET_FLIPLOCK_GROUP |
| 595 / 610 | REGISTER_SURFACE | UNREGISTER_SURFACE | ENABLE_VBLANK_SEM |

On 535–570 hosts the gate passes `cmdType 18` believing it is
UNREGISTER_SURFACE and NVKMS dispatches `GrantSurface`. It shifts *within* one
nvkvm profile: `NVKVM_ABI_570` spans 570.172 and 570.211, which disagree.

Impact is bounded — `GrantSurface` requires owning the surface and
`ACQUIRE_SURFACE` is not allowlisted — so the finding is that the gate's
guarantee is void off the two branches it was measured on. The neighbouring UVM
gate *is* profile-corrected; this one is not, and that is the fix.

---

## P-7 — the dev harness gives guest root a path to host root

`scripts/run_test_vm.sh` exports the repo root to the guest over 9p
**read-write**, while two neighbouring shares use `readonly=on`.
`scripts/run_remote_test.sh restart` then runs
`bash $REMOTE_DIR/scripts/run_test_vm.sh` as root over ssh with no rsync first.
Guest root writes the script; the next `restart` executes it as host root. No
race required.

**Framing:** this contradicts
`audit-boundaries-2026-08-20.md`'s *"Nothing found gives a guest arbitrary code
execution on the host"* — it does **not** contradict `SECURITY.md`, which
disclaims that boundary. The guest builds its module on that share, so
read-only is not a one-line fix. It is a development harness and must never be
pointed at an untrusted guest.

---

## P-8 — a single-parent commit mislabelled `docs:` reverted five things

`4fece85` ("docs: DDX handoff…"), parent `96f5c24`, committed from a stale
working tree. **Nothing about it reads as a merge and it conflicted with
nothing.** It reverted:

1. `.gitignore` (and re-added four `__pycache__` binaries)
2. the nested-heredoc escaping in `scripts/setup_mint_guest.sh`
3. a README trim
4. **`nvkvm_alloc_parms_probe_len()` — the HOPPER_USERMODE_A fix**, 77 lines
5. **the patch-series conversion of `scripts/build_qemu.sh`** (431 → 728 lines)

Also the `abi-parity` CI job. (4) is restored by `c9e3875`, and while it was
missing `docs/reference/tested-platforms.md` carried five H100 rows at 28/28
that the tree could no longer produce.

**The lesson, now in [`CLAUDE.md`](../../CLAUDE.md):** after merging or
cherry-picking from a long-lived branch, diff against the base for files the
change had no business touching. A clean auto-merge is not evidence.

---

## P-9 to P-12 — the rest

- **P-9** `scripts/setup_mint_guest.sh` opens `<<EOF` unquoted at the outer
  level, so quoting the four inner delimiters has no effect — the outer shell
  already expanded the region. Only the `<<'RS'` block is correctly escaped, so
  the author knew the rule and applied it to one of five. Worst instance bakes
  the *build host's* kernel version into a guest systemd unit.
- **P-10** Three isolate → VMM liveness paths, and QEMU is unsandboxed so they
  land on the host: an unbounded fd leak where two reader-signal paths overwrite
  an fd without closing it (the correct idiom is two files away in
  `nvkvm_present_egl.c`); a prefault SIGBUS where a stub returning a short fd
  kills QEMU, which the code's own comment predicts; and `loop_lock` held across
  a deadline-less slot wait.
- **P-11** `NVOS57` is 16 bytes in `src/abi/nvgpu.h` and 24 with `status@20` in
  three other places in the same tree. The guest path uses the 16-byte one, so 8
  bytes are truncated and the stub's status read is gated `(off+4) <= param_size`
  → `24 <= 16` false → **`nvstatus` stays 0 and every RM_SHARE reports NV_OK
  regardless of RM's verdict.** A silent wrong answer on an access-control verb.
- **P-12** `xrm_handles`/`xrm_n` are reset neither in `alloc_isolate_slot` (which
  explicitly resets a dozen other fields) nor in `nvkvm_isolate_kill`. Ids wrap
  at 4096 and `slot = id % 4096`, so a reused slot gets the *identical* id and
  the `iso->id == isolate_id` guard does not catch it: the new occupant inherits
  the dead one's relay list. 4096 short-lived processes permanently break
  cross-isolate sharing VM-wide.

---

## Suspected — plausible, not traced

- **Inner `NVOS54.params_size` is never reconciled with `aux_size`.** The stub
  allocates exactly `aux_size` and leaves the guest's `params_size` untouched;
  QEMU checks only `> 1 MiB`. RM requires `paramsSize == pEntry->paramSize` and
  then copies that many bytes in and out. Likeliest outcome is a SIGSEGV in the
  stub (guest → isolate DoS) since the allocation is a fresh `mmap`; adjacency is
  kernel-layout-dependent and was not determined. Combined with P-3, a controlled
  variant would be code execution in the isolate.
- **Cross-VM `hClient` blind spot.** The per-VM gate reads param offset 0 only;
  several allowlisted operations carry a second client handle deeper
  (`GT200_DEBUGGER`'s `hAppClient`, `0x2080110b`'s `hClientList[64]`,
  `0x20801208`'s `hClient`). RM guards these with `osValidateClientTokens`, which
  P-5 shows is a no-op between isolates.
- **The ring path skips the `hClient` gate.** `ring_exec_one` applies the control
  allowlist but never `nvkvm_client_allow_has()`, which `allowlists.md` §9 calls
  authoritative. Residual risk low — RM's own validation is fd-scoped — but it is
  a documented gate that is not there. Same function does **no aux translation at
  all**, and `nvkvm_ctrl_cmd_allowed` wildcards `cmd & 0x8000` plus
  `(cmd>>16)==0x2081`: 167 explicit entries, and the wildcard admits 2³¹ values.

---

## Ruled out — checked and found clean

Recorded so the next reader does not repeat the work.

- **NVKMS `cmdType 60` as a host-blast-radius entry — refuted.** It requires
  `nvKmsOpenDevHasSubOwnerPermissionOrBetter`; `ALLOC_DEVICE` creates the dev
  with `isPrivileged = FALSE`, and `GRAB_OWNERSHIP`/`ACQUIRE_PERMISSIONS` are not
  allowlisted. On 595/610 — where 60 was actually measured as needed — it is
  `ENABLE_VBLANK_SEM_CONTROL`, not fliplock. The real defect there is P-6.
- **38 allowlisted-but-unsized classes — largely mitigated at HEAD.** `c9e3875`
  restored a generic `ap_size == 0` fallback on both alloc paths, forwarding a
  bounded ≤256-byte window instead of NULL. Residual: the window is capped at the
  page boundary so the forwarded length is allocation-address-dependent, and a
  future class larger than 256 B would silently truncate. Per-class rows are
  still absent and remain worth adding.
- `handle_fds` uninitialised → lookups returning fd 0: **no**, `handle_table_init()`
  sets all to `-1` and runs before any use.
- `export_fd_off` sentinel ambiguity at offset 0: **no**, initialised to `-1` and
  tested `>= 0`.
- Ring double-fetch: **no**, records are copied to private scratch.
- `aux_size` unbounded at the boundary: **no**, bounded by `nv->slot_size`.
- **A-12, A-17, A-18 verified genuinely fixed in code**, not merely marked fixed.
  Minor residual: A-18's size check is skipped when `lseek` returns 0, though hard
  dimension bounds cap the damage.
- **A-15 verified gone** — `clone3` is absent from the filter, and the stub's own
  use precedes `apply_seccomp()`.
- Lock-order `iso->lock → handles.lock` in `send_handle`: no inverse path found.
- `closefrom` fallback returns silently if `/proc` is unavailable, but
  `close_range` only fails with ENOSYS (< 5.9). It should `_exit()` rather than
  return.
- **No `setrlimit`, no cgroup, no `CLONE_NEWCGROUP` anywhere in `src/`** — a
  compromised isolate has unbounded memory. Consistent with the open A-16 item.

---

## Coverage

Read directly: `nvkvm_isolate_handlers.c`, `nvkvm_isolate.c`, `nvkvm_stub.c`,
`nvkvm_handle.c`, `nvkvm_main.c`, `nvkvm_ioctl.c`, `nvkvm_drm.c`,
`nvkvm_session.c`, all four allowlist headers, `nvkvm_proto.h`,
`nvkvm_isolate_uid.h`, `nvkvm_present_egl.c` (partial), plus independent
verification of A-12/15/17/18 and the vendor driver for P-2 and P-5.

**Not covered:** `nvkvm_mmap_host.c` window-allocator internals,
`src/guest/nvkvm_mmap.c` (guest process → guest kernel got only a shallow pass),
`nvkvm_present_egl.c` beyond the fd-close idiom, and the UVM descriptor table.
**Nothing was executed** — static analysis throughout.
