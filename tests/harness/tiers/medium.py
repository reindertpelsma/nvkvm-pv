"""Tier: medium -- ~1 minute, cheap quantitative checks. "Is it fast", not
just "does it run".

Two real workloads, each a self-contained dlopen-based C probe (same
technique as tests/validate.sh's embedded probes: dlopen libcuda, hand-roll
the handful of driver-API types needed, link only -ldl -lm -- no CUDA
toolkit, no -dev packages):

  - GEMM throughput   : reuses tests/integration/gpu_bench.c verbatim (its
                        Phase A). Not reimplemented -- that file already IS
                        the dependency-free, self-contained probe this tier
                        wants.
  - H2D/D2D/D2H bandwidth triad : tests/harness/probes/mem_bandwidth_probe.c
                        (new -- tests/perf/htod_probe.c and dtoh_probe.c are
                        the prior art but `#include <cuda.h>`, i.e. they need
                        actual CUDA headers, which breaks the "only cc" rule
                        this tier follows).

Every check here is produced as a Comparison (see result.py) against the
reference thresholds already established by this project's host-vs-guest
harness (tests/perf/run_parity.sh / tests/perf/README.md, RTX 3060, driver
580.159.04, 2026-06-01) -- reused verbatim, not reinvented:

    GEMM 2048^3 fp32   >=0.90   (this tier uses a smaller/faster N; see below)
    HtoD reused        >=0.80
    DtoH warm cached   >=0.80

DtoD has no equivalent row in that reference (H2D/D2H are the two legs it
measures); this tier applies the same >=0.80 threshold to it as a reasonable
default, not a validated number -- flagged in the band's `note`.

`run()`/`gemm_comparison()`/`bandwidth_comparisons()` all take an optional
`baseline` Machine, the same shape as tiers/realapps.py: give one (e.g.
`baseline=ChrootMachine(base)` alongside `machine=VMMachine(base)`, both
wrapping the same physical box) and every Comparison here carries a real
target-vs-baseline ratio, gated by the bands above, with each Observation
labelled by which machine produced it (`target_label`/`baseline_label`).
`baseline=None` (the default) keeps the old degenerate single-sided
behaviour -- run_tests.py's own default invocation, no --baseline given,
exercises exactly that path.
"""

from __future__ import annotations

import asyncio
import re
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from item import Item, sha256_file
from machine import Machine, ensure_dir
from result import Comparison, Observation, Report, Status, ToleranceBand

PROBES_DIR = Path(__file__).resolve().parent.parent / "probes"
GPU_BENCH_C = Path(__file__).resolve().parent.parent.parent / "integration" / "gpu_bench.c"
BANDWIDTH_C = PROBES_DIR / "mem_bandwidth_probe.c"

DEFAULT_TIMEOUT = 60.0
COMPILE_TIMEOUT = 30.0

GEMM_BAND = ToleranceBand("higher_is_better", 0.90, "tests/perf/run_parity.sh reference: GEMM 2048^3 fp32 gate")
H2D_BAND = ToleranceBand("higher_is_better", 0.80, "tests/perf/run_parity.sh reference: HtoD reused gate")
D2H_BAND = ToleranceBand("higher_is_better", 0.80, "tests/perf/run_parity.sh reference: DtoH warm cached gate")
D2D_BAND = ToleranceBand("higher_is_better", 0.80, "no reference row for DtoD; reusing the H2D/D2H threshold as a default")

# Distinguishes "no GPU/driver here at all" (SKIP) from a real API failure
# (FAIL) by grepping stderr, the same way tests/validate.sh's checks do.
_NO_GPU_MARKERS = ("no libcuda",)


class ProbeCompileError(RuntimeError):
    pass


