"""Tier: large -- tens of minutes, real applications. STUB for this first
slice.

Large ALWAYS includes medium's full set first, so a large result is never
weaker than a medium one -- this module runs tiers/medium.py's `run()`
unmodified and folds its Comparisons in before adding anything of its own.

What's added beyond medium: the launch+sync RTT and alloc+free RTT legs of
tests/integration/gpu_bench.c (its Phases B and C -- the same probe medium's
GEMM check already builds and runs for Phase A). These are the two rows in
tests/perf/run_parity.sh's reference table that are explicitly NOT
parity-seeking -- a known, disclosed multi-hop-ioctl control-path tax,
tracked with a regression tripwire rather than a pass/fail-at-parity gate:

    launch+sync RTT   tripwire <4x   (reference: ~1.85x baseline)
    alloc+free RTT    tripwire <50x  (reference: ~29x baseline, the standing
                                       optimization target -- never at parity)

HONEST GAPS, not papered over:
  - This is a placeholder, not a representative "tens of minutes" workload.
    A real large-tier workload (a training run, an inference-server soak, a
    multi-GPU NCCL job) needs `machine.need()` wired to real model/dataset
    Items and is future work -- see the harness README.
  - It re-runs gpu_bench.c from scratch rather than reusing medium's already-
    captured output, because Comparisons/Reports don't currently plumb raw
    probe stdout through -- a known, small inefficiency (one extra compile +
    ~1s run), not a correctness gap.
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

DEFAULT_TIMEOUT = 120.0

LAUNCH_RTT_BAND = ToleranceBand("lower_is_better", 4.0, "tests/perf/run_parity.sh reference: launch+sync RTT tripwire, ~1.85x baseline, known control-path tax")
ALLOC_RTT_BAND = ToleranceBand("lower_is_better", 50.0, "tests/perf/run_parity.sh reference: alloc+free RTT tripwire, ~29x baseline, the standing optimization target")


async def _control_path_comparisons(machine: Machine, *, timeout: float) -> list:
    status, stdout, stderr, duration = await tier_medium._run_gpu_bench(machine, timeout=timeout)
    legs = [
        ("large:launch_rtt_us", "B launch RTT", r"([\d.]+)\s*us/launch", LAUNCH_RTT_BAND, "us/launch"),
        ("large:alloc_rtt_us", "C alloc RTT", r"([\d.]+)\s*us/pair", ALLOC_RTT_BAND, "us/pair"),
    ]
    if status is not None:
        detail = stderr.strip()[-500:] or f"gpu_bench produced no output (status {status.value})"
        return [
            Comparison(
                name=name, tier="large",
                target=Observation(status=status, detail=detail, unit=unit, duration_s=duration, label="this machine"),
                band=band,
            )
            for name, _, _, band, unit in legs
        ]

    comparisons = []
    for name, prefix, value_re, band, unit in legs:
        m = re.search(rf"^{re.escape(prefix)}.*?{value_re}", stdout, re.MULTILINE)
        if not m:
            obs = Observation(
                status=Status.UNTESTED,
                detail=f"gpu_bench exited 0 but its {prefix!r} line was not parseable; stdout: {stdout.strip()[-300:]!r}",
                unit=unit, duration_s=duration, label="this machine",
            )
        else:
            value = float(m.group(1))
            obs = Observation(status=Status.PASS, detail=f"{value:.2f} {unit}", value=value, unit=unit, duration_s=duration, label="this machine")
        comparisons.append(Comparison(name=name, tier="large", target=obs, band=band))
    return comparisons


async def run(machine: Machine, *, timeout: Optional[float] = None) -> Report:
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT

    medium_report = await tier_medium.run(machine, timeout=bound)
    report = Report(tier="large", meta=dict(medium_report.meta))
    for comparison in medium_report.results:
        report.add(Comparison(name=comparison.name, tier="large", target=comparison.target, baseline=comparison.baseline, band=comparison.band))

    for comparison in await _control_path_comparisons(machine, timeout=bound):
        report.add(comparison)

    report.finish()
    return report
