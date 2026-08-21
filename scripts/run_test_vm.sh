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
# Overridable so a SECOND guest can run alongside the first: a distinct image
# and a distinct ssh forward are all that a concurrent guest needs (each gets
# its own virtio-nvgpu + nvkvm-gpu instance).  Sharing one GPU between two
# live guests is a supported configuration.
IMG="${VM_IMG:-/opt/nvkvm-guest/ubuntu-24.04.qcow2}"
SEED="${VM_SEED-/opt/nvkvm-guest/seed.iso}"
SSH_PORT="${VM_SSH_PORT:-2222}"

# Validate required files.
if [ ! -f "$IMG" ]; then
    echo "ERROR: disk image not found at $IMG"
    echo "       Run scripts/setup_guest.sh first."
    exit 1
fi
# The seed ISO is cloud-init's, and only cloud images consume it.  A guest
# installed from a live ISO (see setup_mint_guest.sh) has no cloud-init at all,
# so VM_SEED= (empty) drops the drive rather than failing.
SEED_ARG=""
if [ -n "$SEED" ]; then
    if [ ! -f "$SEED" ]; then
        echo "ERROR: cloud-init seed ISO not found at $SEED"
        echo "       Run scripts/setup_guest.sh first, or set VM_SEED= to boot without one."
        exit 1
    fi
    SEED_ARG="-drive file=$SEED,format=raw,if=virtio,readonly=on"
fi

# ── Optional extra 9p exports ────────────────────────────────────────────
# NVKVM_HOSTLIBS_DIR: the host's NVIDIA userspace, exported READ-ONLY.  The
# guest links against it instead of copying it in, so `apt upgrade` inside the
# guest cannot replace a driver library -- and the guest cannot write to the
# host's copy either.
# NVKVM_SHARE_DIR: a plain shared folder, read-write, for moving data in and out.
HOSTLIBS_ARG=""
if [ -n "${NVKVM_HOSTLIBS_DIR:-}" ] && [ -d "${NVKVM_HOSTLIBS_DIR}" ]; then
    HOSTLIBS_ARG="-virtfs local,path=${NVKVM_HOSTLIBS_DIR},mount_tag=nvkvm_libs,security_model=none,readonly=on"
fi
SHARE_ARG=""
if [ -n "${NVKVM_SHARE_DIR:-}" ] && [ -d "${NVKVM_SHARE_DIR}" ]; then
    # passthrough, not mapped: a shared folder is only useful if files the
    # guest writes are readable on the host as themselves.  mapped stores the
    # ownership in xattrs and the guest cannot write as its own uid at all.
    SHARE_ARG="-virtfs local,path=${NVKVM_SHARE_DIR},mount_tag=nvkvm_data,security_model=passthrough"
fi

echo "Starting nvkvm test VM..."

echo "QEMU         : $QEMU"
echo "Disk image   : $IMG"
echo "Seed ISO     : ${SEED:-(none)}"
echo "Repo (9p)    : $REPO_ROOT  →  guest:/mnt/nvkvm  (tag: nvkvm_src)"
echo "SSH          : ssh ubuntu@localhost -p $SSH_PORT"
echo ""

# Display backend.  Default "none": the normal deployment is headless and the
# guest is reached over SSH.  To watch (and drive) the guest desktop in a real
# window on a machine with a physical display:
#
#   NVKVM_QEMU_UI=1 scripts/build_qemu.sh --force     # once: builds GTK/SDL in
#   VM_DISPLAY="gtk,gl=on" VM_SERIAL=none scripts/run_test_vm.sh
#
# gl=on matters: the guest's composited frame arrives as a dma-buf and is
# scanned out with dpy_gl_scanout_dmabuf, so there is no readback.
VM_DISPLAY="${VM_DISPLAY:-none}"
VM_SERIAL="${VM_SERIAL:-stdio}"

# NOTE: we deliberately do NOT pass `-vga none`.  QEMU's default VGA is what
# GRUB and the early kernel draw on; removing it leaves the guest with no
# display device at all until nvkvm's KMS head comes up, which is why GRUB used
# to stall in gfxterm video init with nothing on any console to say so.  The two
# problems that once motivated `-vga none` are both *selection* problems and are
# solved by naming things instead of deleting them:
#   * screendump grabbing the text console -> give this device an id (below) so
#     the console CAN be named.  The naming itself currently trips a QEMU
#     abort; see the doc.  Not a reason to delete the boot console.
#   * a compositor in the guest picking the emulated card -> select the DRM node
#     by DRIVER, not by index (bochs-drm usually enumerates first as card0).
#     See scripts/setup_mint_guest.sh's run-session.sh, and
#     docs/internal/mint-guest-desktop.md.

# With a window there has to be something to type on: the headless config has
# no input devices at all, so a GTK/SDL window would show the guest desktop and
# swallow every key and click.  virtio-tablet sends absolute coordinates, so the
# host and guest pointers stay together without grabbing the mouse.
INPUT_ARGS=""
if [ "$VM_DISPLAY" != "none" ]; then
    INPUT_ARGS="-device virtio-keyboard-pci -device virtio-tablet-pci"
fi

exec "$QEMU" \
    -enable-kvm \
    -m "$VM_MEM" \
    -smp "$VM_SMP" \
    -cpu host \
    \
    -drive file="$IMG",format=qcow2,if=virtio \
    $SEED_ARG \
    \
    -netdev user,id=net0,hostfwd=tcp::"$SSH_PORT"-:22 \
    -device virtio-net-pci,netdev=net0 \
    \
    `# id= is needed to name this device's console at all.  The emulated VGA is` \
    `# console 0, so a bare 'screendump f.ppm' captures the BOOT console rather` \
    `# than the desktop.  NOTE: 'screendump <file> nvkvm0' currently ABORTS` \
    `# QEMU -- not our bug to trigger but ours to hit; see` \
    `# docs/internal/mint-guest-desktop.md "screendump by device id".  Until` \
    `# that is fixed, capture the desktop from inside the guest instead.` \
    -device virtio-nvgpu-pci-non-transitional,id=nvkvm0 \
    \
    `# Identity-only NVIDIA PCI device at slot 7 (0000:00:07.0) — gives the` \
    `# DRM render node an NVIDIA-vendor parent so the Vulkan ICD binds it.` \
    `# No BARs/DMA; all GPU I/O still flows through virtio-nvgpu forwarding.` \
    -device nvkvm-gpu,addr=7 \
    \
    -virtfs local,path="$REPO_ROOT",mount_tag=nvkvm_src,security_model=mapped \
    $HOSTLIBS_ARG \
    $SHARE_ARG \
    \
    $INPUT_ARGS \
    -serial "$VM_SERIAL" \
    -display "$VM_DISPLAY" \
    "$@"
