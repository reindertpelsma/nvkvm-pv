"""ChrootMachine: preflight gating and the mount-plan format run without root
or a GPU. The end-to-end chroot test needs root + unshare/chroot/tar/mount +
network (to fetch the ~29MB Ubuntu base rootfs once) -- it skips itself
cleanly when any of those aren't available, per this project's "degrade
gracefully" rule, rather than failing a CI box that has none of them."""

import asyncio
from pathlib import Path
from unittest import mock

import pytest

from chroot_machine import ChrootMachine, UBUNTU_BASE_ROOTFS
from item import Item, sha256_file
from machine import ThisMachine


def test_preflight_ok_flags_non_root():
    with mock.patch("chroot_machine.os.geteuid", return_value=1000):
        reason = ChrootMachine.preflight_ok()
    assert reason is not None
    assert "root" in reason


def test_preflight_ok_flags_missing_tool():
    def fake_which(name):
        return None if name == "chroot" else f"/usr/bin/{name}"

    with mock.patch("chroot_machine.os.geteuid", return_value=0), mock.patch("chroot_machine.shutil.which", side_effect=fake_which):
        reason = ChrootMachine.preflight_ok()
    assert reason is not None
    assert "chroot" in reason


def test_preflight_ok_passes_when_root_and_tools_present():
    with mock.patch("chroot_machine.os.geteuid", return_value=0), mock.patch("chroot_machine.shutil.which", return_value="/usr/bin/x"):
        assert ChrootMachine.preflight_ok() is None


def test_rootfs_item_identity_is_a_real_checksum():
    # Not a network test -- just pins that the bootstrap Item follows the
    # same "identity is the checksum" rule as everything else, and that the
    # sha256 recorded here is well-formed (64 hex chars), not a placeholder.
    assert UBUNTU_BASE_ROOTFS.url.endswith(".tar.gz")
    assert len(UBUNTU_BASE_ROOTFS.sha256) == 64
    int(UBUNTU_BASE_ROOTFS.sha256, 16)  # raises if it isn't hex


def test_mount_plan_includes_repo_scratch_and_items(tmp_path):
    base = ThisMachine(cache_dir=tmp_path / "cache", scratch_root=tmp_path / "scratch")
    repo_dir = tmp_path / "repo"
    repo_dir.mkdir()
    cm = ChrootMachine(base, repo_dir=repo_dir)

    # need() and scratch() register mounts without touching the network or
    # requiring root -- they only resolve/create paths on the base machine.
    src = tmp_path / "payload.bin"
    src.write_bytes(b"payload")
    item = Item(name="payload.bin", sha256=sha256_file(src), local_path=src)
    asyncio.run(cm.need(item))
    cm.scratch()

    plan = cm._mount_plan()
    lines = [line for line in plan.splitlines() if line.strip()]
    assert any(line.startswith(f"{repo_dir}\t/repo\t") for line in lines), plan
    assert any("/scratch\t" in line for line in lines), plan
    assert any(line.endswith("/nvkvm-items/payload.bin\tro") for line in lines), plan


def test_need_does_not_copy_the_underlying_file(tmp_path):
    """The whole point: ChrootMachine.need() must not create a second copy of
    the bytes -- it registers the base machine's resolved path for a bind
    mount, and the returned path is the in-chroot mount point, not a new
    file written by ChrootMachine itself."""

    async def body():
        base = ThisMachine(cache_dir=tmp_path / "cache", scratch_root=tmp_path / "scratch")
        cm = ChrootMachine(base, repo_dir=tmp_path / "repo")
        src = tmp_path / "payload.bin"
        src.write_bytes(b"payload")
        item = Item(name="payload.bin", sha256=sha256_file(src), local_path=src)

        in_chroot_path = await cm.need(item)
        assert in_chroot_path == Path("/nvkvm-items/payload.bin")
        # No file was created anywhere by ChrootMachine.need() itself --
        # only the registration dict changed. The actual mount happens
        # per-run(), inside the disposable namespace.
        assert cm._extra_mounts["payload.bin"] == (tmp_path / "cache" / "payload.bin")

    asyncio.run(body())


def _e2e_available() -> bool:
    if ChrootMachine.preflight_ok() is not None:
        return False
    try:
        import urllib.request

        urllib.request.urlopen(UBUNTU_BASE_ROOTFS.url, timeout=5).close()
    except Exception:
        return False
    return True


@pytest.mark.skipif(not _e2e_available(), reason="needs root, unshare/chroot/tar/mount, and network access to the Ubuntu rootfs mirror")
def test_end_to_end_chroot_runs_a_real_command(tmp_path):
    async def body():
        base = ThisMachine(cache_dir=tmp_path / "cache", scratch_root=tmp_path / "scratch")
        repo_dir = tmp_path / "repo"
        repo_dir.mkdir()
        cm = ChrootMachine(base, repo_dir=repo_dir)

        command = await cm.run(["cat", "/etc/os-release"], timeout=120)
        rc = await command.wait(timeout=120)
        assert rc == 0
        assert "Ubuntu" in command.stdout.decode()

        # Writable repo mount: a file written inside the chroot is visible on
        # the host at repo_dir -- same inode via bind mount, not a copy.
        command = await cm.run(["sh", "-c", "echo from-chroot > /repo/e2e-probe.txt"], timeout=30)
        assert await command.wait(timeout=30) == 0
        assert (repo_dir / "e2e-probe.txt").read_text().strip() == "from-chroot"

        # Read-only Items: a need()'d file cannot be written to.
        src = tmp_path / "ro.txt"
        src.write_text("readonly")
        item = Item(name="ro.txt", sha256=sha256_file(src), local_path=src)
        in_chroot = await cm.need(item)
        command = await cm.run(["sh", "-c", f"echo x > {in_chroot}"], timeout=30)
        rc = await command.wait(timeout=30)
        assert rc != 0, "a need()'d Item must be read-only inside the chroot"

    asyncio.run(body())
