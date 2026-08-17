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
CLOUD_IMG_URL="https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img"
CLOUD_IMG_RAW="$GUEST_DIR/noble-server-cloudimg-amd64.img"
QCOW2_IMG="$GUEST_DIR/ubuntu-24.04.qcow2"
SEED_ISO="$GUEST_DIR/seed.iso"

echo "=== nvkvm guest setup ==="
echo "Guest directory : $GUEST_DIR"
echo "Disk image      : $QCOW2_IMG"
echo ""

# ── Prerequisite tools ────────────────────────────────────────────────────
echo "[prereq] Installing required host tools..."
apt-get update -q
apt-get install -y wget qemu-utils genisoimage

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
packages:
  - build-essential
  - git
  - python3
  - linux-headers-virtual
  # Vendor-neutral loaders only, so tests/validate.sh can exercise the
  # graphics rungs. These are the ICD/vendor dispatch layers (libglvnd, the
  # Vulkan loader); the actual drivers behind them are the NVIDIA libraries
  # that stage_guest_libs.sh installs, together with the ICD/vendor JSON it
  # writes. Deliberately NOT installing mesa-vulkan-drivers or libgl1-mesa-dri:
  # a software rasteriser in the guest would give validate.sh something to
  # succeed against that is not the GPU.
  - libvulkan1
  - libegl1
  - libgles2

runcmd:
  # Mount the shared 9p virtfs (nvkvm repo root).
  # Also record it in fstab: cloud-init runcmd runs ONCE per instance, so on
  # any later boot of the same image (e.g. re-running the VM after changing
  # the host driver) the mount would otherwise be missing and everything
  # under /mnt/nvkvm -- the module source, stage_guest_libs.sh, the test
  # suite -- would silently not be there.
  - mkdir -p /mnt/nvkvm
  - mount -t 9p -o trans=virtio,version=9p2000.L nvkvm_src /mnt/nvkvm
  - grep -q nvkvm_src /etc/fstab || echo 'nvkvm_src /mnt/nvkvm 9p trans=virtio,version=9p2000.L,nofail 0 0' >> /etc/fstab
  # Build the guest kernel module against the running guest kernel.
  - cd /mnt/nvkvm/src/guest && make KDIR=/lib/modules/$(uname -r)/build
  # Load the module.
  - insmod /mnt/nvkvm/src/guest/nvkvm-guest.ko
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
