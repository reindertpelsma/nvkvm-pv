"""VMMachine, tested against a scripted fake base Machine -- no real QEMU
guest, no network, no GPU. What's covered here is: the exact sshpass/ssh
argv shape (must match scripts/sweep.sh's `$G=` line byte for byte, since
that is the ONE hop into the guest every sweep run already depends on),
shell-quoting of the wrapped command, and every "fail cleanly" path (no
guest to attach to, no virtfs share to place items into, host reachable but
guest's /data not mounted). What is NOT covered here -- attaching to a real
booted guest, an actual virtfs need() round trip against a real 9p share --
needs a real rented box with QEMU actually running. That verification has
not happened yet and is not invented here; see vm_machine.py's own
docstring and this project's status notes for what remains open."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Callable, Optional

import pytest

import vm_machine as vmm
from vm_machine import GUEST_PASSWORD, GUEST_PORT, GUEST_USER, VMMachine, VMMachineNotAttached
from item import Item
from machine import Command, Machine


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
    """A Machine double whose run() is driven entirely by a caller-supplied
    responder(cmd) -> (rc, stdout, stderr), so tests can script exactly what
    "the host" says to each command VMMachine sends it, without any real
    process or network."""

    def __init__(self, responder: Callable[[list], tuple], *, need_result: Optional[Path] = None):
        self._responder = responder
        self._need_result = need_result
        self.calls: list = []

    async def run(self, cmd, *, cwd=None, env=None, stdin=None, timeout=None):
        self.calls.append(list(cmd))
        rc, out, err = self._responder(list(cmd))
        return FakeCommand(rc, out, err)

    async def need(self, item: Item) -> Path:
        if self._need_result is None:
            raise NotImplementedError("this test's FakeMachine.need() was not expected to be called")
        return self._need_result

    def scratch(self) -> Path:
        return Path("/tmp/fake-base-scratch")


def _default_responder(cmd):
    return (0, b"", b"")


# --- argv shape: must match scripts/sweep.sh's `$G=` line -----------------------


def test_ssh_prefix_matches_sweep_sh_exactly():
    vm = VMMachine(FakeMachine(_default_responder))
    assert vm._ssh_prefix() == [
        "sshpass",
        "-p",
        "ubuntu",
        "ssh",
        "-o",
        "ConnectTimeout=10",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-p",
        "2222",
        "ubuntu@localhost",
    ]
    assert GUEST_USER == "ubuntu" and GUEST_PASSWORD == "ubuntu" and GUEST_PORT == 2222


def test_wrap_for_guest_joins_and_quotes_the_command():
    vm = VMMachine(FakeMachine(_default_responder))
    wrapped = vm._wrap_for_guest(["echo", "hello world", "$(danger)"])
    assert wrapped[:-1] == vm._ssh_prefix()
    assert wrapped[-1] == "echo 'hello world' '$(danger)'"


def test_wrap_for_guest_prefixes_cwd_with_a_clear_failure_on_missing_dir():
    vm = VMMachine(FakeMachine(_default_responder))
    wrapped = vm._wrap_for_guest(["pwd"], cwd="/tmp/some dir")
    assert wrapped[-1] == "cd '/tmp/some dir' || exit 121; pwd"


def test_run_rejects_env_rather_than_silently_dropping_it():
    async def body():
        vm = VMMachine(FakeMachine(lambda cmd: (0, b"MOUNTED\n", b"")))
        with pytest.raises(NotImplementedError, match="env"):
            await vm.run(["true"], env={"X": "1"})

    asyncio.run(body())


def test_scratch_is_a_fixed_guest_path_and_touches_nothing():
    fake = FakeMachine(lambda cmd: (1, b"", b"scratch() must not call run()"))
    vm = VMMachine(fake)
    assert vm.scratch() == Path("/tmp/nvkvm-harness-scratch")
    assert fake.calls == []


# --- attach: fail cleanly, never retry-forever, never try to boot ---------------


def test_ensure_attached_succeeds_when_the_guest_reports_mounted():
    async def body():
        fake = FakeMachine(lambda cmd: (0, b"MOUNTED\n", b""))
        vm = VMMachine(fake)
        await vm._ensure_attached()
        assert vm._attached is True
        assert fake.calls[0][0] == "sshpass"

    asyncio.run(body())


def test_ensure_attached_fails_cleanly_with_no_guest_listening():
    async def body():
        fake = FakeMachine(lambda cmd: (255, b"", b"ssh: connect to host localhost port 2222: Connection refused\n"))
        vm = VMMachine(fake)
        with pytest.raises(VMMachineNotAttached):
            await vm._ensure_attached()
        # bounded retries, not a boot-wait loop
        assert len(fake.calls) == vmm.ATTACH_RETRIES + 1

    asyncio.run(body())


def test_ensure_attached_fails_cleanly_when_repo_share_not_mounted():
    async def body():
        fake = FakeMachine(lambda cmd: (0, b"NOT_MOUNTED\n", b""))
        vm = VMMachine(fake)
        with pytest.raises(VMMachineNotAttached):
            await vm._ensure_attached()

    asyncio.run(body())


def test_ensure_attached_fails_cleanly_when_the_host_itself_is_unreachable():
    async def body():
        class ExplodingMachine(Machine):
            async def run(self, *a, **kw):
                raise ConnectionError("no route to host")

            async def need(self, item):
                raise NotImplementedError

            def scratch(self):
                return Path("/tmp")

        vm = VMMachine(ExplodingMachine())
        with pytest.raises(VMMachineNotAttached):
            await vm._ensure_attached()

    asyncio.run(body())


def test_ensure_attached_is_memoized_on_success():
    async def body():
        fake = FakeMachine(lambda cmd: (0, b"MOUNTED\n", b""))
        vm = VMMachine(fake)
        await vm._ensure_attached()
        calls_after_first = len(fake.calls)
        await vm._ensure_attached()
        assert len(fake.calls) == calls_after_first  # no second round trip

    asyncio.run(body())


# --- need(): virtfs mount, fail cleanly if there's nothing to mount into --------


def test_need_fails_cleanly_with_no_virtfs_share_and_no_override():
    async def body():
        def responder(cmd):
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /mnt/nvkvm" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._FIND_SHARE_DIR_SH:
                return (0, b"", b"")  # no matching qemu process found
            return (1, b"", b"unexpected call")

        fake = FakeMachine(responder)
        vm = VMMachine(fake)
        item = Item(name="x", sha256="a" * 64, url="https://example.invalid/x")
        with pytest.raises(VMMachineNotAttached, match="no virtfs share"):
            await vm.need(item)

    asyncio.run(body())


def test_need_fails_cleanly_when_guest_data_mount_is_absent():
    async def body():
        def responder(cmd):
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /mnt/nvkvm" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._FIND_SHARE_DIR_SH:
                return (0, b"/srv/nvkvm-share\n", b"")
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /data" in cmd[-1]:
                return (0, b"NOT_MOUNTED\n", b"")
            return (1, b"", b"unexpected call")

        fake = FakeMachine(responder)
        vm = VMMachine(fake)
        item = Item(name="x", sha256="a" * 64, url="https://example.invalid/x")
        with pytest.raises(VMMachineNotAttached, match="does not have /data mounted"):
            await vm.need(item)

    asyncio.run(body())


def test_need_places_item_into_the_share_and_returns_the_guest_path():
    async def body():
        placements = []

        def responder(cmd):
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /mnt/nvkvm" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._FIND_SHARE_DIR_SH:
                return (0, b"/srv/nvkvm-share\n", b"")
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /data" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._PLACE_IN_SHARE_SH:
                placements.append(cmd[4:])
                return (0, b"OK\n", b"")
            return (1, b"", f"unexpected call: {cmd!r}".encode())

        fake = FakeMachine(responder, need_result=Path("/root/.cache/nvkvm-harness/cache/x"))
        vm = VMMachine(fake)
        item = Item(name="x", sha256="a" * 64, url="https://example.invalid/x")

        result = await vm.need(item)
        assert result == Path("/data/x")
        assert placements == [["/root/.cache/nvkvm-harness/cache/x", "/srv/nvkvm-share", "x", "a" * 64]]

    asyncio.run(body())


def test_need_uses_an_explicit_share_dir_when_autodiscovery_finds_nothing():
    async def body():
        def responder(cmd):
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /mnt/nvkvm" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._FIND_SHARE_DIR_SH:
                return (0, b"", b"")  # autodiscovery finds nothing
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /data" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._PLACE_IN_SHARE_SH:
                return (0, b"OK\n", b"")
            return (1, b"", f"unexpected call: {cmd!r}".encode())

        fake = FakeMachine(responder, need_result=Path("/x"))
        vm = VMMachine(fake, share_dir="/explicit/share")
        item = Item(name="x", sha256="a" * 64, url="https://example.invalid/x")
        result = await vm.need(item)
        assert result == Path("/data/x")

    asyncio.run(body())


def test_need_surfaces_a_transit_mismatch_as_checksum_mismatch():
    async def body():
        from item import ChecksumMismatch

        def responder(cmd):
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /mnt/nvkvm" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._FIND_SHARE_DIR_SH:
                return (0, b"/srv/nvkvm-share\n", b"")
            if cmd and cmd[0] == "sshpass" and "mountpoint -q /data" in cmd[-1]:
                return (0, b"MOUNTED\n", b"")
            if cmd[:2] == ["sh", "-c"] and cmd[2] == vmm._PLACE_IN_SHARE_SH:
                return (1, b"MISMATCH deadbeef\n", b"")
            return (1, b"", b"unexpected call")

        fake = FakeMachine(responder, need_result=Path("/root/cache/x"))
        vm = VMMachine(fake)
        item = Item(name="x", sha256="a" * 64, url="https://example.invalid/x")
        with pytest.raises(ChecksumMismatch):
            await vm.need(item)

    asyncio.run(body())


# --- run(): delegates entirely to base, adds no transport of its own -----------


def test_run_wraps_the_command_and_delegates_to_base():
    async def body():
        fake = FakeMachine(lambda cmd: (0, b"MOUNTED\nguest output\n", b""))
        vm = VMMachine(fake)
        command = await vm.run(["echo", "hi"])
        rc = await command.wait(timeout=5)
        assert rc == 0
        last = fake.calls[-1]
        assert last[:1] == ["sshpass"]
        assert last[-1] == "echo hi"

    asyncio.run(body())
