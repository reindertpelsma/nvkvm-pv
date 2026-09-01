"""machine_cli.py -- turns run_tests.py's `--target-*`/`--baseline-*` CLI
flags into real `Machine` objects.

THE GAP THIS CLOSES: every tier (small/medium/large) already knows how to
run a workload on a target AND an optional baseline and turn that into a
real host-vs-guest Comparison (see result.py, tiers/*.py) -- but
run_tests.py itself only ever constructed one bare `ThisMachine()` and
never had a flag to name a second (or even a differently-configured first)
machine. "the matrix for passing on host is not special for writing down,
its the host comparison with nvkvm" -- a bare in-guest number is nearly
worthless; this module is what lets `run_tests.py` actually produce the
comparison that matters.

NOT A NEW CONFIG FORMAT: this mirrors the four `Machine` implementations'
own constructors directly (`machine.ThisMachine`, `ssh_machine.SSHMachine`,
`chroot_machine.ChrootMachine`, `vm_machine.VMMachine`) rather than
inventing a spec/JSON/YAML layer above them -- each CLI flag maps 1:1 to a
constructor kwarg. The one piece of composition those classes already
support that a flat CLI can't express directly -- `ChrootMachine(base)` /
`VMMachine(base)` wrapping some OTHER machine -- gets exactly one level of
nesting here (`--target-base ssh --target-base-host ...`), because that is
the realistic case tiers/realapps.py's own docstring names: a chroot and a
VM wrapping the SAME base machine, so both sides exec the byte-identical
binary. Deeper nesting (`ChrootMachine(VMMachine(...))`) is possible in the
library but not reachable from this CLI -- construct it in Python directly
if you need it; that composability is exactly why Machine/Command were
designed the way machine.py's own docstring describes.

ATTRIBUTION IS NOT OPTIONAL: `build_machine()` always returns a
(machine, label) pair. `label` is what ends up on the `Observation` each
tier produces (see result.py: "a ratio with no attribution is not
evidence") -- auto-derived from the kind/host/etc unless the caller passes
`--target-label`/`--baseline-label` to override it.

`--baseline*` is entirely optional (kind defaults to None -- "not
requested"); `--target*` defaults to kind `this` so the zero-flag
invocation (`run_tests.py --tier small`) is unchanged: a bare `ThisMachine`
labelled "this machine", no baseline, exactly like before this module
existed.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from machine import Machine, ThisMachine

# tests/harness/machine_cli.py -> tests/harness -> tests -> repo root. Used as
# ChrootMachine's repo_dir default (that constructor requires one, no
# default of its own -- "an explicit choice, not an accident", same spirit
# as SSHMachine's known_hosts).
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

KINDS = ("this", "ssh", "chroot", "vm")
BASE_KINDS = ("this", "ssh")


class MachineConfigError(ValueError):
    """A --target-*/--baseline-* flag combination that argparse's own
    grammar can't reject (e.g. kind=ssh with no --*-host) -- raised with a
    message meant to be printed directly, not a traceback."""


def add_machine_args(parser: argparse.ArgumentParser, prefix: str, *, help_noun: str, default_kind: Optional[str]) -> None:
    """Registers the full `--{prefix}-*` flag set. `prefix` is `"target"` or
    `"baseline"`; both get the identical flag shape (kind selector, label
    override, ssh/chroot/vm-specific fields, and a one-level-nested `base`
    for chroot/vm) so `run_tests.py --help` shows one consistent pattern
    twice rather than two different ones."""
    g = parser.add_argument_group(f"--{prefix}-* ({help_noun})")
    g.add_argument(
        f"--{prefix}",
        dest=f"{prefix}_kind",
        choices=KINDS,
        default=default_kind,
        help=f"machine kind for the {help_noun} (default: {default_kind!r})",
    )
    g.add_argument(f"--{prefix}-label", default=None, help="override the auto-derived attribution label for this side")
    # ssh -- used directly when kind=ssh, and reused (via --{prefix}-base-*)
    # for a chroot/vm's nested base when --{prefix}-base=ssh.
    g.add_argument(f"--{prefix}-host", help="ssh: hostname/IP to connect to")
    g.add_argument(f"--{prefix}-port", type=int, default=22, help="ssh: port (default: 22)")
    g.add_argument(f"--{prefix}-username", help="ssh: username")
    g.add_argument(f"--{prefix}-password", default=None, help="ssh: password (prefer --%s-identity)" % prefix)
    g.add_argument(f"--{prefix}-identity", default=None, help="ssh: private key file (asyncssh client_keys)")
    g.add_argument(
        f"--{prefix}-known-hosts",
        default=None,
        help="ssh: known_hosts file path, or the literal 'none' to explicitly disable host-key "
        "verification -- required (no default) whenever kind=ssh, same as SSHMachine's own constructor",
    )
    g.add_argument(f"--{prefix}-remote-base", default="/tmp/nvkvm-harness", help="ssh: remote scratch/staging directory")
    # chroot
    g.add_argument(f"--{prefix}-repo-dir", type=Path, default=None, help=f"chroot: repo dir bind-mounted at /repo (default: {PROJECT_ROOT})")
    # vm
    g.add_argument(f"--{prefix}-share-dir", default=None, help="vm: host directory 9p-shared into the guest as /data (see vm_machine.py)")
    g.add_argument(f"--{prefix}-guest-user", default=None, help="vm: guest SSH username (default: vm_machine.GUEST_USER)")
    g.add_argument(f"--{prefix}-guest-password", default=None, help="vm: guest SSH password (default: vm_machine.GUEST_PASSWORD)")
    g.add_argument(f"--{prefix}-guest-port", type=int, default=None, help="vm: host-forwarded guest SSH port (default: vm_machine.GUEST_PORT)")
    # nested base, for chroot/vm only
    g.add_argument(f"--{prefix}-base", dest=f"{prefix}_base_kind", choices=BASE_KINDS, default="this", help="chroot/vm: what the chroot/VM sits on top of")
    g.add_argument(f"--{prefix}-base-host")
    g.add_argument(f"--{prefix}-base-port", type=int, default=22)
    g.add_argument(f"--{prefix}-base-username")
    g.add_argument(f"--{prefix}-base-password", default=None)
    g.add_argument(f"--{prefix}-base-identity", default=None)
    g.add_argument(f"--{prefix}-base-known-hosts", default=None)


def _known_hosts_value(raw: Optional[str], *, flag: str) -> Optional[str]:
    """SSHMachine's own `known_hosts` param has no default -- "an explicit
    choice, not an accident" (its docstring). Mirror that here: the CLI
    caller must pass either a path or the literal 'none', never silently
    fall through to a default."""
    if raw is None:
        raise MachineConfigError(f"{flag} is required when connecting over ssh: a known_hosts file path, or 'none' to explicitly disable host-key verification")
    if raw.strip().lower() == "none":
        return None
    return raw


def _build_ssh(args: argparse.Namespace, prefix: str) -> Machine:
    from ssh_machine import SSHMachine

    host = getattr(args, f"{prefix}_host")
    username = getattr(args, f"{prefix}_username")
    if not host or not username:
        raise MachineConfigError(f"--{prefix} ssh requires --{prefix}-host and --{prefix}-username")
    identity = getattr(args, f"{prefix}_identity")
    known_hosts = _known_hosts_value(getattr(args, f"{prefix}_known_hosts"), flag=f"--{prefix}-known-hosts")
    return SSHMachine(
        host,
        port=getattr(args, f"{prefix}_port"),
        username=username,
        client_keys=[identity] if identity else None,
        known_hosts=known_hosts,
        password=getattr(args, f"{prefix}_password"),
        remote_base=getattr(args, f"{prefix}_remote_base"),
    )


def _ssh_label(args: argparse.Namespace, prefix: str) -> str:
    host = getattr(args, f"{prefix}_host")
    username = getattr(args, f"{prefix}_username")
    port = getattr(args, f"{prefix}_port")
    return f"ssh:{username}@{host}:{port}"


def _build_base(args: argparse.Namespace, prefix: str) -> Tuple[Machine, str]:
    """The one-level-nested base for a chroot/vm machine. Returns
    (machine, label) the same shape as build_machine() itself."""
    base_kind = getattr(args, f"{prefix}_base_kind")
    if base_kind == "this":
        return ThisMachine(), "local"
    if base_kind == "ssh":
        base_prefix = f"{prefix}_base"
        # Reuse the ssh builder/labeler by pointing them at the *_base_*
        # attributes instead of *_host/*_username/etc -- same field names,
        # different attribute prefix, so a tiny adapter namespace lets
        # _build_ssh/_ssh_label work unmodified rather than duplicating them.
        adapter = argparse.Namespace(**{
            f"{base_prefix}_host": getattr(args, f"{prefix}_base_host"),
            f"{base_prefix}_port": getattr(args, f"{prefix}_base_port"),
            f"{base_prefix}_username": getattr(args, f"{prefix}_base_username"),
            f"{base_prefix}_password": getattr(args, f"{prefix}_base_password"),
            f"{base_prefix}_identity": getattr(args, f"{prefix}_base_identity"),
            f"{base_prefix}_known_hosts": getattr(args, f"{prefix}_base_known_hosts"),
            f"{base_prefix}_remote_base": "/tmp/nvkvm-harness",
        })
        return _build_ssh(adapter, base_prefix), _ssh_label(adapter, base_prefix)
    raise MachineConfigError(f"unknown --{prefix}-base kind {base_kind!r}")


def build_machine(args: argparse.Namespace, prefix: str) -> Tuple[Optional[Machine], Optional[str]]:
    """Returns (machine, label). `(None, None)` means this side was not
    requested at all -- only ever valid for `prefix="baseline"` (`--target`
    always defaults to kind `this`, so a target Machine always exists)."""
    kind = getattr(args, f"{prefix}_kind", None)
    if kind is None:
        return None, None

    override_label = getattr(args, f"{prefix}_label")

    if kind == "this":
        return ThisMachine(), override_label or "this machine"

    if kind == "ssh":
        return _build_ssh(args, prefix), override_label or _ssh_label(args, prefix)

    if kind == "chroot":
        from chroot_machine import ChrootMachine

        base, base_label = _build_base(args, prefix)
        repo_dir = getattr(args, f"{prefix}_repo_dir") or PROJECT_ROOT
        machine = ChrootMachine(base, repo_dir=repo_dir)
        return machine, override_label or f"chroot({base_label})"

    if kind == "vm":
        import vm_machine

        base, base_label = _build_base(args, prefix)
        kwargs = {}
        share_dir = getattr(args, f"{prefix}_share_dir")
        if share_dir:
            kwargs["share_dir"] = share_dir
        for field in ("guest_user", "guest_password", "guest_port"):
            value = getattr(args, f"{prefix}_{field}")
            if value:
                kwargs[field] = value
        machine = vm_machine.VMMachine(base, **kwargs)
        return machine, override_label or f"vm({base_label})"

    raise MachineConfigError(f"unknown --{prefix} kind {kind!r}")
