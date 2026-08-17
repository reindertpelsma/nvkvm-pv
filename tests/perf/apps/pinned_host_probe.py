#!/usr/bin/env python3
"""pinned_host_probe.py — characterise the guest's host-memory registration path.

Standing up vLLM in the guest failed at
`torch.zeros(..., pin_memory=True)` with CUDA error 304 ("OS call failed or
operation not supported on this OS").  This probe pins that down to a number:

  1. largest pinned host allocation that succeeds (bisected exactly),
  2. cost of a pinned allocation as a function of size,
  3. H2D bandwidth from pinned vs pageable vs file-backed (mmap) memory —
     the last is how a serving stack actually uploads model weights.

Run the SAME file on host and guest and compare.  Emits `M <key> <value>`.
"""
from __future__ import annotations

import argparse
import mmap
import os
import time

import torch


def emit(key, value):
    print(f"M {key} {value}", flush=True)


def try_pin(nbytes: int):
    """Returns (ok, seconds)."""
    t0 = time.time()
    try:
        t = torch.empty(nbytes, dtype=torch.uint8, pin_memory=True)
        dt = time.time() - t0
        del t
        return True, dt
    except Exception:
        return False, time.time() - t0


def bisect_max_pin(lo: int, hi: int) -> int:
    """Largest byte count that pins successfully, in [lo, hi]."""
    ok, _ = try_pin(lo)
    if not ok:
        return 0
    while lo < hi:
        mid = (lo + hi + 1) // 2
        ok, _ = try_pin(mid)
        if ok:
            lo = mid
        else:
            hi = mid - 1
    return lo


def h2d(src: torch.Tensor, dst: torch.Tensor, reps: int) -> float:
    """GB/s for repeated H2D of src -> dst."""
    dst.copy_(src)
    torch.cuda.synchronize()
    t0 = time.time()
    for _ in range(reps):
        dst.copy_(src)
    torch.cuda.synchronize()
    dt = time.time() - t0
    return src.numel() * reps / dt / 1e9


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="unknown")
    ap.add_argument("--scratch", default="/tmp/pinprobe.bin")
    ap.add_argument("--xfer-mb", type=int, default=512)
    args = ap.parse_args()

    torch.cuda.init()
    emit("gpu", torch.cuda.get_device_name(0).replace(" ", "_"))

    # -- 1. the cap ---------------------------------------------------------
    max_pin = bisect_max_pin(1 << 20, 2 << 30)
    emit("max_pin_bytes", max_pin)
    emit("max_pin_mib", round(max_pin / (1 << 20), 4))

    # -- 2. cost of pinning, by size ---------------------------------------
    # DO NOT trust these as registration cost.  torch keeps freed pinned
    # blocks in its CachingHostAllocator, so every call after the first is a
    # cache hit and reads as ~0 ms.  Kept only to show the cap taking effect.
    # For real registration cost use cuMemHostAlloc directly, or read the
    # guest module's own DIAG counters ("migrate_range(bulk) N pages ... us").
    for mb in (1, 2, 4, 8, 16):
        nbytes = mb << 20
        if nbytes > max_pin:
            emit(f"pin_{mb}mib_CACHED_ms", "DID_NOT_RUN_over_cap")
            continue
        best = min(try_pin(nbytes)[1] for _ in range(3))
        emit(f"pin_{mb}mib_CACHED_ms", round(best * 1e3, 2))

    # -- 3. H2D bandwidth by source memory kind ----------------------------
    n = args.xfer_mb << 20
    dst = torch.empty(n, dtype=torch.uint8, device="cuda")

    pageable = torch.empty(n, dtype=torch.uint8)          # anon, not pinned
    emit("h2d_pageable_gbps", round(h2d(pageable, dst, 5), 2))
    del pageable

    # file-backed mmap: how model weights (safetensors) actually arrive
    with open(args.scratch, "wb") as fh:
        fh.truncate(n)
    with open(args.scratch, "r+b") as fh:
        mm = mmap.mmap(fh.fileno(), n)
        filebacked = torch.frombuffer(mm, dtype=torch.uint8)
        emit("h2d_filebacked_mmap_gbps", round(h2d(filebacked, dst, 5), 2))
        del filebacked
        mm.close()
    os.unlink(args.scratch)

    # pinned, at the largest size that actually works on this side
    pin_n = min(n, max_pin)
    if pin_n > 0:
        pinned = torch.empty(pin_n, dtype=torch.uint8, pin_memory=True)
        emit("h2d_pinned_size_mib", pin_n >> 20)
        emit("h2d_pinned_gbps", round(h2d(pinned, dst[:pin_n], 20), 2))
        del pinned
    else:
        emit("h2d_pinned_gbps", "DID_NOT_RUN_no_pin")

    emit("ok", 1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
