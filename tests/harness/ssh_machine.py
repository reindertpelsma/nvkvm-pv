"""SSHMachine -- runs commands on a remote host over SSH (asyncssh).

WHY asyncssh OVER SHELLING OUT TO `ssh`: structured errors (auth failure,
connect timeout, and host-key mismatch are distinct exception types, not
exit-code archaeology), real async streams, and an SFTP subsystem for file
I/O without a shell round trip per read. This is the ONE place in the
harness that pulls in a third-party dependency -- see the import guard right
below. Everything else (the `small` tier in particular) MUST keep working
with zero installs: `pip install asyncssh` (or
`pip install -r tests/harness/requirements.txt`, or a venv -- see that file)
only when SSHMachine is actually used, and importing this module without it
installed raises one clear error at the first real use, not an ImportError
stack trace from somewhere three frames away.

UNTRUSTED TARGET, BAKED IN NOW, NOT BOLTED ON LATER: once this is pointed at
a rented box (not yet -- this slice only proves it against a throwaway local
sshd; see the test suite), that host is adversarial-by-default territory:
  - it gets exactly one throwaway credential for the one connection, nothing
    else is ever placed on it;
  - need() is upload-only (local -> remote); nothing here ever copies
    something back from the target and executes it locally;
  - every string this class hands to a LOCAL subprocess or shell is one this
    class itself generated (uuid-based paths, argv the caller supplied) --
    text the target produced (log bytes, sha256sum/kill output) is read as
    DATA via SFTP or parsed against a fixed vocabulary, never executed,
    never spliced into a command line, local or remote.

DURABILITY -- NO RESIDENT DAEMON ON THE TARGET. This is a settled design;
what follows is the specification, not a proposal:

Every `run()` launches a DETACHED process on the target via one exec call:
  - `setsid` puts it in a new session, so it survives the SSH session dying
    (not `nohup`+`&`+hope -- setsid is what actually detaches it from the
    exec channel's process group).
  - stdout/stderr are redirected into two log files under a per-run
    directory, never left connected to the exec channel's own pipes (a
    background job that inherits those pipes holds the channel open and
    hangs the *launching* exec call waiting for EOF that will never come --
    this is the classic footgun with backgrounding over SSH, guarded
    against explicitly below).
  - the backgrounded pid is written out, along with two markers used later
    to rule out pid reuse (see below).
  - on completion, the exit code is written to a separate `.rc` file --
    never inferred from the log, which carries no exit status -- and that
    write is ATOMIC (write to a `.tmp` sibling, then `mv` over the real
    name), so a poll can only ever observe the file fully absent or fully
    present, never a truncated in-progress write.

The client never holds the exec channel open for the run's duration. It
launches, gets the pid back immediately, and then POLLS: each poll re-reads
the `.rc`/pid/marker files (a few bytes) and pulls whatever's new in the two
logs BY BYTE OFFSET over SFTP. If the SSH connection drops mid-poll, the
next poll transparently reconnects and resumes from the SAME offsets --
resuming a tail needs nothing from the target beyond the log files it was
already writing independently of any particular connection; there is no
session state on either side to lose.

THREE completion states, not two -- tailing a log never yields an exit
status by itself:
  1. the `.rc` file exists            -> finished, with that code.
  2. else the pid is confirmed alive  -> still running.
  3. else                             -> DIED WITHOUT STATUS, raised as
     `CommandDiedWithoutStatus` (defined in machine.py, part of the shared
     Command contract). This is a real third outcome, not a FAIL and not a
     timeout -- exactly the UNTESTED-vs-FAIL distinction already built into
     this harness's result vocabulary. A caller that turns this into
     anything but UNTESTED is the bug, not this class.

     NEVER DECLARED ON THE FIRST OBSERVATION. A process that has just
     finished looks, for a brief window, EXACTLY like one that died without
     writing status: `.rc` hasn't landed yet, and if the kernel recycles
     pids fast enough -- this harness's own test suite, spawning many
     short-lived commands back to back, reproduced it -- `kill -0`/cmdline
     can report the healthy, just-exited process as "gone" or "reused".
     `_STATUS_SH`'s `grace_then_recheck` polls for `.rc` (up to ~2s, in
     100ms steps) before concluding a died-looking process is genuinely
     dead, rather than condemning it on one sample. See
     test_ssh_machine.py's `test_status_check_survives_the_exit_rc_write_race`
     for a deterministic reproduction (the delayed write is injected
     directly, not hoped for via real process timing).

PID REUSE GUARD: targets get rebooted (driver installs do this constantly),
after which a bare `kill -0 $pid` can hit a completely unrelated process
that happened to get the same pid on the new boot. Two checks, both
recorded once at launch, both re-verified on every poll and before every
signal `kill()` sends:
  - the kernel boot id (`/proc/sys/kernel/random/boot_id`) -- an immediate,
    unambiguous "this is not the boot that started your process";
  - a byte-for-byte copy of `/proc/$pid/cmdline`, taken right after
    backgrounding, compared against the live process's current cmdline.
Either mismatch is treated as gone, never as still running, and `kill()`
refuses to signal a pid whose cmdline no longer matches.

THE CMDLINE COMPARE MUST BE `cmp FILE1 FILE2 >/dev/null 2>&1`, NEVER
`cmp -s`. MEASURED against real hardware (a bare-metal Ubuntu 24.04.4 box,
GNU diffutils 3.10, 2026-09-01): `cmp -s /proc/$pid/cmdline recorded_file`
reports "differ" on EVERY call, unconditionally, even for a healthy,
unchanged process whose recorded copy is the literal output of `cat
/proc/$pid/cmdline` moments earlier. Cause: `/proc/$pid/cmdline` always
reports `st_size=0` (every procfs pseudo-file does), and `-s`'s silent mode
takes a stat()-size fast path -- "sizes differ (0 vs N) -> files differ,
skip reading" -- that never fires for two ordinary regular files but always
fires here, so `-s` never actually compares content against a procfs file.
Plain `cmp` (no `-s`) does not take that shortcut (it still has to find and
report *where* two files differ, so it always reads), and was confirmed
correct both ways on the same box: identical content compares equal,
different content compares unequal, either argument order. This bug did
not show up against the throwaway local sshd this module's own tests use
(that environment's diffutils happens not to hit it) -- it was only found
by running this class against a real, separately-hosted target, which is
exactly the gap this project's "not yet proven against a rented box"
caveat (above) was flagging. Every command this class launches against a
target whose `cmp` has this behaviour was misdiagnosed as DIED
"pid-reused" the moment its first status poll landed before `exit.rc` was
written -- i.e. almost always, for anything slower than instant.

STDIN: supplied at launch only (uploaded once, referenced by the detached
process's own redirect) and then never touched again -- same contract, same
reasons, as `ThisMachine`. There is no interactive stdin here either.

need() COPIES (uploads via SFTP) -- unlike `ChrootMachine`'s bind mount,
this is the one case that genuinely has to, alongside `ThisMachine`. There
is no cheaper option once an actual network hop is involved: the bytes have
to cross it. A local materialisation (via a plain `ThisMachine`, verified
the normal way) always happens first, so a bad LOCAL source is caught
before anything is sent, and the upload itself is re-verified on the
target before being moved into the trusted cache path -- the same
"never trust a half-written file" rule as everywhere else in this harness.
"""

