#!/usr/bin/env bash
# Entrypoint for the SteamOS compose service.
#
# The durable state is split deliberately:
#   /var/lib/nvkvm-steamos  recovery image, installed qcow2, OVMF vars, SSH key
#   /data                    the read-write 9p share (public key only)
#   /run/nvkvm              host broker directory, bind-mounted as a directory
#
# QEMU is started immediately even when the broker socket is absent. The relay
# reconnects with bounded backoff and re-attaches the last frame, so waiting here
# would recreate an ordering dependency that the broker protocol removed.
set -euo pipefail

STATE_DIR="${NVKVM_STEAMOS_STATE_DIR:-/var/lib/nvkvm-steamos}"
DATA_DIR="${NVKVM_STEAMOS_DATA_DIR:-/data}"
BROKER_DIR="${NVKVM_BROKER_DIR_IN_CONTAINER:-/run/nvkvm}"
BROKER_SOCKET="${NVKVM_BROKER_SOCKET:-$BROKER_DIR/steamos.sock}"
QEMU_DISPLAY="${NVKVM_STEAMOS_QEMU_DISPLAY:-nvkvm-broker,socket=$BROKER_SOCKET}"
QCOW="${NVKVM_STEAMOS_QCOW:-$STATE_DIR/steamos.qcow2}"
PINNED_RECOVERY="steamdeck-oobe-repair-20260707.10-3.8.14.img.bz2"
RECOVERY_BASE="https://steamdeck-images.steamos.cloud/recovery"
LATEST=0
INSTALL_SHELL=0
QEMU_EXTRA=()

log() { printf '[steamos-container] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --latest)       LATEST=1; shift ;;
        --setup-shell)  INSTALL_SHELL=1; shift ;;
        --)             shift; QEMU_EXTRA+=("$@"); break ;;
        *)              QEMU_EXTRA+=("$1"); shift ;;
    esac
done

require_mount() {
    mountpoint -q "$1" || die "$1 is not a mount point. Refusing to put durable VM state in the container layer."
}

[ -c /dev/kvm ] || die "/dev/kvm is absent (Compose must grant the device)."
[ -r /dev/kvm ] && [ -w /dev/kvm ] \
    || die "/dev/kvm is not usable. Set NVKVM_KVM_GID to: stat -c %g /dev/kvm"
require_mount "$STATE_DIR"
require_mount "$DATA_DIR"
require_mount "$BROKER_DIR"
mkdir -p "$STATE_DIR/recovery" "$STATE_DIR/alpine" "$STATE_DIR/ssh"
chmod 0700 "$STATE_DIR/ssh"
chmod 0777 "$DATA_DIR" 2>/dev/null || true

# Keep the private key off the guest-visible share. Only its public half crosses
# 9p; nvkvm-steamos-ssh reads the private half from the state volume. The same
# key is installed for both the interactive user and root: anyone controlling
# this container can already read and alter the VM disk and running QEMU state.
ensure_ssh_key() {
    local private="$STATE_DIR/ssh/id_ed25519" public_tmp="$DATA_DIR/.authorized_keys.tmp"
    local root_tmp="$DATA_DIR/.root_authorized_keys.tmp"
    if [ -s "$DATA_DIR/authorized_keys" ]; then
        log "SSH: using existing /data/authorized_keys"
        if [ ! -s "$DATA_DIR/root_authorized_keys" ]; then
            cp -f "$DATA_DIR/authorized_keys" "$root_tmp"
            chmod 0644 "$root_tmp"
            mv -f "$root_tmp" "$DATA_DIR/root_authorized_keys"
            log "SSH: mirrored the public key for direct root access"
        fi
        return 0
    fi
    if [ ! -s "$private" ]; then
        log "SSH: generating a persistent Ed25519 key in the state volume"
        ssh-keygen -q -t ed25519 -N '' -C nvkvm-steamos -f "$private"
    fi
    ssh-keygen -y -f "$private" > "$public_tmp"
    chmod 0644 "$public_tmp"
    mv -f "$public_tmp" "$DATA_DIR/authorized_keys"
    cp -f "$DATA_DIR/authorized_keys" "$root_tmp"
    chmod 0644 "$root_tmp"
    mv -f "$root_tmp" "$DATA_DIR/root_authorized_keys"
    log "SSH: public key written for the interactive user and root; password login remains disabled"
}

latest_recovery_url() {
    local listing name
    listing="$(curl -fsSL "$RECOVERY_BASE/")" \
        || die "could not read the SteamOS recovery index"
    name="$(printf '%s\n' "$listing" \
        | grep -oE 'steamdeck-oobe-repair-[^"?<> ]+\.img\.bz2' \
        | sort -Vu | tail -1)"
    [ -n "$name" ] || die "the recovery index contained no repair image (page shape changed?)"
    printf '%s/%s\n' "$RECOVERY_BASE" "$name"
}

record_or_verify_checksum() {
    local archive="$1" dir base sumfile
    dir="$(dirname "$archive")"; base="$(basename "$archive")"; sumfile="$archive.sha256"
    if [ -s "$sumfile" ]; then
        (cd "$dir" && sha256sum -c "$(basename "$sumfile")") \
            || die "checksum mismatch for $archive; it was not re-fetched automatically"
    else
        (cd "$dir" && sha256sum "$base" > "$(basename "$sumfile").tmp")
        mv -f "$sumfile.tmp" "$sumfile"
        log "recorded SHA-256 in $sumfile"
    fi
}

