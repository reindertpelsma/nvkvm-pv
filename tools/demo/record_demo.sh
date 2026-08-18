#!/usr/bin/env bash
# record_demo.sh — capture an nvkvm boot as a terminal recording.
#
# Runs ON THE NVKVM HOST.  Deliberately dependency-free: the only tool it
# needs is script(1) from util-linux, which is already on every host that
# can run the test VM.  Rendering (cast/GIF) happens off-box with
# tools/demo/termcast.py, so nothing extra has to be installed here.
#
# Usage:
#   tools/demo/record_demo.sh                  # record the default scenario
#   tools/demo/record_demo.sh -o /tmp/mydemo   # choose the output prefix
#   tools/demo/record_demo.sh -- my-command    # record something else
#
# Produces  <prefix>.typescript  and  <prefix>.timing , which convert with:
#   tools/demo/termcast.py gif <prefix> -o demo.gif
set -euo pipefail

PREFIX="${NVKVM_DEMO_OUT:-/tmp/nvkvm-demo}"
REPO_ROOT="$(realpath "$(dirname "$0")/../..")"
COLS="${NVKVM_DEMO_COLS:-100}"
ROWS="${NVKVM_DEMO_ROWS:-30}"

while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out) PREFIX="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

# ── The scenario ──────────────────────────────────────────────────────────
# Written to a file rather than passed inline so that `script -c` sees one
# tidy command and the recording is not polluted by the wrapper's own quoting.
SCENARIO="$(mktemp /tmp/nvkvm-demo-scenario.XXXXXX.sh)"
trap 'rm -f "$SCENARIO"' EXIT

if [ $# -gt 0 ]; then
    printf '%s\n' "$*" > "$SCENARIO"
else
    cat > "$SCENARIO" <<'SCEN'
set -u
# How to reach the guest.  Overridable because a stock test guest uses a
# cloud-init password while a hardened one uses a key; what is *printed* is
# always the tidy form, so neither ends up in the recording.
GUEST_SSH="${NVKVM_GUEST_SSH:-ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -o LogLevel=ERROR ubuntu@localhost}"

hp()    { printf '\033[1;32mhost \033[0m\033[1;36m$ %s\033[0m\n' "$*"; }
gp()    { printf '\033[1;35mguest\033[0m \033[1;36m$ %s\033[0m\n' "$*"; }
note()  { printf '\033[1;37m%s\033[0m\n' "$*"; }
pause() { sleep "${1:-0.6}"; }

# run <prompt-fn> <what to show> [what to actually run; defaults to shown]
run() {
    local where="$1" shown="$2"; shift 2
    local real="${1:-$shown}"
    "$where" "$shown"; pause 0.4; eval "$real"; echo; pause 0.5
}
# Same, for a command that runs inside the guest.
grun() { run gp "$1" "$GUEST_SSH '${2:-$1}'"; }

note 'nvkvm -- an NVIDIA GPU inside a KVM guest.'
note 'No VFIO, no passthrough: the host keeps using the card the whole time.'
echo; pause 1.2

run hp 'nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader'
run hp 'lspci -k -d 10de: | head -3'  'lspci -k -d 10de: 2>/dev/null | head -3'

# ── boot ──────────────────────────────────────────────────────────────────
hp 'scripts/run_test_vm.sh'
pause 0.4
: "${NVKVM_REPO:=$PWD}"
BOOTLOG=/tmp/nvkvm-demo-boot.log
rm -f "$BOOTLOG"; : > "$BOOTLOG"
setsid bash "$NVKVM_REPO/scripts/run_test_vm.sh" > "$BOOTLOG" 2>&1 < /dev/null &
VMPID=$!
tail -f -n +1 "$BOOTLOG" 2>/dev/null &
TAILER=$!

T0=$(date +%s)
for _ in $(seq 1 180); do
    $GUEST_SSH true 2>/dev/null && break
    sleep 1
done
BOOT_SECS=$(( $(date +%s) - T0 ))
sleep 0.7; kill $TAILER 2>/dev/null; wait $TAILER 2>/dev/null || true
printf '\n\033[1;32m>>> guest is up (%ss)\033[0m\n\n' "$BOOT_SECS"
pause 1.2

# ── guest bring-up ────────────────────────────────────────────────────────
note 'The guest has no GPU yet.  Build and load the paravirtual driver:'
echo; pause 0.8
grun 'sudo make -C /mnt/nvkvm/src/guest' \
     'cd /mnt/nvkvm/src/guest && sudo make KDIR=/lib/modules/$(uname -r)/build 2>&1 | tail -3'
grun 'sudo insmod nvkvm-guest.ko' \
     'cd /mnt/nvkvm/src/guest && sudo insmod ./nvkvm-guest.ko 2>&1 | head -2; lsmod | grep nvkvm'
grun 'ls /dev/nvidia*'
grun 'sudo scripts/stage_guest_libs.sh' \
     'sudo bash /mnt/nvkvm/scripts/stage_guest_libs.sh 2>&1 | tail -2'
pause 0.5
grun 'nvidia-smi'

# ── the point ─────────────────────────────────────────────────────────────
note 'Same physical GPU -- and the host never gave it up:'
echo; pause 0.8
run hp 'lspci -k -d 10de: | grep -m1 "Kernel driver in use"' \
    'lspci -k -d 10de: 2>/dev/null | grep -m1 "Kernel driver in use"'
run hp 'nvidia-smi --query-gpu=name,memory.used --format=csv,noheader'

# Leave the box as we found it.
kill -TERM -$VMPID 2>/dev/null || kill -TERM $VMPID 2>/dev/null || true
SCEN
fi

echo "recording -> $PREFIX.typescript / $PREFIX.timing  (${COLS}x${ROWS})"
rm -f "$PREFIX.typescript" "$PREFIX.timing"

# stty so the recorded geometry matches what we render later.
COLUMNS="$COLS" LINES="$ROWS" \
    script -q -c "stty cols $COLS rows $ROWS 2>/dev/null; NVKVM_REPO='$REPO_ROOT' bash '$SCENARIO'" \
           -T "$PREFIX.timing" "$PREFIX.typescript" || true

printf '%s %s\n' "$COLS" "$ROWS" > "$PREFIX.geom"
echo "done: $PREFIX.typescript ($(wc -c < "$PREFIX.typescript") bytes)"
