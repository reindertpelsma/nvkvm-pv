# GPA window on narrow-MAXPHYADDR hosts — state as of 2026-08-18

## Where it stands

`fix-gpa-window-base` makes the GPA window placement adaptive. On a 39-bit host it
gets **further than main** (which cannot start at all) but is **not finished**:
the guest boots, `nvidia-smi` reports the GPU, and then the guest **hangs** on
sustained use — `nvidia-smi` blocks, guest SSH stops responding, QEMU sits at ~8%
CPU. `validate.sh` has never completed on this host.

Do not merge until a 39-bit host reaches 28/28 and a wide host shows no regression.

## Reproducer

Stock Docker container, i7-11800H (39-bit), RTX 3050 Laptop, driver 580.173.02,
`/dev/kvm` present, user namespaces blocked by the default seccomp/AppArmor profile.
Deterministic.

## Fixed on the way (keep these)

- **Window base was hardcoded at 1 TiB.** Unaddressable below 41 bits. Now derived
  from host/guest MAXPHYADDR. Symptom was
  `KVM_SET_USER_MEMORY_REGION failed, slot=6, start=0x10000000000, size=0x1000000: Invalid argument`.
- **`span <= limit` is necessary but not sufficient.** The sparse window is a
  64-bit PCI BAR and the *firmware* must find a naturally-aligned hole. MEASURED:
  a 128 GiB BAR passed every arithmetic check and SeaBIOS assigned **no BAR at all**
  to the device — every other function on the bus got one. Now capped at `limit/8`
  (39-bit -> 64 GiB; no-op above 46 bits). This removed `kvm run failed Bad address`.
- **`clone3` is blocked by Docker's default seccomp**, independently of
  `CLONE_NEWUSER`. Stub falls back to `clone(2)`.

## Ruled out — do NOT re-investigate

- **Guest does not hardcode any base.** It reads `mmap_win_gpa` / `mmap_win_len`
  from virtio config (`nvkvm_virtio.c:696-699`); per-request `gpa_base` comes over
  the wire. QEMU moving the window is communicated correctly.
- **Sizes are consistent.** `nvkvm_sparse_init` takes `win = nv->gpa.sparse_size`
  (`nvkvm_mmap_host.c:457`), the BAR is created from `gpa.sparse_size`
  (`virtio_nvgpu_pci.c:115`), and config advertises `nv->sparse_size`
  (`virtio_nvgpu.c:1056`). All three are the shrunk value. No mismatch.
- **`00:07.0` is the identity PCI device**, not the window. It legitimately has no
  BARs. Check the virtio-nvgpu function instead — BAR checks against `00:07.0` are
  measuring the wrong device.

## Headroom fix — CONFIRMED PROGRESS, hang persists

Placing the block at `limit - span` left ZERO margin at the top. MEASURED: with
`ceiling = limit - limit/8` the block moved 431 GiB -> 367 GiB and SeaBIOS then
assigned the BAR immediately:

```
00:04.0 Red Hat, Inc. Device 1072
    Region 2: Memory at 6000000000 (64-bit, prefetchable) [size=64G]
```

exactly the computed `sparse_base`. Before this it assigned NO BAR at all. The
1/8 rule also reproduces what firmware picks unaided on a wide host: on 46-bit it
computes 0x37dbc0000000 against the 0x380000000000 the firmware chose itself.

**So "BAR is not assigned" is now RULED OUT as the cause of the hang.** The window
is present, correctly sized and at the expected base, and the guest still hangs on
first sustained use: log stalls at module load, QEMU 7.6% CPU state Ssl, guest SSH
times out, GPU 0% / 33 MiB. The hang is in what happens *after* the window exists.

## Confirmed working with NVKVM_DEBUG=1

```
nvkvm_sparse_init:   64 GiB VMM buffer 0x7b28e4000000 (memslot deferred to BAR base)
nvkvm_sparse_ensure: 64 GiB at GPA=0x6000000000 slot=64
GPU 0: NVIDIA GeForce RTX 3050 Laptop GPU (UUID: GPU-69998bd6-b034-9fe1-a839-b2d779142c60)
```