from __future__ import annotations

import asyncio
import shlex
import time
import uuid
from pathlib import Path, PurePosixPath
from typing import Dict, Optional, Sequence

from item import ChecksumMismatch, Item, sha256_file
from machine import Command, CommandDiedWithoutStatus, Machine, ThisMachine

try:
    import asyncssh

    _ASYNCSSH_IMPORT_ERROR: Optional[Exception] = None
except ImportError as exc:  # pragma: no cover - exercised by test_ssh_machine's guard test
    asyncssh = None  # type: ignore[assignment]
    _ASYNCSSH_IMPORT_ERROR = exc


def require_asyncssh() -> None:
    if asyncssh is None:
        raise RuntimeError(
            "SSHMachine needs the 'asyncssh' package, which is not installed. "
            "Nothing else in tests/harness requires it -- install it only if you're "
            "using SSHMachine: `pip install asyncssh` or "
            "`pip install -r tests/harness/requirements.txt` (a venv is recommended, "
            "see that file)."
        ) from _ASYNCSSH_IMPORT_ERROR


# --- remote script bodies -----------------------------------------------------
# Every one of these is a CONSTANT string sent as the exec channel's stdin to
# `sh -s -- <args...>`, with all variable data passed as quoted positional
# parameters on the command line (shlex.quote()'d client-side) -- never
# string-interpolated into the script body itself. See the module docstring's
# "never build a command from remote output" rule: these scripts only ever
# consume arguments THIS class generated (uuid-based paths, the caller's own
# argv); nothing the target has previously produced is fed back into one.

