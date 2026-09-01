"""Tier: small -- bring-up correctness.

INVOKES the existing tests/validate.sh; does NOT reimplement it. That script
is a deliberately dependency-free shell artifact (probes dlopen() everything,
links only -ldl -lm) so a stranger can run it on bare hardware with nothing
installed -- that property is load-bearing and this tier must not erode it
by, say, requiring Python to reproduce its checks. All this module does is
shell out to it, and translate its --json output and exit code into this
harness's own PASS/FAIL/SKIP/UNTESTED vocabulary.

validate.sh's own vocabulary is already PASS/FAIL/SKIP/UNTESTED (see its
design-rules comment block), so mapping is a direct 1:1 per check. Its
overall exit code (0/1/2/3) is not re-derived here: this tier's
Report.exit_code() is computed the normal way, from the individual per-check
statuses.

Every check becomes a DEGENERATE Comparison (baseline=None): validate.sh runs
entirely inside one guest by design ("Run this INSIDE the guest VM" -- its
own header), so there is no second side to compare against here, and most of
its checks (device identity, byte-exact correctness) have no ratio-worthy
number anyway. See result.py's module docstring for what "degenerate" means.
"""

from __future__ import annotations

import asyncio
import json
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from machine import Machine
from result import Comparison, Observation, Report, Status

VALIDATE_SH = Path(__file__).resolve().parent.parent.parent / "validate.sh"
DEFAULT_TIMEOUT = 300.0  # validate.sh compiles ~24 C probes; generous but bounded

_STATUS_MAP = {
    "PASS": Status.PASS,
    "FAIL": Status.FAIL,
    "SKIP": Status.SKIP,
    "UNTESTED": Status.UNTESTED,
}


def _single(name: str, status: Status, detail: str, duration_s: Optional[float] = None) -> Comparison:
    obs = Observation(status=status, detail=detail, duration_s=duration_s, label="this machine")
    return Comparison(name=name, tier="small", target=obs)


async def run(machine: Machine, *, timeout: Optional[float] = None, extra_args: Optional[list] = None) -> Report:
    report = Report(tier="small")
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT

    if not VALIDATE_SH.exists():
        report.add(_single("small_tier", Status.UNTESTED, f"tests/validate.sh not found at {VALIDATE_SH} -- harness/repo layout mismatch"))
        report.finish()
        return report

    scratch = machine.scratch()
    json_out = scratch / "validate_result.json"
    cmd = ["bash", str(VALIDATE_SH), "--json", str(json_out)]
    if extra_args:
        cmd += list(extra_args)

    t0 = time.monotonic()
    try:
        command = await machine.run(cmd, timeout=bound)
        rc = await command.wait(timeout=bound)
    except asyncio.TimeoutError:
        report.add(
            _single(
                "validate_sh",
                Status.UNTESTED,
                f"tests/validate.sh did not finish within {bound:.0f}s and was killed",
                duration_s=time.monotonic() - t0,
            )
        )
        report.finish()
        return report
    duration = time.monotonic() - t0

    if not json_out.exists():
        stderr_tail = command.stderr.decode(errors="replace")[-2000:]
        report.add(
            _single(
                "validate_sh",
                Status.UNTESTED,
                f"tests/validate.sh exited {rc} without producing --json output ({json_out}); stderr tail: {stderr_tail!r}",
                duration_s=duration,
            )
        )
        report.finish()
        return report

    try:
        payload = json.loads(json_out.read_text())
    except (json.JSONDecodeError, OSError) as exc:
        report.add(
            _single(
                "validate_sh",
                Status.UNTESTED,
                f"tests/validate.sh's --json output at {json_out} was not parseable: {exc}",
                duration_s=duration,
            )
        )
        report.finish()
        return report

    report.meta.update(
        {
            "gpu": payload.get("gpu"),
            "driver": payload.get("driver"),
            "cuda": payload.get("cuda"),
            "validate_sh_exit_code": rc,
        }
    )

    checks = payload.get("checks", [])
    if not checks:
        report.add(
            _single(
                "validate_sh",
                Status.UNTESTED,
                f"tests/validate.sh's JSON reported zero checks (exit {rc}) -- nothing was evaluated",
                duration_s=duration,
            )
        )
        report.finish()
        return report

    per_check = duration / max(len(checks), 1)
    for c in checks:
        raw_status = c.get("status", "")
        status = _STATUS_MAP.get(raw_status)
        name = f"validate_sh:{c.get('name', '?')}"
        if status is None:
            # validate.sh's own vocabulary is closed; a status we don't
            # recognise means something is broken in the harness/parity
            # between the two, not a real result about the GPU.
            report.add(_single(name, Status.UNTESTED, f"unrecognised status {raw_status!r} in validate.sh's JSON output"))
            continue
        detail = c.get("detail") or f"(validate.sh gave no detail for this {raw_status})"
        report.add(_single(name, status, detail, duration_s=per_check))

    report.finish()
    return report
