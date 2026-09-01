"""SSHMachine, exercised for real against a THROWAWAY local sshd -- real ssh,
real transport, no rented/untrusted hardware. Each test spins up its own
sshd on a free loopback port with a freshly generated, never-reused ed25519
keypair, entirely under pytest's tmp_path; nothing is ever written to
~/.ssh, and the throwaway keys/authorized_keys/sshd_config die with the
tmp_path. Skips itself cleanly (not a failure) when asyncssh isn't
installed or sshd/ssh-keygen/the sftp-server subsystem binary can't be
found, per this project's "degrade gracefully when it's absent" rule --
these tests need a venv with `pip install -r tests/harness/requirements.txt`
(see that file), unlike the rest of the harness.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ssh_machine as sm  # noqa: E402
from item import Item, sha256_file  # noqa: E402
from machine import CommandDiedWithoutStatus  # noqa: E402

SSHD_CANDIDATES = ("/usr/sbin/sshd", "/usr/bin/sshd", "/sbin/sshd")
SFTP_SERVER_CANDIDATES = (
    "/usr/lib/openssh/sftp-server",
    "/usr/libexec/sftp-server",
    "/usr/lib/ssh/sftp-server",
    "/usr/lib/misc/sftp-server",
)


def _find(candidates) -> str:
    for c in candidates:
        if os.path.exists(c) and os.access(c, os.X_OK):
            return c
    found = shutil.which(Path(candidates[0]).name)
    return found or ""


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _e2e_available() -> bool:
    return sm.asyncssh is not None and bool(_find(SSHD_CANDIDATES)) and bool(_find(SFTP_SERVER_CANDIDATES)) and shutil.which("ssh-keygen") is not None


def _kill_tree(root_pid: int) -> set:
    """Kill root_pid and every descendant -- used to simulate a real
    connection drop by severing a specific session's process tree without
    touching an unrelated listener (sshd forks a session subprocess per
    connection; killing only the outer listener does NOT drop an already-
    established connection, which is exactly the trap this project's own
    self-testing hit while building this suite)."""
    out = subprocess.run(["ps", "-eo", "pid,ppid"], capture_output=True, text=True).stdout
    pairs = [tuple(map(int, line.split())) for line in out.splitlines()[1:] if len(line.split()) == 2]
    to_kill, frontier = {root_pid}, {root_pid}
    while frontier:
        nxt = {pid for pid, ppid in pairs if ppid in frontier and pid not in to_kill}
        to_kill |= nxt
        frontier = nxt
    for pid in to_kill:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    return to_kill


class ThrowawaySSHD:
    def __init__(self, tmp_path: Path):
        self.tmp_path = tmp_path
        self.port = _free_port()
        self.client_key = tmp_path / "client_key"
        self.proc: subprocess.Popen = None  # type: ignore[assignment]

    def start(self) -> None:
        # Keys/authorized_keys are generated ONCE and reused across restarts
        # within one test (see sever()+start() in the reconnect test) --
        # regenerating them would just be pointless churn, not a security
        # concern, since the whole throwaway sshd+keypair dies with tmp_path.
        host_key = self.tmp_path / "host_key"
        if not host_key.exists():
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(host_key)], check=True)
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(self.client_key)], check=True)
        authorized = self.tmp_path / "authorized_keys"
        authorized.write_text(self.client_key.with_suffix(".pub").read_text())
        cfg = self.tmp_path / "sshd_config"
        cfg.write_text(
            f"Port {self.port}\n"
            "ListenAddress 127.0.0.1\n"
            f"HostKey {host_key}\n"
            f"AuthorizedKeysFile {authorized}\n"
            "PubkeyAuthentication yes\n"
            "PasswordAuthentication no\n"
            "KbdInteractiveAuthentication no\n"
            "UsePAM no\n"
            "PermitRootLogin yes\n"
            "StrictModes no\n"
            f"PidFile {self.tmp_path}/sshd.pid\n"
            "LogLevel ERROR\n"
            f"Subsystem sftp {_find(SFTP_SERVER_CANDIDATES)}\n"
        )
        self.proc = subprocess.Popen([_find(SSHD_CANDIDATES), "-f", str(cfg), "-D"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        time.sleep(0.3)
        if self.proc.poll() is not None:
            stderr = self.proc.stderr.read().decode(errors="replace")
            pytest.skip(f"throwaway sshd would not start: {stderr}")

    def machine(self, **kwargs) -> "sm.SSHMachine":
        base = dict(
            host="127.0.0.1",
            port=self.port,
            username="root",
            client_keys=[str(self.client_key)],
            known_hosts=None,
            remote_base=str(self.tmp_path / "harness-remote-base"),
        )
        base.update(kwargs)
        return sm.SSHMachine(**base)

    def sever(self) -> None:
        """Kill this connection's process tree (simulating a dropped SSH
        session) WITHOUT killing the listener -- the listener stays up so a
        reconnect can succeed."""
        _kill_tree(self.proc.pid)

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            _kill_tree(self.proc.pid)
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait(timeout=5)


@pytest.fixture
def sshd(tmp_path):
    if not _e2e_available():
        pytest.skip("needs asyncssh installed, plus sshd/ssh-keygen/sftp-server on this box")
    d = ThrowawaySSHD(tmp_path)
    d.start()
    try:
        yield d
    finally:
        d.stop()


# --- import guard (runs regardless of whether asyncssh is installed) ---------


def test_require_asyncssh_raises_a_clear_error_when_absent(monkeypatch):
    monkeypatch.setattr(sm, "asyncssh", None)
    monkeypatch.setattr(sm, "_ASYNCSSH_IMPORT_ERROR", ImportError("simulated"))
    with pytest.raises(RuntimeError, match="asyncssh"):
        sm.require_asyncssh()


# --- basic run/wait ------------------------------------------------------------


def test_run_captures_stdout_and_exit_code(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["echo", "hello over ssh"], timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout == b"hello over ssh\n"

    asyncio.run(body())


def test_nonzero_exit_and_stderr_are_reported(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sh", "-c", "echo oops >&2; exit 3"], timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 3
        assert c.stderr == b"oops\n"

    asyncio.run(body())


def test_stdin_supplied_at_launch_is_closed_so_cat_does_not_hang(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["cat"], stdin=b"through ssh, then EOF", timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout == b"through ssh, then EOF"

    asyncio.run(body())


def test_cwd_is_honoured(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["pwd"], cwd="/tmp", timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout.strip() == b"/tmp"

    asyncio.run(body())


def test_progressive_output_is_tailed_incrementally(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sh", "-c", "echo one; sleep 0.3; echo two; sleep 0.3; echo three"], timeout=20)
        await asyncio.sleep(0.15)
        await c._pull_new_bytes()
        first = c.stdout
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout == b"one\ntwo\nthree\n"
        assert first in c.stdout and first != c.stdout  # something was there early, more came later

    asyncio.run(body())


# --- kill() and timeout --------------------------------------------------------


def test_kill_terminates_a_running_remote_process(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sleep", "30"], timeout=None)
        await asyncio.sleep(0.3)
        await c.kill()
        rc = await c.wait(timeout=10)
        assert rc != 0  # killed by SIGTERM/SIGKILL

    asyncio.run(body())


def test_kill_is_safe_after_natural_completion(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["true"], timeout=20)
        await c.wait(timeout=20)
        await c.kill()  # must not raise
        await c.kill()

    asyncio.run(body())


def test_timeout_raises_and_kills_the_remote_process(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sleep", "30"], timeout=None)
        with pytest.raises(asyncio.TimeoutError):
            await c.wait(timeout=0.6)
        await asyncio.sleep(0.3)
        state, _ = await c._check_status()
        assert state != "RUNNING"

    asyncio.run(body())


# --- the three completion states -----------------------------------------------


def test_status_check_survives_the_exit_rc_write_race(sshd):
    """Deterministic reproduction of the race reported against the full
    suite: a process that has genuinely finished (its pid is confirmably
    dead) but whose exit.rc write has not landed yet must NEVER be reported
    as DIED WITHOUT STATUS. The delayed write is injected directly here --
    not hoped for via real process-exit timing, which is exactly what made
    the original bug pass 4/4 in isolation while flaking in the full suite
    (only the full suite's pid churn made the window observable)."""

    async def body():
        m = sshd.machine()

        # A pid CONFIRMED dead: run a real command to completion, then reuse
        # its (now-exited) pid as the "recorded" pid of a hand-built rundir.
        # No boot_id file is written, so the reboot-detection branch is
        # skipped and the check falls straight to `kill -0`, which fails
        # immediately -- landing exactly in the "looks dead, but did it
        # really die without status" branch this test targets.
        warmup = await m.run(["true"], timeout=20)
        await warmup.wait(timeout=20)
        dead_pid = warmup._pid

        rundir = m._run_dir("run-exit-rc-race")
        await m._with_reconnect(lambda conn: conn.run(f"mkdir -p {rundir}", check=True, timeout=20))
        sftp = await m._ensure_sftp()
        async with sftp.open(str(rundir / "pid"), "w") as f:
            await f.write(str(dead_pid))

        async def delayed_atomic_write_of_exit_rc():
            await asyncio.sleep(0.4)  # well inside the ~2s server-side grace window
            sftp2 = await m._ensure_sftp()
            tmp, final = str(rundir / "exit.rc.tmp"), str(rundir / "exit.rc")
            async with sftp2.open(tmp, "w") as f:
                await f.write("42")
            await sftp2.rename(tmp, final)

        writer = asyncio.create_task(delayed_atomic_write_of_exit_rc())
        cmd = sm.SSHCommand(m, rundir=rundir, pid=dead_pid, start_time=time.time(), default_timeout=None)
        rc = await cmd.wait(timeout=10)  # must NOT raise CommandDiedWithoutStatus
        await writer

        assert rc == 42
        assert cmd.returncode == 42

    asyncio.run(body())


def test_died_without_status_reboot_detected(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sleep", "30"], timeout=None)
        await asyncio.sleep(0.2)
        sftp = await m._ensure_sftp()
        async with sftp.open(str(c._rundir / "boot_id"), "wb") as f:
            await f.write(b"a-different-boot-id\n")
        with pytest.raises(CommandDiedWithoutStatus, match="reboot"):
            await c.wait(timeout=10)
        await c.kill()

    asyncio.run(body())


def test_died_without_status_pid_reused(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sleep", "30"], timeout=None)
        await asyncio.sleep(0.2)
        sftp = await m._ensure_sftp()
        async with sftp.open(str(c._rundir / "pid.cmdline"), "wb") as f:
            await f.write(b"not-the-real-cmdline")
        with pytest.raises(CommandDiedWithoutStatus, match="reused"):
            await c.wait(timeout=10)
        await c.kill()

    asyncio.run(body())


def test_a_genuinely_running_process_is_never_misreported_as_pid_reused(sshd):
    """Regression test for a real bug found against real hardware (a
    bare-metal box, GNU diffutils 3.10, 2026-09-01, not this test's own
    throwaway sshd -- see ssh_machine.py's module docstring, "THE CMDLINE
    COMPARE MUST BE `cmp FILE1 FILE2 ...`, NEVER `cmp -s`"): `cmp -s` took a
    stat()-size fast path against `/proc/$pid/cmdline` (which always reports
    st_size=0) and declared EVERY still-running process "pid-reused",
    unconditionally -- so every SSHMachine command whose first status poll
    landed before its exit.rc was written (i.e. almost all of them, against
    a target slower than instant) was misdiagnosed as DIED. This
    environment's own diffutils does not happen to reproduce that specific
    quirk (confirmed separately: GNU diffutils 3.12 here vs 3.10 on the box
    the bug was found on), so this test does not fail on the pre-fix code in
    THIS environment -- it is here anyway as the direct assertion of the
    invariant the fix restores: a real, unmodified, still-running process's
    cmdline must always compare equal to itself, never DIED."""

    async def body():
        m = sshd.machine()
        c = await m.run(["sleep", "3"], timeout=None)
        # No tampering with pid/pid.cmdline/boot_id at all -- this is exactly
        # the healthy, untouched case. Must resolve as a normal completion,
        # never raise CommandDiedWithoutStatus.
        rc = await c.wait(timeout=15)
        assert rc == 0
        assert c.returncode == 0

    asyncio.run(body())


def test_died_without_status_pid_not_alive_and_no_rc(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sh", "-c", "exit 0"], timeout=None)
        await asyncio.sleep(0.4)  # let it actually finish and write exit.rc
        sftp = await m._ensure_sftp()
        await sftp.remove(str(c._rundir / "exit.rc"))
        with pytest.raises(CommandDiedWithoutStatus, match="not-alive"):
            await c.wait(timeout=10)

    asyncio.run(body())


def test_wait_is_idempotent_after_died_without_status(sshd):
    async def body():
        m = sshd.machine()
        c = await m.run(["sh", "-c", "exit 0"], timeout=None)
        await asyncio.sleep(0.4)
        sftp = await m._ensure_sftp()
        await sftp.remove(str(c._rundir / "exit.rc"))
        with pytest.raises(CommandDiedWithoutStatus):
            await c.wait(timeout=10)
        with pytest.raises(CommandDiedWithoutStatus):
            await c.wait(timeout=10)  # must stay resolved, not re-poll and change its mind

    asyncio.run(body())


# --- need() ----------------------------------------------------------------------


def test_need_uploads_and_reuses_on_cache_hit(sshd, tmp_path):
    async def body():
        m = sshd.machine()
        src = tmp_path / "payload.txt"
        src.write_text("uploaded payload")
        item = Item(name="payload.txt", sha256=sha256_file(src), local_path=src)

        remote_path = await m.need(item)
        c = await m.run(["cat", str(remote_path)], timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout == b"uploaded payload"

        remote_path2 = await m.need(item)
        assert remote_path2 == remote_path

    asyncio.run(body())


def test_need_uploads_a_directory_and_verifies_its_own_upload(sshd, tmp_path):
    async def body():
        m = sshd.machine()
        src_dir = tmp_path / "srcdir"
        src_dir.mkdir()
        (src_dir / "a.txt").write_text("file a")
        (src_dir / "b.txt").write_text("file b")

        from item import sha256_dir

        item = Item(name="srcdir", sha256=sha256_dir(src_dir), local_path=src_dir)
        remote_path = await m.need(item)
        c = await m.run(["sh", "-c", f"cat {remote_path}/a.txt {remote_path}/b.txt"], timeout=20)
        rc = await c.wait(timeout=20)
        assert rc == 0
        assert c.stdout == b"file afile b"

    asyncio.run(body())


def test_need_rejects_a_corrupt_local_directory_source(sshd, tmp_path):
    async def body():
        m = sshd.machine()
        src_dir = tmp_path / "srcdir"
        src_dir.mkdir()
        (src_dir / "a.txt").write_text("file a")
        bad_item = Item(name="srcdir", sha256="1" * 64, local_path=src_dir)
        with pytest.raises(Exception):
            await m.need(bad_item)

    asyncio.run(body())


def test_need_rejects_a_corrupt_local_source(sshd, tmp_path):
    async def body():
        m = sshd.machine()
        src = tmp_path / "payload.txt"
        src.write_text("real content")
        bad_item = Item(name="payload.txt", sha256="0" * 64, local_path=src)
        with pytest.raises(Exception):
            await m.need(bad_item)

    asyncio.run(body())


# --- reconnect-and-resume ------------------------------------------------------
# The hard one to exercise: severs the actual established connection (not
# just the listener -- see _kill_tree's docstring) mid-command, confirms the
# reconnect path is actually taken (not just "it happened to still work"),
# and confirms every byte the detached remote process wrote across the whole
# outage -- before, during, and after -- was recovered with none lost or
# duplicated.


def test_reconnect_and_resume_after_a_dropped_connection(sshd, monkeypatch):
    async def body():
        m = sshd.machine()

        reconnect_count = 0
        orig = sm.SSHMachine._ensure_connection

        async def traced(self, *, force=False):
            nonlocal reconnect_count
            if force:
                reconnect_count += 1
            return await orig(self, force=force)

        monkeypatch.setattr(sm.SSHMachine, "_ensure_connection", traced)

        c = await m.run(["sh", "-c", 'i=0; while [ $i -lt 10 ]; do echo "line $i"; i=$((i+1)); sleep 0.3; done'], timeout=None)
        task = asyncio.create_task(c.wait(timeout=30))

        await asyncio.sleep(0.8)
        sshd.sever()
        await asyncio.sleep(1.2)  # outage window: reconnect attempts should fail and retry here
        sshd.start()  # a fresh listener on the SAME port -- the client should pick it back up

        rc = await task
        assert rc == 0
        expected = "".join(f"line {i}\n" for i in range(10)).encode()
        assert c.stdout == expected, (c.stdout, expected)
        assert reconnect_count > 0, "the drop was never actually exercised -- test is not proving what it claims to"

    asyncio.run(body())
