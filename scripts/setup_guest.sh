#!/usr/bin/env bash
# setup_guest.sh — prepare a KVM guest image for nvkvm integration testing.
#
# Host: Ubuntu 22.04, kernel 6.8.0-59-generic, NVIDIA RTX 3060.
# Guest: Ubuntu 24.04 (Noble) cloud image, 20 GB qcow2.
#
# Idempotent: already-downloaded / already-converted artefacts are reused.

set -euo pipefail

GUEST_DIR="/opt/nvkvm-guest"
REPO_ROOT="$(realpath "$(dirname "$0")/..")"

# Source image (Noble Numbat / 24.04).  The filename uses the Ubuntu codename
# "noble" but the user-facing label is 24.04.
# Overridable so you can bring your own cloud image.  Anything that runs
# cloud-init and can build an out-of-tree module works in principle; only the
# Ubuntu 24.04 image below is tested, and a distro whose kernel headers package
# is named differently will need the runcmd below adjusted.
CLOUD_IMG_URL="${NVKVM_GUEST_IMAGE_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img}"
CLOUD_IMG_RAW="$GUEST_DIR/noble-server-cloudimg-amd64.img"
QCOW2_IMG="$GUEST_DIR/ubuntu-24.04.qcow2"
SEED_ISO="$GUEST_DIR/seed.iso"

echo "=== nvkvm guest setup ==="
echo "Guest directory : $GUEST_DIR"
echo "Disk image      : $QCOW2_IMG"
echo ""

# ── Prerequisite tools ────────────────────────────────────────────────────
# Skipped when the tools are already there, so this works in a container
# image that ships them and on a host with no network.
if command -v wget >/dev/null && command -v qemu-img >/dev/null \
   && command -v genisoimage >/dev/null; then
    echo "[prereq] Required host tools already present — skipping apt."
else
    echo "[prereq] Installing required host tools..."
    apt-get update -q
    apt-get install -y wget qemu-utils genisoimage
fi

# ── 1. Create guest directory ─────────────────────────────────────────────
echo "[1/6] Creating $GUEST_DIR..."
mkdir -p "$GUEST_DIR"

# ── 2. Download Ubuntu 24.04 cloud image ──────────────────────────────────
if [ ! -f "$CLOUD_IMG_RAW" ]; then
    echo "[2/6] Downloading Ubuntu 24.04 cloud image..."
    wget -q --show-progress -O "$CLOUD_IMG_RAW" "$CLOUD_IMG_URL"
else
    echo "[2/6] Cloud image already present — skipping download."
fi

# ── 3. Convert to qcow2 and resize ───────────────────────────────────────
# GUEST_DISK_GROW: extra GB to add to the cloud image.  20G is fine for the
# microbenchmarks, but an LLM parity run needs the model weights AND a serving
# stack (vLLM/torch wheels are ~15 GB) on guest-local disk — a 9p read of a
# multi-GB weight file hits EIO, so the model cannot live on /mnt/nvkvm.
GUEST_DISK_GROW="${GUEST_DISK_GROW:-20G}"
if [ ! -f "$QCOW2_IMG" ]; then
    echo "[3/6] Converting to qcow2 and adding $GUEST_DISK_GROW..."
    qemu-img convert -f qcow2 -O qcow2 "$CLOUD_IMG_RAW" "$QCOW2_IMG"
    qemu-img resize "$QCOW2_IMG" "+$GUEST_DISK_GROW"
else
    echo "[3/6] qcow2 image already exists — skipping conversion."
fi

# ── 4. Create cloud-init user-data ────────────────────────────────────────
echo "[4/6] Writing cloud-init user-data..."
cat > "$GUEST_DIR/user-data" <<'CLOUDINIT'
#cloud-config
users:
  - name: ubuntu
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: false
    # video/render own /dev/dri/card0 and renderD128 (0660).  Without them
    # every GL client gets EACCES opening the render node, NVIDIA's EGL never
    # claims the device, and clients silently fall back to llvmpipe software
    # rendering — which looks like "the GPU does not work" rather than a
    # permissions problem.  cloud-init's default user is NOT in these groups.
    groups: [video, render]

