"""Tier: large's real-application legs -- actual applications, not synthetic
probes, run host vs guest ON THE SAME MACHINE with the SAME binary wherever
the caller composes Machines that way (e.g. `baseline=ChrootMachine(base)`,
`target=VMMachine(base)` -- both wrapping the same `base`, so the ffmpeg
binary each one execs is byte-identical, bind-mounted/virtfs'd from the one
host copy, never copied or reinstalled separately -- see chroot_machine.py's
and vm_machine.py's own module docstrings for why that's the point).

A BARE NUMBER FROM ONE SIDE IS NOT THE DELIVERABLE. result.py's own module
docstring says it plainly: the unit of output is a COMPARISON. `run()` below
takes an optional `baseline` for exactly that reason -- pass one (built by
composing a base Machine the way chroot_machine.py/vm_machine.py describe)
and every leg here becomes a real host-vs-guest ratio instead of a lone
number. `baseline=None` still works -- this module's own tests, and a lone
`ThisMachine` invocation from run_tests.py, exercise that path -- and
degrades to the SAME degenerate, single-sided case result.py already defines
for "no baseline machine configured". It never fabricates a ratio or a
1.00x for the missing side.

Every workload here:
  - checks for the application's presence on `machine` FIRST and returns
    SKIP (not FAIL, not UNTESTED) if it's missing -- a personal box or a
    minimal cloud-image guest not having ffmpeg installed is an expected,
    non-alarming outcome, the same shape as this tier's existing GEMM/
    bandwidth checks SKIPping on "no libcuda" (medium.py's
    `_NO_GPU_MARKERS`).
  - measures its OWN wall-clock duration around the app's exec rather than
    trusting the app's self-reported rate: ffmpeg's own `fps=` progress
    field rounds to `0.0` on a run that finishes inside one progress tick
    (observed directly in this sandbox: a 90-frame 640x480 libx264 encode
    completes in ~0.28s wall, well under ffmpeg's default progress
    interval, and its last printed `fps=` line reads `0.0`) -- reporting
    that value would be a fabricated zero, not a measurement. `frame=N`
    from ffmpeg's own progress output is still cross-checked against the
    frame count asked for, so a short/aborted encode is caught as
    UNTESTED rather than silently scored on partial work.
  - has actually been run at least once (this sandbox, ffmpeg 8.0.1, no
    GPU) before being wired in here: the CPU/libx264 leg PASSes with a real
    fps number, and the GPU/NVENC leg SKIPs on "Cannot load libcuda.so.1"
    -- confirming the presence check and the GPU-absent classifier both
    work, not that NVENC itself has been measured anywhere. No number below
    is invented.

FFMPEG is the one real application wired in so far: broadly present, needs
no downloaded dataset (libavfilter's `testsrc` lavfi source generates the
input in-process, so `need()` isn't even involved), and both a CPU
(libx264) and a GPU (h264_nvenc) codec path exist in the ONE binary --
exactly the "same binary, two code paths" shape this tier wants. This is
deliberately a starting set, not a ceiling -- adding another real
application means: presence-check -> SKIP if absent, run -> measure via
wall-clock (never trust the app's own self-timed rate blindly), classify
GPU-absent stderr -> SKIP, anything else non-zero -> FAIL.
"""

from __future__ import annotations

import asyncio
import re
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from machine import Machine
from result import Comparison, Observation, Report, Status, Workload, compare

DEFAULT_TIMEOUT = 60.0
PRESENCE_TIMEOUT = 10.0
FRAME_SIZE = "1280x720"
FRAME_COUNT = 150  # ~5s of 30fps synthetic source -- long enough that wall-clock isn't dominated by process startup

# Stderr substrings (lowercased match) that mean "this machine structurally
# has no usable GPU/NVENC here", as opposed to a real ffmpeg/harness bug.
# Observed directly (see module docstring): "Cannot load libcuda.so.1" is
# ffmpeg's h264_nvenc failure mode on a machine with no NVIDIA driver/GPU at
# all, which is exactly this sandbox's case and will also be nvkvm's guest
# path if the guest never got CUDA working (see the pre-Turing/Maxwell
# caveat in this project's harness verification notes).
_GPU_ABSENT_MARKERS = (
    "cannot load libcuda",
    "cannot load libnvidia-encode",
    "no nvenc capable devices found",
    "unknown encoder",
    "opensessionex failed",
    "no such file or directory) error loading libnvcuvid",
)


