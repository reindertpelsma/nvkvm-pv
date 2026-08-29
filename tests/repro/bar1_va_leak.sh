#!/usr/bin/env bash
# bar1_va_leak.sh — reproduce the BAR1 aperture VA leak, and MEASURE ITS RATE.
#
# The leak is not root-caused (docs/investigations/va-space-leak/FINDINGS.md).
# Until it is, the useful artifact is a reproducer that turns "the host GPU
# wedges eventually" into a number a fix can be measured against.
#
# ── WHAT TRIGGERS IT ────────────────────────────────────────────────────────
# CLIENT TEARDOWN, not rendering. That distinction is the whole test, and it is
# measured, not assumed (FINDINGS.md §3):
#
#   idle guest, 15 min .................. every counter flat
#   vkcube rendering continuously ....... every counter flat
#   ONE Vulkan client starting+exiting .. 59 leaked mappings
#
# So this runs a client that exits, N times, and counts. A test that rendered
# for an hour would show nothing and conclude, wrongly, that there is no leak.
#
# ── WHAT IT COUNTS, AND THE HONEST CAVEAT ───────────────────────────────────
# The host kernel names each mapping it fails to reclaim:
#
#   NVRM: clientUnmapResourceRefMappings: Failed to auto-unmap (status=0x23)
#         hClient c1d0008a: hResource: 47
#
# The documented figure is "59 failures per hClient", NOT per process
# (FINDINGS.md §6: 909 failures in regular runs of 59 across sequential client
# handles). So this counts BY hCLIENT — the recorded evidence has 200 failures
# across 7 clients, and a per-process number would not be comparable to 59.
#
# CAUSALITY IS UNVERIFIED, and FINDINGS.md says so: whether these failed
# auto-unmaps CAUSE the VA exhaustion or merely accompany it is not established.
# So this test does not rest on them alone. It reports the cheap proxy AND, when
# va_capacity is available, the direct thing the leak actually consumes —
# allocatable GPU VA. If the two ever disagree, that disagreement is itself the
# most interesting output of the run, and it is printed rather than hidden.
#
# ── HOW TO READ IT ──────────────────────────────────────────────────────────
# FAILURES_PER_CLIENT is the comparable number: 59 when characterised, 0 is the
# goal. VA_LOST_MB is the direct one and needs no interpretation.
#
# Usage:
#   tests/repro/bar1_va_leak.sh                  # 10 cycles, default guest
#   CYCLES=40 tests/repro/bar1_va_leak.sh        # longer run, better slope
#   GUEST_SSH='ssh -p 15022 root@127.0.0.1' tests/repro/bar1_va_leak.sh
#
# Exit: 0 no leak · 1 leak reproduced · 77 cannot run here (not a failure)
set -uo pipefail

CYCLES="${CYCLES:-10}"
THRESHOLD="${THRESHOLD:-1}"     # leaked mappings per teardown we tolerate
GUEST_SSH="${GUEST_SSH:-}"
CLIENT="${CLIENT:-vk_create_device}"

say() { printf '[bar1-leak] %s\n' "$*"; }
die_skip() { say "SKIP: $*"; exit 77; }

# ── preconditions ───────────────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ] || die_skip "needs root to read the kernel log"
command -v dmesg >/dev/null || die_skip "no dmesg"
[ -e /proc/driver/nvidia/version ] || die_skip "no NVIDIA driver on this host"
[ -n "$GUEST_SSH" ] || die_skip "set GUEST_SSH to reach the guest (this is a host+guest test)"
# shellcheck disable=SC2086  # GUEST_SSH is an operator-supplied command
$GUEST_SSH true 2>/dev/null || die_skip "guest not reachable via GUEST_SSH"

# The count must come from THIS run only. dmesg -C would destroy evidence
# someone else may need, so mark the log instead and count after the mark.
MARK="nvkvm-bar1-leak-probe-$$-$(date -u +%s)"
echo "$MARK" > /dev/kmsg 2>/dev/null || die_skip "cannot write /dev/kmsg to mark the log"

# total failures since the mark
failures_since_mark() {
    dmesg 2>/dev/null | awk -v m="$MARK" '
        index($0, m) { seen = 1; next }
        seen && /clientUnmapResourceRefMappings: Failed to auto-unmap/ { n++ }
        END { print n + 0 }'
}

# distinct RM client handles that leaked since the mark — the unit the 59 is in
clients_since_mark() {
    dmesg 2>/dev/null | awk -v m="$MARK" '
        index($0, m) { seen = 1; next }
        seen && match($0, /hClient [0-9a-f]+/) {
            c = substr($0, RSTART + 8, RLENGTH - 8); seen_c[c] = 1
        }
        END { n = 0; for (c in seen_c) n++; print n + 0 }'
}

