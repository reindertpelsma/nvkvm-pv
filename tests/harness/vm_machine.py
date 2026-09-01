"""VMMachine -- attaches to an ALREADY-BOOTED nvkvm guest VM reached through a
base Machine (in practice `SSHMachine(host)`, an SSH connection to the
physical/rented box running QEMU), via the exact hop scripts/sweep.sh uses to
reach the guest (see its `$G=` line): `sshpass -p ubuntu ssh ... -p 2222
ubuntu@localhost`, port 2222 being QEMU's own forward of the guest's SSH port
22, "ubuntu"/"ubuntu" being the stock cloud-image credential
scripts/setup_guest.sh's cloud-init seed sets. Nothing here manages the VM's
lifecycle: booting, provisioning, and driver staging are
scripts/run_test_vm.sh's and scripts/sweep.sh's job, run separately, outside
this harness. VMMachine's entire contract is ATTACH to what is already
running, and FAIL CLEANLY AND IMMEDIATELY if there is nothing to attach to
-- never retry-forever, never try to boot or fix one. (scripts/sweep.sh's own
30x20s boot-wait loop is a BOOT operation; this is explicitly not that.)

COMPOSITION IS THE WHOLE POINT: VMMachine.run() does not open a second SSH
connection or reimplement any of the detached-process/log-tail/reconnect
durability machinery SSHMachine already has. It builds the
`sshpass ssh ... -p 2222 ubuntu@localhost <cmd>` argv and hands it to
`base.run()` -- the guest-reaching hop just becomes the "detached process"
SSHMachine's own wrapper (setsid, logged, polled) manages on the HOST. If the
connection to the HOST drops mid-run, SSHMachine's own reconnect-and-resume
already covers it; VMMachine adds nothing and duplicates nothing. This is
exactly why Machine/Command were designed to compose (`ChrootMachine(base)`,
here `VMMachine(base)`) rather than each implementation owning its own
transport.

need() IS A VIRTFS MOUNT, NOT A COPY INTO THE GUEST -- the second deliberate
exception alongside ThisMachine/SSHMachine, but pointing the other way: an
Item is materialised (verified) on the HOST via `base.need()`, exactly once,
then placed with a single HOST-LOCAL copy into the directory QEMU is already
sharing into the guest read-write over 9p (mount_tag=nvkvm_data, guest
`/data` -- see scripts/run_test_vm.sh's SHARE_ARG and
scripts/setup_guest.sh's fstab entry for that tag). No second network hop
into the guest, no SFTP over the nested sshpass connection: the guest sees
the file because it was already watching that host directory over 9p, the
same shape as ChrootMachine's bind mount. This only works if the guest was
booted with `NVKVM_SHARE_DIR` set (`SHARE_ARG` in run_test_vm.sh is
OPTIONAL); if the /data share isn't there, need() fails cleanly rather than
falling back to a second SFTP hop through the guest connection, which would
quietly defeat the entire "mount, not copy" point of this class.
"""

from __future__ import annotations

import shlex
from pathlib import Path, PurePosixPath
from typing import Dict, Optional, Sequence

from item import ChecksumMismatch, Item
from machine import Command, Machine

GUEST_USER = "ubuntu"
GUEST_PASSWORD = "ubuntu"  # the stock cloud-image credential; see scripts/setup_guest.sh's cloud-init seed
GUEST_PORT = 2222  # QEMU's host-side forward of the guest's SSH port 22 -- see scripts/run_test_vm.sh
GUEST_SHARE_MOUNT = PurePosixPath("/data")  # mount_tag=nvkvm_data, per scripts/setup_guest.sh's fstab entry
GUEST_REPO_MOUNT = PurePosixPath("/mnt/nvkvm")  # mount_tag=nvkvm_src, READ-ONLY -- readiness signal, not a place to write

ATTACH_CHECK_TIMEOUT = 15.0
ATTACH_RETRIES = 2  # a couple of quick retries for a transient hiccup; NOT a boot-wait loop
CONTROL_TIMEOUT = 30.0

# Same env-passing caveat as SSHMachine: most sshd configs silently drop it,
# and the guest's sshd is whatever the cloud image ships, entirely outside
# this project's control -- do not pretend to support it.
_ENV_UNSUPPORTED_MSG = "VMMachine.run() does not support env= (SSH env-passing is unreliable, and the guest's sshd config is not under this harness's control); pass what you need as argv instead"