async def _present(machine: Machine, binary: str, *, timeout: float = PRESENCE_TIMEOUT) -> bool:
    """True iff `binary` is on `machine`'s PATH. `command -v` is POSIX and
    behaves identically whether `machine` is ThisMachine, ChrootMachine,
    SSHMachine, or VMMachine -- no need() or filesystem assumption needed."""
    try:
        command = await machine.run(["sh", "-c", f"command -v {binary}"], timeout=timeout)
        rc = await command.wait(timeout=timeout)
    except Exception:
        return False
    return rc == 0


def _classify_gpu_absent(stderr: str) -> bool:
    low = stderr.lower()
    return any(marker in low for marker in _GPU_ABSENT_MARKERS)


async def _ffmpeg_encode(
    machine: Machine, *, codec_args: list, frames: int, timeout: float
) -> Tuple[Optional[Status], Optional[float], str, float]:
    """Runs one ffmpeg synthetic-source encode. Returns
    (status_or_None, fps_or_None, detail, duration_s): status_or_None is
    None on a clean, fully-encoded run (caller builds the PASS Observation);
    otherwise it's the terminal Status this leg should report and fps is
    None."""
    duration_s = frames / 30.0
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "info", "-nostats",
        "-f", "lavfi", "-i", f"testsrc=duration={duration_s + 1}:size={FRAME_SIZE}:rate=30",
        *codec_args,
        "-frames:v", str(frames),
        "-f", "null", "-",
    ]
    t0 = time.monotonic()
    try:
        command = await machine.run(cmd, timeout=timeout)
        rc = await command.wait(timeout=timeout)
    except asyncio.TimeoutError:
        return Status.UNTESTED, None, f"ffmpeg did not finish within {timeout:.0f}s and was killed", time.monotonic() - t0
    wall = time.monotonic() - t0

    stderr = command.stderr.decode(errors="replace")
    stdout = command.stdout.decode(errors="replace")
    if rc != 0:
        if _classify_gpu_absent(stderr):
            return Status.SKIP, None, stderr.strip()[-400:] or "GPU/NVENC not available on this machine", wall
        return Status.FAIL, None, f"ffmpeg exited {rc}: {stderr.strip()[-500:] or '(no stderr)'}", wall

    m = re.findall(r"frame=\s*(\d+)", stderr + stdout)
    encoded = int(m[-1]) if m else 0
    if encoded < frames:
        return (
            Status.UNTESTED,
            None,
            f"ffmpeg exited 0 but only reported {encoded}/{frames} frames encoded; stderr tail: {stderr.strip()[-300:]!r}",
            wall,
        )
    if wall <= 0:
        return Status.UNTESTED, None, "ffmpeg exited 0 in effectively zero measured wall-clock time", wall

    fps = encoded / wall
    return None, fps, f"{encoded} frames @ {fps:.1f} fps ({FRAME_SIZE}, wall-clock measured around the exec)", wall


class _FfmpegWorkload(Workload):
    """Shared shape for both ffmpeg legs: presence-check, then one encode."""

    tier = "large"
    unit = "fps"
    band = None  # no established host-vs-guest reference for ffmpeg yet -- report the ratio, don't gate on an invented threshold
    codec_args: list = []

    async def run(self, machine: Machine, *, timeout: float) -> Observation:
        if not await _present(machine, "ffmpeg"):
            return Observation(status=Status.SKIP, detail="ffmpeg not found on PATH", unit=self.unit)
        status, fps, detail, duration = await _ffmpeg_encode(machine, codec_args=self.codec_args, frames=FRAME_COUNT, timeout=timeout)
        if status is not None:
            return Observation(status=status, detail=detail, unit=self.unit, duration_s=duration)
        return Observation(status=Status.PASS, detail=detail, value=fps, unit=self.unit, duration_s=duration)


class FfmpegCpuX264Workload(_FfmpegWorkload):
    name = "large:ffmpeg_cpu_x264_fps"
    codec_args = ["-c:v", "libx264", "-preset", "veryfast"]


class FfmpegNvencWorkload(_FfmpegWorkload):
    name = "large:ffmpeg_nvenc_h264_fps"
    codec_args = ["-c:v", "h264_nvenc", "-preset", "p1"]


WORKLOADS = (FfmpegCpuX264Workload(), FfmpegNvencWorkload())


async def run(target: Machine, *, baseline: Optional[Machine] = None, timeout: Optional[float] = None) -> Report:
    report = Report(tier="large")
    bound = timeout if timeout is not None else DEFAULT_TIMEOUT
    target_label = "guest" if baseline is not None else "this machine"
    for workload in WORKLOADS:
        comparison = await compare(
            workload, target=target, timeout=bound,
            target_label=target_label, baseline=baseline, baseline_label="host",
        )
        report.add(comparison)
    report.finish()
    return report
