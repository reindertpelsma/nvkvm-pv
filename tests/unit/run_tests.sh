#!/usr/bin/env bash
#
# tests/unit/run_tests.sh — build every unit suite, run every unit suite, and
#                           check the result against what we KNOW it should be.
#
# WHY THIS EXISTS, and why it is not just `make run`.
#
# Two separate things went wrong on 2026-08-20/21, and a plain `make && make
# run` catches neither:
#
#   0. DEAD-1 (2026-08-24): test_dispatch and test_frontend used to be pinned
#      here at 39 and 8.  Both suites targeted src/qemu/nvkvm_dispatch.c and
#      src/qemu/nvkvm_frontend.c, which were unreachable end to end and have
#      been deleted; test_frontend went with them and test_dispatch became
#      test_objects at 20.  The 27 assertions that disappeared are named
#      individually in tests/unit/test_objects.c's DEAD-1 banner and in
#      src/qemu/virtio_nvgpu.c's, because "the count went down" is exactly the
#      shape of failure (1) below and must never be allowed to look routine.
#
#   1. test_isolate stopped LINKING (undefined nvkvm_present_forget_isolate).
#      `make` aborts on the first failure, so test_tables and test_open_scm
#      were never built -- 26 passing assertions silently stopped running and
#      nothing said so.  Fixed in d21d3ff, but only because somebody happened
#      to run the suite by hand.  So: build with `make -k`, then REQUIRE that
#      every expected binary exists.  A suite that vanishes is a failure, not
#      a smaller test run.
#
#   2. test_isolate used to fail 4 of its 9 cases at RUNTIME.  That was a real
#      production defect -- see the long note above ISOLATE_KNOWN_FAIL for the
#      four-deep stack of causes and how each was found -- not something to
#      "fix" by deleting the test.  All four are fixed as of 2026-08-21;
#      ISOLATE_KNOWN_FAIL is now EMPTY and must stay that way.  A case that
#      starts failing again is a regression, and this file is what says so.
#
# So the known-failing cases are named here, individually.  The suite is green
# when exactly the named set (currently: none) fails.  It goes red if any case
# fails, if any other suite regresses, or if anything fails to build.
#
# The assertion counts are pinned for the same reason as (1): a suite that
# quietly loses half its cases still exits 0.  If you add tests, bump the
# number here; that one-line diff is the point.
#
#   bash tests/unit/run_tests.sh        # or: make -C tests/unit check
#
set -u
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

# ── What we expect, per suite ──────────────────────────────────────────────
# Suites whose harness prints a trailing "<passed>/<run> tests passed" line.
# The number is the exact case count that must run AND pass.
declare -A TALLY_SUITES=(
    [test_objects]=20
    [test_handle]=11
    [test_tables]=17
    [test_nvkms_allowlist]=618
    [test_stub_ptr_sanitize]=17
    [test_kvm_slot]=12
    [test_stub_window]=27
    [test_drm_devinfo]=67
    [test_r1_type_dev]=33
    [test_transport_ready]=6
)

# Suites with their own ad-hoc output.  Value is a line that must appear.
declare -A MARKER_SUITES=(
    [test_open_scm]="ALL OPEN_SCM TESTS PASSED"
    [test_ctrl_gate]="test_ctrl_gate: PASS"
)

