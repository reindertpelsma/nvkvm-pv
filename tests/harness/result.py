"""The result vocabulary AND the comparison-first data model.

THE UNIT OF OUTPUT IS A COMPARISON, NOT A STANDALONE RESULT.

This project exists to prove a paravirtual GPU is near-bare-metal, so "the
guest passed" is not the interesting artifact -- "the guest matches the host,
within a stated tolerance" is. Every tier produces `Comparison` objects:

    compare(workload, target=..., baseline=..., ...) -> Comparison

A `Comparison` holds an `Observation` for the target machine and, optionally,
one for the baseline machine. When there IS no baseline -- `run_tests.py`
was invoked with no `--baseline`, or a tier's own checks are structurally
not ratio-worthy (see small.py) -- that is the DEGENERATE case:
`Comparison.baseline is None`, `Comparison.ratio is None`, and the report
says exactly that -- it never fabricates a 1.00x, and it never silently
drops the "no baseline" fact into a comment nobody reads.

Status vocabulary (copied deliberately from tests/validate.sh -- see the
design-rules comment near the top of that file):

  PASS      ran, and (for a two-sided comparison) the ratio met its
            ToleranceBand, or (single-sided) the check itself passed.
  FAIL      ran, and did not. A FAIL on EITHER side of a comparison, or a
            ratio outside its band, is the loudest thing there is. Outranks
            everything else in a two-sided Comparison's overall `.status`.
  SKIP      structurally impossible here (no GPU, headless, package absent,
            missing app on one side of a comparison). Carries a reason. A
            legitimate, expected, non-alarming outcome -- and, in a
            two-sided Comparison where neither side FAILed or came back
            UNTESTED, SKIP is what the WHOLE Comparison reports too: a
            structurally-impossible comparison is disclosed evidence
            ("no GPU on the baseline"), not an absence of it.
  UNTESTED  no evidence either way -- timeout, harness broke, or (specific
            to comparisons) EITHER side of a two-sided comparison did not
            produce a value for a reason OTHER than a disclosed SKIP (e.g.
            one side timed out while the other ran clean). A ratio is NEVER
            computed against a missing side, and a missing run NEVER
            renders as 1.00x or as a pass. In a two-sided Comparison,
            UNTESTED outranks SKIP (see Comparison.status) -- a broken/timed
            -out run must never be quietly reclassified as an expected skip
            just because the other side also produced no value. UNTESTED
            always moves the exit code and is never silenceable.

Every Observation carries a non-empty `detail` -- the observed value or the
reason. "Vulkan OK" is not an acceptable detail; "Vulkan: NVIDIA GeForce RTX
3060" is. A numeric `value` is optional on an Observation: some checks (a
correctness assertion, a device-identity check) have no ratio-worthy number
at all, and are always reported single-sided/degenerate for that reason --
see small.py, which wraps tests/validate.sh's own checks this way.
"""

from __future__ import annotations

import abc
import dataclasses
import enum
import json
import platform
import time
from pathlib import Path
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from machine import Machine


class Status(str, enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    UNTESTED = "UNTESTED"


@dataclasses.dataclass(frozen=True)
class Observation:
    """One side's raw measurement of a workload. `label` is filled in by
    `compare()` (e.g. "host" / "guest" / "this machine") -- a Workload's
    `run()` doesn't need to know which side of a comparison it is."""

    status: Status
    detail: str
    value: Optional[float] = None  # None when there's no ratio-worthy number
    unit: str = ""
    duration_s: Optional[float] = None
    label: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.status, Status):
            raise TypeError(f"Observation: status must be a Status, got {self.status!r}")
        if not self.detail or not self.detail.strip():
            raise ValueError(
                f"Observation ({self.status.value}) has an empty detail -- "
                f"every observation must carry an observed value or a reason"
            )

    def to_dict(self) -> dict:
        return {
            "label": self.label,
            "status": self.status.value,
            "detail": self.detail,
            "value": self.value,
            "unit": self.unit,
            "duration_s": self.duration_s,
        }


@dataclasses.dataclass(frozen=True)
class ToleranceBand:
    """What ratio (target/baseline) counts as parity for one workload.

    Reference values are not invented here -- they're carried over verbatim
    from this project's existing host-vs-guest harness
    (tests/perf/run_parity.sh, tests/perf/README.md's "Reference numbers"
    table, RTX 3060 / driver 580.159.04, 2026-06-01): GEMM/bandwidth gate
    at >=0.90 / >=0.80 (higher_is_better), the known control-path tax
    (launch+sync RTT, alloc+free RTT) is a tripwire at <4x / <50x
    (lower_is_better) -- never expected to reach parity, tracked so it
    doesn't silently get worse.
    """

    direction: str  # "higher_is_better" or "lower_is_better"
    threshold: float
    note: str = ""

    def __post_init__(self) -> None:
        if self.direction not in ("higher_is_better", "lower_is_better"):
            raise ValueError(
                f"ToleranceBand.direction must be 'higher_is_better' or 'lower_is_better', got {self.direction!r}"
            )

    def passes(self, ratio: float) -> bool:
        if self.direction == "higher_is_better":
            return ratio >= self.threshold
        return ratio <= self.threshold

    def describe(self) -> str:
        op = ">=" if self.direction == "higher_is_better" else "<="
        return f"gate {op}{self.threshold}x" + (f" ({self.note})" if self.note else "")


