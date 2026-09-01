"""Tier: large -- tens of minutes, real applications.

Large ALWAYS includes medium's full set first, so a large result is never
weaker than a medium one -- this module runs tiers/medium.py's `run()`
unmodified and folds its Comparisons in before adding anything of its own.

What's added beyond medium:
  1. The launch+sync RTT and alloc+free RTT legs of tests/integration/
     gpu_bench.c (its Phases B and C -- the same probe medium's GEMM check
     already builds and runs for Phase A). These are the two rows in
     tests/perf/run_parity.sh's reference table that are explicitly NOT
     parity-seeking -- a known, disclosed multi-hop-ioctl control-path tax,
     tracked with a regression tripwire rather than a pass/fail-at-parity gate:

         launch+sync RTT   tripwire <4x   (reference: ~1.85x baseline)
         alloc+free RTT    tripwire <50x  (reference: ~29x baseline, the
                                            standing optimization target --
                                            never at parity)

  2. tiers/realapps.py's real-application legs (ffmpeg CPU/NVENC encode,
     see that module's docstring).

ALL THREE of medium's checks, (1) above, and (2) now take the SAME
`baseline`/`target_label`/`baseline_label` shape: pass `baseline=` (a
Machine on the same physical box as `machine`, e.g. `ChrootMachine(base)`
alongside `machine=VMMachine(base)`) and every Comparison this tier
produces -- medium's GEMM/bandwidth included -- carries a real ratio,
attributed to the machine that produced each side, instead of a lone
number. `baseline=None` (the default) keeps the existing degenerate
single-sided behaviour for every leg. `run_tests.py`'s `--target`/
`--baseline` CLI flags are what actually construct the two Machines and
pass them down to here.

HONEST GAPS, not papered over:
  - This is a starting point, not a representative "tens of minutes"
    workload. A real large-tier workload (a training run, an
    inference-server soak, a multi-GPU NCCL job) needs `machine.need()`
    wired to real model/dataset Items and is future work.
  - It re-runs gpu_bench.c from scratch rather than reusing medium's already-
    captured output, because Comparisons/Reports don't currently plumb raw
    probe stdout through -- a known, small inefficiency (one extra compile +
    ~1s run per machine), not a correctness gap.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from machine import Machine
from result import Comparison, Observation, Report, Status, ToleranceBand
from tiers import medium as tier_medium
from tiers import realapps as tier_realapps

DEFAULT_TIMEOUT = 120.0

LAUNCH_RTT_BAND = ToleranceBand("lower_is_better", 4.0, "tests/perf/run_parity.sh reference: launch+sync RTT tripwire, ~1.85x baseline, known control-path tax")
ALLOC_RTT_BAND = ToleranceBand("lower_is_better", 50.0, "tests/perf/run_parity.sh reference: alloc+free RTT tripwire, ~29x baseline, the standing optimization target")


# (comparison_name, key, stdout prefix, value regex, band, unit)
_CONTROL_PATH_LEGS = (
    ("large:launch_rtt_us", "launch", "B launch RTT", r"([\d.]+)\s*us/launch", LAUNCH_RTT_BAND, "us/launch"),
    ("large:alloc_rtt_us", "alloc", "C alloc RTT", r"([\d.]+)\s*us/pair", ALLOC_RTT_BAND, "us/pair"),
)


async def _control_path_observations(machine: Machine, *, timeout: float, label: str) -> dict:
    """Runs gpu_bench.c ONCE on `machine`, returns per-leg Observations keyed
    "launch"/"alloc" (see _CONTROL_PATH_LEGS) -- the same one-run/many-legs
    shape as medium.py's `_bandwidth_observations`."""
    status, stdout, stderr, duration = await tier_medium._run_gpu_bench(machine, timeout=timeout)
    if status is not None:
        detail = stderr.strip()[-500:] or f"gpu_bench produced no output (status {status.value})"
        return {
            key: Observation(status=status, detail=detail, unit=unit, duration_s=duration, label=label)
            for _, key, _, _, _, unit in _CONTROL_PATH_LEGS
        }

    observations = {}
    for _, key, prefix, value_re, _, unit in _CONTROL_PATH_LEGS:
        m = re.search(rf"^{re.escape(prefix)}.*?{value_re}", stdout, re.MULTILINE)
        if not m:
            obs = Observation(
                status=Status.UNTESTED,
                detail=f"gpu_bench exited 0 but its {prefix!r} line was not parseable; stdout: {stdout.strip()[-300:]!r}",
                unit=unit, duration_s=duration, label=label,
            )
        else:
            value = float(m.group(1))
            obs = Observation(status=Status.PASS, detail=f"{value:.2f} {unit}", value=value, unit=unit, duration_s=duration, label=label)
        observations[key] = obs
    return observations


async def _control_path_comparisons(
    target: Machine,
    *,
    timeout: float,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> list:
    target_obs = await _control_path_observations(target, timeout=timeout, label=target_label)
    baseline_obs = await _control_path_observations(baseline, timeout=timeout, label=baseline_label) if baseline is not None else None
    return [
        Comparison(
            name=name,
            tier="large",
            target=target_obs[key],
            baseline=(baseline_obs[key] if baseline_obs is not None else None),
            band=band,
        )
        for name, key, _, _, band, _ in _CONTROL_PATH_LEGS
    ]


async def run(
    machine: Machine,
    *,
    timeout: Optional[float] = None,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> Report:
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT

    medium_report = await tier_medium.run(machine, timeout=bound, baseline=baseline, target_label=target_label, baseline_label=baseline_label)
    report = Report(tier="large", meta=dict(medium_report.meta))
    for comparison in medium_report.results:
        report.add(Comparison(name=comparison.name, tier="large", target=comparison.target, baseline=comparison.baseline, band=comparison.band))

    for comparison in await _control_path_comparisons(machine, timeout=bound, baseline=baseline, target_label=target_label, baseline_label=baseline_label):
        report.add(comparison)

    realapps_report = await tier_realapps.run(
        machine, baseline=baseline, timeout=bound, target_label=target_label, baseline_label=baseline_label
    )
    for comparison in realapps_report.results:
        report.add(comparison)

    report.finish()
    return report
