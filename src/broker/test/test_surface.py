#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR Apache-2.0
"""Observe refresh-only host changes and reconnects on the real broker socket."""
import os
import sys
import tempfile
import time

import test_cursor as wire


def surfaces(sock):
    return [(p[3], p[4], p[5]) for p in wire.drain(sock)
            if p[0] == wire.EV_SURFACE]


def main():
    wire.BROKER = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="nb-surface-") as directory:
        broker = wire.Broker(os.path.join(directory, "broker.sock"))
        try:
            with broker.connect() as sock:
                initial = surfaces(sock)
                assert len(initial) == 1 and initial[0][2] == 0, initial
                width, height, _ = initial[0]
                for hz in [60000, 30000, 75000, 0, 60000]:
                    broker.stdin(f"m {width} {height} {hz}")
                    got = surfaces(sock)
                    assert got == [(width, height, hz)], got
                    broker.stdin(f"m {width} {height} {hz}")
                    assert surfaces(sock) == [], "identical hint was not deduplicated"
                broker.stdin("m 1024 768 60000")
                assert surfaces(sock) == [(1024, 768, 60000)]
            time.sleep(0.2)
            with broker.connect() as sock:
                got = surfaces(sock)
                assert got == [(1024, 768, 60000)], got
            print("SURFACE_PASS refresh-only=5 duplicate=5 resize=1 reconnect=1")
        finally:
            broker.stop()


if __name__ == "__main__":
    main()
