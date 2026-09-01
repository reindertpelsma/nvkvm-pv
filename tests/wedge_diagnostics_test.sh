#!/usr/bin/env bash
#
# tests/wedge_diagnostics_test.sh -- exercise validate.sh's wedge-diagnosis
#                                    machinery without a GPU.
#
# WHY THIS EXISTS
#   tests/validate.sh needs a GPU and cannot run here. But the part that
#   MEASURED as broken on 2026-09-01 (a Quadro P4000 sweep ran 2573s and
#   produced no verdict, because neither PROBE_TIMEOUT nor SHELL_TIMEOUT ever
#   fired) is pure process-control logic: checkpoint(), _wait_bounded(),
#   run_probe(), sh_bounded(), note_probe_timeout(), note_shell_timeout() and
#   dump_wedge_evidence() none of them touch the GPU. They only touch pids,
#   signals and /proc. So: source the real validate.sh in LIBRARY MODE
#   (NVKVM_VALIDATE_LIB=1, same pattern tests/sweep_offline_test.sh uses for
#   scripts/sweep.sh) to get the real functions, byte for byte, and drive them
#   against fake "probes" that are ordinary shell scripts standing in for a
#   compiled probe binary.
#
# WHAT THIS PROVES, and how "unkillable" is simulated
#   A process that is genuinely stuck in an uninterruptible (D-state) kernel
#   sleep cannot be reproduced here on purpose -- that needs a wedged driver.
#   But the property that matters is behavioural, not physical: from
#   _wait_bounded's point of view, "unkillable" means every `kill -0 $pid`
#   after SIGKILL keeps reporting the process as alive, and the signals never
#   land. So case 3 below shadows the `kill` builtin with a shell function
#   that, for one specific fake pid, always reports "still alive" and drops
#   every signal -- while a REAL `sleep` process backs that pid so it can be
#   cleaned up afterwards with the unshadowed `command kill`. Everything
#   _wait_bounded observes (kill -0 never goes to plan) is indistinguishable
#   from the real D-state case; everything about how the fake pid is produced
#   is not.
#
#   Case 2 does not need a shadow at all: a plain `sleep` obeys SIGTERM
#   directly, which already exercises "bounded and reaped" -- a real process,
#   a real signal, a real death.
#
# Usage:  bash tests/wedge_diagnostics_test.sh
# Exit :  0 all passed, 1 otherwise.
#
set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ok()  { printf '  \033[32mPASS\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
bad() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
must_contain() {   # <name> <haystack> <needle>
    case "$2" in *"$3"*) ok "$1" ;; *) bad "$1 (wanted to find: $3)"; printf '  --- got ---\n%s\n  -----------\n' "$2" ;; esac
}
must_eq() {   # <name> <got> <want>
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi
}

# ---------------------------------------------------------------------------
# source the real validate.sh, functions only -- see tests/sweep_offline_test.sh
# for the same pattern against scripts/sweep.sh.
# ---------------------------------------------------------------------------
export NVKVM_VALIDATE_LIB=1
set --                       # validate.sh parses "$@"; give it nothing
# shellcheck disable=SC1091
. "$REPO/tests/validate.sh"

# validate.sh's own $WORK (from mktemp) is reused for the fake probe binaries.
EVID="$TMP/evidence.log"
: > "$EVID"
exec 9>"$EVID"               # capture what checkpoint()/dump_wedge_evidence()
                              # write on fd 9, instead of letting it go to
                              # this harness's real stdout.
reset_evid() { : > "$EVID"; }
evid() { cat "$EVID"; }

echo "=== 1. checkpoint() ==="
reset_evid
checkpoint "test:marker-one"
must_eq   "checkpoint updates LAST_CHECKPOINT" "$LAST_CHECKPOINT" "test:marker-one"
must_contain "checkpoint writes CHECKPOINT|...|label on fd 9" "$(evid)" "CHECKPOINT|"
must_contain "checkpoint line names the label" "$(evid)" "test:marker-one"

