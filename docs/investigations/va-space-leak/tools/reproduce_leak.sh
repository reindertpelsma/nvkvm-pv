#!/usr/bin/env bash
# Reproduce the nvkvm BAR1 VA leak, WITH the controls that make it a finding
# rather than an anecdote.
#
# The leak: every fully-initialized guest Vulkan client that PRESENTS leaves
# ~59 BAR1 mappings that RM fails to auto-unmap at client teardown.  It is
# nvkvm-specific: the same work in a GPU container, or natively on the host,
# leaks zero.
#
# READ THIS BEFORE CHANGING THE METRIC.  nvidia-smi CANNOT see this leak --
# BAR1 "Used" stays flat while address space disappears.  Measuring it that way
# produces a confident, wrong "there is no leak".  The metric is the count of
# RM's own teardown failures in the kernel log; use journalctl, not dmesg,
# whose ring buffer rotates and under-counts.
#
# Usage:
#   ./reproduce_leak.sh                 # 5 guest cycles + both controls
#   CYCLES=10 ./reproduce_leak.sh       # more cycles
#   SKIP_CONTROLS=1 ./reproduce_leak.sh # guest only
#
# Config (override by env):
#   VMM_CONTAINER  compose vmm container name        (nvkvm-steamos-vmm-1)
#   GUEST_KEY      ssh key INSIDE that container     (/var/lib/nvkvm-steamos/ssh/id_ed25519)
#   GUEST_PORT     hostfwd port inside the container (15022)
#   GUEST_USER     guest user                        (deck)
set -u

CYCLES="${CYCLES:-5}"
VMM_CONTAINER="${VMM_CONTAINER:-nvkvm-steamos-vmm-1}"
GUEST_KEY="${GUEST_KEY:-/var/lib/nvkvm-steamos/ssh/id_ed25519}"
GUEST_PORT="${GUEST_PORT:-15022}"
GUEST_USER="${GUEST_USER:-deck}"
SKIP_CONTROLS="${SKIP_CONTROLS:-0}"

cnt() { journalctl -k -b 0 --no-pager 2>/dev/null | grep -c 'Failed to auto-unmap'; }
exhausted() { journalctl -k -b 0 --no-pager 2>/dev/null | grep -c "can't alloc VA space"; }

guest() {   # feed stdin to a shell in the guest
    docker exec -i "$VMM_CONTAINER" sh -c \
      "ssh -i $GUEST_KEY -p $GUEST_PORT -o StrictHostKeyChecking=no \
           -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o BatchMode=yes \
           $GUEST_USER@127.0.0.1 sh -s" 2>/dev/null
}

command -v journalctl >/dev/null || { echo "need journalctl"; exit 1; }
docker inspect "$VMM_CONTAINER" >/dev/null 2>&1 || {
    echo "vmm container '$VMM_CONTAINER' not found -- set VMM_CONTAINER"; exit 1; }
echo GUEST_PROBE | guest | grep -q GUEST_PROBE || {
    echo "cannot reach the guest through $VMM_CONTAINER -- check GUEST_KEY/GUEST_PORT"; exit 1; }

echo "=== nvkvm BAR1 VA leak reproducer ==="
echo "driver: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null)"
echo "gpu:    $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null)"
echo "BAR1:   $(nvidia-smi -q 2>/dev/null | awk '/BAR1 Memory Usage/{f=1} f&&/Total/{print $3, $4; exit}')"
echo "start:  auto-unmap=$(cnt)  can't-alloc-VA=$(exhausted)"
echo

# --- the leaking case: a PRESENTING Vulkan client in the guest ---------------
# vulkaninfo does NOT reproduce it (it creates a device and exits without a
# swapchain), so the client must actually render.
fail=0; total=0
for i in $(seq 1 "$CYCLES"); do
    before=$(cnt)
    guest <<'G' >/dev/null
export XDG_RUNTIME_DIR=/run/user/1000
S=$(ls /run/user/1000 2>/dev/null | grep -E '^gamescope-[0-9]+$' | head -1)
if [ -n "$S" ]; then WAYLAND_DISPLAY="$S" nohup vkcube >/tmp/vkcube.log 2>&1 &
else nohup vkcube >/tmp/vkcube.log 2>&1 & fi
G
    sleep 25
    guest <<'G' >/dev/null
pkill -9 -x vkcube 2>/dev/null || true
G
    sleep 8
    after=$(cnt); d=$((after-before)); total=$((total+d))
    printf 'guest vkcube cycle %-2s : %5s -> %-5s  DELTA=%s\n' "$i" "$before" "$after" "$d"
    [ "$d" -eq 0 ] && fail=$((fail+1))
done
echo "guest total: +$total over $CYCLES cycles"
echo

if [ "$SKIP_CONTROLS" != 1 ]; then
    echo "--- CONTROLS (these must be 0, or the metric is measuring something else) ---"
    if [ -x ./va_capacity ]; then
        b=$(cnt); ./va_capacity >/dev/null 2>&1; sleep 5
        echo "host-native CUDA client   : DELTA=$(( $(cnt) - b ))"
        IMG="$(docker images --format '{{.Repository}}:{{.Tag}}' | grep -v '<none>' | head -1)"
        if [ -n "$IMG" ]; then
            b=$(cnt)
            # --entrypoint matters: an image with its own entrypoint will refuse
            # to run the probe and the delta will be a meaningless 0.
            out=$(docker run --rm --gpus all --entrypoint /va_capacity \
                    -v "$PWD/va_capacity:/va_capacity:ro" "$IMG" 2>&1 | tail -1)
            sleep 5
            case "$out" in
              MAXCONTIG_MB=*) echo "GPU container CUDA client : DELTA=$(( $(cnt) - b ))" ;;
              *) echo "GPU container             : SKIPPED (probe did not run: $out)" ;;
            esac
        fi
    else
        echo "(build tools/va_capacity.c first:  gcc -O0 -o va_capacity va_capacity.c -lcuda)"
    fi
    echo
fi

echo "=== verdict ==="
echo "end: auto-unmap=$(cnt)  can't-alloc-VA=$(exhausted)"
if [ -x ./va_capacity ]; then
    echo "VA capacity: $(./va_capacity 2>&1 | tail -1)"
    echo "  healthy when MAXCONTIG_MB ~= CUDA_FREE_MB (allocation is VRAM-bound)."
    echo "  a leak shows as MAXCONTIG falling BELOW CUDA_FREE while nvidia-smi stays flat."
fi
if [ "$fail" -eq "$CYCLES" ]; then
    echo "RESULT: no leak observed -- every cycle was 0."
else
    echo "RESULT: leak reproduced on $((CYCLES-fail))/$CYCLES guest cycles."
    echo "NOTE: 'can't alloc VA space' stays 0 until the aperture actually runs out."
    echo "      On a 16 GB (ReBAR) BAR1 that takes far longer than on 256 MiB, so a"
    echo "      0 there does NOT mean the leak is absent -- only that it has headroom."
fi
