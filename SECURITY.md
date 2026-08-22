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
treat a way to reach them as a vulnerability. A recent audit found one and it is
fixed.

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
  12 findings plus the suspected and ruled-out lists. The most serious was in
  code one day old and is fixed.
- [Boundary audit, 2026-08-20](docs/internal/audit-boundaries-2026-08-20.md) —
  19 findings across all three in-scope boundaries. 16 fixed, 1 partial (A-8,
  by decision), 1 open (A-9), and the low-severity set A-16 four-sixths done.
  What is left open is left open on purpose and says why: A-8 and A-9 both need
  a threading change rather than a fix, one A-16 item is a resource-policy
  decision (no `RLIMIT_MEMLOCK` on pinning) and one was not re-traced. The
  follow-up round is §8 of that document; it is build-verified only and has not
  been run against a GPU.
- [Guest pointer audit](docs/internal/audit-guest-pointers.md) — 14 unenforced
  paths against one invariant, 5 since fixed.

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

**For nvkvm:** [`docker-compose.yml`](../docker-compose.yml) drops `ALL` and
adds back `SETUID`, `SETGID`, `SETPCAP`, `SYS_CHROOT`. None is `CAP_SYS_ADMIN`,
so an nvkvm container cannot read the host's screen even with `/dev/dri`
present. **`--privileged` removes this entirely** — the device nodes are not
the sensitive part, the capability set is.

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
