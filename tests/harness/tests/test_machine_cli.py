"""machine_cli.py -- turning --target-*/--baseline-* argparse flags into
real Machine objects. No network, no GPU: constructing an SSHMachine/
ChrootMachine/VMMachine touches no filesystem/network at all (they connect
lazily, on first real use) -- see machine.py/ssh_machine.py/chroot_machine.py
/vm_machine.py's own __init__ methods. This file only checks that the right
KIND of object comes out, wired to the right constructor arguments, and that
the auto-derived attribution labels are right -- never that a real
connection succeeds (that needs real hardware; see the harness's own README/
commit history for what has and hasn't been verified against one)."""

from __future__ import annotations

import argparse

import pytest

import machine_cli
from chroot_machine import ChrootMachine
from machine import ThisMachine
from ssh_machine import SSHMachine
from vm_machine import GUEST_PASSWORD, GUEST_PORT, GUEST_USER, VMMachine


def _parser():
    p = argparse.ArgumentParser()
    machine_cli.add_machine_args(p, "target", help_noun="target", default_kind="this")
    machine_cli.add_machine_args(p, "baseline", help_noun="baseline", default_kind=None)
    return p


# --- defaults / this --------------------------------------------------------------


def test_target_defaults_to_this_machine_labelled_this_machine():
    args = _parser().parse_args([])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, ThisMachine)
    assert label == "this machine"


def test_baseline_not_requested_by_default_is_none_none():
    args = _parser().parse_args([])
    machine, label = machine_cli.build_machine(args, "baseline")
    assert machine is None
    assert label is None


def test_explicit_this_with_label_override():
    args = _parser().parse_args(["--baseline", "this", "--baseline-label", "the box itself"])
    machine, label = machine_cli.build_machine(args, "baseline")
    assert isinstance(machine, ThisMachine)
    assert label == "the box itself"


# --- ssh ---------------------------------------------------------------------------


def test_ssh_requires_host_and_username():
    args = _parser().parse_args(["--target", "ssh"])
    with pytest.raises(machine_cli.MachineConfigError, match="requires --target-host and --target-username"):
        machine_cli.build_machine(args, "target")


def test_ssh_requires_known_hosts_explicitly():
    args = _parser().parse_args(["--target", "ssh", "--target-host", "box", "--target-username", "root"])
    with pytest.raises(machine_cli.MachineConfigError, match="known_hosts"):
        machine_cli.build_machine(args, "target")


def test_ssh_known_hosts_none_disables_verification():
    args = _parser().parse_args([
        "--target", "ssh", "--target-host", "box", "--target-username", "root", "--target-known-hosts", "none",
    ])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, SSHMachine)
    assert machine._known_hosts is None
    assert label == "ssh:root@box:22"


def test_ssh_known_hosts_path_is_passed_through():
    args = _parser().parse_args([
        "--target", "ssh", "--target-host", "box", "--target-username", "root",
        "--target-known-hosts", "/home/me/.ssh/known_hosts", "--target-port", "2200",
        "--target-identity", "/home/me/.ssh/id_ed25519", "--target-password", "hunter2",
    ])
    machine, label = machine_cli.build_machine(args, "target")
    assert machine._known_hosts == "/home/me/.ssh/known_hosts"
    assert machine._port == 2200
    assert machine._client_keys == ["/home/me/.ssh/id_ed25519"]
    assert machine._password == "hunter2"
    assert label == "ssh:root@box:2200"


def test_ssh_label_override():
    args = _parser().parse_args([
        "--target", "ssh", "--target-host", "box", "--target-username", "root",
        "--target-known-hosts", "none", "--target-label", "the rented box",
    ])
    _, label = machine_cli.build_machine(args, "target")
    assert label == "the rented box"


# --- chroot --------------------------------------------------------------------------


def test_chroot_wraps_this_by_default_and_labels_it():
    args = _parser().parse_args(["--target", "chroot"])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, ChrootMachine)
    assert isinstance(machine._base, ThisMachine)
    assert label == "chroot(local)"
    assert machine._repo_dir == machine_cli.PROJECT_ROOT.resolve()


