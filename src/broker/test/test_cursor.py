#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR Apache-2.0
"""CMD_CURSOR and EV_DEVICE over the real socket, no display required.

Drives the broker's --backend test, which logs exactly what a real backend
would put up as the host pointer ("TEST cursor: shown WxH hot X,Y gen G hash
H" or "TEST cursor: none (why)").  The hash is recomputed here from the bytes
this script sent, so a pass means the broker copied the RIGHT rows from the
RIGHT offset and premultiplied them the documented way -- not merely that
something was logged.

The client here is the hostile VMM: every way the record or its fd can lie is
sent, and each must be REFUSED (connection lives, previous cursor stays) or be
a VIOLATION (BYE reason 2, connection closed), as the protocol header says.
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

PKT = struct.Struct("<HHIiiII")
CMD = struct.Struct("<HHIIIIIQII")
CUR = struct.Struct("<HHIIIIIIIII")
assert PKT.size == 24 and CMD.size == 40 and CUR.size == 40

EV_HELLO, EV_SURFACE, EV_FRAME, EV_FOCUS, EV_GRAB, EV_BYE = 1, 2, 3, 11, 10, 13
EV_FORMAT, EV_DEVICE = 16, 17
CMD_QUERY_FORMAT, CMD_CURSOR = 6, 7
SET, HIDE, SHOW = 1, 2, 3
CAP_CURSOR, CAP_DEVICE = 1 << 10, 1 << 11
F_KNOWN, F_RENDER = 1, 2
AR24, XR24 = 0x34325241, 0x34325258
F_ADD_SEALS, F_SEAL_SHRINK = 1033, 0x0002

FAILS = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what)
    if not cond:
        FAILS.append(what)


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ConnectionError("broker closed the connection")
        data += part
    return bytes(data)


def drain(sock, timeout=0.3):
    out = []
    sock.settimeout(timeout)
    try:
        while True:
            out.append(PKT.unpack(recv_exact(sock, PKT.size)))
    except (socket.timeout, ConnectionError, OSError):
        pass
    return out


def closed(sock, timeout=1.0):
    """True if the broker hung up (after possibly sending a BYE)."""
    sock.settimeout(timeout)
    try:
        while True:
            if not sock.recv(4096):
                return True
    except socket.timeout:
        return False
    except OSError:
        return True


def premultiplied(argb):
    a = argb >> 24
    r = min((argb >> 16) & 0xff, a)
    g = min((argb >> 8) & 0xff, a)
    b = min(argb & 0xff, a)
    return (a << 24) | (r << 16) | (g << 8) | b


def fnv(pixels):
    h = 2166136261
    for v in pixels:
        for k in range(4):
            h ^= (v >> (8 * k)) & 0xff
            h = (h * 16777619) & 0xffffffff
    return h


def image(w, h, seed):
    """Deterministic pixels, some of them deliberately NOT premultiplied."""
    px = []
    for y in range(h):
        for x in range(w):
            v = (seed * 2654435761 + y * 40503 + x * 9973) & 0xffffffff
            if (x + y) % 3 == 0:
                v |= 0xff000000          # opaque
            px.append(v)
    return px


def memfd_with(w, h, stride, offset, px, extra=0, seal=True):
    """A memfd holding `px` (w*h) at `offset` with `stride`, padding = 0xEE."""
    fd = os.memfd_create("cursor-test", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    span = stride * (h - 1) + w * 4
    buf = bytearray(b"\xee" * (offset + span + extra))
    for y in range(h):
        row = struct.pack("<%dI" % w, *px[y * w:(y + 1) * w])
        o = offset + y * stride
        buf[o:o + len(row)] = row
    os.write(fd, bytes(buf))
    if seal:
        import fcntl
        fcntl.fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK)
    return fd


def cursor_cmd(op, w=0, h=0, stride=0, offset=0, fourcc=0, hx=0, hy=0,
               flags=0, reserved=0):
    return CUR.pack(CMD_CURSOR, flags, w, h, stride, offset, fourcc, hx, hy,
                    op, reserved)


def send(sock, payload, fds=()):
    socket.send_fds(sock, [payload], list(fds))


class Broker:
    def __init__(self, path):
        self.proc = subprocess.Popen(
            [BROKER, "--socket", path, "--backend", "test", "--persist"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        self.path = path
        self.log_path = None
        for _ in range(150):
            if os.path.exists(path):
                break
            if self.proc.poll() is not None:
                raise RuntimeError(self.proc.stderr.read())
            time.sleep(0.02)
        else:
            raise RuntimeError("broker socket did not appear")
        # stderr is drained by a reader thread so the broker never blocks on
        # a full pipe (it logs a line per cursor change).
        import threading
        self.lines = []
        self.lock = threading.Lock()

        def pump():
            for line in self.proc.stderr:
                with self.lock:
                    self.lines.append(line.rstrip("\n"))
        self.thread = threading.Thread(target=pump, daemon=True)
        self.thread.start()

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark, settle=0.25):
        time.sleep(settle)
        with self.lock:
            return self.lines[mark:]

    def stdin(self, *lines):
        self.proc.stdin.write("".join(line + "\n" for line in lines))
        self.proc.stdin.flush()
        time.sleep(0.15)

    def connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect(self.path)
        return sock

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdin.close()


def shown(lines):
    """The last 'shown'/'none' line, parsed, or None."""
    for line in reversed(lines):
        m = re.search(r"TEST cursor: shown (\d+)x(\d+) hot (\d+),(\d+) "
                      r"gen (\d+) hash 0x([0-9a-f]+)", line)
        if m:
            w, h, hx, hy, gen, hsh = m.groups()
            return ("shown", int(w), int(h), int(hx), int(hy), int(gen),
                    int(hsh, 16))
        m = re.search(r"TEST cursor: none \(([a-z ]+)\)", line)
        if m:
            return ("none", m.group(1))
    return None


def main():
    global BROKER
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_cursor.py /path/to/broker")
    BROKER = os.path.abspath(sys.argv[1])

    with tempfile.TemporaryDirectory() as tmp:
        b = Broker(os.path.join(tmp, "broker.sock"))
        try:
            run(b)
        finally:
            b.stop()
    if FAILS:
        print("%d cursor/device check(s) FAILED" % len(FAILS))
        sys.exit(1)
    print("cursor/device tests passed")


def run(b):
    print("-- handshake: the five-packet prefix is unchanged, DEVICE follows")
    s = b.connect()
    hs = [PKT.unpack(recv_exact(s, PKT.size)) for _ in range(6)]
    types = [p[0] for p in hs]
    check(types[:5] == [EV_HELLO, EV_SURFACE, EV_FOCUS, EV_GRAB, EV_FRAME],
          "HELLO SURFACE FOCUS GRAB FRAME, byte-for-byte the old order")
    check(types[5] == EV_DEVICE, "EV_DEVICE is the sixth packet")
    check(bool(hs[0][6] & CAP_CURSOR), "HELLO advertises CAP_CURSOR")
    check(bool(hs[0][6] & CAP_DEVICE), "HELLO advertises CAP_DEVICE")
    check([p[2] for p in hs] == list(range(6)), "seq has no gap")
    check(hs[5][3:] == (0, 0, 0, 0),
          "the test backend's device is 'unknown': x=0 y=0 w0=0 w1=0")

    print("-- EV_DEVICE again when the display moves")
    b.stdin("d 226 128 3")
    pk = [p for p in drain(s) if p[0] == EV_DEVICE]
    check(len(pk) == 1 and pk[0][3:] == (F_KNOWN | F_RENDER, 0, 226, 128),
          "a changed device is re-sent: x=KNOWN|RENDER w0=226 w1=128")

    print("-- SET: copied from the right rows, premultiplied, shown")
    w, h, stride, offset = 24, 20, 24 * 4 + 12, 64
    px = image(w, h, 1)
    want_hash = fnv([premultiplied(v) for v in px])
    m = b.mark()
    fd = memfd_with(w, h, stride, offset, px)
    send(s, cursor_cmd(SET, w, h, stride, offset, AR24, 5, 7), [fd])
    os.close(fd)
    st = shown(b.since(m))
    check(st is not None and st[0] == "shown" and st[1:5] == (w, h, 5, 7),
          "SET 24x20 hot 5,7 is shown")
    check(st is not None and st[0] == "shown" and st[6] == want_hash,
          "the pixels are the sent ones, premultiplied (hash %08x)" %
          want_hash)
    gen_a = st[5] if st and st[0] == "shown" else None

    print("-- the last row needs only its pixels: an exact-size memfd")
    m = b.mark()
    px2 = image(2, 2, 2)
    fd = memfd_with(2, 2, 16, 0, px2)          # 16 + 8 = 24 bytes, no slack
    check(os.fstat(fd).st_size == 24, "the memfd really is 24 bytes")
    send(s, cursor_cmd(SET, 2, 2, 16, 0, AR24, 1, 1), [fd])
    os.close(fd)
    st = shown(b.since(m))
    check(st is not None and st[0] == "shown" and st[1:3] == (2, 2),
          "a tightly packed last row is accepted")

    print("-- HIDE / SHOW, and the grab")
    m = b.mark()
    send(s, cursor_cmd(HIDE))
    check(shown(b.since(m)) == ("none", "hidden"), "HIDE shows none")
    m = b.mark()
    send(s, cursor_cmd(SHOW))
    st = shown(b.since(m))
    check(st is not None and st[0] == "shown" and st[1:3] == (2, 2),
          "SHOW brings the last image back")
    b.stdin("f 1")
    m = b.mark()
    b.stdin("k 29 1", "k 56 1", "k 34 1", "k 34 0", "k 56 0", "k 29 0")
    check(shown(b.since(m)) == ("none", "grabbed"),
          "CTRL+ALT+G: the grab hides the guest's cursor")
    m = b.mark()
    b.stdin("k 29 1", "k 56 1", "k 34 1", "k 34 0", "k 56 0", "k 29 0")
    st = shown(b.since(m))
    check(st is not None and st[0] == "shown",
          "ending the grab shows it again")
    drain(s)

    print("-- REFUSED: the connection lives and the previous cursor stays")
    good = shown(b.since(0, 0))      # the last state shown, from the whole log
    pipe_r, pipe_w = os.pipe()
    sa, sb = socket.socketpair()
    devnull = os.open("/dev/null", os.O_RDONLY)
    devzero = os.open("/dev/zero", os.O_RDONLY)
    small = memfd_with(4, 4, 16, 0, image(4, 4, 3))       # 64 bytes
    refused = [
        ("an XR24 cursor", cursor_cmd(SET, 4, 4, 16, 0, XR24, 0, 0), small),
        ("a 257-wide cursor", cursor_cmd(SET, 257, 1, 1028, 0, AR24, 0, 0),
         small),
        ("a 257-high cursor", cursor_cmd(SET, 1, 257, 4, 0, AR24, 0, 0),
         small),
        ("a zero-size cursor", cursor_cmd(SET, 0, 4, 16, 0, AR24, 0, 0),
         small),
        ("hot_x == width", cursor_cmd(SET, 4, 4, 16, 0, AR24, 4, 0), small),
        ("hot_y == height", cursor_cmd(SET, 4, 4, 16, 0, AR24, 0, 4), small),
        ("stride < width*4", cursor_cmd(SET, 4, 4, 15, 0, AR24, 0, 0), small),
        ("stride > 1024", cursor_cmd(SET, 4, 4, 1028, 0, AR24, 0, 0), small),
        ("geometry past the memfd's end",
         cursor_cmd(SET, 4, 5, 16, 0, AR24, 0, 0), small),
        ("an offset that would wrap 32 bits",
         cursor_cmd(SET, 4, 4, 16, 0xffffffff, AR24, 0, 0), small),
        ("a pipe instead of a memfd",
         cursor_cmd(SET, 1, 1, 4, 0, AR24, 0, 0), pipe_r),
        ("a socket instead of a memfd",
         cursor_cmd(SET, 1, 1, 4, 0, AR24, 0, 0), sa.fileno()),
        # /dev is devtmpfs and reports TMPFS_MAGIC: only F_GET_SEALS tells a
        # device node from shmem.  A tty here could block the event loop.
        ("/dev/null (TMPFS_MAGIC, but not shmem)",
         cursor_cmd(SET, 1, 1, 4, 0, AR24, 0, 0), devnull),
        # /dev/zero READS successfully.  Two layers refuse it: the shmem
        # proof (F_GET_SEALS), and its st_size of 0 failing the extent check.
        # Measured by deleting the first: this case still passes on the
        # second, so neither alone is load-bearing -- both stay.
        ("/dev/zero (TMPFS_MAGIC, readable, not shmem)",
         cursor_cmd(SET, 1, 1, 4, 0, AR24, 0, 0), devzero),
    ]
    for what, payload, fd in refused:
        m = b.mark()
        send(s, payload, [fd])
        lines = b.since(m, 0.15)
        check(any("refused" in l for l in lines) and
              not any(re.search(r"TEST cursor: (shown|none)", l)
                      for l in lines),
              "refused, and nothing re-shown: " + what)
    for fd in (pipe_r, pipe_w, devnull, devzero, small):
        os.close(fd)
    sa.close()
    sb.close()
    # Still connected, still the same cursor: a QUERY_FORMAT is answered.
    send(s, CMD.pack(CMD_QUERY_FORMAT, 0, 0, 0, 0, 0, XR24, 0, 0, 0))
    check(any(p[0] == EV_FORMAT for p in drain(s)),
          "the connection survived every refusal")
    m = b.mark()
    send(s, cursor_cmd(SHOW))
    st = shown(b.since(m))
    check(good is not None and st is not None and st[0] == "shown" and
          st[5] == good[5], "the cursor from before the refusals is intact")

    print("-- PACED: a burst is coalesced and the LAST image wins")
    m = b.mark()
    n = 60
    last_hash = None
    for i in range(n):
        p = image(8, 8, 100 + i)
        last_hash = fnv([premultiplied(v) for v in p])
        fd = memfd_with(8, 8, 32, 0, p)
        send(s, cursor_cmd(SET, 8, 8, 32, 0, AR24, 0, 0), [fd])
        os.close(fd)
    lines = b.since(m, 0.4)
    applied = [l for l in lines if "TEST cursor: shown" in l]
    st = shown(lines)
    check(st is not None and st[0] == "shown" and st[6] == last_hash,
          "the last of %d SETs is the one on screen" % n)
    check(0 < len(applied) < n,
          "%d SETs reached the display as %d uploads" % (n, len(applied)))
    drain(s)

    print("-- a new client never inherits the old one's cursor")
    m = b.mark()
    s.close()
    lines = b.since(m, 0.3)
    check(shown(lines) == ("none", "no image"),
          "detach forgets the image at once")
    s2 = b.connect()
    for _ in range(6):
        recv_exact(s2, PKT.size)
    m = b.mark()
    send(s2, cursor_cmd(SHOW))
    check(shown(b.since(m)) == ("none", "no image"),
          "SHOW on a fresh connection shows nothing")
    s2.close()
    time.sleep(0.2)

    print("-- VIOLATIONS: BYE(protocol) and the connection is closed")
    one = memfd_with(1, 1, 4, 0, [0xff000000])
    violations = [
        ("flags set", cursor_cmd(SET, 1, 1, 4, 0, AR24, flags=1), [one]),
        ("reserved1 set", cursor_cmd(SET, 1, 1, 4, 0, AR24, reserved=1),
         [one]),
        ("op 0", cursor_cmd(0, 1, 1, 4, 0, AR24), [one]),
        ("op 4", cursor_cmd(4, 1, 1, 4, 0, AR24), [one]),
        ("SET without an fd", cursor_cmd(SET, 1, 1, 4, 0, AR24), []),
        ("HIDE with an fd", cursor_cmd(HIDE), [one]),
        ("HIDE with a width", cursor_cmd(HIDE, w=1), []),
        ("SHOW with a hot spot", cursor_cmd(SHOW, hx=1), []),
        ("two fds on one SET", cursor_cmd(SET, 1, 1, 4, 0, AR24),
         [one, one]),
    ]
    for what, payload, fds in violations:
        c = b.connect()
        for _ in range(6):
            recv_exact(c, PKT.size)
        m = b.mark()
        send(c, payload, fds)
        pk = drain(c, 0.3)
        gone = closed(c)
        lines = b.since(m, 0.05)
        check(any(p[0] == EV_BYE and p[3] == 2 for p in pk) and gone and
              any("protocol violation" in l for l in lines),
              "violation: " + what)
        c.close()
        time.sleep(0.1)
    os.close(one)


if __name__ == "__main__":
    main()