_LAUNCH_SH = r"""
set -e
RUNDIR="$1"; CWD="$2"; shift 2
mkdir -p "$RUNDIR"
: > "$RUNDIR/out.log"
: > "$RUNDIR/err.log"
if [ -f /proc/sys/kernel/random/boot_id ]; then
    cat /proc/sys/kernel/random/boot_id > "$RUNDIR/boot_id"
else
    echo unknown > "$RUNDIR/boot_id"
fi
if [ -n "$CWD" ] && [ ! -d "$CWD" ]; then
    echo NVKVM_LAUNCH_FAILED_NO_CWD
    exit 1
fi
IN=/dev/null
[ -f "$RUNDIR/stdin.bin" ] && IN="$RUNDIR/stdin.bin"
setsid sh -c '
    RUNDIR="$1"; CWD="$2"; IN="$3"; shift 3
    if [ -n "$CWD" ]; then cd "$CWD" || exit 121; fi
    "$@" >"$RUNDIR/out.log" 2>"$RUNDIR/err.log" <"$IN" &
    child=$!
    echo "$child" > "$RUNDIR/pid"
    cat "/proc/$child/cmdline" > "$RUNDIR/pid.cmdline" 2>/dev/null
    wait "$child"
    rc=$?
    echo "$rc" > "$RUNDIR/exit.rc.tmp"
    mv "$RUNDIR/exit.rc.tmp" "$RUNDIR/exit.rc"
' nvkvm-inner "$RUNDIR" "$CWD" "$IN" "$@" </dev/null >/dev/null 2>&1 &
i=0
while [ ! -s "$RUNDIR/pid" ] && [ $i -lt 100 ]; do
    i=$((i + 1))
    sleep 0.05
done
if [ -s "$RUNDIR/pid" ]; then
    cat "$RUNDIR/pid"
else
    echo NVKVM_LAUNCH_FAILED_NO_PID
    exit 1
fi
"""

_STATUS_SH = r"""
set -e
RUNDIR="$1"

# The .rc write in _LAUNCH_SH is now atomic (write to .tmp, then mv), so
# "exists" and "complete" are the same fact -- no separate emptiness race to
# worry about here, only the ONE window below.
check_done() {
    if [ -s "$RUNDIR/exit.rc" ]; then
        printf 'DONE %s\n' "$(cat "$RUNDIR/exit.rc")"
        return 0
    fi
    return 1
}

# A process that has JUST exited looks, for a brief window, EXACTLY like one
# that died without ever writing its status: exit.rc has not landed yet, and
# -- once the kernel has recycled the pid fast enough (this harness's own
# test suite spawns many short-lived commands back to back and reproduced
# this) -- kill -0/cmdline can also report "gone" or "reused" for a pid that
# was in fact the healthy process, a heartbeat ago. Declaring DIED on that
# single observation is a false negative that destroys a real, completed
# result. So: never condemn on the first look. Poll for exit.rc a little
# longer (the write is already issued by the time any of this is possible)
# before concluding the process is genuinely gone.
grace_then_recheck() {
    i=0
    while [ $i -lt 20 ]; do
        check_done && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

if check_done; then exit 0; fi

pid=""
[ -f "$RUNDIR/pid" ] && pid=$(cat "$RUNDIR/pid")
if [ -z "$pid" ]; then
    grace_then_recheck && exit 0
    echo "DIED no-pid-recorded"
    exit 0
fi

died_reason=""
expected_boot=unknown
[ -f "$RUNDIR/boot_id" ] && expected_boot=$(cat "$RUNDIR/boot_id")
current_boot=unknown
[ -f /proc/sys/kernel/random/boot_id ] && current_boot=$(cat /proc/sys/kernel/random/boot_id)
if [ "$expected_boot" != "unknown" ] && [ "$expected_boot" != "$current_boot" ]; then
    died_reason="reboot-detected"
elif ! kill -0 "$pid" 2>/dev/null; then
    died_reason="pid-not-alive"
elif [ -s "$RUNDIR/pid.cmdline" ] && ! cmp "/proc/$pid/cmdline" "$RUNDIR/pid.cmdline" >/dev/null 2>&1; then
    died_reason="pid-reused"
fi

if [ -n "$died_reason" ]; then
    grace_then_recheck && exit 0
    echo "DIED $died_reason"
    exit 0
fi

echo RUNNING
"""

