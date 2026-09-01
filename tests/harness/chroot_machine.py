"""ChrootMachine -- runs commands inside a chroot of another Machine's
filesystem, in a private mount namespace so nothing leaks into the host's
mount table. The stepping stone to `ChrootMachine(VMMachine(...))`, where
both sides of a host-vs-guest comparison run byte-identical binaries by
construction rather than by checksum -- that is the property this whole
harness exists to produce, so this is not a convenience wrapper.

ROOTFS: a published Ubuntu base rootfs tarball, fetched and verified as an
ordinary `Item` (URL + sha256) -- NOT debootstrap. Reasons, in order:
  - Not reproducible: `debootstrap noble` gives whatever the mirror holds
    today, so two runs a week apart produce different chroots. For a harness
    whose entire output is a parity COMPARISON, a non-reproducible baseline
    is worthless.
  - Version skew: debootstrapping a release newer than the host commonly
    fails on a missing keyring/release script, because those come from the
    host's own debootstrap package -- and newer guests are exactly what this
    project wants to test.
  - It needs installing, needs root, and takes minutes against one download.
OCI/docker image extraction is the FALLBACK for distros with no rootfs
tarball (Fedora, Arch) -- not implemented in this slice; only the Ubuntu
tarball path is.

need() DOES NOT COPY. It bind-mounts whatever `base.need(item)` already
resolved on the base machine into the chroot, and returns the in-chroot
path. ThisMachine's copy is the special case; this is the normal one. The
mount itself is deferred to each `run()` call (see below) rather than done
once up front.

WHY MOUNTS ARE PER-`run()`, NOT PERSISTENT: this Machine does not keep a
long-lived container process around to hold a standing mount namespace open.
Instead, every `run()` re-does the mount setup as the first steps of ONE
`unshare --mount --propagation private` subprocess tree that then chroots
and execs the caller's command. That gets teardown-on-failure for free: a
mount namespace and everything privately mounted in it are destroyed by the
kernel the instant the last process using it exits -- including on SIGKILL
-- with no trap/cleanup script that itself might not run. The five things
that silently break a chroot, all handled in the generated setup script:
  1. /etc/resolv.conf -- a fresh base rootfs has none; bind-mount the host's,
     or DNS-looking failures get misdiagnosed as network/GPU faults (this
     exact confusion cost an hour on this project and had to be retracted).
  2. Its own /tmp, as tmpfs -- sharing the host's collides concurrent runs'
     scratch files and gets misattributed to the GPU.
  3. /dev, /proc, /sys bind-mounted (recursively for /dev and /sys, which
     picks up /dev/nvidia* with no special-casing needed).
  4. A private mount namespace (`unshare --mount --propagation private`) so
     none of the above leaks into the host's mount table.
  5. Teardown that runs even on failure/kill -- see above; this is structural
     here, not a script that has to remember to run.

The repo directory is bind-mounted WRITABLE at /repo -- build artifacts live
there. `scratch()` is a second writable bind, at /scratch, backed by a real
directory on the base machine (so it persists across this ChrootMachine
instance's calls, the same way `ThisMachine.scratch()` does).

Requires root (CAP_SYS_ADMIN for the mounts, CAP_SYS_CHROOT for chroot(2)) --
checked eagerly; `run()` raises PermissionError with a clear message rather
than failing three mounts deep with a confusing errno.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import time
from pathlib import Path
from typing import Dict, Optional, Sequence

from item import Item
from machine import Command, Machine, spawn_local

UBUNTU_BASE_ROOTFS = Item(
    name="ubuntu-base-24.04.4-base-amd64.tar.gz",
    url="https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/ubuntu-base-24.04.4-base-amd64.tar.gz",
    sha256="c1e67ef7b17a6300e136118bd1dc04725009cb376c1aad10abcf8cd453628d58",
)

EXTRACT_TIMEOUT = 120.0
ITEMS_MOUNT_ROOT = "/nvkvm-items"
REPO_MOUNT_POINT = "/repo"
SCRATCH_MOUNT_POINT = "/scratch"

# Bootstrap env vars consumed by the setup script and stripped before the
# final exec, so they never leak into the chrooted command's environment.
_BOOTSTRAP_ENV_VARS = ("NVKVM_ROOT", "NVKVM_MOUNTPLAN", "NVKVM_CWD")

# Generated once; parameterised entirely through env vars (never string-
# interpolated paths) so nothing here is vulnerable to shell injection via a
# path or command argument -- this project has been burned by exactly that
# class of bug before (see fix/vast-shell-injection in this repo's history).
_SETUP_SH = r"""
set -e
ROOT="$NVKVM_ROOT"
mount --make-rprivate / >/dev/null 2>&1 || true
mount --rbind /dev "$ROOT/dev"
mount -t proc proc "$ROOT/proc"
mount --rbind /sys "$ROOT/sys"
mount -t tmpfs tmpfs "$ROOT/tmp"
if [ -e /etc/resolv.conf ]; then
    : > "$ROOT/etc/resolv.conf" 2>/dev/null || true
    mount --bind /etc/resolv.conf "$ROOT/etc/resolv.conf"
fi
if [ -n "${NVKVM_MOUNTPLAN:-}" ] && [ -f "$NVKVM_MOUNTPLAN" ]; then
    while IFS="$(printf '\t')" read -r src dst ro; do
        [ -z "$src" ] && continue
        if [ -d "$src" ]; then
            mkdir -p "$ROOT$dst"
        else
            mkdir -p "$(dirname "$ROOT$dst")"
            : > "$ROOT$dst" 2>/dev/null || true
        fi
        mount --bind "$src" "$ROOT$dst"
        if [ "$ro" = "ro" ]; then
            mount -o remount,bind,ro "$ROOT$dst"
        fi
    done < "$NVKVM_MOUNTPLAN"
