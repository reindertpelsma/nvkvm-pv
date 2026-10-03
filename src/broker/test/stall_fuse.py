# SPDX-License-Identifier: GPL-2.0 OR Apache-2.0
"""A FUSE filesystem whose daemon never answers anyone but its owner.

The hostile VMM's best tool against a process that receives its fds: a file on
a FUSE mount whose daemon it controls.  Any syscall that reaches the file's
filesystem code -- fstatfs (FUSE_STATFS), fstat (GETATTR), close (FLUSH) -- is
a request to that daemon, and the caller waits for the answer.  This one
serves the process that created it (so the test can open the file and close
its own copy) and RECORDS, and never answers, every request from any other
process.  A broker that asks this filesystem anything therefore stops, which
is what the test measures; closing the device fd aborts the connection and
lets it go again, so a failing run still cleans up.

Speaks the kernel FUSE protocol directly (include/uapi/linux/fuse.h): no
libfuse, no Python package.  Mounts with mount(2) when root, otherwise through
fusermount3 and its _FUSE_COMMFD handshake, exactly as libfuse does.

    with StallFuse() as fs:          # raises Unavailable if FUSE cannot be used
        fd = os.open(fs.path, os.O_RDWR)
        ...                           # hand fd to the broker
        fs.requests_from(pid)         # [(opcode name, tid), ...] never answered
"""

import ctypes
import os
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import threading

IN_HDR = struct.Struct("<IIQQIIIHH")        # struct fuse_in_header, 40 bytes
OUT_HDR = struct.Struct("<IiQ")             # struct fuse_out_header, 16 bytes
ATTR = struct.Struct("<QQQQQQIIIIIIIIII")   # struct fuse_attr, 88 bytes
INIT_OUT = struct.Struct("<IIIIHHIIHHII H22x")   # struct fuse_init_out, 64
assert IN_HDR.size == 40 and OUT_HDR.size == 16 and ATTR.size == 88
assert INIT_OUT.size == 64

OPS = {1: "LOOKUP", 2: "FORGET", 3: "GETATTR", 14: "OPEN", 15: "READ",
       17: "STATFS", 18: "RELEASE", 22: "GETXATTR", 25: "FLUSH", 26: "INIT",
       34: "ACCESS", 36: "INTERRUPT", 39: "IOCTL", 42: "BATCH_FORGET",
       46: "LSEEK", 52: "STATX"}
NO_REPLY = {2, 36, 42}                      # FORGET, INTERRUPT, BATCH_FORGET
ENOENT, ENOSYS = 2, 38
S_IFDIR, S_IFREG = 0o040000, 0o100000
FILE_SIZE = 1 << 20                         # big enough for any extent check


class Unavailable(Exception):
    """FUSE cannot be used here: no /dev/fuse, no permission, no helper."""


def _attr(ino, mode, size, nlink):
    return ATTR.pack(ino, size, (size + 511) // 512, 0, 0, 0, 0, 0, 0,
                     mode, nlink, os.getuid(), os.getgid(), 0, 4096, 0)


