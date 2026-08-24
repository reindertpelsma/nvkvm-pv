#!/usr/bin/env bash
#
# tests/uvm_multiarch/battery.sh — the branch-validation battery for
# `uvm-fallback-guest-side`, run INSIDE the guest VM.
#
#   bash battery.sh baseline     # build+load the PARENT commit's guest module,
#                                # run validate.sh, record the baseline
#   bash battery.sh branch       # build+load THIS tree's guest module, run
#                                # validate.sh, then the whole managed-memory
#                                # battery
#
# Why a module swap rather than two boxes: the branch touches ONLY the guest
# kernel module (src/guest/) — src/qemu/ is byte-identical between 451b788 and
# its parent 252bd44 — so the honest A/B is one box, one QEMU, two modules.
# Two boxes would confound the comparison with everything that differs between
# two rentals.
#
# Everything prints machine-readable `@@KEY=value` lines alongside the human
# output, so the driver on the control machine never has to parse prose.
#
set -u -o pipefail

MODE="${1:-branch}"
REPO=/mnt/nvkvm
OUT="${2:-/tmp/uvm-battery-$MODE}"
mkdir -p "$OUT"

case "$MODE" in
    baseline) SRC="$REPO/baseline-base/src" ;;
    branch)   SRC="$REPO/src" ;;
    # `apps` re-runs ONLY section 9 against whatever module is already loaded.
    # The applications are the slow, network-dependent part and the one most
    # likely to need a second attempt for reasons that have nothing to do with
    # the branch; rebuilding and reloading the module to retry them would throw
    # away a good validate.sh run to fix a staging problem.
    apps)     SRC="$REPO/src" ;;
    *) echo "usage: battery.sh {baseline|branch|apps} [outdir]" >&2; exit 3 ;;
esac

say()  { printf '\n=== %s ===\n' "$*"; }
kv()   { printf '@@%s=%s\n' "$1" "$2"; }

# ---------------------------------------------------------------------------
# 0. environment fingerprint
# ---------------------------------------------------------------------------
say "environment ($MODE)"
kv MODE          "$MODE"
kv KERNEL        "$(uname -r)"
kv GPU           "$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
kv DRIVER        "$(cat /sys/module/nvidia/version 2>/dev/null || echo '?')"
kv LIBCUDA       "$(ls /usr/lib/x86_64-linux-gnu/libcuda.so.* 2>/dev/null | head -1)"
kv GUEST_RAM_MB  "$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)"

# ---------------------------------------------------------------------------
# 1. build and load the requested guest module
#
# Built in /tmp, not over the 9p share: 9p is slow, and building into the
# shared tree would leave the two variants' object files fighting over the same
# directory — which is how you end up loading a module you did not build.
# ---------------------------------------------------------------------------
if [ "$MODE" = apps ]; then
    say "apps mode: reusing the module already loaded, skipping straight to the applications"
    lsmod | grep -q nvkvm_guest || { kv MODLOAD NOT_PRESENT; exit 1; }
    if nm /tmp/modbuild-branch/src/guest/nvkvm-guest.ko 2>/dev/null | grep -q nvkvm_uvm_ext_mmap; then
        kv MODHAS_UVM_EXT yes
    fi
fi

if [ "$MODE" != apps ]; then
say "building the $MODE guest module"
BUILD=/tmp/modbuild-$MODE
rm -rf "$BUILD"; mkdir -p "$BUILD"
cp -r "$SRC" "$BUILD/src" || { kv MODBUILD FAIL_COPY; exit 1; }
if ! make -C "$BUILD/src/guest" KDIR="/lib/modules/$(uname -r)/build" \
        > "$OUT/modbuild.log" 2>&1; then
    kv MODBUILD FAIL
    tail -40 "$OUT/modbuild.log"
    exit 1
fi
kv MODBUILD OK
kv MODSIZE "$(stat -c %s "$BUILD/src/guest/nvkvm-guest.ko")"
# The one thing that distinguishes the two modules at the binary level.
if nm "$BUILD/src/guest/nvkvm-guest.ko" 2>/dev/null | grep -q nvkvm_uvm_ext_mmap; then
    kv MODHAS_UVM_EXT yes
else
    kv MODHAS_UVM_EXT no
fi

say "loading the $MODE guest module"
# Clear the ring buffer BEFORE the swap, never after.  validate.sh's
# `abi_profile` check reads the guest dmesg for the line the module prints at
# load; clearing after insmod wipes it and turns a PASS into
# "SKIP: no nvkvm abi/driver line in dmesg", which reads like a real 29/0/1
# instead of the 30/0/0 that actually happened.  Clearing first keeps section
# 10's scan scoped to this run and leaves the load messages intact.
sudo dmesg -C >/dev/null 2>&1 || true
sudo rmmod nvkvm_guest 2>/dev/null
sudo modprobe drm_shmem_helper 2>/dev/null
if ! sudo insmod "$BUILD/src/guest/nvkvm-guest.ko" 2>"$OUT/insmod.err"; then
    kv MODLOAD FAIL
    cat "$OUT/insmod.err"
    exit 1
