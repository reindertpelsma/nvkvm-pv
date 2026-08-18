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

## Next step

Trace where the guest blocks on first sustained window access. `nvkvm_sparse_ensure()`
(`virtio_nvgpu.c:1054`) resolves the window to the firmware-assigned BAR base on the
guest's first config read and installs the memslot there — that resolution, and
whether the BAR base it finds is sane on a shrunk/narrow layout, is the untested path.
QEMU at ~8% CPU with no guest response suggests a spin or an uncompleted forwarded
op rather than a QEMU deadlock.

Last known faulting address before the BAR cap: `CR2=0x5af8dad1e000`.