# ssh_pwauth is a TOP-LEVEL cloud-config key. Nested under the users: entry it is
# silently ignored, sshd keeps PasswordAuthentication no, and the documented
# "password: ubuntu" login fails with 'Permission denied (publickey)'.
ssh_pwauth: true

# chpasswd, not a users: passwd hash. cloud-init's users module will not reset
# the password of a user that already exists in the image, so a hash there only
# works on a genuinely first boot. chpasswd applies every boot.
chpasswd:
  expire: false
  list: |
    ubuntu:ubuntu

package_update: true
write_files:
  # The GPU has to come back on every boot, not just the first one.
  # cloud-init runcmd runs ONCE per instance, so a guest that was set up here
  # and then rebooted used to come up with no module loaded, no /dev/nvidia*
  # and an nvidia-smi that says the driver is not running -- which reads like a
  # broken forward rather than "nothing loaded it".  A unit rebuilds against
  # the running kernel (so a guest kernel upgrade is survivable too) and
  # re-stages, both idempotent.
  - path: /etc/systemd/system/nvkvm-guest.service
    permissions: '0644'
    content: |
      [Unit]
      Description=Build and load the nvkvm guest module, stage NVIDIA userspace
      After=local-fs.target network.target
      [Service]
      Type=oneshot
      RemainAfterExit=yes
      ExecStart=/bin/bash -c 'lsmod | grep -q nvkvm_guest || { modprobe drm_shmem_helper 2>/dev/null; cd /mnt/nvkvm/src/guest && make KDIR=/lib/modules/$(uname -r)/build && insmod ./nvkvm-guest.ko; }'
      # `|| true`: stage_guest_libs.sh exits non-zero when an OPTIONAL library
      # is absent from the bundle (the Wayland/GBM EGL platform libraries are
      # not part of the driver, so a headless host legitimately has none).
      # That must not leave the unit in a failed state when compute is fine --
      # the warnings are in the journal either way.
      ExecStart=/bin/bash -c 'if ls /opt/nvidia-host/libcuda.so.* >/dev/null 2>&1; then NVKVM_LINK_LIBS=1 bash /mnt/nvkvm/scripts/stage_guest_libs.sh /opt/nvidia-host; elif ls -d /mnt/nvkvm/host-libs-* >/dev/null 2>&1; then bash /mnt/nvkvm/scripts/stage_guest_libs.sh; fi || true'
      [Install]
      WantedBy=multi-user.target

packages:
  - build-essential
  - git
  - python3
  # Vendor-neutral loaders only, so tests/validate.sh can exercise the
  # graphics rungs. These are the ICD/vendor dispatch layers (libglvnd, the
  # Vulkan loader); the actual drivers behind them are the NVIDIA libraries
  # that stage_guest_libs.sh installs, together with the ICD/vendor JSON it
  # writes. Deliberately NOT installing mesa-vulkan-drivers or libgl1-mesa-dri:
  # a software rasteriser in the guest would give validate.sh something to
  # succeed against that is not the GPU.
  - libvulkan1
  - libegl1
  # libGLX_nvidia.so.0 -- which IS the NVIDIA Vulkan ICD -- links against
  # libXext.  Minimal cloud images do not all ship it (Debian's does not), and
  # without it the Vulkan loader silently cannot load the ICD and falls back to
  # llvmpipe: measured on a Debian 12 guest as vk_device_is_nvidia FAIL with
  # "SOFTWARE RASTERISER", every other check passing.
  - libxext6
  - libx11-6
  - libgles2

