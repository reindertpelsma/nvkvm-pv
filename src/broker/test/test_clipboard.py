#!/usr/bin/env python3
"""Clipboard transaction sequencing/cap regression test."""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

PKT_SIZE = 24
CLIP = struct.Struct("<HHIIB27s")
LAST = 0x20
CMD_CLIPBOARD = 4
MAX_BYTES = 7168
CHUNK_BYTES = 27
MAX_CHUNKS = (MAX_BYTES + CHUNK_BYTES - 1) // CHUNK_BYTES


def command(chunk, data=b"", last=False):
    if len(data) > CHUNK_BYTES:
        raise ValueError("chunk too large")
    info = len(data) | (LAST if last else 0)
    return CLIP.pack(CMD_CLIPBOARD, 0, chunk, 0, info,
                     data.ljust(CHUNK_BYTES, b"\0"))


def run_case(broker, sends, expect_commits, expect_violation=False):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "broker.sock")
        proc = subprocess.Popen(
            [broker, "--socket", path, "--backend", "test", "--persist",
             "--clipboard", "guest-to-host"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True,
        )
        try:
            for _ in range(100):
                if os.path.exists(path):
                    break
                time.sleep(0.02)
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2)
            sock.connect(path)
            remaining = 5 * PKT_SIZE
            while remaining:
                data = sock.recv(remaining)
                if not data:
                    raise RuntimeError("short handshake")
                remaining -= len(data)
            proc.stdin.write("f 1\n")
            proc.stdin.flush()
            time.sleep(0.1)
            for payload in sends:
                sock.sendall(payload)
            time.sleep(0.2)
            if expect_violation:
                sock.settimeout(1)
                while sock.recv(4096):
                    pass
            sock.close()
        finally:
            proc.terminate()
            proc.wait(timeout=2)
            proc.stdin.close()
            log = proc.stderr.read()
            proc.stderr.close()
        commits = log.count("TEST clipboard from guest: 3 bytes")
        if commits != expect_commits:
            raise RuntimeError(f"expected {expect_commits} clean commit(s), got "
                               f"{commits}\n{log}")
        if expect_violation and "chunks are not a monotonic transaction" not in log:
            raise RuntimeError(f"out-of-order chunk was not rejected\n{log}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_clipboard.py /path/to/broker")
    broker = os.path.abspath(sys.argv[1])

    # A new chunk zero discards an abandoned prefix rather than concatenating.
    run_case(broker, [command(0, b"old"), command(0, b"new", True)], 1)

    # Exceeding the cap cannot wedge the accumulator: a following transaction
    # begins at chunk zero and commits normally.
    oversized = [command(i, b"x" * CHUNK_BYTES)
                 for i in range(MAX_CHUNKS + 1)]
    oversized.append(command(0, b"new", True))
    run_case(broker, oversized, 1)

    # A continuation without a transaction is a framing violation.
    run_case(broker, [command(1, b"bad", True)], 0, expect_violation=True)


if __name__ == "__main__":
    main()
