# The isolate model

An **isolate** is a host process, one per guest process, that holds the real
NVIDIA device fds and runs the real ioctls. It is the thing at the far end of
the forwarding path.

## Why one process per guest process

The obvious design — QEMU opens the devices and runs the ioctls itself — does
not work. The NVIDIA driver ties objects to the calling task in three separate
ways:

- **`rmclientValidate`** compares the RM client's `pOSInfo` against the calling
  task's `nvfp`. A client allocated by one process cannot be used by another.
- **UVM** binds its file's `nvfp` to the calling task's `mm` during
  `UVM_INITIALIZE`, and the matching `mmap` must come from the same `mm`.
- **`NV01_MEMORY_SYSTEM_OS_DESCRIPTOR`** pins the *calling task's* user pages.

So the process that runs the ioctls must be the process whose address space
mirrors the guest process's. The isolation property is a consequence of
satisfying the driver, not the other way round.

`src/common/nvkvm_proto.h:26-31`:

> Each guest userspace process (identified by `mm_struct`) has exactly one
> "isolate" host process that mirrors its virtual address space. The isolate has
> GPU device fds dup'd in via `SCM_RIGHTS` and replays every mmap at the same GVA
> (`MAP_FIXED`), so the NVIDIA driver sees valid mappings in `current->mm` when
> processing ioctls.

## Who opens what

| device | opened by | why |
|---|---|---|
| `/dev/nvidiactl`, `/dev/nvidiaN` | the **stub** | the file's `nvfp` lineage must match the process running RM ioctls |
| `/dev/dri/renderD128`, `/dev/nvidia-modeset` | the **stub** | same |
| `/dev/nvidia-uvm` | the **stub**, locally | see below |
| memfds | **QEMU** | not device files |
| eventfds | **QEMU** | stand-ins for guest eventfds |

`src/qemu/nvkvm_isolate_handlers.c:199-213`:

> UVM stays opened in QEMU (driver enforces opener-does-mmap, and mmap is done in
> QEMU for KVM region installation). The other devices … open inside the isolate
> so nvfp/mm lineage matches the process that runs RM ioctls.

But the stub then *drops* the UVM fd QEMU sends it, in favour of one it opened
itself (`src/stub/nvkvm_stub.c:255-266`):

> The NVIDIA UVM driver's `UVM_MM_INITIALIZE` rejects
> (`NV_ERR_INVALID_ARGUMENT`) when the file passed via `uvm_fd` was opened by a
> different mm than the caller. Since QEMU opens `/dev/nvidia-uvm` and passes it
> via `SCM_RIGHTS` to us, the file's owning mm is QEMU and our
> `MM_INITIALIZE` call is rejected.
>
> Fix: open `/dev/nvidia-uvm` twice in the stub itself (before seccomp), and when
> QEMU sends a `RECEIVE_FD` with `dev_id == NVKVM_DEV_UVM`, drop the
> `SCM_RIGHTS` fd and use one of our local opens instead.

**UVM *ioctls* nonetheless execute in QEMU's process**, not the stub's
(`src/qemu/nvkvm_isolate_handlers.c:1229-1237`) — because UVM's mmap must come
from the same `mm` that ran `UVM_INITIALIZE`, and that mmap is what installs the
KVM memory region. This is the one place privileged QEMU executes a
guest-named ioctl, which is why the UVM schema allowlist exists and is
default-deny.

The open handshake for stub-opened devices is worth following, because it
maintains an invariant: QEMU always holds a copy of every fd the stub opened.
QEMU reserves a handle slot with no fd (`nvkvm_handle_alloc_pending`), sends
`ISOLATE_CMD_OPEN_DEVICE`, and the stub replies with an `SCM_RIGHTS`-attached
copy in the same `sendmsg`, which QEMU attaches to the slot
(`src/qemu/nvkvm_isolate_handlers.c:250-285`,
`src/common/nvkvm_isolate_proto.h:202-214`).

## What the stub is

A freestanding static PIE (`src/stub/nvkvm_stub.c:4-7`):

> Freestanding static binary — built with `-nostdlib -static -fPIE` and linked
> against `-lgcc` only. No libc, no pthread; all primitives come from
> `stub_freestanding.h` (futex-based mutex/cond, raw syscall wrappers, clone3
> trampoline, tiny printf).

It applies its own `R_X86_64_RELATIVE` relocations before touching global data,
because it is `fexecve`'d from a memfd with no dynamic linker
(`src/stub/nvkvm_stub.c:2694-2745`).

It is embedded in the QEMU binary as a byte array produced by `xxd -i`
(`src/stub/Makefile:32-34`) and launched from an anonymous memfd — no file on
disk (`src/qemu/nvkvm_isolate.c:803-864`). `/usr/lib/nvkvm/nvkvm_stub` is a
fallback path used only when the embed is absent, and the build script warns at
length that an accidental fallback is silent
(`scripts/build_qemu.sh:77-89`).

**Threading**: one reader thread on the socket, 16 workers
(`NVKVM_STUB_WORKERS`, `src/stub/nvkvm_stub.c:433`). Non-IOCTL commands run
inline on the reader — they are fast and non-blocking. IOCTLs are queued and
matched back by `txn_id`, so QEMU can have up to `MAX_INFLIGHT` = 64
concurrently outstanding (`:444`).