fi
kv MODLOAD OK
lsmod | grep -q nvkvm_guest || { kv MODLOAD NOT_PRESENT; exit 1; }

# ---------------------------------------------------------------------------
# 2. validate.sh — the same suite both ways, so the delta is the module
# ---------------------------------------------------------------------------
say "validate.sh ($MODE)"
VJSON="$OUT/validate.json"
( cd "$REPO" && bash tests/validate.sh --json "$VJSON" ) > "$OUT/validate.log" 2>&1
VRC=$?
kv VALIDATE_RC "$VRC"
if [ -f "$VJSON" ]; then
    python3 - "$VJSON" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
print("@@VALIDATE_PASS=%s" % v.get("pass"))
print("@@VALIDATE_FAIL=%s" % v.get("fail"))
print("@@VALIDATE_SKIP=%s" % v.get("skip"))
for c in v.get("checks", []):
    if c.get("status") in ("FAIL", "SKIP"):
        print("@@VALIDATE_NONPASS=%s|%s|%s" % (c.get("name"), c.get("status"),
                                               (c.get("detail") or "")[:160]))
    # The two managed-memory checks' DETAIL text, on a pass as well.  One of
    # them is the thing to confirm rather than restate: cuda_managed_coherence
    # reports "CPU<->GPU migration cycles" for a backing that never migrates,
    # and the only way to say whether that wording is still there is to print it.
    if c.get("name") in ("cuda_managed_alloc", "cuda_managed_coherence"):
        print("@@VALIDATE_MANAGED=%s|%s|%s" % (c.get("name"), c.get("status"),
                                               (c.get("detail") or "")[:300]))
PY
else
    kv VALIDATE_PASS "?"; kv VALIDATE_FAIL "?"; kv VALIDATE_SKIP "?"
fi
grep -E '^(PASS|FAIL|SKIP)' "$OUT/validate.log" | tail -40 || true

if [ "$MODE" = baseline ]; then
    say "baseline done — the battery below only means anything with the branch module"
    exit 0
fi

# ===========================================================================
#                    from here on: the branch battery
# ===========================================================================
CC=cc
T="$REPO/tests"

build() {  # build <out> <src> [extra...]
    local o="$1" s="$2"; shift 2
    if $CC -O2 -o "$o" "$s" "$@" > "$OUT/$(basename "$o").build.log" 2>&1; then
        return 0
    fi
    echo "BUILD FAILED: $s"; tail -20 "$OUT/$(basename "$o").build.log"; return 1
}

# ---------------------------------------------------------------------------
# 3. the U-3/U-6 host-side gate must still refuse everything
#
# This is the security bar, not a functional one: the branch hands QEMU new
# UVM traffic (CREATE_EXTERNAL_RANGE / MAP_EXTERNAL_ALLOCATION) on a path that
# the gate arbitrates, so "the gate still accepts nothing" is the claim that
# has to survive on every architecture, not just the one it was written on.
# ---------------------------------------------------------------------------
say "u3_u6_gate_test"
if build /tmp/u3_u6_gate_test "$T/security/u3_u6_gate_test.c"; then
    /tmp/u3_u6_gate_test > "$OUT/gate.log" 2>&1; GRC=$?
    kv GATE_RC "$GRC"
    kv GATE_LINE "$(grep -a 'GATE_TEST' "$OUT/gate.log" | tail -1)"
    cat "$OUT/gate.log"
else
    kv GATE_RC BUILD_FAILED
fi

# ---------------------------------------------------------------------------
# 4. cuda_micro cases 5 (uvm_alloc) and 6 (uvm_migrate)
#
# Case 6 is reported for continuity, NOT as a migration measurement: under this
# fallback nothing migrates, so its number is PCIe access plus launch overhead.
# See docs/internal/known-limitations.md, "Two test names that no longer
# measure what they say".
# ---------------------------------------------------------------------------
say "cuda_micro (cases 5 and 6)"
if build /tmp/cuda_micro "$T/integration/cuda_micro.c" -ldl; then
    # rm_iters alloc_iters bw_mb launch_iters uvm_iters mig_iters
    /tmp/cuda_micro 200 200 64 200 40 40 > "$OUT/cuda_micro.log" 2>&1; CMRC=$?
    kv CUDA_MICRO_RC "$CMRC"
    cat "$OUT/cuda_micro.log"
    kv CUDA_MICRO_5 "$(grep -ai 'uvm_alloc' "$OUT/cuda_micro.log" | tail -1)"
    kv CUDA_MICRO_6 "$(grep -ai 'uvm_migrate' "$OUT/cuda_micro.log" | tail -1)"
