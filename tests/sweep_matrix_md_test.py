#!/usr/bin/env python3
"""Offline regressions for scripts/sweep_matrix_md.py.

No GPU, no network, no vast.ai: everything here runs against small checked-in
fixture JSONL files under tests/fixtures/sweep_matrix_md/. Run directly:

    python3 tests/sweep_matrix_md_test.py

Exits non-zero (via AssertionError) on the first failure.
"""
import importlib.util
import io
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "sweep_matrix_md.py"
FIXTURES = ROOT / "tests" / "fixtures" / "sweep_matrix_md"

SPEC = importlib.util.spec_from_file_location("sweep_matrix_md", SCRIPT)
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def build(*fixture_names: str) -> str:
    reader = M.Reader()
    for name in fixture_names:
        reader.read(str(FIXTURES / name))
    return M.build_report(reader.records, [str(FIXTURES / n) for n in fixture_names],
                           dict(reader.skipped)), reader


passed = 0


def check(label, cond):
    global passed
    assert cond, f"FAILED: {label}"
    passed += 1


# ---------------------------------------------------------------------------
# 1. all-pass run
# ---------------------------------------------------------------------------
report, _ = build("all_pass.jsonl")
check("all-pass: both driver rows verdict PASS",
      "| 515.65.01 | PASS |" in report and "| 580.95.05 | PASS |" in report)
# Note: UNTESTED can still legitimately appear here even in an "all pass"
# fixture -- the canonical driver matrix (scripts/sweep_matrix.py) has more
# turing-applicable versions than these two fixture rows cover, and that gap
# is exactly what this tool exists to surface. Only FAIL/INCOMPLETE/MIXED
# must be absent.
cells_section = report.split("### Cells")[1].split("### Raw")[0]
check("all-pass: cells summary has no FAIL/INCOMPLETE/MIXED count line",
      "| FAIL |" not in cells_section and "| INCOMPLETE |" not in cells_section
      and "MIXED" not in cells_section)

# ---------------------------------------------------------------------------
# 2. a run with a failure
# ---------------------------------------------------------------------------
report, _ = build("with_failure.jsonl")
check("with-failure: FAIL verdict present", "| 580.95.05 | FAIL |" in report)
check("with-failure: PASS verdict present too", "| 515.65.01 | PASS |" in report)
check("with-failure: box-level coverage-shortfall listed",
      "coverage-shortfall" in report.split("## Box-level failures")[1])
check("with-failure: failed_checks detail surfaced", "29P/1F/0S" in report)

# ---------------------------------------------------------------------------
# 3. a driver that produced no verdict at all (UNTESTED, reason carried)
# ---------------------------------------------------------------------------
report, _ = build("driver_no_verdict.jsonl")
check("no-verdict: driver-predates-gpu is UNTESTED, not blank",
      "| 575.51.03 | UNTESTED |" in report)
check("no-verdict: reason for driver-predates-gpu carried through",
      "driver-predates-gpu" in report.split("### blackwell")[1].split("##")[0])
check("no-verdict: driver-install-failed is UNTESTED with reason",
      "| 580.95.05 | UNTESTED |" in report and
      "driver-install-failed" in report.split("### blackwell")[1].split("##")[0])
check("no-verdict: no bare blank verdict cell (every '| <ver> |' row has a state token)",
      "|  |  |" not in report)

# ---------------------------------------------------------------------------
# 4. empty file
# ---------------------------------------------------------------------------
report, reader = build("empty.jsonl")
check("empty: zero skipped lines", sum(reader.skipped.values()) == 0)
check("empty: no crash, valid header present", report.startswith("# nvkvm sweep coverage matrix"))
check("empty: matrix section says nothing to show",
      "no per-driver records" in report)

# ---------------------------------------------------------------------------
# 5. malformed / truncated line must be skipped, not crash
# ---------------------------------------------------------------------------
report, reader = build("malformed.jsonl")
check("malformed: exactly 2 bad lines skipped (truncated json + non-json)",
      sum(reader.skipped.values()) == 2)
check("malformed: the well-formed records around the bad ones still parsed",
      "| 575.51.03 | PASS |" in report and "| 580.95.05 | INCOMPLETE |" in report)
check("malformed: skip count reported in the doc body",
      "2 line(s)" in report)

# ---------------------------------------------------------------------------
# 6. control rows and the (separate) SteamOS axis
# ---------------------------------------------------------------------------
report, _ = build("control_and_steamos.jsonl")
check("control row: control-pass classifies as PASS",
      "| control:575.51.03 | PASS |" in report)
check("control row status literal still shown in raw status table",
      "control-pass" in report.split("### Raw")[1].split("###")[0])
check("steamos kept out of the primary architecture x driver matrix",
      "steamos" not in report.split("## Architecture x driver matrix")[1].split("## Box-level")[0])
check("steamos section renders its own table",
      "steamos-fail" in report.split("## SteamOS")[1])
check("steamos-untested phase distinguished from a validate.sh verdict",
      "steamos-untested" in report.split("## SteamOS")[1])

# ---------------------------------------------------------------------------
# 7. disagreeing verdicts for the same (arch, driver) show as MIXED, not one silently winning
# ---------------------------------------------------------------------------
report, _ = build("mixed_verdicts.jsonl")
check("mixed verdicts surfaced as MIXED", "MIXED" in report)
check("mixed breakdown shows both FAIL and PASS counts",
      "FAIL x1" in report and "PASS x1" in report)

# ---------------------------------------------------------------------------
# 8. determinism: same input (any order/merge) -> byte-identical output
# ---------------------------------------------------------------------------
r1, _ = build("all_pass.jsonl", "with_failure.jsonl", "driver_no_verdict.jsonl")
r2, _ = build("driver_no_verdict.jsonl", "all_pass.jsonl", "with_failure.jsonl")
check("determinism: file order does not affect output", r1 == r2)

r3, _ = build("all_pass.jsonl", "with_failure.jsonl", "driver_no_verdict.jsonl")
check("determinism: repeated run is byte-identical", r1 == r3)

# ---------------------------------------------------------------------------
# 9. CLI smoke test: --help, and end-to-end subprocess invocation
# ---------------------------------------------------------------------------
help_out = subprocess.run([sys.executable, str(SCRIPT), "--help"],
                           capture_output=True, text=True, check=True)
check("--help exits 0 and documents usage", "SWEEP_JSONL" in help_out.stdout)

cli = subprocess.run(
    [sys.executable, str(SCRIPT),
     str(FIXTURES / "all_pass.jsonl"), str(FIXTURES / "with_failure.jsonl")],
    capture_output=True, text=True, check=True)
check("CLI stdout starts with the report header",
      cli.stdout.startswith("# nvkvm sweep coverage matrix"))

cli_report, _ = build("all_pass.jsonl", "with_failure.jsonl")
check("CLI output matches library call for the same inputs", cli.stdout == cli_report)

# no-args must fail cleanly (argparse), not crash with a traceback about something else
bad = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
check("no positional args -> nonzero exit", bad.returncode != 0)
check("no positional args -> usage message, not a traceback",
      "usage:" in bad.stderr.lower())

print(f"OK: {passed} checks passed")
