#!/usr/bin/env bash
# Entrypoint for the nvkvm container image (see Dockerfile).
#
# Prepares the guest disk on first run -- into /opt/nvkvm-guest, which the
# compose file keeps in a named volume so the 20 GB image survives a rebuild --
# and then boots the VM.  Arguments are passed through to QEMU.
set -euo pipefail

if [ ! -e /dev/kvm ]; then
    echo "ERROR: /dev/kvm is not present in the container." >&2
    echo "       Start it with:  --device /dev/kvm" >&2
    exit 1
fi

if ! ls /dev/nvidia0 >/dev/null 2>&1; then
    echo "WARNING: no /dev/nvidia* in the container -- GPU forwarding will be OFF." >&2
    echo "         Start it with:  --gpus all   (needs the NVIDIA container runtime)" >&2
    echo "" >&2
fi

# The guest needs NVIDIA userspace matched to the host driver.  Those libraries
# are not redistributable, so they cannot be baked into the image -- but the
# NVIDIA container runtime injects them here, so the bundle can be assembled at
# start-up and handed to the guest over 9p.
if ! ls -d /opt/nvkvm/host-libs-* >/dev/null 2>&1; then
    if ls /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.*.* >/dev/null 2>&1; then
        echo "=== assembling the host driver bundle for the guest ==="
        bash /opt/nvkvm/scripts/make_host_bundle.sh
        echo ""
    fi
fi

if [ ! -f /opt/nvkvm-guest/ubuntu-24.04.qcow2 ] || [ ! -f /opt/nvkvm-guest/seed.iso ]; then
    echo "=== first run: preparing the guest disk (downloads ~600 MB) ==="
    bash /opt/nvkvm/scripts/setup_guest.sh
    echo ""
fi

# Hand the guest the driver userspace as a read-only 9p share, and a shared
# folder for data.  run_test_vm.sh exports both only if the directory exists.
BUNDLE="$(ls -d /opt/nvkvm/host-libs-* 2>/dev/null | head -1)"
[ -n "$BUNDLE" ] && export NVKVM_HOSTLIBS_DIR="$BUNDLE"
mkdir -p /data
# 0700, not 0777.  The 0777 was there because the share used 9p
# security_model=passthrough and the guest wrote as its own uid; the share is
# mapped-xattr now (see run_test_vm.sh), so QEMU itself does every write and
# nothing needs world-write.  A world-writable directory on a host bind mount
# was the other half of the same hazard: anyone on the host could drop a file
# the guest would then read as trusted.
chmod 0700 /data 2>/dev/null || true
export NVKVM_SHARE_DIR=/data

exec bash /opt/nvkvm/scripts/run_test_vm.sh "$@"
