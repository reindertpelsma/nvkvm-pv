"""tiers/realapps.py against a scripted fake Machine -- no real ffmpeg, no
GPU, no network. What's covered: the presence-check SKIP, the GPU-absent
stderr classifier (against the EXACT string this sandbox produced when
`ffmpeg -c:v h264_nvenc` was actually run here with no GPU -- see
realapps.py's own docstring), the frame-count-mismatch UNTESTED path, a
clean PASS with fps computed from wall-clock (never from ffmpeg's own
`fps=` field), and that `run()` produces a real two-sided Comparison when a
baseline Machine is given.

`tests/test_result.py` covers `compare()`/`Comparison` generically; this
file is only about realapps.py's own logic layered on top of it. A real
ffmpeg run (both the CPU PASS and the GPU SKIP path) was exercised manually
against this module while writing it -- see the commit message -- this
suite does not re-invoke a real ffmpeg (no such binary should be assumed
present in CI)."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Callable, Optional

import pytest

from item import Item
from machine import Command, Machine
from result import Status
from tiers import realapps


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
    """Scripted by a `responder(cmd) -> (rc, stdout, stderr)` callback, same
    shape as test_vm_machine.py's FakeMachine."""

    def __init__(self, responder: Callable[[list], tuple], *, label: str = "fake"):
        self._responder = responder
        self.calls: list = []
        self.label = label

    async def run(self, cmd, *, cwd=None, env=None, stdin=None, timeout=None):
        self.calls.append(list(cmd))
        rc, out, err = self._responder(list(cmd))
        return FakeCommand(rc, out, err)

    async def need(self, item: Item) -> Path:
        raise NotImplementedError("realapps.py never calls need() -- synthetic lavfi source only")

    def scratch(self) -> Path:
        return Path("/tmp/fake-realapps-scratch")


# The exact ffmpeg h264_nvenc failure text observed in this sandbox (no GPU,
# no libcuda) -- see realapps.py's module docstring. Used verbatim so the
# classifier is tested against a real message, not a synthetic stand-in.
_REAL_NVENC_ABSENT_STDERR = (
    "[h264_nvenc @ 0x5b654f1444c0] Cannot load libcuda.so.1\n"
    "[vost#0:0/h264_nvenc @ 0x5b654f143e80] [enc:h264_nvenc @ 0x5b654f144440] "
    "Error while opening encoder - maybe incorrect parameters such as bit_rate, rate, width or height.\n"
    "[out#0/null @ 0x5b654f143a80] Nothing was written into output file, "
    "because at least one of its streams received no packets.\n"
)


def _ffmpeg_ok_responder(frames: int = realapps.FRAME_COUNT, fps_line: str = "fps=0.0"):
    """A responder that answers `command -v ffmpeg` with success and any
    ffmpeg invocation with a clean rc=0 progress line reporting `frames`
    encoded -- the `fps=0.0` default deliberately mirrors the real "finished
    faster than one progress tick" case this module exists to not trust."""

    def responder(cmd):
        if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
            return 0, b"/usr/bin/ffmpeg\n", b""
        assert cmd[0] == "ffmpeg"
        stderr = f"frame={frames:5d} {fps_line} q=-1.0 Lsize=N/A time=00:00:05.00 bitrate=N/A speed=1.0x\n".encode()
        return 0, b"", stderr

    return responder


def test_absent_binary_skips_both_legs():
    async def body():
        machine = FakeMachine(lambda cmd: (1, b"", b""))  # command -v ffmpeg fails
        report = await realapps.run(machine, timeout=5)
        assert len(report.results) == 2
        for comparison in report.results:
            assert comparison.status == Status.SKIP
            assert "ffmpeg not found" in comparison.target.detail

    asyncio.run(body())


