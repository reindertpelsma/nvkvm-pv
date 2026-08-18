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

## The sandbox

Applied in the forked child, before `exec`, while it still has enough privilege
to create namespaces (`src/qemu/nvkvm_isolate.c:50-64`).

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

`NVKVM_ISOLATE_NO_HARDEN=1` disables all of the above
(`src/qemu/nvkvm_isolate.c:795`). `NVKVM_STUB_DEBUG=1` keeps the stub's stdio
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

`tests/security/` contains three programs, and their polarity differs — read
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
