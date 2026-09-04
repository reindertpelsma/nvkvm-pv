# Install a release: container, tarball, or from source

The README shows the shortest path. This is the whole thing: the `/dev/kvm`
preflight, the container knobs, the tarball's runtime dependencies and glibc
floor, and the source build.

## Before anything else: can you open /dev/kvm?


**Check you can actually open `/dev/kvm` before anything else.** That is the
whole test — not CPU flags, which a container inherits from its host, so
`grep vmx /proc/cpuinfo` reads healthy inside one that has no `/dev/kvm`:

```bash
exec 3<>/dev/kvm && echo "KVM usable" && exec 3>&-
```

That opens the device rather than asking about it, which is the only test that
counts: `test -w` consults permission bits, and permission bits are not the
whole story — group membership does not take effect in a shell you were already
in when it was granted.

If it fails with permission denied, you are almost certainly not in the group
that owns the node (`crw-rw---- root kvm` on most distributions):

```bash
sudo usermod -aG kvm "$USER"    # then log out and back in, or: newgrp kvm
```

Containers are fine **provided the device is passed in** — the
[`docker-compose.yml`](../../docker-compose.yml) here does exactly that. What does not
work is a container without it, or a VM whose host has nested virtualisation
switched off.

Without `/dev/kvm`, QEMU silently falls back to software emulation (TCG): it
appears to work and is unusably slow, which is the most expensive way to find
out.

## The three ways in


Three ways in, in the order most people should try them. The container is
first because it is the only one that does not make you wait for a QEMU build.

