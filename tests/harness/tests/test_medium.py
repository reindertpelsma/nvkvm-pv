"""tiers/medium.py against a scripted fake Machine -- no real cc, no GPU, no
network. What's covered: GEMM/bandwidth PASS with a parsed value, the
"no libcuda" SKIP classifier, a compile failure turning into UNTESTED (never
FAIL -- a broken harness step, not an observed GPU result), and -- the new
behaviour this file exists to pin down -- `run()`/`gemm_comparison()`/
`bandwidth_comparisons()` all produce a real two-sided Comparison, correctly
attributed via `target_label`/`baseline_label`, when a `baseline` Machine is
given, and stay degenerate (baseline=None) when one isn't.

Same FakeMachine/FakeCommand shape as tests/test_realapps.py and
tests/test_vm_machine.py (this project's established fake-Machine pattern);
duplicated here rather than shared, matching those two."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Callable, Optional

from item import Item
from machine import Command, Machine
from result import Status
from tiers import medium


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
    """Scripted by a `responder(cmd) -> (rc, stdout, stderr)` callback.
    `need()` is a no-op that just hands back a fixed fake source path --
    medium.py's `_compile()` does call `need()` (unlike realapps.py), but
    only the compiled *command line* matters to the responder, never the
    file's actual bytes."""

    def __init__(self, responder: Callable[[list], tuple], *, label: str = "fake"):
        self._responder = responder
        self.calls: list = []
        self.label = label

    async def run(self, cmd, *, cwd=None, env=None, stdin=None, timeout=None):
        self.calls.append(list(cmd))
        rc, out, err = self._responder(list(cmd))
        return FakeCommand(rc, out, err)

    async def need(self, item: Item) -> Path:
        return Path(f"/fake/src/{item.name}")

    def scratch(self) -> Path:
        return Path("/tmp/fake-medium-scratch")


def _gemm_ok_responder(gflops: float = 512.3, device: str = "Fake GPU 9000"):
    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        # gpu_bench <N> <giter> <biter> <citer>
        stdout = f"device: {device}\nA throughput : 1024x1024 GEMM x10  0.500s  {gflops:.1f} GFLOP/s\n"
        return 0, stdout.encode(), b""

    return responder


def _gemm_no_gpu_responder():
    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        return 1, b"", b"no libcuda\n"

    return responder


def _bandwidth_ok_responder(h2d: float = 11.1, d2d: float = 222.2, d2h: float = 9.9, device: str = "Fake GPU 9000"):
    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        stdout = f"device: {device}\nH2D: {h2d:.2f} GB/s\nD2D: {d2d:.2f} GB/s\nD2H: {d2h:.2f} GB/s\n"
        return 0, stdout.encode(), b""

    return responder


def _bandwidth_no_gpu_responder():
    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        return 1, b"", b"no libcuda: dlopen failed\n"

    return responder


# --- gemm_comparison -----------------------------------------------------------


def test_gemm_pass_reports_parsed_value_and_default_label():
    async def body():
        machine = FakeMachine(_gemm_ok_responder(gflops=512.3, device="Fake GPU 9000"))
        c = await medium.gemm_comparison(machine, timeout=5)
        assert c.status == Status.PASS
        assert c.target.value == 512.3
        assert c.target.label == "this machine"
        assert "Fake GPU 9000" in c.target.detail
        assert c.baseline is None
        assert c.ratio is None

    asyncio.run(body())


def test_gemm_no_gpu_is_skip_not_fail():
    async def body():
        machine = FakeMachine(_gemm_no_gpu_responder())
        c = await medium.gemm_comparison(machine, timeout=5)
        assert c.status == Status.SKIP
        assert "no libcuda" in c.target.detail

    asyncio.run(body())


def test_gemm_compile_failure_is_untested_not_fail():
    async def body():
        def responder(cmd):
            assert cmd[0] == "cc"
            return 1, b"", b"cc: fatal error: no such file\n"

        machine = FakeMachine(responder)
        c = await medium.gemm_comparison(machine, timeout=5)
        assert c.status == Status.UNTESTED
        assert "cc exited" in c.target.detail or "fatal error" in c.target.detail

    asyncio.run(body())


