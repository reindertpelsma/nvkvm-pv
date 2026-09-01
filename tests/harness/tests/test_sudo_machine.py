"""SudoMachine: run() is wrapped, need()/scratch() deliberately are not.

The second half is the load-bearing assertion. If need() or scratch() ever
start going through sudo, the harness begins producing root-owned caches and
root-owned logs, and every later unprivileged run fails on EACCES for reasons
that have nothing to do with the test. These tests exist to make that
regression impossible to land quietly.

No network, no GPU, no real sudo -- a scripted fake Machine records what it was
asked to do, which is the same pattern tests/test_machine_cli.py uses.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Optional, Sequence

import asyncio

from item import Item
from machine import Command, Machine
from sudo_machine import SudoMachine


class _RecordingCommand(Command):
    """Minimal Command, same shape as tests/test_small.py's FakeCommand."""

    @property
    def start_time(self) -> float:
        return 0.0

    @property
    def returncode(self):
        return 0

    async def wait(self, timeout=None) -> int:
        return 0

    async def kill(self) -> None:
        pass

    @property
    def stdout(self) -> bytes:
        return b""

    @property
    def stderr(self) -> bytes:
        return b""


class _FakeMachine(Machine):
    """Records every call so the wrapper's behaviour is observable."""

    def __init__(self) -> None:
        self.run_calls: List[dict] = []
        self.need_calls: List[Item] = []
        self.scratch_calls = 0

    async def run(self, cmd, *, cwd=None, env=None, stdin=None, timeout=None):
        self.run_calls.append(
            {"cmd": list(cmd), "cwd": cwd, "env": env, "stdin": stdin, "timeout": timeout}
        )
        return _RecordingCommand()

    async def need(self, item):
        self.need_calls.append(item)
        return Path("/unprivileged/cache/thing")

    def scratch(self):
        self.scratch_calls += 1
        return Path("/unprivileged/scratch")


def test_run_is_prefixed_with_sudo_n():
    async def body():
        base = _FakeMachine()
        await SudoMachine(base).run(["whoami"])
        assert base.run_calls[0]["cmd"] == ["sudo", "-n", "whoami"]
    asyncio.run(body())


def test_run_passes_every_other_argument_through_untouched():
    async def body():
        base = _FakeMachine()
        env = {"K": "V"}
        await SudoMachine(base).run(
            ["ls", "-l"], cwd=Path("/tmp"), env=env, stdin=b"in", timeout=12.5
        )
        call = base.run_calls[0]
        assert call["cmd"] == ["sudo", "-n", "ls", "-l"]
        assert call["cwd"] == Path("/tmp")
        assert call["env"] is env
        assert call["stdin"] == b"in"
        assert call["timeout"] == 12.5
    asyncio.run(body())


def test_need_is_NOT_wrapped_so_the_cache_never_becomes_root_owned():
    async def body():
        base = _FakeMachine()
        item = Item(name="x", url="https://example.invalid/x", sha256="0" * 64)
        got = await SudoMachine(base).need(item)
        assert base.need_calls == [item]
        assert got == Path("/unprivileged/cache/thing")
        assert base.run_calls == [], "need() must not shell out through sudo"
    asyncio.run(body())


def test_scratch_is_NOT_wrapped_so_logs_stay_readable_by_their_consumer():
    base = _FakeMachine()
    assert SudoMachine(base).scratch() == Path("/unprivileged/scratch")
    assert base.scratch_calls == 1
    assert base.run_calls == [], "scratch() must not shell out through sudo"


def test_there_is_no_auto_escalation_path():
    async def body():
        """A denied command is NOT retried. Rejected deliberately: retrying a
        non-idempotent command with more privilege can double-apply it, and
        'permission denied' is ambiguous enough that sudo usually is not the fix."""
        base = _FakeMachine()
        await SudoMachine(base).run(["touch", "/root/x"])
        assert len(base.run_calls) == 1, "exactly one attempt, never a retry"
    asyncio.run(body())


def test_the_sudo_prefix_is_configurable_for_targets_without_sudo():
    async def body():
        base = _FakeMachine()
        await SudoMachine(base, sudo=("doas",)).run(["id"])
        assert base.run_calls[0]["cmd"] == ["doas", "id"]
    asyncio.run(body())


def test_it_composes_under_another_wrapper_the_normal_way_round():
    async def body():
        """ChrootMachine(SudoMachine(base)) is the stack that motivated this:
        chroot needs CAP_SYS_ADMIN, and previously just assumed base was root."""
        base = _FakeMachine()
        sudo = SudoMachine(base)
        assert sudo.base is base
        await sudo.run(["mount", "--bind", "/a", "/b"])
        assert base.run_calls[0]["cmd"][:2] == ["sudo", "-n"]
    asyncio.run(body())