echo
echo "=== 2. run_probe / sh_bounded: normal completion, no wedge ==="
cat > "$WORK/fastprobe" <<'EOF'
#!/bin/sh
echo 'CHECK|fast_check|PASS|ran fine'
exit 0
EOF
chmod +x "$WORK/fastprobe"
reset_evid
run_probe fastprobe
rc=$?
must_eq "run_probe returns the probe's real exit code" "$rc" "0"
must_eq "run_probe: PROBE_TIMED_OUT stays empty" "$PROBE_TIMED_OUT" ""
must_eq "run_probe: PROBE_SURVIVED stays empty"  "$PROBE_SURVIVED"  ""
must_contain "run_probe's own output was captured to \$bin.out, not fd 9" "$(cat "$WORK/fastprobe.out")" "CHECK|fast_check|PASS"

case "$(evid)" in
    *WEDGE-EVIDENCE*) bad "no WEDGE-EVIDENCE on a normal run (found one anyway)" ;;
    *) ok "no WEDGE-EVIDENCE on a normal run" ;;
esac

reset_evid
sh_bounded ok_label 5 true
rc=$?
must_eq "sh_bounded: normal command returns rc 0" "$rc" "0"
must_eq "sh_bounded: SH_BOUNDED_TIMED_OUT stays empty" "$SH_BOUNDED_TIMED_OUT" ""
if note_shell_timeout would_not_be_used; then
    bad "note_shell_timeout must return false (1) when nothing timed out"
else
    ok "note_shell_timeout returns false when nothing timed out"
fi

echo
echo "=== 3. sh_bounded: exceeds budget, TERM alone reaps it (bounded and reaped) ==="
PROBE_TIMEOUT_SAVE="$PROBE_TIMEOUT"; PROBE_KILL_AFTER_SAVE="$PROBE_KILL_AFTER"
reset_evid
sh_bounded slow_but_termable 1 sleep 30
rc=$?
must_eq  "SH_BOUNDED_TIMED_OUT names the label"    "$SH_BOUNDED_TIMED_OUT" "slow_but_termable"
must_eq  "SH_BOUNDED_SURVIVED is empty (it died)"  "$SH_BOUNDED_SURVIVED"  ""
must_contain "rc reflects termination by signal (>=128)" "$( [ "$rc" -ge 128 ] && echo yes || echo no )" "yes"
must_contain "WEDGE-EVIDENCE was emitted"          "$(evid)" "WEDGE-EVIDENCE|step=shell:slow_but_termable"
must_contain "kill_result=reaped (TERM was enough)" "$(evid)" "kill_result=reaped"
must_contain "evidence names the last checkpoint"   "$(evid)" "last_checkpoint=shell:slow_but_termable"
must_contain "dmesg section present (even if empty/unavailable)" "$(evid)" "dmesg tail"

if note_shell_timeout reaped_check_a reaped_check_b; then
    ok "note_shell_timeout returns true (consumed) when a timeout occurred"
else
    bad "note_shell_timeout should have returned true after a timeout"
fi
must_eq "note_shell_timeout records UNTESTED for every name given" \
        "$(printf '%s\n' "${RESULT_STATUS[@]}" | tail -2 | tr '\n' ',')" "UNTESTED,UNTESTED,"
must_contain "reaped wording says 'bounded and reaped', not SURVIVED" \
        "${RESULT_DETAIL[-1]}" "bounded and reaped"
case "${RESULT_DETAIL[-1]}" in
    *SURVIVED*) bad "reaped case must not use SURVIVED wording" ;;
    *) ok "reaped case does not use SURVIVED wording" ;;
esac

echo
echo "=== 4. run_probe: ignores TERM, dies to KILL (still 'reaped', not 'survived') ==="
cat > "$WORK/stubborn" <<'EOF'
#!/bin/sh
trap '' TERM
sleep 30
EOF
chmod +x "$WORK/stubborn"
PROBE_TIMEOUT=1
PROBE_KILL_AFTER=1
reset_evid
run_probe stubborn
rc=$?
must_eq "PROBE_TIMED_OUT names the probe"        "$PROBE_TIMED_OUT" "stubborn"
must_eq "PROBE_SURVIVED is empty -- KILL worked" "$PROBE_SURVIVED"  ""
must_contain "evidence shows TERM was sent, then KILL" "$(evid)" "sending-TERM"
must_contain "evidence shows KILL escalation"           "$(evid)" "TERM-ignored,sending-KILL"
must_contain "kill_result=reaped"                       "$(evid)" "kill_result=reaped"