else
    kv CUDA_MICRO_RC BUILD_FAILED
fi

# ---------------------------------------------------------------------------
# 5. the size ladder 4 MiB -> 1 GiB, plus clean oversubscription refusal
# ---------------------------------------------------------------------------
say "managed_ladder (size ladder + oversubscription)"
if build /tmp/managed_ladder "$T/integration/managed_ladder.c" -ldl; then
    /tmp/managed_ladder > "$OUT/ladder.log" 2>&1; LRC=$?
    kv LADDER_RC "$LRC"
    cat "$OUT/ladder.log"
else
    kv LADDER_RC BUILD_FAILED
fi

# ---------------------------------------------------------------------------
# 6. two CONCURRENT processes, each running the full ladder
#
# The cross-process VA collision this fallback replaced was a two-process bug,
# so one process passing proves nothing about it.
# ---------------------------------------------------------------------------
say "two concurrent ladders"
if [ -x /tmp/managed_ladder ]; then
    /tmp/managed_ladder > "$OUT/ladder-a.log" 2>&1 & PA=$!
    /tmp/managed_ladder > "$OUT/ladder-b.log" 2>&1 & PB=$!
    wait $PA; RA=$?
    wait $PB; RB=$?
    kv CONCURRENT_A_RC "$RA"
    kv CONCURRENT_B_RC "$RB"
    echo "--- A ---"; tail -6 "$OUT/ladder-a.log"
    echo "--- B ---"; tail -6 "$OUT/ladder-b.log"
else
    kv CONCURRENT_A_RC NO_BINARY; kv CONCURRENT_B_RC NO_BINARY
fi

# ---------------------------------------------------------------------------
# 7. leak check — the free path is host RAM, so a leak here kills the host
# ---------------------------------------------------------------------------
say "managed_leak (alloc/free churn)"
if build /tmp/managed_leak "$T/integration/managed_leak.c" -ldl; then
    /tmp/managed_leak 400 4 > "$OUT/leak.log" 2>&1; KRC=$?
    kv LEAK_RC "$KRC"
    tail -8 "$OUT/leak.log"
else
    kv LEAK_RC BUILD_FAILED
fi

# ---------------------------------------------------------------------------
# 8. NVKVM_UVM_VA_MAX headroom — what actually runs out first?
# ---------------------------------------------------------------------------
say "managed_headroom (NVKVM_UVM_VA_MAX consumption)"
if build /tmp/managed_headroom "$T/uvm_multiarch/managed_headroom.c" -ldl; then
    /tmp/managed_headroom 20000 64 > "$OUT/headroom.log" 2>&1; HRC=$?
    kv HEADROOM_RC "$HRC"
    kv HEADROOM_CEILING "$(grep -a '^CEILING' "$OUT/headroom.log" | tail -1)"
    kv HEADROOM_COSTMODEL "$(grep -a '^COST-MODEL' "$OUT/headroom.log" | tail -1)"
    kv HEADROOM_MAPS_PEAK "$(grep -a '^MAPS\[peak\]' "$OUT/headroom.log" | tail -1)"
    kv HEADROOM_CONTEXT "$(grep -a '^CONTEXT-AFTER-CEILING' "$OUT/headroom.log" | tail -1)"
    kv HEADROOM_RECOVERY "$(grep -a '^RECOVERY' "$OUT/headroom.log" | tail -1)"
    kv HEADROOM_RESULT "$(grep -a '^RESULT' "$OUT/headroom.log" | tail -1)"
    tail -12 "$OUT/headroom.log"

    # A SECOND SIZE.  If libcuda suballocates small managed requests out of one
    # UVM range, the per-allocation table cost at 64 KiB says nothing about the
    # cost at a size big enough to get a range of its own.  2 MiB x 2000 is
    # 4 GiB of pinned host memory, which is affordable, and the VMA ratio at
    # the two sizes together says which model is real.
    /tmp/managed_headroom 2000 2048 > "$OUT/headroom-2m.log" 2>&1; H2=$?
    kv HEADROOM2M_RC "$H2"
    kv HEADROOM2M_COSTMODEL "$(grep -a '^COST-MODEL' "$OUT/headroom-2m.log" | tail -1)"
    kv HEADROOM2M_CEILING "$(grep -a '^CEILING' "$OUT/headroom-2m.log" | tail -1)"
    kv HEADROOM2M_RESULT "$(grep -a '^RESULT' "$OUT/headroom-2m.log" | tail -1)"
    tail -6 "$OUT/headroom-2m.log"
