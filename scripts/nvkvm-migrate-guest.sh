#!/bin/bash
# nvkvm-migrate-guest.sh -- release and reacquire the GPU around a live migration (run INSIDE the guest, as root).
#
#   nvkvm-migrate-guest.sh drain    checkpoint every process that holds /dev/nvidia*, all or nothing
#   nvkvm-migrate-guest.sh resume   restore them, remapping each GPU to the one at the same index now
#   nvkvm-migrate-guest.sh status   list GPU holders and their checkpoint state
#
# GPU state (VRAM, driver objects) is host-local and cannot travel with the VM. cuda-checkpoint moves a CUDA
# process's GPU state into its own memory and closes its device files; the virtio-nvgpu device then has no
# sessions and QEMU allows the migration (it refuses on its own otherwise). After the move, resume restores
# every process onto the new host's GPUs.
#
# drain is all or nothing: if any holder cannot be checkpointed (nvidia-smi, nvtop, a display server; anything
# that is not a CUDA process) or a device file stays open, everything already checkpointed is restored, the
# exit status is 1, and the VM must not be migrated.
#
# Needs NVIDIA's cuda-checkpoint (https://github.com/NVIDIA/cuda-checkpoint) matching the host driver; set
# CUDA_CHECKPOINT to its path if it is not on PATH. State is kept in $STATE_DIR (default /run/nvkvm-migrate).
set -u
CC=${CUDA_CHECKPOINT:-$(command -v cuda-checkpoint || echo /usr/local/bin/cuda-checkpoint)}
STATE_DIR=${STATE_DIR:-/run/nvkvm-migrate}
LOCK_TIMEOUT_MS=${LOCK_TIMEOUT_MS:-10000}

die() { echo "nvkvm-migrate-guest: $*" >&2; exit 1; }
holders() {  # pids with a GPU device open: /dev/nvidia* (not nvidia-caps) or nvkvm's DRM nodes
    local p
    for p in /proc/[0-9]*; do
        [ -n "$(find "$p/fd" -mindepth 1 -maxdepth 1 \( -lname /dev/nvidiactl -o -lname '/dev/nvidia[0-9]*' \
                 -o -lname /dev/nvidia-uvm -o -lname /dev/nvidia-uvm-tools -o -lname /dev/nvidia-modeset \
                 -o -lname '/dev/dri/card[0-9]*' -o -lname '/dev/dri/renderD[0-9]*' \) -print -quit 2>/dev/null)" ] &&
            echo "${p#/proc/}"
    done | sort -n
}
uuids() { nvidia-smi --query-gpu=uuid --format=csv,noheader 2>/dev/null; }
state() { "$CC" --get-state --pid "$1" 2>/dev/null || echo "?"; }

[ "$(id -u)" -eq 0 ] || die "run as root"
[ -x "$CC" ] || die "cuda-checkpoint not found (set CUDA_CHECKPOINT)"
mkdir -p "$STATE_DIR"