def test_chroot_repo_dir_override():
    args = _parser().parse_args(["--target", "chroot", "--target-repo-dir", "/somewhere/else"])
    machine, _ = machine_cli.build_machine(args, "target")
    assert str(machine._repo_dir) == "/somewhere/else"


def test_chroot_can_wrap_ssh_base_with_its_own_credentials():
    args = _parser().parse_args([
        "--target", "chroot",
        "--target-base", "ssh", "--target-base-host", "box", "--target-base-username", "root",
        "--target-base-known-hosts", "none", "--target-base-port", "2201",
    ])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, ChrootMachine)
    assert isinstance(machine._base, SSHMachine)
    assert machine._base._host == "box"
    assert machine._base._port == 2201
    assert machine._base._known_hosts is None
    assert label == "chroot(ssh:root@box:2201)"


def test_chroot_base_ssh_requires_known_hosts_too():
    args = _parser().parse_args([
        "--target", "chroot", "--target-base", "ssh", "--target-base-host", "box", "--target-base-username", "root",
    ])
    with pytest.raises(machine_cli.MachineConfigError, match="known_hosts"):
        machine_cli.build_machine(args, "target")


# --- vm ------------------------------------------------------------------------------


def test_vm_wraps_this_by_default_with_library_defaults():
    args = _parser().parse_args(["--target", "vm"])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, VMMachine)
    assert isinstance(machine._base, ThisMachine)
    assert label == "vm(local)"
    assert machine._guest_user == GUEST_USER
    assert machine._guest_password == GUEST_PASSWORD
    assert machine._guest_port == GUEST_PORT


def test_vm_overrides_and_ssh_base_together():
    args = _parser().parse_args([
        "--target", "vm", "--target-guest-user", "poacher", "--target-guest-password", "s3cr3t", "--target-guest-port", "2299",
        "--target-share-dir", "/srv/share",
        "--target-base", "ssh", "--target-base-host", "rented-box", "--target-base-username", "root", "--target-base-known-hosts", "none",
    ])
    machine, label = machine_cli.build_machine(args, "target")
    assert isinstance(machine, VMMachine)
    assert isinstance(machine._base, SSHMachine)
    assert machine._guest_user == "poacher"
    assert machine._guest_password == "s3cr3t"
    assert machine._guest_port == 2299
    assert machine._explicit_share_dir == "/srv/share"
    assert label == "vm(ssh:root@rented-box:22)"


# --- the flagship scenario: target=vm and baseline=chroot on the SAME base -----------


def test_target_vm_and_baseline_chroot_on_the_same_physical_box():
    """tiers/realapps.py's own docstring names this exact composition as
    the point of the whole exercise: both sides wrap the identical ssh base
    (same host/user/known_hosts), so both exec the byte-identical binary."""
    argv = [
        "--target", "vm", "--target-base", "ssh",
        "--target-base-host", "box", "--target-base-username", "root", "--target-base-known-hosts", "none",
        "--baseline", "chroot", "--baseline-base", "ssh",
        "--baseline-base-host", "box", "--baseline-base-username", "root", "--baseline-base-known-hosts", "none",
    ]
    args = _parser().parse_args(argv)
    target, target_label = machine_cli.build_machine(args, "target")
    baseline, baseline_label = machine_cli.build_machine(args, "baseline")
    assert isinstance(target, VMMachine) and isinstance(target._base, SSHMachine)
    assert isinstance(baseline, ChrootMachine) and isinstance(baseline._base, SSHMachine)
    assert target._base._host == baseline._base._host == "box"
    assert target_label == "vm(ssh:root@box:22)"
    assert baseline_label == "chroot(ssh:root@box:22)"


# --- unknown kind -----------------------------------------------------------------


def test_unknown_kind_is_rejected_by_argparse_itself():
    with pytest.raises(SystemExit):
        _parser().parse_args(["--target", "docker"])