else
    kv HEADROOM_RC BUILD_FAILED
fi
fi   # end of "everything except apps mode"

# ---------------------------------------------------------------------------
# 9. real applications that genuinely use managed memory
#
# Prebuilt on the BOX (which has a CUDA toolkit and a fast network) and dropped
# into the share by build_apps.sh; the guest only runs them.  PyTorch/TF/
# llama.cpp/vLLM are deliberately absent: none of them allocate managed memory,
# so running them would prove nothing about this branch.
# ---------------------------------------------------------------------------
APPS="$REPO/uvm-apps"
if [ -d "$APPS/bin" ]; then
    export LD_LIBRARY_PATH="$APPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    say "attach_verify (cudaStreamAttachMemAsync, numerically checked)"
    if [ -x "$APPS/bin/attach_verify" ]; then
        "$APPS/bin/attach_verify" > "$OUT/attach_verify.log" 2>&1; ARC=$?
        kv ATTACH_RC "$ARC"
        kv ATTACH_RESULT "$(grep -a '^RESULT' "$OUT/attach_verify.log" | tail -1)"
        cat "$OUT/attach_verify.log"
    else
        kv ATTACH_RC NO_BINARY
    fi

    say "conjugateGradientUM (cuBLAS + cuSPARSE on managed memory)"
    if [ -x "$APPS/bin/conjugateGradientUM" ]; then
        timeout 900 "$APPS/bin/conjugateGradientUM" > "$OUT/cgum.log" 2>&1; CRC=$?
        kv CGUM_RC "$CRC"
        kv CGUM_RESIDUAL "$(grep -aiE 'residual|Test Summary|error' "$OUT/cgum.log" | tail -2 | tr '\n' ' ')"
        cat "$OUT/cgum.log"
    else
        kv CGUM_RC NO_BINARY
    fi

    say "UnifiedMemoryStreams (completes; proves nothing on its own — see attach_verify)"
    if [ -x "$APPS/bin/UnifiedMemoryStreams" ]; then
        timeout 900 "$APPS/bin/UnifiedMemoryStreams" > "$OUT/ums.log" 2>&1; URC=$?
        kv UMS_RC "$URC"
        tail -6 "$OUT/ums.log"
    else
        kv UMS_RC NO_BINARY
    fi

    say "UnifiedMemoryPerf (expected CORRECT but very slow — non-migrating backing)"
    if [ -x "$APPS/bin/UnifiedMemoryPerf" ]; then
        # A BOUNDED WAIT, and the timeout is itself the result.  This sample
        # sweeps a large matrix of allocation sizes and access patterns, and on
        # a backing that never migrates every one of them crosses PCIe.  What
        # matters for this branch is the ORDER OF MAGNITUDE and that nothing
        # returns wrong answers — not the full sweep — so a bounded run that
        # reports how far it got is worth more than an unbounded one that keeps
        # a box rented.  Override with UMPERF_TIMEOUT.
        UMPT="${UMPERF_TIMEOUT:-1200}"
        S0="$(date +%s)"
        timeout "$UMPT" "$APPS/bin/UnifiedMemoryPerf" > "$OUT/umperf.log" 2>&1; PRC=$?
        kv UMPERF_RC "$PRC"
        kv UMPERF_SECONDS "$(( $(date +%s) - S0 ))"
        kv UMPERF_TIMEOUT_S "$UMPT"
        kv UMPERF_PROGRESS "$(wc -c < "$OUT/umperf.log") bytes of output"
        [ "$PRC" = 124 ] && kv UMPERF_VERDICT "did not finish within ${UMPT}s — the documented cost of a non-migrating backing, not a regression"
        tail -25 "$OUT/umperf.log"
    else
        kv UMPERF_RC NO_BINARY
    fi
else
    kv APPS NO_APPS_DIR
fi

# ---------------------------------------------------------------------------
# 10. anything the module or QEMU complained about, even on an all-green run
# ---------------------------------------------------------------------------
say "guest dmesg — nvkvm AUDIT/DENY/WARN lines"
sudo dmesg 2>/dev/null | grep -aiE 'nvkvm' | grep -aiE 'audit|deny|warn|error|fail' \
    | sort | uniq -c | sort -rn | head -30 > "$OUT/dmesg-warn.log" || true
cat "$OUT/dmesg-warn.log"
kv DMESG_WARN_LINES "$(wc -l < "$OUT/dmesg-warn.log")"

say "battery complete"
kv BATTERY_DONE 1