@dataclasses.dataclass(frozen=True)
class Comparison:
    name: str
    tier: str
    target: Observation
    baseline: Optional[Observation] = None  # None == degenerate, single-machine mode
    band: Optional[ToleranceBand] = None

    @property
    def ratio(self) -> Optional[float]:
        """target.value / baseline.value, or None if there's no baseline, no
        ratio-worthy value on either side, or the baseline value is zero
        (nothing meaningful to divide by)."""
        if self.baseline is None:
            return None
        if self.target.value is None or self.baseline.value is None:
            return None
        if self.baseline.value == 0:
            return None
        return self.target.value / self.baseline.value

    @property
    def status(self) -> Status:
        if self.baseline is None:
            # Degenerate: nothing to compare against. Pass the target's own
            # status straight through -- there is no ratio to judge.
            return self.target.status
        # Two-sided. Precedence, highest first: FAIL, then UNTESTED, then
        # SKIP, then an actual ratio/band verdict. This is NOT the same
        # ordering as Report.exit_code() (which only ever sees FAIL vs
        # UNTESTED, since SKIP never reaches it) -- it's what a single
        # Comparison reports about ITSELF when its two sides disagree:
        #   - A real, observed FAIL on either side outranks everything,
        #     exactly like validate.sh's verdict() ordering.
        #   - UNTESTED beats SKIP: a harness that broke or timed out (no
        #     evidence either way) is not the same claim as "this side
        #     structurally can't run this" -- an UNTESTED anywhere must
        #     surface as UNTESTED, never get quietly reclassified as an
        #     expected, non-alarming SKIP just because the other side also
        #     happened to skip.
        #   - SKIP wins once neither side FAILed or came back UNTESTED: a
        #     missing app on one side, or a GPU absent on the baseline, is a
        #     structurally impossible comparison -- an expected, disclosed
        #     outcome (SKIP, with a reason on the Observation that skipped),
        #     never a fabricated ratio and never silently downgraded to
        #     "no evidence" (UNTESTED) when there plainly IS evidence: we
        #     know exactly why no ratio exists.
        if self.target.status is Status.FAIL or self.baseline.status is Status.FAIL:
            return Status.FAIL
        if self.target.status is Status.UNTESTED or self.baseline.status is Status.UNTESTED:
            return Status.UNTESTED
        if self.target.status is Status.SKIP or self.baseline.status is Status.SKIP:
            return Status.SKIP
        if self.ratio is None:
            # Neither side FAILed/UNTESTED/SKIPped, yet there's still no
            # ratio (e.g. a zero baseline value -- nothing meaningful to
            # divide by). Structurally unable to judge parity: never compute
            # a ratio against a missing/unusable side, never render that as
            # a pass.
            return Status.UNTESTED
        if self.band is None:
            return Status.PASS
        return Status.PASS if self.band.passes(self.ratio) else Status.FAIL

    def detail(self) -> str:
        if self.baseline is None:
            note = "no baseline machine configured -- raw value only, no ratio"
            return f"{self.target.label or 'target'}: {self.target.detail} ({note})"
        ratio = self.ratio
        ratio_s = f"{ratio:.2f}x" if ratio is not None else "n/a"
        band_s = f"  [{self.band.describe()}]" if self.band else ""
        return (
            f"{self.baseline.label or 'baseline'}={self.baseline.detail}  "
            f"{self.target.label or 'target'}={self.target.detail}  "
            f"ratio(target/baseline)={ratio_s}{band_s}"
        )

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "tier": self.tier,
            "status": self.status.value,
            "detail": self.detail(),
            "ratio": self.ratio,
            "band": dataclasses.asdict(self.band) if self.band else None,
            "target": self.target.to_dict(),
            "baseline": self.baseline.to_dict() if self.baseline else None,
        }

    @property
    def duration_s(self) -> Optional[float]:
        parts = [d for d in (self.target.duration_s, self.baseline.duration_s if self.baseline else None) if d is not None]
        return sum(parts) if parts else None


