#!/usr/bin/env python3
"""Cheap regressions for the paid sweep's remote GPU-quiesce boundary."""

import importlib.util
import pathlib
import shlex
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "sweep_matrix", ROOT / "scripts" / "sweep_matrix.py")
SWEEP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SWEEP)


def remote_script(fn):
    calls = []

    def fake_sh(cmd, timeout=120, check=False):
        calls.append((cmd, timeout, check))
        return "", 0

    SWEEP.sh = fake_sh
    assert fn("ssh-target") == (True, "")
    assert len(calls) == 1
    argv = shlex.split(calls[0][0])
    assert argv[0] == "ssh-target" and len(argv) == 2
    subprocess.run(["bash", "-n"], input=argv[1], text=True, check=True)
    return argv[1]


quiesce = remote_script(SWEEP.quiesce_gpu_clients)
for required in (
        "systemctl isolate multi-user.target",
        "systemctl stop display-manager.service",
        "loginctl terminate-seat seat0",
        "systemctl stop \"user@${uid}.service\"",
        "fuser -k -TERM /dev/nvidia*",
        "fuser -k -KILL /dev/nvidia*",
        "systemctl is-active --quiet display-manager.service"):
    assert required in quiesce, required

unload = remote_script(SWEEP.unload_nvidia_modules)
for module in ("nvidia_peermem", "nvidia_uvm", "nvidia_drm",
               "nvidia_modeset", "nvidia"):
    assert module in unload
assert "grep -q '^nvidia' /proc/modules" in unload

# A failure at this boundary must be reported as a harness failure, and must
# stop install_driver before it even reads the currently loaded version.
SWEEP.quiesce_gpu_clients = lambda _s: (False, "[HARNESS] still busy")
SWEEP.installed_driver_version = lambda _s: (_ for _ in ()).throw(
    AssertionError("version read after failed quiesce"))
assert SWEEP.install_driver("ssh-target", "580.95.05", "ada", "/tmp/log") == (
    False, "[HARNESS] still busy", None)

# The no-download/preinstalled path must quiesce too, but must not purge or
# unload the driver it is about to validate.
SWEEP.quiesce_gpu_clients = lambda _s: (True, "")
SWEEP.installed_driver_version = lambda _s: "580.95.05"
SWEEP.purge_distro_driver = lambda _s: (_ for _ in ()).throw(
    AssertionError("preinstalled fast path purged the driver"))
SWEEP.unload_nvidia_modules = lambda _s: (_ for _ in ()).throw(
    AssertionError("preinstalled fast path unloaded the driver"))
assert SWEEP.install_driver("ssh-target", "580.95.05", "ada", "/tmp/log") == (
    True, "already installed: 580.95.05 (no download; preinstalled or prior step)",
    "580.95.05")

print("sweep-matrix offline GPU quiesce tests: PASS")
