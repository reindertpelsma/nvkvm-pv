"""Execution-target interfaces: `Machine`, `Command`, and the only
implementation for now, `ThisMachine` (runs commands as local subprocesses).

Design contract -- keep this true when SSHMachine / ChrootMachine / VMMachine /
ContainerMachine get added later (not in this slice):

  - `run()` is async. It launches the command and returns a `Command`
    immediately; callers `await` completion explicitly via `Command.wait()`.
  - stdin, if any, is supplied AT LAUNCH ONLY as bytes, written, and closed
    before `run()` returns. There is no interactive stdin API on `Command`.
    This project has been burned twice by leaving stdin open: an unclosed
    `ssh` stdin once silently reduced a 6-driver test matrix to 1 while
    reporting success, and a pipe with an unclosed write end produced a
    phantom `cuInit()` hang that looked like a GPU bug. Closing stdin
    deterministically at launch is the fix, not a missing feature -- do not
    add a way to write to a running Command's stdin later.
  - `need()` and `scratch()` are the only filesystem entry points a test
    should use. `need(item)` resolves a read-only `Item` to a path *on this
    machine*. HOW it gets there is up to the Machine, not the caller: a copy
    (verified against `item.sha256`, as `ThisMachine` does below) is the
    SPECIAL CASE for a machine with its own separate filesystem. The NORMAL
    case, for a Machine layered over another (`ChrootMachine(base)`,
    `VMMachine(base)`), is a bind mount / virtfs mount of whatever
    `base.need(item)` already resolved -- no copy, no re-verification,
    identity holds by construction because it's literally the same bytes.
    Either way the caller gets back a plain path and does not need to know
    which strategy produced it.
  - `scratch()` returns a writable directory that is NOT an Item's cache --
    inputs are read-only, working space is separate, and that split lives in
    the API, not in a comment on call sites.
  - A Machine that wraps another one takes it as a constructor argument
    (`ChrootMachine(base)`, `VMMachine(base)`), not as a special interface
    method -- that's what makes `ChrootMachine(VMMachine(...))` composable
    without touching `Machine`/`Command` themselves.
  - Every subprocess is bound by a timeout. `Command.wait(timeout=...)`
    raises `asyncio.TimeoutError` rather than hanging forever; the caller
    (a tier) turns that into an UNTESTED result. A hang must never look
    like silence -- it must raise.
"""

from __future__ import annotations

import abc
import asyncio
import os
import shutil
import tempfile
import time
from pathlib import Path
from typing import Dict, Optional, Sequence

from item import Item


class CommandDiedWithoutStatus(RuntimeError):
    """Raised by Command.wait() when the underlying process is confirmed to
    no longer be running but no exit status could be recovered for it. This
    is part of the shared Command contract, not an SSHMachine-specific
    detail: any Machine without a resident supervisor over its commands (the
    detached-process design SSHMachine uses, so it survives a dropped SSH
    session) can hit this -- the target rebooted mid-run, or something
    outside this harness's control killed the process before it could write
    its own completion status.

    This is NOT a timeout (the process is not hung -- it is confirmably
    gone) and NOT a normal exit code. It is a third, distinct outcome from
    Command.wait(), matching this harness's UNTESTED-vs-FAIL distinction:
    callers MUST turn this into UNTESTED, never into FAIL and never into a
    fabricated exit code. ThisMachine/ChrootMachine's LocalCommand never
    raises this -- a local child process always yields a real waitpid()
    status, so there is no missing-status case for it to represent."""