prepare_recovery() {
    local url filename archive raw
    url="${NVKVM_STEAMOS_RECOVERY_URL:-$RECOVERY_BASE/$PINNED_RECOVERY}"
    if [ "$LATEST" = 1 ] || [ "${NVKVM_STEAMOS_LATEST:-0}" = 1 ]; then
        url="$(latest_recovery_url)"
        log "latest-image lookup selected: $url"
    else
        log "using pinned recovery: ${url##*/}"
    fi
    filename="${url##*/}"; filename="${filename%%\?*}"
    case "$filename" in *.img.bz2) ;; *) die "recovery URL must name an .img.bz2: $url" ;; esac
    archive="$STATE_DIR/recovery/$filename"
    raw="${archive%.bz2}"

    if [ ! -s "$archive" ]; then
        log "downloading SteamOS recovery (large, resumable): $url"
        curl -fL --retry 3 --retry-all-errors -C - -o "$archive.part" "$url" \
            || die "recovery download failed; the .part file was kept for resume"
        mv -f "$archive.part" "$archive"
    fi
    record_or_verify_checksum "$archive"

    if [ ! -s "$raw" ]; then
        log "decompressing $(basename "$archive")"
        bzip2 -dc "$archive" > "$raw.part" \
            || die "recovery decompression failed"
        mv -f "$raw.part" "$raw"
    fi
    printf '%s\n' "$raw"
}

ensure_ssh_key
export PATH="/opt/qemu-nvkvm/bin:$PATH"

container_driver_version() {
    awk '{for(i=1;i<=NF;i++) if ($i ~ /^[0-9]+\.[0-9]+/) {print $i; exit}}' \
        /proc/driver/nvidia/version 2>/dev/null
}

if [ "$INSTALL_SHELL" = 1 ]; then
    RECOVERY_IMG="$(prepare_recovery)"
    exec /opt/nvkvm/install_steamos_vm.sh \
        --repair "$RECOVERY_IMG" --out "$QCOW" \
        --alpine-dir "$STATE_DIR/alpine" --share /opt/nvkvm --shell
fi

if [ ! -s "$QCOW" ]; then
    RECOVERY_IMG="$(prepare_recovery)"
    DRIVER_VERSION="$(container_driver_version)"
    [[ "$DRIVER_VERSION" =~ ^[0-9]+([.][0-9]+)+$ ]] \
        || die "could not determine the NVIDIA driver version exposed to this container"
    log "first run: installing a dual-slot SteamOS image with Valve's installer"
    log "provisioning NVIDIA userspace for dynamically detected driver $DRIVER_VERSION"
    /opt/nvkvm/install_steamos_vm.sh \
        --repair "$RECOVERY_IMG" \
        --out "$QCOW" \
        --size "${NVKVM_STEAMOS_DISK_SIZE:-64G}" \
        --alpine-dir "$STATE_DIR/alpine" \
        --stages "${NVKVM_STEAMOS_INSTALL_STAGES:-repair,provision}" \
        --share /opt/nvkvm \
        --profile "${NVKVM_STEAMOS_PROFILE:-steamos}" \
        --driver-version "$DRIVER_VERSION" \
        --memory "${NVKVM_INSTALL_MEMORY_MB:-4096}" \
        --cpus "${NVKVM_INSTALL_CPUS:-4}" \
        --log "$STATE_DIR/install.log"
fi

OVMF_CODE="${NVKVM_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_TEMPLATE="${NVKVM_OVMF_VARS_TEMPLATE:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS="$STATE_DIR/OVMF_VARS.fd"
[ -f "$OVMF_CODE" ] || die "OVMF code not found at $OVMF_CODE"
if [ ! -f "$OVMF_VARS" ]; then
    install -m 0600 "$OVMF_TEMPLATE" "$OVMF_VARS"
fi

rm -f "$STATE_DIR/qmp.sock"
log "booting SteamOS now; broker presence is not an ordering requirement"
log "display socket: $BROKER_SOCKET"
log "QEMU display backend: $QEMU_DISPLAY"
log "SSH after provisioning: docker compose exec steamos nvkvm-steamos-ssh"

export NVKVM_PRESENT_TIMING="${NVKVM_PRESENT_TIMING:-1}"
exec /opt/qemu-nvkvm/bin/qemu-system-x86_64 \
    -name nvkvm-steamos \
    -machine q35,accel=kvm -cpu host \
    -m "${VM_MEM:-12G}" -smp "${VM_SMP:-8}" \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,unit=1,file=$OVMF_VARS" \
    -drive "file=$QCOW,format=qcow2,if=none,id=nvm0" \
    -device nvme,drive=nvm0,serial=nvkvmsteamos \
    -netdev "user,id=net0,hostfwd=tcp::${NVKVM_STEAMOS_SSH_PORT:-15022}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -vga none \
    -device virtio-nvgpu-pci-non-transitional,id=nvkvm0 \
    -device nvkvm-gpu,addr=7 \
    -virtfs local,path=/opt/nvkvm,mount_tag=nvkvm,security_model=none,readonly=on \
    -virtfs "local,path=$DATA_DIR,mount_tag=data,security_model=passthrough" \
    -fw_cfg opt/ovmf/X-PciMmio64Mb,string=262144 \
    -device virtio-keyboard-pci -device virtio-tablet-pci -device virtio-mouse-pci \
    -qmp "unix:$STATE_DIR/qmp.sock,server=on,wait=off" \
    -serial "file:$STATE_DIR/serial.log" \
    -monitor none \
    -display "$QEMU_DISPLAY" \
    "${QEMU_EXTRA[@]}"