# test_isolate is handled on its own, below.  NO case is expected to fail any
# more, which is why ISOLATE_KNOWN_FAIL is empty -- but the list stays, because
# what it used to hold is the point.  The explanation that sat here for months
# -- "the test and the isolate table disagree about what isolate id gets handed
# back" -- was wrong, and a confident wrong explanation is what stopped anyone
# looking.  What was actually there, established by strace rather than by
# reading, was FOUR independent faults stacked on top of each other:
#
#   1. A PRODUCTION bug, fixed (see the isolate fd-0 commit).  open() hands
#      back the lowest free descriptor; with stdin closed that is fd 0, and the
#      child's next statement, dup2(sv[1], STDIN_FILENO), destroyed the image fd
#      before it could be parked at fd 3.  The child then fexecve()d the command
#      SOCKET and died EACCES.  This was never a test problem at all.
#
#   2. mock_stub was dynamically linked.  The isolate child pivot_root()s into
#      an empty tmpfs before exec, so the loader is gone and execveat() returns
#      ENOENT.  Fixed by building it -static (see the Makefile).
#
#   3. mock_stub predates ISOLATE_CMD_SETUP_RING (12), which QEMU issues at
#      isolate creation and then WAITS on.  The mock fell through to a silent
#      `default: break`, the wait timed out, and QEMU declared the entire
#      isolate dead -- so every subsequent ioctl returned -ENOENT and looked
#      like an id mismatch.  Fixed by answering RESP_ERROR/ENOSYS, which
#      satisfies the wait without killing the isolate.
#
#   4. A SECOND production bug, and the one that kept the last four cases red:
#      nvkvm_isolate_table_init() memset the table to zero and then hand-set
#      only sock_fd and present_fd to -1.  sync_open_fd was left at 0, and
#      alloc_isolate_slot() retires a stale descriptor with the usual
#      `if (fd >= 0) close(fd)` -- so claiming a slot for the FIRST time closed
#      fd 0.  In this file's own create path that is immediately fatal:
#      socketpair() runs BEFORE the slot is claimed, so once fd 0 is free the
#      next isolate's command socket IS fd 0 and claiming the slot closes it.
#      Measured: sendmsg -> ENOTSOCK on ring setup, then -ENOENT for every
#      later call.  Fixed by initialising every descriptor field to -1.
#
# So the "isolate reports itself not-alive, and its stub sees EOF right after
# exec" symptom had a mundane cause: QEMU had closed its own end of the socket,
# by hand, one instruction before it needed it.
ISOLATE_TOTAL=9
ISOLATE_KNOWN_FAIL=""

# Cases that legitimately differ BY ENVIRONMENT, and so cannot be pinned either
# way.  poll_unpoll passes on a normal host but fails on the GitHub runner: CI
# runs inside a container where the isolate cannot take the namespace rung it
# takes locally, so the spawn path -- and therefore whether the fd relay reaches
# a live stub -- is not the same.  Pinning it as failing breaks the local run;
# pinning it as passing breaks CI.  Listing it here means "either outcome is
# accepted, everything else must match exactly", which keeps the regression
# guard sharp for the cases that ARE deterministic.
#
# This is a hole, and it is deliberate rather than accidental.  Do not add to
# this list to silence a case you have not explained.
# Empty, and that is the goal state.  poll_unpoll lived here because it passed
# as root and failed in CI; the cause turned out to be a real defect -- the
# namespace probe only ran clone() and never checked the namespace was USABLE,
# so on a host that refuses mounts inside an unprivileged userns (Ubuntu 24.04+
# default) auto picked a rung every isolate then died in.  Fixed at the source,
# so the exemption is no longer needed.  The mechanism stays: if a case really
# is environment-dependent, name it here WITH the reason.
ISOLATE_ENV_DEPENDENT=""

ALL_BINARIES="test_objects test_handle test_isolate test_tables test_open_scm test_ctrl_gate test_nvkms_allowlist test_stub_ptr_sanitize test_kvm_slot test_stub_window test_drm_devinfo test_r1_type_dev test_transport_ready mock_stub"

rc=0
fail() { echo "  FAIL: $*"; rc=1; }

# ── 1. Build everything, and keep going after a failure ────────────────────
# -k is load-bearing: without it one broken link hides every suite behind it.
echo "=== building (make -k) ==="
if ! make -k all; then
    echo "  (make reported a failure; checking which binaries survived)"
fi

echo
echo "=== binaries ==="
missing=""
for b in $ALL_BINARIES; do
    if [ -x "./$b" ]; then
        printf '  %-16s built\n' "$b"
    else
        printf '  %-16s MISSING\n' "$b"
        missing="$missing $b"
    fi
done
if [ -n "$missing" ]; then
    fail "these suites did not build:$missing"
    echo
    echo "A suite that does not build is not a smaller test run, it is a hole."
    echo "See d21d3ff: a single undefined symbol took 26 assertions offline."
    exit 1
fi

# ── 2. Suites that must pass outright ──────────────────────────────────────
echo
echo "=== suites ==="
for suite in "${!TALLY_SUITES[@]}"; do
    want="${TALLY_SUITES[$suite]}"
    out="$(./"$suite" 2>&1)"; ec=$?
    tally="$(printf '%s\n' "$out" | grep -oE '[0-9]+/[0-9]+ tests passed' | tail -1)"
    printf '  %-16s exit=%d  %s\n' "$suite" "$ec" "${tally:-<no tally line>}"
    if [ "$ec" -ne 0 ]; then
        fail "$suite exited $ec (expected 0)"
        printf '%s\n' "$out" | grep -E 'FAIL|error' | head -10 | sed 's/^/      /'
    fi
    if [ "$tally" != "$want/$want tests passed" ]; then
        fail "$suite: expected '$want/$want tests passed', got '${tally:-nothing}'"
        echo "      If you added or removed cases, update TALLY_SUITES in this file."
    fi
