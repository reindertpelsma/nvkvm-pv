#!/usr/bin/env python3
"""Adopted listeners must preserve the broker's authentication boundary."""

import os
import socket
import subprocess
import sys
import tempfile


def spawn(broker, listener, *extra):
    return subprocess.Popen(
        [broker, "--socket-fd", str(listener.fileno()), "--backend", "test",
         "--persist", *extra],
        pass_fds=(listener.fileno(),), stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
    )


def stop(proc):
    if proc.poll() is None:
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
    return log


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            break
        data += part
    return bytes(data)


def expect_refused(broker, listener, client, reason, *extra):
    proc = spawn(broker, listener, *extra)
    listener.close()
    try:
        proc.wait(timeout=2)
        client.settimeout(1)
        try:
            data = client.recv(24)
        except (ConnectionError, socket.timeout):
            data = b""
        log = stop(proc)
    finally:
        client.close()
    if proc.returncode == 0 or data:
        raise RuntimeError(f"unsafe adopted listener was served ({reason})\n"
                           f"received={data!r}\n{log}")
    if reason not in log:
        raise RuntimeError(f"adopted-listener refusal was not explicit\n{log}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_adopted_socket.py /path/to/broker")
    broker = os.path.abspath(sys.argv[1])

    # This is the exact SR-04 shape: an already-connected TCP peer waiting on
    # a listener passed through --socket-fd.  Configured 0600 is irrelevant to
    # that descriptor, and the peer must never receive HELLO.
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.bind(("127.0.0.1", 0))
    tcp.listen()
    tcp_client = socket.create_connection(tcp.getsockname())
    expect_refused(broker, tcp, tcp_client, "not an AF_UNIX socket")

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "adopted.sock")

        # Even a named AF_UNIX listener is external state.  --no-peercred is
        # refused rather than trusting configured mode bits that were never
        # applied to it.
        unix = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        unix.bind(path)
        unix.listen()
        unix_client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        unix_client.connect(path)
        expect_refused(broker, unix, unix_client,
                       "--no-peercred is refused", "--no-peercred")

        os.unlink(path)
        unix = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        unix.bind(path)
        unix.listen()
        unix_client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        unix_client.settimeout(2)
        unix_client.connect(path)
        proc = spawn(broker, unix)
        unix.close()
        try:
            hello = recv_exact(unix_client, 24)
            if len(hello) != 24:
                raise RuntimeError(f"safe adopted UNIX listener got {len(hello)} "
                                   "HELLO bytes")
        finally:
            unix_client.close()
            log = stop(proc)
        if proc.returncode not in (0, -15):
            raise RuntimeError(f"safe adopted UNIX listener failed\n{log}")

    print("adopted-socket authentication tests passed")


if __name__ == "__main__":
    main()
