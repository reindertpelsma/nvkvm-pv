#!/usr/bin/env python3
"""sweep_matrix_md.py -- turn sweep.jsonl results into a publishable Markdown matrix.

WHY THIS EXISTS
    "We tested nvkvm on architecture X" is a claim.  A table generated
    straight from the sweep's own newline-delimited JSON output, with every
    cell that was never exercised showing up as UNTESTED (and why) rather
    than as a blank, is the artifact that makes the claim checkable.

    A blank cell and an untested cell render identically in Markdown.  That
    is exactly the failure this script exists to prevent: nothing here is
    ever left blank.  Every cell is one of PASS, FAIL, INCOMPLETE, MIXED, or
    UNTESTED -- and UNTESTED always carries the reason forward from the
    record's own `status` field (or, for an applicable driver with no
    matching record at all, "no verdict recorded").

INPUT
    One or more `sweep.jsonl` files written by `scripts/sweep.sh` (see its
    `jrec`/`emit` helpers).  Each line is one JSON object; malformed or
    truncated lines (the sweep can be killed mid-write) are skipped, not
    fatal.  Multiple files are merged.

STATUS VALUES
    `scripts/sweep.sh` writes a `status` field that is one of:

      - a real validate.sh verdict: pass, fail, incomplete
      - that verdict prefixed with "control-" for the free preinstalled-driver
        control run (e.g. control-pass, control-guest-no-boot)
      - "validate-rc-<N>" for a validate.sh exit code that is not 0/1/2
      - a fixed set of harness/environment statuses that are NOT a
        validate.sh verdict at all: no-ssh, host-not-capable, build-failed,
        no-offer, spend-cap, create-failed, box-never-provisioned, not-a-vm,
        kernel-switch-failed, apt-quiesce-failed, ship-failed, guest-failed,
        guest-image-cache-failed, provision-failed, box-kept-for-inspection,
        no-applicable-drivers, no-driver-present, coverage-shortfall,
        driver-install-failed, driver-wrong-module-flavour,
        driver-predates-gpu, bundle-failed, host-apt-busy, sshpass-missing,
        vm-journal-scope-missing, guest-no-boot, guest-image-mismatch,
        guest-module-not-loaded, stage-failed, validate-unparsed
      - for the (separate, orthogonal) SteamOS OTA stage: steamos-no-verdicts,
        steamos-terminal-recheck, steamos-pass, steamos-fail, steamos-untested

    Anything other than pass/fail/incomplete (with or without a "control-"
    prefix) is UNTESTED in the matrix, and its raw status string is carried
    through as the reason.

USAGE
    scripts/sweep_matrix_md.py sweep-runs/*/sweep.jsonl > matrix.md
    scripts/sweep_matrix_md.py a.jsonl b.jsonl -o matrix.md
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import os
import sys
from typing import Any, Iterable

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# canonical driver/architecture matrix -- best-effort, imported rather than
# restated (scripts/sweep.sh does the same thing via load_matrix()).  This is
# ONLY used to say what a run did NOT cover; the matrix itself is built
# entirely from the records actually supplied.
# ---------------------------------------------------------------------------


def load_canonical_matrix() -> tuple[list[tuple[str, str, str]], dict[str, int]]:
    """Return (DRIVER_MATRIX, ARCH_FLOOR) from scripts/sweep_matrix.py, or
    ([], {}) if it cannot be loaded for any reason.  Never raises: this is a
    "nice to have" for the coverage narrative, not a hard dependency."""
    path = os.path.join(SCRIPT_DIR, "sweep_matrix.py")
    try:
        spec = importlib.util.spec_from_file_location("_sweep_matrix_ref", path)
        if spec is None or spec.loader is None:
            return [], {}
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)  # guarded by `if __name__ == "__main__"`
        driver_matrix = list(getattr(mod, "DRIVER_MATRIX", []))
        arch_floor = dict(getattr(mod, "ARCH_FLOOR", {}))
        return driver_matrix, arch_floor
    except Exception:
        return [], {}


def driver_major(version: str) -> int | None:
    try:
        return int(version.split(".", 1)[0])
    except (ValueError, AttributeError, IndexError):
        return None


def canonical_drivers_for_arch(
    arch: str, driver_matrix: list[tuple[str, str, str]], arch_floor: dict[str, int]
) -> list[str] | None:
    """Drivers from the canonical matrix applicable to `arch` by ABI-profile
    floor, or None if `arch` is not in the canonical floor table at all."""
    if arch not in arch_floor:
        return None
    floor = arch_floor[arch]
    out = []
    for ver, _prof, _why in driver_matrix:
        maj = driver_major(ver)
        if maj is not None and maj >= floor:
            out.append(ver)
    return out


# ---------------------------------------------------------------------------
# reading + classifying records
# ---------------------------------------------------------------------------

REAL_VERDICTS = {"pass": "PASS", "fail": "FAIL", "incomplete": "INCOMPLETE"}


def classify(status: str) -> tuple[str, str | None]:
    """(verdict, reason). verdict is one of PASS/FAIL/INCOMPLETE/UNTESTED.
    reason is the raw status string, set only when verdict is UNTESTED."""
    core = status
    if core.startswith("control-"):
        core = core[len("control-") :]
    if core in REAL_VERDICTS:
        return REAL_VERDICTS[core], None
    return "UNTESTED", status


class Reader:
    """Reads and merges sweep.jsonl files, tolerant of blank/malformed lines."""

    def __init__(self) -> None:
        self.records: list[dict[str, Any]] = []
        # path -> number of non-blank lines that could not be parsed
        self.skipped: "collections.OrderedDict[str, int]" = collections.OrderedDict()

    def read(self, path: str) -> None:
        skipped = 0
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except (ValueError, TypeError):
                        skipped += 1
                        continue
                    if not isinstance(obj, dict):
                        skipped += 1
                        continue
                    obj = dict(obj)
                    obj["_source_file"] = path
                    self.records.append(obj)
        except OSError as exc:
            raise SystemExit(f"sweep_matrix_md.py: cannot read {path!r}: {exc}")
        self.skipped[path] = skipped


def rec_arch(r: dict[str, Any]) -> str:
    a = r.get("arch")
    return a if isinstance(a, str) and a else "(no arch)"


def rec_status(r: dict[str, Any]) -> str:
    s = r.get("status")
    return s if isinstance(s, str) and s else "(no status)"


def rec_driver(r: dict[str, Any]) -> str:
    d = r.get("driver")
    return d if isinstance(d, str) else ""


def rec_gpu(r: dict[str, Any]) -> str:
    g = r.get("gpu")
    return g if isinstance(g, str) and g else ""


def rec_detail_text(r: dict[str, Any]) -> str:
    """Short human text for a record: prefer summary, fall back to detail."""
    for key in ("summary", "detail"):
        v = r.get(key)
        if isinstance(v, str) and v.strip():
            text = " ".join(v.split())  # collapse newlines/whitespace
            return text[:117] + "..." if len(text) > 120 else text
    return ""


def md_escape(s: str) -> str:
    return s.replace("|", "\\|").replace("\n", " ")


# ---------------------------------------------------------------------------
# grouping
# ---------------------------------------------------------------------------


def partition(records: list[dict[str, Any]]):
    """Split records into (primary, box_level, steamos).

    primary   -- a real (arch, driver) verdict row: role driver-set/control,
                 or any other row that still carries a real driver value
                 (driver-install-failed, driver-predates-gpu, ...).
    box_level -- driver == "-"/absent: nothing driver-specific was reached.
    steamos   -- role == "steamos": the OTA stage, an orthogonal axis.
    """
    primary, box_level, steamos = [], [], []
    for r in records:
        role = r.get("role")
        driver = rec_driver(r)
        if role == "steamos":
            steamos.append(r)
        elif driver in ("", "-"):
            box_level.append(r)
        else:
            primary.append(r)
    return primary, box_level, steamos


def driver_sort_key(d: str):
    is_control = d.startswith("control:")
    base = d[len("control:") :] if is_control else d
    parts: list[tuple[int, Any]] = []
    for tok in base.replace("-", ".").split("."):
        if tok.isdigit():
            parts.append((0, int(tok)))
        else:
            parts.append((1, tok))
    return (is_control, parts, d)


def cell_for_group(recs: list[dict[str, Any]]) -> tuple[str, str, list[str]]:
    """recs: all records sharing one (arch, driver).
    Returns (verdict, detail_text, gpus)."""
    classified = [classify(rec_status(r)) for r in recs]
    verdict_counts = collections.Counter(v for v, _ in classified)
    gpus = sorted({rec_gpu(r) for r in recs if rec_gpu(r)})

    if len(verdict_counts) == 1:
        verdict = next(iter(verdict_counts))
        n = len(recs)
        if verdict == "UNTESTED":
            reasons = sorted({reason for _, reason in classified if reason})
            detail = "; ".join(reasons)
        else:
            texts = sorted({rec_detail_text(r) for r in recs if rec_detail_text(r)})
            detail = "; ".join(texts)
        if n > 1:
            verdict = f"{verdict} (x{n})"
        return verdict, detail, gpus

    # genuine disagreement across records for the same (arch, driver)
    breakdown = ", ".join(
        f"{v} x{c}" for v, c in sorted(verdict_counts.items())
    )
    return f"MIXED ({breakdown})", "; ".join(
        sorted({rec_detail_text(r) for r in recs if rec_detail_text(r)})
    ), gpus


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------


def render_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join(["---"] * len(headers)) + "|"]
    for row in rows:
        out.append("| " + " | ".join(md_escape(c) for c in row) + " |")
    return out


def build_report(records: list[dict[str, Any]], input_paths: list[str], skipped: dict) -> str:
    driver_matrix, arch_floor = load_canonical_matrix()
    have_canonical = bool(arch_floor)

    primary, box_level, steamos = partition(records)

    lines: list[str] = []
    lines.append("# nvkvm sweep coverage matrix")
    lines.append("")
    lines.append(
        "Generated by `scripts/sweep_matrix_md.py` from "
        f"{len(input_paths)} input file(s):"
    )
    for p in sorted(input_paths):
        lines.append(f"- `{p}`")
    lines.append("")

    total_skipped = sum(skipped.values())
    if total_skipped:
        lines.append(
            f"**{total_skipped} line(s)** across "
            f"{sum(1 for v in skipped.values() if v)} file(s) could not be "
            "parsed as JSON and were skipped (not fatal)."
        )
        for p in sorted(k for k, v in skipped.items() if v):
            lines.append(f"- `{p}`: {skipped[p]} skipped line(s)")
        lines.append("")

    lines.append("## Legend")
    lines.append("")
    lines.append("- **PASS** -- validate.sh returned rc 0 (28/28-style checks all passed).")
    lines.append("- **FAIL** -- validate.sh ran to completion and reported a failing check.")
    lines.append("- **INCOMPLETE** -- validate.sh completed but skipped one or more checks (rc 2); "
                 "not silently counted as a pass.")
    lines.append("- **MIXED (...)** -- more than one record exists for this (architecture, driver) "
                 "pair and they disagree; the breakdown is shown in place of a single verdict.")
    lines.append("- **UNTESTED -- \\<reason\\>** -- anything that is not a real validate.sh verdict: "
                 "a harness/environment failure (e.g. `driver-install-failed`, `guest-no-boot`), "
                 "a box that never produced a verdict, or (in the coverage rows) a driver in the "
                 "canonical matrix for which no record exists at all (\"no verdict recorded\").")
    lines.append("- **(xN)** suffix -- N records agreed on this verdict for the same "
                 "(architecture, driver) pair.")
    lines.append("")

    # ---------------- summary -------------------------------------------
    lines.append("## Coverage summary")
    lines.append("")

    # group primary records by (arch, driver)
    by_arch_driver: "collections.OrderedDict[tuple[str, str], list]" = collections.OrderedDict()
    for r in primary:
        key = (rec_arch(r), rec_driver(r))
        by_arch_driver.setdefault(key, []).append(r)

    arches_with_data = sorted({rec_arch(r) for r in records})
    arches_with_primary_data = sorted({a for a, _d in by_arch_driver})

    cell_verdict_counts: collections.Counter = collections.Counter()
    not_covered_drivers: "collections.OrderedDict[str, list[str]]" = collections.OrderedDict()

    for arch in arches_with_primary_data:
        seen_drivers = {d for a, d in by_arch_driver if a == arch}
        canon = canonical_drivers_for_arch(arch, driver_matrix, arch_floor)
        if canon is not None:
            missing = sorted(set(canon) - seen_drivers, key=driver_sort_key)
            if missing:
                not_covered_drivers[arch] = missing

    for (arch, driver), recs in by_arch_driver.items():
        verdict, _detail, _gpus = cell_for_group(recs)
        base_verdict = verdict.split(" ", 1)[0].split("(", 1)[0].strip()
        cell_verdict_counts[base_verdict] += 1

    total_missing_cells = sum(len(v) for v in not_covered_drivers.values())
    if total_missing_cells:
        cell_verdict_counts["UNTESTED"] += total_missing_cells

    lines.append("### Cells (architecture x driver pairs)")
    lines.append("")
    if cell_verdict_counts:
        state_rows = [[state, str(cell_verdict_counts[state])]
                      for state in sorted(cell_verdict_counts)]
        lines.extend(render_table(["state", "count"], state_rows))
    else:
        lines.append("(no architecture x driver cells in the supplied input)")
    lines.append("")

    lines.append("### Raw `status` values seen in the input")
    lines.append("")
    status_counts = collections.Counter(rec_status(r) for r in records)
    status_rows = [[s, str(status_counts[s])] for s in sorted(status_counts)]
    lines.extend(render_table(["status", "count"], status_rows))
    lines.append("")

    missing_archs = sorted(set(arch_floor) - set(arches_with_data)) if have_canonical else []

    lines.append("### What was NOT covered")
    lines.append("")
    if have_canonical:
        if missing_archs:
            lines.append(
                "- **Architectures with no rows at all** (present in "
                f"`scripts/sweep_matrix.py`'s `ARCH_FLOOR` but absent from every "
                f"supplied input file): {', '.join(missing_archs)}."
            )
        else:
            lines.append(
                "- Every architecture in `scripts/sweep_matrix.py`'s `ARCH_FLOOR` "
                "has at least one row in the supplied input."
            )
        if not_covered_drivers:
            lines.append(
                "- **Drivers in the canonical matrix that produced no verdict**, "
                "by architecture (floor-filtered from the full `DRIVER_MATRIX` -- "
                "note a given sweep run may have used `--preset boundary`, a "
                "smaller subset, so not all of these were necessarily attempted):"
            )
            for arch in sorted(not_covered_drivers):
                lines.append(f"  - {arch}: {', '.join(not_covered_drivers[arch])}")
        else:
            lines.append(
                "- No canonical-matrix driver gaps: every architecture that has "
                "data has a verdict (of some kind) for every driver in its "
                "floor-filtered canonical set."
            )
    else:
        lines.append(
            "- `scripts/sweep_matrix.py` could not be loaded, so coverage "
            "cannot be checked against the canonical architecture/driver "
            "matrix. The claims below are limited to what appears in the "
            "supplied input."
        )
    if arches_with_data:
        lines.append(
            "- Architectures present in the supplied input: "
            + ", ".join(arches_with_data) + "."
        )
    lines.append("")

    # ---------------- primary matrix --------------------------------------
    lines.append("## Architecture x driver matrix")
    lines.append("")
    if not by_arch_driver and not not_covered_drivers:
        lines.append("(no per-driver records in the supplied input)")
        lines.append("")
    else:
        for arch in arches_with_primary_data:
            lines.append(f"### {arch}")
            lines.append("")
            rows = []
            drivers_here = sorted({d for a, d in by_arch_driver if a == arch}, key=driver_sort_key)
            for driver in drivers_here:
                recs = by_arch_driver[(arch, driver)]
                verdict, detail, gpus = cell_for_group(recs)
                rows.append([driver, verdict, ", ".join(gpus), detail])
            for driver in not_covered_drivers.get(arch, []):
                rows.append([driver, "UNTESTED", "", "no verdict recorded"])
            rows.sort(key=lambda row: driver_sort_key(row[0]))
            lines.extend(render_table(["driver", "verdict", "gpu(s)", "detail"], rows))
            lines.append("")

    # ---------------- box-level failures -----------------------------------
    lines.append("## Box-level failures (no specific driver reached)")
    lines.append("")
    if not box_level:
        lines.append("(none)")
        lines.append("")
    else:
        grouped: "collections.OrderedDict[tuple[str, str], list]" = collections.OrderedDict()
        for r in box_level:
            grouped.setdefault((rec_arch(r), rec_status(r)), []).append(r)
        rows = []
        for (arch, status), recs in sorted(grouped.items()):
            texts = sorted({rec_detail_text(r) for r in recs if rec_detail_text(r)})
            rows.append([arch, status, str(len(recs)), "; ".join(texts)])
        lines.extend(render_table(["arch", "status", "count", "detail"], rows))
        lines.append("")

    # ---------------- steamos ------------------------------------------
    lines.append("## SteamOS OTA stage (separate axis, not a driver verdict)")
    lines.append("")
    if not steamos:
        lines.append("(none)")
        lines.append("")
    else:
        rows = []
        for r in steamos:
            arch = rec_arch(r)
            driver = rec_driver(r) or "-"
            status = rec_status(r)
            phase = r.get("phase") if isinstance(r.get("phase"), str) else ""
            detail = rec_detail_text(r)
            rows.append([arch, driver, phase, status, detail])
        rows.sort(key=lambda row: (row[0], driver_sort_key(row[1]), row[2], row[3]))
        lines.extend(render_table(["arch", "driver", "phase", "status", "detail"], rows))
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="sweep_matrix_md.py",
        description=(
            "Read one or more scripts/sweep.sh sweep.jsonl result files and "
            "emit a Markdown coverage matrix (architecture x driver), with "
            "every cell that was not a real validate.sh pass/fail/incomplete "
            "verdict shown as UNTESTED and its reason. Stdlib only."
        ),
        epilog=(
            "Example:\n"
            "  scripts/sweep_matrix_md.py sweep-runs/*/sweep.jsonl > matrix.md\n"
            "  scripts/sweep_matrix_md.py a.jsonl b.jsonl -o matrix.md\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "inputs", nargs="+", metavar="SWEEP_JSONL",
        help="one or more sweep.jsonl files to read and merge",
    )
    parser.add_argument(
        "-o", "--output", metavar="FILE",
        help="write the Markdown to FILE instead of stdout",
    )
    args = parser.parse_args(argv)

    reader = Reader()
    for path in args.inputs:
        reader.read(path)

    report = build_report(reader.records, list(args.inputs), dict(reader.skipped))

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(report)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
