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
say()  { printf '\033[1;36m$ %s\033[0m\n' "$*"; }
run()  { say "$*"; eval "$*"; echo; }

printf '\033[1;37mnvkvm — paravirtual NVIDIA GPU for KVM guests\033[0m\n\n'

run 'nvidia-smi --query-gpu=name,driver_version --format=csv,noheader'

say 'scripts/run_test_vm.sh   # boot the guest'
# Boot the VM in the background, tee-ing the serial console into view, then
# stop following once the guest reports it is up.
: "${NVKVM_REPO:=$PWD}"
rm -f /tmp/nvkvm-demo-boot.log
setsid bash "$NVKVM_REPO/scripts/run_test_vm.sh" > /tmp/nvkvm-demo-boot.log 2>&1 < /dev/null &
QEMU_SH=$!
tail -f --pid=$$ /tmp/nvkvm-demo-boot.log 2>/dev/null &
TAILER=$!

GUEST_SSH="ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
           -o ConnectTimeout=4 -o LogLevel=ERROR ubuntu@localhost"
for _ in $(seq 1 150); do
    $GUEST_SSH true 2>/dev/null && break
    sleep 2
done
sleep 1; kill $TAILER 2>/dev/null; wait $TAILER 2>/dev/null || true
echo

printf '\033[1;32m--- guest is up -------------------------------------------------\033[0m\n\n'
run "$GUEST_SSH 'sudo modprobe nvkvm_guest 2>/dev/null; ls -l /dev/nvidia*'"
run "$GUEST_SSH 'nvidia-smi --query-gpu=name,driver_version --format=csv,noheader'"
run "$GUEST_SSH 'vulkaninfo --summary 2>/dev/null | grep -m2 deviceName'"

printf '\033[1;37mSame physical GPU. No VFIO, no passthrough, host keeps using it.\033[0m\n'
kill -TERM -$QEMU_SH 2>/dev/null || true
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
