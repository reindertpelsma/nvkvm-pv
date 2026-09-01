"""SudoMachine -- runs commands on another Machine as root, and DELIBERATELY
leaves everything else unprivileged.

Composition, like `ChrootMachine(base)` and `VMMachine(base)`:

    ChrootMachine(SudoMachine(SSHMachine(...)))     "chroot, as root, over ssh"

WHY IT EXISTS. Privilege was an unstated precondition scattered across the
harness. `ChrootMachine`'s own docstring says it "Requires root (CAP_SYS_ADMIN
for the mounts, CAP_SYS_CHROOT for chroot(2))" and then provides no way to
obtain it -- it simply assumes the base Machine is already root.
`preflight.install_missing()` shells out to apt-get with the same assumption.
This makes that requirement explicit and satisfiable instead of implied.

ONLY `run()` IS WRAPPED. `need()` and `scratch()` pass straight through to the
base machine, untouched, and that is the point rather than an oversight:

  - Files placed by `need()` stay owned by the ordinary user. A cache
    populated as root leaves root-owned files in a shared cache directory,
    and the next unprivileged run cannot rewrite or evict them -- the cache
    silently becomes read-only and every later checksum repair fails on
    EACCES rather than on anything to do with the test.
  - `scratch()` and the logs written under it stay writable by the user who
    will read them. A harness that makes its own output root-owned forces
    every consumer -- a log tail, a JSON parse, an artefact copy -- to also
    be root, which spreads the privilege instead of containing it.

So the privilege boundary is exactly one method wide. Everything that
produces a FILE somebody else has to touch afterwards stays unprivileged.

NO AUTO-ESCALATION, BY DESIGN. An earlier sketch retried a command under sudo
whenever it failed with permission denied. That is rejected for three reasons,
and the second is the one that would actually bite:

  1. It escalates silently. A test that quietly ran as root when nobody asked
     for root is a wrong result, not a convenience -- and this harness runs
     against machines it is expected to hand back working.
  2. Retry is unsound for anything non-idempotent. A command that did half its
     work and then hit EPERM gets run AGAIN with more privilege.
     `preflight.install_missing()` is exactly that shape.
  3. "Permission denied" is ambiguous. EACCES comes from a file mode, a
     read-only mount, SELinux, a dropped container capability, or a genuine
     need for root. Sudo fixes only the last; for the rest it re-runs a
     command that fails identically, and now the real cause is buried under
     two failures.

If a tier needs root, say so up front (preflight) and wrap explicitly.

`sudo -n` (non-interactive) is deliberate: a harness must never block on a
hidden password prompt. Without a working sudoers rule this fails immediately
and loudly, which is the correct outcome -- an unattended sweep that hangs on
an invisible prompt is far worse than one that stops.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, Optional, Sequence

from item import Item
from machine import Command, Machine


class SudoMachine(Machine):
    """Wraps `base`, running only `run()` under `sudo -n`."""

    def __init__(self, base: Machine, *, sudo: Sequence[str] = ("sudo", "-n")):
        self._base = base
        self._sudo = list(sudo)

    @property
    def base(self) -> Machine:
        """The wrapped machine. Exposed so a caller can reach the
        unprivileged target deliberately, rather than by unwrapping."""
        return self._base

    async def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        stdin: Optional[bytes] = None,
        timeout: Optional[float] = None,
    ) -> Command:
        # argv prefix only -- no shell, no quoting of our own. Whatever
        # quoting the base transport needs is the base's business, exactly
        # as it is for VMMachine.
        return await self._base.run(
            [*self._sudo, *cmd],
            cwd=cwd,
            env=env,
            stdin=stdin,
            timeout=timeout,
        )

    async def need(self, item: Item) -> Path:
        """Unprivileged, on purpose -- see the module docstring. Inputs are
        placed as the ordinary user so the cache does not become root-owned."""
        return await self._base.need(item)

    def scratch(self) -> Path:
        """Unprivileged, on purpose -- see the module docstring. Output stays
        readable and writable by whoever consumes it."""
        return self._base.scratch()

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"SudoMachine({self._base!r})"