# Direct measure: how much GPU VA can still be allocated. Declines as the leak
# grows, and unlike the dmesg lines it is not a proxy for anything.
# tools/va_capacity.c lives on the va-space-leak branch.
va_capacity_mb() {
    [ -x "${VA_CAPACITY:-}" ] || { echo ""; return; }
    "$VA_CAPACITY" 2>/dev/null | awk -F= '/TOTAL_MB/ { print $2 }' | tr -dc '0-9'
}

say "host driver : $(sed -n 's/.*Module *for *x86_64 *\([0-9.]*\).*/\1/p' /proc/driver/nvidia/version | head -1)"
say "cycles      : $CYCLES   client: $CLIENT   threshold: $THRESHOLD/teardown"

# ── does the guest have a client that EXITS? ────────────────────────────────
# shellcheck disable=SC2086
if ! $GUEST_SSH "command -v $CLIENT >/dev/null || [ -x /root/$CLIENT ]" 2>/dev/null; then
    die_skip "guest has no '$CLIENT'. Build tests/repro/vk_create_device.c in the guest:
             gcc -O0 -o /root/$CLIENT vk_create_device.c -lvulkan"
fi

base=$(failures_since_mark)
base_va=$(va_capacity_mb)
say "baseline failures after mark: $base"
if [ -n "$base_va" ]; then
    say "baseline allocatable GPU VA : ${base_va} MB"
else
    say "va_capacity not set (VA_CAPACITY=/path/to/va_capacity) — proxy only,"
    say "  and the proxy's causal link to the leak is UNVERIFIED. Build it from"
    say "  docs/investigations/va-space-leak/tools/va_capacity.c for the direct number."
fi

# ── churn ───────────────────────────────────────────────────────────────────
declare -a series=()
for i in $(seq 1 "$CYCLES"); do
    # shellcheck disable=SC2086
    $GUEST_SSH "command -v $CLIENT >/dev/null && $CLIENT || /root/$CLIENT" >/dev/null 2>&1
    sleep 1                       # let the host finish tearing the client down
    n=$(failures_since_mark)
    series+=("$n")
    printf '[bar1-leak]   cycle %2d/%s  leaked so far: %s\n' "$i" "$CYCLES" "$n"
done

total=$(failures_since_mark)
grown=$(( total - base ))
nclients=$(clients_since_mark)
per_client=0
[ "$nclients" -gt 0 ] && per_client=$(( grown / nclients ))
end_va=$(va_capacity_mb)

echo
say "===== result ====="
say "teardowns             : $CYCLES"
say "auto-unmap failures   : $grown"
say "leaking RM clients    : $nclients"
say "FAILURES_PER_CLIENT   : $per_client   (characterised at 59; goal is 0)"
if [ -n "$base_va" ] && [ -n "$end_va" ]; then
    lost=$(( base_va - end_va ))
    say "VA_LOST_MB            : $lost   (${base_va} -> ${end_va} MB allocatable)"
else
    lost=""
fi

# Monotonic growth is what distinguishes a leak from noise: a one-off burst
# could be startup, but a count that rises with every teardown cannot be.
rising=0
prev=$base
for v in "${series[@]}"; do [ "$v" -gt "$prev" ] && rising=$((rising+1)); prev=$v; done
say "cycles that grew   : $rising of $CYCLES"

# Report the two signals separately. If the proxy fires and VA does not move,
# that is evidence the auto-unmap failures are NOT the leak -- which is exactly
# the open question, and worth more than a green tick.
if [ -n "$lost" ]; then
    if [ "$grown" -gt 0 ] && [ "$lost" -le 0 ]; then
        say "NOTE: failures observed but allocatable VA did NOT fall."
        say "  That is evidence AGAINST the auto-unmap failures being the leak."
        say "  FINDINGS.md lists that causal link as unverified; record this run there."
    elif [ "$lost" -gt 0 ]; then
        say "NOTE: allocatable VA fell by ${lost} MB — the leak is real and direct,"
        say "  independent of what the dmesg lines mean."
    fi
fi

if [ "$per_client" -ge "$THRESHOLD" ]; then
    say "VERDICT: LEAK REPRODUCED — $per_client failures per RM client."
    say "  Host GPU VA is consumed and never returned; enough teardowns wedge the"
    say "  card for HOST and guest alike, recoverable only by reloading the driver."
    say "  nvidia-smi will show nothing: this is DMA VA, not VRAM."
    exit 1
fi

if [ "$rising" -eq 0 ] && [ "$grown" -eq 0 ]; then
    say "VERDICT: no leak observed over $CYCLES teardowns."
    exit 0
fi
say "VERDICT: below threshold but not zero ($grown failures over $CYCLES teardowns) — re-run with a larger CYCLES."
exit 0