async def _compile(machine: Machine, src_path: Path, out_name: str, timeout: float) -> Path:
    scratch = machine.scratch()
    # See machine.ensure_dir's own docstring: SSHMachine/VMMachine's
    # scratch() is bare path arithmetic (no local filesystem to
    # synchronously mkdir into), so `cc -o <scratch>/<out_name>` fails with
    # a plain "No such file or directory" against either of those unless
    # something creates the directory first.
    await ensure_dir(machine, scratch, timeout=timeout)
    item = Item(name=f"harness-src-{src_path.name}", sha256=sha256_file(src_path), local_path=src_path)
    src_on_machine = await machine.need(item)
    out_path = scratch / out_name
    cmd = ["cc", "-O2", "-o", str(out_path), str(src_on_machine), "-ldl", "-lm"]
    try:
        command = await machine.run(cmd, timeout=timeout)
        rc = await command.wait(timeout=timeout)
    except asyncio.TimeoutError as exc:
        raise ProbeCompileError(f"{src_path.name}: compilation did not finish within {timeout:.0f}s") from exc
    if rc != 0:
        raise ProbeCompileError(f"{src_path.name}: cc exited {rc}: {command.stderr.decode(errors='replace')[-500:]}")
    return out_path


def _classify_failure(rc: int, stderr: str) -> Optional[Status]:
    """None means 'not a structural no-GPU SKIP, caller should treat rc!=0 as FAIL'."""
    if any(marker in stderr for marker in _NO_GPU_MARKERS):
        return Status.SKIP
    return None


async def _run_gpu_bench(machine: Machine, *, timeout: float) -> tuple:
    """Compile+run tests/integration/gpu_bench.c once. Returns
    (status_or_None, stdout, stderr, duration_s) where status_or_None is only
    set for compile-time/timeout failures the caller should turn directly
    into an Observation without trying to parse anything."""
    t0 = time.monotonic()
    try:
        binpath = await _compile(machine, GPU_BENCH_C, "gpu_bench", COMPILE_TIMEOUT)
    except ProbeCompileError as exc:
        return Status.UNTESTED, "", str(exc), time.monotonic() - t0
    try:
        # N=1024 (not gpu_bench's default 2048) to keep this tier at "~1 minute";
        # small iteration counts on Phases B/C are for the large tier's reuse.
        command = await machine.run([str(binpath), "1024", "10", "500", "500"], timeout=timeout)
        rc = await command.wait(timeout=timeout)
    except asyncio.TimeoutError:
        return Status.UNTESTED, "", f"gpu_bench did not finish within {timeout:.0f}s and was killed", time.monotonic() - t0
    stdout = command.stdout.decode(errors="replace")
    stderr = command.stderr.decode(errors="replace")
    duration = time.monotonic() - t0
    skip = _classify_failure(rc, stderr)
    if skip is not None:
        return skip, stdout, stderr, duration
    if rc != 0:
        return Status.FAIL, stdout, stderr, duration
    return None, stdout, stderr, duration


def _device_line(stdout: str) -> str:
    m = re.search(r"^device:\s*(.*)$", stdout, re.MULTILINE)
    return m.group(1).strip() if m and m.group(1).strip() else "(unnamed device)"


async def _gemm_observation(machine: Machine, *, timeout: float, label: str) -> Observation:
    status, stdout, stderr, duration = await _run_gpu_bench(machine, timeout=timeout)
    if status is not None:
        detail = stderr.strip()[-500:] or f"gpu_bench produced no output (status {status.value})"
        return Observation(status=status, detail=detail, unit="GFLOP/s", duration_s=duration, label=label)

    m = re.search(r"A throughput.*?([\d.]+)\s*GFLOP/s", stdout)
    if not m:
        return Observation(
            status=Status.UNTESTED,
            detail=f"gpu_bench exited 0 but its GEMM line was not parseable; stdout: {stdout.strip()[:300]!r}",
            unit="GFLOP/s",
            duration_s=duration,
            label=label,
        )

    gflops = float(m.group(1))
    return Observation(
        status=Status.PASS,
        detail=f"{gflops:.1f} GFLOP/s (1024x1024 fp32, device: {_device_line(stdout)})",
        value=gflops,
        unit="GFLOP/s",
        duration_s=duration,
        label=label,
    )


