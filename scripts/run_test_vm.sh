#!/usr/bin/env bash
# run_test_vm.sh — launch a KVM guest VM with virtio-nvgpu device
#
# ############################################################################
# DEVELOPMENT HARNESS ONLY -- NOT A SANDBOX. DO NOT POINT IT AT AN UNTRUSTED
# GUEST.
#
# The -virtfs line below exports the ENTIRE REPOSITORY to the guest over 9p
# READ-WRITE (mount_tag=nvkvm_src, guest /mnt/nvkvm). The guest builds its
# kernel module on that share, which is why it is writable -- making it
# read-only is not a one-liner, the build has to move off it first.
#
# That gives guest root a three-step path to host root:
#   1. guest root edits /mnt/nvkvm/scripts/run_test_vm.sh (or anything else
#      under the tree);
#   2. scripts/run_remote_test.sh restart runs
#      `bash $REMOTE_DIR/scripts/run_test_vm.sh` ON THE HOST, AS ROOT;
#   3. that is the whole exploit.
#
# security_model=mapped does not mitigate this: it maps guest ownership/mode
# into host xattrs, it does not make the export read-only.
#
# This is a property of the harness, not of nvkvm's guest->host boundary. But
# do not fuzz, benchmark or demo an untrusted guest image with it, and do not
# leave it running on a shared machine. See CONTRIBUTING.md, "The dev VM
# harness is not a sandbox".
#
# The capability is DELIBERATE and is kept: for testing, this does not have to
# be secure. What it must not be is implicit. So since 2026-08-24 the writable
# export is OPT-IN, behind
#
#     NVKVM_DEV_HARNESS_INSECURE_RW=1
#
# and it prints a banner naming the guest-root -> host-root path whenever it is
# on. Unset, the repo is exported READ-ONLY: the VM boots, the guest reads the
# tree, and in-guest module builds fail on a read-only /mnt/nvkvm with a
# message pointing back here. Nobody gets the writable share by accident, and
# nobody who has it can claim they were not told.
# ############################################################################
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

# ── P-7: the repo export is READ-ONLY unless explicitly opted in ─────────
# See the banner at the top of this file for the whole path. Short version:
# guest root writes scripts/run_test_vm.sh on this share, and the next
# `run_remote_test.sh restart` runs it on the host as root.
#
# The normal guest service now copies the module sources into a private guest
# build directory, so boot, module build, benchmark, demo and driver bring-up
# all work through a read-only export.  The opt-in remains only for developers
# who deliberately want to edit or build directly on the mounted host tree;
# keep that exceptional path explicit, named and loudly announced.
if [ "${NVKVM_DEV_HARNESS_INSECURE_RW:-0}" = "1" ]; then
    REPO_9P_MODE="read-write  (NVKVM_DEV_HARNESS_INSECURE_RW=1)"
    REPO_9P_RO=""
    cat >&2 <<'INSECURE'

  ############################################################################
  #  NVKVM_DEV_HARNESS_INSECURE_RW=1 — THE REPO IS EXPORTED TO THE GUEST
  #  READ-WRITE. THIS GIVES GUEST ROOT A PATH TO HOST ROOT:
  #
  #    1. guest root edits any file under /mnt/nvkvm — say
  #       scripts/run_test_vm.sh;
  #    2. scripts/run_remote_test.sh restart runs that script ON THE HOST,
  #       AS ROOT;
  #    3. that is the whole exploit. No race, no timing.
  #
  #  This is a DEVELOPMENT HARNESS. It is not a supported configuration and
  #  it is not a sandbox. Point it at a guest image you trust completely and
  #  nothing else: do not fuzz it, do not demo an untrusted image on it, and
  #  do not leave it running on a shared machine.
  ############################################################################

INSECURE
else
    REPO_9P_MODE="READ-ONLY   (set NVKVM_DEV_HARNESS_INSECURE_RW=1 for read-write)"
    REPO_9P_RO=",readonly=on"
    cat >&2 <<'SAFE'

  NOTE: the repo 9p export is READ-ONLY. The nvkvm-guest service copies module
  sources into a private guest build directory, so normal boot and validation
  work without host-tree writes. To edit or build directly on /mnt/nvkvm,
  re-run with

      NVKVM_DEV_HARNESS_INSECURE_RW=1

  and read the banner it prints first: a writable export hands guest root a
  path to host root.

SAFE
fi

echo "Starting nvkvm test VM..."

