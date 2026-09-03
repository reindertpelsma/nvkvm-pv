# Security

`nvkvm` puts a guest in front of the host's NVIDIA driver. That is a serious
thing to do, and this file says plainly what the boundary is meant to stop, what
it demonstrably does not stop yet, and how to tell us when we are wrong.

## The short version

**Do not put untrusted tenants behind nvkvm.** It is experimental. The
guest→host boundary is not yet a security boundary you should rely on, and an
unprivileged process inside the guest can currently hang the whole VMM.

Run it where you would be comfortable running the guest's workload on the host
anyway: your own VMs, your own CI, your own experiments. Not multi-tenant
hosting, not a public sandbox, not anything where the guest is the adversary.

## What the design is trying to guarantee

The guest never receives the physical device. No BAR is mapped into it, no MMIO
window is handed over, and there is no DMA path from the guest to host memory —
which is a stronger position than PCIe passthrough, where the card keeps DMA
access to host RAM.

Every forwarded request is executed by a **per-process isolate**: a sandboxed
host process with its own RM client and its own address space, so a guest
process that corrupts something corrupts only its own GPU context. Guest
pointers are not meant to cross; the host boundary overwrites pointer-carrying
fields rather than trusting the guest to have sanitised them.

Details: [the isolate model](docs/internal/isolate-model.md) and
[the forwarding model](docs/internal/forwarding-model.md).

## Threat model

**In scope** — we treat these as vulnerabilities:

| from | to |
|---|---|
| guest process | isolate, guest kernel, VMM, **another guest process** |
| guest kernel | isolate, VMM |
| isolate | VMM, host, **guest kernel**, **another guest process** |

**Guest process → guest process is in scope, with a caveat worth stating.** Each
guest process gets its own isolate on the host, with its own RM client, so one
process's GPU objects are not meant to be reachable from another's — and we
treat a way to reach them as a vulnerability. **This boundary is not currently
closed.** Twelve handlers take a guest-supplied `isolate_id`; several carry no
caller identity on the wire, and the code says so where it happens —
`nvkvm_isolate_handlers.c:4951` reads *"KNOWN GAP, do not read this as a closed
boundary"*, and [the audit index](docs/audit/README.md) states plainly that *"the
cross-isolate boundary is not currently a boundary."* Closing it properly needs a
caller session id in the protocol, not a patch.

But it **preserves** a boundary rather than creating one. It is meaningful
between guest processes the guest OS has already separated — different UIDs, or
containers — and means nothing between two processes running as the same user,
which can reach each other by ordinary means regardless.

**An isolate is not trusted either**, which is why two more rows are in the table
than you might expect. A compromised isolate is a sandboxed host process, and
the interesting thing it can try next is not always the host: reaching **the
guest kernel** through the response path is a privilege escalation *inside* the
guest, obtained by way of the host, and reaching **another guest process** is
the mirror of a guest process reaching one. Both count.

**Out of scope**, as safe by construction: an isolate → **its own** guest
process (it exists to serve that process; there is no boundary between them),
guest kernel → guest process, and VMM → anything. The VMM and the host are
trusted; the guest and the isolates are not.

**The contract for every "another guest process" row is that nvkvm preserves a
boundary the guest OS has already drawn.** Where the guest separates two
processes — different UIDs, or containers — nvkvm must not become the way around
that, and a way around it is a vulnerability. Where the guest draws no boundary,
two processes running as the same user, there is nothing to preserve: they can
already reach each other with `ptrace` or `/proc/<pid>/mem`, and nvkvm neither
adds a boundary nor claims to.

Also out of scope: **timing and contention side channels** between guests sharing
a GPU. We have not studied them and you should assume they exist.

**Residual VRAM contents are a different case and should not be lumped in with
those.** NVIDIA's resource manager scrubs video memory on free — enabled by
default since GK110, `bScrubOnFreeEnabled` in the vendor source — so freed VRAM
is not handed to the next allocation with the previous tenant's data in it.
nvkvm inherits that property rather than providing it: every allocation goes
through the same RM, and nvkvm has no allocator of its own to bypass it with.
Two caveats worth stating rather than hiding: we have not independently verified
the scrub end to end through nvkvm, and the driver disables it in several
configurations of its own (simulation, SLI, vGPU host mode, and a broken
framebuffer), none of which are the configuration this project targets.

## What we know is wrong today

We audit our own boundary and publish the results, fixed or not:

