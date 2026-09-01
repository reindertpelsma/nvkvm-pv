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

Every check becomes a DEGENERATE Comparison (baseline=None): validate.sh's
own checks are device identity and byte-exact correctness, not ratio-worthy
numbers, so there is nothing here a host-vs-guest ratio could mean. See
result.py's module docstring for what "degenerate" means.

`run()` still accepts an optional `baseline` -- run_tests.py's CLI passes
whatever `--baseline` it was given to every tier uniformly -- but this tier
NEVER runs validate.sh against it and NEVER produces a ratio: instead, if a
baseline was given, one explicit `small_tier:comparison_support` Comparison
(SKIP, with a reason) says so up front. That is requirement #2 of this
tier's own design brief, verbatim: a tier that structurally cannot compare
must say so explicitly, never quietly report a single-sided number as if
the baseline had simply been forgotten.

KNOWN GAP, confirmed against a real ssh-reachable box while wiring
run_tests.py's --target/--baseline CLI, NOT fixed here: this tier reads
validate.sh's --json output back with a plain local `Path.exists()` /
`Path.read_text()` on the `json_out` path returned by `machine.scratch()`.
That is correct for ThisMachine (real local path) and ChrootMachine (a
host-side path the chroot bind-mounts, so it's ALSO a real local path from
this process's point of view) -- but for SSHMachine/VMMachine, `scratch()`
returns a path that only exists ON THE REMOTE MACHINE; validate.sh runs
there successfully and writes real JSON there, and this process's local
`json_out.exists()` then (almost always correctly) reports False, so the
tier reports UNTESTED "without producing --json output" even though it
did. The Machine interface has no generic "read a file back from this
machine" primitive (need() is for read-only, checksum-known INPUTS;
scratch() output was never meant to be read back this way) -- adding one is
real, separate work, not done in this slice. `--tier small --target ssh`
(or `vm`) is therefore NOT currently usable end to end; `medium`/`large`
have no equivalent gap (they read results from `command.stdout`, which IS
captured correctly for every Machine kind) and were confirmed working over
a real ssh target while diagnosing this.
"""

from __future__ import annotations

import asyncio
import json
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from item import Item, sha256_file
from machine import Machine, ensure_dir
from result import Comparison, Observation, Report, Status

VALIDATE_SH = Path(__file__).resolve().parent.parent.parent / "validate.sh"
DEFAULT_TIMEOUT = 300.0  # validate.sh compiles ~24 C probes; generous but bounded

_STATUS_MAP = {
    "PASS": Status.PASS,
    "FAIL": Status.FAIL,
    "SKIP": Status.SKIP,
    "UNTESTED": Status.UNTESTED,
}


def _single(name: str, status: Status, detail: str, duration_s: Optional[float] = None, label: str = "this machine") -> Comparison:
    obs = Observation(status=status, detail=detail, duration_s=duration_s, label=label)
    return Comparison(name=name, tier="small", target=obs)


async def run(
    machine: Machine,
    *,
    timeout: Optional[float] = None,
    extra_args: Optional[list] = None,
    baseline: Optional[Machine] = None,
    target_label: str = "this machine",
    baseline_label: str = "baseline",
) -> Report:
    report = Report(tier="small")
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT

    if baseline is not None:
        # Say so up front, loudly, rather than silently running only
        # against `machine` and leaving a reader to wonder whether the
        # baseline was ignored by accident. SKIP (never FAIL/UNTESTED,
        # never moves the exit code) -- this is a structural property of
        # the tier, not a result about either machine.
        report.add(
            _single(
                "small_tier:comparison_support",
                Status.SKIP,
                f"small tier (tests/validate.sh) checks are device-identity/correctness, not "
                f"ratio-worthy -- a baseline ({baseline_label}) was given but this tier produces "
                f"no host-vs-guest comparison; every check below runs against {target_label} only",
                label=target_label,
            )
        )

    if not VALIDATE_SH.exists():
        report.add(_single("small_tier", Status.UNTESTED, f"tests/validate.sh not found at {VALIDATE_SH} -- harness/repo layout mismatch", label=target_label))
        report.finish()
        return report

    scratch = machine.scratch()
    await ensure_dir(machine, scratch, timeout=bound)
    json_out = scratch / "validate_result.json"
    # VALIDATE_SH is a path on THIS process's filesystem -- for ThisMachine/
    # ChrootMachine that's also where `machine` reads it from (same or
    # bind-mounted filesystem), but for SSHMachine/VMMachine it is a
    # completely different machine and that bare path would not exist
    # there. need() materialises it correctly either way (a no-op copy for
    # ThisMachine, a real upload for SSHMachine) -- same pattern medium.py's
    # _compile() uses for its own probe sources.
    validate_sh_item = Item(name="validate.sh", sha256=sha256_file(VALIDATE_SH), local_path=VALIDATE_SH)
    validate_sh_on_machine = await machine.need(validate_sh_item)
    cmd = ["bash", str(validate_sh_on_machine), "--json", str(json_out)]
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
                label=target_label,
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
                label=target_label,
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
                label=target_label,
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
                label=target_label,
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
            report.add(_single(name, Status.UNTESTED, f"unrecognised status {raw_status!r} in validate.sh's JSON output", label=target_label))
            continue
        detail = c.get("detail") or f"(validate.sh gave no detail for this {raw_status})"
        report.add(_single(name, status, detail, duration_s=per_check, label=target_label))

    report.finish()
    return report
