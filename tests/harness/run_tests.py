#!/usr/bin/env python3
"""tests/harness/run_tests.py -- tiered GPU test harness for nvkvm-pv.

    python3 tests/harness/run_tests.py --tier small
    python3 tests/harness/run_tests.py --tier medium
    python3 tests/harness/run_tests.py --tier large --install

That's still valid and behaves exactly as before: `--target` defaults to a
bare local machine, no `--baseline` means every check is single-sided
(degenerate) -- see result.py.

THE COMPARISON THAT ACTUALLY MATTERS -- host vs guest, same physical box,
same binary -- needs BOTH sides named explicitly:

    python3 tests/harness/run_tests.py --tier large \\
        --target vm --target-base ssh --target-base-host BOX --target-base-username USER \\
            --target-base-known-hosts none \\
        --baseline chroot --baseline-base ssh --baseline-base-host BOX --baseline-base-username USER \\
            --baseline-base-known-hosts none

(same `--target-base-*`/`--baseline-base-*` naming the SSH host, so
`VMMachine`/`ChrootMachine` both wrap the identical physical machine and
exec the byte-identical binary -- see tiers/realapps.py's docstring). Or,
against an already-reachable box directly (no VM/chroot composition):

    python3 tests/harness/run_tests.py --tier medium \\
        --target ssh --target-host BOX --target-username USER --target-known-hosts none \\
        --baseline this

See `--help` for the full `--target-*`/`--baseline-*` flag set (mirrors
`Machine`, `SSHMachine`, `ChrootMachine`, `VMMachine`'s own constructors --
machine_cli.py's module docstring has the rationale) and TIER CAPABILITIES
below for which tiers actually turn a baseline into a ratio.

No install step is required for `--tier small` (it shells out to the
dependency-free tests/validate.sh) or for the Python harness itself
(standard library only). `medium`/`large` additionally need a C compiler on
PATH to build the probes under tests/harness/probes/ -- see --install.
Preflight tool-checking (local `bash`/`cc` via `shutil.which`) only reflects
THIS PROCESS's own PATH, so it only gates when `--target` resolves to a
bare `this` machine (the default) -- an ssh/chroot/vm target's tool
availability is instead surfaced per-check as UNTESTED/SKIP by the tier
itself when it tries to compile/run there. Not extended to remote preflight
discovery in this slice -- a known gap, not papered over.

Tiers:
  small   bring-up correctness -- wraps tests/validate.sh verbatim. Its
          checks are device-identity/correctness, never ratio-worthy: a
          `--baseline` is accepted for CLI uniformity but this tier can
          NEVER turn it into a comparison -- see tiers/small.py. ALSO:
          `--target ssh`/`--target vm` do not currently work for this tier
          specifically (confirmed against real hardware) -- it reads
          validate.sh's JSON output back with a LOCAL file read, which is
          wrong for a machine with its own separate filesystem; see
          tiers/small.py's "KNOWN GAP" docstring note. medium/large have no
          such gap.
  medium  ~1 minute, cheap quantitative checks (GEMM, memory bandwidth
          triad). Comparison-capable: give `--baseline` and every check
          becomes a real ratio gated by its ToleranceBand.
  large   medium's full set (comparison-capable, as above), plus the
          control-path (launch/alloc RTT) legs (also comparison-capable,
          tracked as a tripwire rather than a parity gate), plus real
          application legs (ffmpeg CPU/NVENC -- tiers/realapps.py,
          comparison-capable). Still a stub for real "tens of minutes"
          application workloads -- see tiers/large.py's docstring.

Every check is reported as PASS / FAIL / SKIP / UNTESTED (never silenceable,
UNTESTED always moves the exit code -- see result.py). Where a comparison is
structurally impossible (an app missing on one side, no GPU on the
baseline), that is a SKIP with a reason, carried on whichever Observation
actually skipped -- never a silent single-sided pass and never a fabricated
ratio (result.py's Comparison.status: SKIP outranks a missing ratio, FAIL
and UNTESTED outrank SKIP). Exit code: 0 all clear, 1 at least one FAIL, 2 a
--target-*/--baseline-* flag combination could not be resolved into a
Machine (mirrors argparse's own exit code for a bad invocation), 3 at least
one UNTESTED and no FAIL.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

import machine_cli
import preflight
from result import Comparison, Observation, Report, Status
from tiers import large as tier_large
from tiers import medium as tier_medium
from tiers import small as tier_small

TIERS = {"small": tier_small.run, "medium": tier_medium.run, "large": tier_large.run}
DEFAULT_OUT_DIR = Path(__file__).resolve().parent / "results"

# Exit code for a --target-*/--baseline-* combination that could not be
# resolved into a Machine -- distinct from the tier-result codes (0/1/3)
# because it means no tier ran at all. Mirrors argparse's own convention
# for "this invocation itself was wrong".
EXIT_CONFIG_ERROR = 2


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
    machine_cli.add_machine_args(p, "target", help_noun="machine under test", default_kind="this")
    machine_cli.add_machine_args(p, "baseline", help_noun="comparison machine for a host-vs-guest ratio -- omit for a single-sided run", default_kind=None)
    return p.parse_args(argv)


def _preflight_report(tier: str, missing, *, label: str) -> Report:
    report = Report(tier=tier)
    for req in missing:
        detail = f"{req.name} not found (tried: {', '.join(req.candidates)}); rerun with --install, or install {req.apt_package or req.name} manually"
        obs = Observation(status=Status.SKIP, detail=detail, label=label)
        report.add(Comparison(name=f"preflight:{req.name}", tier=tier, target=obs))
    report.finish()
    return report


def _config_error_report(tier: str, message: str) -> Report:
    report = Report(tier=tier)
    obs = Observation(status=Status.UNTESTED, detail=message, label="run_tests.py")
    report.add(Comparison(name="machine_config", tier=tier, target=obs))
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
    try:
        target, target_label = machine_cli.build_machine(args, "target")
        baseline, baseline_label = machine_cli.build_machine(args, "baseline")
    except machine_cli.MachineConfigError as exc:
        report = _config_error_report(args.tier, str(exc))
        json_path, md_path = write_outputs(report, args)
        print(f"machine configuration error: {exc}")
        print(f"\n  json: {json_path}\n  report: {md_path}")
        return EXIT_CONFIG_ERROR

    # See the module docstring: local preflight (shutil.which on THIS
    # process) is only meaningful when --target is a bare `this` machine --
    # an ssh/chroot/vm target's tools live on a different machine entirely,
    # and the tier itself reports UNTESTED/SKIP if one is missing there.
    if args.target_kind == "this":
        pf = preflight.check(args.tier)
        if not pf.ok:
            still_missing = preflight.install_missing(pf.missing) if args.install else pf.missing
            if still_missing:
                report = _preflight_report(args.tier, still_missing, label=target_label)
                json_path, md_path = write_outputs(report, args)
                print_summary(report)
                print(f"\n  json: {json_path}\n  report: {md_path}")
                return report.exit_code()

    runner = TIERS[args.tier]
    report = await runner(
        target,
        timeout=args.timeout,
        baseline=baseline,
        target_label=target_label,
        baseline_label=baseline_label or "baseline",
    )
    json_path, md_path = write_outputs(report, args)
    print_summary(report)
    print(f"\n  json: {json_path}\n  report: {md_path}")
    return report.exit_code()


def main(argv: Optional[list] = None) -> int:
    args = parse_args(argv)
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