echo "QEMU         : $QEMU"
echo "Disk image   : $IMG"
echo "Seed ISO     : ${SEED:-(none)}"
echo "Repo (9p)    : $REPO_ROOT  →  guest:/mnt/nvkvm  (tag: nvkvm_src)"
echo "Repo access  : $REPO_9P_MODE"
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
#
# virtio-mouse, the RELATIVE device, is OPT-IN and off by default.  It is not
# redundant with the tablet -- a tablet reports "the pointer is at (x, y)" and
# never "the pointer moved by (dx, dy)", so anything that takes a pointer lock
# (every first-person game) reads deltas and gets nothing, and the view does not
# turn.  Observed with Minecraft rendering at 60 fps in the guest and mouse-look
# dead.
#
# WHY IT IS STILL OPT-IN.  QEMU keeps one "current" mouse handler and makes it
# whichever was most recently ACTIVATED (ui/input.c: activate does
# QTAILQ_INSERT_HEAD, find_handler returns the first match), and the GUEST
# decides that by enabling the device -- so cmdline ordering does not control
# it.  Shipping both silently made the relative mouse current: `info mice`
# reported
#
#     Mouse #4: QEMU Virtio Tablet (absolute)
#   * Mouse #5: QEMU Virtio Mouse
#
# with the tablet present but not current.  qemu_input_is_absolute() is then
# false, and in that state ui/gtk.c delivers pointer motion to the guest only
# while the pointer is grabbed -- so hovering moves nothing and the desktop is
# dead until the user grabs.  It used to be worse: a plain left click took BOTH
# pointer and keyboard and was swallowed rather than forwarded.  That part is
# gone, removed by patches/0006-gtk-no-implicit-grab-on-click.patch, so a click
# is now just a click.
#
# patches/0007-gtk-grab-switches-the-guest-pointing-device.patch is meant to fix
# the TRANSITIONS -- grab activates a relative device, ungrab puts the absolute
# one back.  TWO reasons that is not enough to flip this default:
#
#   * 0007 IS NOT KNOWN TO WORK.  The same switch performed by hand from the
#     monitor (`mouse_set 5`, then grab) left mouse look dead in the guest on
#     real hardware.  Read that patch's header: it carries the analysis and the
#     one `evtest` command in the guest that says whether the break is above or
#     below evdev.  Nobody has run it yet.
#   * even if it works, it fixes the transitions and not the INITIAL state.  No
#     grab has happened when the guest desktop first appears, and the guest has
#     already chosen.  With both devices the window still opens in relative
#     mode, and it takes one ctrl-alt-g in and out -- whose ungrab selects the
#     tablet -- before hovering works.  Recoverable rather than stuck, still a
#     bad first impression.
#
# Flip this default when someone with a display has confirmed, on hardware,
# both that 0007 delivers motion and either that the guest leaves an absolute
# device current at desktop time or that the one-cycle recovery is acceptable.
# That is a measurement, not an argument.  The last version of this comment was
# an argument, and it shipped the bug above.
#
# With VM_RELATIVE_MOUSE=1 both devices exist and the monitor's `mouse_set <n>`
# switches between them by hand, live, no restart.  Either way the guest can
# never take the grab itself: every grab in ui/gtk.c is a user gesture --
# ctrl-alt-g or View -> Grab Input, and nothing else now that the click path is
# gone.  QEMU has no channel to learn that a guest app wanted a pointer lock,
# and while grabbed the window title carries "Press Ctrl+Alt+G to release grab".
# 0007 deliberately changes none of that: it only decides which device a grab
# the USER already asked for delivers to.  Same consent model as a browser's
# Pointer Lock API, and it should stay that way.
INPUT_ARGS=""
if [ "$VM_DISPLAY" != "none" ]; then
    INPUT_ARGS="-device virtio-keyboard-pci -device virtio-tablet-pci"
    if [ "${VM_RELATIVE_MOUSE:-0}" = "1" ]; then
        INPUT_ARGS="$INPUT_ARGS -device virtio-mouse-pci"
    fi
fi

# Preflight KVM before exec'ing QEMU.  Without this the user gets QEMU's bare
# "Could not access KVM kernel module: No such file or directory" after having
# already downloaded a release, installed dependencies and built a guest image
# -- which does not tell them that most GPU cloud rentals are containers that
# cannot provide the device, or that CPU vmx/svm flags prove nothing because a
# container inherits its host's.  Open the device rather than stat it: group
# membership granted since this shell started is not in effect here.
if ! (exec 3<>/dev/kvm) 2>/dev/null; then
    if [ ! -e /dev/kvm ]; then
        echo "ERROR: /dev/kvm does not exist -- this host cannot run nvkvm." >&2
        echo "  Most GPU cloud rentals are containers without it; CPU vmx/svm" >&2
        echo "  flags are inherited from the host and prove nothing." >&2
    else
        echo "ERROR: /dev/kvm exists but this user cannot open it." >&2
        echo "  sudo usermod -aG kvm \"$USER\"   # then log out and back in" >&2
    fi
    exit 1
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
    `# Export of the whole repo. READ-ONLY unless NVKVM_DEV_HARNESS_INSECURE_RW=1,` \
    `# because read-write lets guest root rewrite any script here, including` \
    `# ones the HOST later runs as root (run_remote_test.sh restart). Dev` \
    `# harness only -- see the banner at the top of this file.` \
    -virtfs local,path="$REPO_ROOT",mount_tag=nvkvm_src,security_model=mapped$REPO_9P_RO \
    $HOSTLIBS_ARG \
    $SHARE_ARG \
    \
    $INPUT_ARGS \
    -serial "$VM_SERIAL" \
    -display "$VM_DISPLAY" \
    "$@"