class Command(abc.ABC):
    """A running (or finished) command on some Machine."""

    @property
    @abc.abstractmethod
    def start_time(self) -> float:
        """Wall-clock (epoch seconds) timestamp of when the command launched."""

    @property
    @abc.abstractmethod
    def returncode(self) -> Optional[int]:
        """None until the command has finished (or definitively timed out/was killed)."""

    @abc.abstractmethod
    async def wait(self, timeout: Optional[float] = None) -> int:
        """Wait for completion, bounded by `timeout` seconds if given (falls
        back to whatever default `run()` was given, if any -- pass None with
        no default to wait unboundedly, which no tier should ever do).

        May also raise `CommandDiedWithoutStatus` -- see that class -- for a
        Machine whose commands run without a resident supervisor (SSHMachine).
        ThisMachine/ChrootMachine never raise it.

        Raises `asyncio.TimeoutError` if the command has not finished within
        the bound; the process is killed as part of that (see `kill`), never
        left running in the background. Idempotent: calling `wait()` again
        after a timeout re-raises immediately without relaunching anything.
        """

    @abc.abstractmethod
    async def kill(self) -> None:
        """Forcefully terminate the command. Safe to call after the command
        has already finished (a no-op in that case)."""

    @property
    @abc.abstractmethod
    def stdout(self) -> bytes:
        """Captured stdout. Populated once `wait()` has returned or raised;
        empty before that."""

    @property
    @abc.abstractmethod
    def stderr(self) -> bytes:
        """Captured stderr. Same availability rule as `stdout`."""


class Machine(abc.ABC):
    """An execution target: where commands run and where test inputs live."""

    @abc.abstractmethod
    async def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        stdin: Optional[bytes] = None,
        timeout: Optional[float] = None,
    ) -> Command:
        """Launch `cmd` on this machine. `stdin`, if given, is written and
        closed before this coroutine returns -- see the module docstring.
        `timeout`, if given, becomes the default bound used by
        `Command.wait()` when it is called with no explicit timeout."""

    @abc.abstractmethod
    async def need(self, item: Item) -> Path:
        """Materialise `item` on this machine; return its path ON this
        machine. A cache hit is only trusted after re-verifying its
        checksum -- a half-written or since-corrupted file at the expected
        path is never silently reused."""

    @abc.abstractmethod
    def scratch(self) -> Path:
        """A writable scratch directory on this machine, created on first
        use and stable for the lifetime of this Machine instance. Distinct
        from an Item's cache: inputs from `need()` are read-only, this is
        not."""


async def run_to_completion(
    machine: Machine,
    cmd: Sequence[str],
    *,
    timeout: float,
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    stdin: Optional[bytes] = None,
) -> Command:
    """Convenience used by every tier: launch `cmd` and wait for it, bound by
    `timeout`. Lets `asyncio.TimeoutError` propagate -- callers decide how a
    timeout becomes an UNTESTED result; this helper never swallows it."""
    command = await machine.run(cmd, cwd=cwd, env=env, stdin=stdin, timeout=timeout)
    await command.wait(timeout=timeout)
    return command


class LocalCommand(Command):
    def __init__(self, proc: "asyncio.subprocess.Process", start_time: float, default_timeout: Optional[float]):
        self._proc = proc
        self._start_time = start_time
        self._default_timeout = default_timeout
        self._stdout = b""
        self._stderr = b""
        self._waited = False
        self._timed_out = False

    @property
    def start_time(self) -> float:
        return self._start_time

    @property
    def returncode(self) -> Optional[int]:
        return self._proc.returncode

    @property
    def stdout(self) -> bytes:
        return self._stdout

    @property
    def stderr(self) -> bytes:
        return self._stderr

    async def wait(self, timeout: Optional[float] = None) -> int:
        if self._timed_out:
            # Idempotent: a command that already timed out stays timed out.
            raise asyncio.TimeoutError(f"command already timed out (pid was {self._proc.pid})")
        if self._waited:
            return self._proc.returncode

        bound = timeout if timeout is not None else self._default_timeout

        async def _collect():
            async with asyncio.TaskGroup() as tg:
                out_task = tg.create_task(self._proc.stdout.read())
                err_task = tg.create_task(self._proc.stderr.read())
                wait_task = tg.create_task(self._proc.wait())
            return out_task.result(), err_task.result(), wait_task.result()

        try:
            if bound is not None:
                stdout, stderr, rc = await asyncio.wait_for(_collect(), timeout=bound)
            else:
                stdout, stderr, rc = await _collect()
        except asyncio.TimeoutError:
            self._timed_out = True
            await self.kill()
            raise

        self._stdout = stdout
        self._stderr = stderr
        self._waited = True
        return rc

    async def kill(self) -> None:
        if self._proc.returncode is not None:
            return
        try:
            self._proc.kill()
        except ProcessLookupError:
            return
        try:
            await asyncio.wait_for(self._proc.wait(), timeout=10)
        except asyncio.TimeoutError:
            # Even SIGKILL didn't reap it in time (e.g. stuck in D-state on a
            # wedged driver). Nothing more this layer can do; surface it via
            # returncode staying None rather than pretending it's gone.
            pass