async def gemm_comparison(
    target: Machine,
    *,
    timeout: float,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> Comparison:
    target_obs = await _gemm_observation(target, timeout=timeout, label=target_label)
    baseline_obs = await _gemm_observation(baseline, timeout=timeout, label=baseline_label) if baseline is not None else None
    return Comparison(name="medium:gemm_gflops", tier="medium", target=target_obs, baseline=baseline_obs, band=GEMM_BAND)


_BANDWIDTH_LEGS = (("medium:mem_bandwidth_h2d", "H2D", H2D_BAND), ("medium:mem_bandwidth_d2d", "D2D", D2D_BAND), ("medium:mem_bandwidth_d2h", "D2H", D2H_BAND))


async def _bandwidth_observations(machine: Machine, *, timeout: float, label: str) -> dict:
    """Compiles+runs mem_bandwidth_probe.c ONCE and turns its three legs into
    three Observations, keyed "H2D"/"D2D"/"D2H" -- deliberately not going
    through one-Workload-per-run, to avoid three separate GPU probe
    invocations for numbers one run already produces."""
    t0 = time.monotonic()
    keys = [key for _, key, _ in _BANDWIDTH_LEGS]

    try:
        binpath = await _compile(machine, BANDWIDTH_C, "mem_bandwidth_probe", COMPILE_TIMEOUT)
    except ProbeCompileError as exc:
        duration = time.monotonic() - t0
        return {key: Observation(status=Status.UNTESTED, detail=str(exc), unit="GB/s", duration_s=duration, label=label) for key in keys}

    try:
        command = await machine.run([str(binpath), "64", "8"], timeout=timeout)
        rc = await command.wait(timeout=timeout)
    except asyncio.TimeoutError:
        duration = time.monotonic() - t0
        detail = f"mem_bandwidth_probe did not finish within {timeout:.0f}s and was killed"
        return {key: Observation(status=Status.UNTESTED, detail=detail, unit="GB/s", duration_s=duration, label=label) for key in keys}

    stdout = command.stdout.decode(errors="replace")
    stderr = command.stderr.decode(errors="replace")
    duration = time.monotonic() - t0
    skip = _classify_failure(rc, stderr)

    observations = {}
    for key in keys:
        if skip is not None:
            obs = Observation(status=skip, detail=stderr.strip()[-500:] or "no GPU/driver on this machine", unit="GB/s", duration_s=duration, label=label)
        elif rc != 0:
            obs = Observation(
                status=Status.FAIL,
                detail=f"mem_bandwidth_probe exited {rc}: {stderr.strip()[-400:] or '(no stderr)'}",
                unit="GB/s",
                duration_s=duration,
                label=label,
            )
        else:
            m = re.search(rf"^{key}:\s*([\d.]+)\s*GB/s", stdout, re.MULTILINE)
            if not m:
                obs = Observation(
                    status=Status.UNTESTED,
                    detail=f"mem_bandwidth_probe exited 0 but its {key} line was not parseable; stdout: {stdout.strip()[:300]!r}",
                    unit="GB/s",
                    duration_s=duration,
                    label=label,
                )
            else:
                gbs = float(m.group(1))
                obs = Observation(
                    status=Status.PASS,
                    detail=f"{gbs:.2f} GB/s (64MB x8, device: {_device_line(stdout)})",
                    value=gbs,
                    unit="GB/s",
                    duration_s=duration,
                    label=label,
                )
        observations[key] = obs
    return observations


async def bandwidth_comparisons(
    target: Machine,
    *,
    timeout: float,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> list:
    target_obs = await _bandwidth_observations(target, timeout=timeout, label=target_label)
    baseline_obs = await _bandwidth_observations(baseline, timeout=timeout, label=baseline_label) if baseline is not None else None
    return [
        Comparison(
            name=name,
            tier="medium",
            target=target_obs[key],
            baseline=(baseline_obs[key] if baseline_obs is not None else None),
            band=band,
        )
        for name, key, band in _BANDWIDTH_LEGS
    ]


async def run(
    machine: Machine,
    *,
    timeout: Optional[float] = None,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> Report:
    report = Report(tier="medium")
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT

    report.add(await gemm_comparison(machine, timeout=bound, baseline=baseline, target_label=target_label, baseline_label=baseline_label))
    for comparison in await bandwidth_comparisons(machine, timeout=bound, baseline=baseline, target_label=target_label, baseline_label=baseline_label):
        report.add(comparison)

    report.finish()
    return report