- [Pre-release audit, 2026-08-21](docs/internal/audit-prerelease-2026-08-21.md) —
  **15 findings** (P-1 to P-12, plus P-13 to P-15 found the same evening by
  *running* the tests rather than reading them) plus the suspected and ruled-out
  lists. Both criticals are fixed: P-1 was in code one day old, and P-2 —
  `NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2`, an unbounded kernel-side write
  loop reachable by any client — is excluded from the control allowlist, with
  the reasoning and the correct re-admission test recorded at the top of
  `src/qemu/nvkvm_ctrl_allowlist.h`. Still open: P-5 and P-14 (both assessed as
  wrong comments rather than wrong behaviour, see below), P-7 (dev harness
  only), and P-8 partly.
- [Boundary audit, 2026-08-20](docs/internal/audit-boundaries-2026-08-20.md) —
  19 findings across all three in-scope boundaries. 16 fixed, 1 partial (A-8,
  by decision), 1 open (A-9), and the low-severity set A-16 four-sixths done.
  What is left open is left open on purpose and says why: A-8 and A-9 both need
  a threading change rather than a fix, one A-16 item is a resource-policy
  decision (no `RLIMIT_MEMLOCK` on pinning) and one was not re-traced. The
  follow-up round is §8 of that document; it is build-verified only and has not
  been run against a GPU.
- [The 2026-08-29 round](docs/audit/README.md) — five documents
  ([isolate](docs/audit/2026-08-29-isolate.md),
  [vmm](docs/audit/2026-08-29-vmm.md), [guest](docs/audit/2026-08-29-guest.md),
  [broker](docs/audit/2026-08-29-broker.md),
  [packaging](docs/audit/2026-08-29-packaging.md)), 111 findings, 9 rated
  critical. **8 of the 9 criticals are closed in code**; the index page itself
  predates that and still reads as though none were merged, which is wrong —
  every remediation branch it names is now an ancestor of `main`. What it is
  still right about is the cross-isolate boundary, quoted above.
- [Broker security audit, 2026-08-27](docs/internal/audit-broker-security-2026-08-27.md)
  — the broker's own threat model treats the VMM feeding it as **untrusted and
  possibly fully compromised**, which is the correct assumption: the broker is
  the path from a VM escape to your desktop session. Several medium/low findings
  there are reported and not fixed (unpaced commits, clipboard type passthrough).
- [Reconcile pass, 2026-08-24](docs/internal/audit-reconcile-2026-08-24.md) and
  [full security/reliability, 2026-08-25](docs/internal/audit-full-security-reliability-2026-08-25.md)
  — the reconcile pass exists because earlier documents claimed fixes the code
  did not have. Read it before trusting any status line in this tree, including
  the ones in this file.
- [Release blockers, 2026-09-02](docs/audit/release-blockers-2026-09-02.md).
- [Guest pointer audit](docs/internal/audit-guest-pointers.md) — 14 unenforced
  paths against one invariant, **9 since fixed**. The four that remain (U-8,
  U-9, U-11, U-13) are all **medium or unknown severity, and all contained by
  the isolate** — none of them escapes it. **Nothing rated HIGH is open.** U-8
  is open for a stated reason rather than an oversight: clearing it needs struct
  offsets that should be derived from NVIDIA's source per ABI profile, and
  hand-transcribing them is the kind of fragility this project deliberately
  avoids.

Both documents name locations, not techniques: they contain no working bypass
procedure. The source is public, so they give an attacker nothing the code does
not, and they let a reader judge the boundary honestly instead of taking a claim
on trust.

The honest summary of the first audit: **nothing found gives a guest arbitrary
code execution on the host.** The two memory-safety breaks that cross a trust
boundary are both contained by the isolate — they corrupt within one guest
process's own GPU context, which is exactly the containment the isolate model
exists to provide. The most serious practical exposure is **liveness**: an
unprivileged guest process could hang the entire VMM without corrupting
anything. Those two are fixed; the remaining open items are listed by name in
the audit rather than quietly dropped, each with the reason it is still there.

**A-8 and A-9 were rated HIGH when the round-trip wait was untimed**, i.e. when
an unprivileged guest process could park the VMM forever. A 30 s deadline now
bounds it, which downgrades "hangs until SIGKILL" to "stalls for up to 30 s and
then the isolate is declared dead". That is a real improvement and it is the
whole of the improvement.