_KILL_SH = r"""
set -e
RUNDIR="$1"; SIG="$2"
pid=""
[ -f "$RUNDIR/pid" ] && pid=$(cat "$RUNDIR/pid")
if [ -z "$pid" ]; then
    echo NOPID
    exit 0
fi
if [ -s "$RUNDIR/pid.cmdline" ]; then
    if ! cmp "/proc/$pid/cmdline" "$RUNDIR/pid.cmdline" >/dev/null 2>&1; then
        echo NOTOURS
        exit 0
    fi
fi
if kill "-$SIG" "$pid" 2>/dev/null; then
    echo OK
else
    echo NOSUCHPROC
fi
"""

_VERIFY_SH = r"""
set -e
PATH_="$1"; WANT="$2"
if [ ! -e "$PATH_" ]; then
    echo MISSING
    exit 0
fi
if [ -z "$WANT" ]; then
    echo TRUSTED
    exit 0
fi
got=$(sha256sum -b "$PATH_" 2>/dev/null | awk '{print $1}')
if [ "$got" = "$WANT" ]; then
    echo MATCH
else
    echo "MISMATCH $got"
fi
"""

_MOVE_INTO_PLACE_SH = r"""
set -e
SRC="$1"; DEST="$2"
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
mv "$SRC" "$DEST"
echo OK
"""

_EXTRACT_TAR_SH = r"""
set -e
TARBALL="$1"; DEST="$2"
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TARBALL" -C "$DEST"
rm -f "$TARBALL"
echo OK
"""

_RM_SH = r"""
set -e
rm -rf "$1"
echo OK
"""

POLL_INTERVAL_MIN = 0.25
POLL_INTERVAL_MAX = 2.0
LAUNCH_POLL_TIMEOUT = 15.0
CONTROL_CALL_TIMEOUT = 20.0
RECONNECT_RETRIES = 3

# Connection-shaped exceptions worth retrying-after-reconnect. Deliberately
# NOT catching everything -- a real auth failure or a bug in our own script
# should propagate, not get silently retried forever.
_CONNECTION_ERRORS = (OSError, EOFError, ConnectionError, BrokenPipeError)


