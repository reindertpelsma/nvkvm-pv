"""tiers/small.py against a scripted fake Machine -- no real validate.sh
execution, no GPU, no network (validate.sh itself is invoked as a plain
`machine.run()` call, so a fake responder stands in for it exactly like
tests/test_medium.py's fake `cc`). Covers the direct-mapping of
validate.sh's own PASS/FAIL/SKIP/UNTESTED JSON into this harness's
Comparisons, the various "harness broke" UNTESTED paths (no json output, no
checks), and -- what this file exists to pin down -- that a `baseline`
given to this tier NEVER produces a fabricated single-machine-tier ratio:
it is called out as one explicit, up-front SKIP Comparison, and every
per-check Comparison still stays correctly attributed to the target."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Callable

from item import Item
from machine import Command, Machine
from result import Status
from tiers import small


class FakeCommand(Command):
    def __init__(self, rc: int, stdout: bytes = b"", stderr: bytes = b""):
        self._rc = rc
        self._stdout = stdout
        self._stderr = stderr

    @property
    def start_time(self) -> float:
        return 0.0

    @property
    def returncode(self):
        return self._rc

    async def wait(self, timeout=None) -> int:
        return self._rc

    async def kill(self) -> None:
        pass

    @property
    def stdout(self) -> bytes:
        return self._stdout

    @property
    def stderr(self) -> bytes:
        return self._stderr


class FakeMachine(Machine):
    """`scratch()` returns a REAL tmp_path-backed directory (small.py writes
    validate.sh's --json path under it and then reads it back with a plain
    `Path.read_text()`, so this can't be an in-memory fake the way
    run()/need() are)."""

    def __init__(self, responder: Callable[[list], tuple], *, scratch_dir: Path, label: str = "fake"):
        self._responder = responder
        self._scratch_dir = scratch_dir
        self.calls: list = []
        self.label = label

    async def run(self, cmd, *, cwd=None, env=None, stdin=None, timeout=None):
        self.calls.append(list(cmd))
        rc, out, err = self._responder(list(cmd))
        return FakeCommand(rc, out, err)

    async def need(self, item: Item) -> Path:
        raise NotImplementedError("small.py never calls need()")

    def scratch(self) -> Path:
        self._scratch_dir.mkdir(parents=True, exist_ok=True)
        return self._scratch_dir


def _validate_sh_responder(json_out: Path, payload: dict, rc: int = 0):
    """Writes `payload` to wherever validate.sh's --json arg pointed (the
    exact call shape small.py builds: ["bash", VALIDATE_SH, "--json",
    str(json_out)]) and returns `rc` -- standing in for a real validate.sh
    run without executing any shell script."""

    def responder(cmd):
        assert cmd[0] == "bash"
        assert cmd[2] == "--json"
        Path(cmd[3]).write_text(json.dumps(payload))
        return rc, b"", b""

    return responder


def _sample_payload():
    return {
        "gpu": "Fake GPU 9000",
        "driver": "999.99.99",
        "cuda": "12.9",
        "checks": [
            {"name": "cuda_init", "status": "PASS", "detail": "cuInit ok"},
            {"name": "vk_headless", "status": "SKIP", "detail": "no display"},
        ],
    }


# --- direct PASS/SKIP mapping, no baseline ----------------------------------------


def test_maps_validate_sh_checks_1_to_1(tmp_path):
    async def body():
        scratch = tmp_path / "scratch"
        machine = FakeMachine(
            _validate_sh_responder(scratch / "validate_result.json", _sample_payload()),
            scratch_dir=scratch,
        )
        report = await small.run(machine, timeout=5)
        by_name = {c.name: c for c in report.results}
        assert by_name["validate_sh:cuda_init"].status == Status.PASS
        assert by_name["validate_sh:cuda_init"].target.detail == "cuInit ok"
        assert by_name["validate_sh:vk_headless"].status == Status.SKIP
        assert report.meta["gpu"] == "Fake GPU 9000"
        for c in report.results:
            assert c.baseline is None
            assert c.target.label == "this machine"

    asyncio.run(body())


def test_missing_json_output_is_untested(tmp_path):
    async def body():
        machine = FakeMachine(lambda cmd: (1, b"", b"boom"), scratch_dir=tmp_path / "scratch")
        report = await small.run(machine, timeout=5)
        assert len(report.results) == 1
        assert report.results[0].status == Status.UNTESTED
        assert "without producing --json output" in report.results[0].target.detail

    asyncio.run(body())


# --- baseline given but this tier structurally cannot compare ---------------------


def test_baseline_given_adds_one_explicit_skip_never_a_fabricated_ratio(tmp_path):
    async def body():
        scratch = tmp_path / "scratch"
        target = FakeMachine(
            _validate_sh_responder(scratch / "validate_result.json", _sample_payload()),
            scratch_dir=scratch,
        )
        baseline = FakeMachine(lambda cmd: (0, b"", b""), scratch_dir=tmp_path / "baseline-scratch")
        report = await small.run(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        by_name = {c.name: c for c in report.results}

        note = by_name["small_tier:comparison_support"]
        assert note.status == Status.SKIP
        assert note.baseline is None  # never a two-sided Comparison from this tier
        assert "not ratio-worthy" in note.target.detail
        assert "host" in note.target.detail  # names which baseline was given
        assert note.target.label == "guest"

        # validate.sh itself only ever ran against the target -- never the
        # baseline (baseline's FakeMachine responder is never asked for
        # "bash .../validate.sh").
        assert baseline.calls == []

        # per-check Comparisons are still correctly attributed and still
        # degenerate (no ratio was invented just because a baseline exists).
        real_check = by_name["validate_sh:cuda_init"]
        assert real_check.baseline is None
        assert real_check.target.label == "guest"
        assert real_check.status == Status.PASS

    asyncio.run(body())


def test_no_baseline_has_no_comparison_support_note(tmp_path):
    async def body():
        scratch = tmp_path / "scratch"
        machine = FakeMachine(
            _validate_sh_responder(scratch / "validate_result.json", _sample_payload()),
            scratch_dir=scratch,
        )
        report = await small.run(machine, timeout=5)
        assert "small_tier:comparison_support" not in {c.name for c in report.results}

    asyncio.run(body())