**It does not require any privilege in the guest, and it is not display-only.**
An earlier revision of this file claimed both; the code contradicts it in two
places, and those comments are worth reading before relying on this section.
`virtio_nvgpu.c` lists **ten** synchronous isolate commands — `CLOSE_HANDLE`,
`MMAP`, `MUNMAP`, `POLL`, `UNPOLL`, `COPY_HANDLE`, `SETUP_RING`,
`PRESENT_EXPORT`, `XISO_IMPORT`, `REALIZE_UVM_FD` — that dispatch **inline with
the QEMU BQL held**; only `NVKVM_REQ_IOCTL_ON_ISOLATE` and `ENTER_LOOP` are
offloaded to the thread pool. And `nvkvm_isolate.c:520` states that an
unprivileged guest process can stall the answering stub *"just by keeping the
SPSC ring continuously fed, starving the stub's control-socket service edge"*.
So the trigger is ordinary guest activity, not a gated display path.

Treat these as **open availability findings**, repeatable at will by any guest
process: the guest can stall the VMM's main loop, QMP, timers and every vCPU for
up to the deadline. That is consistent with the opening of this document — an
unprivileged process inside the guest can currently hang the whole VMM — and the
earlier re-rating should not have been read as retracting it.

Two more read as open but are assessments, not defects. **P-5**: the isolate
comment claims uid separation that does not exist on the default namespace rung
— but per-isolate PID and mount namespaces mean peers cannot name or enumerate
each other, so the comment is wrong rather than the boundary. **P-14**: a
ring-setup timeout kills the isolate instead of falling back to the socket path,
contrary to its comment — but a stub that has stopped answering is broken or
compromised, and dropping its handles is the correct response. Both need the
comment corrected, not the code.

## Can a container read the host's screen?

No, unless it is given `CAP_SYS_ADMIN`. This gets asked because a GPU container
is handed `/dev/dri/card*`, and a card node is what screen capture goes
through. Measured rather than reasoned about, on an RTX 4070 driving a 4K
GNOME/Wayland desktop, walking the real capture path
`GETRESOURCES -> GETCRTC -> GETFB -> PRIME_HANDLE_TO_FD` (a GEM handle to the
scanout buffer *is* the screen):

| context | `SET_MASTER` | `GETFB` handle | screen readable |
|---|---|---|---|
| root on the host | `EBUSY` | 1 | **yes** |
| container, Docker default caps | `EACCES` | **0** | **no** |
| container, `--cap-add=SYS_ADMIN` | `EBUSY` | 1 | **yes** |

The kernel returns the framebuffer's **metadata** to anyone who can open the
node — resolution, pitch, bpp, whether a CRTC is active — but **withholds the
GEM handle** without `CAP_SYS_ADMIN`. Resolution leaks; pixels do not.

Note that root on the host captured the screen while **not** being DRM master.
Capture is not gated by who holds the display. Three gates are easy to
conflate and only one of them is a privilege check:

| operation | gated by |
|---|---|
| DRM modesetting | master, i.e. **occupancy** (`CAP_SYS_ADMIN` to steal it) |
| DRM capture (`GETFB` handle) | **`CAP_SYS_ADMIN`** — occupancy irrelevant |
| NVKMS modeset ownership | **occupancy only** — no capability check at all |

The third is the surprising one: `/dev/nvidia-modeset` is mode 0666 and
`GrabModesetOwnership()` has no `capable()` test anywhere — it refuses only
when someone already owns the display. On a machine with a monitor attached
that is essentially always true, because the display server takes ownership at
startup. The exposed window is a machine with a panel but no display server
running, and it is not specific to containers: an unprivileged local user has
the same access.

**For nvkvm:** [`docker-compose.yml`](docker-compose.yml) drops `ALL` and
adds back `SETUID`, `SETGID`, `SETPCAP`, `SYS_CHROOT`. None is `CAP_SYS_ADMIN`,
so an nvkvm container cannot read the host's screen even with `/dev/dri`
present. **`--privileged` removes this entirely** — the device nodes are not
the sensitive part, the capability set is.

## Can a guest process read the *guest's* screen?

Same question one level down, and until 2026-08-22 the answer was yes.

nvkvm's virtual KMS head lives on the same `drm_device` as the render node, so
`/dev/dri/card*` in the guest **is** the guest's display. DRM core hands master
to the *first* opener of a primary node with no capability check at all:

```c
int drm_master_open(struct drm_file *file_priv) {
	if (!dev->master)
		ret = drm_new_set_master(dev, file_priv);   /* no capable() */
```

Explicit `SET_MASTER` *is* gated (`drm_master_check_perm` -> `CAP_SYS_ADMIN`),
but an attacker never needs it. And master alone is enough to read pixels,
because `drm_mode_getfb` withholds the GEM handle only from callers that are
**neither** master **nor** `CAP_SYS_ADMIN`:

```c
if (!drm_is_current_master(file_priv) && !capable(CAP_SYS_ADMIN)) { r->handle = 0; }
```

So the chain was: open the node whenever nothing holds master -- before the
compositor starts, across a VT switch, after it exits -- take master, and both
drive and capture the display. Measured in a guest: an unprivileged uid-1000
process took master and passed `ADDFB` byte-identically to root.

Upstream nvidia-drm has the same gap: `__nv_drm_master_set()` only calls
`nvKms->grabOwnership()`, which is occupancy, and `capable(`/`CAP_SYS_ADMIN`
appear nowhere in `nvidia-drm`. On physical hardware logind/seatd hides it by
opening the node as root and passing the fd. A virtualized head cannot lean on
that, so **nvkvm gates harder than upstream**: opening the primary DRM node
requires `CAP_SYS_ADMIN`, checked with `capable()` against the guest's **init**
user namespace, so root inside an unprivileged user namespace does not qualify.

| node | who may open it | what it is |
|---|---|---|
| `/dev/dri/renderD*` | anyone | render/compute; headless targets userspace allocates for itself |
| `/dev/dri/card*` | **`CAP_SYS_ADMIN` only** | the real virtual display: scanout and capture |

Verified with the node chowned to the caller and `chmod 0666`:

| module state | result as uid 1000 |
|---|---|
| `privileged_modeset=Y` (default) | `open: FAILED (Permission denied)` |
| `privileged_modeset=N` | master seized, `ADDFB` OK -- the old behaviour |

The permission bits are irrelevant to the outcome, which is the point: the
boundary is the capability.

### When to turn it off

`privileged_modeset=N` is a **deployment choice, not a debugging knob**.

Turning it off does not put you below upstream: you land exactly on the
ordinary Linux posture -- unix permissions on the node, plus drm core's own
`SET_MASTER` and `GETFB` checks. Neither drm core nor `nvidia-drm` gates the
*first* opener of a primary node, so `Y` is nvkvm being **stricter than the
driver it emulates**, and that has a cost: a session whose compositor opens the
primary node itself gets `EACCES` and a black screen with no obvious cause.
Measured: SteamOS's `sddm` -> `kwin_wayland` does exactly that -- it opens the
node directly, with no logind fd-passing step -- and the desktop does not start
until the gate is off.

Leave it **on** where a process that is unprivileged *to the guest* should still
not be able to drive or capture the guest's screen: multi-user guests, and above
all guests running containers, where root-inside-a-container with `card*` mapped
is precisely the case the gate answers.

Turning it **off** is reasonable for a single-user appliance image where the
desktop user already holds full privilege over the VM anyway. On SteamOS `deck`
has passwordless sudo, so gating `card0` against `deck` protects nothing it
could not simply take, and CUDA containers are not a scenario there. nvkvm's
shipped default stays `Y`; an image that has made that judgement sets
`options nvkvm_guest privileged_modeset=0` in `/etc/modprobe.d/` and says why.

Render clients are untouched, so CUDA and Vulkan are unaffected, and weston
still drives the head normally (it is started by root, and under logind the
compositor never calls `open()` itself -- it receives the fd).

## Weaker configurations you should know about

- **In containers**, Linux namespaces are usually blocked, so the isolate falls
  back to UID separation — a weaker inner boundary than the namespaced sandbox.
  Do not read that as "containers are the less safe option", because of what the
  isolate does *not* cover: it sandboxes the **stub**, not the VMM. QEMU runs on
  the host unconfined, and **ten of the nineteen findings in the boundary audit
  name the VMM as their target**, with an eleventh (A-15) naming the host
  directly. On a bare host, that is a landing zone on the host itself. Inside a container, the same paths land in the container, behind a
  boundary that is not our code and is scrutinised far more heavily than ours.
  So the container trades a weaker boundary we wrote for a stronger one we did
  not, and it covers the component our own sandbox leaves exposed. A minimal
  image with nothing useful readable by other UIDs strengthens it further. See
  [the isolate model](docs/internal/isolate-model.md).
- **Enforcement is per-ioctl and hand-written**, not categorical. It is audited
  but not complete, and a new ioctl added without its validation is the most
  likely way a hole appears.
- **The NVIDIA container capability list is not the GPU access control it looks
  like.** `/dev/nvidiactl` and every `/dev/nvidiaN` are granted with no
  capability at all, so the full RM ioctl surface is present in a container that
  requests none. `compute` gates UVM, `display` gates modeset, and that is the
  entire device story; libraries and the EGL application profile are data, not a
  boundary. Source line references in
  [container capabilities](docs/reference/container-capabilities.md).