class SSHMachine(Machine):
    def __init__(
        self,
        host: str,
        *,
        port: int = 22,
        username: str,
        client_keys: Optional[Sequence] = None,
        known_hosts,  # required, no default: an explicit choice, not an accident.
        password: Optional[str] = None,
        connect_timeout: float = 10.0,
        remote_base: str = "/tmp/nvkvm-harness",
        local_staging: Optional[Machine] = None,
    ):
        require_asyncssh()
        self._host = host
        self._port = port
        self._username = username
        self._client_keys = list(client_keys) if client_keys else None
        self._known_hosts = known_hosts
        self._password = password
        self._connect_timeout = connect_timeout
        self._remote_base = PurePosixPath(remote_base)
        self._local_staging = local_staging or ThisMachine()

        self._conn = None  # asyncssh.SSHClientConnection, lazily (re)created
        self._sftp = None
        self._conn_lock = asyncio.Lock()

    # --- connection lifecycle -------------------------------------------------

    async def _ensure_connection(self, *, force: bool = False):
        async with self._conn_lock:
            if force and self._conn is not None:
                try:
                    self._conn.close()
                except Exception:
                    pass
                self._conn = None
                self._sftp = None
            if self._conn is not None and not self._conn.is_closed():
                return self._conn
            connect_kwargs = dict(
                host=self._host,
                port=self._port,
                username=self._username,
                known_hosts=self._known_hosts,
                connect_timeout=self._connect_timeout,
            )
            if self._client_keys:
                connect_kwargs["client_keys"] = self._client_keys
            if self._password is not None:
                connect_kwargs["password"] = self._password
            self._conn = await asyncssh.connect(**connect_kwargs)
            self._sftp = None
            return self._conn

    async def _ensure_sftp(self):
        conn = await self._ensure_connection()
        if self._sftp is None:
            self._sftp = await conn.start_sftp_client()
        return self._sftp

    async def close(self) -> None:
        async with self._conn_lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except Exception:
                    pass
                self._conn = None
                self._sftp = None

    async def _with_reconnect(self, op, *, retries: int = RECONNECT_RETRIES):
        """Run `op(conn)` (an async callable taking the live connection).
        On a connection-shaped error, force-reconnect and retry, up to
        `retries` times, with a short backoff -- this is the "if ssh drops,
        reconnect and resume" behaviour, applied uniformly to every control
        call this class makes."""
        last_exc: Optional[Exception] = None
        for attempt in range(retries + 1):
            try:
                conn = await self._ensure_connection(force=(attempt > 0))
                return await op(conn)
            except _CONNECTION_ERRORS as exc:
                last_exc = exc
                if attempt < retries:
                    await asyncio.sleep(min(0.5 * (attempt + 1), 3.0))
                    continue
        assert last_exc is not None
        raise ConnectionError(f"lost connection to {self._username}@{self._host}:{self._port} and could not reconnect after {retries} attempt(s): {last_exc}") from last_exc

    # --- remote script helper -------------------------------------------------

    async def _remote_script(self, script: str, args: Sequence[str], *, timeout: float = CONTROL_CALL_TIMEOUT):
        cmdline = "sh -s -- " + " ".join(shlex.quote(str(a)) for a in args)

        async def op(conn):
            return await conn.run(cmdline, input=script, check=False, timeout=timeout, encoding="utf-8")

        return await self._with_reconnect(op)

    async def _read_remote_range(self, path: PurePosixPath, offset: int) -> bytes:
        """Bytes from `path` starting at `offset`, or b"" if the file does
        not exist yet (a run's log file that hasn't been created by the
        target's launch script the instant this is called, or -- in a
        legitimate race -- has already been rotated away)."""

        async def op(conn):
            sftp = await self._ensure_sftp()
            try:
                async with sftp.open(str(path), "rb") as f:
                    await f.seek(offset)
                    return await f.read()
            except (asyncssh.SFTPError, FileNotFoundError):
                return b""

        return await self._with_reconnect(op)

    # --- paths -----------------------------------------------------------------

    def scratch(self) -> Path:
        # Pure path arithmetic -- Machine.scratch() is synchronous, so the
        # actual `mkdir -p` on the target is deferred into whatever async
        # operation next needs the directory (every run()'s launch script
        # mkdir -p's its own rundir; callers that write into scratch() do so
        # via run(), which is async). See the module docstring.
        return Path(str(self._remote_base / "scratch"))

    def _cache_dir(self) -> PurePosixPath:
        return self._remote_base / "cache"

    def _run_dir(self, run_id: str) -> PurePosixPath:
        return self._remote_base / "runs" / run_id

    # --- run() -------------------------------------------------------------------

    async def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        stdin: Optional[bytes] = None,
        timeout: Optional[float] = None,
    ) -> Command:
        if env:
            # The SSH protocol's env-passing (RFC 4254 "env" channel request)
            # is silently dropped by most sshd configs (AcceptEnv is usually
            # empty or narrow) -- relying on it would be a correctness bug
            # that only shows up against a stricter target than whatever this
            # was last tested on. Nothing in this harness needs remote env
            # vars yet; fail loudly instead of silently ignoring them.
            raise NotImplementedError("SSHMachine.run() does not support env= yet (SSH env-passing is unreliable across sshd configs); pass what you need as argv instead")

        run_id = f"run-{uuid.uuid4().hex}"
        rundir = self._run_dir(run_id)

        sftp = await self._ensure_sftp()
        await self._with_reconnect(lambda conn: conn.run(f"mkdir -p {shlex.quote(str(rundir))}", check=True, timeout=CONTROL_CALL_TIMEOUT))
        if stdin is not None:
            async with sftp.open(str(rundir / "stdin.bin"), "wb") as f:
                await f.write(stdin)

        cwd_str = str(cwd) if cwd else ""
        args = [str(rundir), cwd_str, *[str(c) for c in cmd]]
        result = await self._remote_script(_LAUNCH_SH, args, timeout=LAUNCH_POLL_TIMEOUT)
        out = (result.stdout or "").strip()
        if not out.isdigit():
            raise RuntimeError(f"SSHMachine.run(): launch on {self._host} failed: {out!r} (stderr: {(result.stderr or '').strip()[-500:]!r})")
        pid = int(out)

        return SSHCommand(self, rundir=rundir, pid=pid, start_time=time.time(), default_timeout=timeout)

    # --- need() ------------------------------------------------------------------

    async def need(self, item: Item) -> Path:
        # Materialise (and verify) locally FIRST -- a bad local source is
        # caught before a single byte crosses the network. `local_path`'s
        # content is now known-good against item.sha256.
        local_path = await self._local_staging.need(item)
        dest = self._cache_dir() / item.name
        sftp = await self._ensure_sftp()
        await self._with_reconnect(lambda conn: conn.run(f"mkdir -p {shlex.quote(str(self._cache_dir()))}", check=True, timeout=CONTROL_CALL_TIMEOUT))
        tmp_name = f"{item.name}.uploading-{uuid.uuid4().hex}"

        if local_path.is_dir():
            # No cache-hit fast path for directories in this slice: cheaply
            # reverifying an extracted tree's identity against item.sha256
            # would mean reproducing item.py's exact manifest algorithm
            # (sorted relpaths + per-file hashes) in a remote shell script --
            # doable, but a second, easy-to-drift implementation of that
            # algorithm is a worse bet than just always re-uploading a
            # directory Item. Files (the common case -- probe sources etc.)
            # DO get a real cache-hit check below, by re-hashing the actual
            # remote bytes, never a marker.
            import shutil
            import tempfile

            with tempfile.TemporaryDirectory() as td:
                tar_base = Path(td) / tmp_name
                tar_path = Path(shutil.make_archive(str(tar_base), "gztar", root_dir=str(local_path)))
                # TRANSIT integrity: item.sha256 already verified the pre-tar
                # directory locally, above; what could still go wrong is the
                # upload itself, so what's checked remotely is the TAR's own
                # checksum -- computed here, over exactly the bytes about to
                # be sent -- not a re-derivation of item.sha256.
                tar_sha256 = sha256_file(tar_path)
                remote_tar = self._cache_dir() / (tmp_name + ".tar.gz")
                await sftp.put(str(tar_path), str(remote_tar))

            tar_verify = await self._remote_script(_VERIFY_SH, [str(remote_tar), tar_sha256])
            tar_status = (tar_verify.stdout or "").strip()
            if tar_status != "MATCH":
                await self._remote_script(_RM_SH, [str(remote_tar)])
                raise ChecksumMismatch(item, f"tar upload transit check failed: {tar_status}")

            remote_tmp_path = self._cache_dir() / tmp_name
            extract = await self._remote_script(_EXTRACT_TAR_SH, [str(remote_tar), str(remote_tmp_path)])
            if (extract.stdout or "").strip() != "OK":
                raise RuntimeError(f"need({item.name}): remote tar extraction failed: {(extract.stderr or '').strip()[-500:]}")
        else:
            # Files DO get a real cache-hit check: re-hash whatever is
            # already at `dest` on the target and compare to item.sha256 --
            # never trust a marker, exactly like ThisMachine.need().
            verify = await self._remote_script(_VERIFY_SH, [str(dest), item.sha256 or ""])
            if (verify.stdout or "").strip() in ("MATCH", "TRUSTED"):
                return Path(str(dest))

            remote_tmp_path = self._cache_dir() / tmp_name
            await sftp.put(str(local_path), str(remote_tmp_path))

            reverify = await self._remote_script(_VERIFY_SH, [str(remote_tmp_path), item.sha256 or ""])
            rstatus = (reverify.stdout or "").strip()
            if rstatus not in ("MATCH", "TRUSTED"):
                await self._remote_script(_RM_SH, [str(remote_tmp_path)])
                raise ChecksumMismatch(item, rstatus)

        move = await self._remote_script(_MOVE_INTO_PLACE_SH, [str(remote_tmp_path), str(dest)])
        if (move.stdout or "").strip() != "OK":
            raise RuntimeError(f"need({item.name}): could not move upload into place: {(move.stderr or '').strip()[-500:]}")
        return Path(str(dest))


