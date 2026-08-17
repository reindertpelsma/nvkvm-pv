#!/usr/bin/env bash
# run_test_vm.sh — launch a KVM guest VM with virtio-nvgpu device
#
# This starts a QEMU VM with:
#   - The virtio-nvgpu device patched into QEMU (built by scripts/build_qemu.sh)
#   - nvkvm-guest.ko available to build/load in the guest via 9p virtfs
#   - Ubuntu 24.04 cloud image prepared by scripts/setup_guest.sh
#
# Prerequisites:
#   - scripts/build_qemu.sh has been run  (or system QEMU has virtio-nvgpu)
#   - scripts/setup_guest.sh has been run
#
# Environment overrides:
#   QEMU_BIN — path to qemu-system-x86_64 binary
#   VM_MEM   — guest RAM   (default 16G).  For LLM parity runs this MUST be
#              >= model size + overhead or model load is disk-bound (the old
#              phantom "17x gap"); see tests/perf/README.md methodology #3.
#   VM_SMP   — guest vCPUs (default 4).  Host and guest vCPU counts differing
#              is a real confounder for CPU-side tokenisation/sampling; set
#              this and pin the host side to the same count.

set -euo pipefail

VM_MEM="${VM_MEM:-16G}"
VM_SMP="${VM_SMP:-4}"

REPO_ROOT="$(realpath "$(dirname "$0")/..")"

# ── QEMU binary: prefer our patched build, fall back to system QEMU ───────
NVKVM_QEMU="/opt/qemu-nvkvm/bin/qemu-system-x86_64"
if [ -n "${QEMU_BIN:-}" ]; then
    QEMU="$QEMU_BIN"
elif [ -x "$NVKVM_QEMU" ]; then
    QEMU="$NVKVM_QEMU"
    echo "INFO: Using patched QEMU at $NVKVM_QEMU"
else
    QEMU="qemu-system-x86_64"
    echo "WARN: $NVKVM_QEMU not found — falling back to system QEMU."
    echo "      Run scripts/build_qemu.sh to build the patched binary."
fi

# ── Paths ─────────────────────────────────────────────────────────────────
IMG="/opt/nvkvm-guest/ubuntu-24.04.qcow2"
SEED="/opt/nvkvm-guest/seed.iso"
SSH_PORT=2222

# Validate required files.
if [ ! -f "$IMG" ]; then
    echo "ERROR: disk image not found at $IMG"
    echo "       Run scripts/setup_guest.sh first."
    exit 1
fi
if [ ! -f "$SEED" ]; then
    echo "ERROR: cloud-init seed ISO not found at $SEED"
    echo "       Run scripts/setup_guest.sh first."
    exit 1
fi

echo "Starting nvkvm test VM..."
echo "QEMU         : $QEMU"
echo "Disk image   : $IMG"
echo "Seed ISO     : $SEED"
echo "Repo (9p)    : $REPO_ROOT  →  guest:/mnt/nvkvm  (tag: nvkvm_src)"
echo "SSH          : ssh ubuntu@localhost -p $SSH_PORT"
echo ""

exec "$QEMU" \
    -enable-kvm \
    -m "$VM_MEM" \
    -smp "$VM_SMP" \
    -cpu host \
    \
    -drive file="$IMG",format=qcow2,if=virtio \
    -drive file="$SEED",format=raw,if=virtio,readonly=on \
    \
    -netdev user,id=net0,hostfwd=tcp::"$SSH_PORT"-:22 \
    -device virtio-net-pci,netdev=net0 \
    \
    -device virtio-nvgpu-pci-non-transitional \
    \
    `# Identity-only NVIDIA PCI device at slot 7 (0000:00:07.0) — gives the` \
    `# DRM render node an NVIDIA-vendor parent so the Vulkan ICD binds it.` \
    `# No BARs/DMA; all GPU I/O still flows through virtio-nvgpu forwarding.` \
    -device nvkvm-gpu,addr=7 \
    \
    -virtfs local,path="$REPO_ROOT",mount_tag=nvkvm_src,security_model=mapped \
    \
    -serial stdio \
    -display none \
    "$@"