- **`utility` cannot be dropped, and it carries the `nvidia-persistenced`
  socket.** The guest bundle needs NVML, which rides the same flag. On a host
  running that daemon, container root can reach its RPC — persistence mode and
  NUMA status — and the XDR decode path runs before the daemon's uid check. It
  cannot be masked from `docker-compose.yml`, because the prestart hook mounts
  it after runc sets up the rootfs. Either do not run the daemon, or use Docker
  `userns-remap`.

## The rule we hold ourselves to on the allowlists

Nine default-deny gates stand between a guest ioctl and the host driver — six
static tables and three code checks, listed in the order an ioctl meets them in
[Allowlists](docs/reference/allowlists.md). They are the part of this design
doing the most security work, so:

**Do not widen the control or NVKMS allowlist to make something work.** Widening
is the fastest-looking fix and almost never the right one. Two worked examples:
the NVIDIA DDX stops on a denied `NVKMS_IOCTL_DECLARE_EVENT_INTEREST`, and
allowing it only walks the DDX one rung further down a ladder that ends at the
*host's* physical monitors — the honest answer is a virtual NVKMS, not a wider
gate. Separately, an allowlist entry (`0x3d08`) was measured against the NCCL
shared-memory failure and **did not fix it**; the real bug was a missing fd
translation, and the widening was reverted rather than kept "in case".

What a legitimate addition looks like is set out in
[Add a driver version → Expect the allowlist to need an entry](docs/howto/add-a-driver-version.md#6-expect-the-allowlist-to-need-an-entry):
check what the command is in OGKM at the driver version in question, check its
params struct for embedded pointers, and check whether an equivalent is already
allowed under another class id. **An allowlist entry with no handler is worse
than leaving it out** — `0x70` (`NV_ESC_EXPORT_TO_DMABUF_FD`) was removed from
the frontend list for exactly that reason.

## The broker is part of the boundary

The compose deployment runs a **broker** that owns the window, the input path and
the clipboard on your host session. It is the component a VM escape would have to
go through to reach your desktop, and it is deliberately small: a framebuffer
handle and input events, not a protocol with attacker-controlled structure. It
runs `cap_drop: ALL` (plus `SETUID`/`SETGID`, used only to drop), `read_only`,
`no-new-privileges`, and **`network_mode: none`** — so a fully compromised broker
has no network namespace to exfiltrate through.

Two caveats. On **Wayland** a compromised broker cannot capture other clients'
input, because the compositor mediates and there is no global grab; on **X11**
that protection does not exist and a grab is total. And the broker is granted
`/dev/udmabuf` alongside `/dev/kvm`; see the FAQ for why that is an accepted
risk rather than a neutral one.

## One place the allowlist is wider than "default-deny" suggests

The control gate is a 167-row default-deny table, but two rule-based
passthroughs sit **ahead** of it: `cmd & 0x8000` and the `0x2081` class wildcard.
Between them they admit a large part of the RM control space without a per-row
justification. This is recorded in
[the allowlist reference](docs/reference/allowlists.md) and in the 2026-08-29
isolate audit; narrowing it needs a GPU to test against and has not been done.
Read "nine default-deny gates" with that exception in mind.

## Reporting a vulnerability

Please report privately first, by opening a
[GitHub security advisory](https://github.com/reindertpelsma/nvkvm-pv/security/advisories/new)
on this repository.

Useful things to include: what boundary is crossed, the host driver and GPU, the
guest kernel, and whatever reproduction you have — a crashing input is plenty,
a full exploit is not required.

This is a solo research project, not a vendor with a response team. There is no
bounty and no SLA. What we will do is answer, credit you unless you prefer
otherwise, and publish the finding whether or not there is a fix yet — the audit
documents above are what that commitment looks like in practice.

If a finding is in NVIDIA's driver rather than in nvkvm, we will say so and
point you at NVIDIA; six things that looked like nvkvm bugs turned out to
reproduce identically on bare metal. We check that before answering, and it cuts
both ways — bare metal *passing* is what identified two real bugs of ours. See
[contributing → check the host](CONTRIBUTING.md#before-you-file-a-bug-check-the-host).

## Scope of this file

`nvkvm` is experimental and has no supported versions and no backports. Fixes
land on `main`; a release tag or a published image is a snapshot of it, not a
branch that gets patched. If you are running one and a fix lands, take a newer
snapshot.
