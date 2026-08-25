#!/usr/bin/env python3
"""Persistent-client and delayed-paste regression test, no display required."""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

PKT = struct.Struct("<HHIiiII")
CMD = struct.Struct("<HHIIIIIQII")
EV_HELLO, EV_CLIPBOARD, EV_KEY = 1, 15, 5
CMD_CAPS = 5


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise RuntimeError("broker closed during a packet")
        data += part
    return bytes(data)


def handshake(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(2)
    sock.connect(path)
    packets = [PKT.unpack(recv_exact(sock, PKT.size)) for _ in range(5)]
    if packets[0][0] != EV_HELLO or packets[0][2] != 0:
        raise RuntimeError("HELLO was not seq=0 on a fresh persistent client")
    return sock


def caps(sock):
    sock.sendall(CMD.pack(CMD_CAPS, 0, 1, 0, 0, 0, 0, 0, 0, 0))


def input_lines(proc, *lines):
    proc.stdin.write("".join(line + "\n" for line in lines))
    proc.stdin.flush()
    time.sleep(0.15)


def drain(sock, timeout=0.25):
    packets = []
    sock.settimeout(timeout)
    while True:
        try:
            packets.append(PKT.unpack(recv_exact(sock, PKT.size)))
        except socket.timeout:
            return packets


def paste_chord(proc, finish=True):
    lines = ("k 29 1", "k 47 1", "k 47 0", "k 29 0")
    input_lines(proc, *(lines + (("c",) if finish else ())))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_lifecycle.py /path/to/broker")
    broker = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "broker.sock")

        unsupported = subprocess.run(
            [broker, "--socket", path, "--backend", "test", "--clipboard",
             "full"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True, timeout=2, check=False,
        )
        if unsupported.returncode == 0 or "not implemented" not in unsupported.stderr:
            raise RuntimeError("advertised-but-unimplemented full clipboard mode "
                               "was not rejected explicitly")

        proc = subprocess.Popen(
            [broker, "--socket", path, "--backend", "test", "--persist",
             "--clipboard", "consent"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True,
        )
        try:
            for _ in range(100):
                if os.path.exists(path):
                    break
                if proc.poll() is not None:
                    raise RuntimeError(proc.stderr.read())
                time.sleep(0.02)
            else:
                raise RuntimeError("broker socket did not appear")

            # A starts an asynchronous host-selection fetch and vanishes before
            # the test backend's explicit `c` completion event.
            a = handshake(path)
            caps(a)
            input_lines(proc, "f 1")
            paste_chord(proc, finish=False)
            a.close()
            time.sleep(0.25)

            b = handshake(path)

            # Cancellation closes the real Wayland pipe, but a callback may
            # already have been queued.  The test backend retains A's old
            # generation for `s`; even that forced stale completion must not
            # disclose A's host selection to B.
            input_lines(proc, "s")
            if any(pkt[0] == EV_CLIPBOARD for pkt in drain(b)):
                raise RuntimeError("stale fetch completion crossed clients")

            # CAPS is connection-scoped.  Before B sends it, the same chord is
            # an ordinary guest-local paste and must not receive A's fetch.
            paste_chord(proc, finish=True)
            before_caps = drain(b)
            if any(pkt[0] == EV_CLIPBOARD for pkt in before_caps):
                raise RuntimeError("client B inherited A's clipboard/fetch state")

            # Once B identifies its agent, delayed completion must queue the
            # clipboard first and replay one balanced V press even though the
            # physical V and Ctrl releases raced ahead of completion.
            caps(b)
            paste_chord(proc, finish=True)
            packets = drain(b, 0.5)
            clip_pos = [i for i, pkt in enumerate(packets)
                        if pkt[0] == EV_CLIPBOARD]
            key_edges = [(i, pkt[3], pkt[4]) for i, pkt in enumerate(packets)
                         if pkt[0] == EV_KEY]
            v_edges = [(i, down) for i, code, down in key_edges if code == 47]
            if not clip_pos:
                raise RuntimeError("fresh client did not receive clipboard data")
            if [edge for _, edge in v_edges] != [1, 0]:
                raise RuntimeError(f"paste key was not balanced: {v_edges!r}")
            if max(clip_pos) > v_edges[0][0]:
                raise RuntimeError("paste key arrived before all clipboard chunks")
            replay = [(code, down) for i, code, down in key_edges
                      if i > max(clip_pos)]
            if replay != [(29, 1), (47, 1), (47, 0), (29, 0)]:
                raise RuntimeError(f"paste chord was not coherently replayed: "
                                   f"{replay!r}")

            # The advertised 7 KiB maximum must fit atomically in the fixed
            # event ring.  Exact-limit arithmetic used to disagree between
            # the header, Wayland reader and ring preflight.
            paste_chord(proc, finish=False)
            input_lines(proc, "c 7168")
            packets = drain(b, 1.0)
            clips = [pkt for pkt in packets if pkt[0] == EV_CLIPBOARD]
            total = sum(pkt[3] & 0x1f for pkt in clips)
            lasts = sum(bool((pkt[3] & 0xff) & 0x20) for pkt in clips)
            if len(clips) != 478 or total != 7168 or lasts != 1:
                raise RuntimeError("exact clipboard cap did not fit atomically: "
                                   f"chunks={len(clips)} bytes={total} "
                                   f"last={lasts}")

            paste_chord(proc, finish=False)
            input_lines(proc, "c 7169")
            if any(pkt[0] == EV_CLIPBOARD for pkt in drain(b)):
                raise RuntimeError("clipboard larger than the cap was queued")
            b.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            if proc.stdin:
                proc.stdin.close()
            log = proc.stderr.read() if proc.stderr else ""
            if proc.stderr:
                proc.stderr.close()
            if proc.returncode not in (0, -15):
                raise RuntimeError(log)


if __name__ == "__main__":
    main()