**Handle table**: a flat array of 65536 fds indexed by handle id
(`src/stub/nvkvm_stub.c:442`, `:494-511`). Sized that way because QEMU's handle
ids are a global monotonic counter that never resets.

**Crash behaviour**: a `SIGSEGV` in the stub terminates it rather than returning
(`src/stub/nvkvm_stub.c:669-687`):

> a SIGSEGV here is a *stub-side* bad dereference (a bug in one of the
> embedded-pointer rewrites — the nvidia driver's own bad accesses return
> `-EFAULT`, they don't raise SIGSEGV). Returning from this handler re-executes
> the faulting instruction → an infinite SIGSEGV loop that pins the worker and a
> host core (a DoS). […] instead of looping we terminate the isolate cleanly […]
> One isolate dies; no host-core burn, no cross-tenant impact.

## Isolation modes

There are two strategies for making the isolate a sandbox, and they are **not
equally strong**. Namespace mode is the default and is what everything below
describes unless stated otherwise. UID-separation mode trades a large part of
that boundary away in exchange for being able to run at all in a container.

### Read this first if you are deploying in Docker

**Namespace mode cannot run under Docker's default profile.** This is not an
edge case for hardened or unusual hosts — it is the common case, and the way
most people would deploy nvkvm. `docker run` with no security flags at all
blocks `clone(CLONE_NEWUSER)` through its default seccomp profile and its
`docker-default` AppArmor policy. If you are running nvkvm inside a container,
you will need `NVKVM_ISOLATE_MODE=uid+chroot`, and you should learn that here
rather than from a spawn failure at the guest's first GPU ioctl.

**The kernel sysctls will lie to you about this.** Measured on a stock
`docker run` container (Ubuntu 24.04, kernel 7.0.0-28-generic, GPU and
`/dev/kvm` passed in):

```
kernel.unprivileged_userns_clone  1          <- permissive
user.max_user_namespaces          55416      <- permissive
unshare -U                        "unshare failed: Operation not permitted"
seccomp                           mode 2 (filtered)
apparmor                          docker-default (enforce)
CapEff  CHOWN DAC_OVERRIDE FOWNER FSETID KILL SETGID SETUID SETPCAP
        NET_BIND_SERVICE SYS_CHROOT AUDIT_WRITE SETFCAP
CAP_SYS_ADMIN  no    CAP_NET_ADMIN  no    CAP_SYS_PTRACE  no
```

Both kernel knobs read as permissive and user namespaces are still unavailable.
So nvkvm does **not** consult them, and neither should you: mode selection is an
explicit configuration choice, and namespace mode discovers the block by
attempting the `clone` and failing loudly with a message that names the likely
cause and the knob to change.

That capability set is exactly Docker's default minus `MKNOD` and `NET_RAW` —
the direction production deployments tighten anyway. Two things follow from its
shape, and they are what the UID mode is built around:

- `CAP_SETUID` and `CAP_SETGID` are **present** while `CAP_SYS_ADMIN` is
  **absent** — enough to drop to a unique high uid, not enough to create a
  namespace.
- `CAP_SYS_CHROOT` is **present** while `CAP_SYS_ADMIN` is absent — so
  `chroot(2)` is available even though `pivot_root(2)` (which needs
  `CAP_SYS_ADMIN` in the target mount namespace) is not. That is what the
  `+chroot` add-on below exploits.

### The knobs

`NVKVM_ISOLATE_MODE` (`src/qemu/nvkvm_isolate.c`, `nvkvm_isolate_cfg_resolve`)
names a rung on a degradation ladder. The rungs are ordered by the requirement
each one gives up:

| preset | layers | requires |
|---|---|---|
| `auto` **(default)** | probes, see below | nothing |
| `namespace` | namespaces + `pivot_root` + seccomp | `CAP_SYS_ADMIN`, or a userns not blocked by seccomp/AppArmor |
| `uid` | unique high UID/GID + seccomp | `CAP_SETUID` + `CAP_SETGID` |
| `uid+chroot` | as `uid`, plus `chroot("/dev")` | the above + `CAP_SYS_CHROOT` |
| `seccomp` | seccomp filter only | nothing |
| `none` | nothing | nothing, **plus an explicit acknowledgement** |

`seccomp` is a real rung, not filler: a hardened container that drops
`CAP_SETUID` kills UID mode outright while leaving the 20-syscall allowlist
completely functional. `PR_SET_NO_NEW_PRIVS` and `SECCOMP_SET_MODE_FILTER` both
work fully unprivileged.

Internally these are not six code paths but four independent, cumulative
**layers** — `seccomp`, `uid`, `chroot`, `ns` — and the preset names select
combinations of them. That is why combinations outside the preset list are
expressible without new code: `namespace+uid` applies the uid drop inside the
namespaces (supported, but see below — usually pointless), and `uid+chroot` is
the container recommendation. Every token turns on its layer, and **seccomp is
implied by any other layer**, because it is the floor of every real mode. The
only way to end up without seccomp is `none`.

`NVKVM_ISOLATE_UID_BASE` (default `500000`) is the first uid of the window;
each VM reserves `[base, base+4096)`, one uid per isolate slot.

### `auto`: the ladder, and the floor it never goes below

`auto` is the default. It tries **`namespace`, then `uid` (with `+chroot` when
`CAP_SYS_CHROOT` allows it), then `seccomp`** — and stops there.

**`auto` never selects `none`.** If even seccomp cannot be installed, device
realize fails. `none` is reachable only by explicit configuration together with
the acknowledgement below.

Each rung is decided by **attempting** it, never by inference:

- `namespace` — a real `clone()` with the production flag set, child reaped
  immediately (`nvkvm_iso_probe_namespaces`).
- `uid` — `capget()` for `CAP_SETUID`/`CAP_SETGID`, plus the `/dev/nvidiactl`
  permission check (`nvkvm_iso_probe_uid`).
- `seccomp` — `seccomp(SECCOMP_SET_MODE_FILTER, 0, NULL)`, which faults before
  installing anything, so `EFAULT` is the "supported" answer and nothing is
  applied to QEMU itself (`nvkvm_iso_probe_seccomp`).

Attempting is not merely more convenient than reading configuration; it is more
*correct*. The sysctls say user namespaces are available inside a stock Docker
container that blocks them (measured above). An operator who reads them picks
`namespace` and hits a runtime failure; `auto` attempts the clone and knows.

**How to find out what you actually got.** Two ways, and you should not have to
go looking:

1. **The log, at every start.** When `auto` lands on anything weaker than
   `namespace`, QEMU logs at *warning* level — the selected rung, the stronger
   rungs that were attempted, and the errno and likely cause for each rejection
   (e.g. "clone(CLONE_NEWUSER|...) failed: Operation not permitted. Typical
   under the default Docker seccomp/AppArmor profile ..."). Not a debug line;
   it is there whether or not `NVKVM_DEBUG` is set.
2. **A queryable property.** The resolved mode is exposed as the read-only QOM
   property `isolate-mode-active` on the virtio-nvgpu device:

   ```
   (qemu) qom-get /machine/peripheral/<id> isolate-mode-active
   "isolate sandbox: uid+chroot (uid window 500000..504095, 4096 slots)"
   ```

   The failure mode this exists to prevent is an operator who believes they
   have namespace isolation and has no way to check. A monitoring check can
   assert on this string rather than trusting the configured value.

### No silent fallback for an explicitly named mode

`auto` falls back by design, loudly. Everything else does not. If you name a
rung and it cannot run here — `uid` without `CAP_SETUID`, `uid+chroot` without
`CAP_SYS_CHROOT`, an unparsable mode string, a uid base that could collide with
real accounts — device realize fails with a message naming the problem and the
knob. Auto-downgrading a *pinned* mode would hand someone the weaker boundary
while they believed they had the stronger one.

### `none` is deliberately awkward to reach

`none` is legitimate — debugging, measuring what each layer costs, and
deployments where the whole VMM already sits inside someone else's sandbox. It
is also the one setting where a typo silently removes every boundary, so a bare
`mode=none` is refused. It requires:

```
NVKVM_ISOLATE_MODE=none
NVKVM_ISOLATE_UNSAFE_ACK=i-understand-this-removes-all-isolation
```

There was no precedent for a dangerous-option acknowledgement in this tree;
this establishes one. `none` is also the only mode that turns off the stub's
seccomp filter, which QEMU signals by passing `--no-seccomp` in the stub's
`argv` — chosen over an environment variable because the environment is
deliberately cleared before `exec`, and because argv shows up in `ps`, so a
stub running unconfined cannot hide. The stub applies the filter unless it sees
that exact flag, so a mangled or truncated argv fails **closed**.

The legacy `NVKVM_ISOLATE_NO_HARDEN=1` hatch maps to the **`seccomp`** rung,
which is exactly what it has always done: it turns off the namespaces and has
never turned off the stub's seccomp filter. Silently making an existing hatch
weaker than it was would be its own security bug.

### What UID mode replaces the boundary with

`CLONE_NEWUSER` is the keystone of the namespace sandbox: it is what lets an
unprivileged QEMU create the other four namespaces. Where it is unavailable the
isolate cannot be created at all and nvkvm cannot run, even though `/dev/kvm`
and the GPU are both present and usable.

UID mode replaces the namespace boundary with an ordinary POSIX DAC one: each
isolate runs as its own high uid/gid, so a process at uid 500001 cannot
`ptrace`, signal, or read the memory of one at uid 500002, and cannot open the
other's files. That much survives intact. Everything the namespaces were doing
*besides* separating the isolates from each other does not.

### The honest comparison

Read the columns right-to-left as the ladder degrades. This table is written for
someone deciding whether their threat model tolerates a given rung, not as a
disclaimer.

| property | `namespace` | `uid+chroot` | `uid` | `seccomp` |
|---|---|---|---|---|
| cross-isolate `ptrace` | blocked (pid ns + uid) | blocked (DAC) | blocked (DAC) | **NOT blocked** |
| cross-isolate signals | blocked (peer not nameable) | blocked (DAC) | blocked (DAC) | **NOT blocked** (but `kill` is not in the allowlist) |
| cross-isolate `/proc/<pid>/mem` | blocked (peer invisible) | blocked (DAC) | blocked (DAC) | **NOT blocked** |
| cross-isolate file access | blocked (empty RO root) | blocked (DAC) | blocked (DAC) | **NOT blocked** |
| isolates run as | same uid, own userns | **distinct uid each** | **distinct uid each** | the QEMU user |
| process visibility | own pid ns, sees only itself | **no `/proc` at all** | **every host process** | every host process |
| network | empty net ns | host stack (`/proc/net` hidden) | host stack, `/proc/net` readable | host stack |
| filesystem | RO tmpfs with only `/dev/nvidia*` | **`/dev` only** | **entire host filesystem** | entire host filesystem |
| `/dev` reach | dirfd whose `".."` is the sandbox root | all of `/dev`, `".."` clamped at `/dev` | all of `/dev` | all of `/dev` |
| `/dev/nvidia-uvm` | absent by design | **openable** | **openable** | openable |
| writable location | none | `/dev/shm` | `/tmp`, `/var/tmp`, `/dev/shm` | same |
| SysV IPC / UTS | fresh ns each | shared with host | shared with host | shared with host |
| capabilities dropped | yes | yes | yes | yes |
| `no_new_privs`, `dumpable=0` | yes | yes | yes | yes |
| seccomp allowlist (20 syscalls) | yes | yes | yes | yes |
| fds closed, stdio to `/dev/null`, env cleared | yes | yes | yes | yes |
| requires | `CLONE_NEWUSER` | `CAP_SETUID/SETGID/SYS_CHROOT` | `CAP_SETUID/SETGID` | nothing |

Two rows deserve emphasis because they are easy to misread.

**`seccomp` alone does not separate isolates from each other.** Every isolate
runs as the same uid, in the same namespaces, with the same view of the
filesystem. A compromised stub can `ptrace` a sibling isolate and read its GPU
buffers — subject only to the syscall allowlist, which does not include
`ptrace`, `kill`, `socket` or `execve`. So the practical containment of the
`seccomp` rung is real but narrow: it stops a stub RCE from *executing* anything
new or opening a network socket, and it does not stop it from reading whatever
its uid can read. It is the right rung when the alternative is `none`, and it is
not a substitute for either mode above it.

**`uid+chroot` closes two of UID mode's four gaps, not all four.** `/proc` and
the host filesystem go away; the shared network stack and shared SysV IPC/UTS
remain, and everything in `/dev` stays reachable.

### What the filesystem exposure actually is

The namespace sandbox has a history here worth repeating, because it is the
reason this section exists. An earlier version parked an `O_PATH` handle to the
whole host `/dev` at `NVKVM_DEV_DIRFD` and pivoted into an empty tmpfs; a
compromised stub could `openat(dd, "../../etc/shadow", O_RDONLY)` because the
dirfd's `".."` still resolved above the host `/dev`. The fix was to build a
tmpfs root holding only the nvidia nodes, `pivot_root` into it, and open the
dirfd **after** the pivot (`src/qemu/nvkvm_isolate.c`, `nvkvm_child_enter_mount_ns`).

**That mitigation does not exist in plain `uid` mode, and cannot.** There is no
mount namespace, no `pivot_root`, and therefore no dirfd — the stub's
`stub_detect_dev_dirfd()` finds nothing at fd 4 and falls back to absolute
`/dev/<name>` paths (`src/stub/nvkvm_stub.c:340-367`). The equivalent exposure
is not a bug to be fixed; it is the rung's baseline:

- **`openat` is allowed by the seccomp filter and seccomp cannot filter paths.**
  In namespace mode that is fine, because the only paths that exist are the
  nvidia nodes. In plain `uid` mode a compromised stub can open *any path on the
  host* that its uid may open: everything world-readable — `/etc/passwd`,
  `/proc/*/cmdline`, host configuration, the nvkvm checkout, other tenants'
  world-readable files. Not `/etc/shadow`, not root-owned `0600` files, not
  another isolate's files.
- **It can create files** in world-writable directories (`/tmp`, `/var/tmp`,
  `/dev/shm`). Namespace mode has nowhere writable at all. Two consequences:
  predictable-name and symlink games against anything on the host that trusts
  `/tmp`, and **persistence across teardown** — a file created by the isolate at
  uid 500001 survives it and is owned and writable by the *next* isolate issued
  uid 500001. That is a cross-isolate channel through time that namespace mode
  does not have.
- **`/dev/nvidia-uvm` becomes reachable.** Namespace mode omits it from the
  sandbox root on purpose — UVM is opened by QEMU, never by the stub, and UVM
  ioctls are the one place privileged QEMU runs a guest-named ioctl, which is
  why the UVM schema allowlist exists and is default-deny (see
  [Allowlists](../reference/allowlists.md)). In UID mode the stub can open
  `/dev/nvidia-uvm` itself (it is mode 0666) and issue UVM ioctls **without
  passing that allowlist**. This is the single largest concrete regression of
  UID mode, `+chroot` does not fix it, and it is not theoretical.
- Other `/dev` nodes follow the same rule: anything the uid may open, it may
  open, and `ioctl` is in the seccomp allowlist. `/dev/kvm` is mode 0666 on some
  hosts (it was `0660 root:kvm` on the box this was verified on, so unreachable
  there — check yours). Audit `/dev` permissions before enabling UID mode on a
  shared host.

**`+chroot` is what recovers most of this**, on hosts that grant
`CAP_SYS_CHROOT`. `nvkvm_iso_enter_chroot()` does `chroot("/dev")`, then
`chdir("/")`, then opens the new root as the `/dev` dirfd. Three details are
load-bearing:

1. `chdir("/")` **after** the chroot. `chroot(2)` does not move the cwd, and a
   cwd left outside the new root is the textbook escape — the same class of bug
   as the `O_PATH` traversal above.
2. The dirfd handed to the stub *is* the chroot root, opened after the chroot.
   `".."` at the process root is clamped by the kernel, so
   `openat(dd, "../../etc/passwd")` cannot walk out. Verified, not assumed —
   `tests/security/uid_isolate_test.c` probes exactly that path and two deeper
   ones.
3. All capabilities, `CAP_SYS_CHROOT` included, are dropped immediately after
   (the uid drop itself clears the permitted set on a non-zero uid transition),
   and `no_new_privs` is set. A chroot is escapable by a process that *retains*
   `CAP_SYS_CHROOT` or holds a directory fd from outside; every inherited fd is
   closed before this runs, and the cap drop closes the other route. The test
   attempts the classic double-chroot escape and requires it to fail.

We chroot to `/dev` itself rather than to a scratch root holding only the nvidia
nodes — which is what namespace mode builds — because populating a scratch root
needs either bind mounts (`CAP_SYS_ADMIN`) or `mknod` (`CAP_MKNOD`), and the
target environment has neither.

What still bounds plain `uid` mode: the seccomp allowlist has no `socket`,
`connect`, `bind`, `execve`, `unlink`, `chmod`, `mount` or `ptrace`. So "no
network namespace" is much less bad than it sounds — the stub cannot make a
socket at all — and the write primitive is `openat(O_CREAT)` + `write`, not a
general filesystem-mutation toolkit. In UID mode, seccomp and DAC *are* the
boundary. In namespace mode they are the third and fourth layers of it.

### Which threat model each rung is for

- **Namespace mode** — a stub RCE must not reach the host: not the filesystem,
  not the network, not other tenants' processes, not other isolates. This is the
  mode the rest of this document, the audits and the security tests assume. Use
  it unless it cannot run.
- **UID mode** — a stub RCE must not reach *other isolates or other users*, and
  you accept that it can read the host filesystem, enumerate host processes and
  reach `/dev` nodes its uid may open. Appropriate when the host is
  single-tenant and already effectively trusted by the workload (your own
  container, your own machine) and the property you actually need is that guest
  process A cannot reach guest process B. **Not** appropriate for hostile
  multi-tenant hosting, and not a substitute for the namespace sandbox.
- **`uid+chroot`** — the same threat model as `uid`, minus host-filesystem
  reads and `/proc` enumeration. This is the rung to use in a container, and
  what `auto` selects there.
- **`seccomp`** — a stub RCE must not be able to *execute* anything new, open a
  network socket, or call outside a 20-syscall allowlist. It says nothing about
  isolate-to-isolate separation: every isolate is the same uid in the same
  namespaces, so a compromised stub can `ptrace` a sibling and read its GPU
  buffers. Appropriate only when the whole VMM already sits inside someone
  else's sandbox, or as the floor `auto` lands on when nothing better is
  available. Never appropriate as a deliberate choice for multi-tenant work.
- **`none`** — no threat model. Debugging, and measuring what each layer costs.
  Requires the explicit acknowledgement, and is the only rung `auto` will never
  select.
- **`namespace+uid`** — both. Supported, but usually pointless: if user
  namespaces are available the namespace sandbox is already strictly stronger,
  and the uid drop only buys defence-in-depth against a userns escape. It is not
  the recommended default. (Note it does change one thing: with QEMU running as
  root the rootless map is `0 -> 0`, so the isolate is host-uid 0 with no
  capabilities; adding `uid` makes it a genuine unprivileged uid as well.)

### Operational constraints of UID mode

- **`CAP_SETUID` and `CAP_SETGID` are required** in the QEMU process, and are
  checked with `capget()` at device realize — not at `setresuid()` time — so the
  failure is a startup error naming the missing capability, not an opaque
  isolate spawn failure with forwarding silently off. `--cap-add=SETUID
  --cap-add=SETGID` for a container, or run QEMU as root.
- **Supplementary groups are dropped** (`setgroups(0, NULL)` before the drop),
  so group-based access to the device nodes does not apply to the isolate.
  `/dev/nvidiactl` must be mode `0666` — this is checked at realize and refused
  with the actual mode in the message. The NVIDIA installer's default already
  is `0666`.
- **One uid window per VM.** Each VM reserves `[base, base+4096)`. Two QEMU
  processes left on the default base would hand the same uid to two isolates of
  different VMs, which could then ptrace each other — the cross-VM boundary this
  device otherwise maintains. Give every concurrently-running VM a distinct
  `NVKVM_ISOLATE_UID_BASE`, at least 4096 apart.
- **Uniqueness and reuse.** The uid is `base + slot`, and the slot is held
  exclusively from `alloc_isolate_slot()` until `nvkvm_isolate_kill()` clears
  `in_use` — which happens only *after* `waitpid()` has reaped the stub. So a
  uid is never shared by two live isolates and never re-issued while its
  previous holder still exists. `nvkvm_isolate_create()` additionally scans the
  table for a live isolate holding the same uid and refuses rather than
  duplicating. The residue that reuse *does* carry over is files (above).
- The drop is verified in-process, not assumed: `nvkvm_iso_drop_privilege()`
  sets gid before uid (the classic ordering bug — a uid drop first leaves the
  process unable to change its gid), uses `setresuid`/`setresgid` so the *saved*
  ids go too, re-reads all six ids, and then attempts `setuid(0)`, `seteuid(0)`,
  `setresuid(0,0,0)`, `setresgid(0,0,0)` and `setegid(0)` and requires every one
  to fail. Any surprise `_exit(125)`s the child before `exec`, so a
  half-dropped stub never runs. `tests/security/uid_isolate_test.c` includes the
  same header and probes the same properties across two live uids.

## The sandbox

Applied in the forked child, before `exec`, while it still has enough privilege
to create namespaces (`src/qemu/nvkvm_isolate.c:50-64`).

Everything in this section describes **namespace mode**, the default. In UID
mode, subsections 1 and 2 do not happen and 3, 4 and 5 are unchanged; see
[Isolation modes](#isolation-modes) above for what that costs.

### 1. Namespaces

One `clone` with `CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWIPC |
CLONE_NEWUTS | CLONE_NEWNS` (`src/qemu/nvkvm_isolate.c:124-133`). `CLONE_NEWUSER`
is what lets an unprivileged parent create the rest. The child is PID 1 of the
new pid namespace and `clone()` returns its real host pid, so there is no
intermediate process and no second fork.

The parent writes the rootless single-line uid/gid map
(`0 <euid> 1`) and `setgroups: deny`, then releases the child through a sync
pipe (`src/qemu/nvkvm_isolate.c:97-114`, `:932-946`). Map-write failure is
fail-closed: the child is killed.

### 2. A minimal root

`nvkvm_child_enter_mount_ns()` — `src/qemu/nvkvm_isolate.c:155-231`. Mount
propagation is made private, a 256 KiB tmpfs is mounted over `/proc` and used as
the sandbox root, and *only* the nvidia device nodes are bind-mounted into it:
`nvidiactl`, `nvidia0..7`, `nvidia-modeset`, and `dri/renderD128..135`. Nodes are
created by `touch`-then-bind because `mknod` is not permitted in the user
namespace. `/dev/nvidia-uvm` is deliberately absent — QEMU opens it, never the
sandboxed stub.

Then `pivot_root(".", ".")`, old root detached, `/dev` dirfd opened **after** the
pivot, and the root remounted read-only.

The ordering there is the whole point (`src/qemu/nvkvm_isolate.c:141-148`):

> SECURITY: an earlier version parked an O_PATH handle to the *whole host /dev*
> at `NVKVM_DEV_DIRFD` and pivoted into an empty tmpfs. That handle was an escape
> hatch — a compromised stub could `openat(dd, "../../etc/shadow", O_RDONLY)` and
> read any host file, because the dirfd's ".." resolved to the (still-referenced)
> host root above /dev. Fix (the runc device-bind idiom): construct a tmpfs root
> holding ONLY /dev/nvidia*, pivot into it, then open the dirfd AFTER the pivot
> so its ".." is the sandbox root, which contains nothing but those nodes.

The read-only remount is also fail-closed, and used not to be
(`src/qemu/nvkvm_isolate.c:222-226`):

> Audit R4-L1: this used to ignore the return — a silent partial fail-open (the
> stub would run with a writable root tmpfs if the remount failed). Fail closed
> […] so a weakened sandbox never runs.

The stub detects the dirfd by attempting `openat(fd, ".")` and falls back to
absolute `/dev/<name>` paths when un-hardened
(`src/stub/nvkvm_stub.c:355-395`).

### 3. Capabilities

`PR_SET_NO_NEW_PRIVS`, `PR_SET_DUMPABLE=0`, the whole capability bounding set
dropped, effective/permitted/inheritable zeroed, ambient cleared
(`src/qemu/nvkvm_isolate.c:66-81`). After this the stub is fully unprivileged.

### 4. Inherited fds

Everything above the reserved ones is closed with `close_range(2)`, falling back
to a `getdents64` loop over `/proc/self/fd`
(`src/qemu/nvkvm_isolate.c:276-308`). This is not hygiene
(`:261-266`):

> Without this the stub inherits QEMU's KVM vm fd, the memory-backend fds, every
> other isolate's socket-pair, and so on — turning any stub RCE into "set
> arbitrary host memory region visible to the guest" via
> `KVM_SET_USER_MEMORY_REGION`.

stdout and stderr go to `/dev/null` so a compromised stub cannot write attacker
bytes into the host terminal, log or supervisor pipe
(`src/qemu/nvkvm_isolate.c:837-852`), and the environment is cleared
(`:860`).

### 5. Seccomp

Applied **after** the worker pool is spawned, with `TSYNC`
(`src/stub/nvkvm_stub.c:2610-2692`). 20 syscalls allowed:

```
read write recvmsg sendmsg ioctl mmap mprotect munmap ppoll close
exit exit_group rt_sigaction rt_sigreturn futex clone3 openat
eventfd2 gettid tgkill
```

Everything else returns `EPERM`; an architecture mismatch kills the process.
Vestigial entries with no freestanding caller — `clone`, `set_robust_list`,
`madvise`, `lseek`, `pread64`, `readlinkat` — were dropped to shrink the
post-RCE surface (`:2570-2572`).

Two details are load-bearing.

**`mmap` and `mprotect` deny `PROT_EXEC` outright, not just `W|X`**
(`src/stub/nvkvm_stub.c:2624-2631`):

> Plain W^X (deny only W+X together) is insufficient: an attacker can mmap a page
> RW, write shellcode, then mprotect it R-X — each step passes W^X but the result
> is executable attacker code. The stub's own `.text` is mapped executable by the
> ELF loader BEFORE seccomp and it never JITs (libcuda runs in the guest), so no
> runtime mapping ever needs `PROT_EXEC`.

**`TSYNC` is mandatory** (`:2584-2589`):

> The worker pool is spawned before this runs; a non-TSYNC filter would bind to
> the reader thread only and leave the workers — which run all
> attacker-influenced ioctl handling — completely unsandboxed. TSYNC requires
> every thread to already have `no_new_privs`, which `main()` sets before the
> worker-spawn loop.

A `TSYNC` per-thread failure is reported as a *positive* return value, so any
nonzero result is treated as fatal, not just a negative one (`:2746-2754`).

`openat` remains allowed because seccomp cannot filter paths; it is bounded by
the post-pivot `/dev` dirfd sandbox instead (`:2737-2738`).

### Escape hatches

`NVKVM_ISOLATE_NO_HARDEN=1` disables all of the above (equivalent to
`NVKVM_ISOLATE_MODE=none`; kept for compatibility, and ignored when
`NVKVM_ISOLATE_MODE` is set explicitly). `NVKVM_STUB_DEBUG=1` keeps the stub's stdio
and, in the non-embedded path, its inherited environment
(`:841-851`, `:870-874`). Both are debugging hatches, not supported modes. The
old `NVKVM_STUB_NO_SECCOMP` hatch was removed along with libc — "the parent
calls `clearenv()` before exec so there is no environment to inspect anyway"
(`src/stub/nvkvm_stub.c:2839-2842`).

## Handles

QEMU owns a global handle table: 65536 slots, each wrapping either an open
`/dev/nvidia*` fd or a memfd (`src/qemu/nvkvm_handle.h:17-37`).

The guest sees only handle ids. The stub sees only its own local fds. Neither
sees the other's, and neither sees a raw host fd number.

Handles are distributed to isolates by `SCM_RIGHTS` and refcounted;
`close_handle` returns `-EBUSY` while any isolate holds one
(`src/qemu/nvkvm_handle.c:310-331`).

`nvkvm_handle_acquire_fd()` returns a `dup` taken atomically under the table
lock (`src/qemu/nvkvm_handle.c:251-277`):

> IOCTL_ON_ISOLATE runs on QEMU's thread pool; without this a concurrent
> `nvkvm_handle_close()` on the TX thread could `close()`+recycle the host fd
> while a worker is mid-ioctl on it (use-after-close / wrong-object). The dup is
> an independent fd referencing the SAME struct file, so the kernel keeps that
> open file description alive for the whole ioctl.

## Lifecycle

**Creation** is lazy: on the first device open for a session
(`nvkvm_ensure_isolate()`, `src/guest/nvkvm_main.c:564-581`), under the
session's `isolate_lock` with double-checked locking. QEMU creates the
socketpair, spawns the child, starts the reader thread, and — best-effort —
sets up the command-buffer ring (`src/qemu/nvkvm_isolate.c:761-986`).

**Teardown** is once, from the last session reference
(`src/guest/nvkvm_session.c:101-144`), in a strict order
(`:126-129`):

> Stop the pump before unmapping/killing: it must reach its `wait_event` and exit
> while the isolate is still alive so any in-flight `ENTER_LOOP` completes.

QEMU's side sends `ISOLATE_CMD_EXIT`, tears down the socket **under
`write_lock`** so a concurrent worker either finishes its send or sees `-1` and
skips (`src/qemu/nvkvm_isolate.c:1034-1049`), joins the reader thread, and polls
for the child with `WNOHANG` in 10 ms steps rather than sleeping a fixed 500 ms
(`:1064-1086`):

> KILL runs on the single TX thread, so a fixed 500 ms sleep here stalled ALL
> virtio processing for the whole VM on every teardown (a guest CREATE/KILL loop
> could wedge throughput).

On kill, QEMU also **reaps** anything the guest failed to release: every
`iso_mmap_tbl` entry the isolate still held, its GPA window extents and KVM
slots (`src/qemu/nvkvm_isolate_handlers.c:2535-2573`), and the session itself if
that was its last isolate (`:364-385`).

## The trust boundary

**QEMU's boundary is cross-VM and host-process. It is not intra-VM.**
`src/qemu/nvkvm_isolate_handlers.c:1240-1252`:

> intra-VM, per-guest-process access control (which process may touch which
> object) is emulated entirely by the guest kernel module — it owns the guest's
> pids/uids/namespaces/fds and is the authority. QEMU must NOT second-guess it
> with a session-ownership check: doing so would wrongly reject a handle that the
> guest LEGITIMATELY shared into another isolate via a guest-commanded
> `COPY_HANDLE_TO_ISOLATE` (e.g. CUDA IPC), and it adds no security (malicious
> guest userspace is blocked by the guest module; a malicious guest kernel would
> just forge `session_id`). QEMU's boundary is cross-VM / host-process (the
> per-VM handle table + hClient allowlist + no host-wide TYPE_ALL), not intra-VM.

What QEMU *does* enforce is in [Allowlists](../reference/allowlists.md): six
default-deny gates plus a runtime per-VM `hClient` set.

**The guest kernel module enforces intra-VM.** Its side of the contract:

- Sessions keyed by `mm_struct`, not tgid, so a recycled tgid cannot inherit a
  stale session, isolate and handle table — "a cross-uid info-leak path inside
  one VM" (`src/guest/nvkvm.h:44-56`).
- Embedded fds are checked to be one of *our* device files before
  `private_data` is read (`nvkvm_file_is_ours()`, `src/guest/nvkvm.h:447-470`):

  > a caller pointing the field at any other fd (pipe/socket/eventfd/drm) causes
  > a type-confused read of a foreign subsystem's `private_data`. Mirrors the
  > real driver's `f->f_op == nv_frontend_fops` check in
  > `osUserHandleToKernelPtr`.
- `GET_PIDS` is synthesised from the session table with per-namespace filtering,
  so a process invisible from the caller's pid namespace is not reported —
  correct isolation for containers inside the guest
  (`src/guest/nvkvm_main.c:820-893`).

**The guest does not defend against the host.**
`src/guest/nvkvm_mmap.c:18-20`: "A malicious host could abuse this, but we are
not defending against the hypervisor." The one thing it does validate is any
host response that would cause it to map guest physical memory —
`nvkvm_gpa_in_mmap_window()` (`src/guest/nvkvm_main.c:21-23`,
`src/guest/nvkvm_mmap.c:746-760`).

**The guest never receives the device.** No BAR assigned to it, no MMIO window
handed over, no DMA path from the guest to host memory. Compare PCIe
passthrough, where the GPU retains DMA access to host RAM.

**This is not a hardened multi-tenant boundary.** Two internal audits, no
external audit, experimental software. See
[Known limitations](known-limitations.md#security).

## The security tests

`tests/security/` contains four programs, and their polarity differs — read
each header before interpreting an exit code.

- **`poc_cross_proc_dup.c`** — an *unprivileged host neighbour* that opens
  `/dev/nvidiactl` directly (bypassing QEMU and the stub entirely) and tries to
  `RM_DUP_OBJECT` a guest's live `(hClientSrc, hObjectSrc)`. It tests the only
  layer that can stop a host neighbour: the kernel's own reach-gate. **Exit 0
  means the hole is OPEN**; exit 1 means containment holds; exit 2 is
  inconclusive. It allocates its own `NV01_DEVICE_0` first, because a Memory
  object's dup parent must be a Device or the call fails at
  `INVALID_OBJECT_PARENT` *before* the access check and masks the result.
- **`intra_vm_isolation.c`** — runs entirely inside one guest. Forks; each
  process opens its own `/dev/nvidiactl` (distinct session → distinct isolate →
  distinct host RM client) and the child tries to dup the parent's objects by
  name. **Exit 0 means isolated.**
- **`gpu_holder.c`** — not an assertion. It is the victim fixture: `dlopen`s
  `libcuda`, creates a context, allocates 16 MB and sleeps, so the PoC has a live
  target.
- **`uid_isolate_test.c`** — runs on the **host**, as root. It `#include`s
  `src/qemu/nvkvm_isolate_uid.h` so it exercises the device's actual
  privilege-drop code rather than a copy. Forks two children at different uids
  and requires: the drop is irreversible (`setuid(0)`, `seteuid(0)`,
  `setresuid(0,0,0)`, `setgid(0)` all refused), supplementary groups are gone,
  and neither child can `ptrace`, signal, read `/proc/<peer>/mem` or open the
  peer's `0600` file. It *also* asserts the documented weaknesses — the peer is
  visible in `/proc`, the host filesystem is readable — and prints them as
  `leak (expected in uid mode)` lines, so a change that closes them updates this
  document instead of quietly diverging from it. **Exit 0 = every probe behaved
  as required**; 1 = a real regression; 2 = the privileged half could not run
  (no `CAP_SETUID`) and is therefore a SKIP, never a pass.