fi
unset NVKVM_ROOT NVKVM_MOUNTPLAN
if [ -n "${NVKVM_CWD:-}" ]; then
    cwd="$NVKVM_CWD"
    unset NVKVM_CWD
    exec chroot "$ROOT" /bin/sh -c 'cd "$1" && shift && exec "$@"' -- "$cwd" "$@"
else
    exec chroot "$ROOT" "$@"
fi
"""


class ChrootMachine(Machine):
    """Runs commands in a chroot of `base`'s filesystem. `base` is typically
    `ThisMachine` today; the design supports `ChrootMachine(VMMachine(...))`
    without changes here -- everything the chroot needs (the rootfs tarball,
    every Item, the writable scratch backing store) is resolved through
    `base.need()` / `base.scratch()`, never assumed to be local."""

    def __init__(self, base: Machine, *, repo_dir: Path, rootfs_item: Item = UBUNTU_BASE_ROOTFS):
        self._base = base
        self._repo_dir = Path(repo_dir).resolve()
        self._rootfs_item = rootfs_item
        self._root_dir: Optional[Path] = None
        self._root_lock = asyncio.Lock()
        self._extra_mounts: Dict[str, Path] = {}  # item.name -> resolved host path (read-only)
        self._chroot_scratch_host: Optional[Path] = None

    @staticmethod
    def preflight_ok() -> Optional[str]:
        """None if this machine can plausibly work here; otherwise a SKIP
        reason. Checked eagerly by `run()`/`_ensure_root()` too, but exposed
        so a tier can preflight without launching anything."""
        if os.geteuid() != 0:
            return "ChrootMachine requires root (CAP_SYS_ADMIN for mounts, CAP_SYS_CHROOT for chroot(2))"
        for tool in ("unshare", "chroot", "mount", "tar"):
            if shutil.which(tool) is None:
                return f"ChrootMachine requires {tool!r} on PATH"
        return None

    async def _ensure_root(self) -> Path:
        if self._root_dir is not None:
            return self._root_dir
        async with self._root_lock:
            if self._root_dir is not None:
                return self._root_dir
            reason = self.preflight_ok()
            if reason:
                raise PermissionError(reason)

            tarball = await self._base.need(self._rootfs_item)
            root_dir = self._base.scratch() / f"chroot-{self._rootfs_item.name.replace('.tar.gz', '')}"
            marker = root_dir / ".nvkvm-extracted"
            if not marker.exists():
                root_dir.mkdir(parents=True, exist_ok=True)
                command = await self._base.run(["tar", "-xzf", str(tarball), "-C", str(root_dir)], timeout=EXTRACT_TIMEOUT)
                rc = await command.wait(timeout=EXTRACT_TIMEOUT)
                if rc != 0:
                    raise RuntimeError(f"extracting {tarball} into {root_dir} failed (rc={rc}): {command.stderr.decode(errors='replace')[-500:]}")
                marker.write_text(f"extracted at {time.time()}\n")
            self._root_dir = root_dir
            return root_dir

    def _mount_plan(self) -> str:
        """Tab-separated `src\\tdst\\tro-or-empty` lines, one per bind mount
        the setup script performs -- never shell-interpolated, always read
        with `read -r` inside the script."""
        lines = [f"{self._repo_dir}\t{REPO_MOUNT_POINT}\t"]  # writable
        if self._chroot_scratch_host is not None:
            lines.append(f"{self._chroot_scratch_host}\t{SCRATCH_MOUNT_POINT}\t")  # writable
        for name, host_path in self._extra_mounts.items():
            lines.append(f"{host_path}\t{ITEMS_MOUNT_ROOT}/{name}\tro")
        return "\n".join(lines) + "\n"

    async def run(
        self,
        cmd: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        stdin: Optional[bytes] = None,
        timeout: Optional[float] = None,
    ) -> Command:
        root_dir = await self._ensure_root()

        plan_dir = self._base.scratch()
        plan_dir.mkdir(parents=True, exist_ok=True)
        plan_path = plan_dir / f"mountplan-{os.getpid()}-{id(self)}.tsv"
        plan_path.write_text(self._mount_plan())

        outer_env = dict(os.environ) if env is None else dict(env)
        outer_env["NVKVM_ROOT"] = str(root_dir)
        outer_env["NVKVM_MOUNTPLAN"] = str(plan_path)
        if cwd is not None:
            outer_env["NVKVM_CWD"] = str(cwd)

        outer_cmd = [
            "unshare",
            "--mount",
            "--propagation",
            "private",
            "--",
            "/bin/bash",
            "-c",
            _SETUP_SH,
            "nvkvm-chroot-entry",
            *[str(c) for c in cmd],
        ]
        # The chroot/mount setup itself runs as a plain local subprocess
        # regardless of what `base` is conceptually -- `unshare`/`mount`/
        # `chroot` are host-local syscalls by nature. What varies by `base`
        # is only how Items/scratch are resolved, above.
        return await spawn_local(outer_cmd, cwd=None, env=outer_env, stdin=stdin, timeout=timeout)

    async def need(self, item: Item) -> Path:
        host_path = await self._base.need(item)
        self._extra_mounts[item.name] = host_path
        return Path(ITEMS_MOUNT_ROOT) / item.name

    def scratch(self) -> Path:
        if self._chroot_scratch_host is None:
            self._chroot_scratch_host = self._base.scratch() / "chroot-scratch"
            self._chroot_scratch_host.mkdir(parents=True, exist_ok=True)
        return Path(SCRATCH_MOUNT_POINT)
