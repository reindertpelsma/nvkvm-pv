"""tiers/large.py against a scripted fake Machine -- no real cc, no ffmpeg,
no GPU, no network. What's covered: `run()` folds medium's Comparisons in
(reflagged tier="large") and adds the control-path (launch/alloc RTT) legs
and realapps.py's ffmpeg legs on top, and -- what this file exists to pin
down -- a `baseline` given to `large.run()` reaches ALL THREE layers
(medium's GEMM/bandwidth, the control-path RTT legs, and realapps), each
correctly attributed via `target_label`/`baseline_label`, not just the
realapps legs the way it worked before this task's CLI wiring.

Same FakeMachine/FakeCommand shape as tests/test_medium.py /
tests/test_realapps.py (this project's established fake-Machine pattern);
duplicated here rather than shared, matching those files. The responder
below answers gpu_bench's, mem_bandwidth_probe's, AND `command -v
ffmpeg`/ffmpeg's output/argv shapes all at once, since large.run() invokes
all three probes against the same FakeMachine."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Callable, Optional

from item import Item
from machine import Command, Machine
from result import Status
from tiers import large


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
        return Path("/tmp/fake-large-scratch")


def _everything_ok_responder(
    gflops: float = 100.0,
    h2d: float = 10.0,
    d2d: float = 200.0,
    d2h: float = 8.0,
    launch_us: float = 5.0,
    alloc_us: float = 50.0,
    device: str = "Fake GPU 9000",
    ffmpeg_fps_line: str = "fps= 90",
):
    """Answers gpu_bench (GEMM + RTT Phases B/C in one output),
    mem_bandwidth_probe, `command -v ffmpeg`, and both ffmpeg encode legs
    with a clean PASS-worthy result -- so a single FakeMachine can stand in
    for large.run()'s three separate probes."""

    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
            return 0, b"/usr/bin/ffmpeg\n", b""
        if cmd and cmd[0] == "ffmpeg":
            stderr = f"frame=  150 {ffmpeg_fps_line} q=-1.0 Lsize=N/A time=00:00:05.00 bitrate=N/A speed=1.0x\n".encode()
            return 0, b"", stderr
        # gpu_bench (GEMM + RTT) or mem_bandwidth_probe -- one combined
        # stdout answers whichever regex the caller is about to run.
        stdout = (
            f"device: {device}\n"
            f"A throughput : 1024x1024 GEMM x10  0.500s  {gflops:.1f} GFLOP/s\n"
            f"B launch RTT : 500 launches  0.003s  {launch_us:.2f} us/launch  (166667 launches/s)\n"
            f"C alloc RTT  : 500 alloc+free  0.025s  {alloc_us:.2f} us/pair\n"
            f"H2D: {h2d:.2f} GB/s\nD2D: {d2d:.2f} GB/s\nD2H: {d2h:.2f} GB/s\n"
        )
        return 0, stdout.encode(), b""

    return responder


def _no_gpu_responder():
    def responder(cmd):
        if cmd[0] == "cc":
            return 0, b"", b""
        if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
            return 1, b"", b""  # no ffmpeg either -- keep this machine uniformly GPU/app-absent
        return 1, b"", b"no libcuda\n"

    return responder


# --- degenerate (no baseline) -----------------------------------------------------


def test_run_no_baseline_folds_medium_plus_control_path_plus_realapps():
    async def body():
        machine = FakeMachine(_everything_ok_responder())
        report = await large.run(machine, timeout=5)
        names = {c.name for c in report.results}
        # medium's 4 + control-path's 2 + realapps' 2
        assert names == {
            "medium:gemm_gflops",
            "medium:mem_bandwidth_h2d",
            "medium:mem_bandwidth_d2d",
            "medium:mem_bandwidth_d2h",
            "large:launch_rtt_us",
            "large:alloc_rtt_us",
            "large:ffmpeg_cpu_x264_fps",
            "large:ffmpeg_nvenc_h264_fps",
        }
        for c in report.results:
            assert c.tier == "large"
            assert c.baseline is None

    asyncio.run(body())


def test_control_path_parses_launch_and_alloc_rtt():
    async def body():
        machine = FakeMachine(_everything_ok_responder(launch_us=5.0, alloc_us=50.0))
        comparisons = await large._control_path_comparisons(machine, timeout=5)
        by_name = {c.name: c for c in comparisons}
        assert by_name["large:launch_rtt_us"].target.value == 5.0
        assert by_name["large:alloc_rtt_us"].target.value == 50.0
        for c in comparisons:
            assert c.status == Status.PASS  # well under both tripwires
            assert c.baseline is None

    asyncio.run(body())


# --- baseline reaches every layer, not just realapps -----------------------------


def test_baseline_reaches_medium_control_path_and_realapps():
    async def body():
        target = FakeMachine(_everything_ok_responder(gflops=100.0, h2d=10.0, launch_us=5.0, alloc_us=50.0))
        baseline = FakeMachine(_everything_ok_responder(gflops=100.0, h2d=10.0, launch_us=5.0, alloc_us=50.0))
        report = await large.run(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        by_name = {c.name: c for c in report.results}

        # medium legs
        assert by_name["medium:gemm_gflops"].baseline is not None
        assert by_name["medium:gemm_gflops"].target.label == "guest"
        assert by_name["medium:gemm_gflops"].baseline.label == "host"
        assert by_name["medium:gemm_gflops"].ratio == 1.0

        # control-path legs
        assert by_name["large:launch_rtt_us"].baseline is not None
        assert by_name["large:launch_rtt_us"].ratio == 1.0
        assert by_name["large:launch_rtt_us"].target.label == "guest"
        assert by_name["large:launch_rtt_us"].baseline.label == "host"

        # realapps legs
        assert by_name["large:ffmpeg_cpu_x264_fps"].baseline is not None
        assert by_name["large:ffmpeg_cpu_x264_fps"].target.label == "guest"
        assert by_name["large:ffmpeg_cpu_x264_fps"].baseline.label == "host"

    asyncio.run(body())


def test_control_path_baseline_no_gpu_is_skip_with_reason():
    async def body():
        target = FakeMachine(_everything_ok_responder())
        baseline = FakeMachine(_no_gpu_responder())
        comparisons = await large._control_path_comparisons(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        for c in comparisons:
            assert c.status == Status.SKIP
            assert c.ratio is None
            assert c.baseline.label == "host"
            assert "no libcuda" in c.baseline.detail

    asyncio.run(body())


def test_baseline_no_gpu_no_ffmpeg_skips_everything_gpu_or_app_dependent():
    async def body():
        target = FakeMachine(_everything_ok_responder())
        baseline = FakeMachine(_no_gpu_responder())
        report = await large.run(
            target, timeout=5, baseline=baseline, target_label="guest", baseline_label="host"
        )
        by_name = {c.name: c for c in report.results}
        for name in (
            "medium:gemm_gflops",
            "medium:mem_bandwidth_h2d",
            "large:launch_rtt_us",
            "large:alloc_rtt_us",
            "large:ffmpeg_nvenc_h264_fps",
        ):
            assert by_name[name].status == Status.SKIP, f"{name}: expected SKIP, got {by_name[name].status}"
            assert by_name[name].ratio is None
        # ffmpeg CPU (libx264) doesn't need a GPU, but this baseline has no
        # ffmpeg binary AT ALL either (see _no_gpu_responder) -- also SKIP,
        # never a fabricated ratio against a target-only number.
        assert by_name["large:ffmpeg_cpu_x264_fps"].status == Status.SKIP

    asyncio.run(body())