class SSHCommand(Command):
    def __init__(self, machine: SSHMachine, *, rundir: PurePosixPath, pid: int, start_time: float, default_timeout: Optional[float]):
        self._machine = machine
        self._rundir = rundir
        self._pid = pid
        self._start_time = start_time
        self._default_timeout = default_timeout
        self._stdout = b""
        self._stderr = b""
        self._out_offset = 0
        self._err_offset = 0
        self._returncode: Optional[int] = None
        self._resolved = False  # finished (rc known) OR terminally died-without-status
        self._died_reason: Optional[str] = None

    @property
    def start_time(self) -> float:
        return self._start_time

    @property
    def returncode(self) -> Optional[int]:
        return self._returncode

    @property
    def stdout(self) -> bytes:
        return self._stdout

    @property
    def stderr(self) -> bytes:
        return self._stderr

    async def _pull_new_bytes(self) -> None:
        for attr_off, attr_buf, name in (("_out_offset", "_stdout", "out.log"), ("_err_offset", "_stderr", "err.log")):
            offset = getattr(self, attr_off)
            chunk = await self._machine._read_remote_range(self._rundir / name, offset)
            if chunk:
                setattr(self, attr_buf, getattr(self, attr_buf) + chunk)
                setattr(self, attr_off, offset + len(chunk))

    async def _check_status(self) -> tuple:
        result = await self._machine._remote_script(_STATUS_SH, [str(self._rundir)])
        line = (result.stdout or "").strip()
        if line.startswith("DONE"):
            parts = line.split(None, 1)
            code_str = parts[1].strip() if len(parts) > 1 else ""
            if not (code_str.lstrip("-").isdigit()):
                return ("DIED", f"malformed exit code in .rc: {code_str!r}")
            return ("DONE", int(code_str))
        if line.startswith("DIED"):
            parts = line.split(None, 1)
            return ("DIED", parts[1] if len(parts) > 1 else "unknown")
        if line == "RUNNING":
            return ("RUNNING", None)
        return ("DIED", f"unrecognised status line from target: {line!r}")

    async def wait(self, timeout: Optional[float] = None) -> int:
        if self._resolved:
            if self._died_reason is not None:
                raise CommandDiedWithoutStatus(self._died_reason)
            return self._returncode

        bound = timeout if timeout is not None else self._default_timeout
        deadline = (time.monotonic() + bound) if bound is not None else None
        interval = POLL_INTERVAL_MIN

        while True:
            state, payload = await self._check_status()
            await self._pull_new_bytes()

            if state == "DONE":
                self._resolved = True
                self._returncode = payload
                return self._returncode
            if state == "DIED":
                self._resolved = True
                self._died_reason = payload
                raise CommandDiedWithoutStatus(payload)

            if deadline is not None and time.monotonic() >= deadline:
                await self.kill()
                raise asyncio.TimeoutError(f"remote command (pid {self._pid} on {self._machine._host}) did not finish within {bound:.0f}s")

            sleep_for = interval if deadline is None else min(interval, max(0.0, deadline - time.monotonic()))
            await asyncio.sleep(max(sleep_for, 0.0))
            interval = min(interval * 1.5, POLL_INTERVAL_MAX)

    async def kill(self) -> None:
        if self._resolved:
            return
        for sig in ("TERM", "KILL"):
            result = await self._machine._remote_script(_KILL_SH, [str(self._rundir), sig])
            answer = (result.stdout or "").strip()
            if answer in ("NOPID", "NOTOURS", "NOSUCHPROC"):
                return
            await asyncio.sleep(0.3 if sig == "TERM" else 0.0)
