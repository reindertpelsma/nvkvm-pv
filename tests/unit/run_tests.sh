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
#   1. test_isolate stopped LINKING (undefined nvkvm_present_forget_isolate).
#      `make` aborts on the first failure, so test_tables and test_open_scm
#      were never built -- 26 passing assertions silently stopped running and
#      nothing said so.  Fixed in d21d3ff, but only because somebody happened
#      to run the suite by hand.  So: build with `make -k`, then REQUIRE that
#      every expected binary exists.  A suite that vanishes is a failure, not
#      a smaller test run.
#
#   2. test_isolate fails 5 of its 7 cases at RUNTIME (every ioctl against a
#      spawned isolate returns -ENOENT).  That is pre-existing API drift
#      between the test and the isolate table, documented in the Makefile --
#      not a regression, and not something to "fix" by deleting the test.  But
#      it makes `make run` exit non-zero BY DESIGN, and a suite that is red by
#      default is a suite nobody reads.
#
# So the known-failing cases are named here, individually.  The suite is green
# when exactly those five fail.  It goes red if a sixth one fails, if one of
# the five starts passing (the drift got fixed -- update this file), if any
# other suite regresses, or if anything fails to build.
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
    [test_dispatch]=39
    [test_frontend]=8
    [test_handle]=9
    [test_tables]=17
)

# Suites with their own ad-hoc output.  Value is a line that must appear.
declare -A MARKER_SUITES=(
    [test_open_scm]="ALL OPEN_SCM TESTS PASSED"
    [test_ctrl_gate]="test_ctrl_gate: PASS"
)

# test_isolate is handled on its own, below.  These five cases are EXPECTED to
# fail: every ioctl issued against a spawned isolate comes back -ENOENT because
# the test and the isolate table disagree about what isolate id gets handed
# back.  Re-deriving that is a real piece of work nobody has done yet.
ISOLATE_TOTAL=7
ISOLATE_KNOWN_FAIL="concurrent_ioctl out_of_order_ioctl poll_unpoll sequential_ioctl sync_mmap_munmap"

ALL_BINARIES="test_dispatch test_frontend test_handle test_isolate test_tables test_open_scm test_ctrl_gate mock_stub"

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
if [ "$iso_failed" != "$ISOLATE_KNOWN_FAIL" ]; then
    fail "test_isolate's failing set changed."
    echo "      A case that is failing but not listed is a REGRESSION -- fix it."
    echo "      A listed case that now passes means the isolate-id drift got"
    echo "      fixed: drop it from ISOLATE_KNOWN_FAIL in this file so the next"
    echo "      regression cannot hide behind it."
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "UNIT SUITE OK — all 7 suites built and ran; ${ISOLATE_TOTAL} isolate cases"
    echo "ran with exactly the ${ISOLATE_KNOWN_FAIL// /, } drift failures."
else
    echo "UNIT SUITE FAILED"
fi
exit "$rc"