_FIND_SHARE_DIR_SH = r"""
set -e
ps -eo args= 2>/dev/null | grep -o -- '-virtfs local,path=[^,]*,mount_tag=nvkvm_data[^ ]*' | head -1 \
    | sed -n 's/.*path=\([^,]*\),mount_tag=nvkvm_data.*/\1/p'
"""

_PLACE_IN_SHARE_SH = r"""
set -e
SRC="$1"; SHARE_DIR="$2"; NAME="$3"; WANT="$4"
DEST="$SHARE_DIR/$NAME"
if [ -e "$DEST" ]; then
    if [ -z "$WANT" ]; then
        echo REUSED
        exit 0
    fi
    if [ ! -d "$DEST" ]; then
        got=$(sha256sum -b "$DEST" 2>/dev/null | awk '{print $1}')
        if [ "$got" = "$WANT" ]; then
            echo REUSED
            exit 0
        fi
    fi
fi
TMP="$SHARE_DIR/.${NAME}.placing-$$"
rm -rf "$TMP"
cp -a "$SRC" "$TMP"
if [ -n "$WANT" ] && [ ! -d "$TMP" ]; then
    got=$(sha256sum -b "$TMP" 2>/dev/null | awk '{print $1}')
    if [ "$got" != "$WANT" ]; then
        rm -rf "$TMP"
        echo "MISMATCH $got"
        exit 1
    fi
fi
rm -rf "$DEST"
mv "$TMP" "$DEST"
echo OK
"""


class VMMachineNotAttached(RuntimeError):
    """Raised when there is no already-booted guest to attach to -- the
    sshpass hop into the guest failed, or the guest is up but its 9p repo
    export (`/mnt/nvkvm`, mount_tag=nvkvm_src) isn't mounted. VMMachine never
    tries to boot or repair anything in response to this; it is the "fail
    cleanly" half of "attach to an already-booted guest and fail cleanly if
    there is not one"."""


