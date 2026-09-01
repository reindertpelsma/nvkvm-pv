"""ThisMachine: run/kill/timeout/stdin-closed-at-launch/scratch. Runs entirely
without a GPU -- everything here uses `echo`, `cat`, `sleep`, `python3`."""

import asyncio
import sys
import tempfile
import time
from pathlib import Path

import pytest

from machine import ThisMachine


def test_run_captures_stdout_and_returncode():
    async def body():
        machine = ThisMachine()
        command = await machine.run(["echo", "hello harness"])
        rc = await command.wait(timeout=5)
        assert rc == 0
        assert command.returncode == 0
        assert command.stdout == b"hello harness\n"

    asyncio.run(body())


def test_nonzero_exit_is_reported_not_swallowed():
    async def body():
        machine = ThisMachine()
        command = await machine.run([sys.executable, "-c", "import sys; sys.exit(7)"])
        rc = await command.wait(timeout=5)
        assert rc == 7
        assert command.returncode == 7

    asyncio.run(body())


def test_start_time_is_a_recent_wall_clock_timestamp():
    async def body():
        before = time.time()
        machine = ThisMachine()
        command = await machine.run(["true"])
        after = time.time()
        assert before <= command.start_time <= after
        await command.wait(timeout=5)

    asyncio.run(body())


def test_stdin_supplied_at_launch_is_closed_so_cat_does_not_hang():
    """The one thing this harness must never do: leave stdin open and hang.
    `cat` with no args reads until EOF; if stdin weren't closed after the
    write, this would hang forever instead of returning quickly."""

    async def body():
        machine = ThisMachine()
        command = await machine.run(["cat"], stdin=b"through the pipe, then EOF")
        rc = await command.wait(timeout=5)
        assert rc == 0
        assert command.stdout == b"through the pipe, then EOF"

    asyncio.run(body())


def test_no_stdin_given_does_not_hang_a_reader():
    """When no stdin bytes are supplied, a command that reads stdin must see
    immediate EOF (DEVNULL), never block waiting for input that will never
    come -- there is no interactive stdin API on Command."""

    async def body():
        machine = ThisMachine()
        command = await machine.run(["cat"])  # no stdin=... given
        rc = await command.wait(timeout=5)
        assert rc == 0
        assert command.stdout == b""

    asyncio.run(body())


def test_timeout_raises_and_kills_the_process():
    async def body():
        machine = ThisMachine()
        command = await machine.run([sys.executable, "-c", "import time; time.sleep(30)"])
        with pytest.raises(asyncio.TimeoutError):
            await command.wait(timeout=0.3)
        # A hang must not be silence: the process must actually be dead, not
        # left running in the background after we've moved on.
        await asyncio.sleep(0.2)
        assert command.returncode is not None

    asyncio.run(body())


def test_timeout_is_idempotent_on_repeated_wait():
    async def body():
        machine = ThisMachine()
        command = await machine.run([sys.executable, "-c", "import time; time.sleep(30)"])
        with pytest.raises(asyncio.TimeoutError):
            await command.wait(timeout=0.2)
        # Calling wait() again after a timeout must not relaunch or hang --
        # it stays timed out.
        with pytest.raises(asyncio.TimeoutError):
            await command.wait(timeout=0.2)

    asyncio.run(body())


def test_kill_is_safe_to_call_after_the_process_already_finished():
    async def body():
        machine = ThisMachine()
        command = await machine.run(["true"])
        await command.wait(timeout=5)
        await command.kill()  # must not raise
        await command.kill()  # and be safe to call twice

    asyncio.run(body())


def test_kill_terminates_a_running_process_directly():
    async def body():
        machine = ThisMachine()
        command = await machine.run([sys.executable, "-c", "import time; time.sleep(30)"])
        await asyncio.sleep(0.1)
        assert command.returncode is None
        await command.kill()
        assert command.returncode is not None

    asyncio.run(body())


def test_scratch_is_memoized_and_writable():
    machine = ThisMachine()
    a = machine.scratch()
    b = machine.scratch()
    assert a == b
    assert a.is_dir()
    probe = a / "write-probe.txt"
    probe.write_text("ok")
    assert probe.read_text() == "ok"


def test_scratch_is_distinct_from_needs_cache(tmp_path):
    machine = ThisMachine(cache_dir=tmp_path / "cache")
    scratch_dir = machine.scratch()
    assert scratch_dir != (tmp_path / "cache")


def test_defaults_never_touch_a_directory_the_caller_did_not_name():
    """ThisMachine is what someone with their own GPU runs directly on a
    machine they care about (see its class docstring) -- with no cache_dir/
    scratch_root given at all, both of its writable locations must land
    under the system temp dir, never under the current working directory,
    the repo tree, or anything else the caller didn't explicitly hand it."""
    tmp_root = Path(tempfile.gettempdir()).resolve()
    machine = ThisMachine()
    assert machine.scratch().resolve().is_relative_to(tmp_root)
    # need()'s cache_dir is private, but its default is set at __init__ time
    # and never touched until a test actually calls need() -- assert the
    # computed default path directly rather than forcing a real fetch.
    assert machine._cache_dir.resolve().is_relative_to(tmp_root)