runcmd:
  # Kernel headers for building nvkvm-guest.ko, by whatever name this distro
  # uses.  NOT in packages: above, because that list is distro-agnostic and
  # this name is not -- "linux-headers-virtual" is Ubuntu-only and fails the
  # whole cloud-init packages module on Debian.  The exact-version form works
  # on both; the others are fallbacks.
  - >-
    apt-get install -y "linux-headers-$(uname -r)"
    || apt-get install -y linux-headers-amd64
    || apt-get install -y linux-headers-generic
    || apt-get install -y linux-headers-virtual
  # Mount the shared 9p virtfs (nvkvm repo root).
  # Also record it in fstab: cloud-init runcmd runs ONCE per instance, so on
  # any later boot of the same image (e.g. re-running the VM after changing
  # the host driver) the mount would otherwise be missing and everything
  # under /mnt/nvkvm -- the module source, stage_guest_libs.sh, the test
  # suite -- would silently not be there.
  - mkdir -p /mnt/nvkvm /opt/nvidia-host /data
  - mount -t 9p -o trans=virtio,version=9p2000.L nvkvm_src /mnt/nvkvm
  - grep -q nvkvm_src /etc/fstab || echo 'nvkvm_src /mnt/nvkvm 9p trans=virtio,version=9p2000.L,nofail 0 0' >> /etc/fstab
  # Read-only share holding the host's NVIDIA userspace, when the host or
  # container exported one (tag nvkvm_libs).  Linking against it rather than
  # copying it in is what keeps a guest `apt upgrade` from replacing a driver
  # library: there is no driver library in the guest filesystem to replace.
  - bash -c 'mount -t 9p -o trans=virtio,version=9p2000.L,ro nvkvm_libs /opt/nvidia-host 2>/dev/null || true'
  - bash -c 'grep -q nvkvm_libs /etc/fstab || echo "nvkvm_libs /opt/nvidia-host 9p trans=virtio,version=9p2000.L,ro,nofail 0 0" >> /etc/fstab'
  # Shared folder for moving data in and out of the guest (tag nvkvm_data).
  - bash -c 'mount -t 9p -o trans=virtio,version=9p2000.L nvkvm_data /data 2>/dev/null || true'
  - bash -c 'grep -q nvkvm_data /etc/fstab || echo "nvkvm_data /data 9p trans=virtio,version=9p2000.L,nofail 0 0" >> /etc/fstab'
  # Build, load and stage -- last, because it needs the mounts above, and via
  # a unit so the same thing happens on every later boot rather than only this one.
  - systemctl daemon-reload
  - systemctl enable --now nvkvm-guest.service
CLOUDINIT

# ── 5. Create cloud-init meta-data ────────────────────────────────────────
echo "[5/6] Writing cloud-init meta-data..."
cat > "$GUEST_DIR/meta-data" <<METADATA
instance-id: nvkvm-guest-01
local-hostname: nvkvm-guest
METADATA

# ── 6. Generate seed ISO ──────────────────────────────────────────────────
echo "[6/6] Generating cloud-init seed ISO at $SEED_ISO..."
genisoimage \
    -output "$SEED_ISO" \
    -volid cidata \
    -joliet \
    -rock \
    "$GUEST_DIR/user-data" \
    "$GUEST_DIR/meta-data" \
    2>/dev/null

echo ""
echo "=== Guest setup complete ==="
echo ""
echo "Artefacts:"
echo "  Disk image : $QCOW2_IMG"
echo "  Seed ISO   : $SEED_ISO"
echo ""
echo "To launch the test VM (from the repo root):"
echo ""
echo "  sudo ./scripts/run_test_vm.sh"
echo ""
echo "The guest will:"
echo "  1. Boot Ubuntu 24.04"
echo "  2. Mount the repo at /mnt/nvkvm via 9p virtfs"
echo "  3. Build src/guest/nvkvm-guest.ko for the guest kernel"
echo "  4. insmod the module"
echo ""
echo "SSH access (once the guest has finished booting):"
echo "  ssh ubuntu@localhost -p 2222   # password: ubuntu"
