#!/usr/bin/env python3
"""tests/harness/run_tests.py -- tiered GPU test harness for nvkvm-pv.

    python3 tests/harness/run_tests.py --tier small
    python3 tests/harness/run_tests.py --tier medium
    python3 tests/harness/run_tests.py --tier large --install

No install step is required for `--tier small` (it shells out to the
dependency-free tests/validate.sh) or for the Python harness itself
(standard library only). `medium`/`large` additionally need a C compiler on
PATH to build the probes under tests/harness/probes/ -- see --install.

Tiers:
  small   bring-up correctness -- wraps tests/validate.sh verbatim.
  medium  ~1 minute, cheap quantitative checks (GEMM, memory bandwidth triad).
  large   medium's full set, plus the control-path (launch/alloc RTT) legs.
          Still a stub for real "tens of minutes" application workloads --
          see tiers/large.py's docstring for exactly what is and isn't here.

Every check is reported as PASS / FAIL / SKIP / UNTESTED (never silenceable,
UNTESTED always moves the exit code -- see result.py). Exit code: 0 all
clear, 1 at least one FAIL, 3 at least one UNTESTED and no FAIL.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import preflight
from machine import ThisMachine
from result import Comparison, Observation, Report, Status
from tiers import large as tier_large
from tiers import medium as tier_medium
from tiers import small as tier_small

TIERS = {"small": tier_small.run, "medium": tier_medium.run, "large": tier_large.run}
DEFAULT_OUT_DIR = Path(__file__).resolve().parent / "results"


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="run_tests.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--tier", choices=sorted(TIERS), required=True, help="which tier to run")
    p.add_argument("--install", action="store_true", help="install missing preflight tools via apt-get (never runs without this flag)")
    p.add_argument("--timeout", type=float, default=None, help="override the tier's default overall timeout, in seconds")
    p.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="directory for the JSON/markdown reports (default: tests/harness/results/)")
    p.add_argument("--json", type=Path, default=None, help="machine-readable result path (default: <out-dir>/<tier>-<timestamp>.json)")
    p.add_argument("--report", type=Path, default=None, help="markdown report path (default: <out-dir>/<tier>-<timestamp>.md)")
    return p.parse_args(argv)


def _preflight_report(tier: str, missing) -> Report:
    report = Report(tier=tier)
    for req in missing:
        detail = f"{req.name} not found (tried: {', '.join(req.candidates)}); rerun with --install, or install {req.apt_package or req.name} manually"
        obs = Observation(status=Status.SKIP, detail=detail, label="this machine")
        report.add(Comparison(name=f"preflight:{req.name}", tier=tier, target=obs))
    report.finish()
    return report


def write_outputs(report: Report, args: argparse.Namespace) -> tuple:
    ts = time.strftime("%Y%m%d-%H%M%S", time.localtime(report.started_at))
    json_path = args.json or (args.out_dir / f"{args.tier}-{ts}.json")
    md_path = args.report or (args.out_dir / f"{args.tier}-{ts}.md")
    report.write_json(json_path)
    report.write_markdown(md_path)
    return json_path, md_path


def print_summary(report: Report) -> None:
    c = report.counts()
    print()
    print(f"== tier {report.tier}: VERDICT {report.verdict()} ==")
    print(f"   {len(report.results)} checks -- {c[Status.PASS.value]} PASS, {c[Status.FAIL.value]} FAIL, {c[Status.SKIP.value]} SKIP, {c[Status.UNTESTED.value]} UNTESTED")
    for comparison in report.results:
        print(f"  {comparison.status.value:9s} {comparison.name:30s} {comparison.detail()}")


async def main_async(args: argparse.Namespace) -> int:
    pf = preflight.check(args.tier)
    if not pf.ok:
        still_missing = preflight.install_missing(pf.missing) if args.install else pf.missing
        if still_missing:
            report = _preflight_report(args.tier, still_missing)
            json_path, md_path = write_outputs(report, args)
            print_summary(report)
            print(f"\n  json: {json_path}\n  report: {md_path}")
            return report.exit_code()

    machine = ThisMachine()
    runner = TIERS[args.tier]
    report = await runner(machine, timeout=args.timeout)
    json_path, md_path = write_outputs(report, args)
    print_summary(report)
    print(f"\n  json: {json_path}\n  report: {md_path}")
    return report.exit_code()


def main(argv: Optional[list] = None) -> int:
    args = parse_args(argv)
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