async def spawn_local(
    cmd: Sequence[str],
    *,
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    stdin: Optional[bytes] = None,
    timeout: Optional[float] = None,
) -> Command:
    """Launch `cmd` as a local subprocess and return a `LocalCommand`. This is
    the one place stdin-at-launch-then-closed and stdout/stderr capture are
    implemented; `ThisMachine.run()` uses it directly, and `ChrootMachine`
    uses it too for its `unshare`/`chroot` pipeline -- that pipeline is a
    HOST-local syscall sequence regardless of what `ChrootMachine`'s own
    `base` conceptually is, so it does not go through `base.run()`."""
    full_env = dict(os.environ) if env is None else dict(env)
    proc = await asyncio.create_subprocess_exec(
        *[str(c) for c in cmd],
        cwd=str(cwd) if cwd else None,
        env=full_env,
        stdin=asyncio.subprocess.PIPE if stdin is not None else asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    start_time = time.time()
    if stdin is not None:
        # Supplied at launch only, then closed -- see module docstring.
        proc.stdin.write(stdin)
        await proc.stdin.drain()
        proc.stdin.close()
    return LocalCommand(proc, start_time, timeout)


class ThisMachine(Machine):
    """Runs commands as local subprocesses. The only ground-truth (non-
    wrapping) Machine implementation in this slice; SSHMachine/ContainerMachine
    can implement the same two interfaces later without touching call sites.
    ChrootMachine/VMMachine (chroot_machine.py) wrap a base Machine -- often
    this one -- rather than reimplementing local process spawning."""

    def __init__(self, *, cache_dir: Optional[Path] = None, scratch_root: Optional[Path] = None):
        self._cache_dir = Path(cache_dir) if cache_dir else Path(tempfile.gettempdir()) / "nvkvm-harness-cache"
        self._scratch_root = Path(scratch_root) if scratch_root else None
        self._scratch: Optional[Path] = None

    async def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        stdin: Optional[bytes] = None,
        timeout: Optional[float] = None,
    ) -> Command:
        return await spawn_local(cmd, cwd=cwd, env=env, stdin=stdin, timeout=timeout)

    async def need(self, item: Item) -> Path:
        dest = self._cache_dir / item.name
        if item.verify(dest):
            return dest

        self._cache_dir.mkdir(parents=True, exist_ok=True)
        tmp_dest = dest.with_name(f"{dest.name}.fetching-{os.getpid()}")
        if tmp_dest.exists():
            shutil.rmtree(tmp_dest) if tmp_dest.is_dir() else tmp_dest.unlink()

        await asyncio.to_thread(item.fetch_into, tmp_dest)

        if not item.verify(tmp_dest):
            actual = item.describe_actual(tmp_dest)
            if tmp_dest.exists():
                shutil.rmtree(tmp_dest, ignore_errors=True) if tmp_dest.is_dir() else tmp_dest.unlink(missing_ok=True)
            from item import ChecksumMismatch

            raise ChecksumMismatch(item, actual)

        if dest.exists():
            shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
        os.replace(tmp_dest, dest)
        return dest

    def scratch(self) -> Path:
        if self._scratch is None:
            if self._scratch_root:
                self._scratch_root.mkdir(parents=True, exist_ok=True)
                self._scratch = Path(tempfile.mkdtemp(prefix="run-", dir=str(self._scratch_root)))
            else:
                self._scratch = Path(tempfile.mkdtemp(prefix="nvkvm-harness-scratch-"))
        return self._scratch
