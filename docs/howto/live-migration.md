# Live migration of a VM that uses the GPU

A VM with an nvkvm GPU can be live-migrated to another host with a matching GPU and driver. Its CUDA programs
keep running: they pause for a few seconds while their GPU state moves.

GPU state (VRAM contents, driver objects, the per-session isolates) lives on the host and cannot travel in a
QEMU migration stream. So the guest first releases the GPU. Every CUDA process is checkpointed with NVIDIA's
`cuda-checkpoint`, which copies its VRAM into the process's own memory and closes its device files. The device
then has no GPU sessions, and QEMU migrates it like any other virtio device. On the target, each process is
restored onto the new host's GPU.

## What QEMU checks

The `virtio-nvgpu` device migrates only while it is **quiesced**, meaning no GPU session is open and no forwarded
request is in flight. QEMU enforces this on its own:

| When | Check | If it fails |
|---|---|---|
| Migration start | quiesced | the migration is refused; nothing is copied |
| Migration start, before any RAM | the target's NVIDIA driver version, GPU count, `graphics` setting and GPA layout match the source | the migration fails on the target; the source keeps running |
| Switchover | still quiesced (the guest keeps running while RAM is copied) | the migration is aborted; the source resumes |

The virtqueues and the PCI transport (BARs, MSI-X) migrate, so the guest driver keeps running without a re-probe.

## Requirements

- A QEMU build that includes the migration support (`virtio-nvgpu-early` appears in
  `strings qemu-system-x86_64`), on both hosts.
- The same NVIDIA driver version on both hosts, and the same number of GPUs.
- The same VM configuration on both sides: RAM size, `phys-bits`, `graphics=`. Management layers copy the
  configuration, so this is normally automatic.
- The guest module with the "return registered host memory on close" fix. Without it, a checkpoint loses the
  contents of pinned host buffers (`cudaHostAlloc`, `cudaHostRegister`) and restore fails.
- `cuda-checkpoint` in the guest, from <https://github.com/NVIDIA/cuda-checkpoint>, matching the host driver.
- **No 9p share mounted in the guest.** QEMU refuses to migrate a guest with a mounted 9p export. Set
  `NVKVM_MIGRATABLE=1` in `/etc/nvkvm/guest.conf`. Then `nvkvm-guest.service` builds the module and stages
  the libraries as usual, makes a guest-local, read-only copy of the host bundle under
  `/var/lib/nvkvm/host-libs-<version>`, bind-mounts it over `/opt/nvidia-host`, and unmounts the 9p shares.
  The copy is outside every package-managed path, so a guest `apt upgrade` still has no driver library to
  replace. Any data share (`/data`) has to be unmounted by hand before a migration.
- A service that starts before `nvkvm-guest.service` and loads the NVIDIA libraries (a Jupyter server, a
  monitoring agent) keeps the 9p share in use. The share is then detached lazily and new programs get the local
  copy, but QEMU will not migrate the VM until that process restarts; `drain` names it. Order such services
  `After=nvkvm-guest.service`.

## Procedure

Inside the guest (as root) on the source host:

```bash
CUDA_CHECKPOINT=/path/to/cuda-checkpoint scripts/nvkvm-migrate-guest.sh drain
```

`drain` is all or nothing. If any process holding a GPU device cannot be checkpointed, every process already
checkpointed is restored, the script exits 1, and the VM must not be migrated. Examples of such processes:
`nvidia-smi`, `nvtop`, a display server, or anything else that is not a CUDA program.

Then migrate the VM as usual (for example `migrate` in the QEMU monitor, or the management tool's live
migration). On the target host, inside the guest:

```bash
CUDA_CHECKPOINT=/path/to/cuda-checkpoint scripts/nvkvm-migrate-guest.sh resume
```

`resume` maps each GPU recorded at drain time to the GPU at the same index now (`cuda-checkpoint --device-map`)
and restores and unlocks every process. If the migration was refused or aborted, run `resume` on the source to
continue in place.

## Limits

- Only CUDA processes can be checkpointed. Graphics clients (EGL, Vulkan, NVKMS) block a drain.
- `cuda-checkpoint` does not support CUDA managed memory or IPC-exported memory, so a process using either cannot
  be drained.
- The target GPU needs enough free VRAM for the restored processes. VRAM is shared elastically between VMs, so
  check before migrating. A failed restore leaves the process checkpointed, and it can be retried.
- The GPU pause grows with the amount of VRAM in use, because checkpointed VRAM is copied with guest RAM.