case "${1:-}" in
drain)
    [ -s "$STATE_DIR/pids" ] && die "a previous drain was not resumed ($STATE_DIR/pids); run resume first"
    # QEMU refuses to migrate while a 9p export is mounted; say so before touching any process.
    ninep=$(awk '$3 == "9p" {printf "%s ", $2}' /proc/mounts)
    [ -z "$ninep" ] || die "9p shares mounted ($ninep); QEMU will not migrate the VM. Unmount them (or set NVKVM_MIGRATABLE=1, see docs/howto/live-migration.md)"
    # A share detached lazily at boot (see nvkvm-guest.service) lives on while a process still uses its files.
    if [ -s /run/nvkvm/9p-devs ]; then
        users=""
        for dev in $(sort -u /run/nvkvm/9p-devs); do
            # recorded as decimal major:minor (mountpoint -d); /proc/<pid>/maps prints hex
            hex=$(printf '%02x:%02x' "${dev%%:*}" "${dev#*:}")
            for p in /proc/[0-9]*; do
                awk -v d="$hex" '$4 == d {f = 1} END {exit !f}' "$p/maps" 2>/dev/null &&
                    users="$users ${p#/proc/}($(cat "$p/comm" 2>/dev/null))"
            done
        done
        [ -z "$users" ] || die "these processes still use libraries from the detached 9p share:$users. QEMU will not migrate the VM until they restart"
    fi
    uuids > "$STATE_DIR/uuids.before"
    [ -s "$STATE_DIR/uuids.before" ] || die "no GPU visible; nothing to drain"
    mapfile -t pids < <(holders)
    done=()
    fail=""
    for p in "${pids[@]}"; do
        if "$CC" --action lock --pid "$p" --timeout "$LOCK_TIMEOUT_MS" 2>/dev/null &&
           "$CC" --action checkpoint --pid "$p" 2>/dev/null; then
            done+=("$p")
        else
            "$CC" --action unlock --pid "$p" 2>/dev/null
            fail="cannot checkpoint pid $p ($(cat /proc/$p/comm 2>/dev/null))"
            break
        fi
    done
    if [ -z "$fail" ]; then
        # cuda-checkpoint reports success for some processes it cannot actually release (a non-CUDA
        # holder such as nvidia-smi), so the device files are the ground truth.
        left=$(holders | tr '\n' ' ')
        [ -z "$left" ] || fail="still holding /dev/nvidia*: $(for p in $left; do printf '%s(%s) ' "$p" "$(cat /proc/$p/comm 2>/dev/null)"; done)"
    fi
    if [ -n "$fail" ]; then
        for p in "${done[@]}"; do "$CC" --action restore --pid "$p" 2>/dev/null && "$CC" --action unlock --pid "$p" 2>/dev/null; done
        rm -f "$STATE_DIR/uuids.before"
        die "drain aborted: $fail. ${#done[@]} process(es) restored; do not migrate"
    fi
    printf '%s\n' "${done[@]}" > "$STATE_DIR/pids"
    echo "drained ${#done[@]} GPU process(es): ${done[*]:-none}. The GPU is released; the VM can be migrated."
    ;;
resume)
    [ -f "$STATE_DIR/uuids.before" ] || die "nothing to resume (no drain recorded in $STATE_DIR)"
    # the device may need a moment after the move before the host answers
    for _ in $(seq 1 30); do [ -n "$(uuids)" ] && break; sleep 1; done
    uuids > "$STATE_DIR/uuids.after"
    nb=$(wc -l < "$STATE_DIR/uuids.before"); na=$(wc -l < "$STATE_DIR/uuids.after")
    [ "$nb" -eq "$na" ] || die "GPU count changed ($nb before, $na now); processes stay checkpointed"
    map=$(paste -d= "$STATE_DIR/uuids.before" "$STATE_DIR/uuids.after" | paste -sd,)
    n=0; bad=""
    [ -f "$STATE_DIR/pids" ] && while read -r p; do
        [ -n "$p" ] || continue
        if [ ! -d "/proc/$p" ]; then
            echo "pid $p no longer exists (it was stopped while checkpointed); skipping"
            continue
        fi
        if "$CC" --action restore --pid "$p" --device-map "$map" && "$CC" --action unlock --pid "$p"; then
            n=$((n + 1))
        else
            bad="$bad $p"
        fi
    done < "$STATE_DIR/pids"
    [ -z "$bad" ] || die "restore failed for:$bad (left checkpointed; retry resume or kill them)"
    if cmp -s "$STATE_DIR/uuids.before" "$STATE_DIR/uuids.after"; then where="the same GPU(s)"; else where="new GPU(s): $map"; fi
    rm -f "$STATE_DIR/pids" "$STATE_DIR/uuids.before" "$STATE_DIR/uuids.after"
    echo "resumed $n GPU process(es) on $where"
    ;;
status)
    for p in $(holders); do echo "$p $(cat /proc/$p/comm 2>/dev/null) $(state "$p")"; done
    [ -s "$STATE_DIR/pids" ] && while read -r p; do echo "$p $(cat /proc/$p/comm 2>/dev/null) $(state "$p") (drained)"; done < "$STATE_DIR/pids"
    true
    ;;
*)
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