def test_gemm_with_baseline_produces_a_real_two_sided_comparison():
    async def body():
        target = FakeMachine(_gemm_ok_responder(gflops=100.0))
        baseline = FakeMachine(_gemm_ok_responder(gflops=125.0))
        c = await medium.gemm_comparison(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        assert c.baseline is not None
        assert c.target.label == "guest"
        assert c.baseline.label == "host"
        assert c.ratio == 100.0 / 125.0
        # 0.80x is below the 0.90 GEMM gate -> FAIL, not silently PASS.
        assert c.status == Status.FAIL

    asyncio.run(body())


def test_gemm_baseline_no_gpu_is_skip_with_reason_never_a_fabricated_ratio():
    """The scenario named directly in the host-vs-guest CLI requirement:
    GPU absent on the baseline is a SKIP with a reason, never a silent pass
    and never a fabricated ratio."""

    async def body():
        target = FakeMachine(_gemm_ok_responder(gflops=100.0))
        baseline = FakeMachine(_gemm_no_gpu_responder())
        c = await medium.gemm_comparison(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        assert c.status == Status.SKIP
        assert c.ratio is None
        assert "no libcuda" in c.baseline.detail
        assert c.baseline.label == "host"

    asyncio.run(body())


# --- bandwidth_comparisons ------------------------------------------------------


def test_bandwidth_pass_produces_three_legs_with_parsed_values():
    async def body():
        machine = FakeMachine(_bandwidth_ok_responder(h2d=11.1, d2d=222.2, d2h=9.9))
        comparisons = await medium.bandwidth_comparisons(machine, timeout=5)
        by_name = {c.name: c for c in comparisons}
        assert set(by_name) == {"medium:mem_bandwidth_h2d", "medium:mem_bandwidth_d2d", "medium:mem_bandwidth_d2h"}
        assert by_name["medium:mem_bandwidth_h2d"].target.value == 11.1
        assert by_name["medium:mem_bandwidth_d2d"].target.value == 222.2
        assert by_name["medium:mem_bandwidth_d2h"].target.value == 9.9
        for c in comparisons:
            assert c.status == Status.PASS
            assert c.baseline is None

    asyncio.run(body())


def test_bandwidth_no_gpu_skips_all_three_legs():
    async def body():
        machine = FakeMachine(_bandwidth_no_gpu_responder())
        comparisons = await medium.bandwidth_comparisons(machine, timeout=5)
        assert len(comparisons) == 3
        for c in comparisons:
            assert c.status == Status.SKIP

    asyncio.run(body())


def test_bandwidth_with_baseline_attributes_each_leg_to_its_machine():
    async def body():
        target = FakeMachine(_bandwidth_ok_responder(h2d=10.0, d2d=200.0, d2h=8.0))
        baseline = FakeMachine(_bandwidth_ok_responder(h2d=12.5, d2d=200.0, d2h=10.0))
        comparisons = await medium.bandwidth_comparisons(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        by_name = {c.name: c for c in comparisons}
        h2d = by_name["medium:mem_bandwidth_h2d"]
        assert h2d.target.label == "guest"
        assert h2d.baseline.label == "host"
        assert h2d.ratio == 10.0 / 12.5
        d2d = by_name["medium:mem_bandwidth_d2d"]
        assert d2d.ratio == 1.0
        assert d2d.status == Status.PASS

    asyncio.run(body())


def test_bandwidth_baseline_no_gpu_skips_every_leg_with_a_reason():
    async def body():
        target = FakeMachine(_bandwidth_ok_responder())
        baseline = FakeMachine(_bandwidth_no_gpu_responder())
        comparisons = await medium.bandwidth_comparisons(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        for c in comparisons:
            assert c.status == Status.SKIP
            assert c.ratio is None
            assert c.baseline.label == "host"
            assert "no libcuda" in c.baseline.detail

    asyncio.run(body())


# --- run() -----------------------------------------------------------------------


def test_run_no_baseline_stays_degenerate():
    async def body():
        machine = FakeMachine(_gemm_ok_responder())
        report = await medium.run(machine, timeout=5)
        assert len(report.results) == 4  # gemm + 3 bandwidth legs
        for c in report.results:
            assert c.baseline is None
            assert "no baseline machine configured" in c.detail()

    asyncio.run(body())


def _all_probes_ok_responder(gflops: float = 100.0, h2d: float = 10.0, d2d: float = 200.0, d2h: float = 8.0, device: str = "Fake GPU 9000"):
    """Answers BOTH gpu_bench's and mem_bandwidth_probe's output format --
    run() invokes both probes, and a FakeMachine has no notion of which
    binary is which beyond the argv this responder is willing to inspect."""

    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        stdout = (
            f"device: {device}\n"
            f"A throughput : 1024x1024 GEMM x10  0.500s  {gflops:.1f} GFLOP/s\n"
            f"H2D: {h2d:.2f} GB/s\nD2D: {d2d:.2f} GB/s\nD2H: {d2h:.2f} GB/s\n"
        )
        return 0, stdout.encode(), b""

    return responder


def test_run_with_baseline_labels_every_comparison():
    async def body():
        target = FakeMachine(_all_probes_ok_responder(gflops=100.0, h2d=10.0, d2d=200.0, d2h=8.0))
        baseline = FakeMachine(_all_probes_ok_responder(gflops=100.0, h2d=10.0, d2d=200.0, d2h=8.0))
        report = await medium.run(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        assert len(report.results) == 4
        for c in report.results:
            assert c.baseline is not None
            assert c.target.label == "guest"
            assert c.baseline.label == "host"
            assert c.ratio == 1.0
            assert c.status == Status.PASS

    asyncio.run(body())