class Workload(abc.ABC):
    """A single-machine probe that produces one Observation. Concrete tiers
    implement this per metric; `compare()` runs it on one or two Machines and
    wraps the result(s) in a Comparison. Not every workload goes through this
    -- see tiers/medium.py's bandwidth triad, which shares one probe
    execution across three Comparisons and builds them directly."""

    name: str
    tier: str
    unit: str = ""
    band: Optional[ToleranceBand] = None

    @abc.abstractmethod
    async def run(self, machine: "Machine", *, timeout: float) -> Observation: ...


async def compare(
    workload: Workload,
    *,
    target: "Machine",
    timeout: float,
    target_label: str = "target",
    baseline: Optional["Machine"] = None,
    baseline_label: str = "baseline",
) -> Comparison:
    """Run `workload` on `target` (and, if given, `baseline`) and return the
    Comparison. See the module docstring for the degenerate (no-baseline)
    case and the UNTESTED-on-missing-value rule."""
    target_obs = dataclasses.replace(await workload.run(target, timeout=timeout), label=target_label)
    baseline_obs = None
    if baseline is not None:
        baseline_obs = dataclasses.replace(await workload.run(baseline, timeout=timeout), label=baseline_label)
    return Comparison(name=workload.name, tier=workload.tier, target=target_obs, baseline=baseline_obs, band=workload.band)


@dataclasses.dataclass
class Report:
    tier: str
    results: list = dataclasses.field(default_factory=list)  # list[Comparison]
    started_at: float = dataclasses.field(default_factory=time.time)
    finished_at: Optional[float] = None
    meta: dict = dataclasses.field(default_factory=dict)

    def add(self, comparison: Comparison) -> None:
        self.results.append(comparison)

    def counts(self) -> dict:
        c = {s.value: 0 for s in Status}
        for r in self.results:
            c[r.status.value] += 1
        return c

    def exit_code(self) -> int:
        """0 pass, 1 fail, 3 cannot-validate (untested present). SKIP never
        moves the exit code -- see the module docstring. Ordering matches
        tests/validate.sh's verdict(): FAIL always outranks UNTESTED, which
        always outranks a clean pass, regardless of how many other checks
        passed. Exit code 2 (validate.sh's INCOMPLETE / undeclared-skip) is
        reserved but unused -- this harness has no --allow-skip concept."""
        c = self.counts()
        if c[Status.FAIL.value] > 0:
            return 1
        if c[Status.UNTESTED.value] > 0:
            return 3
        return 0

    def verdict(self) -> str:
        c = self.counts()
        if c[Status.FAIL.value] > 0:
            return "FAIL"
        if c[Status.UNTESTED.value] > 0:
            return "CANNOT VALIDATE"
        return "PASS"

    def finish(self) -> None:
        self.finished_at = time.time()

    def to_dict(self) -> dict:
        c = self.counts()
        return {
            "tier": self.tier,
            "verdict": self.verdict(),
            "exit_code": self.exit_code(),
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "duration_s": (self.finished_at - self.started_at) if self.finished_at else None,
            "counts": c,
            "total": len(self.results),
            "meta": self.meta,
            "comparisons": [r.to_dict() for r in self.results],
        }

    def write_json(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2, sort_keys=False) + "\n")

    def to_markdown(self) -> str:
        c = self.counts()
        lines = [
            f"# nvkvm-pv test harness -- tier `{self.tier}`",
            "",
            f"**Verdict: {self.verdict()}**  (exit code {self.exit_code()})",
            "",
            f"- Host: {self.meta.get('platform', platform.platform())}",
            f"- Started: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.started_at))}",
        ]
        if self.finished_at:
            lines.append(f"- Duration: {self.finished_at - self.started_at:.1f}s")
        lines += [
            f"- Totals: {len(self.results)} checks -- "
            f"{c[Status.PASS.value]} PASS, {c[Status.FAIL.value]} FAIL, "
            f"{c[Status.SKIP.value]} SKIP, {c[Status.UNTESTED.value]} UNTESTED",
            "",
            "| Status | Check | Ratio | Detail | Duration |",
            "|---|---|---|---|---|",
        ]
        for r in self.results:
            dur = f"{r.duration_s:.2f}s" if r.duration_s is not None else "--"
            detail = r.detail().replace("|", "\\|").replace("\n", " ")
            ratio = f"{r.ratio:.2f}x" if r.ratio is not None else "--"
            lines.append(f"| {r.status.value} | {r.name} | {ratio} | {detail} | {dur} |")
        lines.append("")
        if c[Status.UNTESTED.value]:
            lines.append(
                f"> {c[Status.UNTESTED.value]} check(s) were UNTESTED: this run has "
                "**no evidence either way** about them (including: a comparison whose "
                "baseline or target side produced no value). That is not a caveat on a "
                "pass -- see the exit code."
            )
        return "\n".join(lines) + "\n"

    def write_markdown(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(self.to_markdown())