Everything below is published from a tag by
[`.github/workflows/release.yml`](../../.github/workflows/release.yml) on a
GitHub-hosted runner and carries a
[build-provenance attestation](#the-three-ways-in) — so "did this
come from this repository?" is a question you can answer rather than assume.

### Docker (start here)

```bash
docker run --rm -it --device /dev/kvm --gpus all \
    -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics,display,video \
    -p 127.0.0.1:2222:2222 -v nvkvm-guest:/opt/nvkvm-guest \
    ghcr.io/reindertpelsma/nvkvm-pv:v0.2.1
ssh -p 2222 ubuntu@127.0.0.1    # password: ubuntu -- nvidia-smi already works
```

`NVIDIA_DRIVER_CAPABILITIES` is not optional decoration: `--gpus all` alone
gives the container `compute,utility`, and the guest then gets a compute-only
driver with no GL or Vulkan libraries — which does not fail, it silently falls
back to llvmpipe.

[`docker-compose.yml`](../../docker-compose.yml) is the same thing with those
capabilities, a tightened `cap_drop`/`cap_add` set, the shared folder and the
named volume already spelled out, and is the better starting point for anything
you intend to keep:

```bash
docker compose pull && docker compose up   # the published image
docker compose up --build                  # or build QEMU here instead
```

The guest comes up with the module loaded and the GPU present — no manual
staging step.

- **Requirements:** `/dev/kvm` and, for the GPU, the
  [NVIDIA container runtime](https://github.com/NVIDIA/nvidia-container-toolkit).
  Both are already in [`docker-compose.yml`](../../docker-compose.yml). No
  `--privileged`, no added capabilities.
- **The isolation trade differs from a bare host, and is not obviously worse.**
  Containers block user namespaces, so the isolate falls back to UID separation —
  but most of our audit findings target the VMM, which a container confines and a
  bare host does not. Neither is ready for untrusted tenants:
  [the trade in full](../internal/isolate-model.md),
  [`SECURITY.md`](../../SECURITY.md).
- **The guest survives reboots and `apt upgrade`.** The driver userspace is
  mounted read-only rather than copied in, so nothing in the guest can replace
  it, and a systemd unit rebuilds the module against the running kernel on every
  boot. `./data` is shared at `/data` for moving work in and out.
- **Knobs:** `VM_MEM`, `VM_SMP`, and `NVKVM_GUEST_IMAGE_URL` to bring your own
  cloud image — anything cloud-init capable that can build an out-of-tree module
  should work; only Ubuntu 24.04 is tested.

### Prebuilt tarball, on a bare host

The bare-host install — stronger isolation than a container, and no QEMU build.
From the [releases page](https://github.com/reindertpelsma/nvkvm-pv/releases):

```bash
# runtime dependencies -- the tarball ships QEMU, not what QEMU links against
sudo apt install -y libglib2.0-0t64 libpixman-1-0 libslirp0 \
                    qemu-utils genisoimage sshpass

tar xzf nvkvm-<version>-linux-x86_64.tar.gz && cd nvkvm-<version>
sudo cp -a qemu-nvkvm /opt/qemu-nvkvm
sudo install -Dm755 src/stub/nvkvm_stub /usr/lib/nvkvm/nvkvm_stub
```

```bash
sudo bash scripts/setup_guest.sh    # fetches an Ubuntu 24.04 cloud image
```

**Stage the guest's NVIDIA userspace, or the guest boots with no GPU** —
`nvidia-smi: command not found`, and `validate.sh` failing its CUDA checks. The
container does this for you; on a bare host nothing does.

```bash
sudo bash scripts/make_host_bundle.sh            # -> ./host-libs-<ver>
sudo NVKVM_HOSTLIBS_DIR="$PWD/host-libs-<ver>" \
     NVKVM_DEV_HARNESS_INSECURE_RW=1 bash scripts/run_test_vm.sh
```

```bash
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh   # inside the guest
nvidia-smi                                         # should name your GPU
```

The bundle and the guest must match the host driver exactly or `libcuda`
refuses to initialise; [`stage-guest-libraries.md`](stage-guest-libraries.md)
covers why those failures are silent. **Re-run `make_host_bundle.sh` after every
host driver upgrade** — the guest re-stages from whatever bundle it is given, so
a stale one is re-applied on every boot (`cuInit 803`, forever). The container
compares versions at start-up and rebuilds itself.

x86-64 only, and the QEMU in it is headless (no GTK/SDL window — build from
source for that). It is a real binary built on Ubuntu 24.04, so it needs
**glibc 2.38 or newer**: on an older host (Ubuntu 22.04 is glibc 2.35) use the
container, which brings its own userspace. Exact floor and runtime package list
are in `RELEASE.md` inside the tarball
([detail](build.md#installing-the-tarball)).

**The guest kernel module is not in there and cannot be** — it is compiled
against *your* guest's kernel. That is why `src/guest/` ships with the tarball:
`scripts/run_test_vm.sh` exports the unpacked directory to the guest over 9p at
`/mnt/nvkvm`, and cloud-init builds the module there on first boot.

Building it there needs the export to be writable, and that is opt-in — which
is why `NVKVM_DEV_HARNESS_INSECURE_RW=1` appears on the `run_test_vm.sh` line
above.

Read the banner it prints. A writable export gives guest root a path to host
root, so the harness is for guests you trust completely — see
[CONTRIBUTING.md, "The dev VM harness is not a sandbox"](../../CONTRIBUTING.md#the-dev-vm-harness-is-not-a-sandbox).
Without the flag the VM still boots and the export is read-only; only the
in-guest module build needs it.

### From source, if you want to hack on it

```bash
git clone https://github.com/reindertpelsma/nvkvm-pv.git nvkvm && cd nvkvm
bash scripts/build_qemu.sh --install-deps   # builds the isolate stub, then QEMU 11.1 with the nvkvm device
sudo bash scripts/setup_guest.sh            # fetches an Ubuntu 24.04 cloud image and prepares a disk
sudo bash scripts/make_host_bundle.sh       # this host's driver userspace -> ./host-libs-<ver>
```

Then boot and stage, exactly as the tarball path above — `build_qemu.sh`
installs to `/opt/qemu-nvkvm`, so `run_test_vm.sh` finds the binary itself.

```bash
sudo NVKVM_HOSTLIBS_DIR="$PWD/host-libs-<ver>" \
     NVKVM_DEV_HARNESS_INSECURE_RW=1 bash scripts/run_test_vm.sh
```

```bash
# inside the guest, once it has booted
sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh
nvidia-smi                                  # the guest should now name your GPU
```

Most of the wall clock is QEMU. The script is a convenience, not the
mechanism: everything it changes in upstream QEMU is twelve patch files in
[`patches/`](../../patches/) — 2273 lines, applied with `git apply` — plus a copy of the
device sources into `hw/misc/`.
[`docs/howto/build.md`](build.md) lists the whole delta and walks the
same build by hand, command by command, if you would rather not run a script
over your QEMU tree; [`CONTRIBUTING.md`](../../CONTRIBUTING.md) has the traps in
this build, including which changes need a `--force` rebuild.


## If `nvidia-smi` says "No devices were found" on an RTX 50-series card

Blackwell and Hopper cannot bind NVIDIA's **proprietary** module at all, so the
host cannot see the card either — this is an NVIDIA requirement, not an nvkvm
one. Rare on a working desktop (you would have no display, and distro `-open`
metapackages are the default); common on cloud images that ship the wrong
flavour. The reason is in `dmesg`, not `nvidia-smi`:

```
NVRM: RmInitAdapter failed! (0x22:0x56:884)
NVRM: ... requires use of the NVIDIA open kernel modules.
```

`modinfo nvidia | grep '^license:'` — `Dual MIT/GPL` is the open module,
`NVIDIA` the proprietary one. Install `nvidia-driver-580-open` or newer (or
NVIDIA's installer with `-m=kernel-open`); purge first, the 5xx packages
conflict.

## See also

- [`build.md`](build.md) — the full QEMU delta, built by hand, and how to
  verify what you downloaded
- [`run.md`](run.md) — running the guest once it is installed
- [`stage-guest-libraries.md`](stage-guest-libraries.md) — staging the guest
  driver libraries, including `NVKVM_STAGE_XORG`
- [`../reference/supported-drivers.md`](../reference/supported-drivers.md) —
  driver versions and module flavours
