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
| guest process | isolate, guest kernel, VMM |
| guest kernel | isolate, VMM |
| isolate | VMM, host |

**Out of scope**, as safe by construction: isolate → guest process, guest kernel
→ guest process, VMM → anything. The VMM and the host are trusted; the guest is
not.

Also out of scope: side channels between guests sharing a GPU (timing,
contention, residual VRAM contents). We have not studied them and you should
assume they exist.

## What we know is wrong today

We audit our own boundary and publish the results, fixed or not:

- [Boundary audit, 2026-08-20](docs/internal/audit-boundaries-2026-08-20.md) —
  19 findings across all three in-scope boundaries. 15 fixed, 4 open.
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
the audit rather than quietly dropped.

## Weaker configurations you should know about

- **In containers**, Linux namespaces are usually blocked, so the isolate falls
  back to UID separation. That is meaningfully weaker than the namespaced
  sandbox. See [the isolate model](docs/internal/isolate-model.md).
- **Enforcement is per-ioctl and hand-written**, not categorical. It is audited
  but not complete, and a new ioctl added without its validation is the most
  likely way a hole appears.

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
point you at NVIDIA; several things that looked like nvkvm bugs turned out to
reproduce identically on bare metal.

## Scope of this file

`nvkvm` is experimental and has no supported versions and no backports. Fixes
land on `main`; a release tag or a published image is a snapshot of it, not a
branch that gets patched. If you are running one and a fix lands, take a newer
snapshot.