done

for suite in "${!MARKER_SUITES[@]}"; do
    want="${MARKER_SUITES[$suite]}"
    out="$(./"$suite" 2>&1)"; ec=$?
    printf '  %-16s exit=%d\n' "$suite" "$ec"
    if [ "$ec" -ne 0 ]; then
        fail "$suite exited $ec (expected 0)"
        printf '%s\n' "$out" | tail -10 | sed 's/^/      /'
    fi
    if ! printf '%s\n' "$out" | grep -qF "$want"; then
        fail "$suite: expected output line '$want' was not printed"
    fi
done

# ── 3. test_isolate: exactly the known-failing set, no more, no less ────────
# The harness prints "[ RUN  ] <case>" before each case and an indented
# "  FAIL <file>:<line>: ..." for each failed assertion.  It also prints
# "[ PASS ]" unconditionally afterwards -- which is a bug in the harness, and
# exactly why this parses RUN/FAIL pairs instead of trusting the PASS line.
echo
echo "=== test_isolate (known-failing set) ==="
iso_out="$(./test_isolate 2>&1)"
iso_ran="$(printf '%s\n' "$iso_out" | grep -cE '^\[ RUN  \]')"
iso_failed="$(printf '%s\n' "$iso_out" | awk '
    /^\[ RUN  \]/ { cur = $0; sub(/^\[ RUN  \][ \t]*/, "", cur); next }
    /^  FAIL/     { if (cur != "" && !(cur in seen)) { seen[cur] = 1; print cur } }
' | sort | tr '\n' ' ' | sed 's/ $//')"

printf '  cases run   : %s (expected %s)\n' "$iso_ran" "$ISOLATE_TOTAL"
printf '  failing now : %s\n' "${iso_failed:-<none>}"
printf '  known-failing: %s\n' "$ISOLATE_KNOWN_FAIL"

if [ "$iso_ran" -ne "$ISOLATE_TOTAL" ]; then
    fail "test_isolate ran $iso_ran cases, expected $ISOLATE_TOTAL"
fi
# Drop the environment-dependent cases from BOTH sides before comparing, so the
# comparison is over the deterministic cases only.
iso_cmp=""
for c in $iso_failed; do
    skip=0
    for e in $ISOLATE_ENV_DEPENDENT; do [ "$c" = "$e" ] && skip=1; done
    [ "$skip" -eq 0 ] && iso_cmp="$iso_cmp $c"
done
iso_cmp="$(printf '%s\n' $iso_cmp | sort | tr '\n' ' ' | sed 's/ $//')"

printf '  env-dependent: %s (either outcome accepted)\n' "$ISOLATE_ENV_DEPENDENT"
printf '  compared    : %s\n' "${iso_cmp:-<none>}"

if [ "$iso_cmp" != "$ISOLATE_KNOWN_FAIL" ]; then
    fail "test_isolate's failing set changed."
    echo "      ISOLATE_KNOWN_FAIL is empty on purpose: every deterministic"
    echo "      case passes.  A case failing here is a REGRESSION -- fix it."
    echo "      Do NOT re-add it to the list, and do NOT move it to"
    echo "      ISOLATE_ENV_DEPENDENT unless you can say what in the"
    echo "      environment changes the answer."
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "UNIT SUITE OK — all 9 suites built and ran; ${ISOLATE_TOTAL} isolate cases"
    # Counted, not hardcoded: the number said "8" for a while after a ninth
    # suite was added, which is exactly the kind of stale claim this file exists
    # to prevent.
    n_suites=$(( ${#TALLY_SUITES[@]} + ${#MARKER_SUITES[@]} ))
    echo "UNIT SUITE OK — all ${n_suites} suites built and ran; ${ISOLATE_TOTAL} isolate cases"
    if [ -n "$ISOLATE_KNOWN_FAIL" ]; then
        echo "ran with exactly the ${ISOLATE_KNOWN_FAIL// /, } known failures."
    else
        echo "ran with no known failures."
    fi
else
    echo "UNIT SUITE FAILED"
fi
exit "$rc"