def test_gpu_absent_stderr_classifies_as_skip_not_fail():
    async def body():
        def responder(cmd):
            if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
                return 0, b"/usr/bin/ffmpeg\n", b""
            if "h264_nvenc" in cmd:
                return 255, b"", _REAL_NVENC_ABSENT_STDERR.encode()
            # the CPU/libx264 leg still needs a clean answer for this test to
            # isolate the NVENC leg's classification
            stderr = f"frame={realapps.FRAME_COUNT:5d} fps= 90 q=-1.0\n".encode()
            return 0, b"", stderr

        machine = FakeMachine(responder)
        report = await realapps.run(machine, timeout=5)
        by_name = {c.name: c for c in report.results}
        assert by_name["large:ffmpeg_nvenc_h264_fps"].status == Status.SKIP
        assert "libcuda" in by_name["large:ffmpeg_nvenc_h264_fps"].target.detail
        assert by_name["large:ffmpeg_cpu_x264_fps"].status == Status.PASS

    asyncio.run(body())


def test_nonzero_exit_without_gpu_absent_marker_is_a_real_fail():
    async def body():
        def responder(cmd):
            if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
                return 0, b"/usr/bin/ffmpeg\n", b""
            return 1, b"", b"some unrelated ffmpeg error, not a GPU-absent one\n"

        machine = FakeMachine(responder)
        report = await realapps.run(machine, timeout=5)
        for comparison in report.results:
            assert comparison.status == Status.FAIL

    asyncio.run(body())


def test_short_frame_count_is_untested_not_a_silent_partial_pass():
    async def body():
        def responder(cmd):
            if cmd[:3] == ["sh", "-c", "command -v ffmpeg"]:
                return 0, b"/usr/bin/ffmpeg\n", b""
            # rc==0 but far fewer frames than requested -- e.g. a killed/aborted encode
            return 0, b"", b"frame=   12 fps=0.0 q=-1.0\n"

        machine = FakeMachine(responder)
        report = await realapps.run(machine, timeout=5)
        for comparison in report.results:
            assert comparison.status == Status.UNTESTED
            assert "12" in comparison.target.detail

    asyncio.run(body())


def test_pass_computes_fps_from_wallclock_not_from_ffmpegs_own_zero_field():
    """The realistic case this module exists for: ffmpeg's own progress line
    says `fps=0.0` (finished inside one progress tick) but frames were
    genuinely encoded -- realapps.py must report a real fps computed from
    measured wall-clock, never a fabricated/parroted 0.0."""

    async def body():
        machine = FakeMachine(_ffmpeg_ok_responder(fps_line="fps=0.0"))
        report = await realapps.run(machine, timeout=5)
        cpu = next(c for c in report.results if c.name == "large:ffmpeg_cpu_x264_fps")
        assert cpu.status == Status.PASS
        assert cpu.target.value is not None
        assert cpu.target.value > 0  # never the ffmpeg-reported 0.0
        assert cpu.target.unit == "fps"

    asyncio.run(body())


def test_baseline_produces_a_real_two_sided_comparison():
    async def body():
        target = FakeMachine(_ffmpeg_ok_responder(frames=realapps.FRAME_COUNT, fps_line="fps=100"))
        baseline = FakeMachine(_ffmpeg_ok_responder(frames=realapps.FRAME_COUNT, fps_line="fps=110"))
        report = await realapps.run(target, baseline=baseline, timeout=5)
        cpu = next(c for c in report.results if c.name == "large:ffmpeg_cpu_x264_fps")
        assert cpu.baseline is not None
        assert cpu.target.label == "guest"
        assert cpu.baseline.label == "host"
        assert cpu.ratio is not None  # a real ratio, not a fabricated 1.00x
        assert cpu.status == Status.PASS  # band is None -> any computed ratio is a PASS, never gated on an invented threshold

    asyncio.run(body())


def test_no_baseline_stays_degenerate_and_never_fabricates_a_ratio():
    async def body():
        machine = FakeMachine(_ffmpeg_ok_responder())
        report = await realapps.run(machine, timeout=5)
        for comparison in report.results:
            assert comparison.baseline is None
            assert comparison.ratio is None
            assert comparison.target.label == "this machine"

    asyncio.run(body())


def test_command_v_used_for_presence_never_a_filesystem_check():
    async def body():
        machine = FakeMachine(_ffmpeg_ok_responder())
        await realapps.run(machine, timeout=5)
        assert machine.calls[0] == ["sh", "-c", "command -v ffmpeg"]

    asyncio.run(body())
