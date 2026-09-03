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
[Tested platforms](../README.md#tested-platforms) is expected to behave the same — the
forwarded interface is per-architecture, not per-die — so an untested RTX 4080
should match the tested RTX 4070. See
[supported drivers](reference/supported-drivers.md) for the reasoning.

**Which host driver versions are covered?**
Eight ABI profiles span every published open-driver release; six of them have
been booted here, including all the ones in common use (535 LTS, 545, 550–565,
570/575, 580–595, 610). The two unbooted ones cover 515–530, whose drivers no
longer build against a modern kernel. Full matrix, and what "unbooted" means for
your risk, in [supported drivers](reference/supported-drivers.md).

**Does the guest need an NVIDIA driver?**
No kernel driver — the guest loads `nvkvm-guest.ko`, which presents `/dev/nvidia*`
itself. It does need the matching userspace libraries, staged from the host by
[`stage_guest_libs.sh`](../scripts/stage_guest_libs.sh).

**Which guest distros are supported?**
The distro does not matter; the **kernel version** does. `nvkvm-guest.ko` is
built against the guest's own headers, and it builds on **5.15, 6.1, 6.6, 6.8,
6.12, 6.14, 6.19 and 7.0** — every LTS in the range NVIDIA's driver supports,
plus current stable — in both the graphics and compute-only variants. Verify any
kernel yourself with `bash tests/kernel_matrix.sh`, which needs Docker and
nothing else. Table and the API differences it papers over:
[guest kernels](reference/guest-kernels.md).

Caveat worth stating: those are **build** results. Ubuntu 24.04 (6.8) is the
one that gets booted and run through `validate.sh`; a compile pass says the API
surface matches, not that the module behaves. Windows guests are not
supported.

**Does the host driver version have to match the guest's?**
Yes. The libraries staged into the guest come from the host, so they are the same
build by construction. See [ABI profiles](reference/abi-profiles.md).

**Can several VMs share one GPU?**
Yes — each guest process gets its own isolate on the host, so they are separate
address spaces sharing the device the same way host processes do.

**What's the performance cost?**
Close to nothing on throughput, and a real cost on latency. Sustained compute
and bandwidth measure at parity (1.00x) on every workload in
[Tested applications](../README.md#tested-applications), and Geekbench 7 GPU — an
independent benchmark whose runs are published on Geekbench's own servers —
scores **98.0–99.9% of bare metal on four machines**
([all four](reference/parity.md)): RTX 4070 99.6%, RTX 3050 Laptop 99.9%,
H100 PCIe 98.8%, A100 80GB 98.0%.

What costs is any workload dominated by small serialized control calls, because
each one is a forwarded round trip. The sharpest measured case is LLM prefill on
a *tiny* (~5-token) prompt: 0.71x, which is launch latency rather than prefill
compute — on a realistic long prompt the same measurement is 0.98x. Alloc churn
behaves the same way. If your workload is a stream of tiny GPU calls rather than
sustained work, budget for that; otherwise you will not notice.

**Can one guest use several GPUs?**
Yes, and it is autodetected — the guest module probes the host and exposes one
`/dev/nvidiaN` per card. **Six GPUs in one guest** is the largest measured
(6x RTX A4000, `validate.sh` 28/28 with `cuda_device_count 6`, all six busy at
once); 4x RTX 5060 and 2x RTX 4070 are also on the
[tested platforms](reference/tested-platforms.md) list.

Two things to know. Only **one** identity PCI device is created, so anything
that counts GPUs by walking the guest's PCI bus sees one card — CUDA, NVML and
Vulkan all go through RM and are unaffected. And tensor-parallel serving is the
one workload shape measurably below parity: about **0.86x** on the single
measurement taken since NCCL's shared-memory transport was fixed on 2026-08-21.
`NCCL_SHM_DISABLE=1` is **no longer needed** and the 0.12–0.37x figures measured
with it are superseded — see
[reading the parity numbers](reference/parity.md#multi-gpu-tensor-parallel-serving).

**Is it safe to run untrusted guests?**
Not yet — treat it as experimental. The ioctl and alloc-class gates are
default-deny and the guest kernel module is untrusted by design, but the code
has had no *external* security review. It has had two internal ones, both
published with their open findings rather than only their fixed ones: the
[pointer audit](internal/audit-guest-pointers.md) and the
[boundary audit](internal/audit-boundaries-2026-08-20.md). Read the second
before deciding: it found that an unprivileged guest could hang the entire VMM
without corrupting a single byte, which is the kind of thing a "no known memory
bugs" summary hides. See also
[the isolate model](internal/isolate-model.md).

**Can nvkvm itself run inside a container?**
Yes, and much of the testing is done that way — there is a
[`Dockerfile`](../Dockerfile) and a [`docker-compose.yml`](../docker-compose.yml) for
exactly this. A default container is enough:

```bash
docker run --gpus all --device /dev/kvm ...
```

No `--privileged`, no added capabilities, default seccomp and AppArmor. Rootless
Docker works on the same terms, as long as your user can open `/dev/kvm`.

Worth contrasting: containerising the *workload* instead does not get this. A
Proton game needs Steam's own container (pressure-vessel, on bubblewrap), which
must create a user namespace — and Docker's default seccomp profile denies that.
So containerised Steam only runs once the outer container is weakened with
`CAP_SYS_ADMIN` or `seccomp:unconfined`. Putting the VM in the container instead
avoids the conflict entirely: the guest is a different kernel, so its namespaces
never touch the host's filter.

This is a useful way to run it today: the isolates are weaker inside a container
(namespaces are usually blocked, so they fall back to UID separation), but the
container boundary sits *outside* the VMM, so breaking out of the VMM lands the
attacker in the container rather than on the host.

**Is `--device /dev/kvm` as dangerous as mounting the Docker socket?**
No. `/dev/kvm` is designed for unprivileged use — QEMU and libvirt run VMs as
ordinary users, and systemd ships the node world-accessible on that basis.
Holding it is not root-equivalent, and an escalation through it would be a Linux
kernel vulnerability rather than a misconfiguration here. The Docker socket is
root-equivalent *by design*: anything that can talk to it can start a privileged
container.

The compose deployment also grants the broker `/dev/udmabuf`. That one is a much
narrower and less exercised interface; it rides the same `root:kvm 0660` gate,
which is a reason to allow it rather than a safety proof, and
[the broker audit](internal/audit-broker-security-2026-08-27.md) records it as an
accepted risk, not a neutral one. Neither node is ever held by the guest — both
sit with the VMM and broker, so they are reachable only after an escape out of
the VM.

**Why is my GPU showing as llvmpipe?**
Two causes, and the second is easy to miss. Either the NVIDIA userspace
libraries did not stage — see
[staging guest libraries](howto/stage-guest-libraries.md) — or the user
running the client is not in the guest's `video` and `render` groups, so opening
the render node returns `EACCES` and the stack falls back silently. Nothing
errors; you simply get software rendering that looks like a working GPU until
you check the renderer string. `scripts/setup_guest.sh` puts the default user in
both groups.

**Does CUDA give bit-identical results to the host?**
On everything measured on **one** GPU, yes — including token-identical LLM
output at temperature 0, and Geekbench 7 GPU with every workload validating.
The one place it is **not** established is tensor-parallel serving across
several GPUs: in the one run measured since the NCCL shared-memory fix, the
generated-text hashes differed between host and guest. That may be ordinary
reduction-order nondeterminism in collectives, but it has not been shown either
way. Verify your own workload against a host run regardless; see
[Known issues](../README.md#known-issues).