note_probe_timeout
must_contain "note_probe_timeout: reaped wording" "${RESULT_DETAIL[-1]}" "bounded and reaped"
case "${RESULT_DETAIL[-1]}" in
    *SURVIVED*) bad "reaped case (via KILL) must not say SURVIVED" ;;
    *) ok "reaped case (via KILL) does not say SURVIVED" ;;
esac
must_eq "run_probe: rc reflects a killed process (>=128)" "$( [ "$rc" -ge 128 ] && echo yes || echo no )" "yes"
PROBE_TIMEOUT="$PROBE_TIMEOUT_SAVE"; PROBE_KILL_AFTER="$PROBE_KILL_AFTER_SAVE"

echo
echo "=== 5. the D-state simulation: kill -0 keeps saying 'alive' forever ==="
# A real, ordinary, perfectly killable sleep -- backing the pid so cleanup at
# the end of this block is real. We never let _wait_bounded's kill(1) calls
# reach it; that is the whole point.
sleep 60 &
WEDGE_PID=$!

# Shadow `kill`: for WEDGE_PID specifically, -0/-TERM/-KILL all "succeed"
# without doing anything, which is exactly what an unkillable D-state process
# looks like from the caller's side (kill -0 never returns "gone", and TERM
# and KILL both land on nothing). Any other pid goes to the real kill.
kill() {
    local sig="" pid=""
    if [ $# -ge 2 ]; then sig="$1"; pid="$2"; else sig="-TERM"; pid="$1"; fi
    if [ "$pid" = "$WEDGE_PID" ]; then
        return 0
    fi
    command kill "$sig" "$pid" 2>/dev/null
}

PROBE_TIMEOUT_SAVE="$PROBE_TIMEOUT"; PROBE_KILL_AFTER_SAVE="$PROBE_KILL_AFTER"
PROBE_TIMEOUT=1
PROBE_KILL_AFTER=1
reset_evid
_wait_bounded "$WEDGE_PID" "$PROBE_TIMEOUT" "$PROBE_KILL_AFTER" "probe:wedged"
must_eq "_wait_bounded: _BOUNDED_TIMED_OUT=1" "$_BOUNDED_TIMED_OUT" "1"
must_eq "_wait_bounded: _BOUNDED_SURVIVED=1 (the D-state signature)" "$_BOUNDED_SURVIVED" "1"
must_eq "_wait_bounded: rc is the give-up code 124, not a real exit status" "$_BOUNDED_RC" "124"
must_contain "evidence: kill_result=survived" "$(evid)" "kill_result=survived"
must_contain "evidence: the SURVIVED marker line is present" "$(evid)" "SURVIVED SIGKILL"

# Drive it through run_probe's own wiring too (not just the shared helper),
# so the exact machine-readable wording note_probe_timeout emits is pinned.
PROBE_TIMED_OUT="wedged_probe_name"
PROBE_SURVIVED="wedged_probe_name"
note_probe_timeout
must_contain "note_probe_timeout: SURVIVED wording used" "${RESULT_DETAIL[-1]}" "SURVIVED the kill"
must_contain "note_probe_timeout: names it stronger than a bound+reap" \
    "${RESULT_DETAIL[-1]}" "not just bounded and reaped"

unset -f kill                      # stop shadowing before real cleanup
command kill -KILL "$WEDGE_PID" 2>/dev/null
wait "$WEDGE_PID" 2>/dev/null
PROBE_TIMEOUT="$PROBE_TIMEOUT_SAVE"; PROBE_KILL_AFTER="$PROBE_KILL_AFTER_SAVE"

echo
echo "=== 6. dump_wedge_evidence degrades gracefully (no crash) when D-state and /proc/*/stack are unavailable ==="
reset_evid
( dump_wedge_evidence "test:degrade" reaped 999999 )
degrade_rc=$?
must_eq "dump_wedge_evidence never errors out" "$degrade_rc" "0"
must_contain "ps section header is present" "$(evid)" "ps -eo pid,stat,wchan:32,comm"
must_contain "dmesg section header is present" "$(evid)" "dmesg tail"
must_contain "evidence block is terminated" "$(evid)" "WEDGE-EVIDENCE-END|step=test:degrade"

echo
printf '\n%d/%d checks passed\n' "$PASS" "$((PASS+FAIL))"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