class VMMachine(Machine):
    def __init__(self, base: Machine, *, share_dir: Optional[str] = None, guest_user: str = GUEST_USER, guest_password: str = GUEST_PASSWORD, guest_port: int = GUEST_PORT):
        self._base = base
        self._explicit_share_dir = share_dir
        self._guest_user = guest_user
        self._guest_password = guest_password
        self._guest_port = guest_port
        self._attached = False
        self._share_dir: Optional[str] = None  # discovered/confirmed lazily
        self._scratch_ensured = False

    # --- the guest hop -------------------------------------------------------

    def _ssh_prefix(self) -> list:
        return [
            "sshpass",
            "-p",
            self._guest_password,
            "ssh",
            "-o",
            "ConnectTimeout=10",
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
            "-p",
            str(self._guest_port),
            f"{self._guest_user}@localhost",
        ]

    def _wrap_for_guest(self, cmd: Sequence[str], *, cwd: Optional[Path] = None) -> list:
        """Build the argv that -- run on the HOST via `base` -- reaches the
        guest and executes `cmd` there. The whole guest-side command is
        joined into ONE shell-quoted string (shlex.join), because ssh joins
        all trailing argv into a single string handed to the remote shell
        regardless of how many separate arguments it was given -- pre-
        joining ourselves, correctly quoted, is the only way to make that
        deterministic rather than accidental."""
        inner = shlex.join(str(c) for c in cmd)
        if cwd:
            inner = f"cd {shlex.quote(str(cwd))} || exit 121; {inner}"
        return [*self._ssh_prefix(), inner]

    async def _run_on_host(self, cmd: Sequence[str], *, timeout: float):
        command = await self._base.run(cmd, timeout=timeout)
        rc = await command.wait(timeout=timeout)
        return rc, command.stdout, command.stderr

    # --- attach, not boot ------------------------------------------------------

    async def _ensure_attached(self) -> None:
        if self._attached:
            return
        last_detail = ""
        for attempt in range(ATTACH_RETRIES + 1):
            probe = self._wrap_for_guest(["sh", "-c", "mountpoint -q /mnt/nvkvm && echo MOUNTED || echo NOT_MOUNTED"])
            try:
                rc, out, err = await self._run_on_host(probe, timeout=ATTACH_CHECK_TIMEOUT)
            except Exception as exc:  # noqa: BLE001 -- any failure to even reach the host is a clean "not attached"
                last_detail = f"could not reach the host to even attempt the guest hop: {exc}"
                continue
            text = (out or b"").decode(errors="replace").strip()
            if rc == 0 and "MOUNTED" in text and "NOT_MOUNTED" not in text:
                self._attached = True
                return
            last_detail = f"sshpass ssh -p {self._guest_port} {self._guest_user}@localhost rc={rc}, stdout={text!r}, stderr={(err or b'').decode(errors='replace').strip()[-300:]!r}"
        raise VMMachineNotAttached(
            f"no already-booted nvkvm guest to attach to (tried {ATTACH_RETRIES + 1} time(s)): {last_detail}. "
            f"This class only attaches -- boot one with scripts/run_test_vm.sh (or scripts/sweep.sh) first."
        )

    # --- Machine interface -------------------------------------------------------

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
            raise NotImplementedError(_ENV_UNSUPPORTED_MSG)
        await self._ensure_attached()
        if not self._scratch_ensured:
            # scratch() itself is synchronous (see below) -- the mkdir is
            # deferred to here, the same pattern SSHMachine uses for its own
            # scratch dir.
            mkdir_cmd = self._wrap_for_guest(["mkdir", "-p", str(self.scratch())])
            await self._run_on_host(mkdir_cmd, timeout=CONTROL_TIMEOUT)
            self._scratch_ensured = True
        wrapped = self._wrap_for_guest(cmd, cwd=cwd)
        # Handed straight to `base.run()` -- see the module docstring's
        # COMPOSITION note. `base` (SSHMachine in practice) supplies every
        # bit of the detach/log/poll/reconnect durability; VMMachine adds
        # nothing of its own here.
        return await self._base.run(wrapped, stdin=stdin, timeout=timeout)

    async def need(self, item: Item) -> Path:
        await self._ensure_attached()
        share_dir = await self._ensure_share_dir()

        host_path = await self._base.need(item)
        args = [str(host_path), share_dir, item.name, item.sha256 or ""]
        place_cmd = ["sh", "-c", _PLACE_IN_SHARE_SH, "vm-place", *args]
        rc, out, err = await self._run_on_host(place_cmd, timeout=CONTROL_TIMEOUT)
        status = (out or b"").decode(errors="replace").strip()
        if rc != 0 or status.startswith("MISMATCH"):
            raise ChecksumMismatch(item, status or f"rc={rc} stderr={(err or b'').decode(errors='replace')[-300:]!r}")
        if status not in ("OK", "REUSED"):
            raise RuntimeError(f"need({item.name}): unexpected result placing it into the guest's virtfs share: {status!r} (stderr: {(err or b'').decode(errors='replace')[-300:]!r})")
        return Path(str(GUEST_SHARE_MOUNT / item.name))

    def scratch(self) -> Path:
        # Guest-local, NOT the virtfs share -- see the module docstring's
        # distinction between need()'s mounted (host-verified) inputs and
        # scratch's plain writable working space, same split as every other
        # Machine here. The actual mkdir is deferred into run() (this method
        # is synchronous, like every Machine.scratch()).
        return Path("/tmp/nvkvm-harness-scratch")

    async def _ensure_share_dir(self) -> str:
        if self._share_dir:
            return self._share_dir

        # Discovery runs ON THE HOST (qemu's own argv), not through the
        # guest hop.
        discover_cmd = ["sh", "-c", _FIND_SHARE_DIR_SH]
        rc, out, _err = await self._run_on_host(discover_cmd, timeout=CONTROL_TIMEOUT)
        discovered = (out or b"").decode(errors="replace").strip()

        candidate = discovered or self._explicit_share_dir
        if not candidate:
            raise VMMachineNotAttached(
                "need() has no virtfs share to place items into: this guest's QEMU process was not found with a "
                "-virtfs ...mount_tag=nvkvm_data... argument (NVKVM_SHARE_DIR was not set when it was booted -- "
                "SHARE_ARG in scripts/run_test_vm.sh is optional), and no share_dir= was given explicitly. "
                "need() refuses to fall back to copying through the nested SSH hop -- that would defeat the point "
                "of a virtfs-mounted need()."
            )

        # Confirm the GUEST actually has it mounted too -- a host-side path
        # existing does not mean the guest's /data is live (booted before
        # the tag was wired up, or the fstab entry is disabled).
        confirm = self._wrap_for_guest(["sh", "-c", "mountpoint -q /data && echo MOUNTED || echo NOT_MOUNTED"])
        rc, out, _err = await self._run_on_host(confirm, timeout=CONTROL_TIMEOUT)
        text = (out or b"").decode(errors="replace").strip()
        if "MOUNTED" not in text or "NOT_MOUNTED" in text:
            raise VMMachineNotAttached(
                f"found a host-side share directory ({candidate!r}) but the guest does not have /data mounted "
                f"(mountpoint check said {text!r}). need() refuses to place a file the guest cannot see."
            )

        self._share_dir = candidate
        return candidate
