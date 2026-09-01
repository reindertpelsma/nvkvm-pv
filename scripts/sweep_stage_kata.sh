#!/usr/bin/env bash
# sweep_stage_kata.sh -- the nvkvm-kata product stage, run ON A SWEEP BOX.
#
# WHY THIS STAGE EXISTS
#
# Same reasoning as sweep_stage_steamos.sh: the driver stage runs
# tests/validate.sh, which answers "does the ABI hold on this driver" and
# touches no product surface. Kata is the other real consumer of nvkvm -- a
# container runtime that boots a VM per container -- and it exercises paths
# validate.sh never reaches: the CDI spec split, the QEMU shim, containerd's
# runtime handler, the guest rootfs and kernel that kata builds rather than the
# cloud image run_test_vm.sh boots.
#
# The install script is the test. scripts/nvkvm-kata-install.sh runs eight
# stages and the last one is "verify -- running the real proof, not a file
# listing": a CUDA vector-add inside a container on the new runtime, checking
# its own arithmetic. Installed-but-broken is never reported as success, so a
# clean exit here is a real end-to-end verdict rather than a file inventory.
#
# NVIDIA's container toolkit is the one dependency the installer deliberately
# does NOT install (its packaging is distro-specific); we install it here
# because a sweep box is always a known distro and the alternative is a stage
# that fails for a reason unrelated to nvkvm.
#
# Emits one JSON object per phase to verdicts.jsonl. The coordinator reads
# those and never parses prose.
# STATUS: written, NOT wired into sweep.sh. Deliberately.
#
# Wiring it needs a --kata flag, a run_kata_stage() and a verdict parser --
# sweep_parse_steamos.py has a fixed PHASES allowlist ("preflight", "clone",
# "image", "install", "provision", ...) that does not cover this stage's
# "toolkit", "vecadd" or "uninstall", and that allowlist is a security control:
# a hostile box controls every byte of verdicts.jsonl, so phases and statuses
# are checked against fixed lists and details truncated.
#
# It is unwired because adding untested harness code immediately before a sweep
# whose purpose is to come back clean is the wrong order. Finish this after the
# release: extend the parser (or add sweep_parse_kata.py) with the phases
# above, add the flag, and validate it on one cheap box before relying on it.
set -uo pipefail

WORK="${WORK:-/root/kata-stage}"
KATA_REF="${KATA_REF:-main}"
KATA_URL="${KATA_URL:-https://github.com/reindertpelsma/nvkvm-kata.git}"

mkdir -p "$WORK"
LOG="$WORK/stage.log"
VERDICTS="$WORK/verdicts.jsonl"
: >"$VERDICTS"
exec > >(tee -a "$LOG") 2>&1

say() { printf '[kata %s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
verdict() { # verdict <phase> <status> <detail>
    python3 - "$1" "$2" "${3:-}" >>"$VERDICTS" <<'PY'
import json,sys,time
print(json.dumps({"phase":sys.argv[1],"status":sys.argv[2],
                  "detail":sys.argv[3][:400],"t":int(time.time())}))
PY
    say "VERDICT $1 = $2 ${3:-}"
}
die_stage() { verdict "$1" fail "${2:-}"; say "STAGE FAILED at $1"; exit 1; }

say "nvkvm-kata stage starting (ref=$KATA_REF)"

# ---- 1. what are we actually running on -------------------------------------
DRV="$(cat /proc/driver/nvidia/version 2>/dev/null | head -1)"
GPU="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
say "gpu=${GPU:-unknown}"
say "drv=${DRV:-unknown}"
[ -n "$GPU" ] || die_stage "preflight" "no GPU visible to nvidia-smi on the host"
[ -e /dev/kvm ] || die_stage "preflight" "no /dev/kvm -- kata cannot boot a VM per container"
verdict "preflight" ok "gpu=${GPU} drv=${DRV%% *}"

# ---- 2. the container toolkit (the installer's one declared prerequisite) ----
if ! command -v nvidia-ctk >/dev/null 2>&1; then
    say "installing nvidia-container-toolkit (the one dep the installer will not do)"
    curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
        | gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg 2>/dev/null
    curl -fsSL https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
        | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
        > /etc/apt/sources.list.d/nvidia-container-toolkit.list
    DEBIAN_FRONTEND=noninteractive apt-get update -q  >/dev/null 2>&1
    DEBIAN_FRONTEND=noninteractive apt-get install -y nvidia-container-toolkit >/dev/null 2>&1
fi
command -v nvidia-ctk >/dev/null 2>&1 \
    || die_stage "toolkit" "nvidia-ctk still absent after install attempt"
verdict "toolkit" ok "$(nvidia-ctk --version 2>/dev/null | head -1)"

# ---- 3. source ---------------------------------------------------------------
say "cloning nvkvm-kata ($KATA_REF)"
rm -rf "$WORK/nvkvm-kata"
git clone --depth 1 --branch "$KATA_REF" "$KATA_URL" "$WORK/nvkvm-kata" >/dev/null 2>&1 \
    || die_stage "clone" "git clone failed (public repo, no credentials needed)"
verdict "clone" ok "$(git -C "$WORK/nvkvm-kata" rev-parse --short HEAD)"

# ---- 4. install; stage 8 of the installer IS the proof ------------------------
say "running nvkvm-kata-install.sh --install-kata --install-deps (this is the test)"
t0=$(date +%s)
if timeout 3600 bash "$WORK/nvkvm-kata/scripts/nvkvm-kata-install.sh" \
        --install-kata --install-deps >"$WORK/install.log" 2>&1; then
    t1=$(date +%s)
    # Stage 8 runs a CUDA vector-add in a container and checks its arithmetic.
    # Quote its own line rather than asserting success on exit code alone.
    proof="$(grep -aiE 'PASS|vecadd' "$WORK/install.log" | tail -1 | tr -dc '[:print:]' | cut -c1-160)"
    verdict "install" ok "$(( t1 - t0 ))s"
    if [ -n "$proof" ]; then
        verdict "vecadd" ok "$proof"
    else
        # Exit 0 with no visible proof line is not a pass we are willing to claim.
        verdict "vecadd" unknown "installer exited 0 but printed no proof line"
    fi
else
    rc=$?
    t1=$(date +%s)
    last="$(tail -5 "$WORK/install.log" 2>/dev/null | tr -dc '[:print:]\n' | tail -1 | cut -c1-200)"
    verdict "install" fail "rc=$rc after $(( t1 - t0 ))s: $last"
    say "STAGE FAILED at install"
    echo "done" >"$WORK/DONE"
    exit 1
fi

# ---- 5. leave the host as we found it ----------------------------------------
# The box is destroyed by the sweep's auto-destroy timer, so uninstalling is not
# about hygiene -- it is the only test of the uninstall path we get for free.
if [ -x "$WORK/nvkvm-kata/scripts/nvkvm-kata-uninstall.sh" ]; then
    if timeout 900 bash "$WORK/nvkvm-kata/scripts/nvkvm-kata-uninstall.sh" \
            >"$WORK/uninstall.log" 2>&1; then
        verdict "uninstall" ok "host restored from the install manifest"
    else
        verdict "uninstall" fail "$(tail -1 "$WORK/uninstall.log" 2>/dev/null | cut -c1-200)"
    fi
fi

say "nvkvm-kata stage complete"
echo "done" >"$WORK/DONE"