class StallFuse:
    def __init__(self):
        self.dir = None
        self.dev = -1
        self.root_mount = False
        self.owner = os.getpid()
        self.lock = threading.Lock()
        self.seen = []              # (opcode, pid) of every unanswered request
        self.stop = threading.Event()
        self.thread = None

    # -- mounting -----------------------------------------------------------
    def _mount_root(self):
        libc = ctypes.CDLL(None, use_errno=True)
        libc.mount.argtypes = [ctypes.c_char_p] * 3 + [ctypes.c_ulong,
                                                       ctypes.c_char_p]
        dev = os.open("/dev/fuse", os.O_RDWR | os.O_CLOEXEC)
        opts = "fd=%d,rootmode=40000,user_id=%d,group_id=%d" % (
            dev, os.getuid(), os.getgid())
        # MS_NOSUID | MS_NODEV
        if libc.mount(b"nvkvm-stall", self.dir.encode(), b"fuse.nvkvm-stall",
                      2 | 4, opts.encode()) != 0:
            e = ctypes.get_errno()
            os.close(dev)
            raise Unavailable("mount(2): %s" % os.strerror(e))
        self.root_mount = True
        return dev

    def _mount_fusermount(self):
        helper = shutil.which("fusermount3") or shutil.which("fusermount")
        if not helper:
            raise Unavailable("not root and no fusermount3")
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            env = dict(os.environ, _FUSE_COMMFD=str(theirs.fileno()))
            r = subprocess.run(
                [helper, "-o", "fsname=nvkvm-stall,subtype=nvkvm-stall",
                 "--", self.dir], env=env, pass_fds=(theirs.fileno(),),
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                timeout=20)
            theirs.close()
            if r.returncode != 0:
                raise Unavailable("%s: %s" % (helper, r.stderr.strip()))
            ours.settimeout(5)
            _, fds, _, _ = socket.recv_fds(ours, 16, 1)
            if not fds:
                raise Unavailable("%s sent no fd" % helper)
            return fds[0]
        finally:
            ours.close()
            if theirs.fileno() >= 0:
                theirs.close()

    def __enter__(self):
        if not os.path.exists("/dev/fuse"):
            raise Unavailable("no /dev/fuse")
        self.dir = tempfile.mkdtemp(prefix="nvkvm-stall-")
        try:
            self.dev = (self._mount_root() if os.geteuid() == 0
                        else self._mount_fusermount())
        except Exception:
            os.rmdir(self.dir)
            raise
        os.set_blocking(self.dev, False)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        return self

    @property
    def path(self):
        return os.path.join(self.dir, "file")

    # -- the daemon ---------------------------------------------------------
    def _reply(self, unique, error=0, payload=b""):
        try:
            os.write(self.dev, OUT_HDR.pack(OUT_HDR.size + len(payload),
                                            -error, unique) + payload)
        except OSError:
            pass                    # interrupted or aborted: nothing to do

    def _serve(self):
        while not self.stop.is_set():
            r, _, _ = select.select([self.dev], [], [], 0.05)
            if not r:
                continue
            try:
                buf = os.read(self.dev, (1 << 20) + 4096)
            except BlockingIOError:
                continue
            except OSError:
                return              # unmounted or aborted
            if len(buf) < IN_HDR.size:
                continue
            (_, op, unique, node, _, _, pid, _, _) = IN_HDR.unpack_from(buf)
            body = buf[IN_HDR.size:]
            if op == 26:            # INIT: answered for everyone, it is ours
                major, minor, readahead = struct.unpack_from("<III", body)
                self._reply(unique, 0, INIT_OUT.pack(
                    7, 31, readahead, 0, 16, 12, 4096, 1, 0, 0, 0, 0, 0))
                continue
            if pid != self.owner:
                # THE POINT: anyone else waits forever.  FORGET-like ops
                # expect no answer anyway; record only what would block.
                if op not in NO_REPLY:
                    with self.lock:
                        self.seen.append((OPS.get(op, str(op)), pid))
                continue
            if op in NO_REPLY:
                continue
            if op == 1:             # LOOKUP
                name = body.split(b"\0", 1)[0]
                if node == 1 and name == b"file":
                    self._reply(unique, 0, struct.pack(
                        "<QQQQII", 2, 1, 3600, 3600, 0, 0) +
                        _attr(2, S_IFREG | 0o600, FILE_SIZE, 1))
                else:
                    self._reply(unique, ENOENT)
            elif op == 3:           # GETATTR
                attr = (_attr(1, S_IFDIR | 0o700, 0, 2) if node == 1 else
                        _attr(2, S_IFREG | 0o600, FILE_SIZE, 1))
                self._reply(unique, 0, struct.pack("<QII", 3600, 0, 0) + attr)
            elif op == 14:          # OPEN
                self._reply(unique, 0, struct.pack("<QIi", 1, 0, 0))
            elif op == 15:          # READ
                size = struct.unpack_from("<QQI", body)[2]
                self._reply(unique, 0, b"\0" * min(size, FILE_SIZE))
            elif op in (18, 25):    # RELEASE, FLUSH
                self._reply(unique)
            else:
                self._reply(unique, ENOSYS)

    def requests_from(self, pids):
        """Every unanswered request from any pid in `pids` (pids or tids)."""
        pids = set(pids)
        with self.lock:
            return [r for r in self.seen if r[1] in pids]

    def requests(self):
        with self.lock:
            return list(self.seen)

    # -- teardown -----------------------------------------------------------
    def abort(self):
        """Close the device: the kernel aborts the connection and every
        request still waiting on this daemon returns an error at once."""
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=2)
        if self.dev >= 0:
            os.close(self.dev)
            self.dev = -1

    def __exit__(self, *exc):
        self.abort()
        if self.root_mount:
            libc = ctypes.CDLL(None, use_errno=True)
            libc.umount2(self.dir.encode(), 2)          # MNT_DETACH
        else:
            helper = shutil.which("fusermount3") or shutil.which("fusermount")
            subprocess.run([helper, "-u", "-z", self.dir],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
        try:
            os.rmdir(self.dir)
        except OSError:
            pass
        return False
