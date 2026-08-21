# FAQ

The README carries the six questions people ask first; this is all of them.

**Is this vGPU or SR-IOV?**
No. There is no hardware partitioning and no vendor licence. nvkvm forwards the
driver's ioctl interface, so it runs on consumer cards that have no vGPU support
at all.

**So how are resources divided between guests?**
They are not. Nothing is partitioned: guests share VRAM, SMs and bandwidth
dynamically, exactly as GPU containers on one card do today. There is no
per-guest VRAM reservation and no quota, so one guest can exhaust the card for
the others. If you need hard partitioning, MIG sits *below* the interface nvkvm
forwards and should compose with it — but that is untested.

**Will my GPU work?**
If it is **Turing or newer** (GTX 16xx, RTX 20/30/40/50, and the datacenter
parts), yes — that is a hard requirement, not a guess. Pascal and older are out:
the open kernel module will not even probe them, and NVIDIA's 580 branch is the
last to support them at all. A card of the same architecture as one in
[Tested platforms](#tested-platforms) is expected to behave the same — the
forwarded interface is per-architecture, not per-die — so an untested RTX 4080
should match the tested RTX 4070. See
[supported drivers](docs/reference/supported-drivers.md) for the reasoning.

**Which host driver versions are covered?**
Eight ABI profiles span every published open-driver release; six of them have
been booted here, including all the ones in common use (535 LTS, 545, 550–565,
570/575, 580–595, 610). The two unbooted ones cover 515–530, whose drivers no
longer build against a modern kernel. Full matrix, and what "unbooted" means for
your risk, in [supported drivers](docs/reference/supported-drivers.md).

**Does the guest need an NVIDIA driver?**
No kernel driver — the guest loads `nvkvm-guest.ko`, which presents `/dev/nvidia*`
itself. It does need the matching userspace libraries, staged from the host by
[`stage_guest_libs.sh`](scripts/stage_guest_libs.sh).

**Which guest distros are supported?**
The distro does not matter; the **kernel version** does. `nvkvm-guest.ko` is
built against the guest's own headers, and it builds on **5.15, 6.1, 6.6, 6.8,
6.12, 6.14, 6.19 and 7.0** — every LTS in the range NVIDIA's driver supports,
plus current stable — in both the graphics and compute-only variants. Verify any
kernel yourself with `bash tests/kernel_matrix.sh`, which needs Docker and
nothing else. Table and the API differences it papers over:
[guest kernels](docs/reference/guest-kernels.md).

Caveat worth stating: those are **build** results. Ubuntu 24.04 (6.8) is the
one that gets booted and run through `validate.sh`; a compile pass says the API
surface matches, not that the module behaves. Windows guests are not
supported.

**Does the host driver version have to match the guest's?**
Yes. The libraries staged into the guest come from the host, so they are the same
build by construction. See [ABI profiles](docs/reference/abi-profiles.md).

**Can several VMs share one GPU?**
Yes — each guest process gets its own isolate on the host, so they are separate
address spaces sharing the device the same way host processes do.

**What's the performance cost?**
Close to nothing on throughput, and a real cost on latency. Sustained compute
and bandwidth measure at parity (1.00x) on every workload in
[Tested applications](#tested-applications), and Geekbench 7 GPU — an
independent benchmark, both runs public — scores
[99.9% of bare metal](https://browser.geekbench.com/v7/gpu/compare/81189?baseline=79862).

What costs is any workload dominated by small serialized control calls, because
each one is a forwarded round trip. The sharpest measured case is LLM prefill on
a *tiny* (~5-token) prompt: 0.71x, which is launch latency rather than prefill
compute — on a realistic long prompt the same measurement is 0.98x. Alloc churn
behaves the same way. If your workload is a stream of tiny GPU calls rather than
sustained work, budget for that; otherwise you will not notice.

**Is it safe to run untrusted guests?**
Not yet — treat it as experimental. The ioctl and alloc-class gates are
default-deny and the guest kernel module is untrusted by design, but the code
has had no *external* security review. It has had two internal ones, both
published with their open findings rather than only their fixed ones: the
[pointer audit](docs/internal/audit-guest-pointers.md) and the
[boundary audit](docs/internal/audit-boundaries-2026-08-20.md). Read the second
before deciding: it found that an unprivileged guest could hang the entire VMM
without corrupting a single byte, which is the kind of thing a "no known memory
bugs" summary hides. See also
[the isolate model](docs/internal/isolate-model.md).

**Can nvkvm itself run inside a container?**
Yes, and much of the testing is done that way — there is a
[`Dockerfile`](Dockerfile) and a [`docker-compose.yml`](docker-compose.yml) for
exactly this. A default container is enough:

```bash
docker run --gpus all --device /dev/kvm ...
```

No `--privileged`, no added capabilities, default seccomp and AppArmor. Rootless
Docker works on the same terms, as long as your user can open `/dev/kvm`.

This is a useful way to run it today: the isolates are weaker inside a container
(namespaces are usually blocked, so they fall back to UID separation), but the
container boundary sits *outside* the VMM, so breaking out of the VMM lands the
attacker in the container rather than on the host.

**Why is my GPU showing as llvmpipe?**
Two causes, and the second is easy to miss. Either the NVIDIA userspace
libraries did not stage — see
[staging guest libraries](docs/howto/stage-guest-libraries.md) — or the user
running the client is not in the guest's `video` and `render` groups, so opening
the render node returns `EACCES` and the stack falls back silently. Nothing
errors; you simply get software rendering that looks like a working GPU until
you check the renderer string. `scripts/setup_guest.sh` puts the default user in
both groups.

**Does CUDA give bit-identical results to the host?**
On everything measured, yes — including token-identical LLM output at
temperature 0, and Geekbench 7 GPU at 99.9% of bare metal with every workload
validating. Verify your own workload against a host run all the same; see
[Known issues](#known-issues).