So on a 39-bit host the window is now allocated, BAR-assigned, memslot-installed at
the resolved base, and `nvidia-smi -L` returns a real UUID. **Light queries work.**

## The remaining bug

Sustained use still hangs, with `error: kvm run failed Bad address` at **CPL=3** —
the fault is in guest *userspace*, not the kernel:

```
CPL=3  CR2=0x60ece2b43000  RDI=0x200224000
Code=... 48 c1 ee 20 <89> 07 ...      (faulting insn is a store, mov %eax,(%rdi))
```

Preceded by four `nvkvm: DENY ctrl cmd 0x00730102 (not in allowlist / oversize)` —
an NV0073 display-class control, believed pre-existing and benign on a headless
guest (the Blackwell bring-up saw the same DENY with no ill effect), but it has NOT
been ruled out as related.

So: the window exists and is backed, light RM queries succeed, and guest userspace
faults on a *store* into a mapping derived from it. That points at the mapping
handed to userspace rather than the memslot itself — the GPA is right, what is
mapped on top of it is not.

## Reframing — probably a fault, NOT CONFIRMED

`error: kvm run failed Bad address` with a register dump is unambiguously QEMU's
`KVM_RUN` error path, so an EFAULT definitely occurred. **The rest of this section
is inference and was NOT verified — treat it as the leading theory, not fact.**

Not established:
- that the fault and the unreachability happened at the same moment (the fault may
  have occurred earlier and the guest hung separately)
- that the whole VM stopped. The guest has **4 vCPUs**; one returning EFAULT does
  not necessarily stop the others, and QEMU's behaviour here was assumed, not observed
- QEMU's residual 7-13% CPU is consistent with *either* other threads running or a
  vCPU spinning. It does not discriminate.

**Confirm it first, with the QEMU monitor — one command, before any code work:**

```
info status     # running / paused / internal-error
info cpus       # per-vCPU state: all four halted, or one?
```

If it reports `running` with live vCPUs, this is a DEADLOCK and the theory below is
wrong. Only if the VM is stopped should you look for why KVM cannot use the host
userspace address behind a guest GPA. EFAULT from `KVM_RUN` means the GPA resolved to a
`userspace_addr` in the memslot that the kernel could not access — a hole in the
window mapping, an unmapped extent, or an extent whose `MAP_FIXED` install did not
land where the memslot claims.

The fault is a **store from CPL=3** into a window-derived mapping, so the suspect
is what `nvkvm_mmap.c` hands to guest userspace on top of the window, not the
memslot registration (which succeeds — `sparse_ensure: 64 GiB at GPA=0x6000000000
slot=64`).

### Also ruled out
- **Host overcommit / map limits.** Laptop and a working vast host are BOTH
  `vm.overcommit_memory=0`; `max_map_count` is *higher* on the laptop (1048576 vs
  65530). Not the differentiator.
- **`DENY ctrl cmd 0x00730102`** is `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`. The
  Blackwell bring-up saw the identical DENY and still scored 28/28, and a refused
  ioctl cannot produce an EFAULT on a store. Benign. (Worth *synthesizing*
  guest-side anyway — display topology is owned by `nvkvm_kms.c`, so forwarding it
  asks the host's RM about the host's real heads, which is the wrong answer by
  construction. Housekeeping, not a fix.)

## Next step

Trace where the guest blocks on first sustained window access. `nvkvm_sparse_ensure()`
(`virtio_nvgpu.c:1054`) resolves the window to the firmware-assigned BAR base on the
guest's first config read and installs the memslot there — that resolution, and
whether the BAR base it finds is sane on a shrunk/narrow layout, is the untested path.
QEMU at ~8% CPU with no guest response suggests a spin or an uncompleted forwarded
op rather than a QEMU deadlock.

Reproduce with `NVKVM_DEBUG=1` (env var read at `virtio_nvgpu.c:1116`) — that is
what surfaced the sparse_init/sparse_ensure lines above and is the fastest way in.

Faulting addresses seen: `CR2=0x5af8dad1e000` (before the BAR cap),
`CR2=0x60ece2b43000` (after both fixes). Both userspace VAs, both on a store.
